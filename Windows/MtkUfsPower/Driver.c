/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <ntddscsi.h>
#include <Library/MtkUfsPower.h>
#include "PrmClient.h"

#define UFS_POWER_DEVICE_SIGNATURE 0x50554637U
#define UFS_POWER_TAG '7FUM'
#define UFS_POWER_OPERATION_TIMEOUT_100NS 300000000ULL
#define UFS_POWER_IOCTL_TIMEOUT_SECONDS 10U
#define UFS_POWER_POLL_DELAY_100NS 10000LL

typedef struct {
  SRB_IO_CONTROL Header;
  UCHAR RequestBytes[MTK_UFS_POWER_REQUEST_BYTES];
} UFS_POWER_WIRE_BUFFER;
C_ASSERT(sizeof(SRB_IO_CONTROL)==28);
C_ASSERT(sizeof(UFS_POWER_WIRE_BUFFER)==92);
C_ASSERT(FIELD_OFFSET(UFS_POWER_WIRE_BUFFER,RequestBytes)==28);

typedef struct {
  ULONG Signature;
  PDEVICE_OBJECT Device;
  PDEVICE_OBJECT Pdo;
  PDEVICE_OBJECT Lower;
  IO_REMOVE_LOCK RemoveLock;
  KMUTEX UsageMutex;
  ULONG UsageCount[3];
  BOOLEAN LowerPowerPagable;
  volatile LONG Removing;
  volatile LONG PowerWorkActive;
  PIO_WORKITEM PowerWorkItem;
  PIRP PowerWorkIrp;
  KSPIN_LOCK MiniportLock;
  KEVENT MiniportDone;
  PIRP MiniportIrp;
  BOOLEAN MiniportAbandoned;
  UFS_POWER_WIRE_BUFFER MiniportBuffer;
  ULONG PrmState;
  DEVICE_POWER_STATE PowerState;
  BOOLEAN PowerFaulted;
} MTK_UFS_POWER_DEVICE;

static NTSTATUS MtkUfsForwardCompletion(PDEVICE_OBJECT Device,PIRP Irp,PVOID Context);

static NTSTATUS MtkUfsPowerExactStatus(NTSTATUS Status) {
  return (Status!=STATUS_SUCCESS && NT_SUCCESS(Status))?STATUS_UNSUCCESSFUL:Status;
}

static NTSTATUS MtkUfsPowerMapResult(ULONG Result) {
  switch(Result) {
  case MtkUfsPowerSuccess: return STATUS_SUCCESS;
  case MtkUfsPowerInProgress: return STATUS_PENDING;
  case MtkUfsPowerInvalid: return STATUS_INVALID_PARAMETER;
  case MtkUfsPowerUnsupported: return STATUS_NOT_SUPPORTED;
  case MtkUfsPowerWrongState: return STATUS_INVALID_DEVICE_STATE;
  case MtkUfsPowerIoError: return STATUS_IO_DEVICE_ERROR;
  case MtkUfsPowerTimeout: return STATUS_IO_TIMEOUT;
  case MtkUfsPowerStaleGeneration: return STATUS_RETRY;
  case MtkUfsPowerBusy: return STATUS_DEVICE_BUSY;
  default: return STATUS_DATA_ERROR;
  }
}

static NTSTATUS MtkUfsPowerLive(MTK_UFS_POWER_DEVICE *Device) {
  return InterlockedCompareExchange(&Device->Removing,0,0)?STATUS_DEVICE_REMOVED:STATUS_SUCCESS;
}

static BOOLEAN MtkUfsPowerRequestReservedZero(const MTK_UFS_POWER_REQUEST *Request) {
  ULONG I;
  for(I=0;I<RTL_NUMBER_OF(Request->Reserved);I++) if(Request->Reserved[I]) return FALSE;
  return TRUE;
}

