// SPDX-License-Identifier: BSD-2-Clause-Patent
/* USB2 PIO host controller for the MT6789 MUSB instance.
 * UCX owns USB enumeration/HID. No fabricated device descriptors or VID/PID.
 * A passive worker owns the indexed controller registers; all URB completions
 * run on a separate DPC, including cancellation while a request is queued. */
#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <usbiodef.h>
#include <ucxclass.h>
#include <acpiioct.h>
#include <spb.h>
#include <reshub.h>
#include <Library/MtkHusb311TypecCore.h>
#include <Library/MusbHostCore.h>
#include <Library/MtkMusbPlatformCore.h>
#include <Library/MtkUsbPhyCore.h>

#define USB_BASE 0x11200000ULL
#define USB_BYTES 0x10000U
#define USB_HANDOFF 0x43f7f100ULL
#define USB_HANDOFF_BYTES 64U
#define USB_HANDOFF_REVISION_LEGACY 1U
#define USB_HANDOFF_REVISION_POWER_PATH 2U
#define USB_HANDOFF_FLAGS_COMMON 0x6fU
#define USB_HANDOFF_FLAGS_TYPEC_SOURCE (USB_HANDOFF_FLAGS_COMMON | (1U<<4))
#define USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER (USB_HANDOFF_FLAGS_COMMON | (1U<<7))
#define USB_HANDOFF_PATH_LEGACY 0U
#define USB_HANDOFF_PATH_TYPEC_SOURCE 1U
#define USB_HANDOFF_PATH_ID_LOW_HUB_POWER 2U
#define USB_HANDOFF_PATH_TYPEC_ONLY 3U
#define USB_TYPEC_POWER_SAMPLE 0x01000000U
#define USB_TYPEC_POLL_US 100000U
#define USB_REG_POWER 0x01U
#define USB_REG_INTRUSB 0x0aU
#define USB_REG_INTRUSBE 0x0bU
#define USB_REG_L1INTS 0xa0U
#define USB_REG_L1INTM 0xa4U
#define USB_L1_IRQ_GROUPS 0x0fU
#define USB_L1_COMMON 0x04U
#define USB_PORT_IRQ_MASK (MUSB_HOST_CONNECT_DISCONNECT_MASK|MUSB_HOST_RESUME_MASK)
#define USB_POWER_MODE_MASK 0x21U // HSENAB and ENSUSPEND
#define USB_POWER_SUSPENDM 0x02U
#define USB_POWER_TRANSIENT_MASK 0x0cU // RESUME and RESET
#define USB_POWER_SOFTCONN 0x40U
#define USB_PHY_ACR5 0x314U
#define USB_PHY_CALIBRATING 0x8000U

/* intr_cal (generic-tphy-v2 efuse field), eye levels, slew and comparator
 * tuning from the stock T70 USB2 path. Role, UART, BC1.2 and VBUS bits are
 * outside these masks. The slew field precedes eye/receiver tuning in the
 * stock host path; the exact device-specific values come from live D0. */
static const struct { ULONG Offset,Mask; } UsbPhyTuningFields[]={
  {USB_PHY_ACR5,0x00007000U},
  {MTK_MUSB_PLATFORM_PHY_ACR1,0x00f87700U},
  {MTK_MUSB_PLATFORM_PHY_ACR6,0xd01000ffU}
};
typedef struct { ULONG Values[ARRAYSIZE(UsbPhyTuningFields)]; } USB_PHY_TUNING;
#define USB_DEVCTL_REGISTER 0x60U
#define USB_DEVCTL_SESSION 0x01U
#define USB_DEVCTL_HOST_SESSION_MASK 0x85U
#define USB_DEVCTL_BDEVICE 0x80U
#define USB_DEVCTL_HOST_SESSION 0x05U
#define USB_DEVCTL_HOST_READY_MASK 0x9dU
#define USB_DEVCTL_HOST_READY_VALUE 0x1dU
#define USB_DEVCTL_VBUS_VALID 0x18U
#define USB_GSIV 160U
#define USB_MAX_TRANSFER (256U*1024U)
#define USB_INTERRUPT_POLL_TIMEOUT_MAX_MS 100U
/* One hardware poll of an idle interrupt IN endpoint. Software paces each
 * endpoint by bInterval (NextPoll); the controller polls every (micro)frame
 * while armed, so a NAKing device costs at most this slice of the worker. */
#define USB_INTERRUPT_POLL_SLICE_MS 3U
#define USB_TAG 'U07T'
#define USB_EP_BUDGET 32U
#define USB_STOP_PURGE 1L
#define USB_STOP_ABORT 2L
#define USB_STOP_DEVICE_DISABLE 4L

typedef struct USB_CONTROLLER USB_CONTROLLER;
typedef struct USB_ENDPOINT USB_ENDPOINT;
typedef enum { JobTransfer,JobRootControl,JobRootInterrupt,JobAddress,JobConfigure } USB_JOB;
typedef struct {
  WDFREQUEST Request;
  USB_CONTROLLER *Controller;
  USB_ENDPOINT *Endpoint;
  LIST_ENTRY CompletionLink;
  volatile LONG Cancelled,Completed;
  KEVENT CancelDone;
  USB_JOB Job;
  BOOLEAN Marked;
  NTSTATUS Status;
  PURB Urb;
  ULONG Bytes; // Interrupt IN byte offset while requeued; final byte count on completion.
} USB_REQUEST;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USB_REQUEST,UsbRequest)
typedef struct {
  USB_CONTROLLER *Controller;
  UCXUSBDEVICE Handle,TtHub;
  USB_DEVICE_SPEED Speed;
  USB_DEVICE_PORT_PATH Path;
  volatile LONG Enabled;
  UCHAR Address,ReservedAddress;
  BOOLEAN MultiTt;
  USB_ENDPOINT *DefaultEndpoint;
  WDFREQUEST DisableRequest;
} USB_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USB_DEVICE,UsbDeviceContext)
typedef struct { USB_CONTROLLER *Controller; USB_ENDPOINT *Endpoint; } USB_QUEUE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USB_QUEUE,UsbQueue)
typedef struct { USB_CONTROLLER *Controller; } USB_OBJECT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USB_OBJECT,UsbObject)
struct USB_ENDPOINT {
  USB_CONTROLLER *Controller;
  USB_DEVICE *Device;
  UCXENDPOINT Handle;
  WDFQUEUE Incoming,Pending;
  LIST_ENTRY Link,StartLink;
  volatile LONG Dead,Stopped,StopKind,Purges,Inflight;
  UCHAR Address,Type,Interval,Toggle;
  USHORT MaximumPacket;
  ULONGLONG PollUs,NextPoll;
  PETHREAD QueueStartThread;
  BOOLEAN Listed,StartListed,QueueStartActive,QueueStartDispatching,QueueStopPending,QueueStopIssued,ConfigStopPending;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USB_ENDPOINT,UsbEndpoint)
struct USB_CONTROLLER {
  WDFDEVICE Device;
  UCXCONTROLLER Ucx;
  UCXROOTHUB RootHub;
  WDFQUEUE Management,RootInterrupt;
  WDFWORKITEM Worker;
  WDFTIMER Timer;
  WDFINTERRUPT Interrupt;
  WDFIOTARGET TcpcTarget;
  WDFSPINLOCK EndpointLock;
  KEVENT QueueStartsDrained,ResetDrained;
  LIST_ENTRY Endpoints,QueueStartingEndpoints;
  ULONG EndpointCount;
  KSPIN_LOCK CompletionLock;
  LIST_ENTRY Completions;
  KDPC CompletionDpc;
  volatile LONG CompletionCount,Online,Stopping,CoreLock,QueueStartsActive;
  USB_REQUEST *Active;
  USB_ENDPOINT *ActiveEndpoint;
  PUCHAR Registers;
  PUCHAR Phy;
  PULONG Handoff;
  MUSB_HOST Core;
  MTK_USB_PHY_CORE ColdPhy;
  MTK_USB_PHY_MUSB_SESSION ColdPhySession;
  MTK_USB_PHY_RESUME_SNAPSHOT ColdPhySnapshot;
  MTK_USB_PHY_CONFIG ColdPhyConfig;
  MTK_USB_PHY_HOST_TUNING ColdPhyHostTuning;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE ColdHost;
  ULONG LastPortChange,ResumeClock,PhyBytes,PowerPath,ColdSampleSequence,ColdGeneration;
  ULONGLONG NextVbusProbe,TcpcConnection,ColdOwnerToken,ColdRoleToken,ColdIdLowToken;
  USB_PHY_TUNING ResumeTuning;
  UCHAR ResumePowerMode,TypecCc,ColdPhyMode,ColdTypecCc;
  UCHAR SavedInterruptGroups;
  WDF_POWER_DEVICE_STATE ResumeState;
  PETHREAD WorkerThread,ResetThread;
  BOOLEAN WorkerScheduled,SeenD0,Prepared,WaitingForVbus,TypecProbe,RouteProbe;
  BOOLEAN ColdPhyBound,ColdStartPending,ColdStartOwned,ColdResumeRequested,ResumeTuningValid;
  BOOLEAN InterruptReady,InterruptOwned,InterruptUnmasked,LeavingD0;
  BOOLEAN AddressInUse[128];
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USB_CONTROLLER,UsbControllerContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD UsbAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE UsbPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE UsbRelease;
EVT_WDF_DEVICE_D0_ENTRY UsbD0Entry;
EVT_WDF_DEVICE_D0_EXIT UsbD0Exit;
static EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED UsbD0PostInterruptsEnabled;
static EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED UsbD0PreInterruptsDisabled;
static EVT_WDF_INTERRUPT_ISR UsbPortIsr;
static EVT_WDF_INTERRUPT_DPC UsbPortDpc;
static EVT_WDF_INTERRUPT_DISABLE UsbPortInterruptDisable;
static EVT_WDF_INTERRUPT_SYNCHRONIZE UsbPortInterruptSync;
EVT_WDF_OBJECT_CONTEXT_CLEANUP UsbCleanup;
EVT_WDF_OBJECT_CONTEXT_CLEANUP UsbEndpointCleanup;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL UsbSubmit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL UsbIoControl;
EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE UsbQueuedCancel;
EVT_WDF_IO_QUEUE_STATE UsbPurgeDone;
static NTSTATUS UsbWaitForQueueStarts(USB_CONTROLLER *D);
static void UsbPollVbus(USB_CONTROLLER *D);
static int UsbColdFreshGuard(void *Context,uint32_t Phase,
                             MTK_MUSB_PLATFORM_COLD_EVIDENCE *Evidence);
static int UsbColdSetPhyMode(void *Context,uint32_t Mode);
static int UsbColdGetTimeUs(void *Context,uint64_t *Microseconds);
static NTSTATUS UsbBeginColdResume(USB_CONTROLLER *D);
static void UsbAdvanceColdResume(USB_CONTROLLER *D);
static NTSTATUS UsbStopColdResume(USB_CONTROLLER *D);
EVT_WDF_REQUEST_CANCEL UsbCancel;
EVT_WDF_WORKITEM UsbWork;
EVT_WDF_TIMER UsbTimer;
EVT_UCX_CONTROLLER_QUERY_USB_CAPABILITY UsbCapability;
EVT_UCX_CONTROLLER_GET_CURRENT_FRAMENUMBER UsbFrame;
EVT_UCX_CONTROLLER_USBDEVICE_ADD UsbDeviceAdd;
EVT_UCX_CONTROLLER_RESET UsbControllerReset;
EVT_UCX_USBDEVICE_ENABLE UsbEnable;
EVT_UCX_USBDEVICE_DISABLE UsbDisable;
EVT_UCX_USBDEVICE_RESET UsbResetDevice;
EVT_UCX_USBDEVICE_ADDRESS UsbAddressDevice;
EVT_UCX_USBDEVICE_UPDATE UsbUpdateDevice;
EVT_UCX_USBDEVICE_HUB_INFO UsbHubInfo;
EVT_UCX_USBDEVICE_ENDPOINTS_CONFIGURE UsbConfigure;
EVT_UCX_USBDEVICE_DEFAULT_ENDPOINT_ADD UsbDefaultEndpointAdd;
EVT_UCX_USBDEVICE_ENDPOINT_ADD UsbEndpointAdd;
EVT_UCX_ENDPOINT_PURGE UsbEndpointPurge;
EVT_UCX_ENDPOINT_ABORT UsbEndpointAbort;
EVT_UCX_ENDPOINT_START UsbEndpointStart;
EVT_UCX_ENDPOINT_RESET UsbEndpointReset;
EVT_UCX_DEFAULT_ENDPOINT_UPDATE UsbDefaultUpdate;
EVT_UCX_ENDPOINT_OK_TO_CANCEL_TRANSFERS UsbOkToCancel;
EVT_UCX_ROOTHUB_CONTROL_URB UsbRootControl;
EVT_UCX_ROOTHUB_INTERRUPT_TX UsbRootInterrupt;
EVT_UCX_ROOTHUB_GET_INFO UsbRootInfo;
EVT_UCX_ROOTHUB_GET_20PORT_INFO UsbRoot20Info;
EVT_UCX_ROOTHUB_GET_30PORT_INFO UsbRoot30Info;

static void UsbMaybeStopComplete(USB_ENDPOINT *E);
static NTSTATUS UsbQuiesce(USB_CONTROLLER *D);
static BOOLEAN UsbUnmark(USB_REQUEST *R);
static PVOID UsbArgument(WDFREQUEST R) {
  WDF_REQUEST_PARAMETERS P;
  WDF_REQUEST_PARAMETERS_INIT(&P); WdfRequestGetParameters(R,&P);
  return P.Parameters.Others.Arg1;
}
static ULONGLONG UsbNow(void *P) {
  ULONGLONG T; UNREFERENCED_PARAMETER(P); return KeQueryInterruptTimePrecise(&T)/10;
}
static void UsbDelay(void *P,unsigned Us) {
  LARGE_INTEGER Interval;
  UNREFERENCED_PARAMETER(P);

  if(Us<=50U) {
    KeStallExecutionProcessor(Us);
    return;
  }
  /* These signaling and settling intervals run at PASSIVE_LEVEL. */
  Interval.QuadPart=-(LONGLONG)Us*10;
  (void)KeDelayExecutionThread(KernelMode,FALSE,&Interval);
}
static int UsbLock(void *P) {
  return InterlockedCompareExchange(&((USB_CONTROLLER *)P)->CoreLock,1,0)==0;
}
static void UsbUnlock(void *P) { InterlockedExchange(&((USB_CONTROLLER *)P)->CoreLock,0); }
static int UsbCancelled(void *P) {
  USB_CONTROLLER *D=P;
  return D->Stopping || !D->Online ||
    (D->Active && D->Active->Cancelled) ||
    (D->ActiveEndpoint && (D->ActiveEndpoint->Dead || D->ActiveEndpoint->Stopped || !D->ActiveEndpoint->Device->Enabled));
}
#define USB_READ(W,T,F) static int UsbRead##W(void *P,uint32_t O,T *V) { \
  USB_CONTROLLER *D=P; if(!V || !D->Registers || O>USB_BYTES-sizeof(*V) || (O&(sizeof(*V)-1))) return 0; \
  *V=F((volatile T *)(D->Registers+O)); return 1; }
#define USB_WRITE(W,T,F) static int UsbWrite##W(void *P,uint32_t O,T V) { \
  USB_CONTROLLER *D=P; if(!D->Registers || O>USB_BYTES-sizeof(V) || (O&(sizeof(V)-1))) return 0; \
  F((volatile T *)(D->Registers+O),V); return 1; }
USB_READ(8,uint8_t,READ_REGISTER_UCHAR)
USB_READ(16,uint16_t,READ_REGISTER_USHORT)
static int UsbRead32(void *P,uint32_t O,uint32_t *V) { USB_CONTROLLER *D=P; if(!V || !D->Registers || O>USB_BYTES-4 || (O&3)) return 0; *V=READ_REGISTER_ULONG((PULONG)(D->Registers+O));return 1; }
USB_WRITE(8,uint8_t,WRITE_REGISTER_UCHAR)
USB_WRITE(16,uint16_t,WRITE_REGISTER_USHORT)
static int UsbWrite32(void *P,uint32_t O,uint32_t V) { USB_CONTROLLER *D=P; if(!D->Registers || O>USB_BYTES-4 || (O&3)) return 0; WRITE_REGISTER_ULONG((PULONG)(D->Registers+O),V);return 1; }

static NTSTATUS UsbStatus(MUSB_STATUS S,USBD_STATUS *U) {
  switch(S) {
  case MusbOk:*U=USBD_STATUS_SUCCESS;return STATUS_SUCCESS;
  case MusbCancelled:*U=USBD_STATUS_CANCELED;return STATUS_CANCELLED;
  case MusbStall:*U=USBD_STATUS_STALL_PID;return STATUS_UNSUCCESSFUL;
  case MusbTimeout:*U=USBD_STATUS_TIMEOUT;return STATUS_IO_TIMEOUT;
  case MusbDisconnected:*U=USBD_STATUS_DEVICE_GONE;return STATUS_DEVICE_NOT_CONNECTED;
  case MusbBufferOverflow:*U=USBD_STATUS_DATA_OVERRUN;return STATUS_BUFFER_OVERFLOW;
  case MusbInvalid:*U=USBD_STATUS_INVALID_PARAMETER;return STATUS_INVALID_PARAMETER;
  case MusbUnsupported:*U=USBD_STATUS_NOT_SUPPORTED;return STATUS_NOT_SUPPORTED;
  case MusbNotReady:*U=USBD_STATUS_DEVICE_GONE;return STATUS_DEVICE_NOT_READY;
  default:*U=USBD_STATUS_XACT_ERROR;return STATUS_IO_DEVICE_ERROR;
  }
}
static void UsbCompleteNow(WDFREQUEST R,NTSTATUS Status) {
  KIRQL Old=KeGetCurrentIrql();
  if(Old<DISPATCH_LEVEL) KeRaiseIrql(DISPATCH_LEVEL,&Old);
  WdfRequestComplete(R,Status);
  if(Old<DISPATCH_LEVEL) KeLowerIrql(Old);
}
static void UsbCompletionDpc(PKDPC Dpc,PVOID Context,PVOID A,PVOID B) {
  USB_CONTROLLER *D=Context; PLIST_ENTRY Link; USB_REQUEST *R;
  UNREFERENCED_PARAMETER(Dpc); UNREFERENCED_PARAMETER(A); UNREFERENCED_PARAMETER(B);
  for(;;) {
    KeAcquireSpinLockAtDpcLevel(&D->CompletionLock);
    if(IsListEmpty(&D->Completions)) {
      KeReleaseSpinLockFromDpcLevel(&D->CompletionLock); break;
    }
    Link=RemoveHeadList(&D->Completions);
    KeReleaseSpinLockFromDpcLevel(&D->CompletionLock);
    R=CONTAINING_RECORD(Link,USB_REQUEST,CompletionLink);
    {
      WDFREQUEST Request=R->Request; USB_ENDPOINT *E=R->Endpoint; NTSTATUS S=R->Status;
      R->Endpoint=NULL;
      /* Completion may free the request and call purge completion inline. */
      WdfRequestCompleteWithInformation(Request,S,0);
      if(E) {InterlockedDecrement(&E->Inflight);UsbMaybeStopComplete(E);WdfObjectDereference(E->Handle);}
      InterlockedDecrement(&D->CompletionCount);
    }
  }
}
static void UsbFinish(USB_REQUEST *R,NTSTATUS S,USBD_STATUS U,ULONG Bytes) {
  USB_CONTROLLER *D=R->Controller; KIRQL Old;
  if(R->Marked) {
    D->Active=NULL;D->ActiveEndpoint=NULL;
    if(!UsbUnmark(R)) {S=STATUS_CANCELLED;U=USBD_STATUS_CANCELED;}
    R->Marked=FALSE;
  }
  if(InterlockedCompareExchange(&R->Completed,1,0)) return;
  R->Status=S; R->Bytes=Bytes;
  if(R->Urb) {
    R->Urb->UrbHeader.Status=U;
    switch(R->Urb->UrbHeader.Function) {
    case URB_FUNCTION_CONTROL_TRANSFER:R->Urb->UrbControlTransfer.TransferBufferLength=Bytes;break;
    case URB_FUNCTION_CONTROL_TRANSFER_EX:R->Urb->UrbControlTransferEx.TransferBufferLength=Bytes;break;
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:R->Urb->UrbBulkOrInterruptTransfer.TransferBufferLength=Bytes;break;
    }
  }
  InterlockedIncrement(&D->CompletionCount);
  KeAcquireSpinLock(&D->CompletionLock,&Old);
  InsertTailList(&D->Completions,&R->CompletionLink);
  KeReleaseSpinLock(&D->CompletionLock,Old);
  KeInsertQueueDpc(&D->CompletionDpc,NULL,NULL);
}
static NTSTATUS UsbNewRequest(USB_CONTROLLER *D,USB_ENDPOINT *E,WDFREQUEST Request,USB_JOB Job,PURB Urb,USB_REQUEST **Out) {
  WDF_OBJECT_ATTRIBUTES A; USB_REQUEST *R=NULL; NTSTATUS S;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,USB_REQUEST);
  S=WdfObjectAllocateContext(Request,&A,(PVOID *)&R);
  if(S==STATUS_OBJECT_NAME_EXISTS) { R=UsbRequest(Request); S=STATUS_SUCCESS; }
  if(!NT_SUCCESS(S)) return S;
  RtlZeroMemory(R,sizeof(*R)); KeInitializeEvent(&R->CancelDone,NotificationEvent,FALSE);
  R->Request=Request; R->Controller=D; R->Endpoint=E; R->Job=Job; R->Urb=Urb;
  if(E) {WdfObjectReference(E->Handle);InterlockedIncrement(&E->Inflight);}
  *Out=R; return STATUS_SUCCESS;
}
/* EndpointLock protects enqueue admission across D0 exit and serializes the worker. */
static void UsbScheduleLocked(USB_CONTROLLER *D) {
  if(D->Online && !D->Stopping && !D->WorkerScheduled) {
    D->WorkerScheduled=TRUE;
    WdfWorkItemEnqueue(D->Worker);
  }
}

/* The interrupt lock protects the masks. The passive worker owns status
 * acknowledgement and indexed registers, including PIO endpoint status. */
static BOOLEAN UsbMaskPortInterrupt(USB_CONTROLLER *D) {
  PULONG Mask=(PULONG)(D->Registers+USB_REG_L1INTM);
  WRITE_REGISTER_UCHAR(D->Registers+USB_REG_INTRUSBE,0);
  D->InterruptUnmasked=FALSE;
  if(!READ_REGISTER_UCHAR(D->Registers+USB_REG_INTRUSBE)) return TRUE;
  WRITE_REGISTER_ULONG(Mask,READ_REGISTER_ULONG(Mask)&~USB_L1_COMMON);
  return FALSE;
}

static void UsbReleasePortInterrupt(USB_CONTROLLER *D) {
  if(!D->InterruptOwned) return;
  if(UsbMaskPortInterrupt(D)) {
    PULONG Mask=(PULONG)(D->Registers+USB_REG_L1INTM);
    WRITE_REGISTER_ULONG(Mask,(READ_REGISTER_ULONG(Mask)&~USB_L1_IRQ_GROUPS)|D->SavedInterruptGroups);
    D->InterruptOwned=FALSE;
  }
}

