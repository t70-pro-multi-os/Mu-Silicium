// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntddk.h>
#include <wdf.h>
#include <reshub.h>
#include <spb.h>
#include <vhf.h>
#include <acpiioct.h>
#include <Library/HimaxCore.h>
#include "InputReport.h"

#define EVENT_BYTES HM_T70_REPORT_BYTES
#define HM_FRAME_QUEUE_DEPTH 4
#define HM_FAILURE_LIMIT 3
#define HM_RESUME_BOARD_READY 1UL
#define HM_RESUME_SEQUENCE_MASK 0x00ffffffUL

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD HmDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE HmPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE HmRelease;
EVT_WDF_DEVICE_D0_ENTRY HmD0Entry;
EVT_WDF_DEVICE_D0_EXIT HmD0Exit;
EVT_WDF_WORKITEM HmReadWork;
EVT_WDF_OBJECT_CONTEXT_CLEANUP HmCleanup;
EVT_WDF_INTERRUPT_ISR HmInterruptIsr;
EVT_WDF_INTERRUPT_ENABLE HmInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE HmInterruptDisable;
EVT_VHF_READY_FOR_NEXT_READ_REPORT HmVhfReady;
EVT_VHF_ASYNC_OPERATION HmGetFeature;

typedef struct {
  WDFDEVICE Device;
  WDFIOTARGET Spb;
  WDFINTERRUPT Interrupt;
  WDFWORKITEM Work;
  VHFHANDLE Vhf;
  ULONG ConnectionIdLow,ConnectionIdHigh,Speed;
  USHORT Address;
  BOOLEAN InitialEntryCompleted;
  ULONG D0Sequence;
  volatile LONG Active,ReadyValidated,InterruptFaulted,InterruptDisabled,WorkQueued,VhfReady;
  volatile LONG FailureCount,FailureEpoch,ReleasePending,HasPendingFrame;
  KSPIN_LOCK FrameLock;
  HM_FRAME Frames[HM_FRAME_QUEUE_DEPTH]; UCHAR FrameHead,FrameCount;
  HM_FRAME PendingFrame;
  HM_HID_STATE HidState;
  HM_IO HimaxIo;
  HM_HID_REPORT Report;
  HID_XFER_PACKET Transfer;
} DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT,DeviceContext)

#define HM_FINGER \
  0x09,0x22,0xa1,0x02, \
  0x09,0x51,0x15,0x01,0x25,0x0a,0x75,0x08,0x95,0x01,0x81,0x02, \
  0x09,0x42,0x09,0x32,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x02,0x81,0x02, \
  0x75,0x06,0x95,0x01,0x81,0x03, \
  0x05,0x01,0x09,0x30,0x15,0x00,0x26,0x78,0x05,0x75,0x10,0x95,0x01,0x81,0x02, \
  0x09,0x31,0x15,0x00,0x26,0xc0,0x08,0x75,0x10,0x95,0x01,0x81,0x02, \
  0x05,0x0d,0x09,0x30,0x15,0x00,0x25,0xff,0x75,0x08,0x95,0x01,0x81,0x02,0xc0

static UCHAR ReportDescriptor[]={
  0x05,0x0d,0x09,0x04,0xa1,0x01,
  0x85,0x02,0x09,0x55,0x15,0x00,0x25,0x0a,0x75,0x08,0x95,0x01,0xb1,0x02,
  0x85,0x01,0x09,0x54,0x15,0x00,0x25,0x0a,0x75,0x08,0x95,0x01,0x81,0x02,
  HM_FINGER,HM_FINGER,HM_FINGER,HM_FINGER,HM_FINGER,
  HM_FINGER,HM_FINGER,HM_FINGER,HM_FINGER,HM_FINGER,0xc0
};

static BOOLEAN HmIsActive(DEVICE_CONTEXT *D) {
  return InterlockedCompareExchange(&D->Active,0,0)!=0 &&
         InterlockedCompareExchange(&D->ReadyValidated,0,0)!=0;
}

static BOOLEAN HmQueueHasFrames(DEVICE_CONTEXT *D) {
  KIRQL OldIrql; BOOLEAN Has;
  KeAcquireSpinLock(&D->FrameLock,&OldIrql); Has=D->FrameCount!=0;
  KeReleaseSpinLock(&D->FrameLock,OldIrql); return Has;
}

static void HmQueueWork(DEVICE_CONTEXT *D) {
  if (HmIsActive(D) && InterlockedCompareExchange(&D->WorkQueued,1,0)==0)
    WdfWorkItemEnqueue(D->Work);
}

static void HmClearFrames(DEVICE_CONTEXT *D);

