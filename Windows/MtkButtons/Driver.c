/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <wdf.h>
#include "ButtonsCore.h"
#include "../MtkMali/PmicPrm.h"

#define BUTTONS_KPD_PHYSICAL 0x10010000ULL
#define BUTTONS_KPD_LENGTH   0x1000U
#define BUTTONS_POLL_MS      20U
#define BUTTONS_WRITE_TIMEOUT_MS 1000U

EVT_WDF_DRIVER_DEVICE_ADD ButtonsDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE ButtonsPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE ButtonsReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY ButtonsD0Entry;
EVT_WDF_DEVICE_D0_EXIT ButtonsD0Exit;
EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED ButtonsD0ExitPreInterruptsDisabled;
EVT_WDF_TIMER ButtonsTimer;
EVT_WDF_WORKITEM ButtonsWork;
EVT_WDF_INTERRUPT_ISR ButtonsInterruptIsr;
EVT_WDF_INTERRUPT_DPC ButtonsInterruptDpc;
EVT_WDF_INTERRUPT_ENABLE ButtonsInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE ButtonsInterruptDisable;

typedef struct {
  WDFDEVICE Device;
  WDFIOTARGET NotifyTarget;
  WDFWORKITEM Work;
  WDFTIMER Timer;
  WDFINTERRUPT Interrupt;
  PUCHAR Registers;
  MTK_BUTTONS_CORE Core;
  volatile LONG Active;
  volatile LONG WorkQueued;
  volatile LONG SamplePending;
  volatile LONG Quarantined;
  ULONG TranslatedInterruptVector;
  ULONG PmicMode;
  uint64_t KeyIrqLeaseToken;
  MTK_PMIC_KEY_IRQ_SNAPSHOT KeyIrqSnapshot;
  MTK_BUTTON_KPD_SAMPLE KeyIrqKpdSample;
  BOOLEAN InterruptResourcePresent;
  BOOLEAN Prepared;
  BOOLEAN NotifyOpen;
  BOOLEAN KeyIrqLeaseHeld;
  BOOLEAN KeyIrqSnapshotPending;
  BOOLEAN KeyIrqSnapshotSampled;
  BOOLEAN PmicClientFaulted;
} BUTTONS_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BUTTONS_DEVICE, ButtonsContext)

enum {
  ButtonsPmicModeUnknown = 0,
  ButtonsPmicModeLegacy = 1,
  ButtonsPmicModeKeyIrqV2 = 2
};

/* GPIO buttons inbox notify interface; its write payload is a 32-bit enum. */
static const GUID ButtonsNotifyInterfaceGuid = {
  0x30ebfbf8, 0xdf5f, 0x4d4d,
  { 0x9f, 0xc5, 0xa2, 0x6c, 0x7f, 0xd1, 0xdf, 0x4a }
};

static void ButtonsQuarantine(BUTTONS_DEVICE *D) {
  InterlockedExchange(&D->Quarantined, 1);
  InterlockedExchange(&D->Active, 0);
  if (D->Timer) WdfTimerStop(D->Timer, FALSE);
}

static void ButtonsQueueWork(BUTTONS_DEVICE *D) {
  if (InterlockedCompareExchange(&D->Active, 0, 0) == 0 ||
      InterlockedCompareExchange(&D->Quarantined, 0, 0) != 0) {
    return;
  }
  InterlockedExchange(&D->SamplePending, 1);
  if (InterlockedCompareExchange(&D->WorkQueued, 1, 0) == 0) {
    /* Recheck admission; D0 exit waits for every producer before the worker. */
    if (InterlockedCompareExchange(&D->Active, 0, 0) == 0 ||
        InterlockedCompareExchange(&D->Quarantined, 0, 0) != 0) {
      InterlockedExchange(&D->WorkQueued, 0);
      return;
    }
    WdfWorkItemEnqueue(D->Work);
  }
}

BOOLEAN ButtonsInterruptIsr(WDFINTERRUPT Interrupt, ULONG MessageId) {
  BUTTONS_DEVICE *D = ButtonsContext(WdfInterruptGetDevice(Interrupt));
  UNREFERENCED_PARAMETER(MessageId);

  if (D->Prepared == FALSE || D->Registers == NULL ||
      InterlockedCompareExchange(&D->Active, 0, 0) == 0 ||
      InterlockedCompareExchange(&D->Quarantined, 0, 0) != 0) {
    return FALSE;
  }

  /* Edge IRQ: no device-side ACK register is documented by the stock driver. */
  InterlockedExchange(&D->SamplePending, 1);
  (void)WdfInterruptQueueDpcForIsr(Interrupt);
  return TRUE;
}