static BOOLEAN UsbPortInterruptSync(WDFINTERRUPT Interrupt,WDFCONTEXT Context) {
  USB_CONTROLLER *D=UsbControllerContext(WdfInterruptGetDevice(Interrupt));
  PULONG Mask=(PULONG)(D->Registers+USB_REG_L1INTM);
  ULONG Groups;

  if(Context && D->ColdStartPending) {
    Groups=READ_REGISTER_ULONG(Mask);
    if(!D->InterruptOwned) {
      D->SavedInterruptGroups=(UCHAR)(Groups&USB_L1_IRQ_GROUPS);
      D->InterruptOwned=TRUE;
    }
    WRITE_REGISTER_ULONG(Mask,Groups&~USB_L1_IRQ_GROUPS);
    return UsbMaskPortInterrupt(D) &&
        !(READ_REGISTER_ULONG(Mask)&USB_L1_IRQ_GROUPS);
  }
  if(!Context || !D->Online || D->Stopping || D->WaitingForVbus ||
      !D->Core.Initialized || D->Core.State!=MUSB_HOST_STATE_OPERATIONAL ||
      !(D->Core.PortStatus&MUSB_PORT_POWER)) {
    UsbReleasePortInterrupt(D);
    return TRUE;
  }
  if(D->InterruptUnmasked) return TRUE;
  Groups=READ_REGISTER_ULONG(Mask);
  if(!D->InterruptOwned) {
    D->SavedInterruptGroups=(UCHAR)(Groups&USB_L1_IRQ_GROUPS);
    D->InterruptOwned=TRUE;
  }
  WRITE_REGISTER_ULONG(Mask,(Groups&~USB_L1_IRQ_GROUPS)|USB_L1_COMMON);
  if((READ_REGISTER_ULONG(Mask)&USB_L1_IRQ_GROUPS)==USB_L1_COMMON) {
    WRITE_REGISTER_UCHAR(D->Registers+USB_REG_INTRUSBE,USB_PORT_IRQ_MASK);
    if(READ_REGISTER_UCHAR(D->Registers+USB_REG_INTRUSBE)==USB_PORT_IRQ_MASK) {
      D->InterruptUnmasked=TRUE;
      return TRUE;
    }
  }
  UsbReleasePortInterrupt(D);
  return FALSE;
}

/* EndpointLock also serializes the worker's rearm with D0 exit. The ISR
 * never takes this lock, and the DPC never takes the interrupt lock. */
static void UsbUpdatePortInterruptLocked(USB_CONTROLLER *D,BOOLEAN Enable) {
  if(D->InterruptReady)
    WdfInterruptSynchronize(D->Interrupt,UsbPortInterruptSync,Enable?D:NULL);
}

static BOOLEAN UsbMaskResetInterrupt(WDFINTERRUPT Interrupt,WDFCONTEXT Context) {
  USB_CONTROLLER *D=Context;
  PULONG Mask=(PULONG)(D->Registers+USB_REG_L1INTM);
  ULONG Groups=READ_REGISTER_ULONG(Mask);
  UNREFERENCED_PARAMETER(Interrupt);
  if(!D->InterruptOwned) {
    D->SavedInterruptGroups=(UCHAR)(Groups&USB_L1_IRQ_GROUPS);
    D->InterruptOwned=TRUE;
  }
  /* SWRST can re-enable the controller sources before they are remasked. */
  WRITE_REGISTER_ULONG(Mask,Groups&~USB_L1_IRQ_GROUPS);
  return UsbMaskPortInterrupt(D) && !(READ_REGISTER_ULONG(Mask)&USB_L1_IRQ_GROUPS);
}

static BOOLEAN UsbMaskColdStartInterrupts(USB_CONTROLLER *D) {
  BOOLEAN Masked;
  WdfSpinLockAcquire(D->EndpointLock);
  if(D->Stopping || D->LeavingD0) {
    WdfSpinLockRelease(D->EndpointLock);
    return FALSE;
  }
  Masked=D->InterruptReady?
      WdfInterruptSynchronize(D->Interrupt,UsbMaskResetInterrupt,D):
      UsbMaskResetInterrupt(D->Interrupt,D);
  WdfSpinLockRelease(D->EndpointLock);
  return Masked;
}

static BOOLEAN UsbPortIsr(WDFINTERRUPT Interrupt,ULONG MessageId) {
  USB_CONTROLLER *D=UsbControllerContext(WdfInterruptGetDevice(Interrupt));
  UNREFERENCED_PARAMETER(MessageId);
  if(!D->InterruptUnmasked ||
      !(READ_REGISTER_ULONG((PULONG)(D->Registers+USB_REG_L1INTS))&USB_L1_COMMON) ||
      !(READ_REGISTER_UCHAR(D->Registers+USB_REG_INTRUSB)&USB_PORT_IRQ_MASK))
    return FALSE;
  UsbMaskPortInterrupt(D);
  WdfInterruptQueueDpcForIsr(Interrupt);
  return TRUE;
}

static void UsbPortDpc(WDFINTERRUPT Interrupt,WDFOBJECT Device) {
  USB_CONTROLLER *D=UsbControllerContext((WDFDEVICE)Device);
  UNREFERENCED_PARAMETER(Interrupt);
  WdfSpinLockAcquire(D->EndpointLock);
  if(D->InterruptReady) UsbScheduleLocked(D);
  WdfSpinLockRelease(D->EndpointLock);
}

static NTSTATUS UsbPortInterruptDisable(WDFINTERRUPT Interrupt,WDFDEVICE Device) {
  UNREFERENCED_PARAMETER(Interrupt);
  UsbReleasePortInterrupt(UsbControllerContext(Device));
  return STATUS_SUCCESS;
}

static void UsbEnqueue(USB_CONTROLLER *D,USB_ENDPOINT *E,WDFREQUEST Request,USB_JOB Job,PURB Urb,WDFQUEUE Queue) {
  USB_REQUEST *R; NTSTATUS S=UsbNewRequest(D,E,Request,Job,Urb,&R);
  if(!NT_SUCCESS(S)) {
    if(Urb) Urb->UrbHeader.Status=USBD_STATUS_INSUFFICIENT_RESOURCES;
    UsbCompleteNow(Request,S); return;
  }
  WdfSpinLockAcquire(D->EndpointLock);
  if(D->Stopping || !D->Online || (E && (E->Dead || E->Stopped || !E->Device->Enabled))) {
    WdfSpinLockRelease(D->EndpointLock);
    UsbFinish(R,STATUS_DEVICE_NOT_READY,USBD_STATUS_DEVICE_GONE,0); return;
  }
  S=WdfRequestForwardToIoQueue(Request,Queue);
  if(NT_SUCCESS(S)) UsbScheduleLocked(D);
  WdfSpinLockRelease(D->EndpointLock);
  if(!NT_SUCCESS(S)) UsbFinish(R,S,USBD_STATUS_CANCELED,0);
}
void UsbCancel(WDFREQUEST Request) {
  USB_REQUEST *R=UsbRequest(Request);
  InterlockedExchange(&R->Cancelled,1);
  KeSetEvent(&R->CancelDone,IO_NO_INCREMENT,FALSE);
}
void UsbQueuedCancel(WDFQUEUE Q,WDFREQUEST Request) {
  UNREFERENCED_PARAMETER(Q); UsbFinish(UsbRequest(Request),STATUS_CANCELLED,USBD_STATUS_CANCELED,0);
}
static NTSTATUS UsbMdlCopy(PMDL M,PUCHAR B,ULONG N,BOOLEAN Input) {
  ULONG Done=0;
  while(M && Done<N) {
    ULONG K=MmGetMdlByteCount(M); PUCHAR V=MmGetSystemAddressForMdlSafe(M,NormalPagePriority|MdlMappingNoExecute);
    if(K>N-Done) K=N-Done;
    if(!V) return STATUS_INSUFFICIENT_RESOURCES;
    if(Input) RtlCopyMemory(V,B+Done,K); else RtlCopyMemory(B+Done,V,K);
    Done+=K; M=M->Next;
  }
  return Done==N?STATUS_SUCCESS:STATUS_INVALID_BUFFER_SIZE;
}
static NTSTATUS UsbMdlProbe(PMDL M,ULONG N) {
  ULONG Done=0,Count=0;
  while(M && Done<N) {
    ULONG K=MmGetMdlByteCount(M);
    if(++Count>1024 || !MmGetSystemAddressForMdlSafe(M,NormalPagePriority|MdlMappingNoExecute)) return STATUS_INSUFFICIENT_RESOURCES;
    if(K>N-Done) K=N-Done;
    Done+=K;M=M->Next;
  }
  return Done==N?STATUS_SUCCESS:STATUS_INVALID_BUFFER_SIZE;
}
static unsigned UsbSpeed(USB_DEVICE *U) {
  return U->Speed==UsbLowSpeed?MUSB_SPEED_LOW:U->Speed==UsbHighSpeed?MUSB_SPEED_HIGH:MUSB_SPEED_FULL;
}
static NTSTATUS UsbRoute(USB_DEVICE *U,MUSB_HOST_ROUTE *R) {
  RtlZeroMemory(R,sizeof(*R));
  if(U->TtHub) {
    USB_DEVICE *H=UsbDeviceContext(U->TtHub); ULONG Depth=U->Path.TTHubDepth;
    if(!H->Address || H->Speed!=UsbHighSpeed || !Depth || Depth>=U->Path.PortPathDepth ||
       Depth>=MAX_USB_DEVICE_DEPTH || !U->Path.PortPath[Depth] || U->Path.PortPath[Depth]>127)
      return STATUS_DEVICE_CONFIGURATION_ERROR;
    R->HubAddress=H->Address; R->HubPort=(uint8_t)U->Path.PortPath[Depth]; R->MultiTt=H->MultiTt?1:0;
  }
  return STATUS_SUCCESS;
}
static BOOLEAN UsbUnmark(USB_REQUEST *R) {
  NTSTATUS S=WdfRequestUnmarkCancelable(R->Request);
  if(S==STATUS_CANCELLED) {
    KeWaitForSingleObject(&R->CancelDone,Executive,KernelMode,FALSE,NULL);
    return FALSE;
  }
  return NT_SUCCESS(S) && !R->Cancelled;
}
static ULONG UsbInterruptTimeoutMs(const USB_ENDPOINT *E,ULONG Length) {
  ULONGLONG Packets,TimeoutUs,TimeoutMs;
  if(!E || !E->MaximumPacket || !E->PollUs || !Length) return 0;
  Packets=((ULONGLONG)Length+E->MaximumPacket-1U)/E->MaximumPacket;
  TimeoutUs=E->PollUs*(Packets+1U)+1000U;
  TimeoutMs=(TimeoutUs+999U)/1000U;
  /* The core clamps a single transfer to one minute. Refuse larger interrupt
   * URBs before touching hardware instead of reporting a misleading short read. */
  return (TimeoutMs==0U || TimeoutMs>60000U)?0U:(ULONG)TimeoutMs;
}

static void UsbRunTransfer(USB_CONTROLLER *D,USB_ENDPOINT *E,USB_REQUEST *R) {
  PURB U=R->Urb; ULONG Length=0,Flags=0,Timeout=1000; PUCHAR Direct=NULL,Data=NULL;
  PMDL Mdl=NULL; UCHAR *Setup=NULL; MUSB_HOST_ROUTE Route; MUSB_STATUS Engine=MusbInvalid;
  uint32_t Done=0,TransferOffset=0,TransferLength=0,BytesDone=0;
  NTSTATUS S=STATUS_INVALID_PARAMETER; USBD_STATUS Us=USBD_STATUS_INVALID_PARAMETER;
  BOOLEAN Input=FALSE,Marked=FALSE,Retry=FALSE,Control=E->Type==USB_ENDPOINT_TYPE_CONTROL;
  if(!U || U->UrbHeader.Length<sizeof(struct _URB_HEADER)) goto Finish;
  if(Control && U->UrbHeader.Function==URB_FUNCTION_CONTROL_TRANSFER &&
      U->UrbHeader.Length>=sizeof(struct _URB_CONTROL_TRANSFER)) {
    Length=U->UrbControlTransfer.TransferBufferLength; Flags=U->UrbControlTransfer.TransferFlags;
    Direct=U->UrbControlTransfer.TransferBuffer; Mdl=U->UrbControlTransfer.TransferBufferMDL; Setup=U->UrbControlTransfer.SetupPacket;
  } else if(Control && U->UrbHeader.Function==URB_FUNCTION_CONTROL_TRANSFER_EX &&
      U->UrbHeader.Length>=sizeof(struct _URB_CONTROL_TRANSFER_EX)) {
    Length=U->UrbControlTransferEx.TransferBufferLength; Flags=U->UrbControlTransferEx.TransferFlags;
    Direct=U->UrbControlTransferEx.TransferBuffer; Mdl=U->UrbControlTransferEx.TransferBufferMDL; Setup=U->UrbControlTransferEx.SetupPacket;
    Timeout=U->UrbControlTransferEx.Timeout; if(!Timeout || Timeout>5000) Timeout=5000;
  } else if(!Control && U->UrbHeader.Function==URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER &&
      U->UrbHeader.Length>=sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER)) {
    Length=U->UrbBulkOrInterruptTransfer.TransferBufferLength; Flags=U->UrbBulkOrInterruptTransfer.TransferFlags;
    Direct=U->UrbBulkOrInterruptTransfer.TransferBuffer; Mdl=U->UrbBulkOrInterruptTransfer.TransferBufferMDL;
  } else goto Finish;
  if(Control) {
    ULONG SetupLength=(ULONG)Setup[6]|((ULONG)Setup[7]<<8);
    Input=(Setup[0]&0x80)!=0;
    // WDK: USBD_TRANSFER_DIRECTION_IN is optional for control URBs; Setup[0]
    // bit 7 is authoritative, and TransferBufferLength may exceed wLength.
    if(Length>USB_MAX_TRANSFER || SetupLength>Length ||
        (SetupLength && !Direct && !Mdl) ||
        (SetupLength && !Input && (Flags&USBD_TRANSFER_DIRECTION_IN))) goto Finish;
    Length=SetupLength;
  } else {
    Input=(Flags&USBD_TRANSFER_DIRECTION_IN)!=0;
    if(Length>USB_MAX_TRANSFER || (Length && !Direct && !Mdl) ||
        Input!=((E->Address&USB_ENDPOINT_DIRECTION_MASK)!=0)) goto Finish;
  }
  if(E->Type==USB_ENDPOINT_TYPE_INTERRUPT && Input) {
    if(R->Bytes>Length) {S=STATUS_DATA_ERROR;Us=USBD_STATUS_INTERNAL_HC_ERROR;goto Finish;}
    TransferOffset=R->Bytes;
    BytesDone=TransferOffset;
    // The core services one interrupt packet per poll. Keep each worker
    // invocation bounded and carry a full packet into the next poll tick.
    Timeout=UsbInterruptTimeoutMs(E,E->MaximumPacket);
    if(!Timeout) { S=STATUS_NOT_SUPPORTED;Us=USBD_STATUS_INVALID_PARAMETER;goto Finish; }
    if(Timeout>USB_INTERRUPT_POLL_SLICE_MS) Timeout=USB_INTERRUPT_POLL_SLICE_MS;
  }
  TransferLength=Length-TransferOffset;
  if(!E->Device->Enabled || !D->Online || D->WaitingForVbus ||
      D->Core.PortPowerLost || E->Stopped || E->Dead) {
    S=STATUS_DEVICE_NOT_CONNECTED; Us=USBD_STATUS_DEVICE_GONE; goto Finish;
  }
  S=UsbRoute(E->Device,&Route); if(!NT_SUCCESS(S)) goto Finish;
  if(Mdl && Length) {S=UsbMdlProbe(Mdl,Length);if(!NT_SUCCESS(S)) goto Finish;}
  if(Length) {
    Data=ExAllocatePool2(POOL_FLAG_NON_PAGED,Length,USB_TAG);
    if(!Data) { S=STATUS_INSUFFICIENT_RESOURCES;Us=USBD_STATUS_INSUFFICIENT_RESOURCES;goto Finish; }
    if(!Input) {
      if(Mdl) S=UsbMdlCopy(Mdl,Data,Length,FALSE); else { RtlCopyMemory(Data,Direct,Length); S=STATUS_SUCCESS; }
      if(!NT_SUCCESS(S)) goto Finish;
    } else if(TransferOffset!=0U) {
      if(Mdl) S=UsbMdlCopy(Mdl,Data,TransferOffset,FALSE);
      else { RtlCopyMemory(Data,Direct,TransferOffset); S=STATUS_SUCCESS; }
      if(!NT_SUCCESS(S)) goto Finish;
    }
  }
  S=WdfRequestMarkCancelableEx(R->Request,UsbCancel);
  if(!NT_SUCCESS(S)) { S=STATUS_CANCELLED; Us=USBD_STATUS_CANCELED; goto Finish; }
  Marked=TRUE; D->Active=R; D->ActiveEndpoint=E;
  if(Control) Engine=MusbHostControlTransferRouted(&D->Core,E->Device->Address,UsbSpeed(E->Device),
      (uint8_t)E->MaximumPacket,Setup,Data,Length,Timeout,&Route,&Done);
  else {
    uint8_t TransferType=(E->Type==USB_ENDPOINT_TYPE_INTERRUPT && Input) ?
        MUSB_TRANSFER_INTERRUPT_POLL : (uint8_t)E->Type;
    PUCHAR TransferData=Data?Data+TransferOffset:NULL;
    Engine=MusbHostDataTransferRouted(&D->Core,TransferType,E->Device->Address,E->Address,
        UsbSpeed(E->Device),E->MaximumPacket,
        TransferType==MUSB_TRANSFER_INTERRUPT_POLL?(UCHAR)1:E->Interval,TransferData,TransferLength,
        &E->Toggle,Timeout,&Route,&Done);
  }
  D->Active=NULL; D->ActiveEndpoint=NULL;
  S=UsbStatus(Engine,&Us);
  if(Done>TransferLength) { Done=0; S=STATUS_DATA_ERROR;Us=USBD_STATUS_DATA_OVERRUN; }
  BytesDone=TransferOffset+Done;
  if(Input && BytesDone) {
    NTSTATUS Copy;
    if(Mdl) Copy=UsbMdlCopy(Mdl,Data,BytesDone,TRUE); else {RtlCopyMemory(Direct,Data,BytesDone);Copy=STATUS_SUCCESS;}
    if(!NT_SUCCESS(Copy)) {S=Copy;Us=USBD_STATUS_INSUFFICIENT_RESOURCES;BytesDone=0;}
  }
  Retry=E->Type==USB_ENDPOINT_TYPE_INTERRUPT && Input && !E->Stopped && !D->Stopping &&
      ((Engine==MusbTimeout && Done==0U) ||
       (Engine==MusbOk && Done==E->MaximumPacket && BytesDone<Length));
  if(Retry) R->Bytes=BytesDone;
  if(!UsbUnmark(R)) { S=STATUS_CANCELLED; Us=USBD_STATUS_CANCELED; Retry=FALSE; }
  Marked=FALSE;
  if(NT_SUCCESS(S) && Input && BytesDone<Length && !(Flags&USBD_SHORT_TRANSFER_OK)) {
    S=STATUS_DEVICE_DATA_ERROR; Us=USBD_STATUS_ERROR_SHORT_TRANSFER;
  }
Finish:
  if(Marked && !UsbUnmark(R)) { S=STATUS_CANCELLED;Us=USBD_STATUS_CANCELED;Retry=FALSE; }
  if(Data) ExFreePoolWithTag(Data,USB_TAG);
  if(E->Type==USB_ENDPOINT_TYPE_INTERRUPT) E->NextPoll=UsbNow(NULL)+E->PollUs;
  if(Retry) {
    R->Cancelled=0; KeClearEvent(&R->CancelDone);
    S=WdfRequestRequeue(R->Request);
    if(NT_SUCCESS(S)) return;
    Us=USBD_STATUS_CANCELED;
  }
  UsbFinish(R,S,Us,BytesDone);
}
static BOOLEAN UsbBeginOperation(USB_CONTROLLER *D,USB_REQUEST *R) {
  NTSTATUS S=WdfRequestMarkCancelableEx(R->Request,UsbCancel);
  if(!NT_SUCCESS(S)) {UsbFinish(R,STATUS_CANCELLED,USBD_STATUS_CANCELED,0);return FALSE;}
  R->Marked=TRUE;D->Active=R;D->ActiveEndpoint=NULL;
  if(D->Stopping || R->Cancelled) {UsbFinish(R,STATUS_CANCELLED,USBD_STATUS_CANCELED,0);return FALSE;}
  return TRUE;
}
// The worker is the only consumer while D0 is published. Until the
// retained controller has VBUS again, serve the hub's software port view.
static MUSB_STATUS UsbGetRootPortStatus(USB_CONTROLLER *D,uint16_t *Status,uint16_t *Change) {
  if(!D->WaitingForVbus) return MusbHostGetPortStatus(&D->Core,0,Status,Change);
  *Status=D->Core.PortStatus;
  *Change=D->Core.PortChangeStatus;
  return MusbOk;
}
static MUSB_STATUS UsbRootPortFeature(USB_CONTROLLER *D,BOOLEAN Set,UCHAR Feature) {
  USHORT Change=0;
  MUSB_STATUS Status;

  if(!Set && Feature>=MUSB_PORT_FEATURE_CHANGE_CONNECTION &&
      Feature<=MUSB_PORT_FEATURE_CHANGE_RESET)
    Change=(USHORT)(1U<<(Feature-MUSB_PORT_FEATURE_CHANGE_CONNECTION));

  if(!D->WaitingForVbus) {
    Status=Set?MusbHostSetPortFeature(&D->Core,0,Feature):
        MusbHostClearPortFeature(&D->Core,0,Feature);
  } else if(Set) {
    Status=Feature==MUSB_PORT_FEATURE_POWER?MusbOk:MusbNotReady;
  } else if(Change) {
    // The worker owns the disconnected port view, even before Core is initialized.
    D->Core.PortChangeStatus&=~Change;
    Status=MusbOk;
  } else {
    switch(Feature) {
      case MUSB_PORT_FEATURE_ENABLE:
      case MUSB_PORT_FEATURE_RESET: Status=MusbOk;break;
      case MUSB_PORT_FEATURE_SUSPEND: Status=MusbNotReady;break;
      case MUSB_PORT_FEATURE_POWER: Status=MusbUnsupported;break;
      default: Status=MusbInvalid;break;
    }
  }
  // A new edge can set the same change bit before the worker next polls.
  if(Status==MusbOk) D->LastPortChange&=~(ULONG)Change;
  return Status;
}
static void UsbRunRoot(USB_CONTROLLER *D,USB_REQUEST *R) {
  PURB U=R->Urb; PUCHAR Data=NULL; PMDL Mdl=NULL; UCHAR *Setup=NULL,Result[4]={0};
  ULONG N=0,Done=0; uint16_t Status,Change;
  MUSB_STATUS Core; USBD_STATUS Us=USBD_STATUS_INVALID_PARAMETER; NTSTATUS S=STATUS_INVALID_PARAMETER;
  if(!UsbBeginOperation(D,R)) return;
  if(!U || U->UrbHeader.Length<sizeof(struct _URB_HEADER)) goto Exit;
  if(R->Job==JobRootInterrupt) {
    if(U->UrbHeader.Function!=URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER ||
       U->UrbHeader.Length<sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER)) goto Exit;
    N=U->UrbBulkOrInterruptTransfer.TransferBufferLength;Data=U->UrbBulkOrInterruptTransfer.TransferBuffer;
    Mdl=U->UrbBulkOrInterruptTransfer.TransferBufferMDL;
    // The UCX root hub interrupt callback always writes the port-change bitmap.
    if(!N || (!Data && !Mdl)) goto Exit;
    if(Mdl) {S=UsbMdlProbe(Mdl,1);if(!NT_SUCCESS(S)) goto Exit;}
    Core=UsbGetRootPortStatus(D,&Status,&Change); S=UsbStatus(Core,&Us);
    if(NT_SUCCESS(S)) { Result[0]=Change?2:0;Done=1; }
    goto Copy;
  }
  if(U->UrbHeader.Function==URB_FUNCTION_CONTROL_TRANSFER_EX &&
      U->UrbHeader.Length>=sizeof(struct _URB_CONTROL_TRANSFER_EX)) {
    Setup=U->UrbControlTransferEx.SetupPacket;N=U->UrbControlTransferEx.TransferBufferLength;
    Data=U->UrbControlTransferEx.TransferBuffer;Mdl=U->UrbControlTransferEx.TransferBufferMDL;
  } else if(U->UrbHeader.Function==URB_FUNCTION_CONTROL_TRANSFER &&
      U->UrbHeader.Length>=sizeof(struct _URB_CONTROL_TRANSFER)) {
    Setup=U->UrbControlTransfer.SetupPacket;N=U->UrbControlTransfer.TransferBufferLength;
    Data=U->UrbControlTransfer.TransferBuffer;Mdl=U->UrbControlTransfer.TransferBufferMDL;
  } else goto Exit;
  if(Setup[5] || Setup[7] || (ULONG)Setup[6]>N || (Setup[6] && !Data && !Mdl)) goto Exit;
  if(Mdl && Setup[6]) {S=UsbMdlProbe(Mdl,Setup[6]);if(!NT_SUCCESS(S)) goto Exit;}
  if(Setup[0]==0xa3 && Setup[1]==0 && !Setup[2] && !Setup[3] && Setup[4]==1 && Setup[6]==4) {
    Core=UsbGetRootPortStatus(D,&Status,&Change); S=UsbStatus(Core,&Us);
    if(NT_SUCCESS(S)) { Result[0]=(UCHAR)Status;Result[1]=(UCHAR)(Status>>8);Result[2]=(UCHAR)Change;Result[3]=(UCHAR)(Change>>8);Done=4; }
  } else if(Setup[0]==0xa0 && Setup[1]==0 && !Setup[2] && !Setup[3] && !Setup[4] && Setup[6]==4) {
    Done=4;S=STATUS_SUCCESS;Us=USBD_STATUS_SUCCESS;
  } else if(Setup[0]==0x23 && (Setup[1]==1 || Setup[1]==3) && Setup[4]==1 && !Setup[3] && !Setup[6]) {
    Core=UsbRootPortFeature(D,Setup[1]==3,Setup[2]);
    S=UsbStatus(Core,&Us);
  } else if(Setup[0]==0x20 && Setup[1]==1 && !Setup[3] && !Setup[4] && !Setup[6] && Setup[2]<=1) {
    S=STATUS_SUCCESS;Us=USBD_STATUS_SUCCESS;
  } else { S=STATUS_NOT_SUPPORTED;Us=USBD_STATUS_STALL_PID; }