static void HmRecordFailure(DEVICE_CONTEXT *D) {
  if (InterlockedIncrement(&D->FailureCount)>=HM_FAILURE_LIMIT) {
    KIRQL OldIrql; LONG Epoch;
    InterlockedExchange(&D->InterruptFaulted,1);
    KeAcquireSpinLock(&D->FrameLock,&OldIrql);
    Epoch=InterlockedIncrement(&D->FailureEpoch);
    if (!Epoch) Epoch=InterlockedIncrement(&D->FailureEpoch);
    InterlockedExchange(&D->ReleasePending,Epoch);
    InterlockedExchange(&D->HasPendingFrame,0);
    D->FrameHead=0; D->FrameCount=0;
    KeReleaseSpinLock(&D->FrameLock,OldIrql);
  }
}

static void HmQueueFrame(DEVICE_CONTEXT *D,const HM_FRAME *Frame) {
  KIRQL OldIrql; UCHAR Tail;
  KeAcquireSpinLock(&D->FrameLock,&OldIrql);
  if (D->FrameCount==HM_FRAME_QUEUE_DEPTH) {
    /* Resynchronize after dropping history: emit all-up before this latest frame. */
    LONG Epoch=InterlockedIncrement(&D->FailureEpoch);
    if (!Epoch) Epoch=InterlockedIncrement(&D->FailureEpoch);
    D->FrameHead=0; D->FrameCount=0;
    InterlockedExchange(&D->HasPendingFrame,0);
    InterlockedExchange(&D->ReleasePending,Epoch);
  }
  Tail=(UCHAR)((D->FrameHead+D->FrameCount)%HM_FRAME_QUEUE_DEPTH);
  D->Frames[Tail]=*Frame; D->FrameCount++;
  KeReleaseSpinLock(&D->FrameLock,OldIrql);
}

static BOOLEAN HmDequeueFrame(DEVICE_CONTEXT *D,HM_FRAME *Frame) {
  KIRQL OldIrql; BOOLEAN Found=FALSE;
  KeAcquireSpinLock(&D->FrameLock,&OldIrql);
  if (D->FrameCount) {
    *Frame=D->Frames[D->FrameHead];
    D->FrameHead=(UCHAR)((D->FrameHead+1)%HM_FRAME_QUEUE_DEPTH); D->FrameCount--; Found=TRUE;
  }
  KeReleaseSpinLock(&D->FrameLock,OldIrql); return Found;
}

static void HmClearFrames(DEVICE_CONTEXT *D) {
  KIRQL OldIrql;
  KeAcquireSpinLock(&D->FrameLock,&OldIrql);
  D->FrameHead=0; D->FrameCount=0;
  KeReleaseSpinLock(&D->FrameLock,OldIrql);
}

static LONG HmRequestRelease(DEVICE_CONTEXT *D) {
  KIRQL OldIrql; LONG Epoch;
  KeAcquireSpinLock(&D->FrameLock,&OldIrql);
  Epoch=InterlockedIncrement(&D->FailureEpoch);
  if (!Epoch) Epoch=InterlockedIncrement(&D->FailureEpoch);
  InterlockedExchange(&D->ReleasePending,Epoch);
  KeReleaseSpinLock(&D->FrameLock,OldIrql);
  return Epoch;
}

static VOID CopyAcpiIntegerBytes(PVOID Destination,const __unaligned UCHAR *Source,ULONG Bytes) {
  UCHAR *Output=(UCHAR *)Destination;
  ULONG Index;
  for (Index=0;Index<Bytes;Index++) Output[Index]=Source[Index];
}
static BOOLEAN HmFirmwareInteger32(PACPI_METHOD_ARGUMENT Argument,PULONG Value) {
  ULONGLONG Wide;
  if (!Argument || !Value || Argument->Type!=ACPI_METHOD_ARGUMENT_INTEGER) return FALSE;
  if (Argument->DataLength==sizeof(ULONG)) {
    CopyAcpiIntegerBytes(Value,Argument->Data,sizeof(*Value)); return TRUE;
  }
  if (Argument->DataLength!=sizeof(ULONGLONG)) return FALSE;
  CopyAcpiIntegerBytes(&Wide,Argument->Data,sizeof(Wide));
  if (Wide>MAXULONG) return FALSE;
  *Value=(ULONG)Wide; return TRUE;
}

