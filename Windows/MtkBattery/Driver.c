/* SPDX-License-Identifier: GPL-2.0-only */
#include <ntddk.h>
#include <wdf.h>
#include <batclass.h>
#include <acpiioct.h>
#include <wmilib.h>
#include <wmistr.h>
#include "BatteryCore.h"
#include "../MtkMali/PmicPrm.h"

#define MTK_BATTERY_TAG 1U
#define MTK_BATTERY_POLL_MS 10000U
#define MTK_BATTERY_CACHE_MAX_AGE_100NS UINT64_C(100000000)

EVT_WDF_DRIVER_DEVICE_ADD BatteryDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE BatteryPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE BatteryReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY BatteryD0Entry;
EVT_WDF_DEVICE_D0_EXIT BatteryD0Exit;
EVT_WDF_TIMER BatteryPollTimer;
EVT_WDF_WORKITEM BatterySampleWorkItem;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL BatteryUnsupportedIoctl;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS BatteryPreprocessDeviceControl;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS BatteryPreprocessSystemControl;
WMI_QUERY_REGINFO_CALLBACK BatteryQueryWmiRegInfo;
WMI_QUERY_DATABLOCK_CALLBACK BatteryQueryWmiDataBlock;

BCLASS_QUERY_TAG_CALLBACK BatteryQueryTag;
BCLASS_QUERY_INFORMATION_CALLBACK BatteryQueryInformation;
BCLASS_SET_INFORMATION_CALLBACK BatterySetInformation;
BCLASS_QUERY_STATUS_CALLBACK BatteryQueryStatus;
BCLASS_SET_STATUS_NOTIFY_CALLBACK BatterySetStatusNotify;
BCLASS_DISABLE_STATUS_NOTIFY_CALLBACK BatteryDisableStatusNotify;

typedef struct {
  WDFDEVICE Device;
  WDFWAITLOCK StateLock;
  WDFWAITLOCK ClassLock;
  WDFWORKITEM SampleWorkItem;
  WDFTIMER PollTimer;
  PVOID ClassHandle;
  WMILIB_CONTEXT Wmi;
  MTK_BATTERY_CORE Core;
  MTK_BATTERY_STATE State;
  uint64_t CacheTime100ns;
  volatile LONG Active;
  volatile LONG WorkQueued;
  BOOLEAN StateValid;
  BOOLEAN WmiRegistered;
  BOOLEAN NotifyArmed;
  BOOLEAN NotifyNeedsInitialCheck;
  BATTERY_NOTIFY Notify;
} MTK_BATTERY_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MTK_BATTERY_DEVICE, BatteryContext)

static UNICODE_STRING mBatteryRegistryPath;

static BOOLEAN
BatteryNotifySatisfied (
  const BATTERY_NOTIFY *Notify,
  const MTK_BATTERY_STATE *State
  )
{
  return (BOOLEAN)MtkBatteryCoreNotificationSatisfied(
    State, Notify->PowerState, Notify->LowCapacity, Notify->HighCapacity);
}

static MTK_BATTERY_AC_STATE
BatteryReadAcState (WDFDEVICE Device)
{
  ACPI_EVAL_INPUT_BUFFER In = {0};
  UCHAR Storage[sizeof(ACPI_EVAL_OUTPUT_BUFFER) + sizeof(ULONGLONG)] = {0};
  PACPI_EVAL_OUTPUT_BUFFER Out = (PACPI_EVAL_OUTPUT_BUFFER)Storage;
  WDF_MEMORY_DESCRIPTOR Input, Output;
  WDF_REQUEST_SEND_OPTIONS Options;
  ULONG_PTR Returned = 0;
  ULONGLONG Value = 0;
  In.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
  RtlCopyMemory(In.MethodName, "ACST", 4);
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &In, sizeof(In));
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Output, Storage, sizeof(Storage));
  WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
  WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options, WDF_REL_TIMEOUT_IN_SEC(2));
  NTSTATUS Status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device), NULL,
      IOCTL_ACPI_EVAL_METHOD, &Input, &Output, &Options, &Returned);
  if (Status != STATUS_SUCCESS || Returned < sizeof(*Out) || Returned > sizeof(Storage) ||
      Out->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || Out->Count != 1 ||
      Out->Length < sizeof(*Out) || Out->Length > Returned ||
      Out->Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER) {
    return MtkBatteryAcUnknown;
  }
  USHORT Bytes = Out->Argument[0].DataLength;
  if ((Bytes != sizeof(ULONG) && Bytes != sizeof(ULONGLONG)) ||
      Out->Length < FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument[0].Data) + Bytes) {
    return MtkBatteryAcUnknown;
  }
  RtlCopyMemory(&Value, Storage + FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument[0].Data), Bytes);
  if (Value == 0) return MtkBatteryAcOffline;
  if (Value == 1) return MtkBatteryAcOnline;
  return MtkBatteryAcUnknown;
}

