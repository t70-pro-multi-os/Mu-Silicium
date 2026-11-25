// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Mesa function/property bodies are inserted by test-kbase-abi.py.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif

typedef int VkResult;
#define VK_SUCCESS 0
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define DRM_NODE_PRIMARY 0
#define DRM_NODE_RENDER 2
#define PANVK_KBASE_PATH "/dev/mali0"
struct panvk_instance { int unused; };
struct panvk_physical_device { struct { dev_t render_rdev, primary_rdev; } drm; };
struct drm_device { char **nodes; int available_nodes; };
typedef struct drm_device *drmDevicePtr;
struct properties {
   int drmHasPrimary, drmHasRender;
   uint32_t drmPrimaryMajor, drmPrimaryMinor, drmRenderMajor, drmRenderMinor;
};
static char Primary[]="/dev/dri/card0", Render[]="/dev/dri/renderD128";
static int Calls, Failure, Errors;
static unsigned Checks;
static int MockStat(const char *name, struct stat *st) {
   ++Calls;
   if ((name==Render && Failure==1) || (name==Primary && Failure==2)) return -1;
   st->st_rdev=name==Render?makedev(226U,128U):name==Primary?makedev(226U,0U):makedev(10U,54U);
   return 0;
}
#define stat(name, value) MockStat(name, value)
#define vk_errorf(instance, result, ...) ((void)(instance), ++Errors, (result))
/* ACTUAL_DRM_IDS */
#undef stat
static void Properties(const struct panvk_physical_device *device, struct properties *properties) {
/* ACTUAL_DRM_PROPERTIES */
}
#define CHECK(value) do { ++Checks; if (!(value)) return __LINE__; } while (0)
static int TestMain(void) {
   struct panvk_instance instance={0};
   struct panvk_physical_device device={.drm={123,456}};
   struct properties props={0};
   char *nodes[3]={Primary,0,Render};
   struct drm_device drm={nodes,1<<DRM_NODE_RENDER};
   CHECK(get_drm_device_ids(&device,&instance,0)==VK_SUCCESS);
   CHECK(Calls==0 && Errors==0);
   Properties(&device,&props);
   CHECK(!props.drmHasRender && !props.drmHasPrimary);
   CHECK(!props.drmRenderMajor && !props.drmRenderMinor && !props.drmPrimaryMajor && !props.drmPrimaryMinor);
   CHECK(get_drm_device_ids(&device,&instance,&drm)==VK_SUCCESS);
   CHECK(Calls==1 && Errors==0);
   Properties(&device,&props);
   CHECK(props.drmHasRender && !props.drmHasPrimary);
   CHECK(props.drmRenderMajor==226 && props.drmRenderMinor==128);
   drm.available_nodes|=1<<DRM_NODE_PRIMARY; Calls=0;
   CHECK(get_drm_device_ids(&device,&instance,&drm)==VK_SUCCESS);
   CHECK(Calls==2 && Errors==0);
   Properties(&device,&props);
   CHECK(props.drmHasPrimary && props.drmHasRender);
   CHECK(props.drmPrimaryMajor==226 && props.drmPrimaryMinor==0);
   Failure=1; Calls=0;
   CHECK(get_drm_device_ids(&device,&instance,&drm)==VK_ERROR_INITIALIZATION_FAILED);
   CHECK(Calls==1 && Errors==1);
   Failure=2; Calls=0;
   CHECK(get_drm_device_ids(&device,&instance,&drm)==VK_ERROR_INITIALIZATION_FAILED);
   CHECK(Calls==2 && Errors==2);
   Calls=0; Failure=1;
   CHECK(get_drm_device_ids(&device,&instance,0)==VK_SUCCESS);
   CHECK(!device.drm.render_rdev && !device.drm.primary_rdev && Calls==0 && Errors==2);
   return 0;
}
int main(void) {
   int result=TestMain();
   if (result) fprintf(stderr,"DRM identity failed at C line %d\n",result);
   else printf("DRM identity: %u checks PASS\n",Checks);
   return result?1:0;
}