Copy:
  if(Done) {
    if(Mdl) S=UsbMdlCopy(Mdl,Result,Done,TRUE);else RtlCopyMemory(Data,Result,Done);
    if(!NT_SUCCESS(S)) {Done=0;Us=USBD_STATUS_INSUFFICIENT_RESOURCES;}
  }
Exit:
  UsbFinish(R,S,Us,Done);
}
static UCHAR UsbReserveAddress(USB_DEVICE *U) {
  USB_CONTROLLER *D=U->Controller;
  UCHAR Address;
  WdfSpinLockAcquire(D->EndpointLock);
  if(!U->ReservedAddress) {
    for(ULONG Slot=1;Slot<ARRAYSIZE(D->AddressInUse);Slot++) {
      if(!D->AddressInUse[Slot]) {
        D->AddressInUse[Slot]=TRUE;
        U->ReservedAddress=(UCHAR)Slot;
        break;
      }
    }
  }
  Address=U->ReservedAddress;
  WdfSpinLockRelease(D->EndpointLock);
  return Address;
}
static void UsbRunAddress(USB_CONTROLLER *D,USB_REQUEST *R) {
  PUSBDEVICE_ADDRESS P=UsbArgument(R->Request); USB_DEVICE *U; MUSB_HOST_ROUTE Route;
  UCHAR Setup[8]={0,5,0,0,0,0,0,0}; uint32_t Done=0; USBD_STATUS Us; NTSTATUS S;
  if(!UsbBeginOperation(D,R)) return;
  if(!P || P->Size<sizeof(*P) || !P->UsbDevice) {
    UsbFinish(R,STATUS_INVALID_PARAMETER,USBD_STATUS_INVALID_PARAMETER,0);return;
  }
  P->Address=0;
  U=UsbDeviceContext(P->UsbDevice);
  if(U->Controller!=D) {UsbFinish(R,STATUS_INVALID_PARAMETER,USBD_STATUS_INVALID_PARAMETER,0);return;}
  if(D->WaitingForVbus || D->Core.PortPowerLost || !U->Enabled || !U->DefaultEndpoint) {UsbFinish(R,STATUS_DEVICE_NOT_READY,USBD_STATUS_DEVICE_GONE,0);return;}
  S=UsbRoute(U,&Route);
  if(NT_SUCCESS(S)) {
    // UCX supplies the device. The host selects and returns its address.
    Setup[2]=UsbReserveAddress(U);
    if(!Setup[2]) {UsbFinish(R,STATUS_INSUFFICIENT_RESOURCES,USBD_STATUS_INSUFFICIENT_RESOURCES,0);return;}
    S=UsbStatus(MusbHostControlTransferRouted(&D->Core,0,UsbSpeed(U),
        (uint8_t)U->DefaultEndpoint->MaximumPacket,Setup,NULL,0,1000,&Route,&Done),&Us);
    if(NT_SUCCESS(S)) {
      UsbDelay(NULL,2000);
      WdfSpinLockAcquire(D->EndpointLock);
      if(D->Stopping || !U->Enabled) S=STATUS_DEVICE_NOT_READY;
      else {U->Address=Setup[2];P->Address=Setup[2];}
      WdfSpinLockRelease(D->EndpointLock);
    }
  }
  UsbFinish(R,S,NT_SUCCESS(S)?USBD_STATUS_SUCCESS:USBD_STATUS_XACT_ERROR,0);
}
static void UsbRunConfigure(USB_CONTROLLER *D,USB_REQUEST *R) {
  PENDPOINTS_CONFIGURE P=UsbArgument(R->Request);ULONG I;NTSTATUS S=STATUS_INVALID_PARAMETER;
  if(!UsbBeginOperation(D,R)) return;
  if(!P || P->Size<sizeof(*P) || !P->UsbDevice || P->EndpointsToEnableCount>256 || P->EndpointsToDisableCount>256 ||
      (P->EndpointsToEnableCount && !P->EndpointsToEnable) ||
      (P->EndpointsToDisableCount && !P->EndpointsToDisable)) goto Done;
  for(I=0;I<P->EndpointsToDisableCount;I++) {
    USB_ENDPOINT *E=UsbEndpoint(P->EndpointsToDisable[I]);
    if(E->Device->Handle!=P->UsbDevice) goto Done;
  }
  for(I=0;I<P->EndpointsToEnableCount;I++) {
    USB_ENDPOINT *E=UsbEndpoint(P->EndpointsToEnable[I]);
    if(E->Device->Handle!=P->UsbDevice || E->Dead) goto Done;
  }
  for(I=0;I<P->EndpointsToDisableCount;I++) {
    USB_ENDPOINT *E=UsbEndpoint(P->EndpointsToDisable[I]);
    WdfSpinLockAcquire(D->EndpointLock);E->Stopped=1;E->ConfigStopPending=1;WdfSpinLockRelease(D->EndpointLock);
    S=UsbWaitForQueueStarts(D);if(!NT_SUCCESS(S)) goto Done;
    WdfIoQueuePurgeSynchronously(E->Incoming);WdfIoQueuePurgeSynchronously(E->Pending);
    WdfSpinLockAcquire(D->EndpointLock);E->ConfigStopPending=0;WdfSpinLockRelease(D->EndpointLock);
  }
  for(I=0;I<P->EndpointsToEnableCount;I++) {
    USB_ENDPOINT *E=UsbEndpoint(P->EndpointsToEnable[I]);
    WdfSpinLockAcquire(D->EndpointLock);E->Toggle=0;WdfSpinLockRelease(D->EndpointLock);
    UsbEndpointStart(D->Ucx,E->Handle);
  }
  S=STATUS_SUCCESS;
Done:UsbFinish(R,S,NT_SUCCESS(S)?USBD_STATUS_SUCCESS:USBD_STATUS_INVALID_PARAMETER,0);
}
void UsbWork(WDFWORKITEM Work) {
  USB_CONTROLLER *D=UsbObject(Work)->Controller; WDFREQUEST Request;
  ULONG I,Count; USB_ENDPOINT *E; uint16_t Status,Change;
  WdfSpinLockAcquire(D->EndpointLock);
  if(D->WorkerThread) {
    WdfSpinLockRelease(D->EndpointLock);
    return;
  }
  if(!D->Online || D->Stopping) {
    D->WorkerScheduled=FALSE;
    WdfSpinLockRelease(D->EndpointLock);
    return;
  }
  D->WorkerThread=PsGetCurrentThread();
  D->WorkerScheduled=TRUE;
  WdfSpinLockRelease(D->EndpointLock);
  UsbPollVbus(D);
  if(D->Stopping) goto Done;
  if(!D->WaitingForVbus && (D->Core.PortStatus&MUSB_PORT_SUSPEND)) {
    (void)MusbHostServiceRemoteWake(&D->Core);
    if(D->Stopping) goto Done;
  }
  for(I=0;I<8 && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(D->Management,&Request));I++) {
    USB_REQUEST *R=UsbRequest(Request);
    if(R->Job==JobAddress) UsbRunAddress(D,R);
    else if(R->Job==JobConfigure) UsbRunConfigure(D,R);
    else UsbRunRoot(D,R);
    if(D->Stopping) goto Done;
  }
  if(UsbGetRootPortStatus(D,&Status,&Change)==MusbOk) {
    ULONG Pending=Change&~D->LastPortChange;
    D->LastPortChange=Change;
    if(Pending) UcxRootHubPortChanged(D->RootHub);
    if(Change && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(D->RootInterrupt,&Request)))
      UsbRunRoot(D,UsbRequest(Request));
  }
  // Keep URBs queued while root-hub control can still resume the port.
  if(D->Core.PortStatus&MUSB_PORT_SUSPEND) goto Done;
  WdfSpinLockAcquire(D->EndpointLock); Count=D->EndpointCount; WdfSpinLockRelease(D->EndpointLock);
  if(Count>USB_EP_BUDGET) Count=USB_EP_BUDGET;
  for(I=0;I<Count && !D->Stopping;I++) {
    WdfSpinLockAcquire(D->EndpointLock);
    if(IsListEmpty(&D->Endpoints)) {WdfSpinLockRelease(D->EndpointLock);break;}
    E=CONTAINING_RECORD(RemoveHeadList(&D->Endpoints),USB_ENDPOINT,Link);
    InsertTailList(&D->Endpoints,&E->Link); WdfObjectReference(E->Handle);
    WdfSpinLockRelease(D->EndpointLock);
    if(!E->Dead && !E->Stopped && (E->Type!=USB_ENDPOINT_TYPE_INTERRUPT || UsbNow(NULL)>=E->NextPoll) &&
        NT_SUCCESS(WdfIoQueueRetrieveNextRequest(E->Pending,&Request)))
      UsbRunTransfer(D,E,UsbRequest(Request));
    WdfObjectDereference(E->Handle);
  }