static NTSTATUS
BatteryReadAndUpdate (
  MTK_BATTERY_DEVICE *Device,
  BOOLEAN Force,
  BOOLEAN NotifyClass,
  MTK_BATTERY_STATE *State
  )
{
  MTK_PMIC_BATTERY_SNAPSHOT Pmic;
  MTK_BATTERY_SAMPLE Sample;
  MTK_BATTERY_CORE NextCore;
  MTK_BATTERY_STATE Next;
  MALI_RESULT PmicStatus;
  ULONGLONG Now;
  BOOLEAN NotifyClassDriver;
  PVOID ClassHandle;
  NTSTATUS Status;

  if (Device == NULL || State == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  if (InterlockedCompareExchange(&Device->Active, 0, 0) == 0) {
    WdfWaitLockAcquire(Device->StateLock, NULL);
    if (Device->StateValid) {
      *State = Device->State;
      WdfWaitLockRelease(Device->StateLock);
      return STATUS_SUCCESS;
    }
    WdfWaitLockRelease(Device->StateLock);
    return STATUS_DEVICE_NOT_READY;
  }

  Now = KeQueryInterruptTime();
  WdfWaitLockAcquire(Device->StateLock, NULL);
  if (InterlockedCompareExchange(&Device->Active, 0, 0) == 0) {
    if (Device->StateValid) {
      *State = Device->State;
      WdfWaitLockRelease(Device->StateLock);
      return STATUS_SUCCESS;
    }
    WdfWaitLockRelease(Device->StateLock);
    return STATUS_DEVICE_NOT_READY;
  }
  if (!Force && Device->StateValid && Device->CacheTime100ns != 0 &&
      Now >= Device->CacheTime100ns &&
      Now - Device->CacheTime100ns <= MTK_BATTERY_CACHE_MAX_AGE_100NS) {
    *State = Device->State;
    WdfWaitLockRelease(Device->StateLock);
    return STATUS_SUCCESS;
  }

  RtlZeroMemory(&Pmic, sizeof(Pmic));
  PmicStatus = MtkMaliPmicPrmReadBatterySnapshot(&Pmic);
  if (PmicStatus != MaliOk) {
    WdfWaitLockRelease(Device->StateLock);
    return PmicStatus == MaliBusy || PmicStatus == MaliOffline ||
      PmicStatus == MaliTimeout ? STATUS_DEVICE_NOT_READY :
      STATUS_DEVICE_HARDWARE_ERROR;
  }

  Sample.VoltageMv = Pmic.BatteryVoltageMv;
  Sample.CurrentStockUnits = Pmic.BatteryCurrentStockUnits;
  Sample.TemperatureDeciC = Pmic.BatteryTemperatureDeciC;
  Sample.Time100ns = Now;
  Sample.AcState = BatteryReadAcState(Device->Device);
  NextCore = Device->Core;
  Status = MtkBatteryCoreUpdate(&NextCore, &Sample, &Next) ==
      MtkBatteryCoreSuccess ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
  if (!NT_SUCCESS(Status)) {
    WdfWaitLockRelease(Device->StateLock);
    return Status;
  }

  NotifyClassDriver = FALSE;
  if (NotifyClass && Device->NotifyArmed) {
    BOOLEAN Satisfied = BatteryNotifySatisfied(&Device->Notify, &Next);
    if (Device->NotifyNeedsInitialCheck) {
      NotifyClassDriver = !Satisfied;
      Device->NotifyNeedsInitialCheck = FALSE;
    } else if (Device->StateValid &&
               BatteryNotifySatisfied(&Device->Notify, &Device->State) &&
               !Satisfied) {
      NotifyClassDriver = TRUE;
    }
    if (NotifyClassDriver) {
      Device->NotifyArmed = FALSE;
    }
  }
  if (NotifyClass) {
    Device->Core = NextCore;
    Device->State = Next;
    Device->CacheTime100ns = Now;
    Device->StateValid = TRUE;
  }
  *State = Next;
  WdfWaitLockRelease(Device->StateLock);

  if (NotifyClassDriver) {
    WdfWaitLockAcquire(Device->ClassLock, NULL);
    ClassHandle = Device->ClassHandle;
    WdfWaitLockRelease(Device->ClassLock);
    if (ClassHandle != NULL) {
      (void)BatteryClassStatusNotify(ClassHandle);
    }
  }
  return STATUS_SUCCESS;
}

static VOID
BatteryQueueSample (
  MTK_BATTERY_DEVICE *Device
  )
{
  if (Device == NULL ||
      InterlockedCompareExchange(&Device->Active, 0, 0) == 0) {
    return;
  }
  if (InterlockedCompareExchange(&Device->WorkQueued, 1, 0) == 0) {
    if (InterlockedCompareExchange(&Device->Active, 0, 0) != 0) {
      WdfWorkItemEnqueue(Device->SampleWorkItem);
    } else {
      InterlockedExchange(&Device->WorkQueued, 0);
    }
  }
}

VOID
BatteryPollTimer (
  WDFTIMER Timer
  )
{
  WDFDEVICE Device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
  BatteryQueueSample(BatteryContext(Device));
}

VOID
BatterySampleWorkItem (
  WDFWORKITEM WorkItem
  )
{
  WDFDEVICE Device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
  MTK_BATTERY_DEVICE *Context = BatteryContext(Device);
  MTK_BATTERY_STATE State;

  if (InterlockedCompareExchange(&Context->Active, 0, 0) != 0) {
    (void)BatteryReadAndUpdate(Context, TRUE, TRUE, &State);
  }
  InterlockedExchange(&Context->WorkQueued, 0);
}

NTSTATUS
BatteryD0Entry (
  WDFDEVICE Device,
  WDF_POWER_DEVICE_STATE PreviousState
  )
{
  MTK_BATTERY_DEVICE *Context = BatteryContext(Device);
  UNREFERENCED_PARAMETER(PreviousState);

  InterlockedExchange(&Context->Active, 1);
  WdfTimerStart(Context->PollTimer, WDF_REL_TIMEOUT_IN_MS(MTK_BATTERY_POLL_MS));
  BatteryQueueSample(Context);
  return STATUS_SUCCESS;
}

NTSTATUS
BatteryD0Exit (
  WDFDEVICE Device,
  WDF_POWER_DEVICE_STATE TargetState
  )
{
  MTK_BATTERY_DEVICE *Context = BatteryContext(Device);
  UNREFERENCED_PARAMETER(TargetState);

  InterlockedExchange(&Context->Active, 0);
  WdfTimerStop(Context->PollTimer, TRUE);
  WdfWorkItemFlush(Context->SampleWorkItem);
  WdfWaitLockAcquire(Context->StateLock, NULL);
  MtkBatteryCoreSuspend(&Context->Core);
  WdfWaitLockRelease(Context->StateLock);
  return STATUS_SUCCESS;
}

NTSTATUS
BatteryPrepareHardware (
  WDFDEVICE Device,
  WDFCMRESLIST ResourcesRaw,
  WDFCMRESLIST ResourcesTranslated
  )
{
  MTK_BATTERY_DEVICE *Context = BatteryContext(Device);
  BATTERY_MINIPORT_INFO_V1_1 Info;
  NTSTATUS Status;
  PDEVICE_OBJECT DeviceObject;

  UNREFERENCED_PARAMETER(ResourcesRaw);
  UNREFERENCED_PARAMETER(ResourcesTranslated);
  PAGED_CODE();

  RtlZeroMemory(&Info, sizeof(Info));
  Info.MajorVersion = BATTERY_CLASS_MAJOR_VERSION;
  Info.MinorVersion = BATTERY_CLASS_MINOR_VERSION_1;
  Info.Context = Context;
  Info.QueryTag = BatteryQueryTag;
  Info.QueryInformation = BatteryQueryInformation;
  Info.SetInformation = BatterySetInformation;
  Info.QueryStatus = BatteryQueryStatus;
  Info.SetStatusNotify = BatterySetStatusNotify;
  Info.DisableStatusNotify = BatteryDisableStatusNotify;
  Info.Pdo = WdfDeviceWdmGetPhysicalDevice(Device);
  Info.DeviceName = NULL;
  Info.Fdo = WdfDeviceWdmGetDeviceObject(Device);

  WdfWaitLockAcquire(Context->ClassLock, NULL);
  if (Context->ClassHandle == NULL) {
    Status = BatteryClassInitializeDevice(
      (PBATTERY_MINIPORT_INFO)&Info, &Context->ClassHandle);
  } else {
    Status = STATUS_SUCCESS;
  }
  WdfWaitLockRelease(Context->ClassLock);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }

  Context->Wmi.GuidCount = 0;
  Context->Wmi.GuidList = NULL;
  Context->Wmi.QueryWmiRegInfo = BatteryQueryWmiRegInfo;
  Context->Wmi.QueryWmiDataBlock = BatteryQueryWmiDataBlock;
  Context->Wmi.SetWmiDataBlock = NULL;
  Context->Wmi.SetWmiDataItem = NULL;
  Context->Wmi.ExecuteWmiMethod = NULL;
  Context->Wmi.WmiFunctionControl = NULL;
  DeviceObject = WdfDeviceWdmGetDeviceObject(Device);
  Status = IoWMIRegistrationControl(DeviceObject, WMIREG_ACTION_REGISTER);
  if (NT_SUCCESS(Status)) {
    Context->WmiRegistered = TRUE;
  } else {
    /* Battery IOCTLs and class registration remain usable without WMI. */
    Status = STATUS_SUCCESS;
  }
  return Status;
}

