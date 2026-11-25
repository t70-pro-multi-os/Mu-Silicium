// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual boot application with substituted firmware/file/hash services.
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Applications/LinuxBoot/BootPayload.h"
#undef T70_KERNEL_BYTES
#undef T70_INITRD_BYTES
#undef T70_MODULES_BYTES
#define T70_KERNEL_BYTES 128U
#define T70_INITRD_BYTES 17U
#define T70_MODULES_BYTES 23U
#include "../Applications/LinuxBoot/LinuxBoot.c"

EFI_BOOT_SERVICES Services;EFI_BOOT_SERVICES *gBS=&Services;
EFI_RUNTIME_SERVICES Runtime;EFI_RUNTIME_SERVICES *gRT=&Runtime;
EFI_SYSTEM_TABLE System;EFI_SYSTEM_TABLE *gST=&System;
EFI_GUID gFdtTableGuid,gEfiFileInfoGuid,gEfiLoadedImageProtocolGuid,gEfiBlockIoProtocolGuid;
EFI_GUID gEfiSimpleFileSystemProtocolGuid,gEfiDevicePathProtocolGuid,gEfiLoadFile2ProtocolGuid;
EFI_GUID gEfiEventExitBootServicesGuid;
static EFI_LOADED_IMAGE_PROTOCOL Own,*ChildPointer;
#define Child (*ChildPointer)
static int StartUnloads,StartSucceeds;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;
static EFI_BLOCK_IO_MEDIA Media;static EFI_BLOCK_IO_PROTOCOL Block;
static EFI_FILE_PROTOCOL Root,File[3];
static UINT8 Payload[3][128],Tree[256];
static UINTN Length[3]={128,17,23},Position[3];
static VOID *Allocations[32];static unsigned AllocCount,FreeCount,Opens,Closes,Starts,Loads,Unloads,Installs,Uninstalls,Events;
static int FaultRead,FaultInfo,FaultHash,FaultChild,FaultInstall,FaultUninstall,FaultLoad,FaultAllocate,FaultRuntime,FaultTree,ExistingInitrd,WatchdogFails,FaultEvent,FaultCloseEvent;
static int IsRegistered,Halted,ExpectedDevices,FaultLid,FaultTreeInstall,FaultTreeRestore;
static VOID *PublishedTree,*PreparedTree;static unsigned LidPreparations,TreeInstalls,TreeRestores;static jmp_buf HaltJump;
static EFI_DEVICE_PATH_PROTOCOL EndPath={END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};
static BOOLEAN Same16(CONST CHAR16 *A,CONST CHAR16 *B) { while (*A && *A==*B) { A++;B++; }return *A==*B; }
VOID *EFIAPI AllocatePool(UINTN N) {
  if (FaultAllocate) { FaultAllocate=0;return NULL; }
  VOID *P=malloc(N);assert(P && AllocCount<32);Allocations[AllocCount++]=P;return P;
}
VOID EFIAPI FreePool(VOID *P) {
  unsigned I;for (I=0;I<AllocCount;I++) if (Allocations[I]==P) break;
  assert(I<AllocCount);Allocations[I]=NULL;FreeCount++;free(P);
}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N) { return memmove(D,S,N); }
VOID *EFIAPI ZeroMem(VOID *D,UINTN N) { return memset(D,0,N); }
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N) { return memcmp(A,B,N); }
UINTN EFIAPI Print(CONST CHAR16 *Format,...) { (VOID)Format;return 0; }
VOID EFIAPI CpuDeadLoop(VOID) { Halted=1;longjmp(HaltJump,1); }
UINT64 EFIAPI MmioRead64(UINTN Address) {
  switch (Address-HANDOFF_BASE) {
    case 0:return HANDOFF_MAGIC;
    case 0x58:return EFI_SUCCESS;
    case 0x48:return (UINTN)Tree;
    case 0x50:return sizeof(Tree);
  }
  assert(0);return 0;
}
UINT32 EFIAPI Fdt32ToCpu(UINT32 V) { return __builtin_bswap32(V); }
INT32 EFIAPI FdtCheckFull(CONST VOID *T,UINTN N) { assert(T==Tree && N==sizeof(Tree));return FaultTree?-1:0; }
EFI_STATUS BuildRuntimeMemoryMap(CONST VOID *T,UINTN N,EFI_MEMORY_REGION_DESCRIPTOR *R,UINT8 *Count) {
  assert(T==Tree && N==sizeof(Tree) && R && Count && *Count==0);
  return FaultRuntime?EFI_COMPROMISED_DATA:EFI_SUCCESS;
}
EFI_STATUS T70PrepareLinuxLidTree(CONST VOID *Original,UINTN Bytes,VOID *Output,UINTN Capacity) {
  assert(Original==Tree && Bytes==sizeof(Tree) && Output && Capacity==Bytes+T70_LID_TREE_SLACK);
  LidPreparations++;PreparedTree=Output;return FaultLid?EFI_UNSUPPORTED:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TreeTable(EFI_GUID *Guid,VOID *Table) {
  assert(Guid==&gFdtTableGuid && Table);
  if (Table==Tree) {
    TreeRestores++;assert(PublishedTree==PreparedTree);
    if (FaultTreeRestore) return EFI_DEVICE_ERROR;
  } else {
    TreeInstalls++;assert(Table==PreparedTree && PublishedTree==Tree && LidPreparations==1);
    if (FaultTreeInstall) return EFI_OUT_OF_RESOURCES;
  }
  PublishedTree=Table;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI EfiGetSystemConfigurationTable(EFI_GUID *Guid,VOID **Table) {
  assert(Guid==&gFdtTableGuid);*Table=Tree;return EFI_SUCCESS;
}
BOOLEAN EFIAPI Sha256HashAll(CONST VOID *Data,UINTN N,UINT8 *Hash) {
  const UINT8 *Hashes[]={mKernelHash,mInitrdHash,mModulesHash};
  for (unsigned I=0;I<3;I++) if (N==Length[I] && !memcmp(Data,Payload[I],N)) {
    memcpy(Hash,Hashes[I],32);if (FaultHash) Hash[0]^=1;return TRUE;
  }
  memset(Hash,0,32);return TRUE;
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI FileDevicePath(EFI_HANDLE Device,CONST CHAR16 *Name) {
  assert(Device==(EFI_HANDLE)2 && Same16(Name,L"\\EFI\\Teclast\\Image"));
  VOID *P=AllocatePool(sizeof(EndPath));if (P) memcpy(P,&EndPath,sizeof(EndPath));return P;
}
BOOLEAN EFIAPI IsDevicePathEnd(CONST VOID *Path) {
  CONST EFI_DEVICE_PATH_PROTOCOL *P=Path;
  return P->Type==END_DEVICE_PATH_TYPE && P->SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE && P->Length[0]==4 && P->Length[1]==0;
}
static unsigned FileIndex(EFI_FILE_PROTOCOL *This) { assert(This>=File && This<File+3);return (unsigned)(This-File); }
static EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *This) { assert(This==&Root || (This>=File && This<File+3));Closes++;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *This,UINTN *N,VOID *Buffer) {
  unsigned I=FileIndex(This);UINTN Left=Length[I]-Position[I];
  if (FaultRead) { *N=0;return EFI_SUCCESS; }
  // Exercise partial filesystem reads rather than assuming one full read.
  if (*N>7) *N=7;
  if (*N>Left) *N=Left;
  memcpy(Buffer,Payload[I]+Position[I],*N);Position[I]+=*N;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL *This,EFI_GUID *Guid,UINTN *N,VOID *Buffer) {
  unsigned I=FileIndex(This);assert(Guid==&gEfiFileInfoGuid);
  if (!Buffer) { *N=sizeof(EFI_FILE_INFO)+32;return EFI_BUFFER_TOO_SMALL; }
  EFI_FILE_INFO *P=Buffer;memset(P,0,*N);P->Size=*N;P->FileSize=Length[I]+(FaultInfo?1:0);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *This,EFI_FILE_PROTOCOL **Out,CHAR16 *Path,UINT64 Mode,UINT64 Attributes) {
  static CHAR16 *Names[]={L"\\EFI\\Teclast\\Image",L"\\EFI\\Teclast\\early-userspace.cpio.gz",L"\\EFI\\Teclast\\linux-stock-module-payload.cpio"};
  assert(This==&Root && Mode==EFI_FILE_MODE_READ && Attributes==0);
  for (unsigned I=0;I<3;I++) if (Same16(Path,Names[I])) { *Out=File+I;Opens++;return EFI_SUCCESS; }
  assert(0);return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,EFI_FILE_PROTOCOL **Out) { assert(This==&Fs);*Out=&Root;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Protocol(EFI_HANDLE Handle,EFI_GUID *Guid,VOID **Out) {
  if (Guid==&gEfiLoadedImageProtocolGuid) {
    if (Handle==(EFI_HANDLE)1) { *Out=&Own;return EFI_SUCCESS; }
    if (Handle==(EFI_HANDLE)3) { if (FaultChild || !ChildPointer) { *Out=(VOID *)(UINTN)0xdead; return EFI_NOT_FOUND; }*Out=&Child;return EFI_SUCCESS; }
  }
  if (Handle==(EFI_HANDLE)2 && Guid==&gEfiBlockIoProtocolGuid) { *Out=&Block;return EFI_SUCCESS; }
  if (Handle==(EFI_HANDLE)2 && Guid==&gEfiSimpleFileSystemProtocolGuid) { *Out=&Fs;return EFI_SUCCESS; }
  assert(0);return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,EFI_DEVICE_PATH_PROTOCOL **Path,EFI_HANDLE *Handle) {
  assert(Guid==&gEfiLoadFile2ProtocolGuid && *Path==(VOID *)&mInitrdPath);(VOID)Handle;
  return ExistingInitrd?EFI_SUCCESS:EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Load(BOOLEAN BootPolicy,EFI_HANDLE Parent,EFI_DEVICE_PATH_PROTOCOL *Path,VOID *Image,UINTN N,EFI_HANDLE *Handle) {
  Loads++;assert(!BootPolicy && Parent==(EFI_HANDLE)1 && Path && Image && N==Length[0]);
  if (FaultLoad==1) { *Handle=(EFI_HANDLE)(UINTN)0xdead; return EFI_LOAD_ERROR; }
  if (FaultLoad==2) { *Handle=(EFI_HANDLE)3; return EFI_SECURITY_VIOLATION; }
  *Handle=(EFI_HANDLE)3;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *Handle,...) {
  Installs++;assert(Handle && mInitrd.Part[0] && mInitrd.Part[1]);
  if (FaultInstall) return EFI_DEVICE_ERROR;
  *Handle=(EFI_HANDLE)4;IsRegistered=1;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Uninstall(EFI_HANDLE Handle,...) {
  Uninstalls++;assert(Handle==(EFI_HANDLE)4 && IsRegistered && mInitrd.Part[0] && mInitrd.Part[1]);
  if (FaultUninstall) return EFI_ACCESS_DENIED;
  IsRegistered=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Event(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Guid,EFI_EVENT *Out) {
  assert(Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_NOTIFY && Notify==ExitBoot && !Context && Guid==&gEfiEventExitBootServicesGuid);
  if (FaultEvent) { *Out=(EFI_EVENT)(UINTN)0xdead; return EFI_OUT_OF_RESOURCES; }
  Events++;*Out=(EFI_EVENT)5;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CloseEvent(EFI_EVENT E) { assert(E==(EFI_EVENT)5);if (FaultCloseEvent) return EFI_DEVICE_ERROR;Events--;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Watchdog(UINTN Timeout,UINT64 Code,UINTN N,CHAR16 *Data) { assert(!Timeout && !Code && !N && !Data);return WatchdogFails?EFI_DEVICE_ERROR:EFI_SUCCESS; }
static EFI_STATUS EFIAPI Start(EFI_HANDLE Handle,UINTN *N,CHAR16 **Data) {
  Starts++;assert(PublishedTree==PreparedTree && TreeInstalls==1);assert(Handle==(EFI_HANDLE)3 && !N && !Data && IsRegistered && Events==1);
  CHAR16 *Options[]={mOptions,mDeviceOptions,mDesktopOptions,mPixmanOptions,mFullDeviceOptions};
  const UINTN Sizes[]={sizeof(mOptions),sizeof(mDeviceOptions),sizeof(mDesktopOptions),sizeof(mPixmanOptions),sizeof(mFullDeviceOptions)};
  assert(ExpectedDevices>=0 && ExpectedDevices<5);
  assert(Child.LoadOptions==Options[ExpectedDevices] && Child.LoadOptionsSize==Sizes[ExpectedDevices]);
  const CHAR16 *ExpectedOptions[]={
    L"acpi=off rdinit=/init loglevel=7 firmware_class.path=/lib/firmware",
    L"acpi=off rdinit=/init loglevel=7 teclast.modules=usb,input,display,storage teclast.console=drm firmware_class.path=/lib/firmware",
    L"acpi=off rdinit=/init loglevel=7 teclast.modules=usb,input,display,storage teclast.console=desktop-vulkan firmware_class.path=/lib/firmware",
    L"acpi=off rdinit=/init loglevel=7 teclast.modules=usb,input,display,storage teclast.console=desktop-pixman firmware_class.path=/lib/firmware",
    L"acpi=off rdinit=/init loglevel=7 teclast.modules=usb,input,display,storage,power,audio,sensors,camera,connectivity,cellular,sim_radio teclast.console=drm firmware_class.path=/lib/firmware"};
  const CHAR16 *Expected=ExpectedOptions[ExpectedDevices];
  UINTN OptionIndex=0;
  for (;Expected[OptionIndex];OptionIndex++) assert(((CHAR16 *)Child.LoadOptions)[OptionIndex]==Expected[OptionIndex]);
  assert(((CHAR16 *)Child.LoadOptions)[OptionIndex]==0 && Child.LoadOptionsSize==(OptionIndex+1)*sizeof(CHAR16));
  UINTN Size=0;UINT8 Buffer[48];
  assert(LoadInitrd(&mInitrd.Load,&EndPath,FALSE,NULL,NULL)==EFI_INVALID_PARAMETER);
  assert(LoadInitrd(&mInitrd.Load,&EndPath,TRUE,&Size,NULL)==EFI_UNSUPPORTED);
  assert(LoadInitrd(&mInitrd.Load,&EndPath,FALSE,&Size,NULL)==EFI_BUFFER_TOO_SMALL && Size==43);
  Size=42;assert(LoadInitrd(&mInitrd.Load,&EndPath,FALSE,&Size,Buffer)==EFI_BUFFER_TOO_SMALL && Size==43);
  assert(LoadInitrd(&mInitrd.Load,&EndPath,FALSE,&Size,Buffer)==EFI_SUCCESS && Size==43);
  assert(!memcmp(Buffer,Payload[1],17) && Buffer[17]==0 && Buffer[18]==0 && Buffer[19]==0);
  assert(!memcmp(Buffer+20,Payload[2],23));
  {
    UINTN Saved=mInitrd.Bytes[0];
    mInitrd.Bytes[0]=MAX_UINTN;
    Size=0;assert(LoadInitrd(&mInitrd.Load,&EndPath,FALSE,&Size,NULL)==EFI_NOT_READY);
    mInitrd.Bytes[0]=Saved;
  }
  if (StartUnloads) { free(ChildPointer); ChildPointer=NULL; }
  return StartSucceeds?EFI_SUCCESS:EFI_LOAD_ERROR; // No kernel or tablet execution.
}
static EFI_STATUS EFIAPI Unload(EFI_HANDLE Handle) { Unloads++;assert(Handle==(EFI_HANDLE)3 && Child.LoadOptions==NULL);return EFI_SUCCESS; }
static void Reset(void) {
  free(ChildPointer); ChildPointer=calloc(1,sizeof(*ChildPointer)); assert(ChildPointer);
  StartUnloads=StartSucceeds=0;
  for (unsigned I=0;I<AllocCount;I++) if (Allocations[I]) free(Allocations[I]);
  memset(Allocations,0,sizeof(Allocations));AllocCount=FreeCount=Opens=Closes=Starts=Loads=Unloads=Installs=Uninstalls=Events=0;
  FaultRead=FaultInfo=FaultHash=FaultChild=FaultInstall=FaultUninstall=FaultLoad=FaultAllocate=FaultRuntime=FaultTree=ExistingInitrd=WatchdogFails=FaultEvent=FaultCloseEvent=0;
  ExpectedDevices=0;IsRegistered=Halted=0;FaultLid=FaultTreeInstall=FaultTreeRestore=0;
  PublishedTree=Tree;PreparedTree=NULL;LidPreparations=TreeInstalls=TreeRestores=0;memset(Position,0,sizeof(Position));memset(&Child,0,sizeof(Child));
  ((FDT_HEADER *)Tree)->TotalSize=__builtin_bswap32(sizeof(Tree));
  Own.DeviceHandle=(EFI_HANDLE)2;Media.MediaPresent=Media.RemovableMedia=TRUE;Block.Media=&Media;
  for (unsigned I=0;I<3;I++) { memset(Payload[I],0x20+I,Length[I]);File[I].Read=Read;File[I].GetInfo=Info;File[I].Close=Close; }
  Payload[0][0]='M';Payload[0][1]='Z';memcpy(Payload[0]+56,"ARM\x64",4);
  Root.Open=Open;Root.Close=Close;Fs.OpenVolume=Volume;
  Services.HandleProtocol=Protocol;Services.LocateDevicePath=Locate;Services.LoadImage=Load;
  Services.StartImage=Start;Services.UnloadImage=Unload;Services.InstallMultipleProtocolInterfaces=Install;
  Services.UninstallMultipleProtocolInterfaces=Uninstall;Services.CreateEventEx=Event;
  Services.CloseEvent=CloseEvent;Services.SetWatchdogTimer=Watchdog;Services.InstallConfigurationTable=TreeTable;
}
static void Clean(void) { assert(AllocCount==FreeCount && !IsRegistered && Events==0 && PublishedTree==Tree); }
int main(void) {
  Reset();StartUnloads=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_LOAD_ERROR && Unloads==0);Clean();
  Reset();StartUnloads=StartSucceeds=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_SUCCESS && Unloads==0);Clean();
  Reset();assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_LOAD_ERROR);assert(Starts==1 && Loads==1 && Opens==3 && Closes==4 && Unloads==1);Clean();
  Reset();FaultRuntime=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_COMPROMISED_DATA && Opens==0);Clean();
  Reset();FaultTree=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_COMPROMISED_DATA && Opens==0);Clean();
  Reset();Media.RemovableMedia=FALSE;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_ACCESS_DENIED && Opens==0);Clean();
  Reset();ExistingInitrd=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_ALREADY_STARTED && Opens==0);Clean();
  Reset();FaultAllocate=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_OUT_OF_RESOURCES);Clean();
  Reset();FaultRead=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_DEVICE_ERROR);Clean();
  Reset();FaultInfo=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_COMPROMISED_DATA);Clean();
  Reset();FaultHash=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_SECURITY_VIOLATION && Loads==0);Clean();
  Reset();FaultLoad=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_LOAD_ERROR && Starts==0 && Unloads==0);Clean();
  Reset();FaultLoad=2;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_SECURITY_VIOLATION && Starts==0 && Unloads==1);Clean();
  Reset();FaultChild=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_NOT_FOUND && Unloads==1);Clean();
  Reset();FaultInstall=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_DEVICE_ERROR && Starts==0 && Unloads==1);Clean();
  Reset();WatchdogFails=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_DEVICE_ERROR && Starts==0 && Uninstalls==1);Clean();
  Reset();FaultEvent=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_OUT_OF_RESOURCES && Starts==0 && Uninstalls==1);Clean();
  Reset();FaultCloseEvent=1;
  if (!setjmp(HaltJump)) { (VOID)BootPreparedLinux((EFI_HANDLE)1);assert(0); }
  assert(Halted && !IsRegistered && Events==1 && FreeCount<AllocCount);
  Reset();FaultUninstall=1;
  if (!setjmp(HaltJump)) { (VOID)BootPreparedLinux((EFI_HANDLE)1);assert(0); }
  assert(Halted && IsRegistered && mInitrd.Part[0] && mInitrd.Part[1] && FreeCount<AllocCount);
  Reset();Clean();
  Reset();ExpectedDevices=1;assert(BootPreparedLinuxProfile((EFI_HANDLE)1,TRUE)==EFI_LOAD_ERROR && Starts==1);Clean();
  for (unsigned Profile=2;Profile<=4;Profile++) {
    Reset();ExpectedDevices=(int)Profile;
    assert(BootPreparedLinuxProfile((EFI_HANDLE)1,Profile)==EFI_LOAD_ERROR && Starts==1);Clean();
  }
  Reset();assert(BootPreparedLinuxProfile((EFI_HANDLE)1,5)==EFI_INVALID_PARAMETER && !Starts && !Opens);Clean();
  Reset();FaultLid=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_UNSUPPORTED && !Starts && !TreeInstalls);Clean();
  Reset();FaultTreeInstall=1;assert(BootPreparedLinux((EFI_HANDLE)1)==EFI_OUT_OF_RESOURCES && !Starts && !TreeRestores);Clean();
  Reset();FaultTreeRestore=1;
  if (!setjmp(HaltJump)) { (VOID)BootPreparedLinux((EFI_HANDLE)1);assert(0); }
  assert(Halted && PublishedTree==PreparedTree && IsRegistered && FreeCount<AllocCount && TreeRestores==1);
  Reset();Clean();
  puts("Linux boot application: 26 actual-function scenarios passed (runtime tree, removable-only reads, partial reads, pinned hash gate, initrd protocol and failure resource lifetime); no kernel launch");
  return 0;
}