static NTSTATUS MtkUfsMiniportComplete(PDEVICE_OBJECT Target,PIRP Irp,PVOID Context) {
  MTK_UFS_POWER_DEVICE *Device=Context;
  PDEVICE_OBJECT Owner=Device->Device,Lower=Device->Lower;
  KIRQL Irql;
  BOOLEAN Abandoned;
  UNREFERENCED_PARAMETER(Target);
  KeAcquireSpinLock(&Device->MiniportLock,&Irql);
  Abandoned=Device->MiniportAbandoned;
  if(Abandoned) Device->MiniportIrp=NULL;
  else KeSetEvent(&Device->MiniportDone,IO_NO_INCREMENT,FALSE);
  KeReleaseSpinLock(&Device->MiniportLock,Irql);
  if(Abandoned) IoFreeIrp(Irp);
  ObDereferenceObject(Lower);
  ObDereferenceObject(Owner);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS MtkUfsMiniportCall(MTK_UFS_POWER_DEVICE *Device,
                                   MTK_UFS_POWER_REQUEST *Request) {
  UFS_POWER_WIRE_BUFFER Wire;
  KIRQL Irql;
  IO_STATUS_BLOCK IoStatus;
  PIRP Irp;
  PIO_STACK_LOCATION Stack;
  NTSTATUS Status;
  LARGE_INTEGER DueTime;
  ULONG ReturnCode;
  USHORT InputOperation;
  if(!Device || !Device->Lower || !Request || KeGetCurrentIrql()!=PASSIVE_LEVEL)
    return STATUS_INVALID_PARAMETER;
  if(!NT_SUCCESS(MtkUfsPowerLive(Device))) return STATUS_DEVICE_REMOVED;
  // The power worker serializes calls. A late completion owns the previous
  // buffer until the lower stack returns it.
  KeAcquireSpinLock(&Device->MiniportLock,&Irql);
  BOOLEAN Busy=(BOOLEAN)(Device->MiniportIrp!=NULL);
  KeReleaseSpinLock(&Device->MiniportLock,Irql);
  if(Busy) return STATUS_DEVICE_NOT_READY;
  RtlZeroMemory(&Wire,sizeof(Wire));
  InputOperation=Request->Operation;
  Wire.Header.HeaderLength=sizeof(SRB_IO_CONTROL);
  RtlCopyMemory(Wire.Header.Signature,MTK_UFS_POWER_SRB_SIGNATURE,sizeof(Wire.Header.Signature));
  Wire.Header.Timeout=UFS_POWER_IOCTL_TIMEOUT_SECONDS;
  Wire.Header.ControlCode=MTK_UFS_POWER_SRB_CONTROL_CODE;
  Wire.Header.Length=sizeof(*Request);
  RtlCopyMemory(Wire.RequestBytes,Request,sizeof(*Request));
  Irp=IoAllocateIrp(Device->Lower->StackSize,FALSE);
  if(!Irp) return STATUS_INSUFFICIENT_RESOURCES;
  Irp->RequestorMode=KernelMode;
  Irp->Flags=IRP_BUFFERED_IO;
  Irp->AssociatedIrp.SystemBuffer=&Device->MiniportBuffer;
  Stack=IoGetNextIrpStackLocation(Irp);
  Stack->MajorFunction=IRP_MJ_DEVICE_CONTROL;
  Stack->Parameters.DeviceIoControl.IoControlCode=IOCTL_SCSI_MINIPORT;
  Stack->Parameters.DeviceIoControl.InputBufferLength=sizeof(Wire);
  Stack->Parameters.DeviceIoControl.OutputBufferLength=sizeof(Wire);
  Status=IoSetCompletionRoutineEx(Device->Device,Irp,MtkUfsMiniportComplete,Device,TRUE,TRUE,TRUE);
  if(Status!=STATUS_SUCCESS) {
    IoFreeIrp(Irp);
    return MtkUfsPowerExactStatus(Status);
  }

  // Keep the extension (including its wire buffer) and target alive across
  // cancellation or removal. The private completion drops both references.
  ObReferenceObject(Device->Device);
  ObReferenceObject(Device->Lower);
  KeAcquireSpinLock(&Device->MiniportLock,&Irql);
  Device->MiniportBuffer=Wire;
  Device->MiniportIrp=Irp;
  Device->MiniportAbandoned=FALSE;
  KeClearEvent(&Device->MiniportDone);
  KeReleaseSpinLock(&Device->MiniportLock,Irql);
  Status=IoCallDriver(Device->Lower,Irp);
  if(Status==STATUS_PENDING) {
    DueTime.QuadPart=-UFS_POWER_OPERATION_TIMEOUT_100NS;
    Status=KeWaitForSingleObject(&Device->MiniportDone,Executive,KernelMode,FALSE,&DueTime);
    if(Status!=STATUS_SUCCESS) {
      (void)IoCancelIrp(Irp);
      if(Status==STATUS_TIMEOUT) Status=STATUS_IO_TIMEOUT;
    }
  }

  KeAcquireSpinLock(&Device->MiniportLock,&Irql);
  if(!KeReadStateEvent(&Device->MiniportDone)) {
    Device->MiniportAbandoned=TRUE;
    KeReleaseSpinLock(&Device->MiniportLock,Irql);
    return Status==STATUS_SUCCESS?STATUS_IO_DEVICE_ERROR:MtkUfsPowerExactStatus(Status);
  }
  IoStatus=Irp->IoStatus;
  Wire=Device->MiniportBuffer;
  Device->MiniportIrp=NULL;
  KeReleaseSpinLock(&Device->MiniportLock,Irql);
  IoFreeIrp(Irp);
  if(Status!=STATUS_SUCCESS) return MtkUfsPowerExactStatus(Status);
  Status=MtkUfsPowerExactStatus(IoStatus.Status);
  if(Status!=STATUS_SUCCESS) return Status;
  RtlCopyMemory(Request,Wire.RequestBytes,sizeof(*Request));
  ReturnCode=Wire.Header.ReturnCode;
  if(IoStatus.Information!=sizeof(Wire) || Wire.Header.HeaderLength!=sizeof(SRB_IO_CONTROL) ||
     !RtlEqualMemory(Wire.Header.Signature,MTK_UFS_POWER_SRB_SIGNATURE,sizeof(Wire.Header.Signature)) ||
     Wire.Header.ControlCode!=MTK_UFS_POWER_SRB_CONTROL_CODE || Wire.Header.Length!=sizeof(*Request) ||
     ReturnCode!=Request->Result || Request->Size!=sizeof(*Request) ||
     Request->Version!=MTK_UFS_POWER_VERSION || Request->Operation!=InputOperation ||
     Request->Result>MtkUfsPowerBusy ||
     !MtkUfsPowerRequestReservedZero(Request) ||
     (Request->Flags&~MTK_UFS_POWER_DEVICE_DOWN_ACKED) ||
     (Request->Capabilities&~MTK_UFS_POWER_CAP_PROGRESS)) return STATUS_DATA_ERROR;
  return STATUS_SUCCESS;
}

static NTSTATUS MtkUfsMiniQuery(MTK_UFS_POWER_DEVICE *Device,
                                MTK_UFS_POWER_REQUEST *Request) {
  NTSTATUS Status;
  RtlZeroMemory(Request,sizeof(*Request));
  Request->Size=sizeof(*Request); Request->Version=MTK_UFS_POWER_VERSION;
  Request->Operation=MtkUfsMiniPowerQuery;
  Status=MtkUfsMiniportCall(Device,Request);
  if(!NT_SUCCESS(Status)) return Status;
  Status=MtkUfsPowerMapResult(Request->Result);
  if(!NT_SUCCESS(Status)) return Status;
  if(!Request->Generation || Request->State<MtkUfsMiniPowerActive ||
     Request->State>MtkUfsMiniPowerFault ||
     !(Request->Capabilities&MTK_UFS_POWER_CAP_PROGRESS)) return STATUS_DATA_ERROR;
  return STATUS_SUCCESS;
}

static NTSTATUS MtkUfsMiniOperation(MTK_UFS_POWER_DEVICE *Device,
                                    MTK_UFS_POWER_REQUEST *Request,
                                    MTK_UFS_MINIPORT_POWER_OPERATION Operation,
                                    ULONGLONG Generation,
                                    MTK_UFS_MINIPORT_POWER_STATE ExpectedState,
                                    ULONG ExpectedFlags) {
  ULONGLONG Started=KeQueryInterruptTime();
  LARGE_INTEGER Delay;
  NTSTATUS Status;
  ULONG Iteration;
  Delay.QuadPart=-UFS_POWER_POLL_DELAY_100NS;
  for(Iteration=0;Iteration<31000;Iteration++) {
    Status=MtkUfsPowerLive(Device);
    if(!NT_SUCCESS(Status)) return Status;
    RtlZeroMemory(Request,sizeof(*Request));
    Request->Size=sizeof(*Request); Request->Version=MTK_UFS_POWER_VERSION;
    Request->Operation=(USHORT)Operation; Request->Generation=Generation;
    Status=MtkUfsMiniportCall(Device,Request);
    if(!NT_SUCCESS(Status)) return Status;
    Status=MtkUfsPowerLive(Device);
    if(!NT_SUCCESS(Status)) return Status;
    Status=MtkUfsPowerMapResult(Request->Result);
    if(Status==STATUS_PENDING) {
      if(!Request->Generation || Request->Generation<Generation ||
         KeQueryInterruptTime()-Started>=UFS_POWER_OPERATION_TIMEOUT_100NS) return STATUS_IO_TIMEOUT;
      Generation=Request->Generation;
      (void)KeDelayExecutionThread(KernelMode,FALSE,&Delay);
      continue;
    }
    if(!NT_SUCCESS(Status)) return Status;
    if(Request->Generation<Generation || Request->State!=ExpectedState ||
       Request->Flags!=ExpectedFlags) return STATUS_DATA_ERROR;
    return STATUS_SUCCESS;
  }
  return STATUS_IO_TIMEOUT;
}

static NTSTATUS MtkUfsPrmTransaction(MTK_UFS_POWER_DEVICE *Device,
                                     MTK_UFS_POWER_OPERATION Operation,
                                     ULONG Flags,ULONGLONG InputGeneration,
                                     ULONG ExpectedState,
                                     ULONGLONG *OutputGeneration) {
  MTK_UFS_POWER_REQUEST Request;
  LARGE_INTEGER Delay;
  ULONGLONG Started=KeQueryInterruptTime(),Generation=InputGeneration;
  NTSTATUS Status;
  ULONG Iteration;
  Delay.QuadPart=-UFS_POWER_POLL_DELAY_100NS;
  for(Iteration=0;Iteration<31000;Iteration++) {
    Status=MtkUfsPowerLive(Device);
    if(!NT_SUCCESS(Status)) return Status;
    RtlZeroMemory(&Request,sizeof(Request));
    Request.Size=sizeof(Request); Request.Version=MTK_UFS_POWER_VERSION;
    Request.Operation=(USHORT)Operation; Request.Flags=Flags;
    Request.Generation=(Operation==MtkUfsPowerQuery)?0:Generation;
    Status=MtkUfsPrmInvoke(&Request);
    if(!NT_SUCCESS(Status)) return Status;
    Status=MtkUfsPowerLive(Device);
    if(!NT_SUCCESS(Status)) return Status;
    if(Request.Size!=sizeof(Request) || Request.Version!=MTK_UFS_POWER_VERSION ||
       Request.Operation!=(USHORT)Operation || Request.Flags!=Flags ||
       Request.Result>MtkUfsPowerBusy || !MtkUfsPowerRequestReservedZero(&Request) ||
       (Request.Capabilities&~(MTK_UFS_POWER_CAP_RETAINED_VEMC|
          MTK_UFS_POWER_CAP_LEAF_GATES|MTK_UFS_POWER_CAP_PROGRESS))) return STATUS_DATA_ERROR;
    Status=MtkUfsPowerMapResult(Request.Result);
    if(Status==STATUS_PENDING) {
      if(Operation!=MtkUfsPowerQuery) {
        if(!Request.Generation || Request.Generation<Generation) return STATUS_DATA_ERROR;
        Generation=Request.Generation;
      }
      if(KeQueryInterruptTime()-Started>=UFS_POWER_OPERATION_TIMEOUT_100NS) return STATUS_IO_TIMEOUT;
      (void)KeDelayExecutionThread(KernelMode,FALSE,&Delay);
      continue;
    }
    if(!NT_SUCCESS(Status)) return Status;
    if(Request.State> MtkUfsPowerFault ||
       (ExpectedState!=MAXULONG && Request.State!=ExpectedState))
      return STATUS_INVALID_DEVICE_STATE;
    if(Operation==MtkUfsPowerQuery && !Request.Generation) return STATUS_DATA_ERROR;
    if(Operation!=MtkUfsPowerQuery && Request.Generation<=InputGeneration) return STATUS_DATA_ERROR;
    Device->PrmState=Request.State;
    if(OutputGeneration) *OutputGeneration=Request.Generation;
    return STATUS_SUCCESS;
  }
  return STATUS_IO_TIMEOUT;
}

static NTSTATUS MtkUfsForwardPowerSync(MTK_UFS_POWER_DEVICE *Device,PIRP Irp) {
  KEVENT Event;
  NTSTATUS Status,WaitStatus;
  KeInitializeEvent(&Event,NotificationEvent,FALSE);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp,MtkUfsForwardCompletion,&Event,TRUE,TRUE,TRUE);
  Status=PoCallDriver(Device->Lower,Irp);
  if(Status==STATUS_PENDING) {
    WaitStatus=KeWaitForSingleObject(&Event,Executive,KernelMode,FALSE,NULL);
    if(WaitStatus!=STATUS_SUCCESS) return MtkUfsPowerExactStatus(WaitStatus);
  } else {
    Status=MtkUfsPowerExactStatus(Status);
    if(Status!=STATUS_SUCCESS) return Status;
  }
  return MtkUfsPowerExactStatus(Irp->IoStatus.Status);
}