static NTSTATUS HmFirmwareReady(DEVICE_CONTEXT *D) {
  ACPI_EVAL_INPUT_BUFFER In={0};
  UCHAR Storage[sizeof(ACPI_EVAL_OUTPUT_BUFFER)+sizeof(ULONGLONG)]={0};
  PACPI_EVAL_OUTPUT_BUFFER Out=(PACPI_EVAL_OUTPUT_BUFFER)Storage;
  WDF_MEMORY_DESCRIPTOR Input,Output; WDF_REQUEST_SEND_OPTIONS Options;
  ULONG_PTR Returned=0; ULONG Value; NTSTATUS S;
  In.Signature=ACPI_EVAL_INPUT_BUFFER_SIGNATURE; RtlCopyMemory(In.MethodName,"HMXR",4);
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input,&In,sizeof(In));
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Output,Storage,sizeof(Storage));
  WDF_REQUEST_SEND_OPTIONS_INIT(&Options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
  WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options,WDF_REL_TIMEOUT_IN_SEC(1));
  S=WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(D->Device),NULL,
       IOCTL_ACPI_EVAL_METHOD,&Input,&Output,&Options,&Returned);
  if (!NT_SUCCESS(S)) return S;
  if (Returned<sizeof(ACPI_EVAL_OUTPUT_BUFFER) || Returned>sizeof(Storage) ||
      Out->Signature!=ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || Out->Count!=1 ||
      Out->Length>Returned || Out->Length<sizeof(ACPI_EVAL_OUTPUT_BUFFER) ||
      Out->Length<FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+Out->Argument[0].DataLength ||
      Returned<FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+Out->Argument[0].DataLength ||
      !HmFirmwareInteger32(&Out->Argument[0],&Value) || Value!=1)
    return STATUS_DEVICE_NOT_READY;
  return STATUS_SUCCESS;
}

/* HMRD samples the retained reset and IRQ pads for this D0 entry. */
static NTSTATUS HmResumeReady(DEVICE_CONTEXT *D) {
  ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER In={0};
  UCHAR Storage[sizeof(ACPI_EVAL_OUTPUT_BUFFER)+sizeof(ULONGLONG)]={0};
  PACPI_EVAL_OUTPUT_BUFFER Out=(PACPI_EVAL_OUTPUT_BUFFER)Storage;
  WDF_MEMORY_DESCRIPTOR Input,Output; WDF_REQUEST_SEND_OPTIONS Options;
  ULONG_PTR Returned=0; ULONG Sequence,Value; ULONGLONG Expected; NTSTATUS S;
  Sequence=(D->D0Sequence+1)&HM_RESUME_SEQUENCE_MASK;
  if (!Sequence) Sequence=1;
  D->D0Sequence=Sequence;
  In.Signature=ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE;
  RtlCopyMemory(In.MethodName,"HMRD",4); In.IntegerArgument=Sequence;
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input,&In,sizeof(In));
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Output,Storage,sizeof(Storage));
  WDF_REQUEST_SEND_OPTIONS_INIT(&Options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
  WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options,WDF_REL_TIMEOUT_IN_SEC(1));
  S=WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(D->Device),NULL,
       IOCTL_ACPI_EVAL_METHOD,&Input,&Output,&Options,&Returned);
  if (!NT_SUCCESS(S)) return S;
  if (Returned<sizeof(ACPI_EVAL_OUTPUT_BUFFER) || Returned>sizeof(Storage) ||
      Out->Signature!=ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || Out->Count!=1 ||
      Out->Length>Returned || Out->Length<sizeof(ACPI_EVAL_OUTPUT_BUFFER) ||
      Out->Argument[0].Type!=ACPI_METHOD_ARGUMENT_INTEGER ||
      Out->Length<FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+Out->Argument[0].DataLength ||
      Returned<FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+Out->Argument[0].DataLength ||
      Out->Length<FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+Out->Argument[0].DataLength ||
      Returned<FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+Out->Argument[0].DataLength ||
      !HmFirmwareInteger32(&Out->Argument[0],&Value))
    return STATUS_DEVICE_NOT_READY;
  Expected=((ULONGLONG)Sequence<<8)|HM_RESUME_BOARD_READY;
  if ((ULONGLONG)Value!=Expected) return STATUS_DEVICE_NOT_READY;
  return STATUS_SUCCESS;
}

static void HmCloseSpb(DEVICE_CONTEXT *D) {
  if (D->Spb) {
    WdfIoTargetClose(D->Spb); WdfObjectDelete(D->Spb); D->Spb=NULL;
  }
}

