#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host-test the production refresh function, with enumeration/NVRAM mocked.
No EFI images are loaded and no firmware variables or devices are accessed.
"""
from pathlib import Path
import subprocess
import tempfile
import sys
root = Path(__file__).resolve().parents[4]
path = 'MdeModulePkg/Library/UefiBootManagerLib/BmBoot.c'
source = (root / 'Mu_Basecore' / path).read_text()
if '--baseline' in sys.argv[1:]:
    source = subprocess.check_output(['git', '-C', str(root / 'Mu_Basecore'),
        'show', '63efa8db72b2b43cbece0c865fd9a0b405d531ca:' + path], text=True)
start = source.index('EfiBootManagerRefreshAllBootOption (')
brace = source.index('{', start)
depth = 1
end = brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
function = 'void\n' + source[start:end]
shim = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef unsigned long UINTN;
typedef int EFI_STATUS;
typedef void VOID;
typedef struct { int Value; } EFI_GUID;
typedef struct { int Type, SubType; } PATH;
typedef struct { void *OptionalData; UINTN OptionalDataSize;
  PATH *FilePath; UINTN OptionNumber; } EFI_BOOT_MANAGER_LOAD_OPTION;
#define CONST const
#define EFI_ERROR(x) ((x)!=0)
#define ASSERT_EFI_ERROR(x) assert((x)==0)
#define BBS_DEVICE_PATH 5
#define BBS_BBS_DP 1
#define LoadOptionTypeBoot 0
#define DevicePathType(p) ((p)->Type)
#define DevicePathSubType(p) ((p)->SubType)
typedef struct { EFI_STATUS (*RefreshAllBootOptions)(
 const EFI_BOOT_MANAGER_LOAD_OPTION *, const UINTN,
 EFI_BOOT_MANAGER_LOAD_OPTION **, UINTN *); } EDKII_PLATFORM_BOOT_MANAGER_PROTOCOL;
static void (*mBmRefreshLegacyBootOption)(void);
static EFI_GUID mBmAutoCreateBootOptionGuid, gEdkiiPlatformBootManagerProtocolGuid;
static EFI_STATUS Locate(void *g, void *r, void **p) {
 (void)g; (void)r; (void)p; return 1;
}
static struct { EFI_STATUS (*LocateProtocol)(void *,void *,void **); } bs={Locate}, *gBS=&bs;
static unsigned added, deleted, freed, mode;
static PATH path={1,1};
static EFI_BOOT_MANAGER_LOAD_OPTION fresh[2], existing[1];
static void *AllocateCopyPool(UINTN n, const void *p) {
 void *b=malloc(n); assert(b); return memcpy(b,p,n);
}
static EFI_BOOT_MANAGER_LOAD_OPTION *BmEnumerateBootOptions(UINTN *n) {
 memset(fresh,0,sizeof fresh); fresh[0].FilePath=fresh[1].FilePath=&path;
 fresh[0].OptionNumber=10; fresh[1].OptionNumber=11; *n=2; return fresh;
}
static EFI_BOOT_MANAGER_LOAD_OPTION *EfiBootManagerGetLoadOptions(UINTN *n, int t) {
 (void)t; *n=mode==1?2:mode>=2?1:0;
 if(mode<2)return NULL;
 memset(existing,0,sizeof existing);existing[0].FilePath=&path;
 existing[0].OptionNumber=mode==2?10:99; return existing;
}
static int BmIsAutoCreateBootOption(EFI_BOOT_MANAGER_LOAD_OPTION *p) {(void)p; return 1;}
static int EfiBootManagerFindLoadOption(EFI_BOOT_MANAGER_LOAD_OPTION *p,
 EFI_BOOT_MANAGER_LOAD_OPTION *list, UINTN n) {
 for(UINTN i=0;i<n;i++)if(p->OptionNumber==list[i].OptionNumber)return (int)i;
 return -1;
}
static EFI_STATUS EfiBootManagerDeleteLoadOptionVariable(UINTN n,int t) {
 (void)n;(void)t;deleted++;return 0;
}
static EFI_STATUS EfiBootManagerAddLoadOptionVariable(EFI_BOOT_MANAGER_LOAD_OPTION *p,UINTN pos) {
 assert(p && pos==(UINTN)-1);added++;return mode==4?1:0;
}
static void EfiBootManagerFreeLoadOptions(EFI_BOOT_MANAGER_LOAD_OPTION *p,UINTN n) {
 for(UINTN i=0;i<n;i++)free(p[i].OptionalData);freed++;
}
'''
tests = r'''
int main(void) {
 for(mode=0;mode<5;mode++) {
  added=deleted=freed=0;
  EfiBootManagerRefreshAllBootOption();
  assert(added==(mode==1?0:mode==2?1:2));
  assert(deleted==(mode>=3?1:0));
  assert(freed==(mode>=2?2:1));
 }
 puts("Boot refresh: 5 scenarios passed (empty order, allocation failure, existing match, stale entry, failed writes)");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='t70-boot-refresh-') as tmp:
    tmp = Path(tmp)
    (tmp/'test.c').write_text(shim + function + tests)
    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
        str(tmp/'test.c'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