VOID ButtonsInterruptDpc(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject) {
  BUTTONS_DEVICE *D = ButtonsContext((WDFDEVICE)AssociatedObject);
  UNREFERENCED_PARAMETER(Interrupt);

  if (D->Prepared && D->Registers != NULL &&
      InterlockedCompareExchange(&D->Active, 0, 0) != 0 &&
      InterlockedCompareExchange(&D->Quarantined, 0, 0) == 0) {
    ButtonsQueueWork(D);
  }
}

NTSTATUS ButtonsInterruptEnable(WDFINTERRUPT Interrupt, WDFDEVICE Device) {
  BUTTONS_DEVICE *D = ButtonsContext(Device);
  UNREFERENCED_PARAMETER(Interrupt);

  if (!D->Prepared || D->Registers == NULL ||
      InterlockedCompareExchange(&D->Active, 0, 0) == 0 ||
      InterlockedCompareExchange(&D->Quarantined, 0, 0) != 0) {
    return STATUS_DEVICE_NOT_READY;
  }
  return STATUS_SUCCESS;
}

NTSTATUS ButtonsInterruptDisable(WDFINTERRUPT Interrupt, WDFDEVICE Device) {
  UNREFERENCED_PARAMETER(Interrupt);
  UNREFERENCED_PARAMETER(Device);
  /* No device-side IRQ mask is documented; WDF owns D0 interrupt state. */
  return STATUS_SUCCESS;
}

VOID ButtonsTimer(WDFTIMER Timer) {
  BUTTONS_DEVICE *D = ButtonsContext(WdfTimerGetParentObject(Timer));
  ButtonsQueueWork(D);
}

static NTSTATUS ButtonsOpenNotifyTarget(BUTTONS_DEVICE *D) {
  NTSTATUS Status;
  PWSTR InterfaceList = NULL;
  ULONG Length = 0;
  UNICODE_STRING Name;
  WDF_IO_TARGET_OPEN_PARAMS Open;

  if (D->NotifyOpen) return STATUS_SUCCESS;
  Status = IoGetDeviceInterfaces((LPGUID)&ButtonsNotifyInterfaceGuid, NULL, 0,
                                 &InterfaceList);
  if (Status != STATUS_SUCCESS) {
    if (InterfaceList) ExFreePool(InterfaceList);
    return Status == STATUS_NOT_FOUND ? STATUS_DEVICE_NOT_READY : Status;
  }
  if (InterfaceList == NULL || InterfaceList[0] == L'\0') {
    if (InterfaceList) ExFreePool(InterfaceList);
    return STATUS_DEVICE_NOT_READY;
  }

  while (Length < 1024U && InterfaceList[Length] != L'\0') ++Length;
  /* Exactly one array endpoint is expected; never guess among duplicates. */
  if (Length == 0 || Length >= 1024U || InterfaceList[Length + 1U] != L'\0') {
    ExFreePool(InterfaceList);
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }

  Name.Buffer = InterfaceList;
  Name.Length = (USHORT)(Length * sizeof(WCHAR));
  Name.MaximumLength = Name.Length;
  WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Open, &Name, FILE_WRITE_DATA);
  Open.ShareAccess = 0;
  Open.FileAttributes = FILE_ATTRIBUTE_NORMAL;
  Status = WdfIoTargetOpen(D->NotifyTarget, &Open);
  ExFreePool(InterfaceList);
  if (Status == STATUS_SUCCESS) D->NotifyOpen = TRUE;
  else if (Status > STATUS_SUCCESS) WdfIoTargetClose(D->NotifyTarget);
  return Status;
}

