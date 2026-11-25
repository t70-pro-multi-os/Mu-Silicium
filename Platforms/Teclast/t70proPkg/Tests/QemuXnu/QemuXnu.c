// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline VM fixture; refuses the physical tablet's firmware tree.
#include <Uefi.h>
#include <Guid/Fdt.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/UniXNULoader.h>
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System) {
  VOID *Tree=NULL;INT32 Length;CONST VOID *Compatible;
  EFI_LOADED_IMAGE_PROTOCOL *Own;EFI_DEVICE_PATH_PROTOCOL *Path;EFI_HANDLE Driver=NULL;
  UNIXNU_LOADER_PROTOCOL *Loader=NULL;UNIXNU_LOADER_BOOT_REQUEST Request={0};EFI_STATUS Status;
  (VOID)System;
  Status=EfiGetSystemConfigurationTable(&gFdtTableGuid,&Tree);
  if (EFI_ERROR(Status) || !Tree || FdtCheckHeader(Tree)) return EFI_UNSUPPORTED;
  Compatible=FdtGetProp(Tree,0,"compatible",&Length);
  if (!Compatible || FdtStringListContains(Compatible,Length,"linux,dummy-virt")!=1) return EFI_UNSUPPORTED;
  Status=gBS->HandleProtocol(Image,&gEfiLoadedImageProtocolGuid,(VOID **)&Own);
  if (EFI_ERROR(Status)) return Status;
  Path=FileDevicePath(Own->DeviceHandle,L"\\EFI\\Teclast\\DarwinBoot.efi");if(!Path)return EFI_OUT_OF_RESOURCES;
  Status=gBS->LoadImage(FALSE,Image,Path,NULL,0,&Driver);FreePool(Path);if(EFI_ERROR(Status))return Status;
  Status=gBS->StartImage(Driver,NULL,NULL);if(EFI_ERROR(Status))return Status;
  Status=gBS->HandleProtocol(Driver,&gUniXNULoaderProtocolGuid,(VOID **)&Loader);
  if(EFI_ERROR(Status) || !Loader || Loader->Revision!=UNIXNU_LOADER_REVISION || !Loader->Boot)return EFI_UNSUPPORTED;
  Request.Volume=Own->DeviceHandle;Request.KernelPath=L"\\xnu\\kernel.macho";
  Request.BootArgs=L"-v keepsyms=1 debug=0x14e";
  Print(L"T70-QEMU: real T70PRO kernel through DarwinBoot\r\n");
  Status=gBS->SetWatchdogTimer(0,0,0,NULL);if(EFI_ERROR(Status))return Status;
  Status=Loader->Boot(Loader,&Request);
  Print(L"T70-QEMU: loader returned %r\r\n",Status);CpuDeadLoop();return EFI_ABORTED;
}
