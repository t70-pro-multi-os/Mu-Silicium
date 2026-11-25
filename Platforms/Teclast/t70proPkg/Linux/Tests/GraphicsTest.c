// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Graphics.c with fake kernel DRM ioctls and guarded CPU mappings.
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
static int FakeIoctl(int,unsigned long,...);
static void *FakeMmap(void *,size_t,int,int,int,off_t);
static int FakeMunmap(void *,size_t);
#define ioctl FakeIoctl
#define mmap FakeMmap
#define munmap FakeMunmap
#define main GraphicsProgramMain
#include "../Graphics.c"
#undef main
#undef ioctl
#undef mmap
#undef munmap
static unsigned Checks;
#define CHECK(X) do { ++Checks;if(!(X)){fprintf(stderr,"%u: %s\n",__LINE__,#X);abort();} } while(0)
static int Bad,Inactive,Shared,Ambiguous,Maps,Unmaps,Destroys,Removes,Masters,Drops,Sets;
static int FailSet,FailUnmap,MapFailure,FailOnce;
static unsigned long FailRequest;
static unsigned char *Allocation;
static size_t AllocationBytes;
static struct drm_mode_crtc LastSet;
static struct drm_mode_modeinfo Mode(void) {
  struct drm_mode_modeinfo M={0};
  M.clock=25000;M.hdisplay=8;M.hsync_start=10;M.hsync_end=12;M.htotal=16;
  M.vdisplay=6;M.vsync_start=7;M.vsync_end=8;M.vtotal=10;M.type=DRM_MODE_TYPE_PREFERRED;
  return M;
}
static int FakeIoctl(int Fd,unsigned long Q,...) {
  va_list V;void *P;va_start(V,Q);P=va_arg(V,void *);va_end(V);
  CHECK(Fd==42);
  if(Q==FailRequest && FailOnce) { FailOnce=0;errno=EIO;return -1; }
  if(Q==DRM_IOCTL_SET_MASTER) { ++Masters;return 0; }
  if(Q==DRM_IOCTL_DROP_MASTER) { ++Drops;return 0; }
  if(Q==DRM_IOCTL_GET_CAP) {
    struct drm_get_cap *C=P;CHECK(C->capability==DRM_CAP_DUMB_BUFFER);C->value=Bad!=1;return 0;
  }
  if(Q==DRM_IOCTL_MODE_GETRESOURCES) {
    struct drm_mode_card_res *R=P;
    ((uint32_t *)(uintptr_t)R->crtc_id_ptr)[0]=301;
    uint32_t *Cs=(void *)(uintptr_t)R->connector_id_ptr;Cs[0]=101;Cs[1]=102;
    ((uint32_t *)(uintptr_t)R->encoder_id_ptr)[0]=201;
    R->count_crtcs=Bad==2?33:1;R->count_connectors=(Shared||Ambiguous)?2:1;
    R->count_encoders=1;R->max_width=R->max_height=8192;return 0;
  }
  if(Q==DRM_IOCTL_MODE_GETCONNECTOR) {
    struct drm_mode_get_connector *C=P;CHECK(C->connector_id==101||C->connector_id==102);
    C->connection=Bad==3?2:1;
    C->connector_type=C->connector_id==102&&!Ambiguous?DRM_MODE_CONNECTOR_HDMIA:DRM_MODE_CONNECTOR_DSI;
    C->encoder_id=Inactive?0:201;
    if(C->modes_ptr) {
      struct drm_mode_modeinfo *M=(void *)(uintptr_t)C->modes_ptr;*M=Mode();
      if(Bad==4) M->hdisplay=0;
      if(Bad==5) M->hsync_end=M->htotal+1;
    }
    if(C->encoders_ptr) ((uint32_t *)(uintptr_t)C->encoders_ptr)[0]=201;
    C->count_modes=Bad==6?KMS_MODES+1:1;C->count_encoders=1;return 0;
  }
  if(Q==DRM_IOCTL_MODE_GETENCODER) {
    struct drm_mode_get_encoder *E=P;CHECK(E->encoder_id==201);
    E->crtc_id=Inactive?0:301;E->possible_crtcs=Bad==7?0:1;return 0;
  }
  if(Q==DRM_IOCTL_MODE_GETCRTC) {
    struct drm_mode_crtc *C=P;CHECK(C->crtc_id==301);
    C->mode_valid=!Inactive;C->fb_id=Inactive?0:17;C->mode=Mode();C->x=3;C->y=4;
    return 0;
  }
  if(Q==DRM_IOCTL_MODE_CREATE_DUMB) {
    struct drm_mode_create_dumb *D=P;CHECK(D->bpp==32&&D->width==8&&D->height==6);
    D->handle=55;D->pitch=Bad==8?31:64;D->size=Bad==9?100:384;
    if(Bad==10) D->size=(uint64_t)KMS_MAX_BYTES+1;
    return 0;
  }
  if(Q==DRM_IOCTL_MODE_MAP_DUMB) {
    struct drm_mode_map_dumb *M=P;CHECK(M->handle==55);
    M->offset=Bad==11?1:(Bad==12?UINT64_MAX:4096);return 0;
  }
  if(Q==DRM_IOCTL_MODE_ADDFB2) {
    struct drm_mode_fb_cmd2 *F=P;
    CHECK(F->width==8&&F->height==6&&F->pixel_format==DRM_FORMAT_XRGB8888);
    CHECK(F->handles[0]==55&&F->pitches[0]==64&&F->flags==0&&F->offsets[0]==0);
    F->fb_id=Bad==13?0:99;return 0;
  }
  if(Q==DRM_IOCTL_MODE_SETCRTC) {
    struct drm_mode_crtc *C=P;++Sets;LastSet=*C;
    CHECK(C->crtc_id==301);
    if(Sets==1) {
      CHECK(C->mode_valid&&C->fb_id==99&&C->count_connectors==1);
      CHECK(*(uint32_t *)(uintptr_t)C->set_connectors_ptr==101);
    } else if(Inactive) { CHECK(!C->mode_valid&&!C->fb_id&&!C->count_connectors); }
    else {
      CHECK(C->mode_valid&&C->fb_id==17&&C->count_connectors==1&&C->x==3&&C->y==4);
      CHECK(*(uint32_t *)(uintptr_t)C->set_connectors_ptr==101);
    }
    if(FailSet==Sets) { errno=EIO;return -1; }
    return 0;
  }
  if(Q==DRM_IOCTL_MODE_RMFB) { CHECK(*(uint32_t *)P==99);++Removes;return 0; }
  if(Q==DRM_IOCTL_MODE_DESTROY_DUMB) {
    CHECK(((struct drm_mode_destroy_dumb *)P)->handle==55);++Destroys;return 0;
  }
  CHECK(0);return -1;
}
static void *FakeMmap(void *A,size_t N,int Prot,int Flags,int Fd,off_t Offset) {
  CHECK(!A&&N==384&&Prot==(PROT_READ|PROT_WRITE)&&Flags==MAP_SHARED&&Fd==42&&Offset==4096);
  if(MapFailure) { errno=ENOMEM;return MAP_FAILED; }
  CHECK(!Allocation);AllocationBytes=N;
  Allocation=malloc(N+64);CHECK(Allocation);memset(Allocation,0xa5,N+64);++Maps;return Allocation+32;
}
static int FakeMunmap(void *P,size_t N) {
  CHECK(Allocation&&P==Allocation+32&&N==AllocationBytes);
  for(unsigned I=0;I<32;I++) CHECK(Allocation[I]==0xa5&&Allocation[N+32+I]==0xa5);
  if(FailUnmap) { FailUnmap=0;errno=EIO;return -1; }
  ++Unmaps;free(Allocation);Allocation=NULL;return 0;
}
static void Reset(void) {
  CHECK(!Allocation);
  Bad=Inactive=Shared=Ambiguous=Maps=Unmaps=Destroys=Removes=Masters=Drops=Sets=0;
  FailSet=FailUnmap=MapFailure=FailOnce=0;FailRequest=0;
}
int main(void) {
  SCREEN S;
  for(unsigned Idle=0;Idle<2;Idle++) {
    Reset();Inactive=Idle;memset(&S,0,sizeof(S));S.Fd=42;
    CHECK(StartScreen(&S)==0&&S.ModeAttempted&&S.Pitch==64&&S.Framebuffer==99);
    CHECK(S.OriginalConnector==(Idle?0:101));
    uint32_t *Row=S.Map;CHECK(Row[0]==0xffffff&&Row[7]==0);
    for(unsigned Y=0;Y<6;Y++) for(unsigned X=32;X<64;X++)
      CHECK(((uint8_t *)S.Map)[Y*64+X]==0);
    CHECK(StopScreen(&S)==0&&Sets==2&&Maps==Unmaps&&Removes==1&&Destroys==1&&Drops==1);
    CHECK(!S.Handle&&!S.Framebuffer&&!S.Map&&!S.ModeAttempted&&!S.Master);
    CHECK(StopScreen(&S)==0&&Destroys==1);
  }
  for(Bad=1;Bad<=13;Bad++) {
    int Saved=Bad;Reset();Bad=Saved;memset(&S,0,sizeof(S));S.Fd=42;
    CHECK(StartScreen(&S)<0&&Sets==0);CHECK(StopScreen(&S)==0);
    CHECK(Maps==Unmaps&&Drops==1);
  }
  const unsigned long Failures[]={DRM_IOCTL_SET_MASTER,DRM_IOCTL_GET_CAP,
    DRM_IOCTL_MODE_GETRESOURCES,DRM_IOCTL_MODE_GETCONNECTOR,DRM_IOCTL_MODE_GETENCODER,
    DRM_IOCTL_MODE_GETCRTC,DRM_IOCTL_MODE_CREATE_DUMB,DRM_IOCTL_MODE_MAP_DUMB,DRM_IOCTL_MODE_ADDFB2};
  for(unsigned I=0;I<sizeof(Failures)/sizeof(Failures[0]);I++) {
    Reset();FailRequest=Failures[I];FailOnce=1;memset(&S,0,sizeof(S));S.Fd=42;
    CHECK(StartScreen(&S)<0&&Sets==0);CHECK(StopScreen(&S)==0&&Maps==Unmaps);
  }
  for(unsigned Clone=0;Clone<2;Clone++) {
    Reset();if(Clone)Shared=1;else Ambiguous=1;
    memset(&S,0,sizeof(S));S.Fd=42;CHECK(StartScreen(&S)<0&&Sets==0&&Maps==0);
    CHECK(StopScreen(&S)==0);
  }
  Reset();MapFailure=1;memset(&S,0,sizeof(S));S.Fd=42;
  CHECK(StartScreen(&S)<0);CHECK(StopScreen(&S)==0&&Maps==0&&Destroys==1);
  Reset();FailSet=1;memset(&S,0,sizeof(S));S.Fd=42;
  CHECK(StartScreen(&S)<0&&S.ModeAttempted);CHECK(StopScreen(&S)==0&&Sets==2&&Unmaps==1);
  Reset();FailSet=2;memset(&S,0,sizeof(S));S.Fd=42;
  CHECK(StartScreen(&S)==0);CHECK(StopScreen(&S)<0);
  CHECK(S.ModeAttempted&&S.Map&&S.Handle&&S.Framebuffer&&Unmaps==0&&Removes==0&&Destroys==0&&Drops==0);
  FailSet=0;CHECK(StopScreen(&S)==0&&Unmaps==1);
  Reset();FailUnmap=1;memset(&S,0,sizeof(S));S.Fd=42;
  CHECK(StartScreen(&S)==0);CHECK(StopScreen(&S)<0&&S.Map&&!S.Framebuffer&&S.Handle);
  CHECK(StopScreen(&S)==0&&Sets==2&&Unmaps==1);
  CHECK(CardPath("/dev/dri/card0")&&CardPath("/dev/dri/card63"));
  const char *Paths[]={"/dev/dri/card64","/dev/dri/card01","/dev/dri/card-1",
    "/dev/dri/renderD128","/dev/dri/card0/../card1","/dev/dri/card","/tmp/card0","/dev/dri/card99999999999999"};
  for(unsigned I=0;I<sizeof(Paths)/sizeof(Paths[0]);I++)CHECK(!CardPath(Paths[I]));
  CHECK(!Allocation);printf("DRM/KMS graphics: %u checks PASS; fake kernel ioctls/mappings, no device access\n",Checks);
  return 0;
}
