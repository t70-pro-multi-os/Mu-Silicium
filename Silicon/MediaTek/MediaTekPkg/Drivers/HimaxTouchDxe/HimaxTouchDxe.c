// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/I2cMaster.h>
#include <Protocol/MtkI2cExclusive.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/DevicePath.h>
#include <Protocol/HimaxTouch.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/HimaxCore.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/TimerLib.h>
#include <Guid/EventGroup.h>
#define SIGN SIGNATURE_32('H','X','3','2')
typedef struct {
  UINT32 Signature;
  EFI_ABSOLUTE_POINTER_PROTOCOL Pointer;
  EFI_ABSOLUTE_POINTER_MODE Mode;
  EFI_I2C_MASTER_PROTOCOL *Master;
  MTK_I2C_EXCLUSIVE_PROTOCOL *I2c;
  MTK_GPIO_PROTOCOL *Gpio;
  HM_IO Io;
  HIMAX_TOUCH_BOARD *Board;
  HM_POINTER Position, PendingRelease;
  BOOLEAN DeliveredActive, ReleasePending;
  EFI_EVENT Poll, Exit;
  UINT64 PinLeaseToken;
  BOOLEAN Changed, Stopped, Busy, Exiting, PinLeaseHeld, I2cOpen, PointerInstalled;
  UINT8 Failures, Backoff, ResumePhase, ResumeAttempts;
  BOOLEAN ResumeRunning, ResumeFinished, ResumeCancelPending, ResumeHasFrame;
  EFI_STATUS ResumeStatus;
  HM_FRAME ResumeFrame;
} TOUCH;
#define CTX(P) CR(P,TOUCH,Pointer,SIGN)
static TOUCH *mFailedStart;
static TOUCH *mActiveTouch;
static HIMAX_TOUCH_CONTROL mControl;
static EFI_HANDLE mControlOwner;
static UINT64 mControlToken, mNextControlToken;
static BOOLEAN mControlBusy, mQuiesced;
static EFI_HANDLE mFailedStartHandle;
static EFI_HANDLE mFailedStartAgent;
STATIC EFI_STATUS CloseTouchEvents(TOUCH *T);

STATIC EFI_STATUS ReleasePinLease(TOUCH *T) {
  EFI_STATUS Status;
  if (!T || !T->PinLeaseHeld) return EFI_SUCCESS;
  if (!T->Gpio || !T->Gpio->ReleasePins) return EFI_UNSUPPORTED;
  Status=T->Gpio->ReleasePins(T->PinLeaseToken);
  if (EFI_ERROR(Status)) return Status;
  T->PinLeaseHeld=FALSE;
  T->PinLeaseToken=0;
  return EFI_SUCCESS;
}

STATIC BOOLEAN PinLeaseIsValid(TOUCH *T) {
  return T && !T->Exiting && T->Board && T->Gpio && T->PinLeaseHeld &&
    T->PinLeaseToken && T->Gpio->ValidateLease &&
    T->Gpio->ValidateLease(T->PinLeaseToken,152)==EFI_SUCCESS &&
    T->Board->ValidateGpioLease &&
    T->Board->ValidateGpioLease(T->Board->Context,T->Gpio,T->PinLeaseToken) && !T->Exiting;
}