static NTSTATUS HmOpenSpb(DEVICE_CONTEXT *D) {
  static const WCHAR Prefix[]=L"\\Device\\RESOURCE_HUB\\";
  static const WCHAR Hex[]=L"0123456789abcdef";
  WCHAR Buffer[64]; UNICODE_STRING Name; WDF_IO_TARGET_OPEN_PARAMS Open;
  WDF_OBJECT_ATTRIBUTES Attributes; NTSTATUS S; ULONGLONG Id;
  ULONG I,Position=(ULONG)(RTL_NUMBER_OF(Prefix)-1);
  BOOLEAN Opened=FALSE;
  if (Position+16+1>RTL_NUMBER_OF(Buffer)) return STATUS_BUFFER_TOO_SMALL;
  RtlCopyMemory(Buffer,Prefix,Position*sizeof(WCHAR));
  Id=((ULONGLONG)D->ConnectionIdHigh<<32)|D->ConnectionIdLow;
  for (I=0;I<16;I++) Buffer[Position+I]=Hex[(Id>>((15-I)*4))&0xf];
  Buffer[Position+16]=0; RtlInitUnicodeString(&Name,Buffer);
  WDF_OBJECT_ATTRIBUTES_INIT(&Attributes); Attributes.ParentObject=D->Device;
  S=WdfIoTargetCreate(D->Device,&Attributes,&D->Spb); if (!NT_SUCCESS(S)) return S;
  WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Open,&Name,FILE_READ_DATA|FILE_WRITE_DATA);
  S=WdfIoTargetOpen(D->Spb,&Open); if (!NT_SUCCESS(S)) goto Failed;
  Opened=TRUE;
  /* The resource-hub path is reparsed to the MtkI2c SPB target. MtkI2c's
   * TargetConnect validates the ACPI connection descriptor during this open;
   * querying it again on the SPB target sends an unsupported IOCTL to SpbCx. */
  D->Address=0x48; D->Speed=400000;
  return STATUS_SUCCESS;
Failed:
  if (Opened) WdfIoTargetClose(D->Spb);
  WdfObjectDelete(D->Spb); D->Spb=NULL; return S;
}

