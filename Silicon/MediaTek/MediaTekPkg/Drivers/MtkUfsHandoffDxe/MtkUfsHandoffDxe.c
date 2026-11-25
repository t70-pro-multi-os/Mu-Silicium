/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Pi/PiDxeCis.h>
#include <Guid/Fdt.h>
#include <Guid/EventGroup.h>

#include <IndustryStandard/UfsHci.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/IoLib.h>
#include <Library/FdtLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsDeviceConfig.h>
#include <Protocol/UfsHostControllerPlatform.h>
#include <Library/MtkUfsCore.h>
#include <Library/MtkUfsTransport.h>
#include <Library/TimerLib.h>
#include "MtkUfsColdHost.h"

#define MTK_UFS_CONTEXT_SIGNATURE       SIGNATURE_32('M', 'U', 'F', 'S')
#define MTK_UFS_MAPPING_SIGNATURE       SIGNATURE_32('M', 'U', 'M', 'P')
#define MTK_UFS_MMIO_BASE              0x11270000ULL
#define MTK_UFS_MMIO_LENGTH            0x2300ULL
#define MTK_UFS_DMA_LIMIT_64           0x1000000000ULL
#define MTK_UFS_DMA_LIMIT_32           0x100000000ULL
#define MTK_UFS_BOOT_HANDOFF_BASE      0x43F7F000ULL
#define MTK_UFS_BOOT_HANDOFF_MAGIC     0x0000000148303754ULL
#define MTK_UFS_MAX_DTB_SIZE           0x200000ULL

STATIC EFI_GUID mMtkUfsSeedGuid = {
  0x5c568b4c, 0x8d91, 0x4e83, { 0x9d, 0x21, 0x6a, 0x30, 0x17, 0xa4, 0x72, 0x05 }
};

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} MTK_UFS_SEED_DEVICE_PATH;
#pragma pack()

typedef struct {
  LIST_ENTRY                    Link;
  VOID                          *HostAddress;
  UINTN                         Pages;
  UINT64                        OriginalAttributes;
  UINT32                        ActiveMappings;
  BOOLEAN                       Exposed;
  BOOLEAN                       FreePending;
  BOOLEAN                       FreeInProgress;
  BOOLEAN                       RestoreAttributesPending;
} MTK_UFS_COMMON_ALLOCATION;

typedef struct {
  UINT32                                Signature;
  LIST_ENTRY                            Link;
  VOID                                  *HostAddress;
  UINTN                                 NumberOfBytes;
  EDKII_UFS_HOST_CONTROLLER_OPERATION   Operation;
  MTK_UFS_COMMON_ALLOCATION             *Allocation;
  EFI_PHYSICAL_ADDRESS                  BounceAddress;
  UINTN                                 BouncePages;
  BOOLEAN                               BounceCopied;
  BOOLEAN                               Unmapping;
} MTK_UFS_MAPPING;

typedef struct {
  UINT32                               Signature;
  EFI_HANDLE                           SeedHandle;
  EFI_HANDLE                           ControllerHandle;
  BOOLEAN                              Starting;
  BOOLEAN                              StartBusy;
  BOOLEAN                              StopBusy;
  BOOLEAN                              Started;
  BOOLEAN                              CallbackClosing;
  BOOLEAN                              HostPublished;
  BOOLEAN                              PlatformPublished;
  UINT32                               ActiveCallbacks;
  UINT32                               EventStartCallbacks;
  BOOLEAN                              ColdStart;
  BOOLEAN                              ExitedBootServices;
  BOOLEAN                              DmaStopping;
  BOOLEAN                              DmaCleanupActive;
  UINT32                               ActiveDmaOperations;
  EFI_STATUS                           QuiesceStatus;
  MTK_UFS_COLD_HOST                    ColdHost;
  BOOLEAN                              Supports64BitDma;
  UINT32                               Capabilities;
  LIST_ENTRY                           CommonAllocations;
  LIST_ENTRY                           Mappings;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL   HostController;
  EDKII_UFS_HC_PLATFORM_PROTOCOL       Platform;
} MTK_UFS_CONTEXT;

STATIC MTK_UFS_CONTEXT mContext;
STATIC MTK_UFS_SEED_DEVICE_PATH mSeedDevicePath;
STATIC EFI_DRIVER_BINDING_PROTOCOL mDriverBinding;
STATIC EFI_EVENT mFdtEvent;
STATIC EFI_EVENT mEndOfDxeEvent;
STATIC EFI_EVENT mReadyToBootEvent;
STATIC EFI_EVENT mExitBootServicesEvent;
STATIC BOOLEAN mEntryQuarantined;
STATIC VOID MtkUfsPublishOsReady (BOOLEAN Ready);

STATIC
MTK_UFS_CONTEXT *
MtkUfsContextFromHc (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  )
{
  return BASE_CR (This, MTK_UFS_CONTEXT, HostController);
}

STATIC
UINT64
MtkUfsDmaLimit (
  IN MTK_UFS_CONTEXT  *Context
  )
{
  return Context->Supports64BitDma ? MTK_UFS_DMA_LIMIT_64 : MTK_UFS_DMA_LIMIT_32;
}

STATIC
EFI_STATUS
MtkUfsBeginProviderCallback (
  IN MTK_UFS_CONTEXT  *Context
  )
{
  EFI_TPL     OldTpl;
  EFI_STATUS  Status;

  if (Context == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Context->ExitedBootServices) {
    Status = EFI_UNSUPPORTED;
  } else if (Context->CallbackClosing || Context->DmaCleanupActive ||
             (!Context->Starting && !Context->Started))
  {
    Status = EFI_NOT_READY;
  } else if (Context->ActiveCallbacks == MAX_UINT32) {
    Status = EFI_OUT_OF_RESOURCES;
  } else {
    Context->ActiveCallbacks++;
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices && !EFI_ERROR (Status)) {
    /* RestoreTPL may dispatch ExitBootServices at the caller's TPL. The EBS
     * callback has already observed this pin and declined hardware quiesce. */
    Context->ActiveCallbacks--;
    return EFI_UNSUPPORTED;
  }

  return Status;
}

STATIC
BOOLEAN
MtkUfsCanReopenClosedProvider (
  IN MTK_UFS_CONTEXT  *Context
  )
{
  UINTN  Index;

  if ((Context == NULL) || mEntryQuarantined || Context->ExitedBootServices ||
      !Context->CallbackClosing || !Context->DmaStopping ||
      Context->Started || Context->Starting || Context->StartBusy ||
      Context->StopBusy || Context->HostPublished || Context->PlatformPublished ||
      (Context->ControllerHandle != NULL) || (Context->ActiveCallbacks != 0) ||
      (Context->EventStartCallbacks != 0) || (Context->ActiveDmaOperations != 0) ||
      Context->DmaCleanupActive ||
      (Context->ColdHost.StopPhase != MtkUfsColdStopNone &&
       Context->ColdHost.StopPhase != MtkUfsColdStopReleasedNeedsInit) ||
      Context->ColdHost.CleanupPending || Context->ColdHost.Suspended ||
      !IsListEmpty (&Context->Mappings) || !IsListEmpty (&Context->CommonAllocations))
  {
    return FALSE;
  }

  for (Index = 0; Index < ARRAY_SIZE (Context->ColdHost.ClockOwned); Index++) {
    if (Context->ColdHost.ClockOwned[Index]) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
EFI_STATUS
MtkUfsBeginEventOperation (
  IN MTK_UFS_CONTEXT  *Context
  )
{
  EFI_TPL     OldTpl;
  EFI_STATUS  Status;

  if (Context == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Context->ExitedBootServices) {
    Status = EFI_UNSUPPORTED;
  } else if (Context->DmaCleanupActive) {
    Status = EFI_NOT_READY;
  } else if (Context->CallbackClosing) {
    if (!MtkUfsCanReopenClosedProvider (Context)) {
      Status = EFI_NOT_READY;
    } else if (Context->ActiveCallbacks == MAX_UINT32) {
      Status = EFI_OUT_OF_RESOURCES;
    } else {
      Context->CallbackClosing = FALSE;
      Context->DmaStopping = FALSE;
      Context->ActiveCallbacks++;
      Status = EFI_SUCCESS;
    }
  } else if (Context->DmaStopping) {
    Status = EFI_NOT_READY;
  } else if (Context->ActiveCallbacks == MAX_UINT32) {
    Status = EFI_OUT_OF_RESOURCES;
  } else {
    Context->ActiveCallbacks++;
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices && !EFI_ERROR (Status)) {
    Context->ActiveCallbacks--;
    return EFI_UNSUPPORTED;
  }

  return Status;
}

STATIC
EFI_STATUS
MtkUfsBeginEventStart (
  IN MTK_UFS_CONTEXT  *Context
  )
{
  EFI_TPL     OldTpl;
  EFI_STATUS  Status;

  if (Context == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Context->ExitedBootServices) {
    Status = EFI_UNSUPPORTED;
  } else if (Context->CallbackClosing || Context->DmaCleanupActive ||
             (Context->EventStartCallbacks >= Context->ActiveCallbacks))
  {
    Status = EFI_NOT_READY;
  } else if (Context->EventStartCallbacks == MAX_UINT32) {
    Status = EFI_OUT_OF_RESOURCES;
  } else {
    Context->EventStartCallbacks++;
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices && !EFI_ERROR (Status)) {
    Context->EventStartCallbacks--;
    return EFI_UNSUPPORTED;
  }

  return Status;
}

STATIC
VOID
MtkUfsEndEventStart (
  IN MTK_UFS_CONTEXT  *Context
  )
{
  EFI_TPL  OldTpl;

  if (Context == NULL) {
    return;
  }

  if (Context->ExitedBootServices) {
    if (Context->EventStartCallbacks != 0) {
      Context->EventStartCallbacks--;
    }

    return;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Context->EventStartCallbacks != 0) {
    Context->EventStartCallbacks--;
  }

  gBS->RestoreTPL (OldTpl);
}

STATIC
VOID
MtkUfsEndProviderCallback (
  IN MTK_UFS_CONTEXT  *Context
  )
{
  EFI_TPL  OldTpl;

  if (Context == NULL) {
    return;
  }

  if (Context->ExitedBootServices) {
    if (Context->ActiveCallbacks != 0) {
      Context->ActiveCallbacks--;
    }

    return;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Context->ActiveCallbacks != 0) {
    Context->ActiveCallbacks--;
  }

  gBS->RestoreTPL (OldTpl);
}

STATIC
EFI_STATUS
MtkUfsBeginDmaOperation (
  IN MTK_UFS_CONTEXT  *Context,
  IN BOOLEAN          AllowStopping
  )
{
  EFI_TPL     OldTpl;
  EFI_STATUS  Status;

  if (Context == NULL) {
    return EFI_UNSUPPORTED;
  }

  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Context->ExitedBootServices) {
    Status = EFI_UNSUPPORTED;
  } else if (Context->CallbackClosing ||
             (!Context->Starting && !Context->Started) ||
             Context->DmaCleanupActive ||
             (Context->DmaStopping && !AllowStopping))
  {
    Status = EFI_NOT_READY;
  } else if ((Context->ActiveDmaOperations == MAX_UINT32) ||
             (Context->ActiveCallbacks == MAX_UINT32))
  {
    Status = EFI_OUT_OF_RESOURCES;
  } else {
    Context->ActiveCallbacks++;
    Context->ActiveDmaOperations++;
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices && !EFI_ERROR (Status)) {
    Context->ActiveCallbacks--;
    Context->ActiveDmaOperations--;
    return EFI_UNSUPPORTED;
  }

  return Status;
}

STATIC
VOID
MtkUfsEndDmaOperation (
  IN MTK_UFS_CONTEXT  *Context
  )
{
  EFI_TPL  OldTpl;

  if (Context == NULL) {
    return;
  }

  if (Context->ExitedBootServices) {
    if (Context->ActiveDmaOperations != 0) {
      Context->ActiveDmaOperations--;
    }
    if (Context->ActiveCallbacks != 0) {
      Context->ActiveCallbacks--;
    }

    return;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Context->ActiveDmaOperations != 0) {
    Context->ActiveDmaOperations--;
  }
  if (Context->ActiveCallbacks != 0) {
    Context->ActiveCallbacks--;
  }

  gBS->RestoreTPL (OldTpl);
}

STATIC
EFI_STATUS
MtkUfsBeginDmaCleanup (
  IN MTK_UFS_CONTEXT  *Context,
  IN BOOLEAN          AllowEventStartCallback
  )
{
  EFI_TPL     OldTpl;
  EFI_STATUS  Status;
  BOOLEAN     WasCallbackClosing;
  BOOLEAN     EventStartCallbackOnly;

  if ((Context == NULL) || Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Context->ExitedBootServices) {
    Status = EFI_UNSUPPORTED;
  } else if (Context->DmaCleanupActive) {
    /* A reentrant Stop must not clear the first caller's allocation/map gate. */
    Status = EFI_NOT_READY;
  } else {
    WasCallbackClosing = Context->CallbackClosing;
    Context->CallbackClosing = TRUE;
    Context->DmaStopping = TRUE;
    EventStartCallbackOnly = (BOOLEAN)(AllowEventStartCallback &&
        (Context->EventStartCallbacks != 0) &&
        (Context->ActiveCallbacks == Context->EventStartCallbacks));
    if (((Context->ActiveCallbacks != 0) && !EventStartCallbackOnly) ||
        (Context->ActiveDmaOperations != 0))
    {
      Context->CallbackClosing = WasCallbackClosing;
      Context->DmaStopping = WasCallbackClosing;
      Status = EFI_NOT_READY;
    } else {
      Context->DmaCleanupActive = TRUE;
      Status = EFI_SUCCESS;
    }
  }

  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices && !EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }

  return Status;
}