NTSTATUS
BatteryReleaseHardware (
  WDFDEVICE Device,
  WDFCMRESLIST ResourcesTranslated
  )
{
  MTK_BATTERY_DEVICE *Context = BatteryContext(Device);
  NTSTATUS Status = STATUS_SUCCESS;
  NTSTATUS WmiStatus;

  UNREFERENCED_PARAMETER(ResourcesTranslated);
  PAGED_CODE();

  InterlockedExchange(&Context->Active, 0);
  WdfTimerStop(Context->PollTimer, TRUE);
  WdfWorkItemFlush(Context->SampleWorkItem);

  if (Context->WmiRegistered) {
    WmiStatus = IoWMIRegistrationControl(
      WdfDeviceWdmGetDeviceObject(Device), WMIREG_ACTION_DEREGISTER);
    Context->WmiRegistered = FALSE;
    if (!NT_SUCCESS(WmiStatus)) {
      Status = WmiStatus;
    }
  }

  WdfWaitLockAcquire(Context->ClassLock, NULL);
  if (Context->ClassHandle != NULL) {
    NTSTATUS UnloadStatus = BatteryClassUnload(Context->ClassHandle);
    Context->ClassHandle = NULL;
    if (NT_SUCCESS(Status) && !NT_SUCCESS(UnloadStatus)) {
      Status = UnloadStatus;
    }
  }
  WdfWaitLockRelease(Context->ClassLock);
  return Status;
}

