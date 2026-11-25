// SPDX-License-Identifier: BSD-2-Clause-Patent
// Launch only the exact prepared stock Linux kernel from a removable volume.
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Guid/FileInfo.h>
#include <Guid/LinuxEfiInitrdMedia.h>
#include <Library/BaseCryptLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/LoadFile2.h>
#include <Protocol/SimpleFileSystem.h>
#include "BootPayload.h"
#include "LinuxLid.h"

#define HANDOFF_BASE 0x43F7F000U
#define HANDOFF_MAGIC 0x0000000148303754ULL
#define MAX_DTB_SIZE 0x200000U
extern EFI_STATUS BuildRuntimeMemoryMap(CONST VOID *,UINTN,EFI_MEMORY_REGION_DESCRIPTOR *,UINT8 *);

typedef struct { EFI_LOAD_FILE2_PROTOCOL Load; VOID *Part[2]; UINTN Bytes[2]; } INITRD_CONTEXT;
typedef struct { VENDOR_DEVICE_PATH Vendor; EFI_DEVICE_PATH_PROTOCOL End; } INITRD_PATH;
STATIC INITRD_CONTEXT mInitrd;
STATIC INITRD_PATH mInitrdPath={
  {{MEDIA_DEVICE_PATH,MEDIA_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}},LINUX_EFI_INITRD_MEDIA_GUID},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{END_DEVICE_PATH_LENGTH,0}}
};
STATIC BOOLEAN mExited;
STATIC CHAR16 mOptions[]=L"acpi=off rdinit=/init loglevel=7" T70_FIRMWARE_OPTIONS;
STATIC CHAR16 mDeviceOptions[]=L"acpi=off rdinit=/init loglevel=7 teclast.modules=usb,input,display,storage teclast.console=drm" T70_FIRMWARE_OPTIONS;
STATIC CHAR16 mDesktopOptions[]=L"acpi=off rdinit=/init loglevel=7 teclast.modules=usb,input,display,storage teclast.console=desktop-vulkan" T70_FIRMWARE_OPTIONS;
STATIC CHAR16 mPixmanOptions[]=L"acpi=off rdinit=/init loglevel=7 teclast.modules=usb,input,display,storage teclast.console=desktop-pixman" T70_FIRMWARE_OPTIONS;
STATIC CHAR16 mFullDeviceOptions[]=L"acpi=off rdinit=/init loglevel=7 teclast.modules=usb,input,display,storage,power,audio,sensors,camera,connectivity,cellular,sim_radio teclast.console=drm" T70_FIRMWARE_OPTIONS;
STATIC CONST UINT8 mKernelHash[32]=T70_KERNEL_SHA256;
STATIC CONST UINT8 mInitrdHash[32]=T70_INITRD_SHA256;
STATIC CONST UINT8 mModulesHash[32]=T70_MODULES_SHA256;