static NTSTATUS ButtonsDrainActions(BUTTONS_DEVICE *D) {
  MTK_BUTTON_ACTION Action;
  MTK_BUTTONS_CORE_STATUS CoreStatus;
  NTSTATUS Status;
  ULONG_PTR BytesWritten;

  if (D->Core.ActionCount == 0) return STATUS_SUCCESS;
  if (!D->NotifyOpen) {
    Status = ButtonsOpenNotifyTarget(D);
    if (Status != STATUS_SUCCESS) {
      if (Status == STATUS_DEVICE_CONFIGURATION_ERROR) ButtonsQuarantine(D);
      return Status;
    }
  }

  for (;;) {
    CoreStatus = MtkButtonsCorePeekAction(&D->Core, &Action);
    if (CoreStatus == MtkButtonsCoreNotReady) return STATUS_SUCCESS;
    if (CoreStatus != MtkButtonsCoreSuccess) {
      ButtonsQuarantine(D);
      return STATUS_DEVICE_HARDWARE_ERROR;
    }

    /* The in-box interface consumes GPIO_BUTTONS_BUTTON_TYPE, a ULONG enum. */
    {
      ULONG Button = (ULONG)Action.Button;
      WDF_MEMORY_DESCRIPTOR Buffer;
      WDF_REQUEST_SEND_OPTIONS Options;
      BytesWritten = 0;
      WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Buffer, &Button, sizeof(Button));
      WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
      WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
        &Options, WDF_REL_TIMEOUT_IN_MS(BUTTONS_WRITE_TIMEOUT_MS));
      Status = WdfIoTargetSendWriteSynchronously(
        D->NotifyTarget, NULL, &Buffer, NULL, &Options, &BytesWritten);
      CoreStatus = MtkButtonsCoreCompleteAction(
        &D->Core, &Action,
        (uint8_t)(Status == STATUS_SUCCESS && BytesWritten == sizeof(Button)));
    }

    if (Status != STATUS_SUCCESS || BytesWritten != sizeof(ULONG) ||
        CoreStatus != MtkButtonsCoreSuccess) {
      /* A failed toggle may have reached the inbox; replay could undo it. */
      ButtonsQuarantine(D);
      return NT_SUCCESS(Status) ? STATUS_DEVICE_DATA_ERROR : Status;
    }
  }
}

static NTSTATUS ButtonsPmicFault(BUTTONS_DEVICE *D) {
  D->PmicClientFaulted = TRUE;
  ButtonsQuarantine(D);
  return STATUS_DEVICE_HARDWARE_ERROR;
}

/* Query is the only route to v1 polling. Once v2 is accepted, an error cannot
   silently switch back to a lossy level-only read. */
static NTSTATUS ButtonsEnsurePmicPath(BUTTONS_DEVICE *D) {
  MALI_RESULT PmicStatus;
  uint64_t LeaseToken;

  if (D->PmicMode == ButtonsPmicModeUnknown) {
    PmicStatus = MtkMaliPmicPrmQueryKeyIrq();
    if (PmicStatus == MaliUnsupported) {
      D->PmicMode = ButtonsPmicModeLegacy;
      return STATUS_SUCCESS;
    }
    if (PmicStatus == MaliBusy || PmicStatus == MaliOffline) {
      return STATUS_DEVICE_NOT_READY;
    }
    if (PmicStatus != MaliOk) return ButtonsPmicFault(D);
    D->PmicMode = ButtonsPmicModeKeyIrqV2;
  }

  if (D->PmicMode == ButtonsPmicModeLegacy) return STATUS_SUCCESS;
  if (D->PmicMode != ButtonsPmicModeKeyIrqV2) return ButtonsPmicFault(D);
  if (D->KeyIrqLeaseHeld) return STATUS_SUCCESS;

  LeaseToken = 0;
  PmicStatus = MtkMaliPmicPrmAcquireKeyIrqLease(&LeaseToken);
  if (PmicStatus == MaliBusy || PmicStatus == MaliOffline) {
    return STATUS_DEVICE_NOT_READY;
  }
  if (PmicStatus != MaliOk || LeaseToken == 0) return ButtonsPmicFault(D);

  D->KeyIrqLeaseToken = LeaseToken;
  D->KeyIrqLeaseHeld = TRUE;
  return STATUS_SUCCESS;
}

/* Consume an already-read snapshot exactly once, drain all corresponding
   inbox toggles, then ask the PMIC provider to acknowledge the capture. */