STATIC EFI_STATUS AcquirePinLease(TOUCH *T) {
  EFI_STATUS Status;
  UINT32 Pin=152;
  UINT64 Token=0;
  if (!T || T->Exiting || !T->Gpio || !T->Gpio->AcquirePins ||
      !T->Gpio->ValidateLease || !T->Gpio->ReleasePins ||
      !T->Board || !T->Board->ValidateGpioLease) return EFI_UNSUPPORTED;
  if (T->PinLeaseHeld) return EFI_ALREADY_STARTED;
  Status=T->Gpio->AcquirePins(&Pin,1,&Token);
  if (EFI_ERROR(Status)) return Status;
  /* Keep even a malformed zero token recorded so failure cleanup cannot
   * silently forget an acquired reservation. */
  T->PinLeaseToken=Token;
  T->PinLeaseHeld=TRUE;
  if (!Token || !PinLeaseIsValid(T)) return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS CleanupFailedStart(VOID) {
  TOUCH *T=mFailedStart;
  EFI_STATUS Status;
  if (!T) return EFI_SUCCESS;
  if (T->Busy) return EFI_NOT_READY;
  Status=CloseTouchEvents(T);
  if (EFI_ERROR(Status)) return Status;
  if (T->PinLeaseHeld) {
    Status=ReleasePinLease(T);
    if (EFI_ERROR(Status)) return Status;
  }
  if (T->PointerInstalled) {
    Status=gBS->UninstallMultipleProtocolInterfaces(mFailedStartHandle,
      &gEfiAbsolutePointerProtocolGuid,&T->Pointer,NULL);
    if (EFI_ERROR(Status)) {
      EFI_ABSOLUTE_POINTER_PROTOCOL *Existing=NULL;
      EFI_STATUS Lookup=gBS->HandleProtocol(mFailedStartHandle,
        &gEfiAbsolutePointerProtocolGuid,(VOID **)&Existing);
      if (Lookup!=EFI_NOT_FOUND && (EFI_ERROR(Lookup) || Existing==&T->Pointer)) return Status;
    }
    T->PointerInstalled=FALSE;
  }
  if (T->I2cOpen) {
    Status=gBS->CloseProtocol(mFailedStartHandle,&gEfiI2cMasterProtocolGuid,
      mFailedStartAgent,mFailedStartHandle);
    if (EFI_ERROR(Status)) return Status;
    T->I2cOpen=FALSE;
  }
  if (mActiveTouch==T) mActiveTouch=NULL;
  FreePool(T);
  mFailedStart=NULL;
  mFailedStartHandle=NULL;
  mFailedStartAgent=NULL;
  return EFI_SUCCESS;
}

STATIC HM_STATUS HimaxRequest(TOUCH *T,const unsigned char *WriteData,
                             unsigned WriteLength,unsigned char *ReadData,
                             unsigned ReadLength,BOOLEAN Timed) {
  struct { UINTN OperationCount; EFI_I2C_OPERATION Operation[2]; } Packet;
  EFI_STATUS Status;
  if (!T || T->Exiting || !T->Master || !T->Board || !WriteData || !WriteLength ||
      (ReadLength && !ReadData) || WriteLength>HM_BUS_WRITE_MAX_BODY+1 ||
      ReadLength>HM_BUS_READ_MAX_BYTES) return HmInvalid;
  if (!PinLeaseIsValid(T)) return HmIoError;
  ZeroMem(&Packet,sizeof(Packet));
  Packet.OperationCount=ReadLength?2:1;
  Packet.Operation[0].Flags=0;
  Packet.Operation[0].LengthInBytes=WriteLength;
  Packet.Operation[0].Buffer=(UINT8 *)WriteData;
  if (ReadLength) {
    Packet.Operation[1].Flags=I2C_FLAG_READ;
    Packet.Operation[1].LengthInBytes=ReadLength;
    Packet.Operation[1].Buffer=ReadData;
  }
  if (Timed) {
    UINT64 Token=0;
    Status=T->I2c->Acquire(T->I2c,&Token);
    if (EFI_ERROR(Status) || T->Exiting) return HmIoError;
    Status=T->I2c->TransferTimed(T->I2c,Token,T->Board->Address,
      (EFI_I2C_REQUEST_PACKET *)&Packet,5000);
    if (T->Exiting) return HmIoError;
    if (EFI_ERROR(T->I2c->Release(T->I2c,Token))) return HmIoError;
  } else {
    Status=T->Master->StartRequest(T->Master,T->Board->Address,
      (EFI_I2C_REQUEST_PACKET *)&Packet,NULL,NULL);
  }
  if (T->Exiting || !PinLeaseIsValid(T)) return HmIoError;
  return EFI_ERROR(Status)?HmIoError:HmOk;
}

STATIC HM_STATUS HimaxTransfer(VOID *Context,const unsigned char *WriteData,
                                unsigned WriteLength,unsigned char *ReadData,
                                unsigned ReadLength) {
  return HimaxRequest(Context,WriteData,WriteLength,ReadData,ReadLength,FALSE);
}

STATIC VOID HimaxDelay(VOID *Context,unsigned MinimumUs,unsigned MaximumUs) {
  TOUCH *T=(TOUCH *)Context;
  (VOID)MaximumUs;
  if (!T || !T->Exiting) MicroSecondDelay(MinimumUs);
}

STATIC VOID Publish(TOUCH *T,const HM_FRAME *Frame) {
  EFI_TPL Old=gBS->RaiseTPL(TPL_NOTIFY);
  if (!T->Exiting && (Frame ? HmPointerUpdate(&T->Position,Frame)
                            : HmPointerRelease(&T->Position))) {
    // A delivered press must be released before the next contact is exposed.
    if (!T->Position.Active && T->DeliveredActive && !T->ReleasePending) {
      T->PendingRelease=T->Position;
      T->ReleasePending=TRUE;
    }
    T->Changed=TRUE;
  }
  gBS->RestoreTPL(Old);
}
STATIC EFI_STATUS EFIAPI Reset(EFI_ABSOLUTE_POINTER_PROTOCOL *This,BOOLEAN Extended) {
  TOUCH *T; EFI_TPL Old;
  if (!This) return EFI_INVALID_PARAMETER;
  if (Extended) return EFI_UNSUPPORTED;
  T=CTX(This);
  if (T->Exiting) return EFI_DEVICE_ERROR;
  Old=gBS->RaiseTPL(TPL_NOTIFY);
  if (T->Stopped || T->Exiting) { gBS->RestoreTPL(Old); return EFI_DEVICE_ERROR; }
  HmPointerInit(&T->Position); T->Changed=FALSE;
  T->DeliveredActive=FALSE; T->ReleasePending=FALSE;
  gBS->RestoreTPL(Old); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI GetState(EFI_ABSOLUTE_POINTER_PROTOCOL *This,EFI_ABSOLUTE_POINTER_STATE *State) {
  TOUCH *T; EFI_TPL Old; HM_POINTER *Position;
  if (!This || !State) return EFI_INVALID_PARAMETER;
  T=CTX(This);
  if (T->Exiting) return EFI_DEVICE_ERROR;
  Old=gBS->RaiseTPL(TPL_NOTIFY);
  if (!T->Changed) { EFI_STATUS S=T->Stopped?EFI_DEVICE_ERROR:EFI_NOT_READY; gBS->RestoreTPL(Old); return S; }
  Position=T->ReleasePending?&T->PendingRelease:&T->Position;
  State->CurrentX=Position->X; State->CurrentY=Position->Y; State->CurrentZ=Position->Pressure;
  State->ActiveButtons=Position->Active?EFI_ABSP_TouchActive:0;
  T->DeliveredActive=Position->Active!=0;
  T->Changed=T->ReleasePending && T->Position.Active;
  T->ReleasePending=FALSE;
  gBS->RestoreTPL(Old); return EFI_SUCCESS;
}
STATIC VOID EFIAPI Wait(EFI_EVENT Event,VOID *Context) {
  TOUCH *T=Context;
  // Signal only from EVT_NOTIFY_WAIT so a direct GetState/Reset cannot leave
  // a previously posted signal behind after consuming the change.
  if (!T->Exiting && T->Changed) gBS->SignalEvent(Event);
}
STATIC HM_STATUS ReadReport(TOUCH *T,HM_FRAME *Frame) {
  CONST UINT8 Command=HM_EVENT_COMMAND;
  UINT8 Data[HM_T70_REPORT_BYTES];
  HM_STATUS Status=HimaxRequest(T,&Command,1,Data,sizeof(Data),TRUE);
  if (Status!=HmOk) return Status;
  return HmDecode(Data,sizeof(Data),T->Board->Slots,T->Board->XMax,T->Board->YMax,Frame);
}
enum { TouchResumeReadId=4, TouchResumeReadState=7, TouchResumeReport, TouchResumeDone };

STATIC HM_STATUS ResumeStep(TOUCH *T,BOOLEAN Pending) {
  STATIC CONST UINT8 Commands[][5]={
    {0x13,0x31}, {0x0d,0x10},
    {0x00,(UINT8)HM_CHIP_ID_REGISTER,(UINT8)(HM_CHIP_ID_REGISTER>>8),
     (UINT8)(HM_CHIP_ID_REGISTER>>16),(UINT8)(HM_CHIP_ID_REGISTER>>24)},
    {0x0c,0}, {0x08},
    {0x00,0xa8,0,0,0x90}, {0x0c,0}, {0x08}
  };
  STATIC CONST UINT8 Lengths[]={2,2,5,2,1,5,2,1};
  UINT8 Data[4]; HM_STATUS Status;
  if (T->ResumePhase==TouchResumeReport) {
    Status=Pending?ReadReport(T,&T->ResumeFrame):HmOk;
    T->ResumeHasFrame=Pending && Status==HmOk;
    return Status;
  }
  if (Lengths[T->ResumePhase]!=1)
    return HimaxRequest(T,Commands[T->ResumePhase],Lengths[T->ResumePhase],NULL,0,TRUE);
  Status=HimaxRequest(T,Commands[T->ResumePhase],1,Data,sizeof(Data),TRUE);
  if (Status!=HmOk) return Status;
  if (T->ResumePhase==TouchResumeReadId) return HmChipIdMatches(Data)?HmOk:HmInvalid;
  return Data[0]==0x05?HmOk:HmStateError;
}
STATIC EFI_STATUS CancelResumePoll(TOUCH *T) {
  EFI_STATUS Status;
  if (!T || !T->Poll) return EFI_NOT_READY;
  Status=gBS->SetTimer(T->Poll,TimerCancel,0);
  T->ResumeCancelPending=EFI_ERROR(Status);
  return Status;
}

STATIC VOID FinishResume(TOUCH *T,EFI_STATUS Status) {
  T->ResumeStatus=Status;
  T->ResumeRunning=FALSE;
  T->ResumeFinished=FALSE;
  if (!EFI_ERROR(CancelResumePoll(T))) T->ResumeFinished=TRUE;
}

STATIC VOID EFIAPI Poll(EFI_EVENT Event,VOID *Context) {
  TOUCH *T=Context; HM_STATUS Status; HM_FRAME Frame;
  EFI_STATUS LeaseStatus;
  BOOLEAN Pending=FALSE;
  (VOID)Event;
  if (T->Exiting || T->Busy) return;
  if (T->ResumeCancelPending) {
    if (!EFI_ERROR(CancelResumePoll(T))) T->ResumeFinished=TRUE;
    return;
  }
  if (T->Stopped && !T->ResumeRunning) return;
  if (T->Backoff) { T->Backoff--; return; }

  // Never start a new transaction while a previous lease release is pending.
  if (T->PinLeaseHeld) {
    T->Busy=TRUE;
    LeaseStatus=ReleasePinLease(T);
    T->Busy=FALSE;
    if (T->Exiting || !EFI_ERROR(LeaseStatus)) return;
    Status=HmIoError;
  } else {
    T->Busy=TRUE;
    if (!T->Board->Ready(T->Board->Context)) {
      T->Busy=FALSE;
      if (T->Exiting) return;
      if (T->ResumeRunning) {
        FinishResume(T,EFI_NOT_READY);
      } else {
        T->Board->State(T->Board->Context,HIMAX_TOUCH_REPORT_INVALID);
        if (!T->Exiting) Publish(T,NULL);
      }
      return;
    }
    if (!T->ResumeRunning || T->ResumePhase==TouchResumeReport)
      Pending=T->Board->Pending(T->Board->Context);
    if (T->Exiting || (!T->ResumeRunning && !Pending)) {
      T->Busy=FALSE;
      return;
    }
    LeaseStatus=AcquirePinLease(T);
    Status=HmIoError;
    if (!T->Exiting && !EFI_ERROR(LeaseStatus)) {
      Status=T->ResumeRunning?ResumeStep(T,Pending):ReadReport(T,&Frame);
      if (Status==HmIoError) T->Backoff=1; // next attempt after two 10 ms ticks
    }
    if (!T->Exiting) {
      EFI_STATUS ReleaseStatus=ReleasePinLease(T);
      if (EFI_ERROR(ReleaseStatus)) { LeaseStatus=ReleaseStatus; Status=HmIoError; }
    }
    T->Busy=FALSE;
  }
  if (T->Exiting) return;
  if (T->ResumeRunning) {
    if (Status==HmOk && ++T->ResumePhase<TouchResumeDone) return;
    if (Status!=HmOk && ++T->ResumeAttempts<5) {
      T->ResumePhase=0;
      T->Backoff=1;
      return;
    }
    FinishResume(T,Status==HmOk?EFI_SUCCESS:(EFI_ERROR(LeaseStatus)?LeaseStatus:EFI_DEVICE_ERROR));
    return;
  }
  if (Status==HmOk) {
    T->Board->State(T->Board->Context,HIMAX_TOUCH_REPORT_VALID);
    T->Failures=0;
    if (!T->Exiting) Publish(T,&Frame);
  } else {
    T->Board->State(T->Board->Context,HIMAX_TOUCH_REPORT_INVALID);
    if (++T->Failures>=3) {
      T->Failures=3;
      if (!T->Exiting) Publish(T,NULL);
    }
  }
}
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *Context) {
  TOUCH *T=Context; (VOID)Event;
  T->Exiting=TRUE;
  T->Stopped=TRUE; T->Changed=FALSE;
  T->DeliveredActive=FALSE; T->ReleasePending=FALSE;
  if (T->Poll!=NULL) (VOID)gBS->SetTimer(T->Poll,TimerCancel,0);
}

STATIC EFI_STATUS CloseTouchEvents(TOUCH *T) {
  EFI_STATUS S;
  T->Stopped=TRUE; T->Changed=FALSE;
  T->DeliveredActive=FALSE; T->ReleasePending=FALSE;
  if (T->Poll!=NULL) {
    S=gBS->SetTimer(T->Poll,TimerCancel,0);
    if (EFI_ERROR(S)) return S;
    S=gBS->CloseEvent(T->Poll);
    if (EFI_ERROR(S)) return S;
    T->Poll=NULL;
  }
  if (T->Exit!=NULL) {
    S=gBS->CloseEvent(T->Exit);
    if (EFI_ERROR(S)) return S;
    T->Exit=NULL;
  }
  if (T->Pointer.WaitForInput!=NULL) {
    S=gBS->CloseEvent(T->Pointer.WaitForInput);
    if (EFI_ERROR(S)) return S;
    T->Pointer.WaitForInput=NULL;
  }
  return EFI_SUCCESS;
}

/* Keep callbacks at CALLBACK while stopped ownership changes. Timer polling
 * cannot interleave; NOTIFY ExitBootServices can still interrupt and is checked
 * after every fallible service/I2C boundary. Reentry never starts more I/O. */
STATIC EFI_STATUS ControlEnter(HIMAX_TOUCH_CONTROL *This,EFI_TPL *Old,TOUCH **Touch) {
  EFI_TPL Current;
  if (This!=&mControl || This->Revision!=HIMAX_TOUCH_CONTROL_REVISION) return EFI_INVALID_PARAMETER;
  if (!gBS || !gBS->RaiseTPL || !gBS->RestoreTPL) return EFI_ABORTED;
  Current=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Current);
  if (Current>TPL_CALLBACK) return EFI_NOT_READY;
  *Old=gBS->RaiseTPL(TPL_CALLBACK);
  if (mControlBusy) { gBS->RestoreTPL(*Old); return EFI_NOT_READY; }
  if (!mActiveTouch || !mActiveTouch->PointerInstalled || !mActiveTouch->I2cOpen ||
      !mActiveTouch->Poll) { gBS->RestoreTPL(*Old); return EFI_NOT_READY; }
  if (mActiveTouch->Exiting) { gBS->RestoreTPL(*Old); return EFI_ABORTED; }
  if (mActiveTouch->Busy) { gBS->RestoreTPL(*Old); return EFI_NOT_READY; }
  mControlBusy=TRUE;*Touch=mActiveTouch;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ControlLeave(EFI_TPL Old,EFI_STATUS Status) {
  mControlBusy=FALSE;gBS->RestoreTPL(Old);return Status;
}
STATIC EFI_STATUS EFIAPI ControlQuiesce(HIMAX_TOUCH_CONTROL *This,EFI_HANDLE Agent,UINT64 *Token) {
  TOUCH *T;EFI_TPL Old;EFI_STATUS S;
  if (!Agent || !Token) return EFI_INVALID_PARAMETER;
  S=ControlEnter(This,&Old,&T);if (EFI_ERROR(S)) return S;
  if (mControlToken && mControlOwner!=Agent) return ControlLeave(Old,EFI_ACCESS_DENIED);
  if (!mControlToken) {
    if (mNextControlToken==MAX_UINT64) return ControlLeave(Old,EFI_OUT_OF_RESOURCES);
    mControlToken=++mNextControlToken;mControlOwner=Agent;mQuiesced=FALSE;
  }
  mQuiesced=FALSE;
  *Token=mControlToken;T->Stopped=TRUE;
  T->ResumeRunning=FALSE; T->ResumeFinished=FALSE;
  T->Board->State(T->Board->Context,HIMAX_TOUCH_STOPPED);
  if (T->Exiting) return ControlLeave(Old,EFI_ABORTED);
  /* Clear a previously down contact while the consumer still has its pointer
   * protocol; GetState may retrieve this release during the stopped interval. */
  Publish(T,NULL);
  if (T->Exiting) return ControlLeave(Old,EFI_ABORTED);
  S=gBS->SetTimer(T->Poll,TimerCancel,0);
  if (EFI_ERROR(S)) T->ResumeCancelPending=TRUE;
  else T->ResumeCancelPending=FALSE;
  if (T->Exiting) return ControlLeave(Old,EFI_ABORTED);
  if (EFI_ERROR(S)) return ControlLeave(Old,S);
  S=ReleasePinLease(T);
  if (T->Exiting) return ControlLeave(Old,EFI_ABORTED);
  if (EFI_ERROR(S)) return ControlLeave(Old,S);
  mQuiesced=TRUE;
  return ControlLeave(Old,EFI_SUCCESS);
}
STATIC EFI_STATUS EFIAPI ControlResume(HIMAX_TOUCH_CONTROL *This,UINT64 Token) {
  TOUCH *T; EFI_TPL Old; EFI_STATUS S;
  S=ControlEnter(This,&Old,&T); if (EFI_ERROR(S)) return S;
  if (!Token || Token!=mControlToken || !mControlOwner) return ControlLeave(Old,EFI_ACCESS_DENIED);
  if (!mQuiesced || !T->Stopped || T->ResumeRunning) return ControlLeave(Old,EFI_NOT_READY);
  /* A failed asynchronous release may still own GPIO152. Retry that lease
   * before TouchReady probes the same pin, and keep the retryable state if it
   * remains owned. */
  if (T->PinLeaseHeld) {
    S=ReleasePinLease(T);
    if (T->Exiting) return ControlLeave(Old,EFI_ABORTED);
    if (EFI_ERROR(S)) {
      BOOLEAN CancelPending=T->ResumeCancelPending;
      EFI_STATUS CancelStatus=CancelResumePoll(T);
      if (!EFI_ERROR(CancelStatus) && CancelPending) T->ResumeFinished=TRUE;
      return ControlLeave(Old,EFI_NOT_READY);
    }
  }
  if (T->ResumeCancelPending) {
    S=CancelResumePoll(T);
    if (EFI_ERROR(S)) return ControlLeave(Old,EFI_NOT_READY);
    T->ResumeFinished=TRUE;
  }
  if (T->ResumeFinished) {
    S=CancelResumePoll(T);
    if (EFI_ERROR(S)) return ControlLeave(Old,EFI_NOT_READY);
    if (EFI_ERROR(T->ResumeStatus)) {
      T->ResumeFinished=FALSE;
      return ControlLeave(Old,T->ResumeStatus);
    }
    if (!T->Board->Ready(T->Board->Context) || T->Exiting)
      return ControlLeave(Old,T->Exiting?EFI_ABORTED:EFI_NOT_READY);
    T->Board->State(T->Board->Context,HIMAX_TOUCH_STARTING);
    if (T->Exiting) return ControlLeave(Old,EFI_ABORTED);
    S=gBS->SetTimer(T->Poll,TimerPeriodic,100000);
    if (T->Exiting) return ControlLeave(Old,EFI_ABORTED);
    if (EFI_ERROR(S)) return ControlLeave(Old,S);
    T->Board->State(T->Board->Context,T->ResumeHasFrame?HIMAX_TOUCH_REPORT_VALID:HIMAX_TOUCH_READY);
    if (T->Exiting) return ControlLeave(Old,EFI_ABORTED);
    T->ResumeFinished=FALSE;
    T->Stopped=FALSE; T->Failures=0; T->ResumeCancelPending=FALSE;
    mControlToken=0; mControlOwner=NULL; mQuiesced=FALSE;
    if (T->ResumeHasFrame) Publish(T,&T->ResumeFrame);
    return ControlLeave(Old,EFI_SUCCESS);
  }

  if (!T->Board->Ready(T->Board->Context) || T->Exiting)
    return ControlLeave(Old,T->Exiting?EFI_ABORTED:EFI_NOT_READY);
  S=gBS->SetTimer(T->Poll,TimerPeriodic,100000);
  if (T->Exiting) return ControlLeave(Old,EFI_ABORTED);
  if (EFI_ERROR(S)) return ControlLeave(Old,S);
  T->Backoff=0; T->ResumePhase=0; T->ResumeAttempts=0;
  T->ResumeRunning=TRUE;
  return ControlLeave(Old,EFI_NOT_READY);
}
STATIC HIMAX_TOUCH_CONTROL mControl={HIMAX_TOUCH_CONTROL_REVISION,ControlQuiesce,ControlResume};

STATIC EFI_STATUS EnsureWaitEvent(TOUCH *T) {
  if (T->Pointer.WaitForInput!=NULL) return EFI_SUCCESS;
  EFI_STATUS S=gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_NOTIFY,Wait,T,&T->Pointer.WaitForInput);
  if(EFI_ERROR(S))T->Pointer.WaitForInput=NULL;
  return S;
}
STATIC EFI_STATUS Validate(HIMAX_TOUCH_BOARD *B) {
  if (!B || B->Revision!=HIMAX_TOUCH_REVISION || B->Flags!=HIMAX_TOUCH_REQUIRED_FLAGS ||
      B->Address!=0x48 || B->Slots!=10 || B->XMax!=1400 || B->YMax!=2240 ||
      !B->Ready || !B->Pending || !B->State || !B->ValidateGpioLease)
    return EFI_UNSUPPORTED;
  return B->Ready(B->Context)?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC EFI_STATUS EFIAPI Supported(EFI_DRIVER_BINDING_PROTOCOL *This,EFI_HANDLE Handle,EFI_DEVICE_PATH_PROTOCOL *Path) {
  HIMAX_TOUCH_BOARD *B; EFI_STATUS S;
  if (mFailedStart!=NULL) {
    if (mFailedStartHandle!=Handle || mFailedStartAgent!=This->DriverBindingHandle) return EFI_NOT_READY;
    S=CleanupFailedStart();
    if (EFI_ERROR(S)) return S;
  }
  S=gBS->HandleProtocol(Handle,&gHimaxTouchBoardProtocolGuid,(VOID **)&B); if (EFI_ERROR(S)) return S;
  S=Validate(B); if (EFI_ERROR(S)) return S;
  S=gBS->OpenProtocol(Handle,&gEfiI2cMasterProtocolGuid,NULL,This->DriverBindingHandle,Handle,EFI_OPEN_PROTOCOL_TEST_PROTOCOL);
  return S;
}
STATIC EFI_STATUS EFIAPI Start(EFI_DRIVER_BINDING_PROTOCOL *This,EFI_HANDLE Handle,EFI_DEVICE_PATH_PROTOCOL *Path) {
  HIMAX_TOUCH_BOARD *B; EFI_I2C_MASTER_PROTOCOL *Master; EFI_DEVICE_PATH_PROTOCOL *DevicePath;
  TOUCH *T; EFI_STATUS S,LeaseStatus;
  if (mFailedStart!=NULL) {
    if (mFailedStartHandle!=Handle || mFailedStartAgent!=This->DriverBindingHandle) return EFI_NOT_READY;
    S=CleanupFailedStart();
    if (EFI_ERROR(S)) return S;
  }
  if (mActiveTouch) return EFI_ALREADY_STARTED;
  S=gBS->HandleProtocol(Handle,&gHimaxTouchBoardProtocolGuid,(VOID **)&B); if (EFI_ERROR(S)) return S;
  S=Validate(B); if (EFI_ERROR(S)) return S;
  S=gBS->HandleProtocol(Handle,&gEfiDevicePathProtocolGuid,(VOID **)&DevicePath); if (EFI_ERROR(S)) return S;
  T=AllocateZeroPool(sizeof(*T)); if (!T) return EFI_OUT_OF_RESOURCES;
  T->Signature=SIGN; T->Board=B;
  S=gBS->LocateProtocol(&gMediaTekGpioProtocolGuid,NULL,(VOID **)&T->Gpio);
  if (EFI_ERROR(S) || !T->Gpio || !T->Gpio->AcquirePins || !T->Gpio->ReleasePins ||
      !T->Gpio->ValidateLease) {
    FreePool(T);
    return EFI_ERROR(S)?S:EFI_UNSUPPORTED;
  }
  S=gBS->HandleProtocol(Handle,&gMediaTekI2cExclusiveProtocolGuid,(VOID **)&T->I2c);
  if (EFI_ERROR(S) || !T->I2c || T->I2c->Revision!=MTK_I2C_EXCLUSIVE_PROTOCOL_REVISION ||
      !T->I2c->Acquire || !T->I2c->TransferTimed || !T->I2c->Release) {
    FreePool(T);
    return EFI_ERROR(S)?S:EFI_UNSUPPORTED;
  }
  S=gBS->OpenProtocol(Handle,&gEfiI2cMasterProtocolGuid,(VOID **)&Master,
    This->DriverBindingHandle,Handle,EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR(S)) { FreePool(T); return S; }
  T->Master=Master; T->I2cOpen=TRUE;
  B->State(B->Context,HIMAX_TOUCH_STARTING);
  T->Io.Context=T; T->Io.Transfer=HimaxTransfer; T->Io.Delay=HimaxDelay;
  HmPointerInit(&T->Position);
  /* Read the source-confirmed HX83132 ID and use only the no-reset sense-on
   * branch. Firmware-running/exclusive ownership comes from the board gate. */
  S=AcquirePinLease(T);
  if (!EFI_ERROR(S) && (HmReadChipId(&T->Io)!=HmOk || HmSenseOn(&T->Io)!=HmOk)) S=EFI_DEVICE_ERROR;
  LeaseStatus=ReleasePinLease(T);
  if (EFI_ERROR(LeaseStatus)) S=LeaseStatus;
  if (EFI_ERROR(S)) goto FailedStart;
  T->Mode.AbsoluteMaxX=B->XMax; T->Mode.AbsoluteMaxY=B->YMax; T->Mode.AbsoluteMaxZ=255;
  T->Mode.Attributes=EFI_ABSP_SupportsPressureAsZ;
  T->Pointer.Reset=Reset; T->Pointer.GetState=GetState; T->Pointer.Mode=&T->Mode;
  // Failed event creation leaves its OUT handle undefined, not owned.
  S=gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_NOTIFY,Wait,T,&T->Pointer.WaitForInput);
  if (EFI_ERROR(S)) { T->Pointer.WaitForInput=NULL;goto FailedStart; }
  S=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Poll,T,&T->Poll);
  if (EFI_ERROR(S)) { T->Poll=NULL;goto FailedStart; }
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,T,&gEfiEventExitBootServicesGuid,&T->Exit);
  if (EFI_ERROR(S)) { T->Exit=NULL;goto FailedStart; }
  S=gBS->InstallMultipleProtocolInterfaces(&Handle,&gEfiAbsolutePointerProtocolGuid,&T->Pointer,NULL);
  if (EFI_ERROR(S)) goto FailedStart;
  T->PointerInstalled=TRUE;
  S=gBS->SetTimer(T->Poll,TimerPeriodic,100000);
  if (!EFI_ERROR(S)) {
    mActiveTouch=T;
    B->State(B->Context,HIMAX_TOUCH_READY);
    return S;
  }