static NTSTATUS MtkUfsForwardCompletion(PDEVICE_OBJECT Device,PIRP Irp,PVOID Context) {
  UNREFERENCED_PARAMETER(Device); UNREFERENCED_PARAMETER(Irp);
  KeSetEvent((PKEVENT)Context,IO_NO_INCREMENT,FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS MtkUfsPrmQuery(MTK_UFS_POWER_DEVICE *Device,
                               ULONG Expected,
                               ULONGLONG *Generation) {
  return MtkUfsPrmTransaction(Device,MtkUfsPowerQuery,0,0,Expected,Generation);
}

static NTSTATUS MtkUfsRestorePrmAndMini(MTK_UFS_POWER_DEVICE *Device,
                                        MTK_UFS_POWER_REQUEST *Mini) {
  NTSTATUS Status;
  ULONGLONG Generation=0;
  Status=MtkUfsMiniQuery(Device,Mini);
  if(!NT_SUCCESS(Status)) return Status;
  Status=MtkUfsPrmQuery(Device,MAXULONG,&Generation);
  if(!NT_SUCCESS(Status)) return Status;
  if(Mini->State==MtkUfsMiniPowerActive) {
    return Device->PrmState==MtkUfsPowerActive?STATUS_SUCCESS:STATUS_INVALID_DEVICE_STATE;
  }
  if(Mini->State==MtkUfsMiniPowerDevicePrepared) {
    if(Mini->Flags!=MTK_UFS_POWER_DEVICE_DOWN_ACKED) return STATUS_INVALID_DEVICE_STATE;
    if(Device->PrmState==MtkUfsPowerActive) {
      /* PRM never left Active, so the host is still live. Resume only the
       * device WLUN; stopping HCE here would make the Active query/rollback
       * impossible and would incorrectly require a second cold host start. */
      return MtkUfsMiniOperation(Device,Mini,MtkUfsMiniPowerRestartHost,Mini->Generation,
        MtkUfsMiniPowerActive,0);
    }
    if(Device->PrmState!=MtkUfsPowerStopPrepared) return STATUS_INVALID_DEVICE_STATE;
    Status=MtkUfsMiniOperation(Device,Mini,MtkUfsMiniPowerStopHost,Mini->Generation,
      MtkUfsMiniPowerHostStopped,MTK_UFS_POWER_DEVICE_DOWN_ACKED);
    if(!NT_SUCCESS(Status)) return Status;
  } else if(Mini->State!=MtkUfsMiniPowerHostStopped ||
            Mini->Flags!=MTK_UFS_POWER_DEVICE_DOWN_ACKED) {
    return STATUS_INVALID_DEVICE_STATE;
  }
  if(Device->PrmState==MtkUfsPowerStopPrepared || Device->PrmState==MtkUfsPowerReleased) {
    Status=MtkUfsPrmTransaction(Device,MtkUfsPowerRestore,0,Generation,MtkUfsPowerActive,&Generation);
    if(!NT_SUCCESS(Status)) return Status;
  } else if(Device->PrmState!=MtkUfsPowerActive || Mini->State==MtkUfsMiniPowerHostStopped) {
    return STATUS_INVALID_DEVICE_STATE;
  }
  if(Mini->State==MtkUfsMiniPowerHostStopped) {
    Status=MtkUfsMiniQuery(Device,Mini);
    if(!NT_SUCCESS(Status)) return Status;
    if(Mini->State!=MtkUfsMiniPowerHostStopped || Mini->Flags!=MTK_UFS_POWER_DEVICE_DOWN_ACKED)
      return STATUS_INVALID_DEVICE_STATE;
    Status=MtkUfsMiniOperation(Device,Mini,MtkUfsMiniPowerRestartHost,Mini->Generation,
      MtkUfsMiniPowerActive,0);
    if(!NT_SUCCESS(Status)) return Status;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS MtkUfsRollbackD3(MTK_UFS_POWER_DEVICE *Device,
                                MTK_UFS_POWER_REQUEST *Mini) {
  return MtkUfsRestorePrmAndMini(Device,Mini);
}

static NTSTATUS MtkUfsHandleD3(MTK_UFS_POWER_DEVICE *Device,PIRP Irp) {
  MTK_UFS_POWER_REQUEST Mini;
  ULONGLONG MiniGeneration=0,PrmGeneration=0;
  NTSTATUS Status,OriginalStatus;
  if(Device->PowerFaulted) return STATUS_DEVICE_NOT_READY;
  if(Device->PowerState==PowerDeviceD3) return MtkUfsForwardPowerSync(Device,Irp);
  Status=MtkUfsMiniQuery(Device,&Mini);
  if(!NT_SUCCESS(Status)) return Status;
  if(Mini.State!=MtkUfsMiniPowerActive) return STATUS_INVALID_DEVICE_STATE;
  MiniGeneration=Mini.Generation;
  Status=MtkUfsPrmQuery(Device,MtkUfsPowerActive,&PrmGeneration);
  if(!NT_SUCCESS(Status)) return Status;
  Status=MtkUfsMiniOperation(Device,&Mini,MtkUfsMiniPowerPrepareDevice,MiniGeneration,
    MtkUfsMiniPowerDevicePrepared,MTK_UFS_POWER_DEVICE_DOWN_ACKED);
  if(!NT_SUCCESS(Status)) {
    if(MtkUfsMiniQuery(Device,&Mini)==STATUS_SUCCESS && Mini.State==MtkUfsMiniPowerActive) return Status;
    Device->PowerFaulted=TRUE; return Status;
  }
  Status=MtkUfsPrmTransaction(Device,MtkUfsPowerPrepareStop,
    MTK_UFS_POWER_DEVICE_DOWN_ACKED,PrmGeneration,MtkUfsPowerStopPrepared,&PrmGeneration);
  if(!NT_SUCCESS(Status)) {
    OriginalStatus=Status;
    if(!NT_SUCCESS(MtkUfsPowerLive(Device)) || !NT_SUCCESS(MtkUfsRollbackD3(Device,&Mini))) Device->PowerFaulted=TRUE;
    return OriginalStatus;
  }
  Status=MtkUfsPowerLive(Device);
  if(!NT_SUCCESS(Status)) { Device->PowerFaulted=TRUE; return Status; }
  Status=MtkUfsMiniQuery(Device,&Mini);
  if(!NT_SUCCESS(Status)) { Device->PowerFaulted=TRUE; return Status; }
  Status=MtkUfsMiniOperation(Device,&Mini,MtkUfsMiniPowerStopHost,Mini.Generation,
    MtkUfsMiniPowerHostStopped,MTK_UFS_POWER_DEVICE_DOWN_ACKED);
  if(!NT_SUCCESS(Status)) { Device->PowerFaulted=TRUE; return Status; }
  Status=MtkUfsPowerLive(Device);
  if(!NT_SUCCESS(Status)) { Device->PowerFaulted=TRUE; return Status; }
  Status=MtkUfsForwardPowerSync(Device,Irp);
  if(!NT_SUCCESS(Status)) {
    OriginalStatus=Status;
    if(!NT_SUCCESS(MtkUfsPowerLive(Device)) || !NT_SUCCESS(MtkUfsRollbackD3(Device,&Mini))) Device->PowerFaulted=TRUE;
    return OriginalStatus;
  }
  Status=MtkUfsPowerLive(Device);
  if(!NT_SUCCESS(Status)) { Device->PowerFaulted=TRUE; return Status; }
  Status=MtkUfsPrmTransaction(Device,MtkUfsPowerRelease,0,PrmGeneration,
    MtkUfsPowerReleased,&PrmGeneration);
  if(!NT_SUCCESS(Status)) { Device->PowerFaulted=TRUE; return Status; }
  Device->PowerState=PowerDeviceD3;
  (void)PoSetPowerState(Device->Device,DevicePowerState,
    IoGetCurrentIrpStackLocation(Irp)->Parameters.Power.State);
  return STATUS_SUCCESS;
}

static NTSTATUS MtkUfsHandleD0(MTK_UFS_POWER_DEVICE *Device,PIRP Irp) {
  MTK_UFS_POWER_REQUEST Mini;
  ULONGLONG PrmGeneration=0;
  NTSTATUS Status,OriginalStatus;
  if(Device->PowerFaulted) return STATUS_DEVICE_NOT_READY;
  if(Device->PowerState==PowerDeviceD0) return MtkUfsForwardPowerSync(Device,Irp);
  Status=MtkUfsPrmQuery(Device,MtkUfsPowerReleased,&PrmGeneration);
  if(!NT_SUCCESS(Status)) { Device->PowerFaulted=TRUE; return Status; }
  Status=MtkUfsPrmTransaction(Device,MtkUfsPowerRestore,0,PrmGeneration,
    MtkUfsPowerActive,&PrmGeneration);
  if(!NT_SUCCESS(Status)) { Device->PowerFaulted=TRUE; return Status; }
  Status=MtkUfsPowerLive(Device);
  if(!NT_SUCCESS(Status)) { Device->PowerFaulted=TRUE; return Status; }
  Status=MtkUfsForwardPowerSync(Device,Irp);
  if(!NT_SUCCESS(Status)) {
    OriginalStatus=Status;
    Device->PowerFaulted=TRUE;
    return OriginalStatus;
  }
  Status=MtkUfsMiniQuery(Device,&Mini);
  if(!NT_SUCCESS(Status) || Mini.State!=MtkUfsMiniPowerHostStopped ||
     !(Mini.Flags&MTK_UFS_POWER_DEVICE_DOWN_ACKED)) {
    Device->PowerFaulted=TRUE; return NT_SUCCESS(Status)?STATUS_INVALID_DEVICE_STATE:Status;
  }
  Status=MtkUfsMiniOperation(Device,&Mini,MtkUfsMiniPowerRestartHost,Mini.Generation,
    MtkUfsMiniPowerActive,0);
  if(!NT_SUCCESS(Status)) { Device->PowerFaulted=TRUE; return Status; }
  Device->PowerState=PowerDeviceD0;
  (void)PoSetPowerState(Device->Device,DevicePowerState,
    IoGetCurrentIrpStackLocation(Irp)->Parameters.Power.State);
  return STATUS_SUCCESS;
}

static VOID MtkUfsPowerWorker(PDEVICE_OBJECT DeviceObject,PVOID Context) {
  MTK_UFS_POWER_DEVICE *Device=Context;
  PIRP Irp=Device->PowerWorkIrp;
  PIO_STACK_LOCATION Stack=IoGetCurrentIrpStackLocation(Irp);
  DEVICE_POWER_STATE State=Stack->Parameters.Power.State.DeviceState;
  NTSTATUS Status;
  UNREFERENCED_PARAMETER(DeviceObject);
  if(KeGetCurrentIrql()!=PASSIVE_LEVEL) Status=STATUS_INVALID_DEVICE_STATE;
  else if(InterlockedCompareExchange(&Device->Removing,0,0)) Status=MtkUfsForwardPowerSync(Device,Irp);
  else if(State==PowerDeviceD3) Status=MtkUfsHandleD3(Device,Irp);
  else if(State==PowerDeviceD0) Status=MtkUfsHandleD0(Device,Irp);
  else Status=STATUS_NOT_SUPPORTED;
  Irp->IoStatus.Status=MtkUfsPowerExactStatus(Status);
  Irp->IoStatus.Information=0;
  Device->PowerWorkIrp=NULL;
  InterlockedExchange(&Device->PowerWorkActive,0);
  IoCompleteRequest(Irp,IO_NO_INCREMENT);
  IoReleaseRemoveLock(&Device->RemoveLock,Irp);
}

static NTSTATUS MtkUfsPassComplete(PDEVICE_OBJECT Device,PIRP Irp,PVOID Context) {
  MTK_UFS_POWER_DEVICE *Extension=Context;
  if(Irp->PendingReturned) IoMarkIrpPending(Irp);
  IoReleaseRemoveLock(&Extension->RemoveLock,Irp);
  UNREFERENCED_PARAMETER(Device);
  return STATUS_CONTINUE_COMPLETION;
}

static NTSTATUS MtkUfsCheckHardwareId(PDEVICE_OBJECT Pdo) {
  WCHAR Buffer[128];
  WCHAR ExpectedText[]=L"ACPI\\HANA678B";
  UNICODE_STRING Expected={sizeof(ExpectedText)-sizeof(WCHAR),sizeof(ExpectedText),ExpectedText};
  ULONG Bytes=0,Count,Offset;
  NTSTATUS Status=IoGetDeviceProperty(Pdo,DevicePropertyHardwareID,sizeof(Buffer),Buffer,&Bytes);
  if(Status!=STATUS_SUCCESS) return MtkUfsPowerExactStatus(Status);
  if(Bytes<2*sizeof(WCHAR) || Bytes>sizeof(Buffer) || (Bytes&1) ||
     Buffer[Bytes/sizeof(WCHAR)-1] || Buffer[Bytes/sizeof(WCHAR)-2]) return STATUS_DEVICE_CONFIGURATION_ERROR;
  Count=Bytes/sizeof(WCHAR); Offset=0;
  while(Offset<Count && Buffer[Offset]) {
    ULONG End=Offset;
    while(End<Count && Buffer[End]) End++;
    if(End<Count) {
      UNICODE_STRING Id={(USHORT)((End-Offset)*sizeof(WCHAR)),(USHORT)((End-Offset)*sizeof(WCHAR)),Buffer+Offset};
      if(RtlEqualUnicodeString(&Id,&Expected,TRUE)) return STATUS_SUCCESS;
    }
    Offset=End+1;
  }
  return STATUS_NO_SUCH_DEVICE;
}

static LONG MtkUfsUsageIndex(DEVICE_USAGE_NOTIFICATION_TYPE Type) {
  switch(Type) {
  case DeviceUsageTypePaging: return 0;
  case DeviceUsageTypeHibernation: return 1;
  case DeviceUsageTypeDumpFile: return 2;
  default: return -1;
  }
}

static ULONGLONG MtkUfsUsageTotal(const MTK_UFS_POWER_DEVICE *Device) {
  return (ULONGLONG)Device->UsageCount[0]+Device->UsageCount[1]+Device->UsageCount[2];
}

static NTSTATUS MtkUfsUsageComplete(PDEVICE_OBJECT Device,PIRP Irp,PVOID Context) {
  UNREFERENCED_PARAMETER(Device);
  if(Irp->PendingReturned) KeSetEvent((PKEVENT)Context,IO_NO_INCREMENT,FALSE);
  else KeSetEvent((PKEVENT)Context,IO_NO_INCREMENT,FALSE);
  return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS MtkUfsForwardUsageSynchronously(MTK_UFS_POWER_DEVICE *Device,PIRP Irp) {
  KEVENT Event;
  NTSTATUS Status;
  KeInitializeEvent(&Event,NotificationEvent,FALSE);
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp,MtkUfsUsageComplete,&Event,TRUE,TRUE,TRUE);
  Status=IoCallDriver(Device->Lower,Irp);
  if(Status==STATUS_PENDING)
    (void)KeWaitForSingleObject(&Event,Executive,KernelMode,FALSE,NULL);
  return Irp->IoStatus.Status;
}

static NTSTATUS MtkUfsUsageNotification(MTK_UFS_POWER_DEVICE *Device,PIRP Irp,
                                        PIO_STACK_LOCATION Stack) {
  LONG Index=MtkUfsUsageIndex(Stack->Parameters.UsageNotification.Type);
  BOOLEAN InPath=Stack->Parameters.UsageNotification.InPath;
  BOOLEAN WasPagable,SetPagable=FALSE;
  ULONGLONG Total;
  NTSTATUS Status;
  if(Index<0) {
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,MtkUfsPassComplete,Device,TRUE,TRUE,TRUE);
    return IoCallDriver(Device->Lower,Irp);
  }
  if(KeGetCurrentIrql()!=PASSIVE_LEVEL) {
    Status=STATUS_INVALID_DEVICE_STATE;
    Irp->IoStatus.Status=Status;
    IoCompleteRequest(Irp,IO_NO_INCREMENT);
    IoReleaseRemoveLock(&Device->RemoveLock,Irp);
    return Status;
  }

  (void)KeWaitForSingleObject(&Device->UsageMutex,Executive,KernelMode,FALSE,NULL);
  Total=MtkUfsUsageTotal(Device);
  if((InPath && Device->UsageCount[Index]==MAXULONG) ||
     (!InPath && Device->UsageCount[Index]==0U)) {
    Status=InPath?STATUS_INSUFFICIENT_RESOURCES:STATUS_INVALID_DEVICE_STATE;
    (void)KeReleaseMutex(&Device->UsageMutex,FALSE);
    Irp->IoStatus.Status=Status;
    IoCompleteRequest(Irp,IO_NO_INCREMENT);
    IoReleaseRemoveLock(&Device->RemoveLock,Irp);
    return Status;
  }

  WasPagable=(BOOLEAN)((Device->Device->Flags&DO_POWER_PAGABLE)!=0);
  if(!InPath && Total==1U && Device->LowerPowerPagable &&
     !(Device->Device->Flags&DO_POWER_INRUSH)) {
    /* A pageable lower driver may restore this bit while completing the IRP.
     * Set ours first so an intervening power IRP still runs at PASSIVE_LEVEL. */
    Device->Device->Flags|=DO_POWER_PAGABLE;
    SetPagable=(BOOLEAN)!WasPagable;
  }

  Status=MtkUfsForwardUsageSynchronously(Device,Irp);
  if(NT_SUCCESS(Status)) {
    if(InPath) Device->UsageCount[Index]++;
    else Device->UsageCount[Index]--;
    Total=MtkUfsUsageTotal(Device);
    if(Total!=0U) {
      Device->Device->Flags&=~DO_POWER_PAGABLE;
    } else if(!(Device->Device->Flags&DO_POWER_INRUSH)) {
      Device->LowerPowerPagable=(BOOLEAN)(
          (Device->Lower->Flags&DO_POWER_PAGABLE)!=0);
      if(Device->LowerPowerPagable) Device->Device->Flags|=DO_POWER_PAGABLE;
      else Device->Device->Flags&=~DO_POWER_PAGABLE;
    }
  } else if(SetPagable) {
    Device->Device->Flags&=~DO_POWER_PAGABLE;
  }
  (void)KeReleaseMutex(&Device->UsageMutex,FALSE);
  Irp->IoStatus.Status=MtkUfsPowerExactStatus(Status);
  Status=Irp->IoStatus.Status;
  IoCompleteRequest(Irp,IO_NO_INCREMENT);
  IoReleaseRemoveLock(&Device->RemoveLock,Irp);
  return Status;
}

static NTSTATUS MtkUfsPowerDispatch(PDEVICE_OBJECT DeviceObject,PIRP Irp) {
  MTK_UFS_POWER_DEVICE *Device=DeviceObject?DeviceObject->DeviceExtension:NULL;
  PIO_STACK_LOCATION Stack;
  NTSTATUS Status;
  if(!Irp) return STATUS_INVALID_PARAMETER;
  if(!Device || Device->Signature!=UFS_POWER_DEVICE_SIGNATURE) {
    Irp->IoStatus.Status=STATUS_INVALID_PARAMETER; IoCompleteRequest(Irp,IO_NO_INCREMENT);
    return STATUS_INVALID_PARAMETER;
  }
  Stack=IoGetCurrentIrpStackLocation(Irp);
  if(!Stack) { Irp->IoStatus.Status=STATUS_INVALID_PARAMETER; IoCompleteRequest(Irp,IO_NO_INCREMENT); return STATUS_INVALID_PARAMETER; }
  if(Stack->MajorFunction==IRP_MJ_PNP && Stack->MinorFunction==IRP_MN_REMOVE_DEVICE) {
    InterlockedExchange(&Device->Removing,1);
    Status=IoAcquireRemoveLock(&Device->RemoveLock,Irp);
    if(!NT_SUCCESS(Status)) { Irp->IoStatus.Status=Status; IoCompleteRequest(Irp,IO_NO_INCREMENT); return Status; }
    IoReleaseRemoveLockAndWait(&Device->RemoveLock,Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    Status=IoCallDriver(Device->Lower,Irp);
    IoDetachDevice(Device->Lower);
    if(Device->PowerWorkItem) {
      IoFreeWorkItem(Device->PowerWorkItem);
      Device->PowerWorkItem=NULL;
    }
    Device->Signature=0;
    IoDeleteDevice(DeviceObject);
    return Status;
  }
  if(Stack->MajorFunction!=IRP_MJ_PNP && Stack->MajorFunction!=IRP_MJ_POWER &&
     InterlockedCompareExchange(&Device->Removing,0,0)) {
    Irp->IoStatus.Status=STATUS_DEVICE_REMOVED; IoCompleteRequest(Irp,IO_NO_INCREMENT); return STATUS_DEVICE_REMOVED;
  }
  Status=IoAcquireRemoveLock(&Device->RemoveLock,Irp);
  if(!NT_SUCCESS(Status)) { Irp->IoStatus.Status=Status; IoCompleteRequest(Irp,IO_NO_INCREMENT); return Status; }
  if(Stack->MajorFunction==IRP_MJ_PNP && Stack->MinorFunction==IRP_MN_SURPRISE_REMOVAL)
    InterlockedExchange(&Device->Removing,1);
  if(InterlockedCompareExchange(&Device->Removing,0,0)) goto Forward;
  if(Stack->MajorFunction==IRP_MJ_PNP && Stack->MinorFunction==IRP_MN_DEVICE_USAGE_NOTIFICATION)
    return MtkUfsUsageNotification(Device,Irp,Stack);
  if(Stack->MajorFunction==IRP_MJ_POWER && Stack->MinorFunction==IRP_MN_SET_POWER &&
     Stack->Parameters.Power.Type==DevicePowerState &&
     Stack->Parameters.Power.State.DeviceState!=PowerDeviceD0 &&
     Stack->Parameters.Power.State.DeviceState!=PowerDeviceD3) {
    IoReleaseRemoveLock(&Device->RemoveLock,Irp);
    Irp->IoStatus.Status=STATUS_NOT_SUPPORTED; IoCompleteRequest(Irp,IO_NO_INCREMENT);
    return STATUS_NOT_SUPPORTED;
  }
  if(Stack->MajorFunction==IRP_MJ_POWER && Stack->MinorFunction==IRP_MN_SET_POWER &&
     Stack->Parameters.Power.Type==DevicePowerState &&
     (Stack->Parameters.Power.State.DeviceState==PowerDeviceD0 ||
      Stack->Parameters.Power.State.DeviceState==PowerDeviceD3)) {
    if(InterlockedCompareExchange(&Device->PowerWorkActive,1,0)!=0) {
      IoReleaseRemoveLock(&Device->RemoveLock,Irp);
      Irp->IoStatus.Status=STATUS_DEVICE_BUSY; IoCompleteRequest(Irp,IO_NO_INCREMENT); return STATUS_DEVICE_BUSY;
    }
    if(!Device->PowerWorkItem) {
      InterlockedExchange(&Device->PowerWorkActive,0);
      IoReleaseRemoveLock(&Device->RemoveLock,Irp);
      Irp->IoStatus.Status=STATUS_INSUFFICIENT_RESOURCES;
      IoCompleteRequest(Irp,IO_NO_INCREMENT);
      return STATUS_INSUFFICIENT_RESOURCES;
    }
    Device->PowerWorkIrp=Irp;
    IoMarkIrpPending(Irp);
    IoQueueWorkItem(Device->PowerWorkItem,MtkUfsPowerWorker,CriticalWorkQueue,Device);
    return STATUS_PENDING;
  }
Forward:
  IoCopyCurrentIrpStackLocationToNext(Irp);
  IoSetCompletionRoutine(Irp,MtkUfsPassComplete,Device,TRUE,TRUE,TRUE);
  if(Stack->MajorFunction==IRP_MJ_POWER) return PoCallDriver(Device->Lower,Irp);
  return IoCallDriver(Device->Lower,Irp);
}

static NTSTATUS MtkUfsPowerAddDevice(PDRIVER_OBJECT DriverObject,PDEVICE_OBJECT Pdo) {
  PDEVICE_OBJECT DeviceObject=NULL;
  MTK_UFS_POWER_DEVICE *Device;
  NTSTATUS Status;
  if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !DriverObject || !Pdo) return STATUS_INVALID_PARAMETER;
  Status=MtkUfsCheckHardwareId(Pdo);
  if(!NT_SUCCESS(Status)) return Status;
  Status=IoCreateDevice(DriverObject,sizeof(*Device),NULL,Pdo->DeviceType,FILE_DEVICE_SECURE_OPEN,FALSE,&DeviceObject);
  if(!NT_SUCCESS(Status)) return MtkUfsPowerExactStatus(Status);
  Device=DeviceObject->DeviceExtension; RtlZeroMemory(Device,sizeof(*Device));
  Device->Signature=UFS_POWER_DEVICE_SIGNATURE; Device->Device=DeviceObject; Device->Pdo=Pdo;
  Device->PowerState=PowerDeviceD0;
  IoInitializeRemoveLock(&Device->RemoveLock,UFS_POWER_TAG,0,0);
  KeInitializeMutex(&Device->UsageMutex,0);
  KeInitializeSpinLock(&Device->MiniportLock);
  KeInitializeEvent(&Device->MiniportDone,NotificationEvent,FALSE);
  Device->PowerWorkItem=IoAllocateWorkItem(DeviceObject);
  if(!Device->PowerWorkItem) { Status=STATUS_INSUFFICIENT_RESOURCES; goto Fail; }
  Status=IoAttachDeviceToDeviceStackSafe(DeviceObject,Pdo,&Device->Lower);
  if(!NT_SUCCESS(Status)) { Status=MtkUfsPowerExactStatus(Status); goto Fail; }
  DeviceObject->Flags|=Device->Lower->Flags&(DO_BUFFERED_IO|DO_DIRECT_IO|DO_POWER_INRUSH|DO_POWER_PAGABLE);
  if(DeviceObject->Flags&DO_POWER_INRUSH) DeviceObject->Flags&=~DO_POWER_PAGABLE;
  Device->LowerPowerPagable=(BOOLEAN)((Device->Lower->Flags&DO_POWER_PAGABLE)!=0 &&
                                      !(DeviceObject->Flags&DO_POWER_INRUSH));
  if(DeviceObject->AlignmentRequirement<Device->Lower->AlignmentRequirement)
    DeviceObject->AlignmentRequirement=Device->Lower->AlignmentRequirement;
  DeviceObject->Flags&=~DO_DEVICE_INITIALIZING;
  return STATUS_SUCCESS;
Fail:
  if(Device->PowerWorkItem) {
    IoFreeWorkItem(Device->PowerWorkItem);
    Device->PowerWorkItem=NULL;
  }
  Device->Signature=0; IoDeleteDevice(DeviceObject); return Status;
}

static void MtkUfsPowerUnload(PDRIVER_OBJECT DriverObject) {
  UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,PUNICODE_STRING RegistryPath) {
  ULONG I;
  UNREFERENCED_PARAMETER(RegistryPath);
  if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !DriverObject || !DriverObject->DriverExtension || DriverObject->DeviceObject)
    return STATUS_INVALID_PARAMETER;
  for(I=0;I<=IRP_MJ_MAXIMUM_FUNCTION;I++) DriverObject->MajorFunction[I]=MtkUfsPowerDispatch;
  DriverObject->DriverExtension->AddDevice=MtkUfsPowerAddDevice;
  DriverObject->DriverUnload=MtkUfsPowerUnload;
  return STATUS_SUCCESS;
}