static NTSTATUS ButtonsFinishKeyIrqSnapshot(BUTTONS_DEVICE *D) {
  MTK_BUTTONS_CORE_STATUS CoreStatus;
  MALI_RESULT PmicStatus;
  NTSTATUS Status;

  if (!D->KeyIrqSnapshotPending) return STATUS_SUCCESS;
  if (!D->KeyIrqLeaseHeld || D->KeyIrqLeaseToken == 0) {
    return ButtonsPmicFault(D);
  }

  if (!D->KeyIrqSnapshotSampled) {
    CoreStatus = MtkButtonsCoreSamplePmicSnapshot(
      &D->Core, &D->KeyIrqKpdSample, D->KeyIrqSnapshot.TopStatus,
      (uint8_t)(D->KeyIrqSnapshot.GroupStatus & MTK_BUTTON_POWER_EVENT_MASK),
      D->KeyIrqSnapshot.Generation, MTK_BUTTON_SAMPLE_ALL_VALID);
    if (CoreStatus != MtkButtonsCoreSuccess) return ButtonsPmicFault(D);
    D->KeyIrqSnapshotSampled = TRUE;
  }

  Status = ButtonsDrainActions(D);
  if (Status != STATUS_SUCCESS) return Status;
  PmicStatus = MtkMaliPmicPrmAckKeyIrqSnapshot(&D->KeyIrqSnapshot);
  if (PmicStatus == MaliBusy) {
    /* Sibling capture still owns shared status. Keep the same token and do not
       resample or repeat inbox toggles. */
    return STATUS_DEVICE_BUSY;
  }
  if (PmicStatus != MaliOk) return ButtonsPmicFault(D);

  RtlZeroMemory(&D->KeyIrqSnapshot, sizeof(D->KeyIrqSnapshot));
  RtlZeroMemory(&D->KeyIrqKpdSample, sizeof(D->KeyIrqKpdSample));
  D->KeyIrqSnapshotPending = FALSE;
  D->KeyIrqSnapshotSampled = FALSE;
  return STATUS_SUCCESS;
}

static BOOLEAN ButtonsSample(BUTTONS_DEVICE *D) {
  MTK_BUTTON_KPD_SAMPLE Kpd;
  static const ULONG StatusOffsets[5] = {0x04U, 0x08U, 0x0cU, 0x10U, 0x14U};
  ULONG Index;
  USHORT StatusWord;
  uint16_t PmicTopStatus = 0;
  MALI_RESULT PmicStatus;
  MTK_BUTTONS_CORE_STATUS CoreStatus;
  MTK_PMIC_KEY_IRQ_SNAPSHOT Snapshot;
  NTSTATUS Status;

  if (!D->Prepared || D->Registers == NULL ||
      InterlockedCompareExchange(&D->Active, 0, 0) == 0 ||
      InterlockedCompareExchange(&D->Quarantined, 0, 0) != 0) {
    return FALSE;
  }

  /* Read the same five matrix words as the stock bottom half on every sample. */
  Kpd.Enable = READ_REGISTER_USHORT((PUSHORT)(D->Registers + 0x24));
  Kpd.Debounce = READ_REGISTER_USHORT((PUSHORT)(D->Registers + 0x18));
  Kpd.StatusWord0 = 0;
  for (Index = 0; Index < RTL_NUMBER_OF(StatusOffsets); ++Index) {
    StatusWord = READ_REGISTER_USHORT(
      (PUSHORT)(D->Registers + StatusOffsets[Index]));
    if (Index == 0) Kpd.StatusWord0 = StatusWord;
  }

  if ((Kpd.Enable & 1U) == 0 || (Kpd.Debounce & 0x3fffU) != 1024U) {
    return FALSE;
  }

  Status = ButtonsEnsurePmicPath(D);
  if (Status != STATUS_SUCCESS) return FALSE;

  if (D->PmicMode == ButtonsPmicModeKeyIrqV2) {
    if (D->KeyIrqSnapshotPending) return FALSE;
    /* Do not accumulate a new PMIC capture behind older undelivered toggles. */
    if (D->Core.ActionCount != 0) {
      Status = ButtonsDrainActions(D);
      if (Status != STATUS_SUCCESS) return FALSE;
    }

    RtlZeroMemory(&Snapshot, sizeof(Snapshot));
    PmicStatus = MtkMaliPmicPrmReadKeyIrqSnapshot(
      D->KeyIrqLeaseToken, &Snapshot);
    if (PmicStatus == MaliBusy || PmicStatus == MaliOffline) return FALSE;
    if (PmicStatus != MaliOk) {
      ButtonsPmicFault(D);
      return FALSE;
    }

    /* Save provider ownership before any core decision; malformed/stale data
       must remain quarantined and must never be silently discarded. */
    D->KeyIrqSnapshot = Snapshot;
    D->KeyIrqKpdSample = Kpd;
    D->KeyIrqSnapshotPending = TRUE;
    D->KeyIrqSnapshotSampled = FALSE;
    if (Snapshot.Generation == 0) {
      ButtonsPmicFault(D);
      return FALSE;
    }

    CoreStatus = MtkButtonsCoreSamplePmicSnapshot(
      &D->Core, &D->KeyIrqKpdSample, Snapshot.TopStatus,
      (uint8_t)(Snapshot.GroupStatus & MTK_BUTTON_POWER_EVENT_MASK),
      Snapshot.Generation, MTK_BUTTON_SAMPLE_ALL_VALID);
    if (CoreStatus != MtkButtonsCoreSuccess) {
      ButtonsPmicFault(D);
      return FALSE;
    }
    D->KeyIrqSnapshotSampled = TRUE;
    return TRUE;
  }

  PmicStatus = MtkMaliPmicPrmRead16(0x28U, &PmicTopStatus);
  if (PmicStatus == MaliBusy || PmicStatus == MaliOffline ||
      PmicStatus == MaliUnsupported) {
    /* Preserve queued core actions until both halves of a sample are valid. */
    return FALSE;
  }
  if (PmicStatus != MaliOk) {
    ButtonsQuarantine(D);
    return FALSE;
  }
  if (InterlockedCompareExchange(&D->Active, 0, 0) == 0 ||
      InterlockedCompareExchange(&D->Quarantined, 0, 0) != 0) {
    return FALSE;
  }

  CoreStatus = MtkButtonsCoreSample(&D->Core, &Kpd, PmicTopStatus,
                                    MTK_BUTTON_SAMPLE_ALL_VALID);
  if (CoreStatus == MtkButtonsCoreSuccess) return TRUE;
  if (CoreStatus != MtkButtonsCoreNotReady) ButtonsQuarantine(D);
  return FALSE;
}