NTSTATUS
BatteryDeviceAdd (
  WDFDRIVER Driver,
  PWDFDEVICE_INIT DeviceInit
  )
{
  WDF_OBJECT_ATTRIBUTES DeviceAttributes;
  WDF_OBJECT_ATTRIBUTES ChildAttributes;
  WDF_OBJECT_ATTRIBUTES LockAttributes;
  WDF_OBJECT_ATTRIBUTES TimerAttributes;
  WDF_OBJECT_ATTRIBUTES WorkAttributes;
  WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
  WDF_IO_QUEUE_CONFIG QueueConfig;
  WDF_WORKITEM_CONFIG WorkConfig;
  WDF_TIMER_CONFIG TimerConfig;
  WDFDEVICE Device;
  WDFQUEUE Queue;
  MTK_BATTERY_DEVICE *Context;
  NTSTATUS Status;

  PAGED_CODE();

  RtlInitUnicodeString(
    &mBatteryRegistryPath, WdfDriverGetRegistryPath(Driver));

  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
  PnpCallbacks.EvtDevicePrepareHardware = BatteryPrepareHardware;
  PnpCallbacks.EvtDeviceReleaseHardware = BatteryReleaseHardware;
  PnpCallbacks.EvtDeviceD0Entry = BatteryD0Entry;
  PnpCallbacks.EvtDeviceD0Exit = BatteryD0Exit;
  WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpCallbacks);

  Status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
    DeviceInit, BatteryPreprocessDeviceControl, IRP_MJ_DEVICE_CONTROL, NULL, 0);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }
  Status = WdfDeviceInitAssignWdmIrpPreprocessCallback(
    DeviceInit, BatteryPreprocessSystemControl, IRP_MJ_SYSTEM_CONTROL, NULL, 0);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }

  WDF_OBJECT_ATTRIBUTES_INIT(&DeviceAttributes);
  WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&DeviceAttributes, MTK_BATTERY_DEVICE);
  Status = WdfDeviceCreate(&DeviceInit, &DeviceAttributes, &Device);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }

  Context = BatteryContext(Device);
  Context->Device = Device;
  MtkBatteryCoreInitialize(&Context->Core);

  WDF_OBJECT_ATTRIBUTES_INIT(&LockAttributes);
  LockAttributes.ParentObject = Device;
  Status = WdfWaitLockCreate(&LockAttributes, &Context->StateLock);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }
  Status = WdfWaitLockCreate(&LockAttributes, &Context->ClassLock);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }

  WDF_WORKITEM_CONFIG_INIT(&WorkConfig, BatterySampleWorkItem);
  WDF_OBJECT_ATTRIBUTES_INIT(&WorkAttributes);
  WorkAttributes.ParentObject = Device;
  Status = WdfWorkItemCreate(
    &WorkConfig, &WorkAttributes, &Context->SampleWorkItem);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }

  WDF_TIMER_CONFIG_INIT_PERIODIC(
    &TimerConfig, BatteryPollTimer, MTK_BATTERY_POLL_MS);
  WDF_OBJECT_ATTRIBUTES_INIT(&TimerAttributes);
  TimerAttributes.ParentObject = Device;
  Status = WdfTimerCreate(
    &TimerConfig, &TimerAttributes, &Context->PollTimer);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }

  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
    &QueueConfig, WdfIoQueueDispatchSequential);
  QueueConfig.EvtIoDeviceControl = BatteryUnsupportedIoctl;
  WDF_OBJECT_ATTRIBUTES_INIT(&ChildAttributes);
  ChildAttributes.ParentObject = Device;
  Status = WdfIoQueueCreate(Device, &QueueConfig, &ChildAttributes, &Queue);
  return Status;
}