Done:
  WdfSpinLockAcquire(D->EndpointLock);
  UsbUpdatePortInterruptLocked(D,TRUE);
  D->WorkerThread=NULL;
  D->WorkerScheduled=FALSE;
  WdfSpinLockRelease(D->EndpointLock);
}
void UsbTimer(WDFTIMER Timer) {
  USB_CONTROLLER *D=UsbObject(Timer)->Controller;
  WdfSpinLockAcquire(D->EndpointLock);
  UsbScheduleLocked(D);
  WdfSpinLockRelease(D->EndpointLock);
}
void UsbSubmit(WDFQUEUE Q,WDFREQUEST R,size_t Out,size_t In,ULONG Code) {
  USB_ENDPOINT *E=UsbQueue(Q)->Endpoint; PURB U=UsbArgument(R);
  UNREFERENCED_PARAMETER(Out);UNREFERENCED_PARAMETER(In);
  if(Code!=IOCTL_INTERNAL_USB_SUBMIT_URB || !U) {UsbCompleteNow(R,STATUS_NOT_SUPPORTED);return;}
  UsbEnqueue(E->Controller,E,R,JobTransfer,U,E->Pending);
}
void UsbIoControl(WDFQUEUE Q,WDFREQUEST R,size_t Out,size_t In,ULONG Code) {
  WDFDEVICE Device=WdfIoQueueGetDevice(Q);
  if(!UcxIoDeviceControl(Device,R,Out,In,Code)) UsbCompleteNow(R,STATUS_NOT_SUPPORTED);
}
static NTSTATUS UsbManualQueue(USB_CONTROLLER *D,WDFOBJECT Parent,WDFQUEUE *Q) {
  WDF_IO_QUEUE_CONFIG C;WDF_OBJECT_ATTRIBUTES A;NTSTATUS S;
  WDF_IO_QUEUE_CONFIG_INIT(&C,WdfIoQueueDispatchManual);
  C.PowerManaged=WdfFalse;C.EvtIoCanceledOnQueue=UsbQueuedCancel;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,USB_QUEUE);A.ParentObject=Parent;
  S=WdfIoQueueCreate(D->Device,&C,&A,Q);
  if(NT_SUCCESS(S)) UsbQueue(*Q)->Controller=D;
  return S;
}
static NTSTATUS UsbCreateEndpoint(USB_DEVICE *U,PUCXENDPOINT_INIT Init,UCHAR Address,UCHAR Type,USHORT Packet,UCHAR Interval) {
  USB_CONTROLLER *D=U->Controller; USB_ENDPOINT *E; UCXENDPOINT Handle;NTSTATUS S;
  UCX_ENDPOINT_EVENT_CALLBACKS Events; UCX_DEFAULT_ENDPOINT_EVENT_CALLBACKS Default;
  WDF_OBJECT_ATTRIBUTES A;WDF_IO_QUEUE_CONFIG Q;
  if(Type==USB_ENDPOINT_TYPE_CONTROL) {
    UCX_DEFAULT_ENDPOINT_EVENT_CALLBACKS_INIT(&Default,UsbEndpointPurge,UsbEndpointStart,UsbEndpointAbort,UsbOkToCancel,UsbDefaultUpdate);
    UcxDefaultEndpointInitSetEventCallbacks(Init,&Default);
  } else {
    UCX_ENDPOINT_EVENT_CALLBACKS_INIT(&Events,UsbEndpointPurge,UsbEndpointStart,UsbEndpointAbort,UsbEndpointReset,UsbOkToCancel,NULL,NULL,NULL);
    UcxEndpointInitSetEventCallbacks(Init,&Events);
  }
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,USB_ENDPOINT);A.EvtCleanupCallback=UsbEndpointCleanup;
  S=UcxEndpointCreate(U->Handle,&Init,&A,&Handle);if(!NT_SUCCESS(S)) return S;
  E=UsbEndpoint(Handle);E->Controller=D;E->Device=U;E->Handle=Handle;
  E->Address=Address;E->Type=Type;E->MaximumPacket=Packet;E->Interval=Interval;E->Stopped=1;
  InitializeListHead(&E->StartLink);
  if(Type==USB_ENDPOINT_TYPE_INTERRUPT) {
    E->PollUs=U->Speed==UsbHighSpeed?(125ULL<<(Interval-1)):(ULONGLONG)Interval*1000;
    /* A Windows timer has millisecond granularity; shorter HS periods are
     * polled at 1ms. The MUSB packet transaction itself uses the descriptor. */
  }
  WDF_IO_QUEUE_CONFIG_INIT(&Q,WdfIoQueueDispatchParallel);Q.PowerManaged=WdfFalse;
  Q.EvtIoInternalDeviceControl=UsbSubmit;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,USB_QUEUE);A.ParentObject=Handle;
  S=WdfIoQueueCreate(D->Device,&Q,&A,&E->Incoming);if(!NT_SUCCESS(S)) return S;
  UsbQueue(E->Incoming)->Controller=D;UsbQueue(E->Incoming)->Endpoint=E;
  S=UsbManualQueue(D,Handle,&E->Pending);if(!NT_SUCCESS(S)) return S;
  UsbQueue(E->Pending)->Endpoint=E;
  WdfSpinLockAcquire(D->EndpointLock);
  if(D->Stopping) {WdfSpinLockRelease(D->EndpointLock);return STATUS_DEVICE_NOT_READY;}
  InsertTailList(&D->Endpoints,&E->Link);E->Listed=TRUE;D->EndpointCount++;WdfSpinLockRelease(D->EndpointLock);
  if(Type==USB_ENDPOINT_TYPE_CONTROL) U->DefaultEndpoint=E;
  UcxEndpointSetWdfIoQueue(Handle,E->Incoming);return STATUS_SUCCESS;
}
NTSTATUS UsbDefaultEndpointAdd(UCXCONTROLLER C,UCXUSBDEVICE Device,ULONG Packet,PUCXENDPOINT_INIT Init) {
  USB_DEVICE *U=UsbDeviceContext(Device);UNREFERENCED_PARAMETER(C);
  if((Packet!=8 && Packet!=16 && Packet!=32 && Packet!=64) || (U->Speed==UsbLowSpeed && Packet!=8) ||
      (U->Speed==UsbHighSpeed && Packet!=64)) return STATUS_INVALID_PARAMETER;
  return UsbCreateEndpoint(U,Init,0,USB_ENDPOINT_TYPE_CONTROL,(USHORT)Packet,0);
}
NTSTATUS UsbEndpointAdd(UCXCONTROLLER C,UCXUSBDEVICE Device,PUSB_ENDPOINT_DESCRIPTOR Desc,ULONG Bytes,
    PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR Super,PUCXENDPOINT_INIT Init) {
  USB_DEVICE *U=UsbDeviceContext(Device);UCHAR Type;USHORT Packet;UNREFERENCED_PARAMETER(C);
  if(!Desc || Super || Bytes<sizeof(*Desc) || Desc->bLength<sizeof(*Desc) ||
      Desc->bDescriptorType!=USB_ENDPOINT_DESCRIPTOR_TYPE || !(Desc->bEndpointAddress&15) || (Desc->bEndpointAddress&0x70)) return STATUS_INVALID_PARAMETER;
  Type=Desc->bmAttributes&USB_ENDPOINT_TYPE_MASK;Packet=Desc->wMaxPacketSize;
  if((Type!=USB_ENDPOINT_TYPE_BULK && Type!=USB_ENDPOINT_TYPE_INTERRUPT) || !Packet || Packet>512 ||
      (U->Speed==UsbLowSpeed && (Type!=USB_ENDPOINT_TYPE_INTERRUPT || Packet>8)) ||
      (U->Speed==UsbFullSpeed && Packet>64) ||
      (Type==USB_ENDPOINT_TYPE_INTERRUPT && (!Desc->bInterval || (U->Speed==UsbHighSpeed && Desc->bInterval>16))))
    return STATUS_NOT_SUPPORTED;
  return UsbCreateEndpoint(U,Init,Desc->bEndpointAddress,Type,Packet,Desc->bInterval);
}
void UsbEndpointCleanup(WDFOBJECT Object) {
  USB_ENDPOINT *E=UsbEndpoint(Object);USB_CONTROLLER *D=E->Controller;
  if(!D) return;
  WdfSpinLockAcquire(D->EndpointLock);
  E->Dead=1;E->Stopped=1;
  if(E->Listed) {RemoveEntryList(&E->Link);E->Listed=FALSE;D->EndpointCount--;}
  if(E->Device && E->Device->DefaultEndpoint==E) E->Device->DefaultEndpoint=NULL;
  WdfSpinLockRelease(D->EndpointLock);
}
static void UsbMaybeStopComplete(USB_ENDPOINT *E) {
  USB_CONTROLLER *D=E->Controller;LONG Kind=0;WDFREQUEST DisableRequest=NULL;
  WdfSpinLockAcquire(D->EndpointLock);
  if(!E->Purges && !E->Inflight && E->StopKind && E->QueueStopIssued && !E->QueueStartActive) {
    Kind=E->StopKind;E->StopKind=0;E->QueueStopIssued=0;E->QueueStopPending=0;
    if((Kind&USB_STOP_DEVICE_DISABLE) && E->Device) {
      DisableRequest=E->Device->DisableRequest;
      E->Device->DisableRequest=NULL;
    }
  }
  WdfSpinLockRelease(D->EndpointLock);
  if(Kind) {
    if(Kind&USB_STOP_PURGE) UcxEndpointPurgeComplete(E->Handle);
    if(Kind&USB_STOP_ABORT) UcxEndpointAbortComplete(E->Handle);
    if(DisableRequest) {
      UsbCompleteNow(DisableRequest,STATUS_SUCCESS);
      WdfObjectDereference((WDFOBJECT)DisableRequest);
    }
    WdfObjectDereference(E->Handle);
  }
}
void UsbPurgeDone(WDFQUEUE Q,WDFCONTEXT Context) {
  USB_ENDPOINT *E=Context;UNREFERENCED_PARAMETER(Q);
  InterlockedDecrement(&E->Purges);UsbMaybeStopComplete(E);
}
static BOOLEAN UsbPrepareEndpointStopLocked(USB_ENDPOINT *E) {
  if(!E->StopKind || E->QueueStartDispatching || E->QueueStopIssued)return FALSE;
  E->QueueStopPending=0;E->QueueStopIssued=1;E->Purges=2;WdfObjectReference(E->Handle);return TRUE;
}
static VOID UsbIssueEndpointStop(USB_ENDPOINT *E) {
  WdfIoQueueStopAndPurge(E->Incoming,UsbPurgeDone,E);
  WdfIoQueueStopAndPurge(E->Pending,UsbPurgeDone,E);
}
static void UsbStopEndpoint(USB_ENDPOINT *E,LONG Kind) {
  USB_CONTROLLER *D=E->Controller;BOOLEAN Start,CheckComplete;
  WdfSpinLockAcquire(D->EndpointLock);
  E->Stopped=1;
  E->StopKind|=Kind;
  Start=UsbPrepareEndpointStopLocked(E);CheckComplete=E->QueueStopIssued;
  if(!Start && !CheckComplete)E->QueueStopPending=1;
  WdfSpinLockRelease(D->EndpointLock);
  if(Start)UsbIssueEndpointStop(E);
  else if(CheckComplete)UsbMaybeStopComplete(E);
}
void UsbEndpointPurge(UCXCONTROLLER C,UCXENDPOINT E) {UNREFERENCED_PARAMETER(C);UsbStopEndpoint(UsbEndpoint(E),USB_STOP_PURGE);}
void UsbEndpointAbort(UCXCONTROLLER C,UCXENDPOINT E) {UNREFERENCED_PARAMETER(C);UsbStopEndpoint(UsbEndpoint(E),USB_STOP_ABORT);}
static BOOLEAN UsbEndpointStartBeginLocked(USB_ENDPOINT *E,BOOLEAN EnableDevice,BOOLEAN RequireEnabled,BOOLEAN RequireNotStopped) {
  USB_CONTROLLER *D=E->Controller;
  if(!D->Online || D->Stopping || E->Dead || E->StopKind || E->QueueStartActive ||
      E->StartListed || E->QueueStopPending || E->QueueStopIssued || E->ConfigStopPending ||
      (RequireEnabled && !E->Device->Enabled) || (RequireNotStopped && E->Stopped))return FALSE;
  if(EnableDevice)E->Device->Enabled=1;
  E->Stopped=0;E->QueueStartActive=1;E->QueueStartDispatching=1;E->QueueStartThread=PsGetCurrentThread();
  InsertTailList(&D->QueueStartingEndpoints,&E->StartLink);E->StartListed=TRUE;
  if(D->QueueStartsActive++==0)KeClearEvent(&D->QueueStartsDrained);
  WdfObjectReference(E->Handle);WdfObjectReference(D->Device);return TRUE;
}
static BOOLEAN UsbEndpointStartFinish(USB_ENDPOINT *E) {
  USB_CONTROLLER *D=E->Controller;BOOLEAN Stop,Started;
  /* WdfIoQueueStart can synchronously dispatch EvtIo callbacks. No driver
   * lock is held here; QueueStartActive and object references protect state. */
  WdfIoQueueStart(E->Pending);WdfIoQueueStart(E->Incoming);WdfWorkItemEnqueue(D->Worker);
  WdfSpinLockAcquire(D->EndpointLock);
  E->QueueStartDispatching=0;
  Stop=UsbPrepareEndpointStopLocked(E);
  Started=D->Online && !D->Stopping && !E->Dead && !E->Stopped && !E->StopKind && E->Device->Enabled;
  WdfSpinLockRelease(D->EndpointLock);
  if(Stop)UsbIssueEndpointStop(E);
  WdfSpinLockAcquire(D->EndpointLock);
  if(E->StartListed) {RemoveEntryList(&E->StartLink);E->StartListed=FALSE;}
  E->QueueStartActive=0;E->QueueStartDispatching=0;E->QueueStartThread=NULL;
  D->QueueStartsActive--;if(D->QueueStartsActive==0)KeSetEvent(&D->QueueStartsDrained,IO_NO_INCREMENT,FALSE);
  WdfSpinLockRelease(D->EndpointLock);
  /* A synchronous WDF purge callback may have drained both queues while the
   * start pin still prevented stop completion. Recheck after dropping it. */
  UsbMaybeStopComplete(E);
  WdfObjectDereference(E->Handle);WdfObjectDereference(D->Device);return Started;
}
static BOOLEAN UsbQueueStartCurrentThreadLocked(USB_CONTROLLER *D) {
  PLIST_ENTRY Link;
  for(Link=D->QueueStartingEndpoints.Flink;Link!=&D->QueueStartingEndpoints;Link=Link->Flink) {
    USB_ENDPOINT *E=CONTAINING_RECORD(Link,USB_ENDPOINT,StartLink);
    if(E->QueueStartThread==PsGetCurrentThread())return TRUE;
  }
  return FALSE;
}
static BOOLEAN UsbOnRequestThreadLocked(USB_CONTROLLER *D) {
  return D->WorkerThread==PsGetCurrentThread() ||
      (D->QueueStartsActive && UsbQueueStartCurrentThreadLocked(D));
}
static NTSTATUS UsbWaitForQueueStarts(USB_CONTROLLER *D) {
  BOOLEAN Wait;
  WdfSpinLockAcquire(D->EndpointLock);Wait=D->QueueStartsActive!=0;WdfSpinLockRelease(D->EndpointLock);
  if(!Wait)return STATUS_SUCCESS;
  return KeWaitForSingleObject(&D->QueueStartsDrained,Executive,KernelMode,FALSE,NULL);
}
void UsbEndpointStart(UCXCONTROLLER C,UCXENDPOINT Endpoint) {
  USB_ENDPOINT *E=UsbEndpoint(Endpoint);USB_CONTROLLER *D=E->Controller;BOOLEAN Start;UNREFERENCED_PARAMETER(C);
  WdfSpinLockAcquire(D->EndpointLock);Start=UsbEndpointStartBeginLocked(E,FALSE,TRUE,FALSE);WdfSpinLockRelease(D->EndpointLock);
  if(Start)(void)UsbEndpointStartFinish(E);
}
void UsbOkToCancel(UCXENDPOINT E) {UNREFERENCED_PARAMETER(E);}
void UsbEndpointReset(UCXCONTROLLER C,UCXENDPOINT Endpoint,WDFREQUEST R) {
  USB_ENDPOINT *E=UsbEndpoint(Endpoint);PENDPOINT_RESET P=UsbArgument(R);UNREFERENCED_PARAMETER(C);
  if(!P || P->Size<sizeof(*P) || P->Endpoint!=Endpoint || !E->Stopped || E->StopKind) {UsbCompleteNow(R,STATUS_INVALID_DEVICE_STATE);return;}
  if(!(P->Flags&FlagEndpointResetPreserveTransferState)) E->Toggle=0;
  UsbCompleteNow(R,STATUS_SUCCESS);
}
void UsbDefaultUpdate(UCXCONTROLLER C,WDFREQUEST R) {
  PDEFAULT_ENDPOINT_UPDATE P=UsbArgument(R);USB_ENDPOINT *E;UNREFERENCED_PARAMETER(C);
  if(!P || P->Size<sizeof(*P) || !P->DefaultEndpoint) {UsbCompleteNow(R,STATUS_INVALID_PARAMETER);return;}
  E=UsbEndpoint(P->DefaultEndpoint);
  if(
      (P->MaxPacketSize!=8 && P->MaxPacketSize!=16 && P->MaxPacketSize!=32 && P->MaxPacketSize!=64) ||
      (E->Device->Speed==UsbLowSpeed && P->MaxPacketSize!=8) ||
      (E->Device->Speed==UsbHighSpeed && P->MaxPacketSize!=64)) {UsbCompleteNow(R,STATUS_INVALID_PARAMETER);return;}
  E->MaximumPacket=(USHORT)P->MaxPacketSize;UsbCompleteNow(R,STATUS_SUCCESS);
}
static void UsbDeviceCleanup(WDFOBJECT Object) {
  USB_DEVICE *U=UsbDeviceContext(Object);USB_CONTROLLER *D=U->Controller;
  if(!D) return;
  // Keep the address reserved across a reset or a failed SET_ADDRESS.
  WdfSpinLockAcquire(D->EndpointLock);
  if(U->ReservedAddress) {
    D->AddressInUse[U->ReservedAddress]=FALSE;
    U->ReservedAddress=0;
  }
  WdfSpinLockRelease(D->EndpointLock);
}
NTSTATUS UsbDeviceAdd(UCXCONTROLLER C,PUCXUSBDEVICE_INFO Info,PUCXUSBDEVICE_INIT Init) {
  USB_CONTROLLER *D=UsbObject(C)->Controller;USB_DEVICE *U;UCXUSBDEVICE Device;NTSTATUS S;
  UCX_USBDEVICE_EVENT_CALLBACKS Events;WDF_OBJECT_ATTRIBUTES A;
  if(!Init || !Info || Info->Size<sizeof(*Info) || (Info->DeviceSpeed!=UsbLowSpeed && Info->DeviceSpeed!=UsbFullSpeed && Info->DeviceSpeed!=UsbHighSpeed) ||
      Info->PortPath.PortPathDepth<1 || Info->PortPath.PortPathDepth>MAX_USB_DEVICE_DEPTH ||
      Info->PortPath.PortPath[0]!=1) return STATUS_NOT_SUPPORTED;
  UCX_USBDEVICE_EVENT_CALLBACKS_INIT(&Events,UsbConfigure,UsbEnable,UsbDisable,UsbResetDevice,UsbAddressDevice,
      UsbUpdateDevice,UsbHubInfo,UsbDefaultEndpointAdd,UsbEndpointAdd);
  UcxUsbDeviceInitSetEventCallbacks(Init,&Events);
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,USB_DEVICE);A.EvtCleanupCallback=UsbDeviceCleanup;
  S=UcxUsbDeviceCreate(C,&Init,&A,&Device);if(!NT_SUCCESS(S)) return S;
  U=UsbDeviceContext(Device);U->Handle=Device;U->Controller=D;U->Speed=Info->DeviceSpeed;
  U->TtHub=Info->TtHub;U->Path=Info->PortPath;return STATUS_SUCCESS;
}
void UsbEnable(UCXCONTROLLER C,WDFREQUEST R) {
  PUSBDEVICE_ENABLE P=UsbArgument(R);USB_DEVICE *U;USB_ENDPOINT *E;USB_CONTROLLER *D;NTSTATUS S=STATUS_SUCCESS;BOOLEAN Start=FALSE,Started=FALSE;
  if(!P || P->Size<sizeof(*P) || !P->UsbDevice || !P->DefaultEndpoint) {UsbCompleteNow(R,STATUS_INVALID_PARAMETER);return;}
  U=UsbDeviceContext(P->UsbDevice);E=UsbEndpoint(P->DefaultEndpoint);D=U->Controller;
  if(!D || D->Ucx!=C || U->Handle!=P->UsbDevice || E->Handle!=P->DefaultEndpoint || E->Controller!=D || E->Device!=U) {
    UsbCompleteNow(R,STATUS_INVALID_PARAMETER);return;
  }
  WdfSpinLockAcquire(D->EndpointLock);
  if(D->Stopping || !D->Online) S=STATUS_DEVICE_NOT_READY;
  else if(U->DefaultEndpoint!=E) S=STATUS_INVALID_PARAMETER;
  else if(E->Dead) S=STATUS_DEVICE_NOT_READY;
  else if(U->DisableRequest || E->StopKind || E->QueueStartActive || E->ConfigStopPending) S=STATUS_DEVICE_BUSY;
  else Start=UsbEndpointStartBeginLocked(E,TRUE,FALSE,FALSE);
  if(!Start && S==STATUS_SUCCESS)S=STATUS_DEVICE_NOT_READY;
  WdfSpinLockRelease(D->EndpointLock);
  if(Start) {Started=UsbEndpointStartFinish(E);if(!Started)S=STATUS_DEVICE_BUSY;}
  UsbCompleteNow(R,S);
}
void UsbDisable(UCXCONTROLLER C,WDFREQUEST R) {
  PUSBDEVICE_DISABLE P=UsbArgument(R);USB_DEVICE *U;USB_ENDPOINT *E;USB_CONTROLLER *D;NTSTATUS S=STATUS_SUCCESS;
  if(!P || P->Size<sizeof(*P) || !P->UsbDevice || !P->DefaultEndpoint) {UsbCompleteNow(R,STATUS_INVALID_PARAMETER);return;}
  U=UsbDeviceContext(P->UsbDevice);E=UsbEndpoint(P->DefaultEndpoint);D=U->Controller;
  if(!D || D->Ucx!=C || U->Handle!=P->UsbDevice || E->Handle!=P->DefaultEndpoint ||
      E->Controller!=D || E->Device!=U || !E->Incoming || !E->Pending) {
    UsbCompleteNow(R,STATUS_INVALID_PARAMETER);return;
  }
  WdfSpinLockAcquire(D->EndpointLock);
  if(U->DefaultEndpoint!=E) S=STATUS_INVALID_PARAMETER;
  else if(U->DisableRequest) S=STATUS_DEVICE_BUSY;
  else {
    U->Enabled=0;U->Address=0;
    if(E->Stopped && !E->StopKind && !E->Inflight) {
      WdfSpinLockRelease(D->EndpointLock);UsbCompleteNow(R,STATUS_SUCCESS);return;
    }
    E->Stopped=1;
    WdfObjectReference((WDFOBJECT)R);
    U->DisableRequest=R;
  }
  WdfSpinLockRelease(D->EndpointLock);
  if(!NT_SUCCESS(S)) {UsbCompleteNow(R,S);return;}
  UsbStopEndpoint(E,USB_STOP_DEVICE_DISABLE);
}
void UsbResetDevice(UCXCONTROLLER C,WDFREQUEST R) {
  PUSBDEVICE_RESET P=UsbArgument(R);USB_DEVICE *U;USB_CONTROLLER *D;PLIST_ENTRY Link;NTSTATUS S=STATUS_SUCCESS;
  if(!P || P->Size<sizeof(*P) || !P->UsbDevice) {UsbCompleteNow(R,STATUS_INVALID_PARAMETER);return;}
  U=UsbDeviceContext(P->UsbDevice);D=U->Controller;
  if(!D || D->Ucx!=C || U->Handle!=P->UsbDevice) {UsbCompleteNow(R,STATUS_INVALID_PARAMETER);return;}
  WdfSpinLockAcquire(D->EndpointLock);
  if(D->Stopping || !D->Online) S=STATUS_DEVICE_NOT_READY;
  else if(U->DisableRequest) S=STATUS_DEVICE_BUSY;
  else {
    for(Link=D->Endpoints.Flink;Link!=&D->Endpoints;Link=Link->Flink) {
      USB_ENDPOINT *E=CONTAINING_RECORD(Link,USB_ENDPOINT,Link);
      if(E->Device==U && (E->Inflight || E->QueueStartActive || E->StopKind || E->ConfigStopPending)) {
        S=STATUS_DEVICE_BUSY;break;
      }
    }
    if(S==STATUS_SUCCESS) {
      /* UCX reports that the bus reset already happened. Mirror its default
       * address/toggle state; do not claim a hardware reset or re-enable. */
      U->Address=0;
      for(Link=D->Endpoints.Flink;Link!=&D->Endpoints;Link=Link->Flink) {
        USB_ENDPOINT *E=CONTAINING_RECORD(Link,USB_ENDPOINT,Link);
        if(E->Device==U)E->Toggle=0;
      }
    }
  }
  WdfSpinLockRelease(D->EndpointLock);UsbCompleteNow(R,S);
}
void UsbAddressDevice(UCXCONTROLLER C,WDFREQUEST R) {
  USB_CONTROLLER *D=UsbObject(C)->Controller;UsbEnqueue(D,NULL,R,JobAddress,NULL,D->Management);
}
void UsbUpdateDevice(UCXCONTROLLER C,WDFREQUEST R) {
  PUSBDEVICE_UPDATE P=UsbArgument(R);UNREFERENCED_PARAMETER(C);
  if(!P || P->Size<sizeof(*P) || !P->UsbDevice) {UsbCompleteNow(R,STATUS_INVALID_PARAMETER);return;}
  UsbCompleteNow(R,STATUS_SUCCESS);
}
void UsbHubInfo(UCXCONTROLLER C,WDFREQUEST R) {
  PUSBDEVICE_HUB_INFO P=UsbArgument(R);UNREFERENCED_PARAMETER(C);
  if(!P || P->Size<sizeof(*P) || !P->UsbDevice || P->NumberOfPorts>127 || P->NumberOfTTs>127) {UsbCompleteNow(R,STATUS_INVALID_PARAMETER);return;}
  UsbDeviceContext(P->UsbDevice)->MultiTt=P->NumberOfTTs>1;UsbCompleteNow(R,STATUS_SUCCESS);
}
void UsbConfigure(UCXCONTROLLER C,WDFREQUEST R) {
  USB_CONTROLLER *D=UsbObject(C)->Controller;
  UsbEnqueue(D,NULL,R,JobConfigure,NULL,D->Management);
}
NTSTATUS UsbCapability(UCXCONTROLLER C,PGUID G,ULONG Bytes,PVOID Out,PULONG Result) {
  UNREFERENCED_PARAMETER(C);UNREFERENCED_PARAMETER(G);UNREFERENCED_PARAMETER(Bytes);UNREFERENCED_PARAMETER(Out);
  if(Result) *Result=0;
  /* No USB3, streams, offload, DMA or asynchronous TT cancel capability. */
  return STATUS_NOT_SUPPORTED;
}
NTSTATUS UsbFrame(UCXCONTROLLER C,PULONG Frame) {
  USB_CONTROLLER *D=UsbObject(C)->Controller;
  NTSTATUS Status=STATUS_DEVICE_NOT_READY;
  if(!Frame) return Status;

  /* Teardown closes admission under this lock before unmapping the registers. */
  WdfSpinLockAcquire(D->EndpointLock);
  if(D->Online && !D->Stopping && !D->WaitingForVbus && D->Registers) {
    *Frame=READ_REGISTER_USHORT((PUSHORT)(D->Registers+0x0c))&0x7ff;
    Status=STATUS_SUCCESS;
  }
  WdfSpinLockRelease(D->EndpointLock);
  return Status;
}
void UsbRootControl(UCXROOTHUB Hub,WDFREQUEST R) {
  USB_CONTROLLER *D=UsbObject(Hub)->Controller;UsbEnqueue(D,NULL,R,JobRootControl,UsbArgument(R),D->Management);
}
void UsbRootInterrupt(UCXROOTHUB Hub,WDFREQUEST R) {
  USB_CONTROLLER *D=UsbObject(Hub)->Controller;
  UsbEnqueue(D,NULL,R,JobRootInterrupt,UsbArgument(R),D->RootInterrupt);
}
void UsbRootInfo(UCXROOTHUB H,WDFREQUEST R) {
  PROOTHUB_INFO P=UsbArgument(R);UNREFERENCED_PARAMETER(H);
  if(!P || P->Size<sizeof(*P)) {UsbCompleteNow(R,STATUS_INVALID_PARAMETER);return;}
  P->ControllerType=ControllerTypeSoftXhci;P->NumberOf20Ports=1;P->NumberOf30Ports=0;
  P->MaxU1ExitLatency=0;P->MaxU2ExitLatency=0;UsbCompleteNow(R,STATUS_SUCCESS);
}
void UsbRoot20Info(UCXROOTHUB H,WDFREQUEST R) {
  PROOTHUB_20PORTS_INFO P=UsbArgument(R);PROOTHUB_20PORT_INFO Port;UNREFERENCED_PARAMETER(H);
  if(!P || P->Size<sizeof(*P) || P->NumberOfPorts!=1 || P->PortInfoSize<sizeof(*Port) || !P->PortInfoArray || !P->PortInfoArray[0]) {UsbCompleteNow(R,STATUS_INVALID_PARAMETER);return;}
  Port=P->PortInfoArray[0];Port->PortNumber=1;Port->MinorRevision=0;Port->HubDepth=0;
  Port->Removable=TriStateTrue;Port->IntegratedHubImplemented=TriStateUnknown;Port->DebugCapable=TriStateFalse;
  RtlZeroMemory(&Port->ControllerUsb20HardwareLpmFlags,sizeof(Port->ControllerUsb20HardwareLpmFlags));UsbCompleteNow(R,STATUS_SUCCESS);
}
void UsbRoot30Info(UCXROOTHUB H,WDFREQUEST R) {
  PROOTHUB_30PORTS_INFO P=UsbArgument(R);UNREFERENCED_PARAMETER(H);
  UsbCompleteNow(R,P && P->Size>=sizeof(*P) && !P->NumberOfPorts?STATUS_SUCCESS:STATUS_INVALID_PARAMETER);
}
static BOOLEAN UsbValidHandoffPowerPath(ULONG Revision,ULONG Flags,ULONG Path) {
  if(Revision==USB_HANDOFF_REVISION_LEGACY)
    return Flags==MUSB_READY_REQUIRED && Path==USB_HANDOFF_PATH_LEGACY;
  if(Revision==USB_HANDOFF_REVISION_POWER_PATH)
    return (Flags==USB_HANDOFF_FLAGS_TYPEC_SOURCE &&
        (Path==USB_HANDOFF_PATH_TYPEC_SOURCE || Path==USB_HANDOFF_PATH_TYPEC_ONLY)) ||
        (Flags==USB_HANDOFF_FLAGS_ID_LOW_HUB_POWER && Path==USB_HANDOFF_PATH_ID_LOW_HUB_POWER);
  return FALSE;
}
typedef struct {
  ULONG Flags,PowerPath;
  USHORT PortStatus;
  USHORT PortChange;
} USB_HANDOFF_STATE;

static BOOLEAN UsbValidHandoff(USB_CONTROLLER *D,USB_HANDOFF_STATE *State) {
  ULONG V[16],I;
  if(!D->Handoff || !State) return FALSE;
  *State=(USB_HANDOFF_STATE){0};
  for(I=0;I<16;I++) V[I]=READ_REGISTER_ULONG(D->Handoff+I);
  KeMemoryBarrier();
  if(READ_REGISTER_ULONG(D->Handoff)!=1 || V[0]!=1 ||
      !UsbValidHandoffPowerPath(V[1],V[2],V[15]) || V[3]!=USB_BASE || V[4]!=USB_BYTES ||
      V[5]!=MTK_MUSB_PLATFORM_PHY_BASE || V[6]!=MTK_MUSB_PLATFORM_PHY_BYTES || V[7]!=USB_GSIV || V[8]!=16 || V[9]!=1 ||
      V[10]!=MUSB_HOST_T70_FIFO_PROFILE || V[11]!=MUSB_HOST_CONNECT_DISCONNECT_MASK ||
      V[14]!=1 || (V[12]&~0x0703U)!=0 || !(V[12]&MUSB_PORT_POWER) ||
      (V[12]&0x600)==0x600 || (V[13]&~0x1fU)!=0 ||
      (!(V[12]&MUSB_PORT_CONNECTION) && V[12]!=MUSB_PORT_POWER)) {
    return FALSE;
  }
  *State=(USB_HANDOFF_STATE){V[2],V[15],(USHORT)V[12],(USHORT)V[13]};
  return TRUE;
}
static BOOLEAN UsbHostDevctlReady(USB_CONTROLLER *D) {
  return (READ_REGISTER_UCHAR(D->Registers+USB_DEVCTL_REGISTER)&USB_DEVCTL_HOST_READY_MASK)==USB_DEVCTL_HOST_READY_VALUE;
}
static BOOLEAN UsbHostPhyReady(USB_CONTROLLER *D) {
  ULONG Acr1,Acr6,Dtm0,Dtm1;
  if(!D->Phy) return TRUE;  // Older DSDTs supply only the controller and handoff record.
  Acr1=READ_REGISTER_ULONG((PULONG)(D->Phy+MTK_MUSB_PLATFORM_PHY_ACR1));
  Acr6=READ_REGISTER_ULONG((PULONG)(D->Phy+MTK_MUSB_PLATFORM_PHY_ACR6));
  Dtm0=READ_REGISTER_ULONG((PULONG)(D->Phy+MTK_MUSB_PLATFORM_PHY_DTM0));
  Dtm1=READ_REGISTER_ULONG((PULONG)(D->Phy+MTK_MUSB_PLATFORM_PHY_DTM1));
  return MtkMusbPlatformHostPhyConfigured(Acr1,Acr6,Dtm0,Dtm1)!=0;
}
static void UsbPowerQueues(USB_CONTROLLER *D,BOOLEAN Start);
static VOID CopyAcpiIntegerBytes(PVOID Destination,const __unaligned UCHAR *Source,ULONG Bytes) {
  UCHAR *Output=(UCHAR *)Destination;
  ULONG Index;
  for (Index=0;Index<Bytes;Index++) Output[Index]=Source[Index];
}
static BOOLEAN UsbFirmwareInteger32(PACPI_METHOD_ARGUMENT Argument,PULONG Value) {
  ULONGLONG Wide;
  if(!Argument||!Value||Argument->Type!=ACPI_METHOD_ARGUMENT_INTEGER) return FALSE;
  if(Argument->DataLength==sizeof(ULONG)) { CopyAcpiIntegerBytes(Value,Argument->Data,sizeof(*Value)); return TRUE; }
  if(Argument->DataLength!=sizeof(ULONGLONG)) return FALSE;
  CopyAcpiIntegerBytes(&Wide,Argument->Data,sizeof(Wide));
  if(Wide>MAXULONG) return FALSE;
  *Value=(ULONG)Wide; return TRUE;
}
static NTSTATUS UsbEvaluateAcpiInteger(USB_CONTROLLER *D,PVOID Input,ULONG InputSize,PULONG Value) {
  UCHAR Buffer[sizeof(ACPI_EVAL_OUTPUT_BUFFER)+8]={0};
  PACPI_EVAL_OUTPUT_BUFFER Out=(PACPI_EVAL_OUTPUT_BUFFER)Buffer;WDF_MEMORY_DESCRIPTOR A,B;
  WDF_REQUEST_SEND_OPTIONS O;ULONG_PTR Returned=0;NTSTATUS S;
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&A,Input,InputSize);WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&B,Buffer,sizeof(Buffer));
  WDF_REQUEST_SEND_OPTIONS_INIT(&O,WDF_REQUEST_SEND_OPTION_TIMEOUT);WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&O,WDF_REL_TIMEOUT_IN_SEC(1));
  S=WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(D->Device),NULL,IOCTL_ACPI_EVAL_METHOD,&A,&B,&O,&Returned);
  if(!NT_SUCCESS(S)) return S;
  if(Returned<sizeof(ACPI_EVAL_OUTPUT_BUFFER) || Returned>sizeof(Buffer) ||
      Out->Signature!=ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || Out->Count!=1 || Out->Length>Returned ||
      Out->Length<sizeof(ACPI_EVAL_OUTPUT_BUFFER) ||
      Out->Length<FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+Out->Argument[0].DataLength ||
      Returned<FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+Out->Argument[0].DataLength ||
      !UsbFirmwareInteger32(&Out->Argument[0],Value))
    return STATUS_DEVICE_NOT_READY;
  return STATUS_SUCCESS;
}
static NTSTATUS UsbAcpiInteger(USB_CONTROLLER *D,const char Name[4],PULONG Value) {
  ACPI_EVAL_INPUT_BUFFER In={0};
  In.Signature=ACPI_EVAL_INPUT_BUFFER_SIGNATURE;RtlCopyMemory(In.MethodName,Name,4);
  return UsbEvaluateAcpiInteger(D,&In,sizeof(In),Value);
}

