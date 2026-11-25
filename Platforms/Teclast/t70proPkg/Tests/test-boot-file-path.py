#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host-only production file-device-path expansion regression; no media I/O."""
from pathlib import Path
import subprocess, tempfile, sys
root=Path(__file__).resolve().parents[4]
p='MdeModulePkg/Library/UefiBootManagerLib/BmBoot.c'
s=(root/'Mu_Basecore'/p).read_text()
if '--baseline' in sys.argv[1:]:
    s=subprocess.check_output(['git','-C',str(root/'Mu_Basecore'),'show','8969bbe:'+p],text=True)
a=s.index('\nBmExpandFileDevicePath (')+1;b=s.index('{',a);end=b+1;depth=1
while depth:
    depth+=(s[end]=='{')-(s[end]=='}');end+=1
shim=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef unsigned long UINTN;
typedef int EFI_STATUS;
typedef int BOOLEAN;
typedef void VOID;
typedef void *EFI_HANDLE;
typedef unsigned char EFI_DEVICE_PATH_PROTOCOL;
typedef struct { int RemovableMedia; } MEDIA;
typedef struct { MEDIA *Media; } EFI_BLOCK_IO_PROTOCOL;
#define IN
#define TRUE 1
#define FALSE 0
#define EFI_ERROR(s) ((s)!=0)
#define ByProtocol 0
static int gEfiSimpleFileSystemProtocolGuid,gEfiBlockIoProtocolGuid;
static MEDIA media={0};static EFI_BLOCK_IO_PROTOCOL block={&media};
static unsigned char paths[3][32];
static EFI_STATUS Locate(int t,void *g,void *k,UINTN *n,EFI_HANDLE **out) {
 (void)t;(void)g;(void)k;*n=3;*out=malloc(3*sizeof(void*));assert(*out);
 for(int i=0;i<3;i++)(*out)[i]=paths[i];return 0;
}
static EFI_STATUS Handle(EFI_HANDLE h,void *g,void *out){(void)h;(void)g;*(void**)out=&block;return 0;}
static struct { EFI_STATUS (*LocateHandleBuffer)(int,void*,void*,UINTN*,EFI_HANDLE**);
 EFI_STATUS (*HandleProtocol)(EFI_HANDLE,void*,void*); } bs={Locate,Handle},*gBS=&bs;
static void EfiBootManagerConnectAll(void){}
static EFI_DEVICE_PATH_PROTOCOL *DevicePathFromHandle(EFI_HANDLE h){return h;}
static UINTN GetDevicePathSize(EFI_DEVICE_PATH_PROTOCOL *p){return p[0];}
static EFI_DEVICE_PATH_PROTOCOL *AppendDevicePath(EFI_DEVICE_PATH_PROTOCOL *p,EFI_DEVICE_PATH_PROTOCOL *file){
 (void)file;void *out=malloc(p[0]);assert(out);return memcpy(out,p,p[0]);
}
static int CompareMem(const void *a,const void *b,UINTN n){return memcmp(a,b,n);}
static void FreePool(void *p){free(p);}
'''
tests=r'''
int main(void){
 paths[0][0]=32;paths[0][1]=10;paths[1][0]=8;paths[1][1]=11;
 paths[2][0]=16;paths[2][1]=12;
 unsigned char file=0;
 unsigned char *first=BmExpandFileDevicePath(&file,NULL);assert(first&&first[1]==10);
 unsigned char *second=BmExpandFileDevicePath(&file,first);assert(second&&second[1]==11);
 unsigned char *third=BmExpandFileDevicePath(&file,second);assert(third&&third[1]==12);
 assert(BmExpandFileDevicePath(&file,third)==NULL);
 free(first);free(second);free(third);
 // A previous candidate which no longer exists must not select another path.
 unsigned char absent[4]={4,99,0,0};
 assert(BmExpandFileDevicePath(&file,absent)==NULL);
 puts("EFI file path expansion: unequal lengths, continuation, end and absent candidate passed");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='t70-file-path-') as d:
    d=Path(d);(d/'test.c').write_text(shim+'EFI_DEVICE_PATH_PROTOCOL *\n'+s[a:end]+tests)
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-fno-sanitize-recover=all',str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