VOID ButtonsWork(WDFWORKITEM Work) {
  BUTTONS_DEVICE *D = ButtonsContext(WdfWorkItemGetParentObject(Work));
  for (;;) {
    NTSTATUS Status;
    BOOLEAN SampleCommitted;

    if (InterlockedCompareExchange(&D->Active, 0, 0) == 0 ||
        InterlockedCompareExchange(&D->Quarantined, 0, 0) != 0 ||
        InterlockedExchange(&D->SamplePending, 0) == 0) {
      break;
    }

    /* Keep edge delivery active; ISR only marks a deferred whole-sample request. */
    if (!D->NotifyOpen) {
      Status = ButtonsOpenNotifyTarget(D);
      if (Status == STATUS_DEVICE_CONFIGURATION_ERROR) ButtonsQuarantine(D);
    }

    if (InterlockedCompareExchange(&D->Active, 0, 0) == 0 ||
        InterlockedCompareExchange(&D->Quarantined, 0, 0) != 0) {
      break;
    }

    if (D->KeyIrqSnapshotPending) {
      Status = ButtonsFinishKeyIrqSnapshot(D);
      if (Status != STATUS_SUCCESS) break;
      continue;
    }

    SampleCommitted = ButtonsSample(D);

    if (SampleCommitted) {
      if (D->PmicMode == ButtonsPmicModeKeyIrqV2 &&
          D->KeyIrqSnapshotPending) {
        /* A snapshot captured just as D0 exit closes admission remains owned;
           D0Exit drains it after flushing this worker. */
        if (InterlockedCompareExchange(&D->Active, 0, 0) != 0 &&
            InterlockedCompareExchange(&D->Quarantined, 0, 0) == 0) {
          Status = ButtonsFinishKeyIrqSnapshot(D);
          if (Status != STATUS_SUCCESS) break;
        }
      } else if (InterlockedCompareExchange(&D->Active, 0, 0) != 0 &&
                 InterlockedCompareExchange(&D->Quarantined, 0, 0) == 0) {
        (void)ButtonsDrainActions(D);
      }
    }
  }

  InterlockedExchange(&D->WorkQueued, 0);
  if (InterlockedCompareExchange(&D->Active, 0, 0) != 0 &&
      InterlockedCompareExchange(&D->Quarantined, 0, 0) == 0 &&
      InterlockedCompareExchange(&D->SamplePending, 0, 0) != 0) {
    ButtonsQueueWork(D);
  }
}

