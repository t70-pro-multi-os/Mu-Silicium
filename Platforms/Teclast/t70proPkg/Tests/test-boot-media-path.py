#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host-only production media-device-path expansion regression; no media I/O."""
from pathlib import Path
import subprocess, tempfile, sys
root=Path(__file__).resolve().parents[4]
p='MdeModulePkg/Library/UefiBootManagerLib/BmBoot.c'
s=(root/'Mu_Basecore'/p).read_text()
if '--baseline' in sys.argv[1:]:
    s=subprocess.check_output(['git','-C',str(root/'Mu_Basecore'),'show','66d8260:'+p],text=True)
a=s.index('\nBmExpandMediaDevicePath (')+1;b=s.index('{',a);end=b+1;depth=1
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
typedef struct { UINTN BlockSize; unsigned MediaId; } MEDIA;
typedef struct BLOCK { MEDIA *Media; int (*ReadBlocks)(struct BLOCK*,unsigned,UINTN,UINTN,void*); } EFI_BLOCK_IO_PROTOCOL;
#define IN
#define TRUE 1
#define FALSE 0
#define EFI_ERROR(s) ((s)!=0)
#define ASSERT(x) assert(x)
#define ASSERT_EFI_ERROR(s) assert((s)==0)
#define ByProtocol 0
#define END_DEVICE_PATH_LENGTH 4
#define EFI_REMOVABLE_MEDIA_FILE_NAME NULL
static int gEfiSimpleFileSystemProtocolGuid,gEfiBlockIoProtocolGuid;
static unsigned char parent[8]={8,1},paths[2][8]={{8,1},{8,1}};
static int reads,selected,missing;
static int Read(struct BLOCK *b,unsigned id,UINTN lba,UINTN size,void *out){
 (void)b;(void)id;(void)lba;(void)size;(void)out;reads++;return 0;
}
static MEDIA media={512,1};static EFI_BLOCK_IO_PROTOCOL block={&media,Read};
static EFI_STATUS Locate(void *g,EFI_DEVICE_PATH_PROTOCOL **p,EFI_HANDLE *h){
 (void)p;*h=parent;return g==&gEfiBlockIoProtocolGuid?0:1;
}
static EFI_STATUS Handles(int t,void *g,void *k,UINTN *n,EFI_HANDLE **out){
 (void)t;(void)g;(void)k;*n=2;*out=malloc(2*sizeof(void*));assert(*out);
 (*out)[0]=paths[0];(*out)[1]=paths[1];return 0;
}
static EFI_STATUS Handle(EFI_HANDLE h,void *g,void **out){(void)h;(void)g;*out=&block;return 0;}
static EFI_STATUS Connect(EFI_HANDLE h,void *d,void *p,int r){(void)h;(void)d;(void)p;(void)r;return 0;}
static struct { EFI_STATUS (*LocateDevicePath)(void*,EFI_DEVICE_PATH_PROTOCOL**,EFI_HANDLE*);
 EFI_STATUS (*ConnectController)(EFI_HANDLE,void*,void*,int);
 EFI_STATUS (*HandleProtocol)(EFI_HANDLE,void*,void**);
 EFI_STATUS (*LocateHandleBuffer)(int,void*,void*,UINTN*,EFI_HANDLE**);
} bs={Locate,Connect,Handle,Handles},*gBS=&bs;
static int IsDevicePathEnd(EFI_DEVICE_PATH_PROTOCOL *p){(void)p;return 1;}
static void *AllocatePool(UINTN n){return malloc(n);}
static void FreePool(void *p){free(p);}
static EFI_DEVICE_PATH_PROTOCOL *DevicePathFromHandle(EFI_HANDLE h){
 if(h==paths[0] || missing)return NULL;return h;
}
static UINTN GetDevicePathSize(EFI_DEVICE_PATH_PROTOCOL *p){return p[0];}
static int CompareMem(const void *a,const void *b,UINTN n){return memcmp(a,b,n);}
static EFI_DEVICE_PATH_PROTOCOL *FileDevicePath(EFI_HANDLE h,void *name){
 (void)name;assert(h==paths[1]);selected++;void *p=malloc(8);assert(p);return memcpy(p,h,8);
}
'''
tests=r'''
int main(void){
 EFI_DEVICE_PATH_PROTOCOL *result=BmExpandMediaDevicePath(parent,NULL);
 assert(result && selected==1 && reads==1);free(result);
 missing=1;result=BmExpandMediaDevicePath(parent,NULL);
 assert(!result && selected==1 && reads==2);
 puts("EFI media expansion: skip pathless filesystem, reach valid candidate, all pathless passed");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='t70-media-path-') as d:
    d=Path(d);(d/'test.c').write_text(shim+'EFI_DEVICE_PATH_PROTOCOL *\n'+s[a:end]+tests)
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-fno-sanitize-recover=all',str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