/* Validate both resource lists before opening SPB or changing controller state. */
static NTSTATUS HmResources(WDFCMRESLIST Raw,WDFCMRESLIST Translated,
  ULONG *Low,ULONG *High,PCM_PARTIAL_RESOURCE_DESCRIPTOR *RawInterrupt,
  PCM_PARTIAL_RESOURCE_DESCRIPTOR *TranslatedInterrupt) {
  ULONG Pass,I,IdLow=0,IdHigh=0;
  if (!Raw || !Translated || !Low || !High || !RawInterrupt || !TranslatedInterrupt)
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  *RawInterrupt=NULL; *TranslatedInterrupt=NULL;
  for (Pass=0;Pass<2;Pass++) {
    WDFCMRESLIST List=Pass?Translated:Raw;
    BOOLEAN Connection=FALSE; PCM_PARTIAL_RESOURCE_DESCRIPTOR Interrupt=NULL;
    if (WdfCmResourceListGetCount(List)!=2) return STATUS_DEVICE_CONFIGURATION_ERROR;
    for (I=0;I<2;I++) {
      PCM_PARTIAL_RESOURCE_DESCRIPTOR R=WdfCmResourceListGetDescriptor(List,I);
      if (!R) return STATUS_DEVICE_CONFIGURATION_ERROR;
      if (R->Type==CmResourceTypeConnection) {
        if (Connection || R->u.Connection.Class!=CM_RESOURCE_CONNECTION_CLASS_SERIAL ||
            R->u.Connection.Type!=CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
          return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (!Pass) { IdLow=R->u.Connection.IdLowPart; IdHigh=R->u.Connection.IdHighPart; }
        else if (IdLow!=R->u.Connection.IdLowPart || IdHigh!=R->u.Connection.IdHighPart)
          return STATUS_DEVICE_CONFIGURATION_ERROR;
        Connection=TRUE;
      } else if (R->Type==CmResourceTypeInterrupt) {
        if (Interrupt) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (R->ShareDisposition!=CmResourceShareDeviceExclusive &&
            R->ShareDisposition!=CmResourceShareDriverExclusive)
          return STATUS_NOT_SUPPORTED;
        if (!(R->Flags&CM_RESOURCE_INTERRUPT_LATCHED) || (R->Flags&CM_RESOURCE_INTERRUPT_MESSAGE))
          return STATUS_NOT_SUPPORTED;
        Interrupt=R;
      } else return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (!Connection || !Interrupt) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (Pass) *TranslatedInterrupt=Interrupt; else *RawInterrupt=Interrupt;
  }
  *Low=IdLow; *High=IdHigh;
  return STATUS_SUCCESS;
}

NTSTATUS HmPrepare(WDFDEVICE Device,WDFCMRESLIST Raw,WDFCMRESLIST Resources) {
  DEVICE_CONTEXT *D=DeviceContext(Device);
  PCM_PARTIAL_RESOURCE_DESCRIPTOR RawInterrupt=NULL,TranslatedInterrupt=NULL;
  ULONG Low,High; NTSTATUS S;
  if (D->Spb || D->Interrupt) return STATUS_INVALID_DEVICE_STATE;
  S=HmResources(Raw,Resources,&Low,&High,&RawInterrupt,&TranslatedInterrupt);
  if (!NT_SUCCESS(S)) return S;
  D->ConnectionIdLow=Low; D->ConnectionIdHigh=High;
  S=HmOpenSpb(D); if (!NT_SUCCESS(S)) return S;
  {
    WDF_INTERRUPT_CONFIG Config; WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_INTERRUPT_CONFIG_INIT(&Config,HmInterruptIsr,NULL);
    Config.PassiveHandling=TRUE; Config.InterruptRaw=RawInterrupt;
    Config.InterruptTranslated=TranslatedInterrupt;
    Config.EvtInterruptEnable=HmInterruptEnable;
    Config.EvtInterruptDisable=HmInterruptDisable;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes); Attributes.ParentObject=Device;
    S=WdfInterruptCreate(Device,&Config,&Attributes,&D->Interrupt);
  }
  if (!NT_SUCCESS(S)) { HmCloseSpb(D); return S; }
  return STATUS_SUCCESS;
}

NTSTATUS HmRelease(WDFDEVICE Device,WDFCMRESLIST Resources) {
  DEVICE_CONTEXT *D=DeviceContext(Device); UNREFERENCED_PARAMETER(Resources);
  InterlockedExchange(&D->Active,0); InterlockedExchange(&D->ReadyValidated,0);
  WdfWorkItemFlush(D->Work);
  /* WDF deletes this PrepareHardware interrupt after release returns. */
  D->Interrupt=NULL;
  HmCloseSpb(D); D->Address=0; D->Speed=0; return STATUS_SUCCESS;
}

static HM_STATUS HmSpbTransfer(void *Context,const unsigned char *WriteData,
                               unsigned WriteLength,unsigned char *ReadData,
                               unsigned ReadLength) {
  DEVICE_CONTEXT *D=(DEVICE_CONTEXT *)Context;
  SPB_TRANSFER_LIST_AND_ENTRIES(2) List={0};
  WDF_MEMORY_DESCRIPTOR Input; WDF_REQUEST_SEND_OPTIONS Options;
  ULONG_PTR Bytes=0; ULONG Count=ReadLength?2:1; NTSTATUS S;
  if (!D || !D->Spb || !WriteData || !WriteLength ||
      WriteLength>HM_BUS_WRITE_MAX_BODY+1 ||
      (ReadLength && !ReadData) || ReadLength>HM_BUS_READ_MAX_BYTES)
    return HmInvalid;
  SPB_TRANSFER_LIST_INIT(&List.List,Count);
  List.List.Transfers[0]=SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
    SpbTransferDirectionToDevice,0,(PUCHAR)WriteData,WriteLength);
  if (ReadLength) List.List.Transfers[1]=SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
    SpbTransferDirectionFromDevice,0,ReadData,ReadLength);
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input,&List,sizeof(List));
  WDF_REQUEST_SEND_OPTIONS_INIT(&Options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
  WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options,WDF_REL_TIMEOUT_IN_MS(50));
  S=WdfIoTargetSendIoctlSynchronously(D->Spb,NULL,IOCTL_SPB_EXECUTE_SEQUENCE,
                                       &Input,NULL,&Options,&Bytes);
  if (!NT_SUCCESS(S) || Bytes!=(ULONG_PTR)(WriteLength+ReadLength))
    return HmIoError;
  return HmOk;
}

static void HmTransportDelay(void *Context,unsigned MinimumUs,unsigned MaximumUs) {
  LARGE_INTEGER Interval; UNREFERENCED_PARAMETER(Context); UNREFERENCED_PARAMETER(MaximumUs);
  Interval.QuadPart=-((LONGLONG)MinimumUs*10);
  (VOID)KeDelayExecutionThread(KernelMode,FALSE,&Interval);
}

static BOOLEAN HmReadFrame(DEVICE_CONTEXT *D,HM_FRAME *Frame) {
  UCHAR Raw[EVENT_BYTES];
  return HmBusRead(&D->HimaxIo,HM_EVENT_COMMAND,Raw,sizeof(Raw))==HmOk &&
    HmDecode(Raw,sizeof(Raw),HM_MAX_CONTACTS,1400,2240,Frame)==HmOk;
}

