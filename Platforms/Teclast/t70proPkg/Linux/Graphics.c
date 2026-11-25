// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native DRM/KMS scanout through the stock kernel; no GPU or raw MMIO access.
// UAPI: Linux include/uapi/drm/{drm.h,drm_mode.h,drm_fourcc.h}.
#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define KMS_OBJECTS 64U
#define KMS_MODES 128U
#define KMS_MAX_BYTES (128U*1024U*1024U)
typedef struct {
  struct drm_mode_get_connector Info;
  struct drm_mode_modeinfo Modes[KMS_MODES];
  uint32_t Encoders[KMS_OBJECTS];
} CONNECTOR;
typedef struct {
  int Fd, Master, ModeAttempted;
  uint32_t Connector, Handle, Framebuffer, Pitch;
  uint64_t Bytes;
  void *Map;
  struct drm_mode_crtc Original;
  struct drm_mode_modeinfo Mode;
  uint32_t OriginalConnector;
} SCREEN;
static int KmsIoctl(int Fd,unsigned long Request,void *Argument) {
  int R;
  do { R=ioctl(Fd,Request,Argument); } while(R<0 && errno==EINTR);
  return R;
}
static int ReadConnector(int Fd,uint32_t Id,CONNECTOR *C) {
  memset(C,0,sizeof(*C)); C->Info.connector_id=Id;
  // A zero mode count requests a fresh kernel probe. No guessed panel timings.
  if(KmsIoctl(Fd,DRM_IOCTL_MODE_GETCONNECTOR,&C->Info)) return -1;
  if(C->Info.count_modes>KMS_MODES || C->Info.count_encoders>KMS_OBJECTS) {
    errno=E2BIG; return -1;
  }
  C->Info.modes_ptr=(uintptr_t)C->Modes;
  C->Info.encoders_ptr=(uintptr_t)C->Encoders;
  C->Info.props_ptr=C->Info.prop_values_ptr=0; C->Info.count_props=0;
  C->Info.count_modes=KMS_MODES; C->Info.count_encoders=KMS_OBJECTS;
  if(KmsIoctl(Fd,DRM_IOCTL_MODE_GETCONNECTOR,&C->Info)) return -1;
  if(C->Info.connector_id!=Id || C->Info.count_modes>KMS_MODES ||
     C->Info.count_encoders>KMS_OBJECTS) { errno=E2BIG; return -1; }
  return 0;
}
static int InternalPanel(uint32_t Type) {
  return Type==DRM_MODE_CONNECTOR_DSI || Type==DRM_MODE_CONNECTOR_eDP ||
         Type==DRM_MODE_CONNECTOR_LVDS || Type==DRM_MODE_CONNECTOR_DPI;
}
static int ValidMode(const struct drm_mode_modeinfo *M) {
  return M->clock && M->hdisplay && M->vdisplay &&
         M->hdisplay<=8192 && M->vdisplay<=8192 &&
         M->hdisplay<=M->hsync_start && M->hsync_start<=M->hsync_end &&
         M->hsync_end<=M->htotal && M->vdisplay<=M->vsync_start &&
         M->vsync_start<=M->vsync_end && M->vsync_end<=M->vtotal;
}
static int SameMode(const struct drm_mode_modeinfo *A,const struct drm_mode_modeinfo *B) {
  return A->clock==B->clock && A->hdisplay==B->hdisplay &&
    A->hsync_start==B->hsync_start && A->hsync_end==B->hsync_end &&
    A->htotal==B->htotal && A->hskew==B->hskew &&
    A->vdisplay==B->vdisplay && A->vsync_start==B->vsync_start &&
    A->vsync_end==B->vsync_end && A->vtotal==B->vtotal &&
    A->vscan==B->vscan && A->flags==B->flags;
}
static int SelectScreen(SCREEN *S) {
  struct drm_mode_card_res R={0};
  uint32_t Crtcs[KMS_OBJECTS],Connectors[KMS_OBJECTS],Encoders[KMS_OBJECTS];
  CONNECTOR Panel; uint32_t Selected=0;
  R.crtc_id_ptr=(uintptr_t)Crtcs; R.connector_id_ptr=(uintptr_t)Connectors;
  R.encoder_id_ptr=(uintptr_t)Encoders;
  R.count_crtcs=R.count_connectors=R.count_encoders=KMS_OBJECTS;
  if(KmsIoctl(S->Fd,DRM_IOCTL_MODE_GETRESOURCES,&R)) return -1;
  if(!R.count_crtcs || R.count_crtcs>32 || !R.count_connectors ||
     R.count_connectors>KMS_OBJECTS || R.count_encoders>KMS_OBJECTS) { errno=E2BIG;return -1; }
  for(uint32_t I=0;I<R.count_connectors;I++) {
    CONNECTOR C;
    if(ReadConnector(S->Fd,Connectors[I],&C)) return -1;
    // UAPI enum drm_connector_status: connected == 1.
    if(C.Info.connection!=1 || !InternalPanel(C.Info.connector_type) || !C.Info.count_modes) continue;
    if(Selected) { errno=ENOTUNIQ;return -1; }
    Selected=C.Info.connector_id; Panel=C;
  }
  if(!Selected) { errno=ENODEV;return -1; }
  // Prefer an existing route. An inactive panel may use only a free CRTC.
  for(uint32_t Pass=0;Pass<2;Pass++) for(uint32_t I=0;I<Panel.Info.count_encoders;I++) {
    struct drm_mode_get_encoder E={0};
    E.encoder_id=Panel.Encoders[I];
    if(KmsIoctl(S->Fd,DRM_IOCTL_MODE_GETENCODER,&E)) return -1;
    if(Pass==0 && (E.encoder_id!=Panel.Info.encoder_id || !E.crtc_id)) continue;
    if(Pass==1 && E.encoder_id==Panel.Info.encoder_id && E.crtc_id) continue;
    for(uint32_t J=0;J<R.count_crtcs;J++) {
      struct drm_mode_crtc Old={0}; uint32_t Attached=0; int Shared=0;
      if(!(E.possible_crtcs&(1U<<J)) || (Pass==0 && E.crtc_id!=Crtcs[J])) continue;
      Old.crtc_id=Crtcs[J];
      if(KmsIoctl(S->Fd,DRM_IOCTL_MODE_GETCRTC,&Old)) return -1;
      for(uint32_t K=0;K<R.count_connectors;K++) {
        CONNECTOR C; struct drm_mode_get_encoder Other={0};
        if(ReadConnector(S->Fd,Connectors[K],&C)) return -1;
        if(!C.Info.encoder_id) continue;
        Other.encoder_id=C.Info.encoder_id;
        if(KmsIoctl(S->Fd,DRM_IOCTL_MODE_GETENCODER,&Other)) return -1;
        if(Other.crtc_id!=Old.crtc_id) continue;
        if(C.Info.connector_id!=Selected) Shared=1;
        else Attached=Selected;
      }
      if(Shared || (Old.mode_valid && (!Attached || !Old.fb_id)) ||
         (!Old.mode_valid && Old.fb_id)) continue;
      const struct drm_mode_modeinfo *Mode=NULL;
      if(Old.mode_valid) {
        for(uint32_t K=0;K<Panel.Info.count_modes;K++)
          if(SameMode(&Old.mode,&Panel.Modes[K])) { Mode=&Panel.Modes[K];break; }
      } else {
        for(uint32_t K=0;K<Panel.Info.count_modes;K++)
          if(ValidMode(&Panel.Modes[K]) &&
             (!Mode || (Panel.Modes[K].type&DRM_MODE_TYPE_PREFERRED))) {
            Mode=&Panel.Modes[K];
            if(Mode->type&DRM_MODE_TYPE_PREFERRED) break;
          }
      }
      if(!Mode || !ValidMode(Mode) || Mode->hdisplay<R.min_width ||
         Mode->hdisplay>R.max_width || Mode->vdisplay<R.min_height ||
         Mode->vdisplay>R.max_height) continue;
      S->Connector=Selected; S->Original=Old; S->Mode=*Mode;
      S->OriginalConnector=Old.mode_valid?Attached:0;
      return 0;
    }
  }
  errno=ENODEV;return -1;
}
static void DrawPattern(SCREEN *S) {
  static const uint32_t Bars[]={0xffffff,0xffff00,0x00ffff,0x00ff00,
                                0xff00ff,0xff0000,0x0000ff,0x000000};
  memset(S->Map,0,(size_t)S->Bytes);
  for(uint32_t Y=0;Y<S->Mode.vdisplay;Y++) {
    uint32_t *Row=(uint32_t *)((uint8_t *)S->Map+(size_t)Y*S->Pitch);
    for(uint32_t X=0;X<S->Mode.hdisplay;X++) {
      uint32_t Color=Bars[(uint64_t)X*8U/S->Mode.hdisplay];
      if(Y>=S->Mode.vdisplay*3U/4U)
        Color=((X/32U+Y/32U)&1U)?0x303030:0xc0c0c0;
      Row[X]=Color;
    }
  }
  __sync_synchronize();
}
static int StartScreen(SCREEN *S) {
  struct drm_get_cap Cap={.capability=DRM_CAP_DUMB_BUFFER};
  struct drm_mode_create_dumb B={0}; struct drm_mode_map_dumb Map={0};
  struct drm_mode_fb_cmd2 Fb={0}; struct drm_mode_crtc Next={0};
  if(KmsIoctl(S->Fd,DRM_IOCTL_SET_MASTER,NULL)) return -1;
  S->Master=1;
  if(KmsIoctl(S->Fd,DRM_IOCTL_GET_CAP,&Cap) || !Cap.value) { errno=EOPNOTSUPP;return -1; }
  if(SelectScreen(S)) return -1;
  B.width=S->Mode.hdisplay; B.height=S->Mode.vdisplay; B.bpp=32;
  if(KmsIoctl(S->Fd,DRM_IOCTL_MODE_CREATE_DUMB,&B)) return -1;
  S->Handle=B.handle; S->Bytes=B.size; S->Pitch=B.pitch;
  if(B.width!=S->Mode.hdisplay || B.height!=S->Mode.vdisplay || B.bpp!=32 ||
     !B.handle || !B.size || B.size>KMS_MAX_BYTES || B.pitch%4 ||
     B.pitch<(uint64_t)S->Mode.hdisplay*4U ||
     (uint64_t)B.pitch*S->Mode.vdisplay>B.size) { errno=EOVERFLOW;return -1; }
  Map.handle=B.handle;
  if(KmsIoctl(S->Fd,DRM_IOCTL_MODE_MAP_DUMB,&Map)) return -1;
  long Page=sysconf(_SC_PAGESIZE);
  if(Page<=0 || Map.offset>(uint64_t)LLONG_MAX || Map.offset%(uint64_t)Page) {
    errno=EOVERFLOW;return -1;
  }
  S->Map=mmap(NULL,(size_t)B.size,PROT_READ|PROT_WRITE,MAP_SHARED,S->Fd,(off_t)Map.offset);
  if(S->Map==MAP_FAILED) { S->Map=NULL;return -1; }
  DrawPattern(S);
  Fb.width=B.width; Fb.height=B.height; Fb.pixel_format=DRM_FORMAT_XRGB8888;
  Fb.handles[0]=B.handle; Fb.pitches[0]=B.pitch;
  if(KmsIoctl(S->Fd,DRM_IOCTL_MODE_ADDFB2,&Fb)) return -1;
  S->Framebuffer=Fb.fb_id;
  if(!Fb.fb_id) { errno=EPROTO;return -1; }
  Next.crtc_id=S->Original.crtc_id; Next.fb_id=Fb.fb_id;
  Next.set_connectors_ptr=(uintptr_t)&S->Connector; Next.count_connectors=1;
  Next.mode_valid=1; Next.mode=S->Mode;
  // Even an error may leave a driver partly changed. Restore before freeing.
  S->ModeAttempted=1;
  return KmsIoctl(S->Fd,DRM_IOCTL_MODE_SETCRTC,&Next);
}
static int StopScreen(SCREEN *S) {
  if(S->ModeAttempted) {
    struct drm_mode_crtc Restore=S->Original;
    Restore.set_connectors_ptr=Restore.mode_valid?(uintptr_t)&S->OriginalConnector:0;
    Restore.count_connectors=Restore.mode_valid?1:0;
    if(KmsIoctl(S->Fd,DRM_IOCTL_MODE_SETCRTC,&Restore)) return -1;
    S->ModeAttempted=0;
  }
  if(S->Framebuffer) {
    if(KmsIoctl(S->Fd,DRM_IOCTL_MODE_RMFB,&S->Framebuffer)) return -1;
    S->Framebuffer=0;
  }
  if(S->Map) {
    if(munmap(S->Map,(size_t)S->Bytes)) return -1;
    S->Map=NULL;
  }
  if(S->Handle) {
    struct drm_mode_destroy_dumb D={.handle=S->Handle};
    if(KmsIoctl(S->Fd,DRM_IOCTL_MODE_DESTROY_DUMB,&D)) return -1;
    S->Handle=0;
  }
  if(S->Master) {
    if(KmsIoctl(S->Fd,DRM_IOCTL_DROP_MASTER,NULL)) return -1;
    S->Master=0;
  }
  return 0;
}
static volatile sig_atomic_t GraphicsStop;
static void StopPattern(int Signal) { (void)Signal;GraphicsStop=1; }
static int CardPath(const char *P) {
  const char Prefix[]="/dev/dri/card";
  if(strncmp(P,Prefix,sizeof(Prefix)-1)) return 0;
  P+=sizeof(Prefix)-1;
  if(*P<'0'||*P>'9'||(*P=='0'&&P[1])) return 0;
  unsigned Number=0;
  while(*P>='0'&&*P<='9') { Number=Number*10U+(unsigned)(*P++-'0');if(Number>=64)return 0; }
  return !*P;
}
int main(int Argc,char **Argv) {
  SCREEN S={.Fd=-1}; struct stat St; int Pattern,Result=0;
  if(Argc!=3 || ((Pattern=!strcmp(Argv[1],"--pattern"))==0&&strcmp(Argv[1],"--probe")) ||
     !CardPath(Argv[2])) {
    fputs("Usage: teclast-graphics --probe|--pattern /dev/dri/cardN\n",stderr);return 64;
  }
  S.Fd=open(Argv[2],O_RDWR|O_CLOEXEC|O_NOFOLLOW|O_NOCTTY);
  if(S.Fd<0 || fstat(S.Fd,&St) || !S_ISCHR(St.st_mode) ||
     major(St.st_rdev)!=226 || minor(St.st_rdev)>=64) { perror("DRM primary device");if(S.Fd>=0)close(S.Fd);return 1; }
  struct sigaction Action={0}; Action.sa_handler=StopPattern;sigemptyset(&Action.sa_mask);
  sigaction(SIGTERM,&Action,NULL);sigaction(SIGINT,&Action,NULL);
  if((Pattern?StartScreen(&S):SelectScreen(&S))<0) { perror("DRM display setup");Result=1; }
  else {
    printf("DRM panel connector %u, CRTC %u, %ux%u, %u kHz%s\n",S.Connector,
      S.Original.crtc_id,S.Mode.hdisplay,S.Mode.vdisplay,S.Mode.clock,Pattern?"; XRGB8888 test pattern for 10 seconds":"");
    if(Pattern) for(unsigned I=0;I<100&&!GraphicsStop;I++) {
      struct timespec Delay={.tv_nsec=100000000};while(nanosleep(&Delay,&Delay)&&errno==EINTR&&!GraphicsStop){}
    }
  }
  while(StopScreen(&S)<0) {
    // Do not release a buffer possibly still scanned out. A later signal retries
    // restoration; keeping this FD open preserves ownership after an error.
    perror("DRM restore failed; retaining scanout resources");
    sigset_t Block,Previous;sigemptyset(&Block);sigaddset(&Block,SIGTERM);sigaddset(&Block,SIGINT);
    sigprocmask(SIG_BLOCK,&Block,&Previous);
    GraphicsStop=0;while(!GraphicsStop) sigsuspend(&Previous);
    sigprocmask(SIG_SETMASK,&Previous,NULL);Result=1;
  }
  close(S.Fd);return Result;
}