VOID
BatteryUnsupportedIoctl (
  WDFQUEUE Queue,
  WDFREQUEST Request,
  size_t OutputBufferLength,
  size_t InputBufferLength,
  ULONG IoControlCode
  )
{
  UNREFERENCED_PARAMETER(Queue);
  UNREFERENCED_PARAMETER(OutputBufferLength);
  UNREFERENCED_PARAMETER(InputBufferLength);
  UNREFERENCED_PARAMETER(IoControlCode);
  WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
}

NTSTATUS
BatteryPreprocessDeviceControl (
  WDFDEVICE Device,
  PIRP Irp
  )
{
  MTK_BATTERY_DEVICE *Context = BatteryContext(Device);
  NTSTATUS Status = STATUS_NOT_SUPPORTED;

  PAGED_CODE();
  WdfWaitLockAcquire(Context->ClassLock, NULL);
  if (Context->ClassHandle != NULL) {
    Status = BatteryClassIoctl(Context->ClassHandle, Irp);
  }
  WdfWaitLockRelease(Context->ClassLock);
  if (Status == STATUS_NOT_SUPPORTED) {
    IoSkipCurrentIrpStackLocation(Irp);
    Status = WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
  }
  return Status;
}

NTSTATUS
BatteryPreprocessSystemControl (
  WDFDEVICE Device,
  PIRP Irp
  )
{
  MTK_BATTERY_DEVICE *Context = BatteryContext(Device);
  PDEVICE_OBJECT DeviceObject = WdfDeviceWdmGetDeviceObject(Device);
  SYSCTL_IRP_DISPOSITION Disposition = IrpForward;
  NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

  PAGED_CODE();
  WdfWaitLockAcquire(Context->ClassLock, NULL);
  if (Context->ClassHandle != NULL) {
    Status = BatteryClassSystemControl(
      Context->ClassHandle, &Context->Wmi, DeviceObject, Irp, &Disposition);
  }
  WdfWaitLockRelease(Context->ClassLock);

  switch (Disposition) {
  case IrpProcessed:
    return Status;
  case IrpNotCompleted:
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
  case IrpForward:
  case IrpNotWmi:
  default:
    IoSkipCurrentIrpStackLocation(Irp);
    return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
  }
}