BOOLEAN HmInterruptIsr(WDFINTERRUPT Interrupt,ULONG MessageId) {
  DEVICE_CONTEXT *D=DeviceContext(WdfInterruptGetDevice(Interrupt));
  HM_FRAME Frame={0}; UNREFERENCED_PARAMETER(MessageId);
  if (!HmIsActive(D) || !D->Spb) return TRUE;
  if (HmReadFrame(D,&Frame)) {
    InterlockedExchange(&D->FailureCount,0);
    HmQueueFrame(D,&Frame);
  } else HmRecordFailure(D);
  HmQueueWork(D);
  return TRUE;
}

NTSTATUS HmInterruptEnable(WDFINTERRUPT Interrupt,WDFDEVICE Device) {
  DEVICE_CONTEXT *D=DeviceContext(Device); UNREFERENCED_PARAMETER(Interrupt);
  if (!HmIsActive(D) || InterlockedCompareExchange(&D->InterruptFaulted,0,0))
    return STATUS_DEVICE_NOT_READY;
  return STATUS_SUCCESS;
}

NTSTATUS HmInterruptDisable(WDFINTERRUPT Interrupt,WDFDEVICE Device) {
  UNREFERENCED_PARAMETER(Interrupt); UNREFERENCED_PARAMETER(Device); return STATUS_SUCCESS;
}

static BOOLEAN HmReportEpochCurrent(DEVICE_CONTEXT *D,LONG Epoch) {
  return !InterlockedCompareExchange(&D->InterruptFaulted,0,0) &&
         !InterlockedCompareExchange(&D->ReleasePending,0,0) &&
         InterlockedCompareExchange(&D->FailureEpoch,0,0)==Epoch;
}

static NTSTATUS HmSubmitReport(DEVICE_CONTEXT *D,const HM_HID_REPORT *Candidate,
                               const HM_HID_STATE *CandidateState,BOOLEAN IsRelease,LONG Epoch) {
  NTSTATUS S;
  if (!IsRelease && !HmReportEpochCurrent(D,Epoch)) return STATUS_DEVICE_NOT_READY;
  if (InterlockedCompareExchange(&D->VhfReady,0,1)!=1) return STATUS_PENDING;
  if (!IsRelease && !HmReportEpochCurrent(D,Epoch)) {
    InterlockedCompareExchange(&D->VhfReady,1,0); return STATUS_DEVICE_NOT_READY;
  }
  D->Report=*Candidate;
  if (!IsRelease && !HmReportEpochCurrent(D,Epoch)) {
    InterlockedCompareExchange(&D->VhfReady,1,0); return STATUS_DEVICE_NOT_READY;
  }
  D->Transfer.reportBuffer=(PUCHAR)&D->Report;
  D->Transfer.reportBufferLen=sizeof(D->Report); D->Transfer.reportId=1;
  S=VhfReadReportSubmit(D->Vhf,&D->Transfer);
  if (S==STATUS_SUCCESS) {
    D->HidState=*CandidateState;
    if (IsRelease) {
      InterlockedCompareExchange(&D->ReleasePending,0,Epoch);
    } else {
      InterlockedExchange(&D->HasPendingFrame,0);
    }
    return STATUS_SUCCESS;
  }
  /* A VHF-ready callback permits one submit attempt. Keep data pending and wait for a new callback. */
  return S;
}

void HmReadWork(WDFWORKITEM Work) {
  DEVICE_CONTEXT *D=DeviceContext(WdfWorkItemGetParentObject(Work));
  HM_FRAME Frame={0},Empty={0};
  HM_HID_REPORT Candidate; HM_HID_STATE NextState; NTSTATUS S; LONG Epoch;
  BOOLEAN HasReadyWork,NeedsDisable;
  if (!HmIsActive(D)) goto Done;
  for (;;) {
    if (!HmIsActive(D)) break;
    if (InterlockedCompareExchange(&D->InterruptFaulted,0,0)) {
      if (D->Interrupt && InterlockedCompareExchange(&D->InterruptDisabled,1,0)==0)
        (VOID)WdfInterruptDisable(D->Interrupt);
      InterlockedExchange(&D->HasPendingFrame,0); HmClearFrames(D);
    }
    if (InterlockedCompareExchange(&D->ReleasePending,0,0) &&
        InterlockedCompareExchange(&D->VhfReady,0,0)) {
      if (!HmBuildHidReport(&Empty,&D->HidState,&Candidate,&NextState)) break;
      Epoch=InterlockedCompareExchange(&D->ReleasePending,0,0);
      S=HmSubmitReport(D,&Candidate,&NextState,TRUE,Epoch);
      if (!NT_SUCCESS(S)) break;
      continue;
    }
    if (InterlockedCompareExchange(&D->HasPendingFrame,0,0)) {
      if (!InterlockedCompareExchange(&D->VhfReady,0,0)) break;
      Frame=D->PendingFrame;
      if (!HmBuildHidReport(&Frame,&D->HidState,&Candidate,&NextState)) {
        InterlockedExchange(&D->HasPendingFrame,0); HmRecordFailure(D); continue;
      }
      Epoch=InterlockedCompareExchange(&D->FailureEpoch,0,0);
      S=HmSubmitReport(D,&Candidate,&NextState,FALSE,Epoch);
      if (!NT_SUCCESS(S)) break;
      continue;
    }
    if (!HmDequeueFrame(D,&Frame)) break;
    D->PendingFrame=Frame; InterlockedExchange(&D->HasPendingFrame,1);
  }
Done:
  InterlockedExchange(&D->WorkQueued,0);
  if (HmIsActive(D)) {
    NeedsDisable=InterlockedCompareExchange(&D->InterruptFaulted,0,0) &&
                 D->Interrupt &&
                 !InterlockedCompareExchange(&D->InterruptDisabled,0,0);
    HasReadyWork=InterlockedCompareExchange(&D->VhfReady,0,0) &&
                 (HmQueueHasFrames(D) ||
                  InterlockedCompareExchange(&D->ReleasePending,0,0) ||
                  InterlockedCompareExchange(&D->HasPendingFrame,0,0));
    if (NeedsDisable || HasReadyWork) HmQueueWork(D);
  }
}

