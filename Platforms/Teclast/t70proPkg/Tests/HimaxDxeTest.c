// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#include "../../../../Silicon/MediaTek/MediaTekPkg/Drivers/HimaxTouchDxe/HimaxTouchDxe.c"
EFI_GUID gHimaxTouchControlProtocolGuid={7},gHimaxTouchBoardProtocolGuid={1},gEfiI2cMasterProtocolGuid={2},gEfiEventExitBootServicesGuid={3},gEfiAbsolutePointerProtocolGuid={4},gEfiDevicePathProtocolGuid={5},gMediaTekGpioProtocolGuid={6},gMediaTekI2cExclusiveProtocolGuid={8};
static BOOLEAN IsReady=TRUE,HasData=TRUE,RequireLeaseClearForReady;
static unsigned ReadyWhileLeaseHeld,TimerCancelCalls,TimerCancelFailures;
static EFI_EVENT PollTimerEvent;
static BOOLEAN PollTimerActive;
static unsigned Reads,Transfers,Signals,Closed,Events,FailEvent,EventCloses,CloseAttempts;
static UINT32 SelectedAddress;
static BOOLEAN ChipIdValid=TRUE;
static EFI_STATUS PeriodicTimerStatus;
static BOOLEAN ReenterControl,CountControlBoundaries;
static unsigned ControlBoundaries,ExitControlAt,ExitTransferCount;
static TOUCH *ControlExitTouch;
static void ControlBoundary(void) {
  if(!CountControlBoundaries)return;
  ControlBoundaries++;
  if(ExitControlAt && ControlBoundaries==ExitControlAt) {
    ExitControlAt=0;ExitTransferCount=Transfers;
    Exit(NULL,ControlExitTouch);
  }
}
static unsigned SenseOnCommands,InterfaceBadReadbacks,InterfaceReadbacks;
static UINT8 FirmwareState=0x05;
static EFI_STATUS IoStatus,InstallStatus,UninstallStatus,CloseProtocolStatus,CloseEventStatus,TimerStatus;
static EFI_ABSOLUTE_POINTER_PROTOCOL *Installed;
static EFI_EVENT FailCloseEvent,LastCreatedEvent;
static EFI_EVENT_NOTIFY SavedNotify[64],LastNotify;
static VOID *SavedContext[64],*LastEventContext;
static EFI_EVENT SavedEvent[64];
static unsigned char Report[56];
static EFI_TPL CurrentTpl=TPL_APPLICATION;
static EFI_TPL EFIAPI Raise(EFI_TPL T) { EFI_TPL Old=CurrentTpl;CHECK(T>=Old);CurrentTpl=T;return Old; }
static VOID EFIAPI Restore(EFI_TPL T) { CurrentTpl=T; }
static MTK_GPIO_PROTOCOL Gpio;
static BOOLEAN GpioLeaseHeld,BoardGpioSnapshotValid=TRUE,GpioAvailable=TRUE,InvalidateGpioAfterTransfer;
static BOOLEAN BoardGcdMapped=TRUE,BoardInfrastructureReady=TRUE,BoardEintReady=TRUE;
static UINT64 GpioLeaseToken,NextGpioLeaseToken=0x151;
static unsigned GpioAcquireCalls,GpioReleaseCalls,GpioValidateCalls,BoardGpioValidateCalls;
static EFI_STATUS GpioLocateStatus=EFI_SUCCESS,GpioAcquireStatus=EFI_SUCCESS;
static EFI_STATUS GpioReleaseStatus=EFI_SUCCESS,GpioValidateStatus=EFI_SUCCESS;
static HIMAX_TOUCH_BOARD Board;
static BOOLEAN EFIAPI Ready(VOID *P) { ControlBoundary();if(RequireLeaseClearForReady&&GpioLeaseHeld)ReadyWhileLeaseHeld++;return IsReady&&(!RequireLeaseClearForReady||!GpioLeaseHeld); }
static BOOLEAN EFIAPI Pending(VOID *P) { return HasData; }
static UINT32 BoardState;
static VOID EFIAPI StateUpdate(VOID *P,UINT32 State) { CHECK(State<=HIMAX_TOUCH_READY); BoardState=State;ControlBoundary(); }
static BOOLEAN EFIAPI BoardValidateGpioLease(VOID *P,MTK_GPIO_PROTOCOL *Protocol,UINT64 Token) {
  BoardGpioValidateCalls++;ControlBoundary();
  return P==&Board && Protocol==&Gpio && GpioLeaseHeld && Token!=0 &&
    Token==GpioLeaseToken && BoardGpioSnapshotValid && BoardGcdMapped &&
    BoardInfrastructureReady && BoardEintReady;
}
static HIMAX_TOUCH_BOARD Board={HIMAX_TOUCH_REVISION,HIMAX_TOUCH_REQUIRED_FLAGS,0x48,10,1400,2240,NULL,Ready,Pending,StateUpdate,BoardValidateGpioLease};
VOID *EFIAPI AllocateZeroPool(UINTN N) { return calloc(1,N); }
static EFI_STATUS EFIAPI Read(CONST EFI_I2C_MASTER_PROTOCOL *This,UINTN Address,EFI_I2C_REQUEST_PACKET *P,EFI_EVENT E,EFI_STATUS *S) {
  UINT8 *W; UINTN N; Transfers++;ControlBoundary();
  if(ReenterControl) {
    UINT64 Nested=0;ReenterControl=FALSE;
    CHECK(mControl.Resume(&mControl,mControlToken)==EFI_NOT_READY);
    CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Nested)==EFI_NOT_READY);
    CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_NOT_READY);
  }
  CHECK(GpioLeaseHeld && GpioLeaseToken!=0);
  CHECK(Address==0x48&&(P->OperationCount==1||P->OperationCount==2)&&!E&&!S);
  CHECK(!P->Operation[0].Flags&&P->Operation[0].Buffer!=NULL);
  if (!P->OperationCount || !P->Operation[0].Buffer) return EFI_INVALID_PARAMETER;
  W=P->Operation[0].Buffer; N=P->Operation[0].LengthInBytes;
  if (P->OperationCount==2) {
    CHECK(P->Operation[1].Flags==I2C_FLAG_READ&&P->Operation[1].Buffer!=NULL);
    if (!P->Operation[1].Buffer) return EFI_INVALID_PARAMETER;
  }
  if (W[0]==0x30) {
    Reads++; CHECK(N==1&&P->OperationCount==2&&P->Operation[1].LengthInBytes==56);
    if (!EFI_ERROR(IoStatus)) memcpy(P->Operation[1].Buffer,Report,56);
  } else if (W[0]==0x13 || W[0]==0x0d) {
    UINT8 Value=W[0]==0x13?0x31:0x10;
    if (P->OperationCount==2) {
      CHECK(N==1&&P->Operation[1].LengthInBytes==1);
      if (!EFI_ERROR(IoStatus)) {
        *(UINT8 *)P->Operation[1].Buffer=InterfaceReadbacks<InterfaceBadReadbacks?0:Value;
        if (W[0]==0x0d) InterfaceReadbacks++;
      }
    }
    else CHECK(N==2&&W[1]==Value);
  }
  else if (W[0]==0x00) {
    CHECK(P->OperationCount==1&&(N==5||N==9));
    if (N>=5) SelectedAddress=(UINT32)W[1]|((UINT32)W[2]<<8)|((UINT32)W[3]<<16)|((UINT32)W[4]<<24);
    if (N==9) CHECK(SelectedAddress==0x9000005c&&W[5]==0&&W[6]==0&&W[7]==0&&W[8]==0);
  } else if (W[0]==0x0c) CHECK(N==2&&W[1]==0&&P->OperationCount==1);
  else if (W[0]==0x08) {
    CHECK(N==1&&P->OperationCount==2&&P->Operation[1].LengthInBytes==4);
    if (!EFI_ERROR(IoStatus)) {
      UINT8 Data[4]={0x55,(UINT8)(ChipIdValid?0x2a:0x29),0x13,0x83};
      CHECK(SelectedAddress==HM_CHIP_ID_REGISTER || SelectedAddress==0x900000a8U);
      if (SelectedAddress==0x900000a8U) {
        memset(Data,0,sizeof(Data));
        Data[0]=FirmwareState;
      }
      memcpy(P->Operation[1].Buffer,Data,sizeof(Data));
    }
  } else if (W[0]==0x31) { SenseOnCommands++;CHECK(N==2&&W[1]==0&&P->OperationCount==1); }
  else CHECK(0);
  if(InvalidateGpioAfterTransfer)BoardGpioSnapshotValid=FALSE;
  return IoStatus;
}
static EFI_I2C_MASTER_PROTOCOL Master={.StartRequest=Read};
static UINT64 I2cToken;
static EFI_STATUS EFIAPI AcquireI2c(MTK_I2C_EXCLUSIVE_PROTOCOL *This,UINT64 *Token) {
  CHECK(!I2cToken && CurrentTpl==TPL_CALLBACK);
  *Token=I2cToken=0x135;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TransferTimed(MTK_I2C_EXCLUSIVE_PROTOCOL *This,UINT64 Token,UINTN Address,
                                      EFI_I2C_REQUEST_PACKET *Packet,UINT32 TimeoutUs) {
  CHECK(Token && Token==I2cToken && TimeoutUs==5000 && CurrentTpl==TPL_CALLBACK);
  return Read(&Master,Address,Packet,NULL,NULL);
}
static EFI_STATUS EFIAPI ReleaseI2c(MTK_I2C_EXCLUSIVE_PROTOCOL *This,UINT64 Token) {
  CHECK(Token && Token==I2cToken && CurrentTpl==TPL_CALLBACK);
  I2cToken=0;
  return EFI_SUCCESS;
}
static MTK_I2C_EXCLUSIVE_PROTOCOL I2c={.Revision=MTK_I2C_EXCLUSIVE_PROTOCOL_REVISION,
  .Acquire=AcquireI2c,.Release=ReleaseI2c,.TransferTimed=TransferTimed};

static void Tick(TOUCH *T) {
  const unsigned Before=Transfers;
  const UINT64 BeforeTime=TimeNs;
  EFI_TPL Old=Raise(TPL_CALLBACK);
  Poll(T->Poll,T);
  Restore(Old);
  CHECK(Transfers-Before<=1 && TimeNs==BeforeTime);
}

static EFI_STATUS RunResume(TOUCH *T,UINT64 Token) {
  EFI_STATUS S=mControl.Resume(&mControl,Token);
  if(S!=EFI_NOT_READY || !T->ResumeRunning) return S;
  for(unsigned Count=0;Count<64 && T->ResumeRunning && !T->Exiting;Count++) Tick(T);
  CHECK(!T->ResumeRunning || T->Exiting);
  return mControl.Resume(&mControl,Token);
}
static EFI_STATUS EFIAPI AcquirePins(CONST UINT32 *Pins,UINTN Count,UINT64 *Token) {
  GpioAcquireCalls++;ControlBoundary();CHECK(Pins && Count==1 && Pins[0]==152 && Token);
  if(GpioAcquireStatus!=EFI_SUCCESS)return GpioAcquireStatus;
  if(GpioLeaseHeld)return EFI_ACCESS_DENIED;
  GpioLeaseHeld=TRUE;GpioLeaseToken=++NextGpioLeaseToken;*Token=GpioLeaseToken;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ReleasePins(UINT64 Token) {
  GpioReleaseCalls++;ControlBoundary();
  if(!GpioLeaseHeld || Token!=GpioLeaseToken)return EFI_INVALID_PARAMETER;
  if(GpioReleaseStatus!=EFI_SUCCESS)return GpioReleaseStatus;
  GpioLeaseHeld=FALSE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ValidateLease(UINT64 Token,UINT32 Pin) {
  GpioValidateCalls++;CHECK(Pin==152);
  if(GpioValidateStatus!=EFI_SUCCESS)return GpioValidateStatus;
  return GpioLeaseHeld && Token==GpioLeaseToken?EFI_SUCCESS:EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI LocateGpio(EFI_GUID *G,VOID *Registration,VOID **Out) {
  CHECK(G==&gMediaTekGpioProtocolGuid && !Registration && Out);
  if(GpioLocateStatus!=EFI_SUCCESS)return GpioLocateStatus;
  if(!GpioAvailable)return EFI_NOT_FOUND;
  *Out=&Gpio;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **Out) {
  if(G==&gHimaxTouchBoardProtocolGuid)*Out=&Board;
  else if(G==&gEfiAbsolutePointerProtocolGuid)*Out=Installed;
  else if(G==&gMediaTekI2cExclusiveProtocolGuid)*Out=&I2c;
  else if(G==&gEfiDevicePathProtocolGuid)*Out=(VOID *)0x4000;
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_HANDLE H,EFI_GUID *G,VOID **Out,EFI_HANDLE A,EFI_HANDLE C,UINT32 Flags) { if(Out)*Out=&Master;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Close(EFI_HANDLE H,EFI_GUID *G,EFI_HANDLE A,EFI_HANDLE C) {
  CloseAttempts++; if (EFI_ERROR(CloseProtocolStatus)) return CloseProtocolStatus;
  Closed++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Event(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY Notify,VOID *C,EFI_EVENT *Out) {
  EFI_EVENT E; Events++; if(Events==FailEvent) { *Out=(VOID *)0xdead;return EFI_OUT_OF_RESOURCES; }
  E=(VOID *)(UINTN)(0x1000+Events); *Out=E; LastCreatedEvent=E; LastNotify=Notify; LastEventContext=C;
  if(Events<64) { SavedEvent[Events]=E; SavedNotify[Events]=Notify; SavedContext[Events]=C; }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventEx(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY Notify,CONST VOID *C,CONST EFI_GUID *Group,EFI_EVENT *Out) {return Event(Type,T,Notify,(VOID *)C,Out);}
static EFI_STATUS EFIAPI CloseEvent(EFI_EVENT E) {
  CHECK(E!=(VOID *)0xdead);
  EventCloses++;
  if(E==FailCloseEvent)return CloseEventStatus;
  for(unsigned I=1;I<64;I++) if(SavedEvent[I]==E) { SavedNotify[I]=NULL; SavedContext[I]=NULL; }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Timer(EFI_EVENT E,EFI_TIMER_DELAY T,UINT64 N) {
  CHECK(E!=(VOID *)0xdead);
  ControlBoundary();
  if(T==TimerCancel) {
    TimerCancelCalls++;
    if(TimerCancelFailures) { TimerCancelFailures--; return EFI_DEVICE_ERROR; }
    if(EFI_ERROR(TimerStatus))return TimerStatus;
    if(E==PollTimerEvent)PollTimerActive=FALSE;
  }
  if(T==TimerPeriodic) {
    CHECK(Installed && N==100000);
    if(EFI_ERROR(PeriodicTimerStatus))return PeriodicTimerStatus;
    PollTimerEvent=E;PollTimerActive=TRUE;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Signal(EFI_EVENT E) {Signals++;ControlBoundary();return EFI_SUCCESS;}
static EFI_STATUS EFIAPI InstallPointer(EFI_HANDLE *H,...) {va_list A;va_start(A,H);(void)va_arg(A,EFI_GUID *);if(!EFI_ERROR(InstallStatus))Installed=va_arg(A,VOID *);va_end(A);return InstallStatus;}
static EFI_STATUS EFIAPI UninstallPointer(EFI_HANDLE H,...) {if(!EFI_ERROR(UninstallStatus))Installed=NULL;return UninstallStatus;}
static void MakeReport(void) {unsigned I,S=0;memset(Report,255,40);memset(Report+40,0,16);Report[0]=0;Report[1]=123;Report[2]=1;Report[3]=200;Report[40]=80;Report[52]=1;for(I=0;I<55;I++)S+=Report[I];Report[55]=(UINT8)-S;}
static void SecondaryReport(void) {
  unsigned I,S=0;
  MakeReport();memset(Report,255,4);
  Report[4]=0;Report[5]=42;Report[6]=0;Report[7]=90;Report[41]=55;
  for(I=0;I<55;I++)S+=Report[I];Report[55]=(UINT8)-S;
}
static void TestDeferredRelease(TOUCH *T) {
  EFI_ABSOLUTE_POINTER_STATE S; TOUCH Local; unsigned Before;
  /* A was already delivered. Two further polls must not replace its release
   * with B's press before the consumer gets a chance to read it. */
  SecondaryReport();Tick(T);Tick(T);Tick(T);
  CHECK(GetState(&T->Pointer,NULL)==EFI_INVALID_PARAMETER);
  CHECK(GetState(&T->Pointer,&S)==EFI_SUCCESS&&!S.ActiveButtons&&S.CurrentX==123);
  Before=Signals;Wait(T->Pointer.WaitForInput,T);CHECK(Signals==Before+1);
  CHECK(GetState(&T->Pointer,&S)==EFI_SUCCESS&&S.ActiveButtons&&S.CurrentX==42);
  CHECK(GetState(&T->Pointer,&S)==EFI_NOT_READY);
  Before=Signals;Wait(T->Pointer.WaitForInput,T);CHECK(Signals==Before);
  /* Error release followed by recovery is subject to the same ordering. */
  IoStatus=EFI_DEVICE_ERROR;
  unsigned FailedTransfers=Transfers;
  for(unsigned TickIndex=0;TickIndex<5;TickIndex++) Tick(T);
  CHECK(Transfers==FailedTransfers+3);
  IoStatus=EFI_SUCCESS;Tick(T);Tick(T);
  CHECK(GetState(&T->Pointer,&S)==EFI_SUCCESS&&!S.ActiveButtons&&S.CurrentX==42);
  CHECK(GetState(&T->Pointer,&S)==EFI_SUCCESS&&S.ActiveButtons&&S.CurrentX==42);
  IoStatus=EFI_DEVICE_ERROR;
  FailedTransfers=Transfers;
  for(unsigned TickIndex=0;TickIndex<5;TickIndex++) Tick(T);
  CHECK(Transfers==FailedTransfers+3);
  IoStatus=EFI_SUCCESS;Tick(T);
  CHECK(Reset(&T->Pointer,FALSE)==EFI_SUCCESS);
  CHECK(GetState(&T->Pointer,&S)==EFI_NOT_READY);
  MakeReport();Tick(T);
  CHECK(GetState(&T->Pointer,&S)==EFI_SUCCESS&&S.ActiveButtons&&S.CurrentX==123);
  /* Final lifecycle teardown discards pending input even if events are gone. */
  memset(&Local,0,sizeof(Local));Local.Signature=SIGN;
  Local.Changed=Local.ReleasePending=Local.DeliveredActive=TRUE;
  Exit(NULL,&Local);
  CHECK(!Local.Changed&&!Local.ReleasePending&&!Local.DeliveredActive);
  CHECK(GetState(&Local.Pointer,&S)==EFI_DEVICE_ERROR);
  Local.Exiting=FALSE;Local.Changed=Local.ReleasePending=Local.DeliveredActive=TRUE;
  CHECK(CloseTouchEvents(&Local)==EFI_SUCCESS);
  CHECK(!Local.Changed&&!Local.ReleasePending&&!Local.DeliveredActive);
  Before=Signals;Wait(NULL,&Local);CHECK(Signals==Before);
}

static void ControlTests(TOUCH *T) {
  UINT64 Token=0,Other=0,OldToken;
  unsigned Before,SenseBefore=SenseOnCommands;
  CHECK(mActiveTouch==T && !mControlToken && !mControlBusy);
  MakeReport();IsReady=TRUE;IoStatus=EFI_SUCCESS;
  CHECK(mControl.Quiesce(NULL,(VOID *)0xabc,&Token)==EFI_INVALID_PARAMETER);
  CHECK(mControl.Quiesce(&mControl,NULL,&Token)==EFI_INVALID_PARAMETER);
  CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,NULL)==EFI_INVALID_PARAMETER);
  T->Busy=TRUE;Before=Transfers;
  CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Token)==EFI_NOT_READY && !Token && Transfers==Before);
  T->Busy=FALSE;CurrentTpl=TPL_NOTIFY;
  CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Token)==EFI_NOT_READY && !Token);
  CurrentTpl=TPL_APPLICATION;
  CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Token)==EFI_SUCCESS && Token && T->Stopped && mQuiesced);
  CHECK(Transfers==Before && BoardState==HIMAX_TOUCH_STOPPED && T->I2cOpen && T->PointerInstalled && !T->PinLeaseHeld);
  Tick(T);CHECK(Transfers==Before);
  CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_ACCESS_DENIED && Installed);
  CHECK(mControl.Quiesce(&mControl,(VOID *)0xdef,&Other)==EFI_ACCESS_DENIED && !Other);
  CHECK(mControl.Resume(&mControl,0)==EFI_ACCESS_DENIED && Transfers==Before);
  CHECK(mControl.Resume(&mControl,Token+1)==EFI_ACCESS_DENIED && Transfers==Before);
  IsReady=FALSE;CHECK(mControl.Resume(&mControl,Token)==EFI_NOT_READY && T->Stopped && Transfers==Before);IsReady=TRUE;
  IoStatus=EFI_TIMEOUT;CHECK(RunResume(T,Token)==EFI_DEVICE_ERROR && T->Stopped && mControlToken==Token && !T->PinLeaseHeld);
  IoStatus=EFI_SUCCESS;ChipIdValid=FALSE;
  CHECK(RunResume(T,Token)==EFI_DEVICE_ERROR && T->Stopped && mControlToken==Token);ChipIdValid=TRUE;
  Report[3]^=1;
  CHECK(RunResume(T,Token)==EFI_DEVICE_ERROR && T->Stopped && BoardState==HIMAX_TOUCH_STOPPED);
  MakeReport();PeriodicTimerStatus=EFI_DEVICE_ERROR;
  CHECK(RunResume(T,Token)==EFI_DEVICE_ERROR && T->Stopped && mControlToken==Token);
  PeriodicTimerStatus=EFI_SUCCESS;ReenterControl=TRUE;
  CHECK(RunResume(T,Token)==EFI_SUCCESS && !ReenterControl && !T->Stopped && !mControlToken && BoardState==HIMAX_TOUCH_REPORT_VALID);
  CHECK(SenseOnCommands==SenseBefore && !GpioLeaseHeld && !mControlBusy && CurrentTpl==TPL_APPLICATION);
  Token=0;CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Token)==EFI_SUCCESS);
  HasData=FALSE;unsigned BeforeIdle=Reads;
  FirmwareState=0x0c;
  CHECK(RunResume(T,Token)==EFI_DEVICE_ERROR && T->Stopped && Reads==BeforeIdle);
  FirmwareState=0x05;
  CHECK(RunResume(T,Token)==EFI_SUCCESS && !T->Stopped &&
        BoardState==HIMAX_TOUCH_READY && Reads==BeforeIdle);
  HasData=TRUE;
  Token=0;CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Token)==EFI_SUCCESS);
  MakeReport();CHECK(mControl.Resume(&mControl,Token)==EFI_NOT_READY && T->ResumeRunning);
  for(unsigned I=0;I<40 && T->ResumeRunning;I++)Tick(T);
  CHECK(!T->ResumeRunning && T->ResumeFinished && !PollTimerActive);
  PeriodicTimerStatus=EFI_DEVICE_ERROR;
  CHECK(mControl.Resume(&mControl,Token)==EFI_DEVICE_ERROR && T->Stopped &&
        T->ResumeFinished && !PollTimerActive && BoardState==HIMAX_TOUCH_STARTING);
  PeriodicTimerStatus=EFI_SUCCESS;
  CHECK(mControl.Resume(&mControl,Token)==EFI_SUCCESS && !T->Stopped && PollTimerActive &&
        BoardState==HIMAX_TOUCH_REPORT_VALID);
  OldToken=Token;Token=0;TimerStatus=EFI_DEVICE_ERROR;Before=Transfers;
  CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Token)==EFI_DEVICE_ERROR && Token && Token!=OldToken && T->Stopped && !mQuiesced);
  CHECK(mControl.Resume(&mControl,Token)==EFI_NOT_READY && Transfers==Before);
  TimerStatus=EFI_SUCCESS;
  CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Other)==EFI_SUCCESS && Other==Token && mQuiesced && Transfers==Before);
  CHECK(mControl.Resume(&mControl,OldToken)==EFI_ACCESS_DENIED);
  GpioAcquireStatus=EFI_ACCESS_DENIED;
  CHECK(RunResume(T,Token)==EFI_ACCESS_DENIED && T->Stopped && Transfers==Before);
  GpioAcquireStatus=EFI_SUCCESS;GpioReleaseStatus=EFI_DEVICE_ERROR;
  TimerCancelFailures=2;
  CHECK(RunResume(T,Token)==EFI_NOT_READY && T->PinLeaseHeld && GpioLeaseHeld && T->Stopped);
  CHECK(T->ResumeCancelPending && !T->ResumeFinished && PollTimerActive);
  GpioReleaseStatus=EFI_SUCCESS;RequireLeaseClearForReady=TRUE;Before=Transfers;
  CHECK(mControl.Resume(&mControl,Token)==EFI_DEVICE_ERROR);
  CHECK(!T->PinLeaseHeld && !GpioLeaseHeld && !PollTimerActive && Transfers==Before && ReadyWhileLeaseHeld==0);
  CHECK(mControl.Resume(&mControl,Token)==EFI_NOT_READY && T->ResumeRunning && PollTimerActive);
  CHECK(ReadyWhileLeaseHeld==0);
  CHECK(RunResume(T,Token)==EFI_SUCCESS && !T->Stopped && !mControlToken && PollTimerActive);
  Before=Transfers;HasData=TRUE;Tick(T);CHECK(Transfers>Before);
  RequireLeaseClearForReady=FALSE;
  CHECK(AcquirePinLease(T)==EFI_SUCCESS);GpioReleaseStatus=EFI_DEVICE_ERROR;Token=0;Before=Transfers;
  CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Token)==EFI_DEVICE_ERROR && Token && T->PinLeaseHeld && T->Stopped && !mQuiesced && Transfers==Before);
  GpioReleaseStatus=EFI_SUCCESS;
  CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Other)==EFI_SUCCESS && Token==Other && !T->PinLeaseHeld);
  CHECK(RunResume(T,Token)==EFI_SUCCESS && !mControlToken && !T->Stopped);
  { UINT64 Saved=mNextControlToken;mNextControlToken=MAX_UINT64;Token=0;
    CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Token)==EFI_OUT_OF_RESOURCES && !Token && !T->Stopped);
    mNextControlToken=Saved;
  }
  CHECK(SenseOnCommands==SenseBefore);
  Token=0;
  CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Token)==EFI_SUCCESS && mQuiesced);
  TimerStatus=EFI_DEVICE_ERROR;
  CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Other)==EFI_DEVICE_ERROR &&
        Other==Token && !mQuiesced && T->ResumeCancelPending);
  Before=Transfers;
  CHECK(mControl.Resume(&mControl,Token)==EFI_NOT_READY && Transfers==Before);
  TimerStatus=EFI_SUCCESS;
  CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Other)==EFI_SUCCESS && mQuiesced);
  CHECK(RunResume(T,Token)==EFI_SUCCESS && !T->Stopped && !mControlToken && PollTimerActive);
}
static void ControlExitTests(void) {
  for(unsigned Operation=0;Operation<2;Operation++) {
    unsigned Boundaries=0;
    for(unsigned Inject=0;Inject<=Boundaries;Inject++) {
      UINT64 Token=0;EFI_STATUS S;TOUCH *T;
      CHECK(!mActiveTouch && !mControlToken);
      CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS);T=mActiveTouch;
      MakeReport();HasData=TRUE;Tick(T);
      if(Operation) CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Token)==EFI_SUCCESS);
      CountControlBoundaries=TRUE;ControlBoundaries=0;ExitControlAt=Inject;ControlExitTouch=T;
      S=Operation?RunResume(T,Token):mControl.Quiesce(&mControl,(VOID *)0xabc,&Token);
      CountControlBoundaries=FALSE;
      if(!Inject) {CHECK(S==EFI_SUCCESS);Boundaries=ControlBoundaries;}
      else {
        CHECK(S==EFI_ABORTED && T->Exiting && T->Stopped && !mControlBusy);
        CHECK(Transfers==ExitTransferCount);
        unsigned Before=Transfers;
        Tick(T);
        CHECK(mControl.Resume(&mControl,Token)==EFI_ABORTED && Transfers==Before);
        CHECK(mControl.Quiesce(&mControl,(VOID *)0xabc,&Token)==EFI_ABORTED && Transfers==Before);
      }
      /* Terminal test-only cleanup after injected EBS. The production API
       * intentionally never revives a session after EBS starts. */
      mControlToken=0;mControlOwner=NULL;mQuiesced=FALSE;ExitControlAt=0;I2cToken=0;
      CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_SUCCESS && !mActiveTouch && !Installed);
    }
    CHECK(Boundaries>=2);
  }
}
int main(void) {
 EFI_ABSOLUTE_POINTER_STATE State;TOUCH *T;unsigned BeforeTransfers,BeforeGpioAcquires,BeforeGpioReleases,BeforeClosed;
 Board.Context=&Board;
 IoStatus=InstallStatus=UninstallStatus=CloseProtocolStatus=TimerStatus=EFI_SUCCESS;
 CloseEventStatus=EFI_ACCESS_DENIED; FailCloseEvent=NULL; Events=EventCloses=Closed=CloseAttempts=0;
 Gpio.AcquirePins=AcquirePins;Gpio.ReleasePins=ReleasePins;Gpio.ValidateLease=ValidateLease;
 Services.LocateProtocol=LocateGpio;
 Services.RaiseTPL=Raise;Services.RestoreTPL=Restore;Services.HandleProtocol=Handle;Services.OpenProtocol=Open;Services.CloseProtocol=Close;
 Services.CreateEvent=Event;Services.CreateEventEx=EventEx;Services.CloseEvent=CloseEvent;Services.SetTimer=Timer;Services.SignalEvent=Signal;
 Services.InstallMultipleProtocolInterfaces=InstallPointer;Services.UninstallMultipleProtocolInterfaces=UninstallPointer;
 Board.Revision=1;CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_UNSUPPORTED);
 Board.Revision=2;CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_UNSUPPORTED);Board.Revision=HIMAX_TOUCH_REVISION;
 Board.State=NULL;CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_UNSUPPORTED);Board.State=StateUpdate;
 Board.ValidateGpioLease=NULL;CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_UNSUPPORTED);Board.ValidateGpioLease=BoardValidateGpioLease;
 Board.Flags=0;CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_UNSUPPORTED&&Reads==0);Board.Flags=HIMAX_TOUCH_REQUIRED_FLAGS;
 IsReady=FALSE;CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_NOT_READY&&Reads==0);IsReady=TRUE;
 GpioAvailable=FALSE;CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_NOT_FOUND&&Transfers==0);GpioAvailable=TRUE;
 Gpio.AcquirePins=NULL;CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_UNSUPPORTED&&Transfers==0);Gpio.AcquirePins=AcquirePins;
 BoardGpioSnapshotValid=FALSE;CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_DEVICE_ERROR&&Transfers==0&&!GpioLeaseHeld);BoardGpioSnapshotValid=TRUE;
 GpioValidateStatus=EFI_DEVICE_ERROR;CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_DEVICE_ERROR&&Transfers==0&&!GpioLeaseHeld);GpioValidateStatus=EFI_SUCCESS;
 BeforeClosed=Closed;FailEvent=2;CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_OUT_OF_RESOURCES&&!Installed&&Closed==BeforeClosed+1);FailEvent=0;
 /* Failed cancellation during a Start error must keep its callback context alive. */
 FailEvent=Events+3; TimerStatus=EFI_DEVICE_ERROR; BeforeTransfers=Transfers;
 CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_OUT_OF_RESOURCES&&!Installed&&Transfers==BeforeTransfers+14);
 CHECK(Closed==BeforeClosed+1&&LastEventContext!=NULL&&LastNotify==Poll&&mFailedStart==LastEventContext);
 { VOID *Retained=LastEventContext; EFI_EVENT RetainedEvent=LastCreatedEvent; unsigned BeforeSignals=Signals;
   LastNotify(RetainedEvent,Retained); CHECK(Signals==BeforeSignals);
   CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_DEVICE_ERROR&&mFailedStart==Retained);
   TimerStatus=EFI_SUCCESS;
   CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS&&mFailedStart==NULL&&Closed==BeforeClosed+2);
   CHECK(SavedContext[Events-1]==NULL);
 }
 /* An event close failure also retains the failed-start context until a later
  * binding callback closes every callback before closing I2C or freeing it. */
 FailEvent=Events+3;FailCloseEvent=(VOID *)(UINTN)(0x1000+Events+2);BeforeTransfers=Transfers;
 CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_OUT_OF_RESOURCES&&!Installed&&mFailedStart!=NULL&&Transfers==BeforeTransfers+14);
 { UINTN EventBase=Events-3;FailCloseEvent=NULL;
   CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS&&mFailedStart==NULL&&Closed==BeforeClosed+3);
   CHECK(SavedContext[EventBase+1]==NULL&&SavedContext[EventBase+2]==NULL);
 }
 FailEvent=0; TimerStatus=EFI_SUCCESS;
 CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS&&Installed);T=CTX(Installed);
 CHECK(Transfers==56&&Reads==0&&BoardState==HIMAX_TOUCH_READY); /* Three failed-event Starts and the successful Start completed ID plus sense-on. */
 CHECK(T->Mode.AbsoluteMaxX==1400&&T->Mode.AbsoluteMaxY==2240&&T->Mode.AbsoluteMaxZ==255);
 CHECK(GetState(Installed,NULL)==EFI_INVALID_PARAMETER && GetState(NULL,&State)==EFI_INVALID_PARAMETER);
 CHECK(Reset(NULL,FALSE)==EFI_INVALID_PARAMETER && GetState(Installed,&State)==EFI_NOT_READY);
 MakeReport();HasData=FALSE;Tick(T);CHECK(!Reads);HasData=TRUE;
 BeforeGpioAcquires=GpioAcquireCalls;BeforeGpioReleases=GpioReleaseCalls;
 Tick(T);CHECK(Reads==1 && Signals==0 && T->Changed);
 Wait(Installed->WaitForInput,T);CHECK(Signals==1);
 CHECK(!GpioLeaseHeld && !T->PinLeaseHeld && GpioAcquireCalls==BeforeGpioAcquires+1 && GpioReleaseCalls==BeforeGpioReleases+1);
 CHECK(BoardGpioValidateCalls>0 && GpioValidateCalls>=BoardGpioValidateCalls);
 CHECK(GetState(Installed,&State)==EFI_SUCCESS&&State.ActiveButtons==EFI_ABSP_TouchActive&&State.CurrentX==123&&State.CurrentY==456&&State.CurrentZ==80);
 CHECK(GetState(Installed,&State)==EFI_NOT_READY);Tick(T);CHECK(Signals==1 && BoardState==HIMAX_TOUCH_REPORT_VALID);
 Report[3]^=1;Tick(T);Tick(T);CHECK(Signals==1);Tick(T);
 CHECK(Signals==1);Wait(Installed->WaitForInput,T);CHECK(Signals==2);
 CHECK(GetState(Installed,&State)==EFI_SUCCESS&&!State.ActiveButtons&&State.CurrentX==123);
 CHECK(Reset(Installed,TRUE)==EFI_UNSUPPORTED);MakeReport();Tick(T);CHECK(GetState(Installed,&State)==EFI_SUCCESS&&State.ActiveButtons);
 TestDeferredRelease(T);
 /* A report request cannot begin unless the same-token GPIO snapshot passes. */
 GpioAcquireStatus=EFI_ACCESS_DENIED;BeforeTransfers=Transfers;Tick(T);
 CHECK(Transfers==BeforeTransfers && !GpioLeaseHeld && BoardState==HIMAX_TOUCH_REPORT_INVALID);
 GpioAcquireStatus=EFI_SUCCESS;BoardGpioSnapshotValid=FALSE;BeforeTransfers=Transfers;Tick(T);
 CHECK(Transfers==BeforeTransfers && !GpioLeaseHeld && BoardState==HIMAX_TOUCH_REPORT_INVALID);
 BoardGpioSnapshotValid=TRUE;InvalidateGpioAfterTransfer=TRUE;BeforeTransfers=Transfers;Tick(T);
 CHECK(Transfers==BeforeTransfers+1 && !GpioLeaseHeld && !T->PinLeaseHeld && BoardState==HIMAX_TOUCH_REPORT_INVALID);
 InvalidateGpioAfterTransfer=FALSE;BoardGpioSnapshotValid=TRUE;
 /* The board rechecks LiveReady's mapped ranges/infrastructure masks and the
  * GPIO9 EINT route for each lease-backed request, not just at discovery. */
 BoardGcdMapped=FALSE;BeforeTransfers=Transfers;Tick(T);
 CHECK(Transfers==BeforeTransfers&&!GpioLeaseHeld&&BoardState==HIMAX_TOUCH_REPORT_INVALID);
 BoardGcdMapped=TRUE;BoardInfrastructureReady=FALSE;BeforeTransfers=Transfers;Tick(T);
 CHECK(Transfers==BeforeTransfers&&!GpioLeaseHeld&&BoardState==HIMAX_TOUCH_REPORT_INVALID);
 BoardInfrastructureReady=TRUE;BoardEintReady=FALSE;BeforeTransfers=Transfers;Tick(T);
 CHECK(Transfers==BeforeTransfers&&!GpioLeaseHeld&&BoardState==HIMAX_TOUCH_REPORT_INVALID);
 BoardEintReady=TRUE;
 /* A failed release retains the token; the next tick releases it before any
  * later report transaction is allowed. */
 GpioReleaseStatus=EFI_DEVICE_ERROR;BeforeTransfers=Transfers;Tick(T);
 CHECK(Transfers==BeforeTransfers+1 && GpioLeaseHeld && T->PinLeaseHeld && !T->Busy);
 GpioReleaseStatus=EFI_SUCCESS;BeforeTransfers=Transfers;Tick(T);
 CHECK(Transfers==BeforeTransfers && !GpioLeaseHeld && !T->PinLeaseHeld);
 Tick(T);CHECK(Transfers==BeforeTransfers+1 && !GpioLeaseHeld && !T->PinLeaseHeld);
 IsReady=FALSE;Tick(T);CHECK(GetState(Installed,&State)==EFI_SUCCESS&&!State.ActiveButtons);IsReady=TRUE;
 CHECK(Reset(Installed,FALSE)==EFI_SUCCESS&&!T->Changed&&T->Position.Primary==255);
 IoStatus=EFI_TIMEOUT;Tick(T);CHECK(T->Failures==1&&!T->Busy&&BoardState==HIMAX_TOUCH_REPORT_INVALID);IoStatus=EFI_SUCCESS;
 ControlTests(T);
 /* Cancellation failure and event-close failure leave Stop retryable without freeing T. */
 GpioReleaseStatus=EFI_DEVICE_ERROR;BeforeTransfers=Transfers;Tick(T);
 CHECK(Transfers==BeforeTransfers+1 && GpioLeaseHeld && T->PinLeaseHeld);
 TimerStatus=EFI_DEVICE_ERROR;
 CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_DEVICE_ERROR&&Installed&&T->Stopped&&T->PinLeaseHeld&&T->Poll!=NULL);
 GpioReleaseStatus=EFI_SUCCESS;
 CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_DEVICE_ERROR&&Installed&&!T->PinLeaseHeld&&T->Poll!=NULL);
 TimerStatus=EFI_SUCCESS;FailCloseEvent=T->Poll;
 CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_ACCESS_DENIED&&Installed&&T->Poll!=NULL);
 FailCloseEvent=NULL;UninstallStatus=EFI_ACCESS_DENIED;
 CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_ACCESS_DENIED&&Installed&&T->Stopped&&T->Poll==NULL&&T->Pointer.WaitForInput!=NULL);
 UninstallStatus=EFI_SUCCESS;CHECK(Stop(&mBinding,(VOID *)1,1,NULL)==EFI_UNSUPPORTED);
 CloseProtocolStatus=EFI_ACCESS_DENIED;
 CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_ACCESS_DENIED&&Installed&&!T->PinLeaseHeld&&T->Pointer.WaitForInput!=NULL);
 Exit(NULL,T);CHECK(T->Stopped);unsigned Before=Transfers;unsigned BeforeReleases=GpioReleaseCalls;
 Tick(T);CHECK(Transfers==Before && GpioReleaseCalls==BeforeReleases && !T->PinLeaseHeld);
 CloseProtocolStatus=EFI_SUCCESS;
 CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_SUCCESS&&!Installed);
 /* If CloseProtocol fails after AbsolutePointer removal, failures while
  * restoring WaitForInput or the pointer must leave a retryable owner record. */
 CloseProtocolStatus=EFI_ACCESS_DENIED;
 CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS&&Installed);T=CTX(Installed);
 InstallStatus=EFI_OUT_OF_RESOURCES;
 CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_OUT_OF_RESOURCES&&!Installed&&
       mFailedStart==T&&T->I2cOpen&&!T->PointerInstalled);
 CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_ACCESS_DENIED&&mFailedStart==T&&T->I2cOpen);
 CloseProtocolStatus=EFI_SUCCESS;
 CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_SUCCESS&&mFailedStart==NULL&&!Installed);
 InstallStatus=EFI_SUCCESS;
 CloseProtocolStatus=EFI_ACCESS_DENIED;
 CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS&&Installed);T=CTX(Installed);
 FailEvent=Events+1;
 CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_OUT_OF_RESOURCES&&!Installed&&
       mFailedStart==T&&T->I2cOpen&&!T->PointerInstalled);
 CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_ACCESS_DENIED&&mFailedStart==T);
 FailEvent=0;CloseProtocolStatus=EFI_SUCCESS;
 CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_SUCCESS&&mFailedStart==NULL&&!Installed);
 /* If a failed Start cannot release GPIO152, retain both token and I2C open.
  * The next Start retries cleanup before claiming the bus again. */
 GpioReleaseStatus=EFI_DEVICE_ERROR;BeforeTransfers=Transfers;
 CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_DEVICE_ERROR&&!Installed&&GpioLeaseHeld&&mFailedStart!=NULL&&Transfers==BeforeTransfers+14);
 GpioReleaseStatus=EFI_SUCCESS;
 CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS&&!GpioLeaseHeld&&mFailedStart==NULL);
 CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS&&Installed&&!GpioLeaseHeld&&mFailedStart==NULL);
 CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_SUCCESS&&!Installed);
 CloseProtocolStatus=EFI_ACCESS_DENIED;InstallStatus=EFI_OUT_OF_RESOURCES;
 CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_ACCESS_DENIED&&!Installed&&mFailedStart!=NULL&&mFailedStart->I2cOpen);
 CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_ACCESS_DENIED&&mFailedStart!=NULL&&mFailedStart->I2cOpen);
 CloseProtocolStatus=EFI_SUCCESS;InstallStatus=EFI_SUCCESS;
 CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS&&mFailedStart==NULL);
 ChipIdValid=FALSE;BeforeTransfers=Transfers;
 CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_DEVICE_ERROR&&!Installed&&Transfers==BeforeTransfers+25);
 ChipIdValid=TRUE;
 InterfaceBadReadbacks=10;InterfaceReadbacks=0;BeforeTransfers=Transfers;
 { unsigned BeforeSense=SenseOnCommands;
   CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS&&Installed);
   CHECK(InterfaceReadbacks==10&&SenseOnCommands==BeforeSense+1&&Transfers==BeforeTransfers+50);
   CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_SUCCESS&&!Installed);
 }
 InterfaceBadReadbacks=0;InterfaceReadbacks=0;
 IoStatus=EFI_TIMEOUT;BeforeTransfers=Transfers;
 { unsigned BeforeSense=SenseOnCommands;
   CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_DEVICE_ERROR&&!Installed);
   CHECK(SenseOnCommands==BeforeSense&&Transfers==BeforeTransfers+15);
 }
 IoStatus=EFI_SUCCESS;
 ControlExitTests();
 printf("Himax DXE: %u checks passed (GPIO152 leases, coordinated host quiesce/retained resume, failed ownership retries, EBS bus stop, pointer lifetime)\n",Checks);
}
