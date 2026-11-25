// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute the production launcher with firmware services replaced, never a real kernel.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Applications/XnuBoot/XnuBoot.c"

EFI_BOOT_SERVICES Services;EFI_BOOT_SERVICES *gBS=&Services;
EFI_SYSTEM_TABLE System;EFI_SYSTEM_TABLE *gST=&System;
EFI_GUID gFdtTableGuid,gEfiLoadedImageProtocolGuid,gEfiBlockIoProtocolGuid;
EFI_GUID gEfiSimpleFileSystemProtocolGuid,gUniXNULoaderProtocolGuid,gEfiEventExitBootServicesGuid,gEfiGraphicsOutputProtocolGuid;
static EFI_LOADED_IMAGE_PROTOCOL Own;
static EFI_BLOCK_IO_PROTOCOL Block;static EFI_BLOCK_IO_MEDIA Media;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;static EFI_SIMPLE_TEXT_INPUT_PROTOCOL Console;
static UNIXNU_LOADER_PROTOCOL Loader;
static EFI_DEVICE_PATH_PROTOCOL EndPath={END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};
static UINT8 Tree[256];
static EFI_EVENT_NOTIFY Notify;
static VOID *LastAllocation;
static unsigned Loads,Starts,Boots,Unloads,Creates,Closes,Allocations,LiveAllocations;
static BOOLEAN Exited,Halted;
static jmp_buf HaltJump;
enum {
  OK, BAD_MAGIC, BAD_HANDOFF, WRONG_TREE, BAD_SIZE, BAD_FDT, BAD_RESERVATIONS,
  NO_OWN, INTERNAL_MEDIA, NO_MEDIA, NO_FS, EXISTING_LOADER, LOCATE_ERROR,
  NO_PATH_MEMORY, LOAD_ERROR, LOAD_SECURITY, EVENT_ERROR, START_ERROR,
  NO_PROTOCOL, OLD_REVISION, NO_BOOT, WATCHDOG_ERROR, BOOT_RETURNS_SUCCESS,
  BOOT_RETURNS_AFTER_EXIT, START_RETURNS_AFTER_EXIT, CLOSE_EVENT_ERROR, UNLOAD_ERROR, NO_GOP, BAD_PLATFORM
};
static int Fault;
UINTN EFIAPI ArmReadMpidr(VOID) { return 0; }
UINTN EFIAPI ArmGenericTimerGetTimerFreq(VOID) { return 13000000; }
EFI_STATUS T70XnuValidatePlatform(CONST VOID *P,UINTN N,CONST EFI_MEMORY_REGION_DESCRIPTOR *R,
  UINT8 C,CONST EFI_GRAPHICS_OUTPUT_PROTOCOL *G,UINT64 M,UINT32 F) {
  assert(P==Tree && N==sizeof(Tree) && R && C==0 && G && M==0 && F==13000000);
  return Fault==BAD_PLATFORM?EFI_UNSUPPORTED:EFI_SUCCESS;
}
static BOOLEAN Same16(CONST CHAR16 *A,CONST CHAR16 *B) {
  while (*A && *A==*B) { A++;B++; }return *A==*B;
}
static VOID Active(VOID) { assert(!Exited); }
VOID *EFIAPI AllocatePool(UINTN N) {
  Active();if (Fault==NO_PATH_MEMORY) return NULL;
  VOID *P=malloc(N);assert(P);Allocations++;LiveAllocations++;LastAllocation=P;return P;
}
VOID EFIAPI FreePool(VOID *P) { Active();assert(P && LiveAllocations);LiveAllocations--;free(P); }
UINTN EFIAPI Print(CONST CHAR16 *Format,...) { Active();(VOID)Format;return 0; }
VOID EFIAPI CpuDeadLoop(VOID) { Halted=TRUE;longjmp(HaltJump,1); }
UINT64 EFIAPI MmioRead64(UINTN Address) {
  Active();
  switch (Address-HANDOFF_BASE) {
    case 0:return Fault==BAD_MAGIC?0:HANDOFF_MAGIC;
    case 0x58:return Fault==BAD_HANDOFF?EFI_NOT_READY:EFI_SUCCESS;
    case 0x48:return (UINTN)Tree;
    case 0x50:return Fault==BAD_SIZE?MAX_DTB_SIZE+1:sizeof(Tree);
  }
  assert(0);return 0;
}
UINT32 EFIAPI Fdt32ToCpu(UINT32 N) { return __builtin_bswap32(N); }
INT32 EFIAPI FdtCheckFull(CONST VOID *P,UINTN N) {
  Active();assert(P==Tree && N==sizeof(Tree));return Fault==BAD_FDT?-1:0;
}
EFI_STATUS BuildRuntimeMemoryMap(CONST VOID *P,UINTN N,EFI_MEMORY_REGION_DESCRIPTOR *R,UINT8 *Count) {
  Active();assert(P==Tree && N==sizeof(Tree) && R && *Count==0);
  return Fault==BAD_RESERVATIONS?EFI_COMPROMISED_DATA:EFI_SUCCESS;
}
EFI_STATUS EFIAPI EfiGetSystemConfigurationTable(EFI_GUID *Guid,VOID **Out) {
  Active();assert(Guid==&gFdtTableGuid);*Out=Fault==WRONG_TREE?Tree+4:Tree;return EFI_SUCCESS;
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI FileDevicePath(EFI_HANDLE Device,CONST CHAR16 *Path) {
  Active();assert(Device==(EFI_HANDLE)2 && Same16(Path,L"\\EFI\\Teclast\\DarwinBoot.efi"));
  VOID *P=AllocatePool(sizeof(EndPath));if (P) memcpy(P,&EndPath,sizeof(EndPath));return P;
}
static EFI_STATUS EFIAPI Protocol(EFI_HANDLE Handle,EFI_GUID *Guid,VOID **Out) {
  Active();
  if (Guid==&gEfiGraphicsOutputProtocolGuid) {
    *Out=(VOID *)5;return Fault==NO_GOP?EFI_NOT_FOUND:EFI_SUCCESS;
  }
  if (Handle==(EFI_HANDLE)1 && Guid==&gEfiLoadedImageProtocolGuid) {
    if (Fault==NO_OWN) { return EFI_NOT_FOUND; }*Out=&Own;return EFI_SUCCESS;
  }
  if (Handle==(EFI_HANDLE)2 && Guid==&gEfiBlockIoProtocolGuid) { *Out=&Block;return EFI_SUCCESS; }
  if (Handle==(EFI_HANDLE)2 && Guid==&gEfiSimpleFileSystemProtocolGuid) {
    if (Fault==NO_FS) { return EFI_NOT_FOUND; }*Out=&Fs;return EFI_SUCCESS;
  }
  if (Handle==(EFI_HANDLE)3 && Guid==&gUniXNULoaderProtocolGuid) {
    if (Fault==NO_PROTOCOL) { return EFI_NOT_FOUND; }*Out=&Loader;return EFI_SUCCESS;
  }
  assert(0);return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Out) {
  Active();
  if (Guid==&gEfiGraphicsOutputProtocolGuid) {
    *Out=(VOID *)5;return Fault==NO_GOP?EFI_NOT_FOUND:EFI_SUCCESS;
  }
  assert(Guid==&gUniXNULoaderProtocolGuid && !Registration);
  if (Fault==EXISTING_LOADER) { *Out=&Loader;return EFI_SUCCESS; }
  return Fault==LOCATE_ERROR?EFI_DEVICE_ERROR:EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Load(BOOLEAN Policy,EFI_HANDLE Parent,EFI_DEVICE_PATH_PROTOCOL *Path,
                              VOID *Image,UINTN Bytes,EFI_HANDLE *Out) {
  Active();assert(!Policy && Parent==(EFI_HANDLE)1 && Path && !Image && !Bytes);Loads++;
  if (Fault==LOAD_ERROR) return EFI_LOAD_ERROR;
  *Out=(EFI_HANDLE)3;return Fault==LOAD_SECURITY?EFI_SECURITY_VIOLATION:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Event(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Callback,CONST VOID *Context,
                               CONST EFI_GUID *Group,EFI_EVENT *Out) {
  Active();Creates++;assert(Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_NOTIFY && !Context &&
                           Group==&gEfiEventExitBootServicesGuid && Callback);
  if (Fault==EVENT_ERROR) return EFI_OUT_OF_RESOURCES;
  Notify=Callback;*Out=(EFI_EVENT)4;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_EVENT Handle) {
  Active();Closes++;assert(Handle==(EFI_EVENT)4);
  if (Fault==CLOSE_EVENT_ERROR) return EFI_DEVICE_ERROR;
  Notify=NULL;return EFI_SUCCESS;
}
static VOID SimulateExit(VOID) {
  assert(Notify);Notify((EFI_EVENT)4,NULL);Exited=TRUE;
}
static EFI_STATUS EFIAPI Start(EFI_HANDLE Handle,UINTN *Bytes,CHAR16 **Data) {
  Active();Starts++;assert(Handle==(EFI_HANDLE)3 && !Bytes && !Data);
  if (Fault==START_RETURNS_AFTER_EXIT) SimulateExit();
  return Fault==START_ERROR?EFI_LOAD_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Unload(EFI_HANDLE Handle) {
  Active();assert(Handle==(EFI_HANDLE)3);Unloads++;
  return Fault==UNLOAD_ERROR?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Watchdog(UINTN Timeout,UINT64 Code,UINTN Bytes,CHAR16 *Data) {
  Active();assert(!Timeout && !Code && !Bytes && !Data);
  return Fault==WATCHDOG_ERROR?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Boot(UNIXNU_LOADER_PROTOCOL *This,CONST UNIXNU_LOADER_BOOT_REQUEST *Request) {
  Active();Boots++;
  assert(This==&Loader && Request && Request->Volume==(EFI_HANDLE)2);
  assert(Same16(Request->KernelPath,L"\\xnu\\kernel.macho"));
  assert(Same16(Request->BootArgs,L"-v keepsyms=1 debug=0x14e"));
  assert(!Request->DeviceTree && !Request->DeviceTreeSize && !Request->Ramdisk &&
         !Request->RamdiskSize && !Request->Kexts && !Request->KextCount);
  if (Fault==BOOT_RETURNS_AFTER_EXIT) SimulateExit();
  return Fault==BOOT_RETURNS_SUCCESS?EFI_SUCCESS:EFI_LOAD_ERROR;
}
static EFI_STATUS EFIAPI Wait(UINTN Count,EFI_EVENT *Events,UINTN *Index) {
  Active();assert(Count==1 && Events==&Console.WaitForKey);*Index=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ReadKey(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,EFI_INPUT_KEY *Key) {
  assert(This==&Console);Key->ScanCode=SCAN_ESC;Key->UnicodeChar=0;return EFI_SUCCESS;
}
static VOID Reset(int Scenario) {
  Fault=Scenario;Loads=Starts=Boots=Unloads=Creates=Closes=Allocations=LiveAllocations=0;
  Exited=Halted=FALSE;Notify=NULL;memset(&Services,0,sizeof(Services));memset(&System,0,sizeof(System));
  memset(&Own,0,sizeof(Own));memset(&Media,0,sizeof(Media));
  memset(Tree,0,sizeof(Tree));((FDT_HEADER *)Tree)->TotalSize=__builtin_bswap32(sizeof(Tree));
  Own.DeviceHandle=(EFI_HANDLE)2;Media.MediaPresent=Scenario!=NO_MEDIA;
  Media.RemovableMedia=Scenario!=INTERNAL_MEDIA;Block.Media=&Media;
  Loader.Revision=Scenario==OLD_REVISION?UNIXNU_LOADER_REVISION_1:UNIXNU_LOADER_REVISION;
  Loader.Boot=Scenario==NO_BOOT?NULL:Boot;
  Services.HandleProtocol=Protocol;Services.LocateProtocol=Locate;Services.LoadImage=Load;
  Services.StartImage=Start;Services.UnloadImage=Unload;Services.CreateEventEx=Event;
  Services.CloseEvent=Close;Services.SetWatchdogTimer=Watchdog;Services.WaitForEvent=Wait;
  Console.ReadKeyStroke=ReadKey;System.ConIn=&Console;
}
int main(void) {
  static CONST struct { int Scenario;EFI_STATUS Status;unsigned Load,Start,Boot,Unload,Create,Close; } Cases[]={
    {OK,EFI_LOAD_ERROR,1,1,1,1,1,1},
    {BAD_MAGIC,EFI_NOT_READY,0,0,0,0,0,0},
    {BAD_HANDOFF,EFI_NOT_READY,0,0,0,0,0,0},
    {WRONG_TREE,EFI_NOT_READY,0,0,0,0,0,0},
    {BAD_SIZE,EFI_COMPROMISED_DATA,0,0,0,0,0,0},
    {BAD_FDT,EFI_COMPROMISED_DATA,0,0,0,0,0,0},
    {BAD_RESERVATIONS,EFI_COMPROMISED_DATA,0,0,0,0,0,0},
    {NO_OWN,EFI_NOT_FOUND,0,0,0,0,0,0},
    {INTERNAL_MEDIA,EFI_ACCESS_DENIED,0,0,0,0,0,0},
    {NO_MEDIA,EFI_ACCESS_DENIED,0,0,0,0,0,0},
    {NO_FS,EFI_NOT_FOUND,0,0,0,0,0,0},
    {EXISTING_LOADER,EFI_ALREADY_STARTED,0,0,0,0,0,0},
    {LOCATE_ERROR,EFI_DEVICE_ERROR,0,0,0,0,0,0},
    {NO_PATH_MEMORY,EFI_OUT_OF_RESOURCES,0,0,0,0,0,0},
    {LOAD_ERROR,EFI_LOAD_ERROR,1,0,0,0,0,0},
    {LOAD_SECURITY,EFI_SECURITY_VIOLATION,1,0,0,1,0,0},
    {EVENT_ERROR,EFI_OUT_OF_RESOURCES,1,0,0,1,1,0},
    {START_ERROR,EFI_LOAD_ERROR,1,1,0,0,1,1},
    {NO_PROTOCOL,EFI_NOT_FOUND,1,1,0,1,1,1},
    {OLD_REVISION,EFI_UNSUPPORTED,1,1,0,1,1,1},
    {NO_BOOT,EFI_UNSUPPORTED,1,1,0,1,1,1},
    {WATCHDOG_ERROR,EFI_DEVICE_ERROR,1,1,0,1,1,1},
    {BOOT_RETURNS_SUCCESS,EFI_ABORTED,1,1,1,1,1,1},
    {UNLOAD_ERROR,EFI_DEVICE_ERROR,1,1,1,1,1,1},
    {NO_GOP,EFI_NOT_FOUND,0,0,0,0,0,0},
    {BAD_PLATFORM,EFI_UNSUPPORTED,0,0,0,0,0,0}
  };
  for (unsigned I=0;I<sizeof(Cases)/sizeof(Cases[0]);I++) {
    Reset(Cases[I].Scenario);EFI_STATUS S=BootXnu((EFI_HANDLE)1);
    assert(S==Cases[I].Status && !Halted && !LiveAllocations && !Notify);
    assert(Loads==Cases[I].Load && Starts==Cases[I].Start && Boots==Cases[I].Boot &&
           Unloads==Cases[I].Unload && Creates==Cases[I].Create && Closes==Cases[I].Close);
  }
  // A failed event close must retain application code; an exited firmware must not be called again.
  for (int I=BOOT_RETURNS_AFTER_EXIT;I<=CLOSE_EVENT_ERROR;I++) {
    Reset(I);
    if (setjmp(HaltJump)==0) { (void)BootXnu((EFI_HANDLE)1);assert(0); }
    assert(Halted && Notify && !Unloads && LiveAllocations==1);
    assert(Closes==(I==CLOSE_EVENT_ERROR?1U:0U));
    // Simulate teardown of the isolated test process; production intentionally keeps these alive.
    Exited=FALSE;Notify=NULL;FreePool(LastAllocation);assert(!LiveAllocations);
  }
  Reset(OK);assert(UefiMain((EFI_HANDLE)1,&System)==EFI_ABORTED && Loads==0);
  System.ConIn=NULL;assert(UefiMain((EFI_HANDLE)1,&System)==EFI_NOT_READY && Loads==0);
  puts("PASS: 31 launcher cases (runtime DT, removable volume, loader lifecycle, exit guard, UI)");
  return 0;
}