NTSTATUS HmD0Entry(WDFDEVICE Device,WDF_POWER_DEVICE_STATE Previous) {
  DEVICE_CONTEXT *D=DeviceContext(Device); KIRQL OldIrql; NTSTATUS S;
  HM_STATUS HimaxStatus; HM_FRAME Frame={0};
  UNREFERENCED_PARAMETER(Previous);
  if (!D->Spb || !D->Interrupt || D->Address!=0x48 || D->Speed!=400000)
    return STATUS_DEVICE_NOT_READY;
  if (HmIsActive(D)) return STATUS_INVALID_DEVICE_STATE;
  if (!D->InitialEntryCompleted) {
    /* HMXR is sampled for every failed initial attempt; it is never reused as
     * evidence after a successful D0 entry. */
    S=HmFirmwareReady(D);
    if (!NT_SUCCESS(S)) return STATUS_DEVICE_NOT_READY;
    HimaxStatus=HmEnter(&D->HimaxIo,HmEntryInitialHandoff);
  } else {
    /* Check the pads, then verify the chip ID and running firmware over SPB.
     * The KMDF interrupt remains disabled until D0 entry succeeds. */
    S=HmResumeReady(D);
    if (!NT_SUCCESS(S)) return STATUS_DEVICE_NOT_READY;
    HimaxStatus=HmEnter(&D->HimaxIo,HmEntryRetainedResume);
  }
  if (HimaxStatus!=HmOk)
    return HimaxStatus==HmInvalid?STATUS_DEVICE_CONFIGURATION_ERROR:STATUS_DEVICE_NOT_READY;
  KeAcquireSpinLock(&D->FrameLock,&OldIrql);
  D->FrameHead=0; D->FrameCount=0;
  KeReleaseSpinLock(&D->FrameLock,OldIrql);
  InterlockedExchange(&D->HasPendingFrame,0);
  InterlockedExchange(&D->FailureCount,0);
  InterlockedExchange(&D->InterruptFaulted,0); InterlockedExchange(&D->InterruptDisabled,0);
  if (D->HidState.ActiveMask) (VOID)HmRequestRelease(D);
  else InterlockedExchange(&D->ReleasePending,0);
  // The GPIO controller can clear an edge that arrived while interrupts were masked.
  // A failed idle snapshot is not an interrupt fault.
  if (HmReadFrame(D,&Frame)) HmQueueFrame(D,&Frame);
  InterlockedExchange(&D->ReadyValidated,1); InterlockedExchange(&D->Active,1);
  D->InitialEntryCompleted=TRUE;
  if (InterlockedCompareExchange(&D->VhfReady,0,0)) HmQueueWork(D);
  return STATUS_SUCCESS;
}