FailedStart:
  B->State(B->Context,HIMAX_TOUCH_STOPPED);
  {
    EFI_ABSOLUTE_POINTER_PROTOCOL *Existing=NULL;
    EFI_STATUS CleanupStatus,Lookup;
    Lookup=gBS->HandleProtocol(Handle,&gEfiAbsolutePointerProtocolGuid,(VOID **)&Existing);
    if ((Lookup==EFI_SUCCESS && Existing==&T->Pointer) ||
        (EFI_ERROR(Lookup) && Lookup!=EFI_NOT_FOUND)) T->PointerInstalled=TRUE;
    mFailedStart=T; mFailedStartHandle=Handle; mFailedStartAgent=This->DriverBindingHandle;
    CleanupStatus=CloseTouchEvents(T);
    if (EFI_ERROR(CleanupStatus)) return S;
    CleanupStatus=CleanupFailedStart();
    return EFI_ERROR(CleanupStatus)?CleanupStatus:S;
  }
}
STATIC EFI_STATUS EFIAPI Stop(EFI_DRIVER_BINDING_PROTOCOL *This,EFI_HANDLE Handle,UINTN Children,EFI_HANDLE *ChildHandles) {
  EFI_ABSOLUTE_POINTER_PROTOCOL *Pointer; TOUCH *T; EFI_STATUS S,RestoreStatus;
  if (Children) return EFI_UNSUPPORTED;
  if (mFailedStart!=NULL) {
    if (mFailedStartHandle!=Handle || mFailedStartAgent!=This->DriverBindingHandle) return EFI_NOT_READY;
    return CleanupFailedStart();
  }
  S=gBS->HandleProtocol(Handle,&gEfiAbsolutePointerProtocolGuid,(VOID **)&Pointer); if (EFI_ERROR(S)) return S;
  T=CTX(Pointer); if (T->Busy || mControlBusy) return EFI_NOT_READY;
  if (T==mActiveTouch && mControlToken) return EFI_ACCESS_DENIED;
  T->Stopped=TRUE; T->Changed=FALSE;
  T->DeliveredActive=FALSE; T->ReleasePending=FALSE;
  T->Board->State(T->Board->Context,HIMAX_TOUCH_STOPPED);
  S=ReleasePinLease(T);
  if (EFI_ERROR(S)) return S;
  S=CloseTouchEvents(T);
  if (EFI_ERROR(S)) return S;
  S=gBS->UninstallMultipleProtocolInterfaces(Handle,&gEfiAbsolutePointerProtocolGuid,Pointer,NULL);
  if (EFI_ERROR(S)) {
    EFI_ABSOLUTE_POINTER_PROTOCOL *Existing=NULL;
    EFI_STATUS Lookup=gBS->HandleProtocol(Handle,&gEfiAbsolutePointerProtocolGuid,(VOID **)&Existing);
    if (Lookup!=EFI_NOT_FOUND && (EFI_ERROR(Lookup) || Existing==Pointer)) {
      (VOID)EnsureWaitEvent(T);
      return S;
    }
  }
  T->PointerInstalled=FALSE;
  S=gBS->CloseProtocol(Handle,&gEfiI2cMasterProtocolGuid,This->DriverBindingHandle,Handle);
  if (EFI_ERROR(S)) {
    RestoreStatus=EnsureWaitEvent(T);
    if (!EFI_ERROR(RestoreStatus)) {
      RestoreStatus=gBS->InstallMultipleProtocolInterfaces(&Handle,
        &gEfiAbsolutePointerProtocolGuid,&T->Pointer,NULL);
      if (!EFI_ERROR(RestoreStatus)) T->PointerInstalled=TRUE;
      else {
        EFI_ABSOLUTE_POINTER_PROTOCOL *Existing=NULL;
        EFI_STATUS Lookup=gBS->HandleProtocol(Handle,&gEfiAbsolutePointerProtocolGuid,
            (VOID **)&Existing);
        if ((Lookup==EFI_SUCCESS && Existing==&T->Pointer) ||
            (EFI_ERROR(Lookup) && Lookup!=EFI_NOT_FOUND))
          T->PointerInstalled=TRUE;
      }
    }
    if (EFI_ERROR(RestoreStatus)) {
      /* Keep an uninstalled device reachable until its event and I2C owners
       * have been closed by a later Supported/Start/Stop retry. */
      mFailedStart=T; mFailedStartHandle=Handle; mFailedStartAgent=This->DriverBindingHandle;
      return RestoreStatus;
    }
    return S;
  }
  T->I2cOpen=FALSE;
  if (mActiveTouch==T) mActiveTouch=NULL;
  FreePool(T); return EFI_SUCCESS;
}
STATIC EFI_DRIVER_BINDING_PROTOCOL mBinding={Supported,Start,Stop,0x10,NULL,NULL};
EFI_STATUS EFIAPI HimaxEntry(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS S=EfiLibInstallDriverBindingComponentName2(ImageHandle,SystemTable,&mBinding,ImageHandle,NULL,NULL);
  if (EFI_ERROR(S)) return S;
  (VOID)gBS->InstallMultipleProtocolInterfaces(&ImageHandle,&gHimaxTouchControlProtocolGuid,&mControl,NULL);
  /* DriverBinding owns this image even if the optional control publication
   * fails. Its absence prevents coordinated panel work; no touch I/O here. */
  return EFI_SUCCESS;
}