static void UsbCloseTcpc(USB_CONTROLLER *D) {
  WDFIOTARGET Target=D->TcpcTarget;
  D->TcpcTarget=NULL;
  if(Target) {
    WdfIoTargetClose(Target);
    WdfObjectDelete(Target);
  }
}

static NTSTATUS UsbOpenTcpc(USB_CONTROLLER *D) {
  static const WCHAR Prefix[]=RESOURCE_HUB_DEVICE_NAME_PREFIX;
  static const WCHAR Hex[]=L"0123456789abcdef";
  WCHAR NameBuffer[RESOURCE_HUB_PATH_CHARS];
  UNICODE_STRING Name={sizeof(NameBuffer)-sizeof(WCHAR),sizeof(NameBuffer),NameBuffer};
  WDF_IO_TARGET_OPEN_PARAMS Open;
  WDFIOTARGET Target;
  // The Resource Hub file name is the full, zero-padded hexadecimal connection ID.
  RtlCopyMemory(NameBuffer,Prefix,sizeof(Prefix)-sizeof(WCHAR));
  for(ULONG Digit=0;Digit<RESOURCE_HUB_FILE_CHARS-1;Digit++) {
    ULONG Shift=(ULONG)(RESOURCE_HUB_FILE_CHARS-2-Digit)*4;
    NameBuffer[ARRAYSIZE(Prefix)-1+Digit]=Hex[(D->TcpcConnection>>Shift)&0xf];
  }
  NameBuffer[ARRAYSIZE(NameBuffer)-1]=UNICODE_NULL;
  NTSTATUS Status=WdfIoTargetCreate(D->Device,WDF_NO_OBJECT_ATTRIBUTES,&Target);
  if(Status!=STATUS_SUCCESS) return Status;
  WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Open,&Name,GENERIC_READ|GENERIC_WRITE);
  Open.ShareAccess=FILE_SHARE_READ|FILE_SHARE_WRITE;
  Open.CreateDisposition=FILE_OPEN;
  Open.FileAttributes=FILE_ATTRIBUTE_NORMAL;
  Status=WdfIoTargetOpen(Target,&Open);
  if(Status==STATUS_SUCCESS) D->TcpcTarget=Target;
  else WdfObjectDelete(Target);
  return Status;
}

static NTSTATUS UsbTcpcIoctl(WDFIOTARGET Target,ULONG Code,PWDF_MEMORY_DESCRIPTOR Input,PULONG_PTR Bytes) {
  WDF_REQUEST_SEND_OPTIONS Options;
  WDF_REQUEST_SEND_OPTIONS_INIT(&Options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
  WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&Options,WDF_REL_TIMEOUT_IN_MS(50));
  return WdfIoTargetSendIoctlSynchronously(Target,NULL,Code,Input,NULL,&Options,Bytes);
}

static NTSTATUS UsbReadTcpcRegisters(WDFIOTARGET Target,UCHAR Register,PUCHAR Data,ULONG Length) {
  SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
  WDF_MEMORY_DESCRIPTOR Input;
  ULONG_PTR Bytes=0;
  NTSTATUS Status;
  SPB_TRANSFER_LIST_INIT(&Sequence.List,2);
  Sequence.List.Transfers[0]=SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice,0,&Register,1);
  Sequence.List.Transfers[1]=SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice,0,Data,Length);
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input,&Sequence,sizeof(Sequence));
  Status=UsbTcpcIoctl(Target,IOCTL_SPB_EXECUTE_SEQUENCE,&Input,&Bytes);
  if(Status!=STATUS_SUCCESS) return Status;
  return Bytes==Length+1?STATUS_SUCCESS:STATUS_DEVICE_DATA_ERROR;
}

