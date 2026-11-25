#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Test actual load-option filtering with host-only variable/allocation mocks."""
from pathlib import Path
import subprocess, tempfile, sys
root=Path(__file__).resolve().parents[4]
p='MdeModulePkg/Library/UefiBootManagerLib/BmLoadOption.c'
s=(root/'Mu_Basecore'/p).read_text()
if '--baseline' in sys.argv[1:] or '--allocation-baseline' in sys.argv[1:]:
    revision = '32b278b' if '--allocation-baseline' in sys.argv[1:] else '4697e33'
    s=subprocess.check_output(['git','-C',str(root/'Mu_Basecore'),'show',revision+':'+p],text=True)
a=s.index('\nEfiBootManagerGetLoadOptions (')+1; b=s.index('{',a); end=b+1; depth=1
while depth:
    depth+=(s[end]=='{')-(s[end]=='}');end+=1
shim=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
typedef unsigned long UINTN;
typedef uint16_t UINT16;
typedef uint16_t CHAR16;
typedef int EFI_STATUS;
typedef int EFI_BOOT_MANAGER_LOAD_OPTION_TYPE;
typedef void VOID;
typedef struct { UINTN OptionNumber; char *Description; } EFI_BOOT_MANAGER_LOAD_OPTION;
typedef struct { int OptionType; EFI_BOOT_MANAGER_LOAD_OPTION *Options;
 UINTN OptionCount; void *Guid; } BM_COLLECT_LOAD_OPTIONS_PARAM;
#define OUT
#define IN
#define EFI_ERROR(s) ((s)!=0)
#define ASSERT(x) ((void)0)
#define DEBUG(x) ((void)0)
#define BM_OPTION_NAME_LEN 16
#define LoadOptionTypeDriver 0
#define LoadOptionTypeSysPrep 1
#define LoadOptionTypeBoot 2
#define LoadOptionTypePlatformRecovery 3
static void *mBmLoadOptionOrderName[4], *mBmLoadOptionName[4];
static int gEfiGlobalVariableGuid;
static int mode, shrinkCalls, deleted, orderOutstanding;
static void *order;
static UINT16 current;
static void GetEfiGlobalVariable2(void *name,void **out,UINTN *bytes) {
 (void)name; *bytes=3*sizeof(UINT16);UINT16 *a=malloc(*bytes);
 assert(a);order=a;orderOutstanding++;a[0]=0;a[1]=1;a[2]=2;*out=a;
}
static void *AllocatePool(UINTN n){return mode==3?NULL:malloc(n);}
static void FreePool(void *p){if(p==order)orderOutstanding--;free(p);}
static void UnicodeSPrint(CHAR16 *n,UINTN size,const void *fmt,void *name,UINT16 num) {
 (void)n;(void)size;(void)fmt;(void)name; current=num;
}
static EFI_STATUS EfiBootManagerVariableToLoadOption(CHAR16 *n,EFI_BOOT_MANAGER_LOAD_OPTION *o) {
 (void)n;if(mode==2 || (mode==1 && current!=1))return 1;
 o->OptionNumber=current;o->Description=malloc(1);assert(o->Description);return 0;
}
static void EfiBootManagerDeleteLoadOptionVariable(UINT16 n,int t){(void)n;(void)t;deleted++;}
static void BmCollectLoadOptions(void){}
static void BmForEachVariable(void (*f)(void),void *p){(void)f;(void)p;}
// The production allocator retains the old buffer on allocation failure.
static __attribute__((unused)) void *ReallocatePool(UINTN old,UINTN size,void *p) {
 (void)old;(void)size;(void)p;shrinkCalls++;return NULL;
}
'''
tests=r'''
int main(void){
 for(mode=0;mode<4;mode++){
  UINTN count=99;deleted=shrinkCalls=0;
  EFI_BOOT_MANAGER_LOAD_OPTION *options=EfiBootManagerGetLoadOptions(&count,LoadOptionTypeBoot);
  assert(orderOutstanding==0);
  if(mode==3){assert(!options && count==3 && deleted==0);continue;}
  assert(options);assert(count==(mode==0?3:mode==1?1:0));
  assert(deleted==(mode==0?0:mode==1?2:3));assert(shrinkCalls==0);
  if(mode==1)assert(options[0].OptionNumber==1);
  for(UINTN i=0;i<count;i++)free(options[i].Description);free(options);
 }
 puts("Boot option filtering: all valid, mixed stale/valid, all stale, and initial allocation failure passed");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='t70-boot-filter-') as d:
    d=Path(d);(d/'test.c').write_text(shim+'EFI_BOOT_MANAGER_LOAD_OPTION *\n'+s[a:end]+tests)
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-fno-sanitize-recover=all',str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
