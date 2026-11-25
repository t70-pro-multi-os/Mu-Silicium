// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit early-XNU launch from this application's removable volume.
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Library/BaseLib.h>
#include <Library/ArmLib.h>
#include <Library/ArmGenericTimerCounterLib.h>
#include "XnuPlatform.h"
#include <Library/DevicePathLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/UniXNULoader.h>

#define HANDOFF_BASE 0x43F7F000U
#define HANDOFF_MAGIC 0x0000000148303754ULL
#define MAX_DTB_SIZE 0x200000U
extern EFI_STATUS BuildRuntimeMemoryMap(CONST VOID *,UINTN,EFI_MEMORY_REGION_DESCRIPTOR *,UINT8 *);

STATIC BOOLEAN mExited;
STATIC CHAR16 mDriverPath[]=L"\\EFI\\Teclast\\DarwinBoot.efi";
STATIC CONST CHAR16 mKernelPath[]=L"\\xnu\\kernel.macho";
STATIC CONST CHAR16 mBootArgs[]=L"-v keepsyms=1 debug=0x14e";

STATIC VOID EFIAPI ExitBoot(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;mExited=TRUE;
}
STATIC EFI_STATUS ValidateRuntimeTree(VOID) {
  VOID *Tree=NULL;UINT64 Bytes;EFI_STATUS Status;EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop=NULL;
  EFI_MEMORY_REGION_DESCRIPTOR Regions[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT];UINT8 Count=0;
  if (MmioRead64(HANDOFF_BASE)!=HANDOFF_MAGIC || MmioRead64(HANDOFF_BASE+0x58)!=EFI_SUCCESS)
    return EFI_NOT_READY;
  Status=EfiGetSystemConfigurationTable(&gFdtTableGuid,&Tree);
  if (EFI_ERROR(Status) || !Tree || (UINTN)Tree!=MmioRead64(HANDOFF_BASE+0x48))
    return EFI_NOT_READY;
  Bytes=MmioRead64(HANDOFF_BASE+0x50);
  if (Bytes<sizeof(FDT_HEADER) || Bytes>MAX_DTB_SIZE ||
      FdtTotalSize(Tree)!=Bytes || FdtCheckFull(Tree,(UINTN)Bytes)!=0)
    return EFI_COMPROMISED_DATA;
  // SEC's parser includes secure/modem reservations; an offline donor FDT is insufficient.
  Status=BuildRuntimeMemoryMap(Tree,(UINTN)Bytes,Regions,&Count);
  if (EFI_ERROR(Status)) return Status;
  Status=gBS->HandleProtocol(gST->ConsoleOutHandle,&gEfiGraphicsOutputProtocolGuid,(VOID **)&Gop);
  if (EFI_ERROR(Status)) Status=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);
  if (EFI_ERROR(Status)) return Status;
  return T70XnuValidatePlatform(Tree,(UINTN)Bytes,Regions,Count,Gop,ArmReadMpidr(),
                                ArmGenericTimerGetTimerFreq());
}