NTSTATUS ButtonsPrepareHardware(
  WDFDEVICE Device,
  WDFCMRESLIST Raw,
  WDFCMRESLIST Translated
  ) {
  BUTTONS_DEVICE *D = ButtonsContext(Device);
  PCM_PARTIAL_RESOURCE_DESCRIPTOR RawMemory = NULL;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR TranslatedMemory = NULL;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR RawInterrupt = NULL;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR TranslatedInterrupt = NULL;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
  ULONG Index, Count;
  PHYSICAL_ADDRESS Address;
  NTSTATUS Status;

  if (D->Prepared || Raw == NULL || Translated == NULL) {
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }
  if (D->PmicClientFaulted || D->Core.Quarantined ||
      InterlockedCompareExchange(&D->Quarantined, 0, 0) != 0) {
    return STATUS_DEVICE_HARDWARE_ERROR;
  }
  if (D->KeyIrqLeaseHeld || D->KeyIrqSnapshotPending ||
      D->Core.ActionCount != 0 || D->Core.DeliveredPressed != 0) {
    return STATUS_DEVICE_BUSY;
  }

  Count = WdfCmResourceListGetCount(Raw);
  if (Count == 0 || Count > 2 ||
      WdfCmResourceListGetCount(Translated) != Count) {
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }

  for (Index = 0; Index < Count; ++Index) {
    Descriptor = WdfCmResourceListGetDescriptor(Raw, Index);
    if (Descriptor == NULL) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (Descriptor->Type == CmResourceTypeMemory) {
      if (RawMemory != NULL ||
          Descriptor->u.Memory.Start.QuadPart != BUTTONS_KPD_PHYSICAL ||
          Descriptor->u.Memory.Length != BUTTONS_KPD_LENGTH ||
          (Descriptor->Flags & CM_RESOURCE_MEMORY_READ_ONLY) == 0) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
      }
      RawMemory = Descriptor;
    } else if (Descriptor->Type == CmResourceTypeInterrupt) {
      if (RawInterrupt != NULL ||
          Descriptor->u.Interrupt.Vector != 138U ||
          (Descriptor->Flags & CM_RESOURCE_INTERRUPT_LATCHED) == 0 ||
          (Descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) != 0 ||
          Descriptor->ShareDisposition != CmResourceShareDeviceExclusive) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
      }
      RawInterrupt = Descriptor;
    } else {
      return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
  }

  for (Index = 0; Index < Count; ++Index) {
    Descriptor = WdfCmResourceListGetDescriptor(Translated, Index);
    if (Descriptor == NULL) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (Descriptor->Type == CmResourceTypeMemory) {
      if (TranslatedMemory != NULL ||
          Descriptor->u.Memory.Start.QuadPart != BUTTONS_KPD_PHYSICAL ||
          Descriptor->u.Memory.Length != BUTTONS_KPD_LENGTH ||
          (Descriptor->Flags & CM_RESOURCE_MEMORY_READ_ONLY) == 0) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
      }
      TranslatedMemory = Descriptor;
    } else if (Descriptor->Type == CmResourceTypeInterrupt) {
      if (TranslatedInterrupt != NULL ||
          (Descriptor->Flags & CM_RESOURCE_INTERRUPT_LATCHED) == 0 ||
          (Descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) != 0 ||
          Descriptor->ShareDisposition != CmResourceShareDeviceExclusive) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
      }
      TranslatedInterrupt = Descriptor;
    } else {
      return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
  }

  if (RawMemory == NULL || TranslatedMemory == NULL ||
      ((RawInterrupt == NULL) != (TranslatedInterrupt == NULL)) ||
      (D->Interrupt != NULL && RawInterrupt == NULL)) {
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }

  Address = TranslatedMemory->u.Memory.Start;
  D->Registers = (PUCHAR)MmMapIoSpaceEx(
    Address, BUTTONS_KPD_LENGTH, PAGE_READONLY | PAGE_NOCACHE);
  if (D->Registers == NULL) return STATUS_INSUFFICIENT_RESOURCES;

  if (RawInterrupt != NULL && D->Interrupt == NULL) {
    WDF_INTERRUPT_CONFIG InterruptConfig;
    WDFINTERRUPT NewInterrupt = NULL;

    WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, ButtonsInterruptIsr,
                              ButtonsInterruptDpc);
    InterruptConfig.AutomaticSerialization = FALSE;
    InterruptConfig.PassiveHandling = FALSE;
    InterruptConfig.CanWakeDevice = FALSE;
    InterruptConfig.InterruptRaw = RawInterrupt;
    InterruptConfig.InterruptTranslated = TranslatedInterrupt;
    InterruptConfig.EvtInterruptEnable = ButtonsInterruptEnable;
    InterruptConfig.EvtInterruptDisable = ButtonsInterruptDisable;
    Status = WdfInterruptCreate(Device, &InterruptConfig,
                                WDF_NO_OBJECT_ATTRIBUTES, &NewInterrupt);
    if (Status != STATUS_SUCCESS || NewInterrupt == NULL) {
      MmUnmapIoSpace(D->Registers, BUTTONS_KPD_LENGTH);
      D->Registers = NULL;
      return Status == STATUS_SUCCESS ? STATUS_DEVICE_CONFIGURATION_ERROR : Status;
    }
    D->Interrupt = NewInterrupt;
  }

  D->InterruptResourcePresent = (TranslatedInterrupt != NULL);
  D->TranslatedInterruptVector = TranslatedInterrupt != NULL
                              ? TranslatedInterrupt->u.Interrupt.Vector : 0U;
  D->Prepared = TRUE;
  return STATUS_SUCCESS;
}