STATIC
VOID
MtkUfsEndDmaCleanup (
  IN MTK_UFS_CONTEXT  *Context,
  IN BOOLEAN          KeepStopped,
  IN BOOLEAN          KeepCallbacksClosed
  )
{
  EFI_TPL  OldTpl;

  if ((Context == NULL) || Context->ExitedBootServices) {
    return;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Context->DmaCleanupActive = FALSE;
  Context->DmaStopping      = KeepStopped;
  Context->CallbackClosing = KeepCallbacksClosed;
  gBS->RestoreTPL (OldTpl);
}

STATIC
VOID
MtkUfsEndDriverStop (
  IN MTK_UFS_CONTEXT  *Context
  )
{
  EFI_TPL  OldTpl;

  if (Context == NULL) {
    return;
  }

  if (Context->ExitedBootServices) {
    Context->StopBusy = FALSE;
    return;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Context->StopBusy = FALSE;
  gBS->RestoreTPL (OldTpl);
}

STATIC
BOOLEAN
MtkUfsAllocationIsLinked (
  IN MTK_UFS_CONTEXT            *Context,
  IN MTK_UFS_COMMON_ALLOCATION  *Allocation
  )
{
  LIST_ENTRY                 *Link;
  MTK_UFS_COMMON_ALLOCATION  *Candidate;

  for (Link = GetFirstNode (&Context->CommonAllocations);
       !IsNull (&Context->CommonAllocations, Link);
       Link = GetNextNode (&Context->CommonAllocations, Link))
  {
    Candidate = BASE_CR (Link, MTK_UFS_COMMON_ALLOCATION, Link);
    if (Candidate == Allocation) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
MTK_UFS_COMMON_ALLOCATION *
MtkUfsFindCommonAllocationRange (
  IN  MTK_UFS_CONTEXT  *Context,
  IN  VOID             *HostAddress,
  IN  UINTN            NumberOfBytes,
  OUT BOOLEAN          *PartialOverlap
  )
{
  LIST_ENTRY                 *Link;
  MTK_UFS_COMMON_ALLOCATION  *Allocation;
  UINT64                     Start;
  UINT64                     End;
  UINT64                     AllocationStart;
  UINT64                     AllocationEnd;

  *PartialOverlap = FALSE;
  Start = (UINT64)(UINTN)HostAddress;
  End   = Start + (UINT64)NumberOfBytes;
  for (Link = GetFirstNode (&Context->CommonAllocations);
       !IsNull (&Context->CommonAllocations, Link);
       Link = GetNextNode (&Context->CommonAllocations, Link))
  {
    Allocation = BASE_CR (Link, MTK_UFS_COMMON_ALLOCATION, Link);
    AllocationStart = (UINT64)(UINTN)Allocation->HostAddress;
    AllocationEnd   = AllocationStart + EFI_PAGES_TO_SIZE (Allocation->Pages);
    if ((Start < AllocationEnd) && (AllocationStart < End)) {
      if ((Start >= AllocationStart) && (End <= AllocationEnd)) {
        return Allocation;
      }

      *PartialOverlap = TRUE;
      return NULL;
    }
  }

  return NULL;
}

STATIC
EFI_STATUS
MtkUfsReleaseCommonAllocation (
  IN MTK_UFS_CONTEXT            *Context,
  IN MTK_UFS_COMMON_ALLOCATION  *Allocation
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;
  UINT64      Length;
  UINT64      Address;

  if ((Context == NULL) || (Allocation == NULL) || Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (!MtkUfsAllocationIsLinked (Context, Allocation) ||
      !Allocation->FreePending || !Allocation->FreeInProgress)
  {
    gBS->RestoreTPL (OldTpl);
    return EFI_INVALID_PARAMETER;
  }

  if (Allocation->ActiveMappings != 0) {
    Allocation->FreeInProgress = FALSE;
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }

  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  Length  = EFI_PAGES_TO_SIZE (Allocation->Pages);
  Address = (UINT64)(UINTN)Allocation->HostAddress;
  if (Allocation->RestoreAttributesPending) {
    Status = gDS->SetMemorySpaceAttributes (
                    Address,
                    Length,
                    Allocation->OriginalAttributes
                    );
    if (Context->ExitedBootServices) {
      return EFI_UNSUPPORTED;
    }
    if (EFI_ERROR (Status)) {
      goto ReleaseFailed;
    }

    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    Allocation->RestoreAttributesPending = FALSE;
    gBS->RestoreTPL (OldTpl);
  }

  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  Status = gBS->FreePages (Address, Allocation->Pages);
  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    goto ReleaseFailed;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (!MtkUfsAllocationIsLinked (Context, Allocation)) {
    gBS->RestoreTPL (OldTpl);
    return EFI_COMPROMISED_DATA;
  }

  RemoveEntryList (&Allocation->Link);
  InitializeListHead (&Allocation->Link);
  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }
  FreePool (Allocation);
  return EFI_SUCCESS;

ReleaseFailed:
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (MtkUfsAllocationIsLinked (Context, Allocation)) {
    Allocation->FreePending     = TRUE;
    Allocation->FreeInProgress = FALSE;
  }

  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }
  return Status;
}

STATIC
EFI_STATUS
MtkUfsReclaimUnexposedAllocations (
  IN MTK_UFS_CONTEXT  *Context
  )
{
  LIST_ENTRY                 *Link;
  LIST_ENTRY                 *NextLink;
  MTK_UFS_COMMON_ALLOCATION  *Allocation;
  EFI_STATUS                 Status;
  EFI_STATUS                 FirstError;
  EFI_TPL                    OldTpl;

  if ((Context == NULL) || Context->ExitedBootServices || !Context->DmaCleanupActive) {
    return EFI_UNSUPPORTED;
  }

  FirstError = EFI_SUCCESS;
  BASE_LIST_FOR_EACH_SAFE (Link, NextLink, &Context->CommonAllocations) {
    Allocation = BASE_CR (Link, MTK_UFS_COMMON_ALLOCATION, Link);
    if (Allocation->Exposed) {
      continue;
    }

    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    if (Allocation->ActiveMappings != 0) {
      if (!EFI_ERROR (FirstError)) {
        FirstError = EFI_NOT_READY;
      }

      gBS->RestoreTPL (OldTpl);
      if (Context->ExitedBootServices) {
        return EFI_UNSUPPORTED;
      }
      continue;
    }

    if (Allocation->FreeInProgress) {
      if (!EFI_ERROR (FirstError)) {
        FirstError = EFI_NOT_READY;
      }

      gBS->RestoreTPL (OldTpl);
      if (Context->ExitedBootServices) {
        return EFI_UNSUPPORTED;
      }
      continue;
    }

    Allocation->FreePending     = TRUE;
    Allocation->FreeInProgress = TRUE;
    gBS->RestoreTPL (OldTpl);
    if (Context->ExitedBootServices) {
      return EFI_UNSUPPORTED;
    }

    Status = MtkUfsReleaseCommonAllocation (Context, Allocation);
    if (Context->ExitedBootServices) {
      return EFI_UNSUPPORTED;
    }
    if (EFI_ERROR (Status) && !EFI_ERROR (FirstError)) {
      FirstError = Status;
    }
  }

  if (EFI_ERROR (FirstError)) {
    return FirstError;
  }

  if (!IsListEmpty (&Context->Mappings) || !IsListEmpty (&Context->CommonAllocations)) {
    return EFI_NOT_READY;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MtkUfsPrepareDmaForStart (
  IN MTK_UFS_CONTEXT  *Context
  )
{
  EFI_TPL     OldTpl;
  EFI_STATUS  Status;

  if ((Context == NULL) || Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if ((Context->ActiveDmaOperations != 0) ||
      !IsListEmpty (&Context->Mappings) ||
      !IsListEmpty (&Context->CommonAllocations) ||
      Context->DmaCleanupActive)
  {
    Status = EFI_NOT_READY;
  } else {
    Context->DmaStopping      = FALSE;
    Context->DmaCleanupActive = FALSE;
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }
  return Status;
}

STATIC
BOOLEAN
MtkUfsRangeIsDmaAddressable (
  IN MTK_UFS_CONTEXT  *Context,
  IN VOID             *HostAddress,
  IN UINTN            NumberOfBytes
  )
{
  UINT64  Address;
  UINT64  Limit;

  if (HostAddress == NULL) {
    return FALSE;
  }

  Address = (UINT64)(UINTN)HostAddress;
  Limit   = MtkUfsDmaLimit (Context);
  if (Address >= Limit) {
    return FALSE;
  }

  return (NumberOfBytes <= (UINTN)(Limit - Address));
}

STATIC
BOOLEAN
MtkUfsIsSeedPath (
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath
  )
{
  VENDOR_DEVICE_PATH         *Vendor;
  EFI_DEVICE_PATH_PROTOCOL   *End;
  UINT16                     Length;

  if (DevicePath == NULL) {
    return FALSE;
  }

  Vendor = (VENDOR_DEVICE_PATH *)DevicePath;
  Length = DevicePathNodeLength (DevicePath);
  if ((Vendor->Header.Type != HARDWARE_DEVICE_PATH) ||
      (Vendor->Header.SubType != HW_VENDOR_DP) ||
      (Length != sizeof (VENDOR_DEVICE_PATH)) ||
      !CompareGuid (&Vendor->Guid, &mMtkUfsSeedGuid))
  {
    return FALSE;
  }

  End = NextDevicePathNode (DevicePath);
  return IsDevicePathEnd (End) &&
         (End->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE);
}

STATIC
BOOLEAN
MtkUfsMmioReadAllowed (
  IN UINT64  Offset
  )
{
  if (mContext.ColdStart && ((Offset == UFS_HC_UCMD_ARG1_OFFSET) ||
      (Offset == UFS_HC_UCMD_ARG2_OFFSET) || (Offset == UFS_HC_UCMD_ARG3_OFFSET))) return TRUE;
  switch (Offset) {
    case UFS_HC_CAP_OFFSET:
    case UFS_HC_VER_OFFSET:
    case UFS_HC_IS_OFFSET:
    case UFS_HC_IE_OFFSET:
    case UFS_HC_STATUS_OFFSET:
    case UFS_HC_ENABLE_OFFSET:
    case UFS_HC_UTRLBA_OFFSET:
    case UFS_HC_UTRLBAU_OFFSET:
    case UFS_HC_UTRLDBR_OFFSET:
    case UFS_HC_UTRLCLR_OFFSET:
    case UFS_HC_UTRLRSR_OFFSET:
    case UFS_HC_UTMRLBA_OFFSET:
    case UFS_HC_UTMRLBAU_OFFSET:
    case UFS_HC_UTMRLDBR_OFFSET:
    case UFS_HC_UTMRLCLR_OFFSET:
    case UFS_HC_UTMRLRSR_OFFSET:
      return TRUE;
    default:
      return FALSE;
  }
}

STATIC
BOOLEAN
MtkUfsMmioWriteAllowed (
  IN UINT64  Offset,
  IN UINT32  Value
  )
{
  if (Offset == UFS_HC_IE_OFFSET) {
    return (BOOLEAN)(Value == 0U);
  }

  if (Offset == UFS_HC_ENABLE_OFFSET) {
    return mContext.ColdStart ?
           (BOOLEAN)((Value == 0U) || (Value == 1U)) :
           (BOOLEAN)(Value == 0U);
  }

  if ((Offset == UFS_HC_UTRLBA_OFFSET) || (Offset == UFS_HC_UTRLBAU_OFFSET) ||
      (Offset == UFS_HC_UTMRLBA_OFFSET) || (Offset == UFS_HC_UTMRLBAU_OFFSET))
  {
    return TRUE;
  }

  if (mContext.ColdStart && ((Offset == UFS_HC_UIC_CMD_OFFSET) ||
      (Offset == UFS_HC_UCMD_ARG1_OFFSET) || (Offset == UFS_HC_UCMD_ARG2_OFFSET) ||
      (Offset == UFS_HC_UCMD_ARG3_OFFSET))) return TRUE;
  switch (Offset) {
    case UFS_HC_IS_OFFSET:
    case UFS_HC_UTRLDBR_OFFSET:
    case UFS_HC_UTRLCLR_OFFSET:
    case UFS_HC_UTRLRSR_OFFSET:
    case UFS_HC_UTMRLDBR_OFFSET:
    case UFS_HC_UTMRLCLR_OFFSET:
    case UFS_HC_UTMRLRSR_OFFSET:
      return TRUE;
    default:
      return FALSE;
  }
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsGetMmioBar (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  OUT UINTN                               *MmioBar
  )
{
  EFI_STATUS  Status;

  if ((This == NULL) || (This != &mContext.HostController) || (MmioBar == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = MtkUfsBeginProviderCallback (&mContext);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *MmioBar = (UINTN)MTK_UFS_MMIO_BASE;
  MtkUfsEndProviderCallback (&mContext);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsReadMmio (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  )
{
  UINTN   Index;
  UINT32  *Words;
  EFI_STATUS Status;

  if ((This == NULL) || (This != &mContext.HostController)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = MtkUfsBeginProviderCallback (&mContext);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Buffer == NULL) || (Count == 0)) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  if ((Width != EfiUfsHcWidthUint32) ||
      (Offset > MTK_UFS_MMIO_LENGTH - sizeof (UINT32)) ||
      (Count > (MTK_UFS_MMIO_LENGTH / sizeof (UINT32))) ||
      ((Offset & (sizeof (UINT32) - 1)) != 0) ||
      ((Offset + (Count * sizeof (UINT32))) > MTK_UFS_MMIO_LENGTH))
  {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  Words = (UINT32 *)Buffer;
  for (Index = 0; Index < Count; Index++) {
    UINT64  CurrentOffset;

    CurrentOffset = Offset + (Index * sizeof (UINT32));
    if (!MtkUfsMmioReadAllowed (CurrentOffset)) {
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
  }

  for (Index = 0; Index < Count; Index++) {
    UINT64  CurrentOffset;

    CurrentOffset = Offset + (Index * sizeof (UINT32));
    Words[Index] = MmioRead32 ((UINTN)(MTK_UFS_MMIO_BASE + CurrentOffset));
  }

  Status = EFI_SUCCESS;

Done:
  MtkUfsEndProviderCallback (&mContext);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsWriteMmio (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  )
{
  UINTN   Index;
  UINT32  *Words;
  EFI_STATUS Status;

  if ((This == NULL) || (This != &mContext.HostController)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = MtkUfsBeginProviderCallback (&mContext);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Buffer == NULL) || (Count == 0)) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  if ((Width != EfiUfsHcWidthUint32) ||
      (Offset > MTK_UFS_MMIO_LENGTH - sizeof (UINT32)) ||
      (Count > (MTK_UFS_MMIO_LENGTH / sizeof (UINT32))) ||
      ((Offset & (sizeof (UINT32) - 1)) != 0) ||
      ((Offset + (Count * sizeof (UINT32))) > MTK_UFS_MMIO_LENGTH))
  {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  Words = (UINT32 *)Buffer;
  for (Index = 0; Index < Count; Index++) {
    UINT64  CurrentOffset;

    CurrentOffset = Offset + (Index * sizeof (UINT32));
    if (!MtkUfsMmioWriteAllowed (CurrentOffset, Words[Index])) {
      Status = EFI_UNSUPPORTED;
      goto Done;
    }
  }

  for (Index = 0; Index < Count; Index++) {
    UINT64  CurrentOffset;

    CurrentOffset = Offset + (Index * sizeof (UINT32));
    MmioWrite32 ((UINTN)(MTK_UFS_MMIO_BASE + CurrentOffset), Words[Index]);
  }

  Status = EFI_SUCCESS;

Done:
  MtkUfsEndProviderCallback (&mContext);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsAllocateBuffer (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL   *This,
  IN  EFI_ALLOCATE_TYPE                    Type,
  IN  EFI_MEMORY_TYPE                      MemoryType,
  IN  UINTN                                Pages,
  OUT VOID                                 **HostAddress,
  IN  UINT64                               Attributes
  )
{
  MTK_UFS_CONTEXT                *Context;
  MTK_UFS_COMMON_ALLOCATION      *Allocation;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  EFI_PHYSICAL_ADDRESS           Address;
  EFI_PHYSICAL_ADDRESS           MaximumAddress;
  EFI_STATUS                     Status;
  EFI_STATUS                     CleanupStatus;
  EFI_TPL                        OldTpl;
  UINT64                         Length;
  UINT64                         DescriptorEnd;
  UINT64                         UncachedAttributes;

  if ((This == NULL) || (This != &mContext.HostController) ||
      (HostAddress == NULL) || (Pages == 0) ||
      ((MemoryType != EfiBootServicesData) &&
       (MemoryType != EfiRuntimeServicesData)))
  {
    return EFI_INVALID_PARAMETER;
  }

  *HostAddress = NULL;
  (VOID)Type;
  if (Attributes != 0) {
    return EFI_UNSUPPORTED;
  }

  if ((UINT64)Pages > (MAX_UINT64 / EFI_PAGE_SIZE)) {
    return EFI_OUT_OF_RESOURCES;
  }

  Context = MtkUfsContextFromHc (This);
  Status  = MtkUfsBeginDmaOperation (Context, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* Reserve ownership metadata before AllocatePages so every successful page
   * allocation has an owner even if a later service or cleanup step fails. */
  Allocation = AllocateZeroPool (sizeof (*Allocation));
  if (Context->ExitedBootServices) {
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }
  if (Allocation == NULL) {
    MtkUfsEndDmaOperation (Context);
    return EFI_OUT_OF_RESOURCES;
  }

  InitializeListHead (&Allocation->Link);
  Length        = EFI_PAGES_TO_SIZE (Pages);
  MaximumAddress = MtkUfsDmaLimit (Context) - 1;
  Address        = MaximumAddress;
  Status = gBS->AllocatePages (AllocateMaxAddress, MemoryType, Pages, &Address);
  if (Context->ExitedBootServices) {
    if (!EFI_ERROR (Status)) {
      Allocation->HostAddress = (VOID *)(UINTN)Address;
      Allocation->Pages       = Pages;
      InsertTailList (&Context->CommonAllocations, &Allocation->Link);
    }
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    FreePool (Allocation);
    MtkUfsEndDmaOperation (Context);
    return Status;
  }

  Allocation->HostAddress = (VOID *)(UINTN)Address;
  Allocation->Pages       = Pages;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  InsertTailList (&Context->CommonAllocations, &Allocation->Link);
  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices) {
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }

  if ((Address >= MtkUfsDmaLimit (Context)) ||
      (Length > (MtkUfsDmaLimit (Context) - Address)))
  {
    Status = EFI_UNSUPPORTED;
    goto ReclaimUnexposed;
  }

  Status = gDS->GetMemorySpaceDescriptor (Address, &Descriptor);
  if (Context->ExitedBootServices) {
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    goto ReclaimUnexposed;
  }

  if ((Descriptor.GcdMemoryType != EfiGcdMemoryTypeSystemMemory) ||
      ((Descriptor.Capabilities & EFI_MEMORY_UC) == 0) ||
      (Descriptor.Length == 0U) ||
      (Descriptor.BaseAddress > Address) ||
      (Descriptor.BaseAddress > MAX_UINT64 - Descriptor.Length))
  {
    Status = EFI_UNSUPPORTED;
    goto ReclaimUnexposed;
  }

  DescriptorEnd = Descriptor.BaseAddress + Descriptor.Length;
  if ((Address >= DescriptorEnd) || (Length > (DescriptorEnd - Address))) {
    Status = EFI_UNSUPPORTED;
    goto ReclaimUnexposed;
  }

  Allocation->OriginalAttributes = Descriptor.Attributes;
  Allocation->RestoreAttributesPending = TRUE;
  UncachedAttributes = (Descriptor.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) |
                       EFI_MEMORY_UC;
  Status = gDS->SetMemorySpaceAttributes (Address, Length, UncachedAttributes);
  if (Context->ExitedBootServices) {
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    goto ReclaimUnexposed;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Allocation->Exposed = TRUE;
  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices) {
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }
  *HostAddress = Allocation->HostAddress;
  MtkUfsEndDmaOperation (Context);
  return EFI_SUCCESS;

ReclaimUnexposed:
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Allocation->FreePending     = TRUE;
  Allocation->FreeInProgress = TRUE;
  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices) {
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }
  CleanupStatus = MtkUfsReleaseCommonAllocation (Context, Allocation);
  if (Context->ExitedBootServices) {
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (CleanupStatus)) {
    DEBUG ((DEBUG_ERROR, "UFS DMA page allocation cleanup retained owner: %r (cause %r)\n",
            CleanupStatus, Status));
    Status = CleanupStatus;
  }

  MtkUfsEndDmaOperation (Context);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsFreeBuffer (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN UINTN                              Pages,
  IN VOID                               *HostAddress
  )
{
  MTK_UFS_CONTEXT            *Context;
  LIST_ENTRY                 *Link;
  MTK_UFS_COMMON_ALLOCATION  *Allocation;
  EFI_STATUS                 Status;
  EFI_TPL                    OldTpl;

  if ((This == NULL) || (This != &mContext.HostController) ||
      (HostAddress == NULL) || (Pages == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Context = MtkUfsContextFromHc (This);
  Status = MtkUfsBeginDmaOperation (Context, TRUE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Allocation = NULL;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  for (Link = GetFirstNode (&Context->CommonAllocations);
       !IsNull (&Context->CommonAllocations, Link);
       Link = GetNextNode (&Context->CommonAllocations, Link))
  {
    MTK_UFS_COMMON_ALLOCATION  *Candidate;

    Candidate = BASE_CR (Link, MTK_UFS_COMMON_ALLOCATION, Link);
    if ((Candidate->HostAddress == HostAddress) && (Candidate->Pages == Pages)) {
      Allocation = Candidate;
      break;
    }
  }

  if (Allocation == NULL) {
    Status = EFI_INVALID_PARAMETER;
  } else if ((Allocation->ActiveMappings != 0) || Allocation->FreeInProgress) {
    Status = EFI_NOT_READY;
  } else {
    Allocation->FreePending     = TRUE;
    Allocation->FreeInProgress = TRUE;
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices) {
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }
  if (!EFI_ERROR (Status)) {
    Status = MtkUfsReleaseCommonAllocation (Context, Allocation);
    if (Context->ExitedBootServices) {
      MtkUfsEndDmaOperation (Context);
      return EFI_UNSUPPORTED;
    }
  }

  MtkUfsEndDmaOperation (Context);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsMap (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL   *This,
  IN     EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  IN     VOID                                 *HostAddress,
  IN OUT UINTN                                *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS                 *DeviceAddress,
  OUT    VOID                                 **Mapping
  )
{
  MTK_UFS_CONTEXT            *Context;
  MTK_UFS_MAPPING            *Map;
  MTK_UFS_COMMON_ALLOCATION  *Allocation;
  EFI_STATUS                 Status;
  EFI_TPL                    OldTpl;
  EFI_PHYSICAL_ADDRESS       Address;
  UINTN                      Alignment, Pages, CacheBytes;
  BOOLEAN                    PartialOverlap, Bounce;

  if ((This == NULL) || (This != &mContext.HostController) ||
      (HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (DeviceAddress == NULL) || (Mapping == NULL) ||
      ((UINTN)Operation >= EdkiiUfsHcOperationMaximum) || (*NumberOfBytes == 0) ||
      (*NumberOfBytes > MAX_UINTN - (UINTN)HostAddress))
  {
    return EFI_INVALID_PARAMETER;
  }

  *DeviceAddress = 0;
  *Mapping       = NULL;
  Context = MtkUfsContextFromHc (This);
  Status = MtkUfsBeginDmaOperation (Context, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Bounce = !MtkUfsRangeIsDmaAddressable (Context, HostAddress, *NumberOfBytes);
  if (Operation == EdkiiUfsHcOperationBusMasterWrite) {
    Alignment = ArmDataCacheLineLength ();
    Bounce = Bounce || (((UINTN)HostAddress | *NumberOfBytes) & (Alignment - 1)) != 0;
  }
  if (Bounce && ((Operation == EdkiiUfsHcOperationBusMasterCommonBuffer) ||
                 (*NumberOfBytes > MtkUfsDmaLimit (Context)))) {
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }

  Map = AllocateZeroPool (sizeof (*Map));
  if (Context->ExitedBootServices) {
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }
  if (Map == NULL) {
    MtkUfsEndDmaOperation (Context);
    return EFI_OUT_OF_RESOURCES;
  }

  InitializeListHead (&Map->Link);
  Map->Signature     = MTK_UFS_MAPPING_SIGNATURE;
  Map->HostAddress   = HostAddress;
  Map->NumberOfBytes = *NumberOfBytes;
  Map->Operation     = Operation;
  Address            = (UINT64)(UINTN)HostAddress;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Allocation = MtkUfsFindCommonAllocationRange (Context, HostAddress, *NumberOfBytes, &PartialOverlap);
  if (PartialOverlap) {
    Status = EFI_UNSUPPORTED;
  } else if ((Allocation != NULL) && Allocation->FreePending) {
    Status = EFI_NOT_READY;
  } else if ((Allocation != NULL) && (Allocation->ActiveMappings == MAX_UINT32)) {
    Status = EFI_OUT_OF_RESOURCES;
  } else if ((Operation == EdkiiUfsHcOperationBusMasterCommonBuffer) && (Allocation == NULL)) {
    Status = EFI_UNSUPPORTED;
  } else {
    Map->Allocation = Allocation;
    if (Allocation != NULL) {
      Allocation->ActiveMappings++;
    }

    InsertTailList (&Context->Mappings, &Map->Link);
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices) {
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    FreePool (Map);
    MtkUfsEndDmaOperation (Context);
    return Status;
  }

  CacheBytes = *NumberOfBytes;
  if (Bounce) {
    Pages = EFI_SIZE_TO_PAGES (*NumberOfBytes);
    Address = MtkUfsDmaLimit (Context) - 1;
    Status = gBS->AllocatePages (AllocateMaxAddress, EfiRuntimeServicesData, Pages, &Address);
    if (!EFI_ERROR (Status)) {
      Map->BounceAddress = Address;
      Map->BouncePages = Pages;
    }
    if (Context->ExitedBootServices) {
      MtkUfsEndDmaOperation (Context);
      return EFI_UNSUPPORTED;
    }
    if (EFI_ERROR (Status)) {
      OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
      if (Allocation != NULL) Allocation->ActiveMappings--;
      RemoveEntryList (&Map->Link);
      gBS->RestoreTPL (OldTpl);
      if (!Context->ExitedBootServices) FreePool (Map);
      MtkUfsEndDmaOperation (Context);
      return Status;
    }
    // Preserve bytes beyond a short incoming transfer, as a direct map would.
    CopyMem ((VOID *)(UINTN)Address, HostAddress, *NumberOfBytes);
    CacheBytes = EFI_PAGES_TO_SIZE (Pages);
  }

  if (Operation == EdkiiUfsHcOperationBusMasterRead) {
    WriteBackDataCacheRange ((VOID *)(UINTN)Address, CacheBytes);
  } else if (Operation == EdkiiUfsHcOperationBusMasterWrite) {
    WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Address, CacheBytes);
  }

  MemoryFence ();
  *DeviceAddress = Address;
  *Mapping       = Map;
  MtkUfsEndDmaOperation (Context);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsUnmap (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN VOID                               *Mapping
  )
{
  MTK_UFS_CONTEXT            *Context;
  LIST_ENTRY                 *Link;
  MTK_UFS_MAPPING            *Map;
  MTK_UFS_COMMON_ALLOCATION  *Allocation;
  EFI_STATUS                 Status;
  EFI_TPL                    OldTpl;
  VOID                       *HostAddress;
  UINTN                      NumberOfBytes;
  EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation;

  if ((This == NULL) || (This != &mContext.HostController) || (Mapping == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Context = MtkUfsContextFromHc (This);
  Status = MtkUfsBeginDmaOperation (Context, TRUE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Map = NULL;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  for (Link = GetFirstNode (&Context->Mappings);
       !IsNull (&Context->Mappings, Link);
       Link = GetNextNode (&Context->Mappings, Link))
  {
    MTK_UFS_MAPPING  *Candidate;

    Candidate = BASE_CR (Link, MTK_UFS_MAPPING, Link);
    if ((VOID *)Candidate == Mapping) {
      Map = Candidate;
      break;
    }
  }

  if (Map == NULL) {
    Status = EFI_INVALID_PARAMETER;
  } else if (Map->Signature != MTK_UFS_MAPPING_SIGNATURE) {
    Status = EFI_COMPROMISED_DATA;
  } else if (Map->Unmapping) {
    Status = EFI_NOT_READY;
  } else if ((Map->Allocation != NULL) &&
             (!MtkUfsAllocationIsLinked (Context, Map->Allocation) ||
              (Map->Allocation->ActiveMappings == 0)))
  {
    Status = EFI_COMPROMISED_DATA;
  } else {
    Map->Unmapping = TRUE;
    HostAddress    = Map->HostAddress;
    NumberOfBytes  = Map->NumberOfBytes;
    Operation      = Map->Operation;
    Allocation     = Map->Allocation;
    Status         = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices) {
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    MtkUfsEndDmaOperation (Context);
    return Status;
  }

  if (Operation == EdkiiUfsHcOperationBusMasterWrite && !Map->BounceCopied) {
    if (Map->BouncePages == 0) {
      InvalidateDataCacheRange (HostAddress, NumberOfBytes);
    } else {
      VOID *Bounce = (VOID *)(UINTN)Map->BounceAddress;
      InvalidateDataCacheRange (Bounce, EFI_PAGES_TO_SIZE (Map->BouncePages));
      MemoryFence ();
      CopyMem (HostAddress, Bounce, NumberOfBytes);
      Map->BounceCopied = TRUE;
    }
  }

  if (Map->BouncePages != 0) {
    Status = gBS->FreePages (Map->BounceAddress, Map->BouncePages);
    if (Context->ExitedBootServices) {
      MtkUfsEndDmaOperation (Context);
      return EFI_UNSUPPORTED;
    }
    if (EFI_ERROR (Status)) {
      OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
      Map->Unmapping = FALSE;
      gBS->RestoreTPL (OldTpl);
      MtkUfsEndDmaOperation (Context);
      return Status;
    }
    Map->BouncePages = 0;
  }

  MemoryFence ();
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if ((Allocation != NULL) && !MtkUfsAllocationIsLinked (Context, Allocation)) {
    Map->Unmapping = FALSE;
    Status = EFI_COMPROMISED_DATA;
  } else {
    if (Allocation != NULL) {
      Allocation->ActiveMappings--;
    }

    Map->Signature = 0;
    RemoveEntryList (&Map->Link);
    InitializeListHead (&Map->Link);
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices) {
    MtkUfsEndDmaOperation (Context);
    return EFI_UNSUPPORTED;
  }
  if (!EFI_ERROR (Status)) {
    FreePool (Map);
    if (Context->ExitedBootServices) {
      MtkUfsEndDmaOperation (Context);
      return EFI_UNSUPPORTED;
    }
  }

  MtkUfsEndDmaOperation (Context);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsFlush (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  )
{
  MTK_UFS_CONTEXT  *Context;
  EFI_STATUS       Status;

  if ((This == NULL) || (This != &mContext.HostController)) {
    return EFI_INVALID_PARAMETER;
  }

  Context = MtkUfsContextFromHc (This);
  Status = MtkUfsBeginDmaOperation (Context, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MemoryFence ();
  if (MmioRead32 ((UINTN)(MTK_UFS_MMIO_BASE + UFS_HC_STATUS_OFFSET)) == MAX_UINT32) {
    Status = EFI_DEVICE_ERROR;
  } else {
    Status = EFI_SUCCESS;
  }

  MtkUfsEndDmaOperation (Context);
  return Status;
}

STATIC
UINT64
MtkUfsReadFdtCells (
  IN CONST UINT8  *Data,
  IN UINTN        CellCount
  )
{
  UINT64  Value;
  UINTN   Index;

  Value = 0;
  for (Index = 0; Index < (CellCount * sizeof (UINT32)); Index++) {
    Value = (Value << 8) | Data[Index];
  }

  return Value;
}

STATIC
BOOLEAN
MtkUfsFdtNodeEnabled (
  IN CONST VOID  *Fdt,
  IN INT32       Node
  )
{
  INT32        Length;
  CONST CHAR8  *Status;

  Status = FdtGetProp (Fdt, Node, "status", &Length);
  return (BOOLEAN)((Status == NULL) ||
                   ((Length == 5) && (CompareMem (Status, "okay", 5) == 0)) ||
                   ((Length == 3) && (CompareMem (Status, "ok", 3) == 0)));
}

STATIC
BOOLEAN
MtkUfsFdtCompatible (
  IN CONST VOID  *Fdt,
  IN INT32       Node,
  IN CONST CHAR8 *Expected
  )
{
  INT32        Length;
  CONST CHAR8  *Compatible;
  UINTN        ExpectedLength;
  UINTN        Offset;

  Compatible = FdtGetProp (Fdt, Node, "compatible", &Length);
  if ((Compatible == NULL) || (Length <= 0)) {
    return FALSE;
  }

  ExpectedLength = AsciiStrLen (Expected);
  for (Offset = 0; Offset < (UINTN)Length; ) {
    UINTN  End;

    End = Offset;
    while ((End < (UINTN)Length) && (Compatible[End] != '\0')) {
      End++;
    }

    if (End == (UINTN)Length) {
      return FALSE;
    }

    if (((End - Offset) == ExpectedLength) &&
        (CompareMem (Compatible + Offset, Expected, ExpectedLength) == 0))
    {
      return TRUE;
    }

    Offset = End + 1U;
  }

  return FALSE;
}

STATIC
EFI_STATUS
MtkUfsGetRuntimeFdt (
  OUT CONST VOID  **Fdt,
  OUT UINTN       *FdtSize
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  VOID                             *Table;
  UINT64                           Address;
  UINT64                           Size;
  UINT64                           DescriptorEnd;

  if ((Fdt == NULL) || (FdtSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Fdt = NULL;
  *FdtSize = 0;
  if (MmioRead64 (MTK_UFS_BOOT_HANDOFF_BASE) != MTK_UFS_BOOT_HANDOFF_MAGIC) {
    return EFI_NOT_READY;
  }

  if (MmioRead64 (MTK_UFS_BOOT_HANDOFF_BASE + 0x58U) != EFI_SUCCESS) {
    return EFI_NOT_READY;
  }

  Address = MmioRead64 (MTK_UFS_BOOT_HANDOFF_BASE + 0x48U);
  Size = MmioRead64 (MTK_UFS_BOOT_HANDOFF_BASE + 0x50U);
  if ((Address == 0U) || ((Address & 7U) != 0U) ||
      (Address > MAX_UINTN) || (Size < sizeof (FDT_HEADER)) ||
      (Size > MTK_UFS_MAX_DTB_SIZE) || (Size > MAX_UINTN) ||
      (Address > MAX_UINT64 - Size))
  {
    return EFI_COMPROMISED_DATA;
  }

  Table = NULL;
  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Table);
  if (EFI_ERROR (Status) || (Table == NULL)) {
    return EFI_NOT_READY;
  }

  if ((UINT64)(UINTN)Table != Address) {
    return EFI_COMPROMISED_DATA;
  }

  Status = gDS->GetMemorySpaceDescriptor (Address, &Descriptor);
  if (EFI_ERROR (Status) ||
      (Descriptor.GcdMemoryType != EfiGcdMemoryTypeSystemMemory) ||
      (Descriptor.Length == 0U) ||
      (Descriptor.BaseAddress > Address) ||
      (Descriptor.BaseAddress > MAX_UINT64 - Descriptor.Length))
  {
    return EFI_UNSUPPORTED;
  }

  DescriptorEnd = Descriptor.BaseAddress + Descriptor.Length;
  if ((Address >= DescriptorEnd) || (Size > (DescriptorEnd - Address))) {
    return EFI_UNSUPPORTED;
  }

  if ((FdtTotalSize (Table) != Size) ||
      (FdtCheckFull (Table, (UINTN)Size) != 0))
  {
    return EFI_COMPROMISED_DATA;
  }

  *Fdt = Table;
  *FdtSize = (UINTN)Size;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MtkUfsValidateBoardFdt (
  IN CONST VOID  *Fdt
  )
{
  CONST UINT8  *Property;
  INT32        Root;
  INT32        Soc;
  INT32        UfsNode;
  INT32        Node;
  INT32        Depth;
  INT32        Length;
  INT32        Parent;
  UINTN        UfsCompatibleCount;

  if (Fdt == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!MtkUfsFdtCompatible (Fdt, 0, "mediatek,MT6789")) {
    return EFI_UNSUPPORTED;
  }

  Root = 0;
  Soc = FdtPathOffset (Fdt, "/soc");
  UfsNode = FdtPathOffset (Fdt, "/soc/ufshci@11270000");
  if ((Soc < 0) || (UfsNode < 0) ||
      !MtkUfsFdtNodeEnabled (Fdt, Root) ||
      !MtkUfsFdtNodeEnabled (Fdt, Soc) ||
      !MtkUfsFdtNodeEnabled (Fdt, UfsNode) ||
      (FdtParentOffset (Fdt, UfsNode) != Soc) ||
      (FdtAddressCells (Fdt, Root) != 2) ||
      (FdtSizeCells (Fdt, Root) != 2) ||
      (FdtAddressCells (Fdt, Soc) != 2) ||
      (FdtSizeCells (Fdt, Soc) != 2) ||
      !MtkUfsFdtCompatible (Fdt, UfsNode, "mediatek,mt8183-ufshci"))
  {
    return EFI_UNSUPPORTED;
  }

  UfsCompatibleCount = 0;
  Depth = 0;
  for (Node = FdtNextNode (Fdt, -1, &Depth);
       (Node >= 0) && (Depth >= 0);
       Node = FdtNextNode (Fdt, Node, &Depth))
  {
    if (MtkUfsFdtCompatible (Fdt, Node, "mediatek,mt8183-ufshci")) {
      UfsCompatibleCount++;
      if (Node != UfsNode) {
        return EFI_UNSUPPORTED;
      }
    }
  }

  // libfdt returns -FDT_ERR_NOTFOUND at the valid end of the tree and leaves
  // Depth at zero after closing the root node. Other negative results indicate
  // a traversal error even though the full-blob check above should catch it.
  if ((Node != -FDT_ERR_NOTFOUND) || (Depth != 0) || (UfsCompatibleCount != 1U)) {
    return EFI_COMPROMISED_DATA;
  }

  Property = FdtGetProp (Fdt, UfsNode, "reg", &Length);
  if ((Property == NULL) || (Length != 16) ||
      (MtkUfsReadFdtCells (Property, 2) != MTK_UFS_MMIO_BASE) ||
      (MtkUfsReadFdtCells (Property + 8, 2) != MTK_UFS_MMIO_LENGTH))
  {
    return EFI_UNSUPPORTED;
  }

  Property = FdtGetProp (Fdt, Soc, "ranges", &Length);
  if ((Property == NULL) || (Length != 0)) {
    return EFI_UNSUPPORTED;
  }

  Property = FdtGetProp (Fdt, Soc, "dma-ranges", &Length);
  if ((Property == NULL) || (Length != 24) ||
      (MtkUfsReadFdtCells (Property, 2) != 0U) ||
      (MtkUfsReadFdtCells (Property + 8, 2) != 0U) ||
      (MtkUfsReadFdtCells (Property + 16, 2) != MTK_UFS_DMA_LIMIT_64))
  {
    return EFI_UNSUPPORTED;
  }

  for (Parent = UfsNode; Parent >= 0; Parent = FdtParentOffset (Fdt, Parent)) {
    if ((FdtGetProp (Fdt, Parent, "iommus", &Length) != NULL) ||
        (FdtGetProp (Fdt, Parent, "iommu-map", &Length) != NULL) ||
        (FdtGetProp (Fdt, Parent, "dma-coherent", &Length) != NULL))
    {
      return EFI_UNSUPPORTED;
    }

    if ((Parent != Soc) &&
        (FdtGetProp (Fdt, Parent, "dma-ranges", &Length) != NULL))
    {
      return EFI_UNSUPPORTED;
    }

    if (Parent == Root) {
      break;
    }
  }

  if (Parent != Root) {
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MtkUfsValidateMappedController (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  UINT64                           Offset;

  Status = gDS->GetMemorySpaceDescriptor (MTK_UFS_MMIO_BASE, &Descriptor);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Descriptor.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo) ||
      ((Descriptor.Attributes & EFI_MEMORY_UC) == 0U) ||
      (Descriptor.Length == 0U) ||
      (Descriptor.BaseAddress > MTK_UFS_MMIO_BASE) ||
      (Descriptor.BaseAddress > MAX_UINT64 - Descriptor.Length))
  {
    return EFI_UNSUPPORTED;
  }

  Offset = MTK_UFS_MMIO_BASE - Descriptor.BaseAddress;
  if ((Offset >= Descriptor.Length) ||
      (MTK_UFS_MMIO_LENGTH > (Descriptor.Length - Offset)))
  {
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MtkUfsValidateBoardResources (
  OUT CONST VOID  **Fdt OPTIONAL,
  OUT UINTN       *FdtSize OPTIONAL
  )
{
  CONST VOID  *Tree;
  UINTN       TreeSize;
  EFI_STATUS  Status;

  Status = MtkUfsGetRuntimeFdt (&Tree, &TreeSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = MtkUfsValidateBoardFdt (Tree);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = MtkUfsValidateMappedController ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Fdt != NULL) {
    *Fdt = Tree;
  }

  if (FdtSize != NULL) {
    *FdtSize = TreeSize;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsDriverSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_STATUS                Status;
  EFI_TPL                   OldTpl;
  BOOLEAN                   CanRetryStart;

  if ((This == NULL) || mEntryQuarantined || mContext.ExitedBootServices ||
      (ControllerHandle != mContext.SeedHandle)) {
    return EFI_UNSUPPORTED;
  }

  if ((RemainingDevicePath != NULL) && !IsDevicePathEnd (RemainingDevicePath)) {
    return EFI_UNSUPPORTED;
  }

  Status = gBS->HandleProtocol (
                  ControllerHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&DevicePath
                  );
  if (EFI_ERROR (Status) || !MtkUfsIsSeedPath (DevicePath)) {
    return EFI_UNSUPPORTED;
  }

  Status = MtkUfsValidateBoardResources (NULL, NULL);
  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  CanRetryStart = (BOOLEAN)(mContext.Starting && !mContext.StartBusy &&
                            !mContext.StopBusy && !mContext.CallbackClosing &&
                            (mContext.ControllerHandle == ControllerHandle));
  Status = (mContext.Started || (mContext.Starting && !CanRetryStart)) ?
           EFI_ALREADY_STARTED : EFI_SUCCESS;
  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsPlatformCallback (
  IN EFI_HANDLE ControllerHandle,
  IN EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE Phase,
  IN OUT VOID *CallbackData
  )
{
  EDKII_UFS_HC_DRIVER_INTERFACE *Interface = CallbackData;
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  BOOLEAN StopIntent;

  if (mContext.ExitedBootServices || mContext.ColdHost.IoClosed) {
    return EFI_UNSUPPORTED;
  }
  Status = MtkUfsBeginProviderCallback (&mContext);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (mContext.ExitedBootServices || mContext.ColdHost.IoClosed) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  if (ControllerHandle != mContext.ControllerHandle || Interface == NULL ||
      Interface->UfsHcProtocol != &mContext.HostController || (UINT32)Phase > EdkiiUfsHcHostStopped)
  {
    Status = EFI_INVALID_PARAMETER;
  } else {
    StopIntent = (BOOLEAN)(Phase == EdkiiUfsHcPrepareStop || Phase == EdkiiUfsHcHostStopped);
    if (StopIntent) {
      /* Revoke the OS-ready handoff in a short critical section, then run the
       * bounded UIC work at TPL_CALLBACK or below. */
      if (mContext.ExitedBootServices || mContext.ColdHost.IoClosed) {
        Status = EFI_UNSUPPORTED;
      } else {
        OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
        if (mContext.ExitedBootServices || mContext.ColdHost.IoClosed) {
          Status = EFI_UNSUPPORTED;
        } else {
          MtkUfsPublishOsReady (FALSE);
          Status = EFI_SUCCESS;
        }
        gBS->RestoreTPL (OldTpl);
        if (mContext.ExitedBootServices || mContext.ColdHost.IoClosed) {
          Status = EFI_UNSUPPORTED;
        } else if (!mContext.ColdStart ||
                   (mContext.Platform.Version < EDKII_UFS_HC_PLATFORM_PROTOCOL_STOP_VERSION)) {
          Status = EFI_UNSUPPORTED;
        } else {
          Status = MtkUfsColdHostCallback (&mContext.ColdHost, Phase, Interface);
          if (mContext.ExitedBootServices || mContext.ColdHost.IoClosed) Status = EFI_UNSUPPORTED;
        }
      }
    } else if (!mContext.ColdStart) {
      Status = EFI_SUCCESS;
    } else {
      Status = MtkUfsColdHostCallback (&mContext.ColdHost, Phase, Interface);
    }
  }

Done:
  MtkUfsEndProviderCallback (&mContext);
  return Status;
}

STATIC
BOOLEAN
MtkUfsColdResourcesOwned (VOID)
{
  UINTN Index;
  /* VEMC is deliberately retained because it is shared with MMC; only owned
   * UFS gates participate in the cold-resource Stop guard. */
  for (Index = 0; Index < ARRAY_SIZE (mContext.ColdHost.ClockOwned); Index++)
    if (mContext.ColdHost.ClockOwned[Index]) return TRUE;
  return FALSE;
}

STATIC
BOOLEAN
MtkUfsProtocolNotInstalled (
  IN EFI_STATUS  Status
  )
{
  return (BOOLEAN)((Status == EFI_NOT_FOUND) || (Status == EFI_UNSUPPORTED));
}

STATIC
EFI_STATUS
MtkUfsProbePublishedProtocols (
  IN MTK_UFS_CONTEXT  *Context,
  IN EFI_HANDLE      ControllerHandle
  )
{
  EFI_STATUS  Status;
  VOID        *Interface;
  BOOLEAN     HostPublished;
  BOOLEAN     PlatformPublished;
  EFI_TPL     OldTpl;

  if ((Context == NULL) || (ControllerHandle == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  HostPublished = FALSE;
  Interface = NULL;
  Status = gBS->HandleProtocol (
                 ControllerHandle,
                 &gEdkiiUfsHostControllerProtocolGuid,
                 &Interface
                 );
  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }
  if (Status == EFI_SUCCESS) {
    if (Interface != &Context->HostController) {
      return EFI_ACCESS_DENIED;
    }
    HostPublished = TRUE;
  } else if (!MtkUfsProtocolNotInstalled (Status)) {
    return Status;
  }

  PlatformPublished = FALSE;
  Interface = NULL;
  Status = gBS->HandleProtocol (
                 ControllerHandle,
                 &gEdkiiUfsHcPlatformProtocolGuid,
                 &Interface
                 );
  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }
  if (Status == EFI_SUCCESS) {
    if (Interface != &Context->Platform) {
      return EFI_ACCESS_DENIED;
    }
    PlatformPublished = TRUE;
  } else if (!MtkUfsProtocolNotInstalled (Status)) {
    return Status;
  }

  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Context->ControllerHandle == ControllerHandle) {
    Context->HostPublished     = HostPublished;
    Context->PlatformPublished = PlatformPublished;
  }
  gBS->RestoreTPL (OldTpl);
  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MtkUfsPublishProviderProtocols (
  IN MTK_UFS_CONTEXT  *Context,
  IN EFI_HANDLE      ControllerHandle
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;
  BOOLEAN     HandleChanged;

  if ((Context == NULL) || Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  Status = MtkUfsProbePublishedProtocols (Context, ControllerHandle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Context->HostPublished && Context->PlatformPublished) {
    return EFI_SUCCESS;
  }

  Handle = ControllerHandle;
  HandleChanged = FALSE;
  if (!Context->HostPublished && !Context->PlatformPublished) {
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Handle,
                    &gEdkiiUfsHostControllerProtocolGuid,
                    &Context->HostController,
                    &gEdkiiUfsHcPlatformProtocolGuid,
                    &Context->Platform,
                    NULL
                    );
  } else if (!Context->HostPublished) {
    Status = gBS->InstallProtocolInterface (
                    &Handle,
                    &gEdkiiUfsHostControllerProtocolGuid,
                    EFI_NATIVE_INTERFACE,
                    &Context->HostController
                    );
  } else {
    Status = gBS->InstallProtocolInterface (
                    &Handle,
                    &gEdkiiUfsHcPlatformProtocolGuid,
                    EFI_NATIVE_INTERFACE,
                    &Context->Platform
                    );
  }

  if (Handle != ControllerHandle) {
    EFI_TPL  OldTpl;

    HandleChanged = TRUE;
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    Context->ControllerHandle = Handle;
    gBS->RestoreTPL (OldTpl);
    if (Context->ExitedBootServices) {
      return EFI_UNSUPPORTED;
    }
  }

  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  {
    EFI_STATUS  ProbeStatus;

    ProbeStatus = MtkUfsProbePublishedProtocols (Context, Handle);
    if (EFI_ERROR (ProbeStatus)) {
      return ProbeStatus;
    }
  }

  if (Context->HostPublished && Context->PlatformPublished) {
    if (HandleChanged) {
      mEntryQuarantined = TRUE;
      return EFI_DEVICE_ERROR;
    }
    return EFI_SUCCESS;
  }

  if (HandleChanged) {
    mEntryQuarantined = TRUE;
    return EFI_DEVICE_ERROR;
  }

  return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
}

STATIC
EFI_STATUS
MtkUfsCleanupUnpublishedStart (
  IN MTK_UFS_CONTEXT  *Context
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;

  if (Context == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = MtkUfsProbePublishedProtocols (Context, Context->ControllerHandle);
  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Context->HostPublished || Context->PlatformPublished) {
    return EFI_NOT_READY;
  }

  Status = MtkUfsBeginDmaCleanup (Context, TRUE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = MtkUfsReclaimUnexposedAllocations (Context);
  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }
  if (!EFI_ERROR (Status) && Context->ColdStart) {
    Status = MtkUfsReleaseUnstartedColdHost (&Context->ColdHost);
    if (Context->ExitedBootServices) {
      return EFI_UNSUPPORTED;
    }
    if (EFI_ERROR (Status) && !Context->ColdHost.CleanupPending) {
      mEntryQuarantined = TRUE;
    }
  }

  if (EFI_ERROR (Status)) {
    MtkUfsEndDmaCleanup (Context, FALSE, FALSE);
    return Status;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Context->Starting           = FALSE;
  Context->Started            = FALSE;
  Context->CallbackClosing    = FALSE;
  Context->ControllerHandle  = NULL;
  Context->ColdStart          = FALSE;
  Context->HostPublished      = FALSE;
  Context->PlatformPublished  = FALSE;
  gBS->RestoreTPL (OldTpl);
  MtkUfsEndDmaCleanup (Context, FALSE, FALSE);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MtkUfsUninstallPublishedProtocols (
  IN MTK_UFS_CONTEXT  *Context,
  IN EFI_HANDLE      ControllerHandle
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  ProbeStatus;

  ProbeStatus = MtkUfsProbePublishedProtocols (Context, ControllerHandle);
  if (EFI_ERROR (ProbeStatus)) {
    return ProbeStatus;
  }
  if (!Context->HostPublished && !Context->PlatformPublished) {
    return EFI_SUCCESS;
  }

  if (Context->HostPublished && Context->PlatformPublished) {
    Status = gBS->UninstallMultipleProtocolInterfaces (
                    ControllerHandle,
                    &gEdkiiUfsHostControllerProtocolGuid,
                    &Context->HostController,
                    &gEdkiiUfsHcPlatformProtocolGuid,
                    &Context->Platform,
                    NULL
                    );
  } else if (Context->HostPublished) {
    Status = gBS->UninstallProtocolInterface (
                    ControllerHandle,
                    &gEdkiiUfsHostControllerProtocolGuid,
                    &Context->HostController
                    );
  } else {
    Status = gBS->UninstallProtocolInterface (
                    ControllerHandle,
                    &gEdkiiUfsHcPlatformProtocolGuid,
                    &Context->Platform
                    );
  }

  if (Context->ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  ProbeStatus = MtkUfsProbePublishedProtocols (Context, ControllerHandle);
  if (EFI_ERROR (ProbeStatus)) {
    return ProbeStatus;
  }
  if (!Context->HostPublished && !Context->PlatformPublished) {
    return EFI_SUCCESS;
  }

  return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsDriverStartWorker (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS              Status;
  UINT32                  Hce;
  UINT32                  Hcs;
  UINT32                  Capabilities;
  UINT32                  TransferDoorbell;
  UINT32                  TaskDoorbell;
  MTK_UFS_HANDOFF_STATUS  Handoff;
  CONST VOID             *Tree;
  EFI_TPL                 OldTpl;

  if ((This == NULL) || mEntryQuarantined || mContext.ExitedBootServices ||
      (ControllerHandle != mContext.SeedHandle))
  {
    return EFI_UNSUPPORTED;
  }

  if ((RemainingDevicePath != NULL) && !IsDevicePathEnd (RemainingDevicePath)) {
    return EFI_UNSUPPORTED;
  }

  if (mContext.Starting) {
    if (mContext.ControllerHandle != ControllerHandle) {
      return EFI_ALREADY_STARTED;
    }

    if (mContext.ColdHost.CleanupPending &&
        !mContext.HostPublished && !mContext.PlatformPublished)
    {
      Status = MtkUfsCleanupUnpublishedStart (&mContext);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    } else {
      Status = MtkUfsPublishProviderProtocols (&mContext, ControllerHandle);
      if (EFI_ERROR (Status) &&
          !mContext.HostPublished && !mContext.PlatformPublished)
      {
        EFI_STATUS  CleanupStatus;

        CleanupStatus = MtkUfsCleanupUnpublishedStart (&mContext);
        if (EFI_ERROR (CleanupStatus)) {
          return CleanupStatus;
        }
      }

      return Status;
    }

  }

  if (mContext.Started || mContext.CallbackClosing ||
      (mContext.HostPublished || mContext.PlatformPublished))
  {
    return EFI_ALREADY_STARTED;
  }

  Status = MtkUfsPrepareDmaForStart (&mContext);
  if (mContext.ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* An earlier acquisition/unstarted cleanup may have left only some owned
   * resources on. Finish that serialized cleanup before probing MMIO again. */
  if (mContext.ColdHost.CleanupPending) {
    Status = (mContext.ColdHost.StopPhase == MtkUfsColdStopGateReleasePending) ?
             MtkUfsColdReleaseStartedHost (&mContext.ColdHost) :
             MtkUfsReleaseUnstartedColdHost (&mContext.ColdHost);
    if (mContext.ExitedBootServices) return EFI_UNSUPPORTED;
    if (EFI_ERROR (Status)) return Status;
    mContext.ColdStart = FALSE;
  }

  Status = MtkUfsValidateBoardResources (&Tree, NULL);
  if (mContext.ExitedBootServices) return EFI_UNSUPPORTED;
  if (EFI_ERROR (Status)) {
    return Status;
  }
  mContext.ColdHost.BootServicesExited = &mContext.ExitedBootServices;

  /* HCI registers may be inaccessible with the DT clocks off. The retained
   * probe reads the IP only after proving every clock and VEMC is on; the
   * cold path acquires those resources before its own first IP/HCI read. */
  Status = MtkUfsValidateRetainedHost (Tree, &mContext.ColdHost);
  if (mContext.ExitedBootServices) return EFI_UNSUPPORTED;
  if (Status == EFI_NOT_READY) {
    Status = MtkUfsConfigureColdHost (Tree, &mContext.ColdHost);
    if (mContext.ExitedBootServices) return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    if (MtkUfsColdResourcesOwned () && !mContext.ColdHost.CleanupPending) mEntryQuarantined = TRUE;
    return Status;
  }

  Hce = MmioRead32 ((UINTN)(MTK_UFS_MMIO_BASE + UFS_HC_ENABLE_OFFSET));
  Hcs = MmioRead32 ((UINTN)(MTK_UFS_MMIO_BASE + UFS_HC_STATUS_OFFSET));
  TransferDoorbell = MmioRead32 ((UINTN)(MTK_UFS_MMIO_BASE + UFS_HC_UTRLDBR_OFFSET));
  TaskDoorbell = MmioRead32 ((UINTN)(MTK_UFS_MMIO_BASE + UFS_HC_UTMRLDBR_OFFSET));
  Capabilities = MmioRead32 ((UINTN)(MTK_UFS_MMIO_BASE + UFS_HC_CAP_OFFSET));

  if ((Hce == MAX_UINT32) || (Hcs == MAX_UINT32) ||
      (TransferDoorbell == MAX_UINT32) || (TaskDoorbell == MAX_UINT32) ||
      (Capabilities == MAX_UINT32) || (Capabilities == 0))
  {
    /* An unreadable controller does not prove it is stopped. Preserve newly
     * acquired resources until a later authorized recovery can establish that. */
    if (MtkUfsColdResourcesOwned ()) mEntryQuarantined = TRUE;
    return EFI_DEVICE_ERROR;
  }

  Handoff = MtkUfsAssessHandoff (Hce, Hcs, TransferDoorbell, TaskDoorbell);
  mContext.ColdStart = FALSE;
  if (MtkUfsColdResourcesOwned () &&
      ((Hce != 0U) || (TransferDoorbell != 0U) || (TaskDoorbell != 0U))) {
    /* A previously running host may depend on a resource just restored. Never
     * roll it back or reset the host based solely on a software Idle state. */
    mEntryQuarantined = TRUE;
    return EFI_NOT_READY;
  }
  if (Handoff != MtkUfsHandoffReadyForNop) {
    if ((Hce != 0U) || (TransferDoorbell != 0U) || (TaskDoorbell != 0U)) return EFI_NOT_READY;
    mContext.ColdStart = TRUE;
  }

  mContext.Capabilities     = Capabilities;
  mContext.Supports64BitDma = (BOOLEAN)((Capabilities & UFS_HC_CAP_64ADDR) != 0);
  mContext.HostController.GetUfsHcMmioBar = MtkUfsGetMmioBar;
  mContext.HostController.AllocateBuffer  = MtkUfsAllocateBuffer;
  mContext.HostController.FreeBuffer      = MtkUfsFreeBuffer;
  mContext.HostController.Map             = MtkUfsMap;
  mContext.HostController.Unmap           = MtkUfsUnmap;
  mContext.HostController.Flush           = MtkUfsFlush;
  mContext.HostController.Read            = MtkUfsReadMmio;
  mContext.HostController.Write           = MtkUfsWriteMmio;

  mContext.Platform.Version            = mContext.ColdStart ?
                                         EDKII_UFS_HC_PLATFORM_PROTOCOL_STOP_VERSION :
                                         EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION;
  mContext.Platform.OverrideHcInfo     = NULL;
  mContext.Platform.Callback            = MtkUfsPlatformCallback;
  mContext.Platform.RefClkFreq          = EdkiiUfsCardRefClkFreqObsolete;
  mContext.Platform.SkipHceReenable     = (BOOLEAN)!mContext.ColdStart;
  mContext.Platform.SkipLinkStartup     = (BOOLEAN)!mContext.ColdStart;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  mContext.ControllerHandle = ControllerHandle;
  mContext.Starting         = TRUE;
  mContext.CallbackClosing  = FALSE;
  gBS->RestoreTPL (OldTpl);
  if (mContext.ExitedBootServices) return EFI_UNSUPPORTED;

  Status = MtkUfsPublishProviderProtocols (&mContext, ControllerHandle);
  if (EFI_ERROR (Status) &&
      !mContext.HostPublished && !mContext.PlatformPublished)
  {
    EFI_STATUS  CleanupStatus;

    CleanupStatus = MtkUfsCleanupUnpublishedStart (&mContext);
    if (EFI_ERROR (CleanupStatus)) {
      return CleanupStatus;
    }
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsDriverStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (mContext.ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mContext.ExitedBootServices) {
    Status = EFI_UNSUPPORTED;
  } else if (mEntryQuarantined || (ControllerHandle != mContext.SeedHandle)) {
    Status = EFI_UNSUPPORTED;
  } else if ((RemainingDevicePath != NULL) && !IsDevicePathEnd (RemainingDevicePath)) {
    Status = EFI_UNSUPPORTED;
  } else if (mContext.Started) {
    Status = EFI_ALREADY_STARTED;
  } else if (mContext.StartBusy || mContext.StopBusy) {
    Status = EFI_NOT_READY;
  } else if (mContext.CallbackClosing &&
             !MtkUfsCanReopenClosedProvider (&mContext))
  {
    Status = EFI_NOT_READY;
  } else if (mContext.DmaStopping && !mContext.CallbackClosing) {
    Status = EFI_NOT_READY;
  } else if (mContext.Starting &&
             (ControllerHandle != mContext.ControllerHandle))
  {
    Status = EFI_ALREADY_STARTED;
  } else if ((mContext.HostPublished || mContext.PlatformPublished) &&
             !mContext.Starting)
  {
    Status = EFI_NOT_READY;
  } else {
    mContext.CallbackClosing = FALSE;
    mContext.DmaStopping = FALSE;
    mContext.StartBusy = TRUE;
    Status = EFI_SUCCESS;
  }

  gBS->RestoreTPL (OldTpl);
  if (mContext.ExitedBootServices && !EFI_ERROR (Status)) {
    mContext.StartBusy = FALSE;
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = MtkUfsDriverStartWorker (This, ControllerHandle, RemainingDevicePath);
  if (mContext.ExitedBootServices) {
    mContext.StartBusy = FALSE;
    return EFI_UNSUPPORTED;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (!mContext.ExitedBootServices) {
    mContext.StartBusy = FALSE;
    if (!EFI_ERROR (Status) &&
        mContext.HostPublished && mContext.PlatformPublished)
    {
      mContext.Started           = TRUE;
      mContext.Starting          = FALSE;
      mContext.CallbackClosing   = FALSE;
    } else if (mContext.HostPublished || mContext.PlatformPublished) {
      mContext.Started  = FALSE;
      mContext.Starting = TRUE;
    }

  }
  gBS->RestoreTPL (OldTpl);
  if (mContext.ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  return Status;
}

/* ReadyToBoot runs at TPL_CALLBACK after the UFS driver has produced its
 * descriptor interface. Read only this controller's device descriptor; no
 * vendor guess or boot-time readiness flag supplies the manufacturer delay. */
STATIC
EFI_STATUS
MtkUfsConfigureManufacturer (VOID)
{
  EFI_UFS_DEVICE_CONFIG_PROTOCOL *Device;
  MTK_UFS_DESCRIPTOR_VIEW View;
  EFI_STATUS Status;
  UINT8 Descriptor[255];
  UINT32 Size = sizeof (Descriptor);
  UINT16 Manufacturer;

  if (mEntryQuarantined || mContext.ExitedBootServices || !mContext.Started ||
      !mContext.ColdStart || mContext.ControllerHandle == NULL ||
      !mContext.ColdHost.Configured || mContext.ColdHost.Suspended ||
      mContext.ColdHost.Platform.State != MtkUfsPlatformLinkReady)
    return EFI_NOT_READY;
  if (mContext.ColdHost.RefClockWaitConfigured) return EFI_SUCCESS;
  if (MmioRead32 (MTK_UFS_MMIO_BASE + UFS_HC_UTRLDBR_OFFSET) != 0 ||
      MmioRead32 (MTK_UFS_MMIO_BASE + UFS_HC_UTMRLDBR_OFFSET) != 0)
    return EFI_NOT_READY;
  Device = NULL;
  Status = gBS->HandleProtocol (mContext.ControllerHandle,
                                &gEfiUfsDeviceConfigProtocolGuid, (VOID **)&Device);
  if (mContext.ExitedBootServices) return EFI_UNSUPPORTED;
  if (EFI_ERROR (Status)) return Status;
  if (Device == NULL || Device->RwUfsDescriptor == NULL) return EFI_DEVICE_ERROR;
  ZeroMem (Descriptor, sizeof (Descriptor));
  Status = Device->RwUfsDescriptor (Device, TRUE, 0, 0, 0, Descriptor, &Size);
  if (mContext.ExitedBootServices) return EFI_UNSUPPORTED;
  if (EFI_ERROR (Status)) return Status;
  if (Size > sizeof (Descriptor) ||
      MtkUfsParseDescriptorHeader (Descriptor, Size, &View) != MtkUfsCoreSuccess ||
      View.DescriptorType != 0 || View.DescriptorLength < 0x1AU)
    return EFI_COMPROMISED_DATA;
  Manufacturer = (UINT16)(((UINT16)Descriptor[0x18] << 8) | Descriptor[0x19]);
  return MtkUfsColdHostSetManufacturer (&mContext.ColdHost, Manufacturer);
}

STATIC
VOID
EFIAPI
MtkUfsFdtTableChanged (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;
  BOOLEAN     StartEventCallback;

  (VOID)Event;
  (VOID)Context;
  StartEventCallback = FALSE;

  Status = MtkUfsBeginEventOperation (&mContext);
  if (EFI_ERROR (Status)) {
    return;
  }

  if ((mContext.SeedHandle == NULL) || mEntryQuarantined || mContext.ExitedBootServices) {
    goto Done;
  }
  if (mContext.Started) {
    (VOID)MtkUfsConfigureManufacturer ();
    goto Done;
  }

  // Protocol-table installation can notify this event before BootStatus has
  // published its copied FDT pointer and success status. EndOfDxe retries
  // before BDS connects storage; ReadyToBoot covers a later publication. Call only this
  // driver's binding methods here; ConnectController would also try every
  // other binding on the synthetic seed after this driver succeeds.
  Status = mDriverBinding.Supported (&mDriverBinding, mContext.SeedHandle, NULL);
  if ((Status != EFI_SUCCESS) || mContext.ExitedBootServices) {
    goto Done;
  }

  Status = MtkUfsBeginEventStart (&mContext);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  StartEventCallback = TRUE;
  Status = mDriverBinding.Start (&mDriverBinding, mContext.SeedHandle, NULL);
  MtkUfsEndEventStart (&mContext);
  StartEventCallback = FALSE;
  if (mContext.ExitedBootServices) {
    goto Done;
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "MTK UFS runtime handoff did not start: %r\n", Status));
  }

Done:
  if (StartEventCallback) {
    MtkUfsEndEventStart (&mContext);
  }
  MtkUfsEndProviderCallback (&mContext);
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsDriverStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer OPTIONAL
  )
{
  EFI_STATUS  Status;
  EFI_TPL     OldTpl;
  BOOLEAN     WasCallbackClosing;
  BOOLEAN     ClaimedStop;
  BOOLEAN     ColdStopRequired;
  BOOLEAN     ColdStopProven;
  BOOLEAN     ColdUnstartedReleaseRequired;
  EFI_HANDLE  PublishedHandle;

  if ((This == NULL) || mContext.ExitedBootServices) {
    return EFI_UNSUPPORTED;
  }

  if (NumberOfChildren != 0) {
    return EFI_INVALID_PARAMETER;
  }

  (VOID)ChildHandleBuffer;

  ClaimedStop = FALSE;
  PublishedHandle = NULL;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mContext.ExitedBootServices) {
    Status = EFI_UNSUPPORTED;
  } else if (mContext.StartBusy || mContext.StopBusy) {
    Status = EFI_NOT_READY;
  } else if (((ControllerHandle != mContext.ControllerHandle) &&
              !(mEntryQuarantined && (ControllerHandle == mContext.SeedHandle))) ||
             !(mContext.Started || mContext.Starting ||
               mContext.HostPublished || mContext.PlatformPublished))
  {
    Status = EFI_UNSUPPORTED;
  } else {
    PublishedHandle = mContext.ControllerHandle;
    mContext.StopBusy = TRUE;
    ClaimedStop = TRUE;
    Status = EFI_SUCCESS;
  }
  WasCallbackClosing = mContext.CallbackClosing;
  gBS->RestoreTPL (OldTpl);
  if (mContext.ExitedBootServices) {
    if (ClaimedStop) {
      mContext.StopBusy = FALSE;
    }
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ColdStopProven = (BOOLEAN)(mContext.ColdHost.StopProofValid &&
      mContext.ColdHost.StopGeneration != 0 &&
      mContext.ColdHost.StopProofGeneration == mContext.ColdHost.StopGeneration &&
      (mContext.ColdHost.StopPhase == MtkUfsColdStopHceProven ||
       mContext.ColdHost.StopPhase == MtkUfsColdStopGateReleasePending));
  ColdStopRequired = (BOOLEAN)(mContext.ColdStart && mContext.ColdHost.Configured &&
      mContext.ColdHost.Platform.State != MtkUfsPlatformIdle &&
      mContext.ColdHost.StopPhase != MtkUfsColdStopReleasedNeedsInit);
  ColdUnstartedReleaseRequired = (BOOLEAN)(MtkUfsColdResourcesOwned () &&
      mContext.ColdHost.Configured && mContext.ColdHost.Platform.State == MtkUfsPlatformIdle);
  /* Retain VEMC enable history/policy because MMC shares it. A cold host may
   * release only its own UFS gates, and only after the v4 stop proof. */
  if ((ColdStopRequired ||
       (MtkUfsColdResourcesOwned () && !ColdUnstartedReleaseRequired)) && !ColdStopProven) {
    MtkUfsEndDriverStop (&mContext);
    return EFI_NOT_READY;
  }

  Status = MtkUfsBeginDmaCleanup (&mContext, FALSE);
  if (EFI_ERROR (Status)) {
    MtkUfsEndDriverStop (&mContext);
    return Status;
  }

  Status = MtkUfsProbePublishedProtocols (&mContext, PublishedHandle);
  if (mContext.ExitedBootServices) {
    MtkUfsEndDriverStop (&mContext);
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    MtkUfsEndDmaCleanup (&mContext, TRUE, TRUE);
    MtkUfsEndDriverStop (&mContext);
    return Status;
  }

  Status = MtkUfsReclaimUnexposedAllocations (&mContext);
  if (mContext.ExitedBootServices) {
    MtkUfsEndDriverStop (&mContext);
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    MtkUfsEndDmaCleanup (&mContext, WasCallbackClosing, WasCallbackClosing);
    MtkUfsEndDriverStop (&mContext);
    return Status;
  }

  if (!IsListEmpty (&mContext.Mappings) ||
      !IsListEmpty (&mContext.CommonAllocations) ||
      (mContext.ActiveDmaOperations != 0) ||
      (mContext.ActiveCallbacks != 0))
  {
    MtkUfsEndDmaCleanup (&mContext, WasCallbackClosing, WasCallbackClosing);
    MtkUfsEndDriverStop (&mContext);
    return EFI_NOT_READY;
  }

  Status = MtkUfsUninstallPublishedProtocols (&mContext, PublishedHandle);
  if (mContext.ExitedBootServices) {
    MtkUfsEndDriverStop (&mContext);
    return EFI_UNSUPPORTED;
  }
  if (EFI_ERROR (Status)) {
    MtkUfsEndDmaCleanup (&mContext, TRUE, TRUE);
    MtkUfsEndDriverStop (&mContext);
    return Status;
  }

  /* The consumer has closed its host-protocol reference, and both provider
   * protocols are now absent. Only at this point may the provider release its
   * exclusive UFS gates. VEMC is deliberately retained for the MMC user. */
  if (ColdStopProven) {
    if (mContext.ExitedBootServices) {
      MtkUfsEndDriverStop (&mContext);
      return EFI_UNSUPPORTED;
    }
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    if (mContext.ExitedBootServices) {
      Status = EFI_UNSUPPORTED;
    } else {
      Status = MtkUfsColdReleaseStartedHost (&mContext.ColdHost);
    }
    gBS->RestoreTPL (OldTpl);
    if (mContext.ExitedBootServices) {
      MtkUfsEndDriverStop (&mContext);
      return EFI_UNSUPPORTED;
    }
    if (EFI_ERROR (Status)) {
      MtkUfsEndDmaCleanup (&mContext, TRUE, TRUE);
      MtkUfsEndDriverStop (&mContext);
      return Status;
    }
  } else if (ColdUnstartedReleaseRequired ||
             (mContext.ColdHost.CleanupPending &&
              mContext.ColdHost.StopPhase == MtkUfsColdStopNone)) {
    Status = MtkUfsReleaseUnstartedColdHost (&mContext.ColdHost);
    if (mContext.ExitedBootServices) {
      MtkUfsEndDriverStop (&mContext);
      return EFI_UNSUPPORTED;
    }
    if (EFI_ERROR (Status)) {
      MtkUfsEndDmaCleanup (&mContext, TRUE, TRUE);
      MtkUfsEndDriverStop (&mContext);
      return Status;
    }
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  mContext.Starting          = FALSE;
  mContext.Started           = FALSE;
  mContext.HostPublished     = FALSE;
  mContext.PlatformPublished = FALSE;
  mContext.ControllerHandle = NULL;
  gBS->RestoreTPL (OldTpl);
  MtkUfsEndDmaCleanup (&mContext, TRUE, TRUE);
  MtkUfsEndDriverStop (&mContext);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
MtkUfsWaitStopped (
  IN BOOLEAN HostDisabled
  )
{
  UINTN Elapsed;
  UINT32 Hce;
  for (Elapsed = 0; ; Elapsed += 100U) {
    Hce = MmioRead32 ((UINTN)(MTK_UFS_MMIO_BASE + UFS_HC_ENABLE_OFFSET));
    if (Hce == MAX_UINT32) return EFI_DEVICE_ERROR;
    if (HostDisabled) {
      if ((Hce & 1U) == 0U) return EFI_SUCCESS;
    } else {
      UINT32 Transfer = MmioRead32 ((UINTN)(MTK_UFS_MMIO_BASE + UFS_HC_UTRLDBR_OFFSET));
      UINT32 Task = MmioRead32 ((UINTN)(MTK_UFS_MMIO_BASE + UFS_HC_UTMRLDBR_OFFSET));
      if (Transfer == MAX_UINT32 || Task == MAX_UINT32) return EFI_DEVICE_ERROR;
      if ((Transfer | Task) == 0U) return EFI_SUCCESS;
    }
    if (Elapsed == 100000U) return EFI_TIMEOUT;
    MicroSecondDelay (100U);
  }
}

/* OS identity is stricter than the UEFI polled path: translated IRQ 169
 * must come from the original four-cell GICv3 specifier and direct DMA map. */
STATIC
BOOLEAN
MtkUfsOsResourcesReady (VOID)
{
  CONST VOID *Tree;
  CONST UINT8 *P;
  INT32 Node, Parent, Gic, Length;
  MTK_UFS_COLD_HOST Probe;
  if (EFI_ERROR (MtkUfsValidateBoardResources (&Tree, NULL))) return FALSE;
  Node = FdtPathOffset (Tree, "/soc/ufshci@11270000");
  P = FdtGetProp (Tree, Node, "interrupts", &Length);
  if (P == NULL || Length != 16 || MtkUfsReadFdtCells(P,1) != 0 ||
      MtkUfsReadFdtCells(P+4,1) != 137 || MtkUfsReadFdtCells(P+8,1) != 4 ||
      MtkUfsReadFdtCells(P+12,1) != 0) return FALSE;
  Gic = FdtPathOffset (Tree, "/interrupt-controller");
  if (Gic < 0 || !MtkUfsFdtNodeEnabled(Tree,Gic) || !MtkUfsFdtCompatible(Tree,Gic,"arm,gic-v3")) return FALSE;
  P = FdtGetProp(Tree,Gic,"#interrupt-cells",&Length);
  if (P == NULL || Length != 4 || MtkUfsReadFdtCells(P,1) != 4 ||
      FdtGetProp(Tree,Gic,"interrupt-controller",&Length) == NULL || Length != 0) return FALSE;
  for (Parent=Node; Parent>=0; Parent=FdtParentOffset(Tree,Parent)) {
    if (FdtGetProp(Tree,Parent,"interrupt-map",&Length) != NULL ||
        FdtGetProp(Tree,Parent,"interrupts-extended",&Length) != NULL) return FALSE;
    P=FdtGetProp(Tree,Parent,"interrupt-parent",&Length);
    if (P != NULL && (Length != 4 || FdtNodeOffsetByPhandle(Tree,(UINT32)MtkUfsReadFdtCells(P,1)) != Gic)) return FALSE;
  }
  P=FdtGetProp(Tree,0,"interrupt-parent",&Length);
  if (P == NULL || Length != 4 || FdtNodeOffsetByPhandle(Tree,(UINT32)MtkUfsReadFdtCells(P,1)) != Gic) return FALSE;
  /* This function only queries the live clock gates, VCC, mappings and IP;
   * it does not invoke the power/reset/SMC callbacks it prepares in Probe. */
  ZeroMem (&Probe, sizeof (Probe));
  Probe.BootServicesExited = &mContext.ExitedBootServices;
  if (EFI_ERROR(MtkUfsValidateRetainedHost(Tree,&Probe))) return FALSE;
  return TRUE;
}

STATIC VOID MtkUfsPublishOsReady (BOOLEAN Ready)
{
  MmioWrite32 (MTK_UFS_BOOT_HANDOFF_BASE+0xb0U,0);
  MemoryFence();
  MmioWrite32 (MTK_UFS_BOOT_HANDOFF_BASE+0xb4U,MTK_UFS_MMIO_BASE);
  MmioWrite32 (MTK_UFS_BOOT_HANDOFF_BASE+0xb8U,MTK_UFS_USER_LUN);
  MmioWrite32 (MTK_UFS_BOOT_HANDOFF_BASE+0xbcU,1);
  MemoryFence();
  if (Ready) MmioWrite32 (MTK_UFS_BOOT_HANDOFF_BASE+0xb0U,1);
  /* The reserved handoff page is WB in firmware. Publish to PoC before the
   * OS/AML reads the physical page through an uncached device mapping. */
  WriteBackDataCacheRange ((VOID *)(UINTN)(MTK_UFS_BOOT_HANDOFF_BASE+0xb0U),16);
  MemoryFence();
}

STATIC
VOID
EFIAPI
MtkUfsExitBootServices (
  IN EFI_EVENT Event,
  IN VOID *Context
  )
{
  EFI_STATUS Status;
  UINT32 Hcs, Version, Is;
  BOOLEAN Ready = FALSE;
  BOOLEAN WasStopBusy;
  CONST VOID *Tree;
  MTK_UFS_COLD_HOST Probe;
  (VOID)Event; (VOID)Context;
  mContext.ColdHost.IoClosed = TRUE;
  if (mContext.ExitedBootServices) {
    /* ExitBootServices may be delivered again by firmware. Only revoke the
     * ready bit; do not touch the HCI or call Boot Services a second time. */
    MmioWrite32 (MTK_UFS_BOOT_HANDOFF_BASE + 0xb0U, 0U);
    MemoryFence ();
    WriteBackDataCacheRange ((VOID *)(UINTN)(MTK_UFS_BOOT_HANDOFF_BASE + 0xb0U), 16U);
    MemoryFence ();
    return;
  }
  Status = EFI_SUCCESS;
  {
    EFI_TPL  OldTpl;

    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    WasStopBusy = mContext.StopBusy;
    mContext.CallbackClosing = TRUE;
    mContext.StopBusy = TRUE;
    if ((mContext.ActiveCallbacks != 0) || mContext.StartBusy ||
        WasStopBusy || mContext.DmaCleanupActive)
    {
      Status = EFI_NOT_READY;
    }
    gBS->RestoreTPL (OldTpl);
  }
  if (mContext.ExitedBootServices) {
    return;
  }

  MtkUfsPublishOsReady(FALSE);
  if (mContext.ExitedBootServices) {
    return;
  }
  if (EFI_ERROR (Status)) {
    mContext.QuiesceStatus = Status;
    goto Done;
  }
  if (!mContext.Started || mEntryQuarantined) goto Done;
  /* Do not access a clock-gated controller or publish a suspended/partially
   * released link. IRQ/OS mapping checks remain separate from this power proof. */
  if (mContext.ColdHost.Suspended || mContext.ColdHost.CleanupPending) {
    mContext.QuiesceStatus = EFI_NOT_READY;
    goto Done;
  }
  Status = MtkUfsValidateBoardResources (&Tree, NULL);
  if (EFI_ERROR (Status)) {
    mContext.QuiesceStatus = Status;
    goto Done;
  }
  ZeroMem (&Probe, sizeof (Probe));
  Probe.BootServicesExited = &mContext.ExitedBootServices;
  Status = MtkUfsValidateRetainedHost (Tree, &Probe);
  if (EFI_ERROR (Status)) {
    mContext.QuiesceStatus = Status;
    goto Done;
  }
  /* First block submissions and drain. Stopped lists + zero old base addresses
   * permit safe OS memory reuse while preserving the established UFS link.
   * If any live gate fails, fall back to the original HCE-off path. */
  mContext.QuiesceStatus = MtkUfsWaitStopped (FALSE);
  MmioWrite32 ((UINTN)(MTK_UFS_MMIO_BASE + UFS_HC_IE_OFFSET), 0U);
  MmioWrite32 ((UINTN)(MTK_UFS_MMIO_BASE + UFS_HC_UTRLRSR_OFFSET), 0U);
  MmioWrite32 ((UINTN)(MTK_UFS_MMIO_BASE + UFS_HC_UTMRLRSR_OFFSET), 0U);
  MemoryFence ();
  Hcs = MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_STATUS_OFFSET);
  Version = MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_VER_OFFSET);
  Is = MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_IS_OFFSET);
  if (!EFI_ERROR(mContext.QuiesceStatus) && Hcs != MAX_UINT32 && (Hcs&15U) == 15U &&
      (Version == 0x200U || Version == 0x210U) && !(Is&MTK_UFS_ERROR_INTERRUPTS) &&
      MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_ENABLE_OFFSET) == 1 &&
      MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_IE_OFFSET) == 0 &&
      MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_UTRLRSR_OFFSET) == 0 &&
      MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_UTMRLRSR_OFFSET) == 0 &&
      MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_UTRLDBR_OFFSET) == 0 &&
      MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_UTMRLDBR_OFFSET) == 0 &&
      MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_AHIT_OFFSET) == 0 && MtkUfsOsResourcesReady()) {
    MmioWrite32(MTK_UFS_MMIO_BASE+UFS_HC_UTRLBA_OFFSET,0);
    MmioWrite32(MTK_UFS_MMIO_BASE+UFS_HC_UTRLBAU_OFFSET,0);
    MmioWrite32(MTK_UFS_MMIO_BASE+UFS_HC_UTMRLBA_OFFSET,0);
    MmioWrite32(MTK_UFS_MMIO_BASE+UFS_HC_UTMRLBAU_OFFSET,0);
    MmioWrite32(MTK_UFS_MMIO_BASE+UFS_HC_IS_OFFSET,MTK_UFS_INTERRUPTS);
    MemoryFence();
    Ready = (BOOLEAN)(MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_UTRLBA_OFFSET) == 0 &&
                      MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_UTRLBAU_OFFSET) == 0 &&
                      MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_UTMRLBA_OFFSET) == 0 &&
                      MmioRead32(MTK_UFS_MMIO_BASE+UFS_HC_UTMRLBAU_OFFSET) == 0);
  }
  if (mContext.ExitedBootServices) return;
  if (!Ready) {
    MmioWrite32 ((UINTN)(MTK_UFS_MMIO_BASE + UFS_HC_ENABLE_OFFSET), 0U);
    MemoryFence ();
    Status = MtkUfsWaitStopped (TRUE);
    if (EFI_ERROR (Status)) mContext.QuiesceStatus = Status;
  } else MtkUfsPublishOsReady(TRUE);
  DEBUG ((EFI_ERROR (mContext.QuiesceStatus) ? DEBUG_ERROR : DEBUG_INFO,
          "MTK UFS OS handoff quiesce: %r, link retained: %d\n", mContext.QuiesceStatus, Ready));

Done:
  /* This is the final event action. No Boot Services or cache maintenance is
   * used after ExitBootServices becomes visible to provider callbacks. */
  mContext.ExitedBootServices = TRUE;
}

STATIC
EFI_STATUS
MtkUfsRollbackEntryPoint (
  IN EFI_STATUS  FailureStatus,
  IN EFI_HANDLE  DriverHandle
  )
{
  EFI_STATUS  Status;
  BOOLEAN     CleanupFailed;
  BOOLEAN     ProviderMayExist;
  EFI_HANDLE  ProviderHandle;

  // Any object that could retain a callback or protocol pointer must either
  // be removed successfully or keep this image resident in a quarantined state.
  mEntryQuarantined = TRUE;
  CleanupFailed = FALSE;

  ProviderHandle = mContext.ControllerHandle;
  ProviderMayExist = (BOOLEAN)(ProviderHandle != NULL ||
                               mContext.Starting || mContext.StartBusy ||
                               mContext.StopBusy || mContext.Started ||
                               mContext.HostPublished || mContext.PlatformPublished ||
                               mContext.ActiveCallbacks != 0 ||
                               mContext.ActiveDmaOperations != 0 ||
                               mContext.DmaCleanupActive ||
                               !IsListEmpty (&mContext.Mappings) ||
                               !IsListEmpty (&mContext.CommonAllocations));
  if (ProviderHandle != NULL) {
    if (mContext.ExitedBootServices ||
        EFI_ERROR (MtkUfsProbePublishedProtocols (&mContext, ProviderHandle)))
    {
      ProviderMayExist = TRUE;
    } else if (mContext.HostPublished || mContext.PlatformPublished) {
      ProviderMayExist = TRUE;
    }
  }

  if (ProviderMayExist) {
    if ((ProviderHandle == NULL) || mContext.ExitedBootServices) {
      DEBUG ((DEBUG_ERROR, "MTK UFS entry rollback retained an unprovable provider owner\n"));
      return EFI_SUCCESS;
    }

    Status = MtkUfsDriverStop (&mDriverBinding, ProviderHandle, 0, NULL);
    if (EFI_ERROR (Status) || mContext.ControllerHandle != NULL ||
        mContext.Starting || mContext.StartBusy || mContext.StopBusy ||
        mContext.Started || mContext.HostPublished || mContext.PlatformPublished ||
        (mContext.ActiveCallbacks != 0) || (mContext.ActiveDmaOperations != 0) ||
        mContext.DmaCleanupActive || !IsListEmpty (&mContext.Mappings) ||
        !IsListEmpty (&mContext.CommonAllocations))
    {
      DEBUG ((DEBUG_ERROR, "MTK UFS entry rollback could not drain provider; image retained (%r)\n",
              Status));
      return EFI_SUCCESS;
    }
  }

  if (mContext.SeedHandle != NULL) {
    Status = gBS->UninstallProtocolInterface (
                    mContext.SeedHandle,
                    &gEfiDevicePathProtocolGuid,
                    &mSeedDevicePath
                    );
    if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
      CleanupFailed = TRUE;
    } else {
      mContext.SeedHandle = NULL;
    }
  }

  Status = gBS->UninstallProtocolInterface (
                  DriverHandle,
                  &gEfiDriverBindingProtocolGuid,
                  &mDriverBinding
                  );
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    CleanupFailed = TRUE;
  }

  if (mFdtEvent != NULL) {
    Status = gBS->CloseEvent (mFdtEvent);
    if (EFI_ERROR (Status)) {
      CleanupFailed = TRUE;
    } else {
      mFdtEvent = NULL;
    }
  }

  if (mEndOfDxeEvent != NULL) {
    Status = gBS->CloseEvent (mEndOfDxeEvent);
    if (EFI_ERROR (Status)) {
      CleanupFailed = TRUE;
    } else {
      mEndOfDxeEvent = NULL;
    }
  }

  if (mReadyToBootEvent != NULL) {
    Status = gBS->CloseEvent (mReadyToBootEvent);
    if (EFI_ERROR (Status)) {
      CleanupFailed = TRUE;
    } else {
      mReadyToBootEvent = NULL;
    }
  }

  if (mExitBootServicesEvent != NULL) {
    Status = gBS->CloseEvent (mExitBootServicesEvent);
    if (EFI_ERROR (Status)) CleanupFailed = TRUE;
    else mExitBootServicesEvent = NULL;
  }

  if (CleanupFailed) {
    DEBUG ((DEBUG_ERROR, "MTK UFS entry rollback incomplete; image kept resident and quarantined\n"));
    return EFI_SUCCESS;
  }

  mEntryQuarantined = FALSE;
  return FailureStatus;
}

EFI_STATUS
EFIAPI
MtkUfsHandoffEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  DriverHandle;

  ZeroMem (&mContext, sizeof (mContext));
  mEntryQuarantined = FALSE;
  (VOID)SystemTable;
  mContext.Signature = MTK_UFS_CONTEXT_SIGNATURE;
  InitializeListHead (&mContext.CommonAllocations);
  InitializeListHead (&mContext.Mappings);

  ZeroMem (&mSeedDevicePath, sizeof (mSeedDevicePath));
  mSeedDevicePath.Vendor.Header.Type = HARDWARE_DEVICE_PATH;
  mSeedDevicePath.Vendor.Header.SubType = HW_VENDOR_DP;
  SetDevicePathNodeLength (&mSeedDevicePath.Vendor, sizeof (VENDOR_DEVICE_PATH));
  CopyGuid (&mSeedDevicePath.Vendor.Guid, &mMtkUfsSeedGuid);
  SetDevicePathEndNode (&mSeedDevicePath.End);

  mDriverBinding.Supported = MtkUfsDriverSupported;
  mDriverBinding.Start = MtkUfsDriverStart;
  mDriverBinding.Stop = MtkUfsDriverStop;
  mDriverBinding.Version = 0xFFFFFFF0;
  mDriverBinding.ImageHandle = ImageHandle;
  mDriverBinding.DriverBindingHandle = ImageHandle;

  DriverHandle = ImageHandle;
  mFdtEvent = NULL;
  mEndOfDxeEvent = NULL;
  mReadyToBootEvent = NULL;
  mExitBootServicesEvent = NULL;
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  MtkUfsFdtTableChanged,
                  NULL,
                  &gFdtTableGuid,
                  &mFdtEvent
                  );
  if (EFI_ERROR (Status)) {
    // Failed creation does not transfer ownership of the output handle.
    mFdtEvent = NULL;
    return MtkUfsRollbackEntryPoint (Status, DriverHandle);
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  MtkUfsFdtTableChanged,
                  NULL,
                  &gEfiEndOfDxeEventGroupGuid,
                  &mEndOfDxeEvent
                  );
  if (EFI_ERROR (Status)) {
    // Failed creation does not transfer ownership of the output handle.
    mEndOfDxeEvent = NULL;
    return MtkUfsRollbackEntryPoint (Status, DriverHandle);
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  MtkUfsFdtTableChanged,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &mReadyToBootEvent
                  );
  if (EFI_ERROR (Status)) {
    // Failed creation does not transfer ownership of the output handle.
    mReadyToBootEvent = NULL;
    return MtkUfsRollbackEntryPoint (Status, DriverHandle);
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  MtkUfsExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    // Failed creation does not transfer ownership of the output handle.
    mExitBootServicesEvent = NULL;
    return MtkUfsRollbackEntryPoint (Status, DriverHandle);
  }

  Status = gBS->InstallProtocolInterface (
                  &DriverHandle,
                  &gEfiDriverBindingProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mDriverBinding
                  );
  if (EFI_ERROR (Status)) {
    return MtkUfsRollbackEntryPoint (Status, DriverHandle);
  }

  Status = gBS->InstallProtocolInterface (
                  &mContext.SeedHandle,
                  &gEfiDevicePathProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mSeedDevicePath
                  );
  if (EFI_ERROR (Status)) {
    return MtkUfsRollbackEntryPoint (Status, DriverHandle);
  }

  MtkUfsFdtTableChanged (NULL, NULL);
  return EFI_SUCCESS;
}