NTSTATUS
BatteryQueryWmiRegInfo (
  PDEVICE_OBJECT DeviceObject,
  PULONG RegFlags,
  PUNICODE_STRING InstanceName,
  PUNICODE_STRING *RegistryPath,
  PUNICODE_STRING MofResourceName,
  PDEVICE_OBJECT *Pdo
  )
{
  WDFDEVICE Device = WdfWdmDeviceGetWdfDeviceHandle(DeviceObject);
  UNREFERENCED_PARAMETER(InstanceName);
  UNREFERENCED_PARAMETER(MofResourceName);
  PAGED_CODE();

  *RegFlags = WMIREG_FLAG_INSTANCE_PDO;
  *RegistryPath = &mBatteryRegistryPath;
  *Pdo = WdfDeviceWdmGetPhysicalDevice(Device);
  return STATUS_SUCCESS;
}

NTSTATUS
BatteryQueryWmiDataBlock (
  PDEVICE_OBJECT DeviceObject,
  PIRP Irp,
  ULONG GuidIndex,
  ULONG InstanceIndex,
  ULONG InstanceCount,
  PULONG InstanceLengthArray,
  ULONG BufferAvail,
  PUCHAR Buffer
  )
{
  WDFDEVICE Device = WdfWdmDeviceGetWdfDeviceHandle(DeviceObject);
  MTK_BATTERY_DEVICE *Context = BatteryContext(Device);
  NTSTATUS Status;

  PAGED_CODE();
  if (InstanceIndex != 0 || InstanceCount != 1) {
    return WmiCompleteRequest(
      DeviceObject, Irp, STATUS_INVALID_PARAMETER, 0, IO_NO_INCREMENT);
  }
  if (InstanceLengthArray == NULL) {
    return WmiCompleteRequest(
      DeviceObject, Irp, STATUS_BUFFER_TOO_SMALL, 0, IO_NO_INCREMENT);
  }

  Status = BatteryClassQueryWmiDataBlock(
    Context->ClassHandle, DeviceObject, Irp, GuidIndex,
    InstanceLengthArray, BufferAvail, Buffer);
  if (Status == STATUS_WMI_GUID_NOT_FOUND) {
    Status = WmiCompleteRequest(
      DeviceObject, Irp, STATUS_WMI_GUID_NOT_FOUND, 0, IO_NO_INCREMENT);
  }
  return Status;
}