static NTSTATUS ButtonsStopPolling(BUTTONS_DEVICE *D) {
  InterlockedExchange(&D->Active, 0);
  /* WDF has drained interrupt DPCs before D0Exit/ReleaseHardware. */
  WdfTimerStop(D->Timer, TRUE);
  WdfWorkItemFlush(D->Work);
  return InterlockedCompareExchange(&D->Quarantined, 0, 0) != 0
       ? STATUS_DEVICE_HARDWARE_ERROR : STATUS_SUCCESS;
}

NTSTATUS ButtonsD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Previous) {
  BUTTONS_DEVICE *D = ButtonsContext(Device);
  UNREFERENCED_PARAMETER(Previous);
  if (!D->Prepared || D->Registers == NULL) return STATUS_DEVICE_NOT_READY;
  if (InterlockedCompareExchange(&D->Quarantined, 0, 0) != 0) {
    return STATUS_DEVICE_HARDWARE_ERROR;
  }
  if (D->KeyIrqSnapshotPending || D->Core.ActionCount != 0 ||
      D->Core.DeliveredPressed != 0) {
    /* Do not overwrite a retained v2 capture or restart before its exact
       inbox sequence has been delivered and acknowledged. */
    return STATUS_DEVICE_BUSY;
  }
  if (D->InterruptResourcePresent) {
    WDF_INTERRUPT_INFO Info;
    if (D->Interrupt == NULL) return STATUS_DEVICE_CONFIGURATION_ERROR;
    WDF_INTERRUPT_INFO_INIT(&Info);
    WdfInterruptGetInfo(D->Interrupt, &Info);
    if (Info.Vector != D->TranslatedInterruptVector ||
        Info.Mode != Latched || Info.Polarity != WdfInterruptActiveHigh ||
        Info.ShareDisposition != CmResourceShareDeviceExclusive) {
      return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
  }
  InterlockedExchange(&D->SamplePending, 0);
  InterlockedExchange(&D->Active, 1);
  WdfTimerStart(D->Timer, WDF_REL_TIMEOUT_IN_MS(BUTTONS_POLL_MS));
  ButtonsQueueWork(D);
  return STATUS_SUCCESS;
}

NTSTATUS ButtonsD0ExitPreInterruptsDisabled(
  WDFDEVICE Device,
  WDF_POWER_DEVICE_STATE Target
  ) {
  BUTTONS_DEVICE *D = ButtonsContext(Device);
  UNREFERENCED_PARAMETER(Target);
  /* Stop new ISR and timer work before WDF disconnects the interrupt. */
  InterlockedExchange(&D->Active, 0);
  return STATUS_SUCCESS;
}

NTSTATUS ButtonsD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target) {
  BUTTONS_DEVICE *D = ButtonsContext(Device);
  NTSTATUS Status;
  MALI_RESULT PmicStatus;
  UNREFERENCED_PARAMETER(Target);

  Status = ButtonsStopPolling(D);
  if (Status != STATUS_SUCCESS) return Status;
  if (D->KeyIrqSnapshotPending) {
    Status = ButtonsFinishKeyIrqSnapshot(D);
    if (Status != STATUS_SUCCESS) return Status;
  }
  Status = ButtonsDrainActions(D);
  if (Status != STATUS_SUCCESS) return Status;
  if (MtkButtonsCoreQueueReleaseAll(&D->Core) != MtkButtonsCoreSuccess) {
    ButtonsQuarantine(D);
    return STATUS_DEVICE_HARDWARE_ERROR;
  }
  Status = ButtonsDrainActions(D);
  if (Status != STATUS_SUCCESS) return Status;
  if (MtkButtonsCoreResetBaseline(&D->Core) != MtkButtonsCoreSuccess) {
    ButtonsQuarantine(D);
    return STATUS_DEVICE_HARDWARE_ERROR;
  }

  if (D->KeyIrqLeaseHeld) {
    PmicStatus = MtkMaliPmicPrmReleaseKeyIrqLease(D->KeyIrqLeaseToken);
    if (PmicStatus == MaliBusy) {
      /* The token remains owned; a later D0 teardown may retry this release. */
      return STATUS_DEVICE_BUSY;
    }
    if (PmicStatus != MaliOk) return ButtonsPmicFault(D);
    D->KeyIrqLeaseToken = 0;
    D->KeyIrqLeaseHeld = FALSE;
  }

  if (D->NotifyOpen) {
    WdfIoTargetClose(D->NotifyTarget);
    D->NotifyOpen = FALSE;
  }
  InterlockedExchange(&D->SamplePending, 0);
  return STATUS_SUCCESS;
}