STATIC VOID EFIAPI ExitBoot(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;mExited=TRUE;
}
STATIC EFI_STATUS EFIAPI LoadInitrd(EFI_LOAD_FILE2_PROTOCOL *This,EFI_DEVICE_PATH_PROTOCOL *Path,
                                   BOOLEAN BootPolicy,UINTN *Size,VOID *Buffer) {
  UINTN Prefix,Padding,Total;
  if (This!=&mInitrd.Load || Path==NULL || Size==NULL) return EFI_INVALID_PARAMETER;
  if (BootPolicy) return EFI_UNSUPPORTED;
  if (!IsDevicePathEnd(Path)) return EFI_NOT_FOUND;
  if (!mInitrd.Part[0] || !mInitrd.Part[1]) return EFI_NOT_READY;
  /* Each initramfs archive header must start on a 4-byte boundary. The first
   * payload is gzip, whose byte length is not necessarily word aligned; pad
   * with zeroes before the raw newc module archive instead of concatenating
   * at the compressed stream end. */
  Padding=(4U-(mInitrd.Bytes[0]&3U))&3U;
  if (mInitrd.Bytes[0]>MAX_UINTN-Padding) return EFI_NOT_READY;
  Prefix=mInitrd.Bytes[0]+Padding;
  if (Prefix>MAX_UINTN-mInitrd.Bytes[1]) return EFI_NOT_READY;
  Total=Prefix+mInitrd.Bytes[1];
  if (Buffer==NULL || *Size<Total) { *Size=Total;return EFI_BUFFER_TOO_SMALL; }
  CopyMem(Buffer,mInitrd.Part[0],mInitrd.Bytes[0]);
  if (Padding) ZeroMem((UINT8 *)Buffer+mInitrd.Bytes[0],Padding);
  CopyMem((UINT8 *)Buffer+Prefix,mInitrd.Part[1],mInitrd.Bytes[1]);
  *Size=Total;return EFI_SUCCESS;
}
STATIC EFI_STATUS RuntimeTree(VOID **Original,UINTN *Size) {
  VOID *Tree=NULL;UINT64 Bytes;EFI_STATUS Status;
  EFI_MEMORY_REGION_DESCRIPTOR Regions[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];UINT8 Count=0;
  if (MmioRead64(HANDOFF_BASE)!=HANDOFF_MAGIC || MmioRead64(HANDOFF_BASE+0x58)!=EFI_SUCCESS)
    return EFI_NOT_READY;
  Status=EfiGetSystemConfigurationTable(&gFdtTableGuid,&Tree);
  if (EFI_ERROR(Status) || Tree==NULL || (UINTN)Tree!=MmioRead64(HANDOFF_BASE+0x48))
    return EFI_NOT_READY;
  Bytes=MmioRead64(HANDOFF_BASE+0x50);
  if (Bytes<sizeof(FDT_HEADER) || Bytes>MAX_DTB_SIZE ||
      FdtTotalSize(Tree)!=Bytes || FdtCheckFull(Tree,(UINTN)Bytes)!=0) return EFI_COMPROMISED_DATA;
  // Reuse the SEC parser, including modem LK-buffer reservations, before Linux sees RAM.
  Status=BuildRuntimeMemoryMap(Tree,(UINTN)Bytes,Regions,&Count);
  if (!EFI_ERROR(Status)) { *Original=Tree;*Size=(UINTN)Bytes; }
  return Status;
}
STATIC EFI_STATUS ReadPayload(EFI_FILE_PROTOCOL *Root,CHAR16 *Path,UINTN Expected,
                               CONST UINT8 *Hash,VOID **Output) {
  EFI_FILE_PROTOCOL *File=NULL;EFI_FILE_INFO *Info=NULL;
  EFI_STATUS Status,CloseStatus;UINTN InfoSize=0,Bytes,Done=0;UINT8 Digest[32];VOID *Data=NULL;
  if (Root==NULL || Path==NULL || Hash==NULL || Output==NULL || Expected==0)
    return EFI_INVALID_PARAMETER;
  *Output=NULL;
  Status=Root->Open(Root,&File,Path,EFI_FILE_MODE_READ,0);
  if (EFI_ERROR(Status)) return Status;
  Status=File->GetInfo(File,&gEfiFileInfoGuid,&InfoSize,NULL);
  if (Status!=EFI_BUFFER_TOO_SMALL || InfoSize<SIZE_OF_EFI_FILE_INFO || InfoSize>4096) {
    Status=EFI_COMPROMISED_DATA;goto Done;
  }
  Info=AllocatePool(InfoSize);if (!Info) { Status=EFI_OUT_OF_RESOURCES;goto Done; }
  Status=File->GetInfo(File,&gEfiFileInfoGuid,&InfoSize,Info);
  if (EFI_ERROR(Status)) goto Done;
  if (InfoSize<SIZE_OF_EFI_FILE_INFO || Info->Size< SIZE_OF_EFI_FILE_INFO ||
      Info->Size>InfoSize || Info->FileSize!=Expected || (Info->Attribute&EFI_FILE_DIRECTORY)) {
    Status=EFI_COMPROMISED_DATA;goto Done;
  }
  Data=AllocatePool(Expected);if (!Data) { Status=EFI_OUT_OF_RESOURCES;goto Done; }
  while (Done<Expected) {
    Bytes=Expected-Done;Status=File->Read(File,&Bytes,(UINT8 *)Data+Done);
    if (EFI_ERROR(Status)) goto Done;
    if (Bytes==0 || Bytes>Expected-Done) { Status=EFI_DEVICE_ERROR;goto Done; }
    Done+=Bytes;
  }
  if (!Sha256HashAll(Data,Expected,Digest) || CompareMem(Digest,Hash,32)!=0) {
    Status=EFI_SECURITY_VIOLATION;goto Done;
  }
  Status=EFI_SUCCESS;
Done:
  CloseStatus=File->Close(File);
  if (!EFI_ERROR(Status) && EFI_ERROR(CloseStatus)) Status=CloseStatus;
  if (Info) FreePool(Info);
  if (EFI_ERROR(Status)) { if (Data) FreePool(Data); }
  else *Output=Data;
  return Status;
}
EFI_STATUS BootPreparedLinuxProfile(EFI_HANDLE ImageHandle,UINTN Profile) {
  CHAR16 *Options[]={mOptions,mDeviceOptions,mDesktopOptions,mPixmanOptions,mFullDeviceOptions};
  CONST UINT32 OptionBytes[]={sizeof(mOptions),sizeof(mDeviceOptions),sizeof(mDesktopOptions),sizeof(mPixmanOptions),sizeof(mFullDeviceOptions)};
  if (Profile>=sizeof(Options)/sizeof(Options[0])) return EFI_INVALID_PARAMETER;
  EFI_LOADED_IMAGE_PROTOCOL *Own=NULL,*Child=NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs=NULL;EFI_BLOCK_IO_PROTOCOL *Block=NULL;
  EFI_FILE_PROTOCOL *Root=NULL;EFI_HANDLE Kernel=NULL,InitrdHandle=NULL,Existing=NULL;
  EFI_DEVICE_PATH_PROTOCOL *KernelPath=NULL,*Probe=(EFI_DEVICE_PATH_PROTOCOL *)&mInitrdPath;
  EFI_STATUS Status,CleanupStatus;EFI_EVENT ExitEvent=NULL;VOID *Image=NULL;
  BOOLEAN Registered=FALSE,TreeRegistered=FALSE;VOID *OriginalTree=NULL,*LinuxTree=NULL;UINTN TreeBytes=0;
  ZeroMem(&mInitrd,sizeof(mInitrd));mExited=FALSE;mInitrd.Load.LoadFile=LoadInitrd;
  Status=RuntimeTree(&OriginalTree,&TreeBytes);if (EFI_ERROR(Status)) return Status;
  Status=gBS->HandleProtocol(ImageHandle,&gEfiLoadedImageProtocolGuid,(VOID **)&Own);
  if (EFI_ERROR(Status) || !Own) return EFI_NOT_FOUND;
  Status=gBS->HandleProtocol(Own->DeviceHandle,&gEfiBlockIoProtocolGuid,(VOID **)&Block);
  // The application never searches or mounts internal Android volumes.
  if (EFI_ERROR(Status) || !Block || !Block->Media || !Block->Media->MediaPresent ||
      !Block->Media->RemovableMedia) return EFI_ACCESS_DENIED;
  Status=gBS->HandleProtocol(Own->DeviceHandle,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs);
  if (EFI_ERROR(Status) || !Fs) return EFI_NOT_FOUND;
  Status=gBS->LocateDevicePath(&gEfiLoadFile2ProtocolGuid,&Probe,&Existing);
  if (!EFI_ERROR(Status)) return EFI_ALREADY_STARTED;
  if (Status!=EFI_NOT_FOUND) return Status;
  Status=Fs->OpenVolume(Fs,&Root);if (EFI_ERROR(Status)) return Status;
  Status=ReadPayload(Root,L"\\EFI\\Teclast\\Image",T70_KERNEL_BYTES,mKernelHash,&Image);
  if (EFI_ERROR(Status)) goto Done;
  Status=ReadPayload(Root,L"\\EFI\\Teclast\\early-userspace.cpio.gz",T70_INITRD_BYTES,mInitrdHash,&mInitrd.Part[0]);
  if (EFI_ERROR(Status)) goto Done;
  mInitrd.Bytes[0]=T70_INITRD_BYTES;
  Status=ReadPayload(Root,L"\\EFI\\Teclast\\linux-stock-module-payload.cpio",T70_MODULES_BYTES,mModulesHash,&mInitrd.Part[1]);
  if (EFI_ERROR(Status)) goto Done;
  mInitrd.Bytes[1]=T70_MODULES_BYTES;
  Status=Root->Close(Root);Root=NULL;if (EFI_ERROR(Status)) goto Done;
  if (((UINT8 *)Image)[0]!='M' || ((UINT8 *)Image)[1]!='Z' ||
      CompareMem((UINT8 *)Image+56,"ARM\x64",4)!=0) { Status=EFI_UNSUPPORTED;goto Done; }
  KernelPath=FileDevicePath(Own->DeviceHandle,L"\\EFI\\Teclast\\Image");
  if (!KernelPath) { Status=EFI_OUT_OF_RESOURCES;goto Done; }
  Status=gBS->LoadImage(FALSE,ImageHandle,KernelPath,Image,T70_KERNEL_BYTES,&Kernel);
  if (EFI_ERROR(Status)) {
    // SECURITY_VIOLATION uniquely returns a loaded but forbidden-to-start image.
    // Other failure outputs do not transfer image ownership.
    if (Status!=EFI_SECURITY_VIOLATION) Kernel=NULL;
    goto Done;
  }
  Status=gBS->HandleProtocol(Kernel,&gEfiLoadedImageProtocolGuid,(VOID **)&Child);
  if (EFI_ERROR(Status) || !Child) { Child=NULL;Status=EFI_NOT_FOUND;goto Done; }
  Child->LoadOptions=Options[Profile];
  Child->LoadOptionsSize=OptionBytes[Profile];
  Status=gBS->InstallMultipleProtocolInterfaces(&InitrdHandle,
       &gEfiDevicePathProtocolGuid,&mInitrdPath,&gEfiLoadFile2ProtocolGuid,&mInitrd.Load,NULL);
  if (EFI_ERROR(Status)) goto Done;
  Registered=TRUE;
  Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitBoot,NULL,
                           &gEfiEventExitBootServicesGuid,&ExitEvent);
  // A failed event creation does not transfer ownership of its output.
  if (EFI_ERROR(Status)) { ExitEvent=NULL; goto Done; }
  Status=gBS->SetWatchdogTimer(0,0,0,NULL);if (EFI_ERROR(Status)) goto Done;
  if (TreeBytes>T70_LID_TREE_MAX-T70_LID_TREE_SLACK) { Status=EFI_UNSUPPORTED;goto Done; }
  LinuxTree=AllocatePool(TreeBytes+T70_LID_TREE_SLACK);
  if (!LinuxTree) { Status=EFI_OUT_OF_RESOURCES;goto Done; }
  Status=T70PrepareLinuxLidTree(OriginalTree,TreeBytes,LinuxTree,TreeBytes+T70_LID_TREE_SLACK);
  if (EFI_ERROR(Status)) goto Done;
  Status=gBS->InstallConfigurationTable(&gFdtTableGuid,LinuxTree);
  if (EFI_ERROR(Status)) goto Done;
  TreeRegistered=TRUE;
  Status=gBS->StartImage(Kernel,NULL,NULL);
  if (mExited) CpuDeadLoop(); // A returned image after ExitBootServices cannot use firmware cleanup.
  // StartImage unloads returned applications (and images returning errors).
  // Never dereference the pre-start LoadedImage pointer after control returns.
  Child=NULL;
  CleanupStatus=gBS->HandleProtocol(Kernel,&gEfiLoadedImageProtocolGuid,(VOID **)&Child);
  if (EFI_ERROR(CleanupStatus)) {
    Child=NULL;
    if (CleanupStatus==EFI_INVALID_PARAMETER || CleanupStatus==EFI_UNSUPPORTED ||
        CleanupStatus==EFI_NOT_FOUND) Kernel=NULL;
  }