EFI_STATUS BootXnu(EFI_HANDLE ImageHandle) {
  EFI_LOADED_IMAGE_PROTOCOL *Own=NULL;
  EFI_BLOCK_IO_PROTOCOL *Block=NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs=NULL;
  UNIXNU_LOADER_PROTOCOL *Loader=NULL;
  EFI_DEVICE_PATH_PROTOCOL *Path=NULL;
  EFI_HANDLE Driver=NULL;
  EFI_EVENT Event=NULL;
  VOID *Existing=NULL;
  EFI_STATUS Status,Cleanup;
  UNIXNU_LOADER_BOOT_REQUEST Request={0};

  mExited=FALSE;
  Status=ValidateRuntimeTree();if (EFI_ERROR(Status)) return Status;
  Status=gBS->HandleProtocol(ImageHandle,&gEfiLoadedImageProtocolGuid,(VOID **)&Own);
  if (EFI_ERROR(Status) || !Own || !Own->DeviceHandle) return EFI_NOT_FOUND;
  Status=gBS->HandleProtocol(Own->DeviceHandle,&gEfiBlockIoProtocolGuid,(VOID **)&Block);
  if (EFI_ERROR(Status) || !Block || !Block->Media || !Block->Media->MediaPresent ||
      !Block->Media->RemovableMedia) return EFI_ACCESS_DENIED;
  Status=gBS->HandleProtocol(Own->DeviceHandle,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs);
  if (EFI_ERROR(Status) || !Fs) return EFI_NOT_FOUND;
  // Use only the driver we load here, never an unrelated installed loader.
  Status=gBS->LocateProtocol(&gUniXNULoaderProtocolGuid,NULL,&Existing);
  if (!EFI_ERROR(Status)) return EFI_ALREADY_STARTED;
  if (Status!=EFI_NOT_FOUND) return Status;
  Path=FileDevicePath(Own->DeviceHandle,mDriverPath);
  if (!Path) return EFI_OUT_OF_RESOURCES;
  Status=gBS->LoadImage(FALSE,ImageHandle,Path,NULL,0,&Driver);
  if (EFI_ERROR(Status)) goto Done;
  Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitBoot,NULL,
                           &gEfiEventExitBootServicesGuid,&Event);
  if (EFI_ERROR(Status)) goto Done;
  Status=gBS->StartImage(Driver,NULL,NULL);
  if (mExited) CpuDeadLoop();
  // UEFI unloads an unsuccessful driver during StartImage.
  if (EFI_ERROR(Status)) { Driver=NULL;goto Done; }
  Status=gBS->HandleProtocol(Driver,&gUniXNULoaderProtocolGuid,(VOID **)&Loader);
  if (EFI_ERROR(Status)) goto Done;
  if (!Loader || Loader->Revision!=UNIXNU_LOADER_REVISION || !Loader->Boot) {
    Status=EFI_UNSUPPORTED;goto Done;
  }
  Request.Volume=Own->DeviceHandle;
  Request.KernelPath=mKernelPath;
  Request.BootArgs=mBootArgs;
  // DarwinBoot reads the staged ADT and allocates its entire RAM window from free UEFI memory.
  // The first milestone has no Data ramdisk, kext injection or root-volume setup.
  Status=gBS->SetWatchdogTimer(0,0,0,NULL);if (EFI_ERROR(Status)) goto Done;
  Status=Loader->Boot(Loader,&Request);
  if (mExited) CpuDeadLoop(); // Firmware calls are invalid after ExitBootServices.
  if (!EFI_ERROR(Status)) Status=EFI_ABORTED; // A boot operation must not return success.
Done:
  if (Event) {
    Cleanup=gBS->CloseEvent(Event);
    if (EFI_ERROR(Cleanup)) CpuDeadLoop(); // Keep the callback code resident.
  }
  if (Driver) {
    // Includes a live image returned by LoadImage with EFI_SECURITY_VIOLATION.
    Cleanup=gBS->UnloadImage(Driver);
    if (EFI_ERROR(Cleanup)) Status=Cleanup;
  }
  if (Path) FreePool(Path);
  return Status;
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;UINTN Index;EFI_INPUT_KEY Key;
  (VOID)SystemTable;
  if (!gST->ConIn) return EFI_NOT_READY;
  Print(L"T70Pro XNU bring-up\r\nX: Start prepared kernel\r\nEsc: Return\r\n");
  for (;;) {
    Status=gBS->WaitForEvent(1,&gST->ConIn->WaitForKey,&Index);if (EFI_ERROR(Status)) return Status;
    Status=gST->ConIn->ReadKeyStroke(gST->ConIn,&Key);if (Status==EFI_NOT_READY) continue;
    if (EFI_ERROR(Status)) return Status;
    if (Key.ScanCode==SCAN_ESC) return EFI_ABORTED;
    if (Key.UnicodeChar==L'x' || Key.UnicodeChar==L'X') {
      Status=BootXnu(ImageHandle);
      Print(L"T70Pro XNU returned: %r\r\n",Status);
      return Status;
    }
  }
}