NTSTATUS ButtonsReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Resources) {
  BUTTONS_DEVICE *D = ButtonsContext(Device);
  NTSTATUS Status;
  UNREFERENCED_PARAMETER(Resources);

  Status = ButtonsStopPolling(D);
  if (D->PmicClientFaulted) Status = STATUS_DEVICE_HARDWARE_ERROR;
  if (Status == STATUS_SUCCESS &&
      (D->KeyIrqLeaseHeld || D->KeyIrqSnapshotPending ||
       D->Core.ActionCount != 0 || D->Core.DeliveredPressed != 0)) {
    Status = STATUS_DEVICE_BUSY;
  }
  /* Resource ownership ends even on failure. Keep journal tokens and pending
     toggles, but never retain an MMIO mapping or reuse the deleted interrupt. */
  if (D->NotifyOpen) {
    WdfIoTargetClose(D->NotifyTarget);
    D->NotifyOpen = FALSE;
  }
  if (D->Registers != NULL) {
    MmUnmapIoSpace(D->Registers, BUTTONS_KPD_LENGTH);
    D->Registers = NULL;
  }
  D->Prepared = FALSE;
  /* Created in PrepareHardware: WDF deletes it after this callback returns. */
  D->Interrupt = NULL;
  D->InterruptResourcePresent = FALSE;
  D->TranslatedInterruptVector = 0U;
  return Status;
}

NTSTATUS ButtonsDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT Init) {
  WDFDEVICE Device;
  BUTTONS_DEVICE *D;
  WDF_OBJECT_ATTRIBUTES Attributes;
  WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
  WDF_WORKITEM_CONFIG WorkConfig;
  WDF_TIMER_CONFIG TimerConfig;
  NTSTATUS Status;
  UNREFERENCED_PARAMETER(Driver);

  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
  Pnp.EvtDevicePrepareHardware = ButtonsPrepareHardware;
  Pnp.EvtDeviceReleaseHardware = ButtonsReleaseHardware;
  Pnp.EvtDeviceD0Entry = ButtonsD0Entry;
  Pnp.EvtDeviceD0ExitPreInterruptsDisabled =
    ButtonsD0ExitPreInterruptsDisabled;
  Pnp.EvtDeviceD0Exit = ButtonsD0Exit;
  WdfDeviceInitSetPnpPowerEventCallbacks(Init, &Pnp);

  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, BUTTONS_DEVICE);
  Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
  Status = WdfDeviceCreate(&Init, &Attributes, &Device);
  if (Status != STATUS_SUCCESS) return Status;
  D = ButtonsContext(Device);
  D->Device = Device;
  Status = MtkButtonsCoreInitialize(&D->Core) == MtkButtonsCoreSuccess
         ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
  if (!NT_SUCCESS(Status)) return Status;

  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Attributes.ParentObject = Device;
  Status = WdfIoTargetCreate(Device, &Attributes, &D->NotifyTarget);
  if (Status != STATUS_SUCCESS) return Status;

  WDF_WORKITEM_CONFIG_INIT(&WorkConfig, ButtonsWork);
  WorkConfig.AutomaticSerialization = FALSE;
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Attributes.ParentObject = Device;
  Status = WdfWorkItemCreate(&WorkConfig, &Attributes, &D->Work);
  if (Status != STATUS_SUCCESS) return Status;

  WDF_TIMER_CONFIG_INIT_PERIODIC(&TimerConfig, ButtonsTimer, BUTTONS_POLL_MS);
  TimerConfig.AutomaticSerialization = FALSE;
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Attributes.ParentObject = Device;
  Status = WdfTimerCreate(&TimerConfig, &Attributes, &D->Timer);
  if (Status != STATUS_SUCCESS) return Status;
  return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Driver, PUNICODE_STRING RegistryPath) {
  WDF_DRIVER_CONFIG Config;
  WDF_DRIVER_CONFIG_INIT(&Config, ButtonsDeviceAdd);
  {
    NTSTATUS Status = WdfDriverCreate(Driver, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                                      &Config, WDF_NO_HANDLE);
    return Status == STATUS_SUCCESS ? STATUS_SUCCESS : Status;
  }
}