Done:
  if (TreeRegistered) {
    CleanupStatus=gBS->InstallConfigurationTable(&gFdtTableGuid,OriginalTree);
    if (EFI_ERROR(CleanupStatus)) {
      Print(L"Linux device tree is still published. Restart the device to recover.\r\n");
      CpuDeadLoop();
    }
  }
  if (LinuxTree) FreePool(LinuxTree);
  // Stop publishing initrd pointers before freeing their source buffers.
  if (Registered) {
    CleanupStatus=gBS->UninstallMultipleProtocolInterfaces(InitrdHandle,
        &gEfiDevicePathProtocolGuid,&mInitrdPath,&gEfiLoadFile2ProtocolGuid,&mInitrd.Load,NULL);
    if (EFI_ERROR(CleanupStatus)) {
      Print(L"Initrd interface is still in use. Restart the device to recover.\r\n");
      CpuDeadLoop(); // Keep application code and source buffers alive while callbacks are registered.
    }
  }
  if (ExitEvent) {
    CleanupStatus=gBS->CloseEvent(ExitEvent);
    if (EFI_ERROR(CleanupStatus)) {
      Print(L"Boot event is still active. Restart the device to recover.\r\n");
      CpuDeadLoop(); // The registered callback still points into this application.
    }
  }
  if (Kernel) {
    if (Child) { Child->LoadOptions=NULL;Child->LoadOptionsSize=0; }
    CleanupStatus=gBS->UnloadImage(Kernel);
    if (EFI_ERROR(CleanupStatus)) Status=CleanupStatus;
  }
  if (Root) Root->Close(Root);
  if (KernelPath) FreePool(KernelPath);
  if (Image) FreePool(Image);
  for (UINTN I=0;I<2;I++) { if (mInitrd.Part[I]) FreePool(mInitrd.Part[I]);mInitrd.Part[I]=NULL; }
  return Status;
}
EFI_STATUS BootPreparedLinux(EFI_HANDLE ImageHandle) {
  return BootPreparedLinuxProfile(ImageHandle,FALSE);
}
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;UINTN Index;EFI_INPUT_KEY Key;
  (VOID)SystemTable;
  if (!gST->ConIn) return EFI_NOT_READY;
  Print(L"T70Pro\r\nL: Linux diagnostic shell\r\nD: Linux diagnostic console\r\nF: Linux all devices\r\nG: Linux Vulkan desktop\r\nP: Linux CPU desktop\r\nA: Restart\r\nEsc: Return\r\n");
  for (;;) {
    Status=gBS->WaitForEvent(1,&gST->ConIn->WaitForKey,&Index);if (EFI_ERROR(Status)) return Status;
    Status=gST->ConIn->ReadKeyStroke(gST->ConIn,&Key);if (Status==EFI_NOT_READY) continue;
    if (EFI_ERROR(Status)) return Status;
    if (Key.ScanCode==SCAN_ESC) return EFI_ABORTED;
    if (Key.UnicodeChar==L'a' || Key.UnicodeChar==L'A') {
      gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);return EFI_DEVICE_ERROR;
    }
    if (Key.UnicodeChar==L'l' || Key.UnicodeChar==L'L' || Key.UnicodeChar==L'd' || Key.UnicodeChar==L'D' ||
        Key.UnicodeChar==L'g' || Key.UnicodeChar==L'G' || Key.UnicodeChar==L'p' || Key.UnicodeChar==L'P' ||
        Key.UnicodeChar==L'f' || Key.UnicodeChar==L'F') {
      UINTN Profile=0;
      if (Key.UnicodeChar==L'd' || Key.UnicodeChar==L'D') Profile=1;
      if (Key.UnicodeChar==L'g' || Key.UnicodeChar==L'G') Profile=2;
      if (Key.UnicodeChar==L'p' || Key.UnicodeChar==L'P') Profile=3;
      if (Key.UnicodeChar==L'f' || Key.UnicodeChar==L'F') Profile=4;
      Status=BootPreparedLinuxProfile(ImageHandle,Profile);Print(L"Linux returned: %r\r\n",Status);return Status;
    }
  }
}