NTSTATUS
BatteryQueryTag (
  PVOID Context,
  PULONG BatteryTag
  )
{
  if (Context == NULL || BatteryTag == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  *BatteryTag = MTK_BATTERY_TAG;
  return STATUS_SUCCESS;
}

static NTSTATUS
BatteryGetInformation (
  MTK_BATTERY_DEVICE *Context,
  BATTERY_QUERY_INFORMATION_LEVEL Level,
  PVOID Buffer,
  ULONG BufferLength,
  PULONG ReturnedLength
  )
{
  MTK_BATTERY_STATE State;
  NTSTATUS Status;

  if (ReturnedLength == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  *ReturnedLength = 0;

  switch (Level) {
  case BatteryInformation:
    *ReturnedLength = sizeof(BATTERY_INFORMATION);
    if (Buffer == NULL || BufferLength < sizeof(BATTERY_INFORMATION)) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    {
      BATTERY_INFORMATION Info;
      RtlZeroMemory(&Info, sizeof(Info));
      Info.Capabilities = BATTERY_SYSTEM_BATTERY | BATTERY_SEALED;
      Info.Technology = 1;
      Info.Chemistry[0] = 'L';
      Info.Chemistry[1] = 'I';
      Info.Chemistry[2] = 'O';
      Info.Chemistry[3] = 'N';
      Info.DesignedCapacity = (ULONG)MtkBatteryCoreDesignCapacityMwh();
      Info.FullChargedCapacity = Info.DesignedCapacity;
      Info.DefaultAlert1 = Info.DesignedCapacity / 10U;
      Info.DefaultAlert2 = Info.DesignedCapacity / 20U;
      Info.CriticalBias = 0;
      Info.CycleCount = 0;
      RtlCopyMemory(Buffer, &Info, sizeof(Info));
      *ReturnedLength = sizeof(Info);
      return STATUS_SUCCESS;
    }

  case BatteryTemperature:
    *ReturnedLength = sizeof(ULONG);
    if (Buffer == NULL || BufferLength < sizeof(ULONG)) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    Status = BatteryReadAndUpdate(Context, FALSE, FALSE, &State);
    if (!NT_SUCCESS(Status)) {
      return Status;
    }
    {
      ULONG TemperatureKelvinTenth =
        (ULONG)(State.TemperatureDeciC + 2731);
      RtlCopyMemory(Buffer, &TemperatureKelvinTenth, sizeof(TemperatureKelvinTenth));
      *ReturnedLength = sizeof(TemperatureKelvinTenth);
      return STATUS_SUCCESS;
    }

  case BatteryEstimatedTime:
    *ReturnedLength = sizeof(ULONG);
    if (Buffer == NULL || BufferLength < sizeof(ULONG)) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    Status = BatteryReadAndUpdate(Context, FALSE, FALSE, &State);
    if (!NT_SUCCESS(Status)) {
      return Status;
    }
    {
      ULONG Seconds = BATTERY_UNKNOWN_TIME;
      if (State.RateMw < 0 && State.RemainingCapacityMwh > 0) {
        uint64_t Estimate =
          ((uint64_t)State.RemainingCapacityMwh * 3600U) /
          (uint32_t)(-State.RateMw);
        Seconds = Estimate > MAXULONG ? MAXULONG : (ULONG)Estimate;
      }
      RtlCopyMemory(Buffer, &Seconds, sizeof(Seconds));
      *ReturnedLength = sizeof(Seconds);
      return STATUS_SUCCESS;
    }

  case BatteryDeviceName:
  case BatteryManufactureName:
  case BatteryUniqueID:
    {
      static const WCHAR Name[] = L"T70 Pro Battery";
      static const WCHAR Manufacturer[] = L"Teclast";
      static const WCHAR UniqueId[] = L"T70Pro-MT6789-MT6366-0";
      const WCHAR *Text;
      ULONG Bytes;

      if (Level == BatteryDeviceName) {
        Text = Name;
      } else if (Level == BatteryManufactureName) {
        Text = Manufacturer;
      } else {
        Text = UniqueId;
      }
      Bytes = (ULONG)((wcslen(Text) + 1U) * sizeof(WCHAR));
      if (Buffer == NULL || BufferLength < Bytes) {
        *ReturnedLength = Bytes;
        return STATUS_BUFFER_TOO_SMALL;
      }
      RtlCopyMemory(Buffer, Text, Bytes);
      *ReturnedLength = Bytes;
      return STATUS_SUCCESS;
    }

  case BatteryGranularityInformation:
    *ReturnedLength = sizeof(BATTERY_REPORTING_SCALE);
    if (Buffer == NULL || BufferLength < sizeof(BATTERY_REPORTING_SCALE)) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    {
      BATTERY_REPORTING_SCALE Scale;
      Scale.Granularity = 1;
      Scale.Capacity = (ULONG)MtkBatteryCoreDesignCapacityMwh();
      RtlCopyMemory(Buffer, &Scale, sizeof(Scale));
      *ReturnedLength = sizeof(Scale);
      return STATUS_SUCCESS;
    }

  case BatteryManufactureDate:
  case BatterySerialNumber:
  default:
    return STATUS_INVALID_DEVICE_REQUEST;
  }
}

NTSTATUS
BatteryQueryInformation (
  PVOID Context,
  ULONG BatteryTag,
  BATTERY_QUERY_INFORMATION_LEVEL Level,
  LONG AtRate,
  PVOID Buffer,
  ULONG BufferLength,
  PULONG ReturnedLength
  )
{
  UNREFERENCED_PARAMETER(AtRate);
  if (Context == NULL || BatteryTag != MTK_BATTERY_TAG) {
    return STATUS_NO_SUCH_DEVICE;
  }
  return BatteryGetInformation(
    (MTK_BATTERY_DEVICE *)Context, Level, Buffer, BufferLength, ReturnedLength);
}

NTSTATUS
BatterySetInformation (
  PVOID Context,
  ULONG BatteryTag,
  BATTERY_SET_INFORMATION_LEVEL Level,
  PVOID Buffer
  )
{
  UNREFERENCED_PARAMETER(Level);
  UNREFERENCED_PARAMETER(Buffer);
  if (Context == NULL || BatteryTag != MTK_BATTERY_TAG) {
    return STATUS_NO_SUCH_DEVICE;
  }
  return STATUS_NOT_SUPPORTED;
}

NTSTATUS
BatteryQueryStatus (
  PVOID Context,
  ULONG BatteryTag,
  PBATTERY_STATUS BatteryStatus
  )
{
  MTK_BATTERY_DEVICE *Device = (MTK_BATTERY_DEVICE *)Context;
  MTK_BATTERY_STATE State;
  NTSTATUS Status;

  if (Device == NULL || BatteryStatus == NULL ||
      BatteryTag != MTK_BATTERY_TAG) {
    return STATUS_INVALID_PARAMETER;
  }
  Status = BatteryReadAndUpdate(Device, FALSE, FALSE, &State);
  if (!NT_SUCCESS(Status)) {
    return Status;
  }

  BatteryStatus->PowerState = State.PowerState;
  BatteryStatus->Capacity = (ULONG)State.RemainingCapacityMwh;
  BatteryStatus->Voltage = (ULONG)State.VoltageMv;
  BatteryStatus->Rate = State.RateMw;
  return STATUS_SUCCESS;
}

NTSTATUS
BatterySetStatusNotify (
  PVOID Context,
  ULONG BatteryTag,
  PBATTERY_NOTIFY BatteryNotify
  )
{
  MTK_BATTERY_DEVICE *Device = (MTK_BATTERY_DEVICE *)Context;
  const ULONG AllowedPowerState = BATTERY_POWER_ON_LINE |
      BATTERY_DISCHARGING | BATTERY_CHARGING | BATTERY_CRITICAL;

  if (Device == NULL || BatteryTag != MTK_BATTERY_TAG) {
    return STATUS_NO_SUCH_DEVICE;
  }
  if (BatteryNotify == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  if ((BatteryNotify->PowerState & ~AllowedPowerState) != 0 ||
      (BatteryNotify->LowCapacity != 0 &&
       BatteryNotify->HighCapacity != 0 &&
       BatteryNotify->HighCapacity != BATTERY_UNKNOWN_CAPACITY &&
       BatteryNotify->LowCapacity > BatteryNotify->HighCapacity)) {
    return STATUS_NOT_SUPPORTED;
  }

  WdfWaitLockAcquire(Device->StateLock, NULL);
  Device->Notify = *BatteryNotify;
  Device->NotifyArmed = TRUE;
  Device->NotifyNeedsInitialCheck = TRUE;
  WdfWaitLockRelease(Device->StateLock);
  BatteryQueueSample(Device);
  return STATUS_SUCCESS;
}

NTSTATUS
BatteryDisableStatusNotify (
  PVOID Context
  )
{
  MTK_BATTERY_DEVICE *Device = (MTK_BATTERY_DEVICE *)Context;
  if (Device == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  WdfWaitLockAcquire(Device->StateLock, NULL);
  Device->NotifyArmed = FALSE;
  Device->NotifyNeedsInitialCheck = FALSE;
  RtlZeroMemory(&Device->Notify, sizeof(Device->Notify));
  WdfWaitLockRelease(Device->StateLock);
  return STATUS_SUCCESS;
}

NTSTATUS
DriverEntry (
  PDRIVER_OBJECT DriverObject,
  PUNICODE_STRING RegistryPath
  )
{
  WDF_DRIVER_CONFIG Config;
  WDF_OBJECT_ATTRIBUTES Attributes;
  WDFDRIVER Driver;
  NTSTATUS Status;

  UNREFERENCED_PARAMETER(RegistryPath);
  WDF_DRIVER_CONFIG_INIT(&Config, BatteryDeviceAdd);
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  Status = WdfDriverCreate(
    DriverObject, RegistryPath, &Attributes, &Config, &Driver);
  if (NT_SUCCESS(Status)) {
    RtlInitUnicodeString(
      &mBatteryRegistryPath, WdfDriverGetRegistryPath(Driver));
  }
  return Status;
}
