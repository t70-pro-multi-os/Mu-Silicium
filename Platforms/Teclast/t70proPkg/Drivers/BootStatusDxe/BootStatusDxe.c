// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/Fdt.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define HANDOFF_BASE   0x43F7F000
#define HANDOFF_MAGIC  0x0000000148303754ULL
#define DTB_BASE       0x47C80000
#define DTB_END        0x48080000
#define MAX_DTB_SIZE   0x200000

STATIC
EFI_STATUS
PublishBootDtb (VOID)
{
  UINT64                Address;
  UINT64                Size;
  CONST VOID            *Original;
  EFI_PHYSICAL_ADDRESS  Copy;
  UINTN                 Pages;
  EFI_STATUS            Status;

  Address = MmioRead64 (HANDOFF_BASE + 8);
  Size = MmioRead64 (HANDOFF_BASE + 16);
  // Check the recorded range before any access through the recorded pointer.
  if ((Address < DTB_BASE) || (Address >= DTB_END) || ((Address & 7) != 0) ||
      (Size < sizeof (FDT_HEADER)) || (Size > MAX_DTB_SIZE) ||
      (Size > DTB_END - Address)) {
    return EFI_COMPROMISED_DATA;
  }
  Original = (CONST VOID *)(UINTN)Address;
  if ((FdtTotalSize (Original) != Size) ||
      (FdtCheckFull (Original, (UINTN)Size) != 0)) {
    return EFI_COMPROMISED_DATA;
  }

  // Keep every runtime memory reservation, phandle, chosen property and vendor
  // modem property intact. An offline DTB is not a safe runtime substitute.
  Pages = EFI_SIZE_TO_PAGES ((UINTN)Size);
  Copy = 0;
  Status = gBS->AllocatePages (AllocateAnyPages, EfiACPIReclaimMemory, Pages, &Copy);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  ZeroMem ((VOID *)(UINTN)Copy, EFI_PAGES_TO_SIZE (Pages));
  CopyMem ((VOID *)(UINTN)Copy, Original, (UINTN)Size);
  // InstallConfigurationTable can invoke GOP/DSI notifications before it
  // returns. Publish the validated copy's fields before that notification.
  MmioWrite64 (HANDOFF_BASE + 0x48, Copy);
  MmioWrite64 (HANDOFF_BASE + 0x50, Size);
  MmioWrite64 (HANDOFF_BASE + 0x58, EFI_SUCCESS);
  MemoryFence ();
  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, (VOID *)(UINTN)Copy);
  if (EFI_ERROR (Status)) {
    MmioWrite64 (HANDOFF_BASE + 0x58, Status);
    MemoryFence ();
    MmioWrite64 (HANDOFF_BASE + 0x48, 0);
    MmioWrite64 (HANDOFF_BASE + 0x50, 0);
    gBS->FreePages (Copy, Pages);
    return Status;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
BootStatusEntry (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;

  // This driver records DXE dispatch; it does not imply that an OS booted.
  if (MmioRead64 (HANDOFF_BASE) != HANDOFF_MAGIC) {
    return EFI_NOT_FOUND;
  }
  MmioWrite64 (HANDOFF_BASE + 0x40, (UINTN)SystemTable);
  MmioWrite64 (HANDOFF_BASE + 0x48, 0);
  MmioWrite64 (HANDOFF_BASE + 0x50, 0);
  MmioWrite64 (HANDOFF_BASE + 0x58, EFI_NOT_READY);
  Status = PublishBootDtb ();
  MmioWrite64 (HANDOFF_BASE + 0x58, Status);
  MmioWrite64 (HANDOFF_BASE + 0x38, 4);
  WriteBackDataCacheRange ((VOID *)(UINTN)HANDOFF_BASE, 0x1000);
  return Status;
}