NTSTATUS HmD0Exit(WDFDEVICE Device,WDF_POWER_DEVICE_STATE Target) {
  DEVICE_CONTEXT *D=DeviceContext(Device); KIRQL OldIrql; NTSTATUS S;
  HM_FRAME Empty={0}; HM_HID_REPORT Candidate; HM_HID_STATE NextState; LONG Epoch;
  UNREFERENCED_PARAMETER(Target);
  InterlockedExchange(&D->Active,0); InterlockedExchange(&D->ReadyValidated,0);
  WdfWorkItemFlush(D->Work);
  KeAcquireSpinLock(&D->FrameLock,&OldIrql); D->FrameHead=0; D->FrameCount=0;
  KeReleaseSpinLock(&D->FrameLock,OldIrql);
  InterlockedExchange(&D->HasPendingFrame,0);
  if (D->HidState.ActiveMask) (VOID)HmRequestRelease(D);
  else InterlockedExchange(&D->ReleasePending,0);
  if (D->HidState.ActiveMask && InterlockedCompareExchange(&D->VhfReady,0,0) &&
      HmBuildHidReport(&Empty,&D->HidState,&Candidate,&NextState)) {
    Epoch=InterlockedCompareExchange(&D->ReleasePending,0,0);
    S=HmSubmitReport(D,&Candidate,&NextState,TRUE,Epoch);
    UNREFERENCED_PARAMETER(S);
  }
  return STATUS_SUCCESS;
}

void HmVhfReady(PVOID Context) {
  DEVICE_CONTEXT *D=Context;
  InterlockedExchange(&D->VhfReady,1);
  HmQueueWork(D);
}

void HmGetFeature(PVOID Context,VHFOPERATIONHANDLE Operation,PVOID OperationContext,
                  PHID_XFER_PACKET Packet) {
  NTSTATUS S=STATUS_INVALID_PARAMETER; UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(OperationContext);
  if (Packet && Packet->reportId==2 &&
      HmBuildMaxContactsFeature(Packet->reportBuffer,Packet->reportBufferLen))
    S=STATUS_SUCCESS;
  (VOID)VhfAsyncOperationComplete(Operation,S);
}

void HmCleanup(WDFOBJECT Object) {
  DEVICE_CONTEXT *D=DeviceContext((WDFDEVICE)Object);
  if (D->Vhf) { VhfDelete(D->Vhf,TRUE); D->Vhf=NULL; }
}

NTSTATUS HmDeviceAdd(WDFDRIVER Driver,PWDFDEVICE_INIT Init) {
  WDF_PNPPOWER_EVENT_CALLBACKS Pnp; WDF_OBJECT_ATTRIBUTES A; WDFDEVICE Device;
  DEVICE_CONTEXT *D; WDF_WORKITEM_CONFIG WorkConfig;
  VHF_CONFIG VhfConfig; NTSTATUS S;
  UNREFERENCED_PARAMETER(Driver); WdfDeviceInitSetDeviceType(Init,FILE_DEVICE_UNKNOWN);
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
  Pnp.EvtDevicePrepareHardware=HmPrepare; Pnp.EvtDeviceReleaseHardware=HmRelease;
  Pnp.EvtDeviceD0Entry=HmD0Entry; Pnp.EvtDeviceD0Exit=HmD0Exit;
  WdfDeviceInitSetPnpPowerEventCallbacks(Init,&Pnp);
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,DEVICE_CONTEXT);
  A.EvtCleanupCallback=HmCleanup; A.ExecutionLevel=WdfExecutionLevelPassive;
  S=WdfDeviceCreate(&Init,&A,&Device); if (!NT_SUCCESS(S)) return S;
  D=DeviceContext(Device); D->Device=Device; KeInitializeSpinLock(&D->FrameLock);
  D->HimaxIo.Context=D; D->HimaxIo.Transfer=HmSpbTransfer;
  D->HimaxIo.Delay=HmTransportDelay;
  WDF_WORKITEM_CONFIG_INIT(&WorkConfig,HmReadWork);
  WDF_OBJECT_ATTRIBUTES_INIT(&A); A.ParentObject=Device;
  S=WdfWorkItemCreate(&WorkConfig,&A,&D->Work); if (!NT_SUCCESS(S)) return S;
  VHF_CONFIG_INIT(&VhfConfig,WdfDeviceWdmGetDeviceObject(Device),
                  (USHORT)sizeof(ReportDescriptor),ReportDescriptor);
  VhfConfig.VhfClientContext=D; VhfConfig.EvtVhfReadyForNextReadReport=HmVhfReady;
  VhfConfig.EvtVhfAsyncOperationGetFeature=HmGetFeature;
  S=VhfCreate(&VhfConfig,&D->Vhf); if (!NT_SUCCESS(S)) return S;
  return VhfStart(D->Vhf);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,PUNICODE_STRING RegistryPath) {
  WDF_DRIVER_CONFIG Config; WDF_OBJECT_ATTRIBUTES Attributes;
  WDF_DRIVER_CONFIG_INIT(&Config,HmDeviceAdd); WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
  return WdfDriverCreate(DriverObject,RegistryPath,&Attributes,&Config,WDF_NO_HANDLE);
}