static NTSTATUS UsbSampleTcpc(USB_CONTROLLER *D,PULONG Value) {
  static const UCHAR HusbId[]={0x99,0x2e,0x11,0x03};
  ULONG Samples[2]={0};
  UCHAR Identity[sizeof(HusbId)];
  NTSTATUS Status,Unlock;
  *Value=0;
  // Only D0 entry or the passive worker samples the bus. D0 exit and
  // release drain that worker before closing its connection.
  if(KeGetCurrentIrql()!=PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
  if(!D->TcpcTarget) {
    Status=UsbOpenTcpc(D);
    if(Status!=STATUS_SUCCESS) return Status;
  }
  Status=UsbTcpcIoctl(D->TcpcTarget,IOCTL_SPB_LOCK_CONNECTION,NULL,NULL);
  if(Status!=STATUS_SUCCESS) {
    UsbCloseTcpc(D);
    return Status;
  }
  Status=UsbReadTcpcRegisters(D->TcpcTarget,MTK_HUSB311_TYPEC_REG_VENDOR_ID,Identity,sizeof(Identity));
  if(Status==STATUS_SUCCESS && !RtlEqualMemory(Identity,HusbId,sizeof(HusbId))) Status=STATUS_NO_SUCH_DEVICE;
  for(ULONG I=0;I<ARRAYSIZE(Samples) && Status==STATUS_SUCCESS;I++) {
    // ROLE_CONTROL through POWER_STATUS are contiguous and have no read-to-clear bits.
    UCHAR State[5],Vendor;
    Status=UsbReadTcpcRegisters(D->TcpcTarget,MTK_HUSB311_TYPEC_REG_ROLE_CONTROL,State,sizeof(State));
    if(Status!=STATUS_SUCCESS) break;
    Status=UsbReadTcpcRegisters(D->TcpcTarget,MTK_HUSB311_TYPEC_REG_VENDOR_STATUS,&Vendor,1);
    if(Status==STATUS_SUCCESS && (!(Vendor&MTK_HUSB311_TYPEC_ALERT_VBUS_80_MASK) ||
        !(State[4]&MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT)))
      Samples[I]=USB_TYPEC_POWER_SAMPLE|((ULONG)State[0]<<16)|((ULONG)State[3]<<8)|State[4];
  }
  Unlock=UsbTcpcIoctl(D->TcpcTarget,IOCTL_SPB_UNLOCK_CONNECTION,NULL,NULL);
  if(Status==STATUS_SUCCESS) Status=Unlock;
  if(Status!=STATUS_SUCCESS) {
    // Close also drops the connection lock if the unlock did not reach SpbCx.
    UsbCloseTcpc(D);
    return Status;
  }
  if(Samples[0]==Samples[1]) *Value=Samples[0];
  return STATUS_SUCCESS;
}

typedef enum { UsbSourceUnknown, UsbSourceAbsent, UsbSourceReady } USB_SOURCE_STATE;

static USB_SOURCE_STATE UsbReadTypecSourceRaw(USB_CONTROLLER *D,BOOLEAN Discover,
                                               UCHAR *Cc,PULONG SampleOut) {
  ULONG Sample;
  UCHAR CcStatus,Role;
  NTSTATUS Status;
  const ULONG Detect=MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED;
  *Cc=0;
  if(SampleOut) *SampleOut=0;
  if(!Discover && !D->TypecProbe) return UsbSourceReady;
  if(D->TcpcConnection) {
    Status=UsbSampleTcpc(D,&Sample);
  } else {
    Status=UsbAcpiInteger(D,"MTCV",&Sample);
    if(Status==STATUS_OBJECT_NAME_NOT_FOUND && !D->TypecProbe) return UsbSourceReady;
  }
  D->TypecProbe=TRUE;
  if(Status!=STATUS_SUCCESS || (Sample&0xff000000U)!=USB_TYPEC_POWER_SAMPLE ||
      (Sample&(MTK_MUSB_PLATFORM_HUSB311_TCPC_INITIALIZING|Detect))!=Detect)
    return UsbSourceUnknown;
  if(!(Sample&MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT)) return UsbSourceAbsent;
  CcStatus=(UCHAR)(Sample>>8);
  Role=(UCHAR)(Sample>>16);
  if((CcStatus&(MTK_MUSB_PLATFORM_HUSB311_CC_RESERVED_MASK |
                MTK_MUSB_PLATFORM_HUSB311_CC_LOOKING_FOR_CONNECTION)) ||
      (Role&MTK_MUSB_PLATFORM_HUSB311_ROLE_RESERVED_MASK))
    return UsbSourceUnknown;
  if(!MtkMusbPlatformHusb311SourceAttached(CcStatus,Role)) return UsbSourceAbsent;
  // The validated source presentation has exactly one Rd termination.
  *Cc=((CcStatus>>MTK_MUSB_PLATFORM_HUSB311_CC1_SHIFT)&
       MTK_MUSB_PLATFORM_HUSB311_CC_STATE_MASK)==MTK_MUSB_PLATFORM_HUSB311_SRC_RD?1:2;
  if(SampleOut) *SampleOut=Sample;
  return UsbSourceReady;
}

static USB_SOURCE_STATE UsbReadBoardRoute(USB_CONTROLLER *D,ULONG Path,BOOLEAN Discover,
                                          BOOLEAN AllowLegacy) {
  ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER In={0};
  ULONG Ready=0;
  NTSTATUS Status;
  if(!Discover && !D->RouteProbe) return UsbSourceReady;
  In.Signature=ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE;
  RtlCopyMemory(In.MethodName,"MROU",4);
  In.IntegerArgument=Path;
  Status=UsbEvaluateAcpiInteger(D,&In,sizeof(In),&Ready);
  if(Status==STATUS_OBJECT_NAME_NOT_FOUND && !D->RouteProbe)
    return AllowLegacy?UsbSourceReady:UsbSourceAbsent;
  D->RouteProbe=TRUE;
  if(Status!=STATUS_SUCCESS || Ready>1) return UsbSourceUnknown;
  return Ready?UsbSourceReady:UsbSourceAbsent;
}

static USB_SOURCE_STATE UsbReadPortSourceEx(USB_CONTROLLER *D,BOOLEAN Discover,
                                             BOOLEAN AllowSwitch,UCHAR *Cc,PULONG SampleOut) {
  ULONG Path=D->PowerPath,AlternatePath;
  USB_SOURCE_STATE Source,Alternate;
  *Cc=0;
  if(SampleOut) *SampleOut=0;

  if(Path==USB_HANDOFF_PATH_ID_LOW_HUB_POWER) {
    Source=UsbReadBoardRoute(D,Path,Discover,TRUE);
    if(Source!=UsbSourceAbsent || !AllowSwitch) return Source;
    AlternatePath=USB_HANDOFF_PATH_TYPEC_SOURCE;
  } else {
    Source=UsbReadTypecSourceRaw(D,Discover,Cc,SampleOut);
    if(Source==UsbSourceReady) Source=UsbReadBoardRoute(D,Path,Discover,TRUE);
    if(Source==UsbSourceReady || !AllowSwitch || Path!=USB_HANDOFF_PATH_TYPEC_SOURCE)
      return Source;
    AlternatePath=USB_HANDOFF_PATH_ID_LOW_HUB_POWER;
  }

  // Pogo and the hub-backed Type-C route share the same root port.
  Alternate=UsbReadBoardRoute(D,AlternatePath,TRUE,FALSE);
  if(Alternate==UsbSourceReady && AlternatePath==USB_HANDOFF_PATH_TYPEC_SOURCE) {
    Alternate=UsbReadTypecSourceRaw(D,TRUE,Cc,SampleOut);
    if(Alternate==UsbSourceReady && !*Cc) Alternate=UsbSourceUnknown;
  }
  if(Alternate==UsbSourceReady) {
    D->PowerPath=AlternatePath;
    if(AlternatePath==USB_HANDOFF_PATH_ID_LOW_HUB_POWER) {
      *Cc=0;
      if(SampleOut) *SampleOut=0;
    }
    return UsbSourceReady;
  }
  return Source==UsbSourceUnknown || Alternate==UsbSourceUnknown?UsbSourceUnknown:UsbSourceAbsent;
}

static USB_SOURCE_STATE UsbReadPortSource(USB_CONTROLLER *D,BOOLEAN Discover,UCHAR *Cc,
                                           PULONG SampleOut) {
  return UsbReadPortSourceEx(D,Discover,TRUE,Cc,SampleOut);
}

static ULONG UsbClockIdentity(USB_CONTROLLER *D) {
  ULONG Clock=0;
  return NT_SUCCESS(UsbAcpiInteger(D,"MCLK",&Clock))?Clock:0;
}
static BOOLEAN UsbReadRetainedPower(USB_CONTROLLER *D,PUCHAR Power) {
  if(!D->Registers || !D->Phy ||
      (READ_REGISTER_UCHAR(D->Registers+USB_DEVCTL_REGISTER)&USB_DEVCTL_HOST_SESSION_MASK)!=
          USB_DEVCTL_HOST_SESSION) return FALSE;
  *Power=READ_REGISTER_UCHAR(D->Registers+USB_REG_POWER);
  return !(*Power&(USB_POWER_TRANSIENT_MASK|USB_POWER_SOFTCONN)) &&
      (!(*Power&USB_POWER_SUSPENDM) || D->WaitingForVbus || D->Core.PortPowerLost ||
       (D->Core.PortStatus&MUSB_PORT_SUSPEND));
}
static BOOLEAN UsbRetainedControllerReady(USB_CONTROLLER *D) {
  UCHAR Power;
  return UsbReadRetainedPower(D,&Power);
}
static BOOLEAN UsbRetainedHostReady(USB_CONTROLLER *D) {
  return UsbRetainedControllerReady(D) && UsbHostDevctlReady(D) && UsbHostPhyReady(D);
}

static BOOLEAN UsbPhyTuningOwned(USB_CONTROLLER *D) {
  return D->PhyBytes==MTK_MUSB_PLATFORM_PHY_REG_BYTES;
}
static BOOLEAN UsbPhyHostSignalsSafe(USB_CONTROLLER *D) {
  ULONG Dtm0,Dtm1,Acr6;
  Dtm0=READ_REGISTER_ULONG((PULONG)(D->Phy+MTK_MUSB_PLATFORM_PHY_DTM0));
  Dtm1=READ_REGISTER_ULONG((PULONG)(D->Phy+MTK_MUSB_PLATFORM_PHY_DTM1));
  Acr6=READ_REGISTER_ULONG((PULONG)(D->Phy+MTK_MUSB_PLATFORM_PHY_ACR6));
  /* The stock TPHY clears forced UART and UART enable in USB mode. */
  return !(Dtm0&0x04000000U) && !(Dtm1&0x00010000U) &&
      MtkMusbPlatformHostPhyLinkReady(Acr6,Dtm0,Dtm1) &&
      !(READ_REGISTER_ULONG((PULONG)(D->Phy+USB_PHY_ACR5))&USB_PHY_CALIBRATING);
}
static BOOLEAN UsbPhyTuningSafe(USB_CONTROLLER *D) {
  return UsbRetainedControllerReady(D) && UsbPhyHostSignalsSafe(D);
}
static BOOLEAN UsbRetainedSignalsReady(USB_CONTROLLER *D) {
  return UsbPhyTuningOwned(D)?UsbPhyTuningSafe(D):
      (UsbRetainedControllerReady(D) && UsbHostPhyReady(D));
}
static BOOLEAN UsbResumeSignalsReady(USB_CONTROLLER *D) {
  return UsbRetainedSignalsReady(D) && UsbHostDevctlReady(D);
}
static BOOLEAN UsbCapturePhyTuning(USB_CONTROLLER *D,USB_PHY_TUNING *Tuning) {
  if(!UsbPhyTuningSafe(D)) return FALSE;
  for(ULONG I=0;I<ARRAYSIZE(UsbPhyTuningFields);++I)
    Tuning->Values[I]=READ_REGISTER_ULONG((PULONG)(D->Phy+UsbPhyTuningFields[I].Offset))&
        UsbPhyTuningFields[I].Mask;
  return UsbPhyTuningSafe(D);
}
static BOOLEAN UsbRestoreRetainedRegisters(USB_CONTROLLER *D,UCHAR PowerMode,
                                            const USB_PHY_TUNING *Tuning) {
  BOOLEAN Valid=FALSE;
  UCHAR Power;
  UCHAR Mask=USB_POWER_MODE_MASK;
  if(D->WaitingForVbus) Mask|=USB_POWER_SUSPENDM;
  if(!UsbLock(D)) return FALSE;
  if(!UsbResumeSignalsReady(D) || !UsbReadRetainedPower(D,&Power)) goto Done;

  /* Stock musb_restore_context writes the saved POWER byte before the
   * endpoint context. After VBUS loss, a formerly suspended device must
   * enumerate again, so release its stale SUSPENDM without signaling RESUME.
   * RESET and the hardware-owned speed bit are never replayed. */
  if((Power&Mask)!=PowerMode) {
    WRITE_REGISTER_UCHAR(D->Registers+USB_REG_POWER,
                         (UCHAR)((Power&~Mask)|PowerMode));
    if(!UsbReadRetainedPower(D,&Power) || (Power&Mask)!=PowerMode) goto Done;
  }
  if(UsbPhyTuningOwned(D)) {
    for(ULONG I=0;I<ARRAYSIZE(UsbPhyTuningFields);++I) {
      ULONG Mask=UsbPhyTuningFields[I].Mask, Value;
      PULONG Address=(PULONG)(D->Phy+UsbPhyTuningFields[I].Offset);
      if(!UsbResumeSignalsReady(D)) goto Done;
      Value=READ_REGISTER_ULONG(Address);
      if((Value&Mask)==Tuning->Values[I]) continue;
      WRITE_REGISTER_ULONG(Address,(Value&~Mask)|Tuning->Values[I]);
      if((READ_REGISTER_ULONG(Address)&Mask)!=Tuning->Values[I]) goto Done;
    }
  }
  Valid=UsbResumeSignalsReady(D) && UsbRetainedHostReady(D);
Done:
  UsbUnlock(D);
  return Valid;
}
static MTK_MUSB_PLATFORM_IO UsbPlatformIo(USB_CONTROLLER *D) {
  return (MTK_MUSB_PLATFORM_IO){
    .Context=D,.ControllerLength=USB_BYTES,
    .Read8=UsbRead8,.Read16=UsbRead16,.Read32=UsbRead32,
    .Write8=UsbWrite8,.Write16=UsbWrite16,.Write32=UsbWrite32,
    .TryLock=UsbLock,.Unlock=UsbUnlock
  };
}
static MTK_USB_PHY_STATUS
UsbColdPhyRead32(void *Context,uint32_t Offset,uint32_t *Value)
{
  USB_CONTROLLER *D=(USB_CONTROLLER *)Context;
  if(!D||!Value||!D->Phy||!UsbPhyTuningOwned(D)||(Offset&3U)||
      (uint64_t)Offset+sizeof(*Value)>D->PhyBytes) return MtkUsbPhyInvalid;
  *Value=READ_REGISTER_ULONG((PULONG)(D->Phy+Offset));
  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
UsbColdPhyWrite32(void *Context,uint32_t Offset,uint32_t Value)
{
  USB_CONTROLLER *D=(USB_CONTROLLER *)Context;
  if(!D||!D->Phy||!UsbPhyTuningOwned(D)||(Offset&3U)||
      (uint64_t)Offset+sizeof(Value)>D->PhyBytes) return MtkUsbPhyInvalid;
  WRITE_REGISTER_ULONG((PULONG)(D->Phy+Offset),Value);
  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
UsbColdPhyNowNs(void *Context,uint64_t *Value)
{
  USB_CONTROLLER *D=(USB_CONTROLLER *)Context;
  if(!D||!Value) return MtkUsbPhyInvalid;
  *Value=UsbNow(NULL)*1000ULL;
  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
UsbColdPhyDelayUs(void *Context,uint32_t Microseconds)
{
  USB_CONTROLLER *D=(USB_CONTROLLER *)Context;
  if(!D||KeGetCurrentIrql()!=PASSIVE_LEVEL||Microseconds>10000U)
    return MtkUsbPhyDelayError;
  UsbDelay(NULL,Microseconds);
  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
UsbColdPhyResolveClock(void *Context,MTK_USB_PHY_CLOCK_SLOT Slot,uint8_t *Present)
{
  USB_CONTROLLER *D=(USB_CONTROLLER *)Context;
  if(!D||!Present||(uint32_t)Slot>=(uint32_t)MtkUsbPhyClockCount||
      !D->ResumeClock||UsbClockIdentity(D)!=D->ResumeClock)
    return MtkUsbPhyNotReady;
  /* MCLK/MGON owns the fixed board clock contract. The PHY core must not
   * acquire an independent clock reference through an absent Windows provider. */
  *Present=0U;
  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
UsbColdPhyClockAction(void *Context,MTK_USB_PHY_CLOCK_SLOT Slot)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Slot);
  return MtkUsbPhyUnsupported;
}

static MTK_USB_PHY_STATUS
UsbColdPhyReadCachedCalibration(void *Context,const char *Name,
                                uint32_t Mask,uint32_t *RawValue)
{
  USB_CONTROLLER *D=(USB_CONTROLLER *)Context;
  static const CHAR Cell[]="intr_cal";
  ULONG Index;
  if(!D||!Name||!RawValue||Mask!=MTK_USB_TPHY_T70_NVMEM_MASK||
      !D->ResumeClock) return MtkUsbPhyInvalid;
  for(Index=0U;Index<sizeof(Cell);++Index)
    if(Name[Index]!=Cell[Index]) return MtkUsbPhyUnsupported;
  /* Reuse the validated ACR1 snapshot; do not access devinfo or raw NVMEM. */
  *RawValue=(D->ColdPhySnapshot.Acr1>>19)&Mask;
  return MtkUsbPhySuccess;
}

static NTSTATUS
UsbColdPhyStatusToNt(MTK_USB_PHY_STATUS Status)
{
  switch(Status) {
    case MtkUsbPhySuccess: return STATUS_SUCCESS;
    case MtkUsbPhyInvalid: return STATUS_INVALID_PARAMETER;
    case MtkUsbPhyUnsupported: return STATUS_NOT_SUPPORTED;
    case MtkUsbPhyNotReady: return STATUS_DEVICE_NOT_READY;
    case MtkUsbPhyBusy: return STATUS_DEVICE_BUSY;
    case MtkUsbPhyTimeout: return STATUS_IO_TIMEOUT;
    case MtkUsbPhyClockError: return STATUS_DEVICE_CONFIGURATION_ERROR;
    case MtkUsbPhyNvmemError: return STATUS_DATA_ERROR;
    default: return STATUS_IO_DEVICE_ERROR;
  }
}

static NTSTATUS
UsbColdPhyPrepare(USB_CONTROLLER *D)
{
  MTK_USB_PHY_IO Io={0};
  MTK_USB_PHY_STATUS Ps;
  if(!D||D->ColdPhyBound||!D->Phy||!UsbPhyTuningOwned(D)||
      !D->ResumeTuningValid||!D->ResumeClock||UsbClockIdentity(D)!=D->ResumeClock)
    return STATUS_DEVICE_NOT_READY;
  D->ColdPhySnapshot.Acr5=D->ResumeTuning.Values[0];
  D->ColdPhySnapshot.Acr1=D->ResumeTuning.Values[1];
  D->ColdPhySnapshot.Acr6=D->ResumeTuning.Values[2];
  Ps=MtkUsbPhyCoreBuildResumeProfile(&D->ColdPhySnapshot,
      &D->ColdPhyConfig,&D->ColdPhyHostTuning);
  if(Ps!=MtkUsbPhySuccess) return UsbColdPhyStatusToNt(Ps);
  Io.Context=D;
  Io.PhyBase=MTK_MUSB_PLATFORM_PHY_BASE;
  Io.PhyLength=D->PhyBytes;
  Io.Read32=UsbColdPhyRead32;
  Io.Write32=UsbColdPhyWrite32;
  Io.NowNs=UsbColdPhyNowNs;
  Io.DelayUs=UsbColdPhyDelayUs;
  Io.ResolveClock=UsbColdPhyResolveClock;
  Io.ClockPrepare=UsbColdPhyClockAction;
  Io.ClockEnable=UsbColdPhyClockAction;
  Io.ClockDisable=UsbColdPhyClockAction;
  Io.ClockUnprepare=UsbColdPhyClockAction;
  Io.ReadNvmemCell=UsbColdPhyReadCachedCalibration;
  Ps=MtkUsbPhyCoreInitialize(&D->ColdPhy,&Io,&D->ColdPhyConfig);
  if(Ps!=MtkUsbPhySuccess) {
    if(D->ColdPhy.Signature!=0U) D->ColdPhyBound=TRUE;
    return UsbColdPhyStatusToNt(Ps);
  }
  D->ColdPhyBound=TRUE;
  Ps=MtkUsbPhyCorePowerOn(&D->ColdPhy);
  if(Ps==MtkUsbPhySuccess)
    Ps=MtkUsbPhyCoreSetUsbMode(&D->ColdPhy,MtkUsbPhyModeOtg,NULL);
  if(Ps!=MtkUsbPhySuccess) return UsbColdPhyStatusToNt(Ps);
  D->ColdPhyMode=MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG;
  RtlZeroMemory(&D->ColdPhySession,sizeof(D->ColdPhySession));
  return STATUS_SUCCESS;
}

static NTSTATUS
UsbColdPhyRelease(USB_CONTROLLER *D)
{
  MTK_USB_PHY_STATUS Ps;
  if(!D||!D->ColdPhyBound) return STATUS_SUCCESS;
  if(MtkUsbPhyCoreIsPowered(&D->ColdPhy)||
      D->ColdPhy.PowerCleanupPending) {
    Ps=MtkUsbPhyCorePowerOff(&D->ColdPhy);
    if(Ps!=MtkUsbPhySuccess) return UsbColdPhyStatusToNt(Ps);
  }
  Ps=MtkUsbPhyCoreExit(&D->ColdPhy);
  if(Ps!=MtkUsbPhySuccess) return UsbColdPhyStatusToNt(Ps);
  D->ColdPhyBound=FALSE;
  RtlZeroMemory(&D->ColdPhySession,sizeof(D->ColdPhySession));
  D->ColdPhyMode=MTK_MUSB_PLATFORM_PHY_USB_MODE_UNKNOWN;
  return STATUS_SUCCESS;
}

static int
UsbColdGetTimeUs(void *Context,uint64_t *Microseconds)
{
  if(!Context||!Microseconds) return 0;
  *Microseconds=UsbNow(NULL);
  return 1;
}

static int
UsbColdFreshGuard(void *Context,uint32_t Phase,
                  MTK_MUSB_PLATFORM_COLD_EVIDENCE *Evidence)
{
  USB_CONTROLLER *D=(USB_CONTROLLER *)Context;
  MTK_MUSB_PLATFORM_EVIDENCE *P;
  BOOLEAN StopPhase;
  UCHAR Cc=0,CcStatus=0,RoleControl=0,PowerStatus=0;
  ULONG Sample=0,Clock,Path;
  if(!D||!Evidence||!D->Prepared||!D->Registers||!D->Phy||
      !D->ColdStartOwned||!D->ResumeTuningValid||!D->ColdOwnerToken||
      D->ColdSampleSequence==MAXULONG) return 0;
  StopPhase=Phase==MtkMusbPlatformColdBeforeStop||
      Phase==MtkMusbPlatformColdAfterStop;
  Clock=UsbClockIdentity(D);
  if(!Clock||Clock!=D->ResumeClock) return 0;
  Path=D->PowerPath;
  if(Path!=USB_HANDOFF_PATH_TYPEC_SOURCE&&
      Path!=USB_HANDOFF_PATH_TYPEC_ONLY&&
      Path!=USB_HANDOFF_PATH_ID_LOW_HUB_POWER&&
      Path!=USB_HANDOFF_PATH_LEGACY) return 0;
  if(!StopPhase) {
    if(UsbReadPortSourceEx(D,TRUE,FALSE,&Cc,&Sample)!=UsbSourceReady || !D->RouteProbe) return 0;
    if(Path!=USB_HANDOFF_PATH_ID_LOW_HUB_POWER) {
      if(!D->TypecProbe||Sample==0U) return 0;
      if(D->ColdRoleToken==0U) {
        D->ColdTypecCc=Cc;
        D->ColdRoleToken=((uint64_t)D->ColdGeneration<<32)|
            ((uint64_t)Path<<8)|(uint64_t)Cc|1U;
      } else if(Cc!=D->ColdTypecCc) {
        return 0;
      }
    } else if(D->ColdIdLowToken==0U) {
      D->ColdIdLowToken=((uint64_t)D->ColdGeneration<<32)|
          ((uint64_t)Path<<8)|1U;
    }
  }

  RtlZeroMemory(Evidence,sizeof(*Evidence));
  Evidence->ExclusiveOwnerToken=D->ColdOwnerToken;
  Evidence->SampleSequence=++D->ColdSampleSequence;
  Evidence->DataRoleToken=D->ColdRoleToken;
  Evidence->UsbDataRole=Path==USB_HANDOFF_PATH_ID_LOW_HUB_POWER?
      MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW:
      MTK_MUSB_PLATFORM_USB_DATA_ROLE_DFP;
  Evidence->PhyUsbMode=D->ColdPhyMode;
  P=&Evidence->Platform;
  P->ControllerBase=USB_BASE;
  P->ControllerLength=USB_BYTES;
  P->PhyBase=MTK_MUSB_PLATFORM_PHY_BASE;
  P->PhyLength=D->PhyBytes;
  P->ControllerMapped=P->PhyMapped=1U;
  P->AcpiColdFlags=MTK_MUSB_PLATFORM_ACPI_CLOCK_VALID;
  P->AcpiClockIdentity=Clock;
  if(Path==USB_HANDOFF_PATH_TYPEC_SOURCE||
      Path==USB_HANDOFF_PATH_TYPEC_ONLY)
    P->HostPowerPath=Path==USB_HANDOFF_PATH_TYPEC_ONLY?
        MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY:
        MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE;
  else if(Path==USB_HANDOFF_PATH_ID_LOW_HUB_POWER) {
    P->HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB;
    P->IdLowRoleToken=D->ColdIdLowToken;
  } else
    P->HostPowerPath=MTK_MUSB_PLATFORM_POWER_PATH_LEGACY_TYPEC;

  if(!StopPhase) {
    P->AcpiColdFlags|=MTK_MUSB_PLATFORM_ACPI_ROUTE_VALID;
    if(Path==USB_HANDOFF_PATH_ID_LOW_HUB_POWER) {
      if(!D->ColdIdLowToken) return 0;
      P->AcpiColdFlags|=MTK_MUSB_PLATFORM_ACPI_ID_LOW_ROUTE;
    } else {
      CcStatus=(UCHAR)(Sample>>8);
      RoleControl=(UCHAR)(Sample>>16);
      PowerStatus=(UCHAR)Sample;
      if(!MtkMusbPlatformHusb311SourceAttached(CcStatus,RoleControl)||
          (PowerStatus&(MTK_MUSB_PLATFORM_HUSB311_TCPC_INITIALIZING|
            MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED|
            MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT))!=
            (MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED|
             MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT)) return 0;
      P->AcpiColdFlags|=MTK_MUSB_PLATFORM_ACPI_TYPEC_SOURCE;
      P->Husb311PowerStatusReadValid=1U;
      P->Husb311PowerStatus=PowerStatus;
      P->Husb311CcStatusReadValid=1U;
      P->Husb311CcStatus=CcStatus;
      P->Husb311RoleControl=RoleControl;
    }
  }

  P->PhyState=D->ColdPhy.State==MTK_USB_TPHY_STATE_FAULTED?
      MTK_MUSB_PLATFORM_PHY_STATE_FAULTED:
      MtkUsbPhyCoreIsPowered(&D->ColdPhy)?
        MTK_MUSB_PLATFORM_PHY_POWERED:MTK_MUSB_PLATFORM_PHY_STATE_UNOBSERVED;
  P->PhyPowerCleanupPending=D->ColdPhy.PowerCleanupPending?
      MTK_MUSB_PLATFORM_PHY_POWER_CLEANUP_PENDING:0U;
  P->PhyReadMask=0x0fU;
  P->PhyAcr1=READ_REGISTER_ULONG((PULONG)(D->Phy+MTK_MUSB_PLATFORM_PHY_ACR1));
  P->PhyAcr6=READ_REGISTER_ULONG((PULONG)(D->Phy+MTK_MUSB_PLATFORM_PHY_ACR6));
  P->PhyDtm0=READ_REGISTER_ULONG((PULONG)(D->Phy+MTK_MUSB_PLATFORM_PHY_DTM0));
  P->PhyDtm1=READ_REGISTER_ULONG((PULONG)(D->Phy+MTK_MUSB_PLATFORM_PHY_DTM1));
  return 1;
}

static int
UsbColdSetPhyMode(void *Context,uint32_t Mode)
{
  USB_CONTROLLER *D=(USB_CONTROLLER *)Context;
  MTK_USB_PHY_STATUS Ps;
  if(!D||!D->ColdPhyBound||!MtkUsbPhyCoreIsPowered(&D->ColdPhy))
    return 0;
  if(Mode==MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST) {
    if(D->ColdPhySession.Owned!=0U) return 0;
    Ps=MtkUsbPhyCoreSetMusbSession(&D->ColdPhy,&D->ColdPhySession,
        MtkUsbPhyMusbAcquire);
    if(Ps==MtkUsbPhySuccess)
      Ps=MtkUsbPhyCoreSetUsbMode(&D->ColdPhy,MtkUsbPhyModeHost,
          &D->ColdPhyHostTuning);
    if(Ps==MtkUsbPhySuccess)
      Ps=MtkUsbPhyCoreSetMusbSession(&D->ColdPhy,&D->ColdPhySession,
          MtkUsbPhyMusbHost);
  } else if(Mode==MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG) {
    Ps=D->ColdPhySession.Owned?
      MtkUsbPhyCoreSetMusbSession(&D->ColdPhy,&D->ColdPhySession,
          MtkUsbPhyMusbRestore):MtkUsbPhySuccess;
    if(Ps==MtkUsbPhySuccess)
      Ps=MtkUsbPhyCoreSetUsbMode(&D->ColdPhy,MtkUsbPhyModeOtg,NULL);
  } else if(Mode==MTK_MUSB_PLATFORM_PHY_USB_MODE_INVALID) {
    if(D->ColdPhySession.Owned!=0U) return 0;
    Ps=MtkUsbPhyCoreSetMusbSession(&D->ColdPhy,&D->ColdPhySession,
        MtkUsbPhyMusbInvalid);
  } else {
    return 0;
  }
  if(Ps!=MtkUsbPhySuccess) return 0;
  D->ColdPhyMode=(UCHAR)Mode;
  return 1;
}

static MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS
UsbColdCallbacks(USB_CONTROLLER *D)
{
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS C={0};
  C.Context=D;
  C.FreshGuard=UsbColdFreshGuard;
  C.SetPhyMode=UsbColdSetPhyMode;
  C.GetTimeUs=UsbColdGetTimeUs;
  return C;
}

static void UsbUnmap(USB_CONTROLLER *D) {
  // KMDF disconnects the ISR before ReleaseHardware and disposes of the
  // interrupt created in PrepareHardware after this callback returns.
  D->Interrupt=NULL;D->InterruptReady=D->InterruptOwned=D->InterruptUnmasked=FALSE;
  UsbCloseTcpc(D);
  D->TcpcConnection=0;
  if(D->Phy) MmUnmapIoSpace(D->Phy,D->PhyBytes);
  if(D->Handoff) MmUnmapIoSpace(D->Handoff,USB_HANDOFF_BYTES);
  if(D->Registers) MmUnmapIoSpace(D->Registers,USB_BYTES);
  D->Phy=NULL;
  D->PhyBytes=0;
  D->Handoff=NULL;
  D->Registers=NULL;
}
NTSTATUS UsbPrepare(WDFDEVICE Device,WDFCMRESLIST Raw,WDFCMRESLIST Translated) {
  USB_CONTROLLER *D=UsbControllerContext(Device);ULONG I,Mem=0,PhyBytes=0;PHYSICAL_ADDRESS A;
  WDF_INTERRUPT_CONFIG InterruptConfig;
  PCM_PARTIAL_RESOURCE_DESCRIPTOR RawInterrupt=NULL,TranslatedInterrupt=NULL;
  NTSTATUS Status=STATUS_INSUFFICIENT_RESOURCES;
  ULONGLONG TcpcConnection=0;
  if(D->Prepared || WdfCmResourceListGetCount(Raw)!=WdfCmResourceListGetCount(Translated)) return STATUS_DEVICE_CONFIGURATION_ERROR;
  for(I=0;I<WdfCmResourceListGetCount(Translated);I++) {
    PCM_PARTIAL_RESOURCE_DESCRIPTOR R=WdfCmResourceListGetDescriptor(Raw,I),T=WdfCmResourceListGetDescriptor(Translated,I);
    if(!R || !T || R->Type!=T->Type) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if(T->Type==CmResourceTypeMemory) {
      static const ULONGLONG Addresses[]={USB_BASE,USB_HANDOFF,MTK_MUSB_PLATFORM_PHY_BASE};
      static const ULONG Lengths[]={USB_BYTES,USB_HANDOFF_BYTES,MTK_MUSB_PLATFORM_PHY_BYTES};
      const USHORT AccessMask=CM_RESOURCE_MEMORY_READ_ONLY|CM_RESOURCE_MEMORY_WRITE_ONLY;
      ULONGLONG Address;ULONG Bytes;
      USHORT Access=T->Flags&AccessMask;
      if(Mem>=ARRAYSIZE(Addresses)) return STATUS_DEVICE_CONFIGURATION_ERROR;
      Address=Addresses[Mem];Bytes=Lengths[Mem];
      if(Mem==2 && Access==CM_RESOURCE_MEMORY_READ_WRITE) {
        /* Only the DT PHY child aperture has OS write ownership. */
        if((R->Flags&AccessMask)!=CM_RESOURCE_MEMORY_READ_WRITE) return STATUS_DEVICE_CONFIGURATION_ERROR;
        Bytes=MTK_MUSB_PLATFORM_PHY_REG_BYTES;
      } else if(Access!=(Mem?CM_RESOURCE_MEMORY_READ_ONLY:CM_RESOURCE_MEMORY_READ_WRITE)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
      }
      if(R->u.Memory.Start.QuadPart!=Address || T->u.Memory.Start.QuadPart!=Address ||
          R->u.Memory.Length!=Bytes || T->u.Memory.Length!=Bytes) return STATUS_DEVICE_CONFIGURATION_ERROR;
      if(Mem==2) PhyBytes=Bytes;
      Mem++;
    } else if(T->Type==CmResourceTypeInterrupt) {
      if(TranslatedInterrupt || (T->Flags&(CM_RESOURCE_INTERRUPT_MESSAGE|CM_RESOURCE_INTERRUPT_LATCHED)) ||
          R->u.Interrupt.Vector!=USB_GSIV) return STATUS_DEVICE_CONFIGURATION_ERROR;
      RawInterrupt=R;
      TranslatedInterrupt=T;
    } else if(T->Type==CmResourceTypeConnection) {
      if(TcpcConnection || T->u.Connection.Class!=CM_RESOURCE_CONNECTION_CLASS_SERIAL ||
          T->u.Connection.Type!=CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C ||
          R->u.Connection.Class!=T->u.Connection.Class || R->u.Connection.Type!=T->u.Connection.Type ||
          R->u.Connection.IdLowPart!=T->u.Connection.IdLowPart || R->u.Connection.IdHighPart!=T->u.Connection.IdHighPart)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
      TcpcConnection=((ULONGLONG)T->u.Connection.IdHighPart<<32)|T->u.Connection.IdLowPart;
      if(!TcpcConnection) return STATUS_DEVICE_CONFIGURATION_ERROR;
    } else if(T->Type!=CmResourceTypeDevicePrivate)
      return STATUS_DEVICE_CONFIGURATION_ERROR;
  }
  if(Mem<2 || !TranslatedInterrupt) return STATUS_DEVICE_CONFIGURATION_ERROR;
  A.QuadPart=USB_BASE;D->Registers=MmMapIoSpaceEx(A,USB_BYTES,PAGE_READWRITE|PAGE_NOCACHE);
  if(!D->Registers) return STATUS_INSUFFICIENT_RESOURCES;
  A.QuadPart=USB_HANDOFF;D->Handoff=MmMapIoSpaceEx(A,USB_HANDOFF_BYTES,PAGE_READONLY|PAGE_NOCACHE);
  if(!D->Handoff) goto Failed;
  if(PhyBytes) {
    A.QuadPart=MTK_MUSB_PLATFORM_PHY_BASE;
    D->PhyBytes=PhyBytes;
    D->Phy=MmMapIoSpaceEx(A,PhyBytes,PAGE_NOCACHE|
        (UsbPhyTuningOwned(D)?PAGE_READWRITE:PAGE_READONLY));
    if(!D->Phy) goto Failed;
  }
  WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig,UsbPortIsr,UsbPortDpc);
  InterruptConfig.InterruptRaw=RawInterrupt;
  InterruptConfig.InterruptTranslated=TranslatedInterrupt;
  InterruptConfig.EvtInterruptDisable=UsbPortInterruptDisable;
  Status=WdfInterruptCreate(Device,&InterruptConfig,WDF_NO_OBJECT_ATTRIBUTES,&D->Interrupt);
  if(!NT_SUCCESS(Status)) goto Failed;
  D->TcpcConnection=TcpcConnection;
  D->Prepared=TRUE;return STATUS_SUCCESS;
Failed:
  UsbUnmap(D);
  return Status;
}
static NTSTATUS UsbD0Failed(USB_CONTROLLER *D,NTSTATUS Status) {
  WdfSpinLockAcquire(D->EndpointLock);
  D->Stopping=1;D->Online=0;
  if(!D->ColdStartPending&&!D->ColdStartOwned&&!D->ColdPhyBound&&!D->ColdHost.Signature)
    D->ResumeClock=0;
  D->WaitingForVbus=FALSE;D->ColdResumeRequested=FALSE;
  if(!D->ColdStartPending&&!D->ColdStartOwned&&!D->ColdPhyBound&&!D->ColdHost.Signature) {
    D->ColdStartPending=FALSE;D->ColdStartOwned=FALSE;
  }
  UsbUpdatePortInterruptLocked(D,FALSE);
  WdfSpinLockRelease(D->EndpointLock);
  UcxControllerSetFailed(D->Ucx);return Status;
}
/* Readable without a debugger or input device:
 * reg query HKLM\SYSTEM\CurrentControlSet\Services\MtkUsb\Parameters\Diag */
static void UsbDiag(PCWSTR Name,ULONG Value) {
  static const WCHAR Parameters[]=L"MtkUsb\\Parameters",Diag[]=L"MtkUsb\\Parameters\\Diag";
  if(KeGetCurrentIrql()!=PASSIVE_LEVEL) return;
  (void)RtlCreateRegistryKey(RTL_REGISTRY_SERVICES,(PWSTR)Parameters);
  (void)RtlCreateRegistryKey(RTL_REGISTRY_SERVICES,(PWSTR)Diag);
  (void)RtlWriteRegistryValue(RTL_REGISTRY_SERVICES,Diag,Name,REG_DWORD,&Value,sizeof(Value));
}
static NTSTATUS UsbFirstD0(USB_CONTROLLER *D) {
  MUSB_HOST_CONFIG C={0};MUSB_HOST_IO Io={0};
  MTK_MUSB_PLATFORM_IO Platform=UsbPlatformIo(D);
  MTK_MUSB_PLATFORM_STATUS Taken;
  USB_HANDOFF_STATE Handoff;MUSB_STATUS Core;ULONG Clock=0;
  UCHAR Devctl;
  /* The boot record says that firmware left the controller clock and PHY on.
   * Without it, a clock read through ACPI is needed before touching MMIO. */
  if(!UsbValidHandoff(D,&Handoff)) {
    UsbDiag(L"Record",READ_REGISTER_ULONG(D->Handoff));
    Clock=UsbClockIdentity(D);
    if(!Clock) {UsbDiag(L"Step",1);return STATUS_DEVICE_NOT_READY;}
    Handoff=(USB_HANDOFF_STATE){MUSB_READY_REQUIRED,USB_HANDOFF_PATH_TYPEC_SOURCE,MUSB_PORT_POWER,0};
  } else UsbDiag(L"Record",1);
  D->PowerPath=Handoff.PowerPath;
  if(D->WorkerThread && D->Stopping) return STATUS_CANCELLED;

  Taken=MtkMusbPlatformTakeOverHost(&Platform);
  Devctl=READ_REGISTER_UCHAR(D->Registers+USB_DEVCTL_REGISTER);
  UsbDiag(L"TakeOver",(ULONG)Taken);UsbDiag(L"Devctl",Devctl);
  UsbDiag(L"Power",READ_REGISTER_UCHAR(D->Registers+USB_REG_POWER));
  if(D->Phy) UsbDiag(L"PhyDtm1",READ_REGISTER_ULONG((PULONG)(D->Phy+MTK_MUSB_PLATFORM_PHY_DTM1)));
  if(Taken!=MtkMusbPlatformSuccess) {UsbDiag(L"Step",2);return STATUS_DEVICE_NOT_READY;}
  if(!UsbHostDevctlReady(D)) {
    // Keep the root hub and retry: the session can come up after D0 entry.
    if(!(Devctl&USB_DEVCTL_SESSION))
      WRITE_REGISTER_UCHAR(D->Registers+USB_DEVCTL_REGISTER,Devctl|USB_DEVCTL_SESSION);
    UsbDiag(L"Step",3);
    return STATUS_RETRY;
  }
  Io.Context=D;Io.ControllerLength=USB_BYTES;Io.Read8=UsbRead8;Io.Read16=UsbRead16;Io.Read32=UsbRead32;
  Io.Write8=UsbWrite8;Io.Write16=UsbWrite16;Io.Write32=UsbWrite32;Io.NowUs=UsbNow;Io.DelayUs=UsbDelay;
  Io.TryLock=UsbLock;Io.Unlock=UsbUnlock;Io.Cancelled=UsbCancelled;
  C.Revision=1;C.Flags=Handoff.Flags;C.ControllerLength=USB_BYTES;C.PhyLength=MTK_MUSB_PLATFORM_PHY_BYTES;
  C.InterruptVector=USB_GSIV;C.EndpointCount=16;C.PortCount=1;C.FifoProfile=MUSB_HOST_T70_FIFO_PROFILE;
  C.InterruptMask=USB_PORT_IRQ_MASK;
  Core=MusbHostInitialize(&D->Core,&Io,&C);
  UsbDiag(L"Initialize",(ULONG)Core);
  // After a wait for the session, only the live port describes the connection.
  if(D->WaitingForVbus) {
    Handoff.PortStatus=MUSB_PORT_POWER;
    Handoff.PortChange=0;
  }
  if(Core==MusbOk) Core=MusbHostAdoptPortState(&D->Core,Handoff.PortStatus,Handoff.PortChange);
  if(Core==MusbOk) Core=MusbHostSetState(&D->Core,MUSB_HOST_STATE_OPERATIONAL);
  UsbDiag(L"Core",(ULONG)Core);UsbDiag(L"Port",D->Core.PortStatus);
  if(Core!=MusbOk) {
    D->Core.Initialized=0;
    UsbDiag(L"Step",4);
    return Core==MusbNotReady?STATUS_RETRY:STATUS_DEVICE_NOT_READY;
  }
  UsbDiag(L"Step",0x100);
  return STATUS_SUCCESS;
}
static BOOLEAN UsbRestoreResumeClock(USB_CONTROLLER *D,ULONG Expected) {
  ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER In={0};
  ULONG Live;
  if(!NT_SUCCESS(UsbAcpiInteger(D,"MCLK",&Live))) return FALSE;
  if(Live) return Live==Expected;

  // MGON verifies the saved parent/selector and restores the dedicated USB
  // clock path. It can program a blank USBPLL only while isolated and gated.
  In.Signature=ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE;
  RtlCopyMemory(In.MethodName,"MGON",4);
  In.IntegerArgument=Expected;
  return NT_SUCCESS(UsbEvaluateAcpiInteger(D,&In,sizeof(In),&Live)) &&
      Live==Expected && UsbClockIdentity(D)==Expected;
}

static NTSTATUS UsbRestoreHostD0(USB_CONTROLLER *D,UCHAR PowerMode,
                                 const USB_PHY_TUNING *Tuning) {
  MTK_MUSB_PLATFORM_IO Platform=UsbPlatformIo(D);
  MTK_MUSB_PLATFORM_STATUS Fifo;
  USHORT Status,Change;
  if(!UsbResumeSignalsReady(D)) return STATUS_DEVICE_NOT_READY;

  /* PIO completion flags can remain after draining software requests.
   * Retire them only while endpoints/DMA are idle. Keep the common USB
   * connect/disconnect bits for the normal root-port polling path. */
  Fifo=MtkMusbPlatformVerifyHostFifo(&Platform);
  if(Fifo!=MtkMusbPlatformSuccess && Fifo!=MtkMusbPlatformProfileMismatch)
    return STATUS_DEVICE_NOT_READY;
  if(!UsbResumeSignalsReady(D) ||
      MtkMusbPlatformAcknowledgeIdleInterrupts(&Platform)!=MtkMusbPlatformSuccess)
    return STATUS_DEVICE_NOT_READY;
  if(!UsbRestoreRetainedRegisters(D,PowerMode,Tuning)) return STATUS_DEVICE_NOT_READY;
  if(Fifo==MtkMusbPlatformProfileMismatch &&
      (!UsbRetainedHostReady(D) ||
       MtkMusbPlatformRestoreRetainedHostFifo(&Platform)!=MtkMusbPlatformSuccess))
    return STATUS_DEVICE_NOT_READY;
  if(!UsbRetainedHostReady(D) ||
      MusbHostGetPortStatus(&D->Core,0,&Status,&Change)!=MusbOk ||
      MusbHostSetState(&D->Core,MUSB_HOST_STATE_OPERATIONAL)!=MusbOk)
    return STATUS_DEVICE_NOT_READY;
  return STATUS_SUCCESS;
}
static NTSTATUS
UsbColdPlatformStatusToNt(MTK_MUSB_PLATFORM_STATUS Status)
{
  switch(Status) {
    case MtkMusbPlatformSuccess: return STATUS_SUCCESS;
    case MtkMusbPlatformInvalid: return STATUS_INVALID_PARAMETER;
    case MtkMusbPlatformUnsupported: return STATUS_NOT_SUPPORTED;
    case MtkMusbPlatformBusy: return STATUS_DEVICE_BUSY;
    case MtkMusbPlatformTimeout: return STATUS_IO_TIMEOUT;
    case MtkMusbPlatformConflict: return STATUS_ACCESS_DENIED;
    case MtkMusbPlatformQuiescenceFailed: return STATUS_DEVICE_BUSY;
    case MtkMusbPlatformIoError: return STATUS_IO_DEVICE_ERROR;
    default: return STATUS_DEVICE_NOT_READY;
  }
}

static NTSTATUS
UsbBeginColdResume(USB_CONTROLLER *D)
{
  if(!D||!D->Prepared||!D->Core.Initialized||
      D->Core.State!=MUSB_HOST_STATE_HALTED||!D->ResumeTuningValid||
      !D->ResumeClock||!D->Phy||!UsbPhyTuningOwned(D))
    return STATUS_DEVICE_NOT_READY;
  if(D->ColdStartPending||D->ColdStartOwned||D->ColdPhyBound)
    return STATUS_DEVICE_BUSY;
  if(D->PowerPath!=USB_HANDOFF_PATH_TYPEC_SOURCE&&
      D->PowerPath!=USB_HANDOFF_PATH_TYPEC_ONLY&&
      D->PowerPath!=USB_HANDOFF_PATH_ID_LOW_HUB_POWER&&
      D->PowerPath!=USB_HANDOFF_PATH_LEGACY)
    return STATUS_NOT_SUPPORTED;
  if(!UsbRestoreResumeClock(D,D->ResumeClock))
    return STATUS_DEVICE_NOT_READY;

  ++D->ColdGeneration;
  if(!D->ColdGeneration) ++D->ColdGeneration;
  D->ColdOwnerToken=((ULONGLONG)(ULONG_PTR)D ^
      ((ULONGLONG)D->ColdGeneration<<32) ^ 0x4d555342434f4c44ULL);
  if(!D->ColdOwnerToken) D->ColdOwnerToken=1U;
  D->ColdRoleToken=0U;
  D->ColdIdLowToken=0U;
  D->ColdTypecCc=0U;
  D->ColdSampleSequence=0U;
  D->ColdPhyMode=MTK_MUSB_PLATFORM_PHY_USB_MODE_UNKNOWN;
  RtlZeroMemory(&D->ColdHost,sizeof(D->ColdHost));
  RtlZeroMemory(&D->ColdPhySession,sizeof(D->ColdPhySession));
  if(MusbHostMarkVbusLost(&D->Core)!=MusbOk)
    return STATUS_DEVICE_NOT_READY;

  WdfSpinLockAcquire(D->EndpointLock);
  D->ColdStartOwned=TRUE;
  D->ColdStartPending=TRUE;
  D->WaitingForVbus=TRUE;
  WdfSpinLockRelease(D->EndpointLock);
  D->NextVbusProbe=UsbNow(NULL);
  return STATUS_SUCCESS;
}

static NTSTATUS
UsbStopColdResume(USB_CONTROLLER *D)
{
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_STATUS Ms;
  NTSTATUS Status;
  if(!D) return STATUS_INVALID_PARAMETER;
  if(D->ColdHost.Signature) {
    if(!UsbRestoreResumeClock(D,D->ResumeClock)) return STATUS_DEVICE_NOT_READY;
    Io=UsbPlatformIo(D);
    Callbacks=UsbColdCallbacks(D);
    Ms=MtkMusbPlatformStopColdHost(&Io,&Callbacks,&D->ColdHost);
    if(Ms!=MtkMusbPlatformSuccess) return UsbColdPlatformStatusToNt(Ms);
  }
  Status=UsbColdPhyRelease(D);
  if(!NT_SUCCESS(Status)) return Status;
  RtlZeroMemory(&D->ColdHost,sizeof(D->ColdHost));
  RtlZeroMemory(&D->ColdPhySession,sizeof(D->ColdPhySession));
  D->ColdStartPending=FALSE;
  D->ColdStartOwned=FALSE;
  D->ColdResumeRequested=FALSE;
  D->ColdRoleToken=0U;
  D->ColdIdLowToken=0U;
  D->ColdOwnerToken=0U;
  D->ColdTypecCc=0U;
  D->ColdSampleSequence=0U;
  return STATUS_SUCCESS;
}

static void
UsbColdResumeFailed(USB_CONTROLLER *D,NTSTATUS Failure)
{
  NTSTATUS Cleanup;
  Cleanup=UsbStopColdResume(D);
  if(!NT_SUCCESS(Cleanup)) {
    D->ColdStartPending=TRUE;
    D->NextVbusProbe=UsbNow(NULL)+MTK_MUSB_PLATFORM_COLD_POLL_US;
    return;
  }
  UsbD0Failed(D,Failure);
}

static void
UsbAdvanceColdResume(USB_CONTROLLER *D)
{
  MTK_MUSB_PLATFORM_IO Io;
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS Callbacks;
  MTK_MUSB_PLATFORM_STATUS Ms;
  NTSTATUS Status;
  USB_PHY_TUNING Tuning;
  if(!D||!D->ColdStartPending||D->Stopping) return;

  if(!D->ColdPhyBound) {
    Status=UsbColdPhyPrepare(D);
    if(!NT_SUCCESS(Status)) {
      UsbColdResumeFailed(D,Status);
      return;
    }
  }
  if(!MtkUsbPhyCoreIsPowered(&D->ColdPhy)) {
    UsbColdResumeFailed(D,STATUS_DEVICE_NOT_READY);
    return;
  }
  Io=UsbPlatformIo(D);
  Callbacks=UsbColdCallbacks(D);
  Ms=MtkMusbPlatformAdvanceColdHost(&Io,&Callbacks,&D->ColdHost);
  if(Ms==MtkMusbPlatformPending) {
    D->NextVbusProbe=UsbNow(NULL)+MTK_MUSB_PLATFORM_COLD_POLL_US;
    return;
  }
  if(Ms!=MtkMusbPlatformSuccess) {
    UsbColdResumeFailed(D,UsbColdPlatformStatusToNt(Ms));
    return;
  }

  if((!D->Core.PortPowerLost&&MusbHostMarkVbusLost(&D->Core)!=MusbOk)||
      UsbRestoreHostD0(D,D->ResumePowerMode,&D->ResumeTuning)!=STATUS_SUCCESS) {
    UsbColdResumeFailed(D,STATUS_DEVICE_NOT_READY);
    return;
  }
  if(UsbCapturePhyTuning(D,&Tuning)) {
    D->ResumeTuning=Tuning;
    D->ResumeTuningValid=TRUE;
  }
  D->TypecCc=D->ColdTypecCc;
  WdfSpinLockAcquire(D->EndpointLock);
  D->ColdStartPending=FALSE;
  D->WaitingForVbus=FALSE;
  WdfSpinLockRelease(D->EndpointLock);
  D->NextVbusProbe=UsbNow(NULL)+USB_TYPEC_POLL_US;
}

static ULONG UsbCaptureRetainedHost(USB_CONTROLLER *D, USB_PHY_TUNING *Tuning, UCHAR *Power) {
  ULONG Clock=UsbClockIdentity(D);
  if(!Clock || (D->Stopping && D->WorkerThread) ||
      !UsbRetainedSignalsReady(D) ||
      (UsbPhyTuningOwned(D) && !UsbCapturePhyTuning(D,Tuning)) ||
      !UsbReadRetainedPower(D,Power)) return 0;
  *Power&=USB_POWER_MODE_MASK;
  return Clock;
}

static BOOLEAN UsbResetMayProceed(USB_CONTROLLER *D) {
  BOOLEAN Ready;
  WdfSpinLockAcquire(D->EndpointLock);
  Ready=!D->LeavingD0;
  WdfSpinLockRelease(D->EndpointLock);
  return Ready;
}

static BOOLEAN UsbResetRetainedHost(USB_CONTROLLER *D) {
  MTK_MUSB_PLATFORM_IO Platform=UsbPlatformIo(D);
  USHORT Status,Change;
  BOOLEAN Masked;

  if(!D->Prepared || !D->Registers || !D->InterruptReady ||
      D->WaitingForVbus || !D->Core.Initialized ||
      D->Core.State!=MUSB_HOST_STATE_OPERATIONAL || !UsbResetMayProceed(D))
    return FALSE;

  WdfSpinLockAcquire(D->EndpointLock);
  Masked=!D->LeavingD0 && WdfInterruptSynchronize(D->Interrupt,UsbMaskResetInterrupt,D);
  WdfSpinLockRelease(D->EndpointLock);
  /* Stop what the failed transfers left behind, repeat the stock controller
   * reset, then install the host FIFO layout again. Session, VBUS and PHY are
   * kept; the hub driver sees a new connection and enumerates it. */
  if(!Masked || MusbHostSetState(&D->Core,MUSB_HOST_STATE_HALTED)!=MusbOk ||
      MtkMusbPlatformTakeOverHost(&Platform)!=MtkMusbPlatformSuccess ||
      !UsbResetMayProceed(D) ||
      MtkMusbPlatformResetRetainedHost(&Platform)!=MtkMusbPlatformSuccess)
    return FALSE;

  // The stock virtual hub allows the controller to settle for 3 ms.
  UsbDelay(D,3000);
  return UsbResetMayProceed(D) &&
      MtkMusbPlatformTakeOverHost(&Platform)==MtkMusbPlatformSuccess &&
      UsbResetMayProceed(D) && UsbHostDevctlReady(D) &&
      MusbHostMarkVbusLost(&D->Core)==MusbOk &&
      MusbHostGetPortStatus(&D->Core,0,&Status,&Change)==MusbOk &&
      MusbHostSetState(&D->Core,MUSB_HOST_STATE_OPERATIONAL)==MusbOk;
}

void UsbControllerReset(UCXCONTROLLER C) {
  USB_CONTROLLER *D=UsbObject(C)->Controller;
  UCX_CONTROLLER_RESET_COMPLETE_INFO Info;
  BOOLEAN Started,Recovered=FALSE;

  WdfSpinLockAcquire(D->EndpointLock);
  Started=D->Online && !D->Stopping && !D->LeavingD0 && !UsbOnRequestThreadLocked(D);
  D->Stopping=1;
  D->Online=0;
  if(!D->ColdStartPending&&!D->ColdStartOwned&&!D->ColdPhyBound&&!D->ColdHost.Signature)
    D->ResumeClock=0;
  if(Started) {
    D->ResetThread=PsGetCurrentThread();
    KeClearEvent(&D->ResetDrained);
    UsbUpdatePortInterruptLocked(D,FALSE);
  }
  WdfSpinLockRelease(D->EndpointLock);

  if(Started && UsbQuiesce(D)==STATUS_SUCCESS && UsbResetRetainedHost(D)) {
    // Old endpoint queues remain purged until UCX re-enumerates.
    WdfSpinLockAcquire(D->EndpointLock);
    for(PLIST_ENTRY Link=D->Endpoints.Flink;Link!=&D->Endpoints;Link=Link->Flink) {
      USB_ENDPOINT *E=CONTAINING_RECORD(Link,USB_ENDPOINT,Link);
      E->Device->Address=0;
      E->Device->Enabled=FALSE;
      E->Stopped=TRUE;
      E->Toggle=0;
    }
    D->LastPortChange=0;
    WdfSpinLockRelease(D->EndpointLock);
    WdfIoQueueStart(D->Management);
    WdfIoQueueStart(D->RootInterrupt);
    WdfTimerStart(D->Timer,WDF_REL_TIMEOUT_IN_MS(1));
    Recovered=TRUE;
  }
  WdfSpinLockAcquire(D->EndpointLock);
  Recovered=Recovered && !D->LeavingD0;
  if(Recovered) {
    D->Online=1;
    D->Stopping=0;
  } else {
    D->WaitingForVbus=FALSE;
  }
  if(Started) {
    D->ResetThread=NULL;
    KeSetEvent(&D->ResetDrained,IO_NO_INCREMENT,FALSE);
  }
  WdfSpinLockRelease(D->EndpointLock);

  if(!Recovered) UcxControllerSetFailed(C);
  UCX_CONTROLLER_RESET_COMPLETE_INFO_INIT(&Info,UcxControllerStateLost,TRUE);
  UcxControllerResetComplete(C,&Info);
}

static void UsbFinishVbusWait(USB_CONTROLLER *D) {
  D->NextVbusProbe=UsbNow(NULL)+USB_TYPEC_POLL_US;
  WdfSpinLockAcquire(D->EndpointLock);
  D->WaitingForVbus=FALSE;
  if(!D->ColdStartPending&&!D->ColdStartOwned&&!D->ColdPhyBound&&!D->ColdHost.Signature)
    D->ResumeClock=0;
  WdfSpinLockRelease(D->EndpointLock);
}
static void UsbWaitForVbus(USB_CONTROLLER *D) {
  D->NextVbusProbe=UsbNow(NULL)+USB_TYPEC_POLL_US;
  WdfSpinLockAcquire(D->EndpointLock);
  D->WaitingForVbus=TRUE;
  WdfSpinLockRelease(D->EndpointLock);
}
static void UsbPollAttachedRoute(USB_CONTROLLER *D, ULONGLONG Now) {
  USB_PHY_TUNING Tuning={0};
  UCHAR Power=0,Cc=0;
  ULONG Clock,PreviousPath;
  USB_SOURCE_STATE Source;
  if(Now<D->NextVbusProbe) return;
  PreviousPath=D->PowerPath;
  Source=UsbReadPortSource(D,FALSE,&Cc,NULL);
  D->NextVbusProbe=UsbNow(NULL)+USB_TYPEC_POLL_US;
  if(D->Stopping) return;
  // Keep the live session while its sideband status is unavailable. The
  // root-port poll still sees physical MUSB disconnect events.
  if(Source==UsbSourceUnknown && UsbHostDevctlReady(D)) return;
  if(Source==UsbSourceReady) {
    if(D->PowerPath==USB_HANDOFF_PATH_ID_LOW_HUB_POWER) {
      D->TypecCc=0;
      return;
    }
    if(PreviousPath==USB_HANDOFF_PATH_ID_LOW_HUB_POWER) {
      D->TypecCc=Cc;
      return;
    }
    if(Cc==D->TypecCc) return;
  }

  // DEVCTL can retain VBUS across a board routing or active CC pin change.
  // Detach the previous port before resuming transactions.
  WdfSpinLockAcquire(D->EndpointLock);
  UsbUpdatePortInterruptLocked(D,FALSE);
  WdfSpinLockRelease(D->EndpointLock);
  Clock=UsbCaptureRetainedHost(D,&Tuning,&Power);
  if(D->Stopping) return;
  if(!Clock || MusbHostSetState(&D->Core,MUSB_HOST_STATE_HALTED)!=MusbOk ||
      MusbHostMarkVbusLost(&D->Core)!=MusbOk) {
    UsbD0Failed(D,STATUS_DEVICE_NOT_READY);
    return;
  }
  D->ResumeClock=Clock;
  D->ResumeTuning=Tuning;
  D->ResumePowerMode=Power;
  UsbWaitForVbus(D);
}

static BOOLEAN UsbRetainedSessionLost(USB_CONTROLLER *D,UCHAR *Devctl) {
  *Devctl=READ_REGISTER_UCHAR(D->Registers+USB_DEVCTL_REGISTER);
  return (*Devctl&USB_DEVCTL_HOST_SESSION_MASK)==(USB_DEVCTL_HOST_SESSION&~USB_DEVCTL_SESSION) &&
      !(READ_REGISTER_UCHAR(D->Registers+USB_REG_POWER)&(USB_POWER_TRANSIENT_MASK|USB_POWER_SOFTCONN)) &&
      UsbPhyTuningOwned(D) && UsbPhyHostSignalsSafe(D);
}

static BOOLEAN UsbSessionVbusReady(UCHAR Devctl) {
  return (Devctl&USB_DEVCTL_VBUS_VALID)==USB_DEVCTL_VBUS_VALID;
}

static NTSTATUS UsbResumeLostSession(USB_CONTROLLER *D,UCHAR Cc) {
  MTK_MUSB_PLATFORM_IO Platform=UsbPlatformIo(D);
  PUCHAR Address=D->Registers+USB_DEVCTL_REGISTER;
  UCHAR Devctl;

  // Keep the retained PHY role and board power route. Like stock host
  // resume, reassert SESSION, but leave HOSTMODE and VBUS to hardware.
  if(D->Core.State!=MUSB_HOST_STATE_HALTED ||
      (D->WorkerThread && (D->Stopping || !D->WaitingForVbus)) ||
      (D->PowerPath!=USB_HANDOFF_PATH_ID_LOW_HUB_POWER && !Cc) ||
      !UsbRetainedSessionLost(D,&Devctl)) return STATUS_DEVICE_NOT_READY;
  if(!UsbSessionVbusReady(Devctl)) return STATUS_RETRY;
  if(MtkMusbPlatformAcknowledgeIdleInterrupts(&Platform)!=MtkMusbPlatformSuccess ||
      !UsbLock(D)) return STATUS_DEVICE_NOT_READY;
  if((D->WorkerThread && D->Stopping) || !UsbRetainedSessionLost(D,&Devctl)) {
    UsbUnlock(D);
    return STATUS_DEVICE_NOT_READY;
  }
  if(!UsbSessionVbusReady(Devctl)) {
    UsbUnlock(D);
    return STATUS_RETRY;
  }
  WRITE_REGISTER_UCHAR(Address,Devctl|USB_DEVCTL_SESSION);
  UsbUnlock(D);

  // The target may have lost its address when SESSION stopped. Publish
  // removal and make the hub reset it before admitting USB transfers.
  UsbWaitForVbus(D);
  if(!(D->WorkerThread && D->Stopping) && UsbResumeSignalsReady(D) &&
      MusbHostMarkVbusLost(&D->Core)==MusbOk &&
      UsbRestoreHostD0(D,D->ResumePowerMode,&D->ResumeTuning)==STATUS_SUCCESS) {
    D->TypecCc=Cc;
    UsbFinishVbusWait(D);
    return STATUS_SUCCESS;
  }

  // The recovery path still owns the quiesced controller. Clear only the SESSION
  // bit changed by this attempt, using the current hardware status byte.
  if(UsbLock(D)) {
    Devctl=READ_REGISTER_UCHAR(Address);
    if(Devctl&USB_DEVCTL_SESSION) WRITE_REGISTER_UCHAR(Address,Devctl&~USB_DEVCTL_SESSION);
    UsbUnlock(D);
  }
  return STATUS_DEVICE_NOT_READY;
}

static void UsbPollVbus(USB_CONTROLLER *D) {
  ULONGLONG Now;
  ULONG Clock;
  UCHAR Cc=0,Devctl;
  BOOLEAN Probe=D->TypecProbe || D->RouteProbe,SessionLost=FALSE;
  NTSTATUS Status;
  if(D->Stopping || (!Probe && !D->WaitingForVbus)) return;
  Now=UsbNow(NULL);
  if(D->ColdStartPending) {
    if(Now<D->NextVbusProbe) return;
    D->NextVbusProbe=Now+MTK_MUSB_PLATFORM_COLD_POLL_US;
    UsbAdvanceColdResume(D);
    return;
  }
  if(!D->WaitingForVbus) {
    if(D->Core.Initialized) UsbPollAttachedRoute(D,Now);
    return;
  }
  if(!D->Core.Initialized) {
    if(Now<D->NextVbusProbe) return;
    D->NextVbusProbe=Now+USB_TYPEC_POLL_US;
    Status=UsbFirstD0(D);
    if(D->Stopping || Status==STATUS_RETRY) return;
    if(Status==STATUS_SUCCESS) UsbFinishVbusWait(D);
    else UsbD0Failed(D,Status);
    return;
  }
  if(!UsbHostDevctlReady(D)) {
    SessionLost=D->RouteProbe && UsbRetainedSessionLost(D,&Devctl);
    if(!SessionLost) {
      if(!UsbRetainedSignalsReady(D)) UsbD0Failed(D,STATUS_DEVICE_NOT_READY);
      return;
    }
    if(!UsbSessionVbusReady(Devctl)) return;
  }
  if(Probe) {
    if(Now<D->NextVbusProbe) return;
    D->NextVbusProbe=Now+USB_TYPEC_POLL_US;
    if(UsbReadPortSource(D,FALSE,&Cc,NULL)!=UsbSourceReady || D->Stopping) return;
  }
  Clock=UsbClockIdentity(D);
  if(D->Stopping) return;
  if(!Clock || Clock!=D->ResumeClock) {
    UsbD0Failed(D,STATUS_DEVICE_NOT_READY);
    return;
  }
  if(SessionLost) {
    Status=UsbResumeLostSession(D,Cc);
    if(!D->Stopping && Status!=STATUS_SUCCESS && Status!=STATUS_RETRY) UsbD0Failed(D,Status);
    return;
  }
  if(UsbRestoreHostD0(D,D->ResumePowerMode,&D->ResumeTuning)!=STATUS_SUCCESS) {
    UsbD0Failed(D,STATUS_DEVICE_NOT_READY);
    return;
  }
  D->TypecCc=Cc;
  UsbFinishVbusWait(D);
}

static NTSTATUS UsbResumeD0(USB_CONTROLLER *D,WDF_POWER_DEVICE_STATE Previous) {
  ULONG Clock=Previous==D->ResumeState?D->ResumeClock:0;
  NTSTATUS Status;
  UCHAR Cc,Devctl,ResumePower;
  ULONG Sample;
  MTK_MUSB_PLATFORM_IO Platform=UsbPlatformIo(D);
  BOOLEAN SessionLost;
  USB_SOURCE_STATE Source;
  D->ColdResumeRequested=FALSE;
  if(!Clock || !D->Core.Initialized || D->Core.State!=MUSB_HOST_STATE_HALTED)
    return STATUS_NOT_SUPPORTED;
  if(!UsbRestoreResumeClock(D,Clock)) return STATUS_DEVICE_NOT_READY;
  SessionLost=D->RouteProbe && UsbRetainedSessionLost(D,&Devctl);
  if(SessionLost && UsbSessionVbusReady(Devctl)) {
    if(UsbReadPortSource(D,FALSE,&Cc,NULL)!=UsbSourceReady || UsbClockIdentity(D)!=Clock)
      return STATUS_DEVICE_NOT_READY;
    Status=UsbResumeLostSession(D,Cc);
    if(Status!=STATUS_RETRY) return Status;
  }
  if(!SessionLost&&!UsbRetainedSignalsReady(D)) {
    Devctl=READ_REGISTER_UCHAR(D->Registers+USB_DEVCTL_REGISTER);
    ResumePower=READ_REGISTER_UCHAR(D->Registers+USB_REG_POWER);
    if(!(Devctl&USB_DEVCTL_BDEVICE)&&
        !(ResumePower&(USB_POWER_TRANSIENT_MASK|USB_POWER_SOFTCONN))&&
        D->ResumeTuningValid&&D->RouteProbe&&
        UsbReadPortSource(D,TRUE,&Cc,&Sample)==UsbSourceReady&&D->RouteProbe&&
        MtkMusbPlatformCheckQuiescent(&Platform,0U)==MtkMusbPlatformSuccess) {
      D->ColdResumeRequested=TRUE;
    }
    return STATUS_DEVICE_NOT_READY;
  }
  {
    ULONG PreviousPath=D->PowerPath;
    Source=UsbReadPortSource(D,FALSE,&Cc,NULL);
    BOOLEAN CcChanged=Source==UsbSourceReady &&
        D->PowerPath!=USB_HANDOFF_PATH_ID_LOW_HUB_POWER &&
        PreviousPath!=USB_HANDOFF_PATH_ID_LOW_HUB_POWER &&
        Cc!=D->TypecCc;
    if(SessionLost || !UsbHostDevctlReady(D) || Source==UsbSourceAbsent || CcChanged ||
        (Source==UsbSourceUnknown && (D->WaitingForVbus || D->Core.PortPowerLost))) {
      if((!SessionLost && !UsbRetainedSignalsReady(D)) || MusbHostMarkVbusLost(&D->Core)!=MusbOk)
        return STATUS_DEVICE_NOT_READY;
      // Keep the root hub available while the already-owned A-device waits
      // for its board-supplied VBUS. The worker is responsible for restoration.
      UsbWaitForVbus(D);
      return STATUS_SUCCESS;
    }
  }
  Status=UsbRestoreHostD0(D,D->ResumePowerMode,&D->ResumeTuning);
  if(Status==STATUS_SUCCESS) {
    if(Source==UsbSourceReady) D->TypecCc=Cc;
    UsbFinishVbusWait(D);
  }
  return Status;
}
NTSTATUS UsbD0Entry(WDFDEVICE Device,WDF_POWER_DEVICE_STATE Previous) {
  USB_CONTROLLER *D=UsbControllerContext(Device);NTSTATUS S;
  if(!D->Prepared) return UsbD0Failed(D,STATUS_DEVICE_NOT_READY);
  if(!D->SeenD0 || (D->WaitingForVbus && !D->Core.Initialized)) {
    S=UsbFirstD0(D);
    if(S==STATUS_RETRY) {
      UsbWaitForVbus(D);
      S=STATUS_SUCCESS;
    } else if(S==STATUS_SUCCESS) {
      UsbFinishVbusWait(D);
    }
  } else {
    S=UsbResumeD0(D,Previous);
    if(D->ColdResumeRequested) {
      D->ColdResumeRequested=FALSE;
      S=UsbBeginColdResume(D);
    }
  }
  if(!NT_SUCCESS(S)) return UsbD0Failed(D,S);
  WdfSpinLockAcquire(D->EndpointLock);
  D->SeenD0=TRUE;D->LeavingD0=FALSE;D->LastPortChange=0;D->Stopping=0;D->Online=1;
  WdfSpinLockRelease(D->EndpointLock);
  if(D->ColdStartPending && !UsbMaskColdStartInterrupts(D))
    return UsbD0Failed(D,STATUS_DEVICE_NOT_READY);
  UsbPowerQueues(D,TRUE);WdfIoQueueStart(D->Management);WdfIoQueueStart(D->RootInterrupt);
  WdfTimerStart(D->Timer,WDF_REL_TIMEOUT_IN_MS(1));return STATUS_SUCCESS;
}

static NTSTATUS UsbD0PostInterruptsEnabled(WDFDEVICE Device,WDF_POWER_DEVICE_STATE Previous) {
  USB_CONTROLLER *D=UsbControllerContext(Device);
  UNREFERENCED_PARAMETER(Previous);
  WdfSpinLockAcquire(D->EndpointLock);
  D->InterruptReady=TRUE;
  UsbScheduleLocked(D);
  WdfSpinLockRelease(D->EndpointLock);
  return STATUS_SUCCESS;
}

static NTSTATUS UsbStopBeforePowerDown(USB_CONTROLLER *D) {
  BOOLEAN Wait;
  WdfSpinLockAcquire(D->EndpointLock);
  if(D->ResetThread==PsGetCurrentThread() || UsbOnRequestThreadLocked(D)) {
    WdfSpinLockRelease(D->EndpointLock);
    return STATUS_DEVICE_BUSY;
  }
  D->LeavingD0=TRUE;
  D->Stopping=1;
  Wait=D->ResetThread!=NULL;
  WdfSpinLockRelease(D->EndpointLock);

  // Keep the clock, registers and TCPC target alive until reset drains.
  if(Wait)
    return KeWaitForSingleObject(&D->ResetDrained,Executive,KernelMode,FALSE,NULL);
  return STATUS_SUCCESS;
}

static NTSTATUS UsbD0PreInterruptsDisabled(WDFDEVICE Device,WDF_POWER_DEVICE_STATE Target) {
  USB_CONTROLLER *D=UsbControllerContext(Device);
  NTSTATUS S=UsbStopBeforePowerDown(D);
  UNREFERENCED_PARAMETER(Target);
  if(S!=STATUS_SUCCESS) return S;
  WdfSpinLockAcquire(D->EndpointLock);
  D->InterruptReady=FALSE;
  WdfSpinLockRelease(D->EndpointLock);
  return STATUS_SUCCESS;
}

static void UsbPowerQueues(USB_CONTROLLER *D,BOOLEAN Start) {
  ULONG I,Count;USB_ENDPOINT *E;BOOLEAN QueueStart;
  WdfSpinLockAcquire(D->EndpointLock);Count=D->EndpointCount;WdfSpinLockRelease(D->EndpointLock);
  for(I=0;I<Count;I++) {
    WdfSpinLockAcquire(D->EndpointLock);
    if(IsListEmpty(&D->Endpoints)) {WdfSpinLockRelease(D->EndpointLock);break;}
    E=CONTAINING_RECORD(RemoveHeadList(&D->Endpoints),USB_ENDPOINT,Link);
    InsertTailList(&D->Endpoints,&E->Link);WdfObjectReference(E->Handle);
    QueueStart=Start && !E->Stopped && !E->Dead &&
        UsbEndpointStartBeginLocked(E,FALSE,FALSE,TRUE);
    WdfSpinLockRelease(D->EndpointLock);
    if(Start) {if(QueueStart)(void)UsbEndpointStartFinish(E);}
    else {WdfIoQueuePurgeSynchronously(E->Incoming);WdfIoQueuePurgeSynchronously(E->Pending);}
    WdfObjectDereference(E->Handle);
  }
}
static NTSTATUS UsbQuiesce(USB_CONTROLLER *D) {
  NTSTATUS S=UsbWaitForQueueStarts(D);
  if(S!=STATUS_SUCCESS) return S;
  WdfTimerStop(D->Timer,TRUE);WdfWorkItemFlush(D->Worker);
  UsbPowerQueues(D,FALSE);
  WdfIoQueuePurgeSynchronously(D->Management);WdfIoQueuePurgeSynchronously(D->RootInterrupt);
  KeFlushQueuedDpcs();
  return STATUS_SUCCESS;
}
NTSTATUS UsbD0Exit(WDFDEVICE Device,WDF_POWER_DEVICE_STATE Target) {
  USB_CONTROLLER *D=UsbControllerContext(Device);NTSTATUS S;ULONG Clock=0;
  UCHAR Power=0;
  USB_PHY_TUNING Tuning={0};
  BOOLEAN PowerLost=FALSE;
  BOOLEAN CanResume=Target==WdfPowerDeviceD1 || Target==WdfPowerDeviceD2 || Target==WdfPowerDeviceD3;
  S=UsbStopBeforePowerDown(D);if(S!=STATUS_SUCCESS)return S;
  S=UsbQuiesce(D);if(S!=STATUS_SUCCESS)return S;
  if(D->ColdStartPending||D->ColdStartOwned) {
    if(D->Core.Initialized&&D->Core.State==MUSB_HOST_STATE_OPERATIONAL&&
        MusbHostSetState(&D->Core,MUSB_HOST_STATE_HALTED)!=MusbOk) {
      UsbCloseTcpc(D);
      return STATUS_IO_DEVICE_ERROR;
    }
    S=UsbStopColdResume(D);
    UsbCloseTcpc(D);
    if(!NT_SUCCESS(S)) return S;
    WdfSpinLockAcquire(D->EndpointLock);
    D->Online=0;
    D->WaitingForVbus=FALSE;
    D->ResumeState=Target;
    WdfSpinLockRelease(D->EndpointLock);
    return STATUS_SUCCESS;
  }
  UsbCloseTcpc(D);
  if(D->WaitingForVbus && CanResume && D->Online) {
    WdfSpinLockAcquire(D->EndpointLock);
    D->ResumeState=Target;D->Online=0;
    WdfSpinLockRelease(D->EndpointLock);
    return STATUS_SUCCESS;
  }
  if(D->Core.Initialized && D->Core.State==MUSB_HOST_STATE_OPERATIONAL) {
    MUSB_STATUS CoreStatus=MusbHostSetState(&D->Core,MUSB_HOST_STATE_HALTED);
    if(CoreStatus!=MusbOk) {
      WdfSpinLockAcquire(D->EndpointLock);D->Online=0;D->ResumeClock=0;WdfSpinLockRelease(D->EndpointLock);
      return STATUS_IO_DEVICE_ERROR;
    }
    if(D->Online && D->Phy && CanResume) {
      Clock=UsbCaptureRetainedHost(D,&Tuning,&Power);
      if(Clock && !UsbHostDevctlReady(D)) {
        PowerLost=MusbHostMarkVbusLost(&D->Core)==MusbOk;
        if(!PowerLost) Clock=0;
      } else if(Clock) {
        // VBUS may have returned after the port observed its loss during D0.
        PowerLost=D->Core.PortPowerLost;
      }
    }
  }
  WdfSpinLockAcquire(D->EndpointLock);
  D->ResumeClock=D->Online?Clock:0;D->ResumeState=Target;D->ResumeTuning=Tuning;
  D->ResumePowerMode=Power;D->WaitingForVbus=PowerLost;D->Online=0;
  D->ResumeTuningValid=(BOOLEAN)(Clock!=0U);
  WdfSpinLockRelease(D->EndpointLock);
  return STATUS_SUCCESS;
}
NTSTATUS UsbRelease(WDFDEVICE Device,WDFCMRESLIST Resources) {
  USB_CONTROLLER *D=UsbControllerContext(Device);NTSTATUS S;UNREFERENCED_PARAMETER(Resources);
  S=UsbStopBeforePowerDown(D);if(S!=STATUS_SUCCESS)return S;
  S=UsbQuiesce(D);if(S!=STATUS_SUCCESS)return S;
  if(D->ColdStartPending||D->ColdStartOwned||D->ColdPhyBound) {
    if(D->Core.Initialized&&D->Core.State==MUSB_HOST_STATE_OPERATIONAL&&
        MusbHostSetState(&D->Core,MUSB_HOST_STATE_HALTED)!=MusbOk)
      return STATUS_IO_DEVICE_ERROR;
    S=UsbStopColdResume(D);if(!NT_SUCCESS(S))return S;
  }
  WdfSpinLockAcquire(D->EndpointLock);D->Online=0;WdfSpinLockRelease(D->EndpointLock);
  UsbUnmap(D);
  D->ResumeClock=0;D->Prepared=FALSE;D->WaitingForVbus=FALSE;
  D->ResumeTuningValid=FALSE;
  D->SeenD0=FALSE;D->TypecProbe=FALSE;D->RouteProbe=FALSE;D->TypecCc=0;
  RtlZeroMemory(&D->Core,sizeof(D->Core));return STATUS_SUCCESS;
}
void UsbCleanup(WDFOBJECT Object) {
  USB_CONTROLLER *D=UsbControllerContext(Object);
  if(D->EndpointLock) {
    WdfSpinLockAcquire(D->EndpointLock);D->Stopping=1;D->Online=0;WdfSpinLockRelease(D->EndpointLock);
  } else {D->Stopping=1;D->Online=0;}
  KeFlushQueuedDpcs();
}
NTSTATUS UsbAdd(WDFDRIVER Driver,PWDFDEVICE_INIT Init) {
  WDF_PNPPOWER_EVENT_CALLBACKS P;WDF_OBJECT_ATTRIBUTES A;WDFDEVICE Device;USB_CONTROLLER *D;NTSTATUS S;
  UCX_CONTROLLER_CONFIG C;UCX_ROOTHUB_CONFIG H;WDF_IO_QUEUE_CONFIG Q;WDF_WORKITEM_CONFIG W;WDF_TIMER_CONFIG T;
  UNREFERENCED_PARAMETER(Driver);
  S=UcxInitializeDeviceInit(Init);if(!NT_SUCCESS(S)) return S;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&P);P.EvtDevicePrepareHardware=UsbPrepare;P.EvtDeviceReleaseHardware=UsbRelease;
  P.EvtDeviceD0Entry=UsbD0Entry;P.EvtDeviceD0Exit=UsbD0Exit;
  P.EvtDeviceD0EntryPostInterruptsEnabled=UsbD0PostInterruptsEnabled;
  P.EvtDeviceD0ExitPreInterruptsDisabled=UsbD0PreInterruptsDisabled;
  WdfDeviceInitSetPnpPowerEventCallbacks(Init,&P);
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,USB_CONTROLLER);
  A.SynchronizationScope=WdfSynchronizationScopeNone;
  A.ExecutionLevel=WdfExecutionLevelPassive;
  A.EvtCleanupCallback=UsbCleanup;
  S=WdfDeviceCreate(&Init,&A,&Device);if(!NT_SUCCESS(S)) return S;
  D=UsbControllerContext(Device);D->Device=Device;D->Stopping=1;
  InitializeListHead(&D->Endpoints);InitializeListHead(&D->QueueStartingEndpoints);
  InitializeListHead(&D->Completions);KeInitializeSpinLock(&D->CompletionLock);
  KeInitializeEvent(&D->QueueStartsDrained,NotificationEvent,TRUE);
  KeInitializeEvent(&D->ResetDrained,NotificationEvent,TRUE);
  KeInitializeDpc(&D->CompletionDpc,UsbCompletionDpc,D);
  WDF_OBJECT_ATTRIBUTES_INIT(&A);A.ParentObject=Device;
  S=WdfSpinLockCreate(&A,&D->EndpointLock);if(!NT_SUCCESS(S)) return S;
  WDF_WORKITEM_CONFIG_INIT(&W,UsbWork);W.AutomaticSerialization=FALSE;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,USB_OBJECT);A.ParentObject=Device;
  S=WdfWorkItemCreate(&W,&A,&D->Worker);if(!NT_SUCCESS(S)) return S;UsbObject(D->Worker)->Controller=D;
  WDF_TIMER_CONFIG_INIT_PERIODIC(&T,UsbTimer,1);T.AutomaticSerialization=FALSE;
  /* Without it the 1 ms period rounds to the 15.6 ms clock tick. */
  T.UseHighResolutionTimer=WdfTrue;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,USB_OBJECT);A.ParentObject=Device;
  A.ExecutionLevel=WdfExecutionLevelDispatch;
  S=WdfTimerCreate(&T,&A,&D->Timer);if(!NT_SUCCESS(S)) return S;UsbObject(D->Timer)->Controller=D;
  S=UsbManualQueue(D,Device,&D->Management);if(!NT_SUCCESS(S)) return S;
  S=UsbManualQueue(D,Device,&D->RootInterrupt);if(!NT_SUCCESS(S)) return S;
  UCX_CONTROLLER_CONFIG_INIT(&C,"MediaTek MT6789 USB2 PIO");
  C.EvtControllerQueryUsbCapability=UsbCapability;C.EvtControllerGetCurrentFrameNumber=UsbFrame;
  C.EvtControllerUsbDeviceAdd=UsbDeviceAdd;C.EvtControllerReset=UsbControllerReset;UCX_CONTROLLER_CONFIG_SET_ACPI_INFO(&C,"HANA","678D","0001");
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,USB_OBJECT);
  S=UcxControllerCreate(Device,&C,&A,&D->Ucx);if(!NT_SUCCESS(S)) return S;UsbObject(D->Ucx)->Controller=D;
  // WinPE UCX routes standard hub requests through their dedicated callbacks.
  UCX_ROOTHUB_CONFIG_INIT(&H,UsbRootControl,UsbRootControl,UsbRootControl,
      UsbRootControl,UsbRootControl,UsbRootControl,UsbRootControl,
      UsbRootInterrupt,UsbRootInfo,UsbRoot20Info,UsbRoot30Info);
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,USB_OBJECT);
  S=UcxRootHubCreate(D->Ucx,&H,&A,&D->RootHub);if(!NT_SUCCESS(S)) return S;UsbObject(D->RootHub)->Controller=D;
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&Q,WdfIoQueueDispatchParallel);Q.PowerManaged=WdfFalse;Q.EvtIoDeviceControl=UsbIoControl;
  S=WdfIoQueueCreate(Device,&Q,WDF_NO_OBJECT_ATTRIBUTES,WDF_NO_HANDLE);if(!NT_SUCCESS(S)) return S;
  return WdfDeviceCreateDeviceInterface(Device,&GUID_DEVINTERFACE_USB_HOST_CONTROLLER,NULL);
}
NTSTATUS DriverEntry(PDRIVER_OBJECT Driver,PUNICODE_STRING Path) {
  WDF_DRIVER_CONFIG C;WDF_DRIVER_CONFIG_INIT(&C,UsbAdd);return WdfDriverCreate(Driver,Path,WDF_NO_OBJECT_ATTRIBUTES,&C,WDF_NO_HANDLE);
}
