// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <PiDxe.h>
#include "MediaTekTest.h"
#include <libfdt.h>
#include "../Drivers/GpioPlatformDxe/GpioPlatformDxe.c"
EFI_GUID gFdtTableGuid={.Data1=1},gEfiEventExitBootServicesGuid={.Data1=2},gEfiI2cMasterProtocolGuid={.Data1=3},gMediaTekI2cControllerProtocolGuid={.Data1=4},gHimaxTouchBoardProtocolGuid={.Data1=5},gMediaTekGpioProtocolGuid={.Data1=6},gMtkPanelBoardProtocolGuid={.Data1=7},gEfiAbsolutePointerProtocolGuid={.Data1=8};
UINT32 EFIAPI InterlockedCompareExchange32 (
  volatile UINT32 *Value, UINT32 CompareValue, UINT32 ExchangeValue
  )
{
  UINT32 Previous = CompareValue;
  __atomic_compare_exchange_n (Value, &Previous, ExchangeValue, FALSE,
                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  return Previous;
}
static VOID *Tree;
static UINT64 Bytes;
static unsigned BadHandoff,MapFailure,MapMode,Reads,CacheCleans,EventCalls,EventCloseCalls,PanelGpioReads;
static EFI_STATUS TableStatus,EventStatus;
static unsigned TableLookups;
static UINT32 Masks[5],Domains[5],Handoff[4],TouchHandoff[4],HallHandoff[4],ChargerIrqHandoff;
static UINT32 Mode9,Mode152,Dir9,Dir152,Out152,In152,In9,Ies9,Ies152,Mode0,Ies0;
static UINT8 PanelModes[188];
static BOOLEAN PanelDirections[188],PanelOutputs[188],GpioLeasePins[188];
static unsigned GpioLeaseCount,PanelLeaseAcquires,PanelLeaseReleases,PanelSetterCalls;
static unsigned PanelFailSetterAt,PanelReleaseFailures,PanelInstallCalls,PanelWrites;
static BOOLEAN PanelExitDuringSetter,PanelExitInjected,PanelInstalled;
static BOOLEAN PanelReenterOnSetter,PanelReentryObservedBusy;
static EFI_STATUS PanelReentryStatus;
static unsigned PanelReentryCalls,PanelExitReadsBefore,PanelExitReadsAfter;
static EFI_STATUS PanelInstallStatus,PanelSetterFailure;
static UINT64 PanelReturnedToken;
typedef struct { UINT8 Kind;UINT32 Pin;UINT32 Value; } PANEL_TRACE_ENTRY;
static PANEL_TRACE_ENTRY PanelTrace[512];
static unsigned PanelTraceCount;
static UINT32 ProbeRegister;
static UINT8 FirmwareState;
static BOOLEAN ChipIdValid,ExistingBoard,DuplicateBus;
static unsigned Requests,Opens,Closes,Connects,FrequencyCalls;
static unsigned ConnectFailures;
static BOOLEAN PointerPresent,ConnectNoPointer,ConnectReenter,ConnectExit;
static EFI_STATUS OpenStatus,CloseStatus,IoStatus,InstallStatus,FrequencyStatus;
static EFI_STATUS CloseEventStatus;
static UINTN ActualFrequency;
static BOOLEAN GpioAvailable,GpioExternalBusy,GpioLeaseHeld,ChangePinDuringProbe,BusyPinDuringProbe;
static BOOLEAN ProbeLeaseConflict,ConflictAttempted,PanelAcquireDenied,PanelWriteDenied;
static BOOLEAN ExitDuringOpen,ExitOpenInjected,ExitDuringProbe,ExitInjected,ExitDuringPublish,ExitPublished;
static unsigned GpioBusyCalls,GpioAcquireCalls,GpioReleaseCalls,GpioValidateCalls,GpioBusyFailCall,GpioReleaseFailCall;
static EFI_STATUS GpioLocateStatus,GpioAcquireStatus,GpioReleaseStatus,GpioValidateStatus;
static EFI_TPL CurrentTpl;
static UINT64 GpioLeaseToken,GpioReturnedToken;
static MTK_I2C_CONTROLLER Bus={MTK_I2C_CONTROLLER_REVISION,3,0x11E00000,0x1000,0x10217080,0x80,136000000,0,0,0,EFI_NOT_READY};
static MTK_GPIO_PROTOCOL GpioProtocol;
static EFI_EVENT_NOTIFY Notify;
static EFI_DXE_SERVICES Dxe;
EFI_DXE_SERVICES *gDS=&Dxe;
EFI_STATUS EFIAPI EfiGetSystemConfigurationTable(EFI_GUID *Guid,VOID **Value) {
  TableLookups++;CHECK(Guid==&gFdtTableGuid);*Value=Tree;return TableStatus;
}
static UINT32 PanelModeWord(UINTN Address) {
  UINT32 Group=(UINT32)((Address-0x10005300U)/0x10U),Value=0;
  UINT32 First=Group*8U;
  for(UINT32 I=0;I<8U;I++)if(First+I<ARRAY_SIZE(PanelModes))Value|=((UINT32)PanelModes[First+I]&0xFU)<<(I*4U);
  if(Group==0U)Value=(Value&~0xFU)|(Mode0&0xFU);
  if(Group==1U)Value=(Value&~(0xFU<<4))|(Mode9&(0xFU<<4));
  if(Group==19U)Value=(Value&~0xFU)|(Mode152&0xFU);
  return Value;
}
static UINT32 PanelDirectionWord(UINT32 Bank) {
  UINT32 Value=0,First=Bank*32U;
  for(UINT32 I=0;I<32U;I++)if(First+I<ARRAY_SIZE(PanelDirections) && PanelDirections[First+I])Value|=1U<<I;
  if(Bank==0U)Value=(Value&~((1U<<0)|(1U<<9)))|(Dir9&((1U<<0)|(1U<<9)));
  if(Bank==4U)Value=(Value&~(1U<<24))|(Dir152&(1U<<24));
  return Value;
}
static UINT32 PanelOutputWord(UINT32 Bank) {
  UINT32 Value=0,First=Bank*32U;
  for(UINT32 I=0;I<32U;I++)if(First+I<ARRAY_SIZE(PanelOutputs) && PanelOutputs[First+I])Value|=1U<<I;
  if(Bank==4U)Value=(Value&~(1U<<24))|(Out152&(1U<<24));
  return Value;
}
UINT64 EFIAPI MmioRead64(UINTN A) {
  switch(A) {
    case 0x43F7F000:return BadHandoff==1?0:0x0000000148303754ULL;
    case 0x43F7F048:return (UINT64)(UINTN)Tree+(BadHandoff==2?8:0);
    case 0x43F7F050:return BadHandoff==4?Bytes+1:Bytes;
    case 0x43F7F058:return BadHandoff==3?EFI_UNSUPPORTED:EFI_SUCCESS;
    default:CHECK(0);return 0;
  }
}
UINT32 EFIAPI MmioRead32(UINTN A) {
  Reads++;
  if(A>=0x1000B080 && A<0x1000B094 && !(A&3))return Masks[(A-0x1000B080)/4];
  if(A>=0x1000B400 && A<0x1000B414 && !(A&3))return Domains[(A-0x1000B400)/4];
  if(A>=0x10005300 && A<=0x10005470 && ((A-0x10005300)&0xFU)==0){PanelGpioReads++;return PanelModeWord(A);}
  if(A>=0x10005000 && A<=0x10005050 && ((A-0x10005000)&0xFU)==0){PanelGpioReads++;return PanelDirectionWord((UINT32)((A-0x10005000)/0x10));}
  if(A>=0x10005100 && A<=0x10005150 && ((A-0x10005100)&0xFU)==0){PanelGpioReads++;return PanelOutputWord((UINT32)((A-0x10005100)/0x10));}
  switch(A) {
    case 0x11EA0070:return Ies0;case 0x11EA0080:return Ies9;case 0x11E200D0:return Ies152;
    case 0x10005240:return In152;case 0x10005200:return In9;
  }
  CHECK(0);return 0;
}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V) {
  // Production readiness must never write a GPIO or EINT register.
  CHECK(!(A&3));
  if(A>=0x43F7F0F0 && A<=0x43F7F0FC)HallHandoff[(A-0x43F7F0F0)/4]=V;
  else if(A>=0x43F7F0A0 && A<=0x43F7F0AC)TouchHandoff[(A-0x43F7F0A0)/4]=V;
  else if(A==CHARGER_IRQ_HANDOFF)ChargerIrqHandoff=V;
  else { CHECK(A>=HANDOFF && A<=HANDOFF+12);Handoff[(A-HANDOFF)/4]=V; }
  return V;
}
VOID EFIAPI MemoryFence(VOID) { __sync_synchronize(); }
VOID *EFIAPI WriteBackDataCacheRange(VOID *A,UINTN N) {
  CHECK(N==((UINTN)A==CHARGER_IRQ_HANDOFF?sizeof(UINT32):16));
  if((UINTN)A==HANDOFF)CHECK(Handoff[3]==1);
  else if((UINTN)A==CHARGER_IRQ_HANDOFF)CHECK(ChargerIrqHandoff<=1);
  else if((UINTN)A==0x43F7F0F0)CHECK(HallHandoff[3]==1);
  else { CHECK((UINTN)A==0x43F7F0A0 && TouchHandoff[3]==1); }
  CacheCleans++;return A;
}
static EFI_STATUS EFIAPI Descriptor(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D) {
  CHECK(A==0x10005000 || A==0x1000B000 || A==0x11C30000 || A==0x11D10000 || A==0x11D40000 || A==0x11E20000 || A==0x11E60000 || A==0x11EA0000);
  if(A==MapFailure)return EFI_NOT_FOUND;
  ZeroMem(D,sizeof(*D));D->BaseAddress=A;D->Length=0x1000;
  D->GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo;D->Attributes=EFI_MEMORY_UC;
  switch(MapMode) {
    case 1:D->BaseAddress++;break;
    case 2:D->Length--;break;
    case 3:D->GcdMemoryType=EfiGcdMemoryTypeSystemMemory;break;
    case 4:D->Attributes=EFI_MEMORY_WB;break;
    case 5:D->Attributes|=EFI_MEMORY_RO;break;case 6:D->Attributes|=EFI_MEMORY_RP;break;
    case 7:D->Attributes|=EFI_MEMORY_WC;break;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Function,CONST VOID *Context,CONST EFI_GUID *Guid,EFI_EVENT *Event) {
  EventCalls++;CHECK(Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_NOTIFY && !Context && Guid==&gEfiEventExitBootServicesGuid);
  if(EFI_ERROR(EventStatus))return EventStatus;
  Notify=Function;*Event=(VOID *)1;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TimerCreate(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Function,VOID *Context,EFI_EVENT *Event) {
  CHECK(Type==(EVT_TIMER|EVT_NOTIFY_SIGNAL) && Tpl==TPL_CALLBACK && Function==ObserveTouch && !Context);
  *Event=(VOID *)2;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TimerSet(EFI_EVENT Event,EFI_TIMER_DELAY Type,UINT64 Ticks) {
  CHECK(Event==(VOID *)2 && Type==TimerPeriodic && Ticks==100000);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CloseEvent(EFI_EVENT Event) {
  EventCloseCalls++;CHECK(Event==(VOID *)1);return CloseEventStatus;
}
static EFI_TPL EFIAPI RaiseTpl(EFI_TPL NewTpl) {
  EFI_TPL OldTpl=CurrentTpl;CHECK(NewTpl>OldTpl && NewTpl<=TPL_HIGH_LEVEL);
  CurrentTpl=NewTpl;return OldTpl;
}
static VOID EFIAPI RestoreTpl(EFI_TPL OldTpl) {
  CHECK(OldTpl<CurrentTpl);CurrentTpl=OldTpl;
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *G,VOID *Key,UINTN *Count,EFI_HANDLE **Handles) {
  CHECK(Type==ByProtocol && G==&gEfiI2cMasterProtocolGuid && !Key);
  *Count=DuplicateBus?2:1;*Handles=malloc(*Count*sizeof(EFI_HANDLE));CHECK(*Handles);
  (*Handles)[0]=(VOID *)3;if(DuplicateBus)(*Handles)[1]=(VOID *)4;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ProtocolLocate(EFI_GUID *G,VOID *Registration,VOID **P) {
  CHECK(!Registration && P);
  if(G!=&gMediaTekGpioProtocolGuid)return EFI_NOT_FOUND;
  if(GpioLocateStatus!=EFI_SUCCESS)return GpioLocateStatus;
  if(!GpioAvailable)return EFI_NOT_FOUND;
  *P=&GpioProtocol;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GpioIsBusy(UINT32 Pin,BOOLEAN *Busy) {
  GpioBusyCalls++;CHECK(Pin<ARRAY_SIZE(GpioLeasePins) && Busy && CurrentTpl<=TPL_NOTIFY);
  if(GpioBusyFailCall==GpioBusyCalls)return EFI_DEVICE_ERROR;
  *Busy=(Pin==152 && GpioExternalBusy) || GpioLeasePins[Pin];return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GpioAcquire(CONST UINT32 *Pins,UINTN Count,UINT64 *Token) {
  BOOLEAN Panel=FALSE;
  GpioAcquireCalls++;CHECK(Pins && Count && Count<=ARRAY_SIZE(mPanelPins) && Token && CurrentTpl<=TPL_NOTIFY);
  if(Count==ARRAY_SIZE(mPanelPins)) {
    Panel=TRUE;
    CHECK(CompareMem(Pins,mPanelPins,sizeof(mPanelPins))==0);
  } else CHECK(Count==1 && Pins[0]==152);
  if(GpioAcquireStatus!=EFI_SUCCESS)return GpioAcquireStatus;
  for(UINTN I=0;I<Count;I++)if(Pins[I]>=ARRAY_SIZE(GpioLeasePins) || GpioLeasePins[Pins[I]])return EFI_ACCESS_DENIED;
  if(GpioExternalBusy)return EFI_ACCESS_DENIED;
  GpioLeaseHeld=TRUE;GpioLeaseCount=(unsigned)Count;
  // A corrupted/zero returned touch token must not become the token that
  // actually owns the pins: validation and release must reject that token.
  GpioLeaseToken=Panel?0x9001:0x152;
  *Token=Panel?PanelReturnedToken:GpioReturnedToken;
  for(UINTN I=0;I<Count;I++)GpioLeasePins[Pins[I]]=TRUE;
  if(Panel)PanelLeaseAcquires++;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GpioRelease(UINT64 Token) {
  GpioReleaseCalls++;CHECK(CurrentTpl<=TPL_NOTIFY);
  if(Token!=GpioLeaseToken || !GpioLeaseHeld)return EFI_INVALID_PARAMETER;
  if(GpioLeaseCount==ARRAY_SIZE(mPanelPins))PanelLeaseReleases++;
  if(GpioReleaseStatus!=EFI_SUCCESS || GpioReleaseFailCall==GpioReleaseCalls)return EFI_DEVICE_ERROR;
  for(UINTN I=0;I<ARRAY_SIZE(GpioLeasePins);I++)GpioLeasePins[I]=FALSE;
  GpioLeaseHeld=FALSE;GpioLeaseCount=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GpioValidate(UINT64 Token,UINT32 Pin) {
  GpioValidateCalls++;CHECK(Pin<ARRAY_SIZE(GpioLeasePins) && CurrentTpl<=TPL_NOTIFY);
  if(GpioValidateStatus!=EFI_SUCCESS)return GpioValidateStatus;
  return GpioLeaseHeld && GpioLeasePins[Pin] && Token==GpioLeaseToken?EFI_SUCCESS:EFI_ACCESS_DENIED;
}
static VOID PanelTraceAdd(UINT8 Kind,UINT32 Pin,UINT32 Value) {
  CHECK(PanelTraceCount<ARRAY_SIZE(PanelTrace));PanelTrace[PanelTraceCount++]=(PANEL_TRACE_ENTRY){Kind,Pin,Value};
}
static EFI_STATUS PanelSetCheck(UINT64 Token,UINT32 Pin) {
  CHECK(CurrentTpl<=TPL_NOTIFY && Pin<ARRAY_SIZE(GpioLeasePins));
  return GpioLeaseHeld && GpioLeaseCount==ARRAY_SIZE(mPanelPins) &&
         GpioLeasePins[Pin] && Token==GpioLeaseToken?EFI_SUCCESS:EFI_ACCESS_DENIED;
}
static EFI_STATUS EFIAPI GpioSetModeLease(UINT64 Token,UINT32 Pin,UINT32 Mode) {
  EFI_STATUS Status=PanelSetCheck(Token,Pin);if(EFI_ERROR(Status))return Status;
  PanelSetterCalls++;PanelTraceAdd('M',Pin,Mode);
  if(PanelReenterOnSetter) {
    PanelReenterOnSetter=FALSE;PanelReentryCalls++;
    PanelReentryObservedBusy=(PanelCallBusy==1U);
    PanelReentryStatus=PanelBoard.RunInit(&PanelBoard);
  }
  if(PanelFailSetterAt==PanelSetterCalls)return PanelSetterFailure;
  PanelModes[Pin]=(UINT8)Mode;if(Pin==152)Mode152=Mode;PanelWrites++;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GpioSetDirLease(UINT64 Token,UINT32 Pin,BOOLEAN Direction) {
  EFI_STATUS Status=PanelSetCheck(Token,Pin);if(EFI_ERROR(Status))return Status;
  PanelSetterCalls++;PanelTraceAdd('D',Pin,Direction);
  if(PanelFailSetterAt==PanelSetterCalls)return PanelSetterFailure;
  PanelDirections[Pin]=Direction;PanelWrites++;
  if(Pin==152)Dir152=Direction?(1U<<24):0;
  if(Pin==9)Dir9=Direction?(1U<<9):0;
  return EFI_SUCCESS;
}
static VOID PanelMaybeExit(VOID) {
  if(PanelExitDuringSetter && !PanelExitInjected) {
    PanelExitInjected=TRUE;
    PanelExitReadsBefore=PanelGpioReads;
    ExportToAcpi(NULL,NULL);
    PanelExitReadsAfter=PanelGpioReads;
  }
}
static EFI_STATUS EFIAPI GpioSetStateLease(UINT64 Token,UINT32 Pin,BOOLEAN State) {
  EFI_STATUS Status=PanelSetCheck(Token,Pin);if(EFI_ERROR(Status))return Status;
  PanelSetterCalls++;PanelTraceAdd('V',Pin,State);
  if(PanelFailSetterAt==PanelSetterCalls)return PanelSetterFailure;
  PanelOutputs[Pin]=State;PanelWrites++;
  if(Pin==152)Out152=State?(1U<<24):0;
  PanelMaybeExit();
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GpioLegacySetState(UINT32 Pin,BOOLEAN State) {
  CHECK(Pin==152 && CurrentTpl<=TPL_NOTIFY);
  if(GpioLeaseHeld)return EFI_ACCESS_DENIED;
  if(State)Out152|=1U<<24;else Out152&=~(1U<<24);
  return EFI_SUCCESS;
}
static VOID InitGpioProtocol(VOID) {
  ZeroMem(&GpioProtocol,sizeof(GpioProtocol));GpioProtocol.IsBusy=GpioIsBusy;
  GpioProtocol.SetState=GpioLegacySetState;
  GpioProtocol.AcquirePins=GpioAcquire;GpioProtocol.ReleasePins=GpioRelease;
  GpioProtocol.ValidateLease=GpioValidate;GpioProtocol.SetModeWithLease=GpioSetModeLease;
  GpioProtocol.SetDirWithLease=GpioSetDirLease;GpioProtocol.SetStateWithLease=GpioSetStateLease;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **P) {
  CHECK(H==(VOID *)3 || H==(VOID *)4);
  if(G==&gEfiAbsolutePointerProtocolGuid) { *P=PointerPresent?(VOID *)&TouchBoard:NULL;return PointerPresent?EFI_SUCCESS:EFI_NOT_FOUND; }
  if(G==&gMediaTekI2cControllerProtocolGuid) { *P=&Bus;return EFI_SUCCESS; }
  CHECK(G==&gHimaxTouchBoardProtocolGuid);
  if(ExistingBoard) { *P=&TouchBoard;return EFI_SUCCESS; }
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Frequency(CONST EFI_I2C_MASTER_PROTOCOL *M,UINTN *Hz) {
  (VOID)M;
  FrequencyCalls++;CHECK(CurrentTpl==TPL_CALLBACK && *Hz==400000 && ProbePinLeaseHeld &&
                         ProbePinLeaseOwner==&GpioProtocol && GpioLeaseHeld);
  *Hz=ActualFrequency;return FrequencyStatus;
}
static EFI_STATUS EFIAPI Request(CONST EFI_I2C_MASTER_PROTOCOL *M,UINTN Address,EFI_I2C_REQUEST_PACKET *P,EFI_EVENT E,EFI_STATUS *Status) {
  (VOID)M;
  Requests++;CHECK(ProbeOwned==(VOID *)3 && Address==0x48 && !E && !Status);
  CHECK(ProbePinLeaseHeld && ProbePinLeaseOwner==&GpioProtocol &&
        ProbePinLeaseToken==GpioLeaseToken && GpioLeaseHeld);
  if(ProbeLeaseConflict && !ConflictAttempted) {
    UINT32 Pin=152;UINT64 OtherToken=0;
    PanelAcquireDenied=GpioProtocol.AcquirePins(&Pin,1,&OtherToken)==EFI_ACCESS_DENIED && !OtherToken;
    PanelWriteDenied=GpioProtocol.SetState(Pin,FALSE)==EFI_ACCESS_DENIED;
    ConflictAttempted=TRUE;
  }
  if(ExitDuringProbe && !ExitInjected) {
    EFI_TPL OldTpl=CurrentTpl;CurrentTpl=TPL_NOTIFY;
    ExportToAcpi(NULL,NULL);CurrentTpl=OldTpl;ExitInjected=TRUE;
  }
  CHECK(P->OperationCount==1 || P->OperationCount==2);
  UINT8 *W=P->Operation[0].Buffer;CHECK(W && !P->Operation[0].Flags);
  if(EFI_ERROR(IoStatus))return IoStatus;
  if(P->OperationCount==2) {
    CHECK(P->Operation[0].LengthInBytes==1 && P->Operation[1].Flags==I2C_FLAG_READ);
    CHECK(W[0]==0x08 && P->Operation[1].LengthInBytes==4);
    if(ProbeRegister==HM_CHIP_ID_REGISTER) {
      const UINT8 Id[]={0,0x2a,0x13,0x83};
      CopyMem(P->Operation[1].Buffer,Id,4);
      if(!ChipIdValid)P->Operation[1].Buffer[3]=0;
    } else {
      CHECK(ProbeRegister==0x900000a8U);
      ZeroMem(P->Operation[1].Buffer,4);
      P->Operation[1].Buffer[0]=FirmwareState;
      if(ChangePinDuringProbe)Out152=0;
      if(BusyPinDuringProbe) {
        UINT32 Pin=152;UINT64 OtherToken=0;
        CHECK(GpioProtocol.AcquirePins(&Pin,1,&OtherToken)==EFI_ACCESS_DENIED && !OtherToken);
      }
    }
  } else {
    CHECK((W[0]==0x13 && W[1]==0x31 && P->Operation[0].LengthInBytes==2) ||
          (W[0]==0x0d && W[1]==0x10 && P->Operation[0].LengthInBytes==2) ||
          (W[0]==0 && P->Operation[0].LengthInBytes==5) ||
          (W[0]==0x0c && W[1]==0 && P->Operation[0].LengthInBytes==2));
    if(W[0]==0)
      ProbeRegister=(UINT32)W[1]|((UINT32)W[2]<<8)|((UINT32)W[3]<<16)|((UINT32)W[4]<<24);
  }
  return EFI_SUCCESS;
}
static EFI_I2C_MASTER_PROTOCOL Master={.SetBusFrequency=Frequency,.StartRequest=Request};
static EFI_STATUS EFIAPI Open(EFI_HANDLE H,EFI_GUID *G,VOID **P,EFI_HANDLE Agent,EFI_HANDLE Controller,UINT32 Flags) {
  Opens++;CHECK(H==(VOID *)3 && Controller==H && Agent==Image && G==&gEfiI2cMasterProtocolGuid && Flags==EFI_OPEN_PROTOCOL_BY_DRIVER);
  if(ExitDuringOpen && !ExitOpenInjected) {
    EFI_TPL OldTpl=CurrentTpl;CurrentTpl=TPL_NOTIFY;
    ExportToAcpi(NULL,NULL);CurrentTpl=OldTpl;ExitOpenInjected=TRUE;
  }
  if(!EFI_ERROR(OpenStatus)){*P=&Master;}
  return OpenStatus;
}
static EFI_STATUS EFIAPI Close(EFI_HANDLE H,EFI_GUID *G,EFI_HANDLE Agent,EFI_HANDLE Controller) {
  Closes++;CHECK(H==(VOID *)3 && Controller==H && Agent==Image && G==&gEfiI2cMasterProtocolGuid);
  CHECK(ProbePinLeaseHeld && GpioLeaseHeld && ProbePinLeaseOwner==&GpioProtocol);
  return CloseStatus;
}
static EFI_STATUS EFIAPI PublishTouch(EFI_HANDLE *H,...) {
  va_list Args;EFI_GUID *Guid;VOID *Interface;
  va_start(Args,H);CHECK(CurrentTpl==TPL_CALLBACK);
  Guid=va_arg(Args,EFI_GUID *);Interface=va_arg(Args,VOID *);
  if(Guid==&gMtkPanelBoardProtocolGuid) {
    CHECK(!PanelInstalled && *H==NULL && Interface==&PanelBoard && !ProbeOwned &&
          !GpioLeaseHeld && !PanelWrites && !PanelTraceCount);
    CHECK(PanelBoard.Revision==MTK_PANEL_BOARD_PROTOCOL_REVISION && PanelBoard.RunInit==PanelRunInit &&
          PanelBoard.RunSuspend==PanelRunSuspend &&
          PanelBoard.RunKernelPrepare==PanelRunKernelPrepare &&
          PanelBoard.RunKernelEnable==PanelRunKernelEnable &&
          PanelBoard.RunKernelUnprepare==PanelRunKernelUnprepare && va_arg(Args,VOID *)==NULL);
    PanelInstallCalls++;
    if(EFI_ERROR(PanelInstallStatus)){va_end(Args);return PanelInstallStatus;}
    PanelInstalled=TRUE;*H=(EFI_HANDLE)0x30;va_end(Args);return EFI_SUCCESS;
  }
  CHECK(Guid==&gHimaxTouchBoardProtocolGuid && *H==(VOID *)3 && !ProbeOwned);
  CHECK(!ProbePinLeaseHeld && !TouchPinCleanupPending && !GpioLeaseHeld);
  CHECK(Interface==&TouchBoard && va_arg(Args,VOID *)==NULL);va_end(Args);
  CHECK(TouchReady(&TouchBoard));Installs++;
  if(ExitDuringPublish && !ExitPublished) {
    EFI_TPL OldTpl=CurrentTpl;CurrentTpl=TPL_NOTIFY;
    ExportToAcpi(NULL,NULL);CurrentTpl=OldTpl;ExitPublished=TRUE;
  }
  return InstallStatus;
}
static EFI_STATUS EFIAPI Connect(EFI_HANDLE H,EFI_HANDLE *Drivers,EFI_DEVICE_PATH_PROTOCOL *Path,BOOLEAN Recursive) {
  Connects++;CHECK(CurrentTpl==TPL_CALLBACK && H==(VOID *)3 && !Drivers && !Path && !Recursive && !ProbeOwned && TouchHandle &&
                    !ProbePinLeaseHeld && !GpioLeaseHeld);
  if(ConnectReenter)ObserveTouch(NULL,NULL);
  if(ConnectExit) { Exiting=TRUE;return EFI_ABORTED; }
  if(ConnectFailures) { ConnectFailures--;return EFI_DEVICE_ERROR; }
  if(!ConnectNoPointer)PointerPresent=TRUE;
  return EFI_SUCCESS;
}
static VOID Reset(VOID) {
  for(unsigned I=0;I<5;I++)Masks[I]=Domains[I]=MAX_UINT32;
  for(unsigned I=0;I<4;I++)Handoff[I]=MAX_UINT32;
  BadHandoff=MapFailure=MapMode=Reads=CacheCleans=EventCloseCalls=PanelGpioReads=0;TableLookups=0;TableStatus=EFI_SUCCESS;
  Exiting=FALSE;TouchHandle=ProbeOwned=NULL;TouchFdt=NULL;TouchBytes=0;TouchState=ProbeAttempts=0;
  PanelCoreInitialized=FALSE;ZeroMem(&PanelCore,sizeof(PanelCore));PanelCallBusy=0;
  PanelPinOwner=NULL;PanelLeaseToken=0;PanelLeaseHeld=PanelLeaseUncertain=FALSE;
  PanelLeaseOperation=MtkPanelOperationNone;PanelFdt=NULL;PanelFdtBytes=0;PanelHandle=NULL;
  PanelInstalled=FALSE;PanelInstallCalls=PanelLeaseAcquires=PanelLeaseReleases=0;
  PanelSetterCalls=PanelFailSetterAt=PanelReleaseFailures=PanelWrites=PanelTraceCount=0;
  PanelExitDuringSetter=PanelExitInjected=FALSE;PanelInstallStatus=EFI_SUCCESS;PanelSetterFailure=EFI_DEVICE_ERROR;
  PanelReenterOnSetter=PanelReentryObservedBusy=FALSE;PanelReentryStatus=EFI_SUCCESS;
  PanelReentryCalls=PanelExitReadsBefore=PanelExitReadsAfter=0;
  PanelReturnedToken=0x9001;ZeroMem(PanelModes,sizeof(PanelModes));
  ZeroMem(PanelDirections,sizeof(PanelDirections));ZeroMem(PanelOutputs,sizeof(PanelOutputs));
  ZeroMem(GpioLeasePins,sizeof(GpioLeasePins));GpioLeaseCount=0;ZeroMem(PanelTrace,sizeof(PanelTrace));
  TimeNs=0;
  TouchPinLeaseOwner=NULL;TouchPinCleanupToken=0;TouchPinCleanupPending=FALSE;
  ProbePinLeaseOwner=NULL;ProbePinLeaseToken=0;ProbePinLeaseHeld=FALSE;
  Mode0=0;Ies0=1U<<20;
  Mode9=Mode152=Dir9=0;In9=1U<<9;Dir152=Out152=In152=1U<<24;Ies9=1;Ies152=1U<<22;
  ProbeRegister=0;FirmwareState=0x05;ChipIdValid=TRUE;ExistingBoard=DuplicateBus=FALSE;
  GpioAvailable=TRUE;GpioExternalBusy=GpioLeaseHeld=ChangePinDuringProbe=BusyPinDuringProbe=FALSE;
  ProbeLeaseConflict=ConflictAttempted=PanelAcquireDenied=PanelWriteDenied=FALSE;
  ExitDuringOpen=ExitOpenInjected=ExitDuringProbe=ExitInjected=ExitDuringPublish=ExitPublished=FALSE;
  GpioBusyCalls=GpioAcquireCalls=GpioReleaseCalls=GpioValidateCalls=GpioBusyFailCall=GpioReleaseFailCall=0;
  GpioLocateStatus=GpioAcquireStatus=GpioReleaseStatus=GpioValidateStatus=EFI_SUCCESS;
  GpioLeaseToken=0;GpioReturnedToken=0x152;CurrentTpl=TPL_CALLBACK;InitGpioProtocol();
  ExitEvent=TouchTimer=NULL;
  Requests=Opens=Closes=Connects=FrequencyCalls=Installs=0;
  ConnectFailures=0;PointerPresent=ConnectNoPointer=ConnectReenter=ConnectExit=FALSE;
  TouchConnectAttempts=TouchConnectDelay=TouchConnectCountdown=0;TouchConnectBusy=FALSE;
  OpenStatus=CloseStatus=IoStatus=InstallStatus=FrequencyStatus=CloseEventStatus=EFI_SUCCESS;ActualFrequency=400000;
  ZeroMem(TouchHandoff,sizeof(TouchHandoff));memset(HallHandoff,0xff,sizeof(HallHandoff));
  ChargerIrqHandoff=MAX_UINT32;
}
static VOID Export(BOOLEAN Ready) {
  EFI_TPL OldTpl=CurrentTpl;CurrentTpl=TPL_NOTIFY;ExportToAcpi(NULL,NULL);CurrentTpl=OldTpl;
  CHECK(CacheCleans && Handoff[0]==Ready && Handoff[3]==1);
  CHECK(Handoff[1]==(Ready?188:0) && Handoff[2]==(Ready?126:0));
}
static VOID StartPanelBoard(VOID) {
  EventStatus=PanelInstallStatus=CloseEventStatus=EFI_SUCCESS;
  CHECK(T70GpioPlatformEntry((EFI_HANDLE)0x99,NULL)==EFI_SUCCESS);
  CHECK(PanelInstalled && PanelHandle==(EFI_HANDLE)0x30 && PanelCoreInitialized);
  CHECK(!GpioAcquireCalls && !GpioLeaseHeld && !PanelWrites && !PanelTraceCount);
}
static VOID TestPanelBoard(VOID *Original, UINTN Size) {
  // A live touch session owns the reset policy even between its short pin
  // leases. Only an explicit stopped input session permits the panel reset.
  for(UINT32 State=HIMAX_TOUCH_STARTING;State<=HIMAX_TOUCH_READY;State++) {
    Reset();StartPanelBoard();TouchHandle=(VOID *)3;TouchState=State;
    CHECK(PanelBoard.RunKernelPrepare(&PanelBoard)==EFI_NOT_READY);
    CHECK(!PanelWrites && !GpioAcquireCalls && !PanelLeaseHeld && !PanelCallBusy);
    TouchState=HIMAX_TOUCH_STOPPED;
    CHECK(PanelBoard.RunKernelPrepare(&PanelBoard)==EFI_SUCCESS && PanelWrites==24);
  }
  for(unsigned Kind=0;Kind<3;Kind++) {
    Reset();
    if(Kind==0)PanelCallBusy=1;
    if(Kind==1)PanelLeaseHeld=TRUE;
    if(Kind==2)PanelLeaseUncertain=TRUE;
    ObserveTouch(NULL,NULL);
    CHECK(!TouchHandle && !Requests && !Opens && !GpioAcquireCalls);
  }
  // Protocol publication is passive: no rail/reset/backlight GPIO work occurs
  // until the actual display consumer calls RunInit.
  Reset();StartPanelBoard();
  CHECK(PanelBoard.RunInit(NULL)==EFI_INVALID_PARAMETER);
  CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_SUCCESS);
  CHECK(!GpioLeaseHeld && PanelLeaseAcquires==1 && PanelLeaseReleases==1 &&
        PanelSetterCalls==27 && PanelWrites==27 && PanelTraceCount==27);
  CHECK(PanelTrace[0].Kind=='M' && PanelTrace[0].Pin==43 && PanelTrace[0].Value==0);
  CHECK(PanelTrace[1].Kind=='D' && PanelTrace[1].Pin==43 && PanelTrace[1].Value==1);
  CHECK(PanelTrace[2].Kind=='V' && PanelTrace[2].Pin==43 && PanelTrace[2].Value==1);
  CHECK(TimeNs==335000000ULL && PanelCore.State==MtkPanelStateLkInitComplete);
  CHECK(PanelDirections[43] && !PanelOutputs[43] && PanelOutputs[3] &&
        PanelOutputs[155] && PanelOutputs[49] && PanelOutputs[152] &&
        PanelOutputs[153] && PanelOutputs[36] && PanelOutputs[15]);
  {
    unsigned Writes=PanelWrites,Acquires=PanelLeaseAcquires;
    CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_SUCCESS);
    CHECK(PanelWrites==Writes && PanelLeaseAcquires==Acquires);
  }
  CHECK(PanelBoard.RunSuspend(&PanelBoard)==EFI_SUCCESS);
  CHECK(!GpioLeaseHeld && PanelLeaseAcquires==2 && PanelLeaseReleases==2 &&
        !PanelOutputs[15] && !PanelOutputs[37] && !PanelOutputs[43] &&
        !PanelOutputs[153] && !PanelOutputs[49] && !PanelOutputs[3]);
  CHECK(PanelOutputs[152] && PanelOutputs[155] && PanelOutputs[36]);
  {
    unsigned Writes=PanelWrites,Acquires=PanelLeaseAcquires;
    CHECK(PanelBoard.RunSuspend(&PanelBoard)==EFI_SUCCESS);
    CHECK(PanelWrites==Writes && PanelLeaseAcquires==Acquires);
    CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_NOT_READY);
    CHECK(PanelWrites==Writes && PanelLeaseAcquires==Acquires);
  }

  // The shipping kernel GPIO lifecycle is reachable through the actual
  // board protocol, while its display-stream preconditions remain the caller's
  // responsibility. Repeat real rail cycles and preserve shared touch reset.
  Reset();StartPanelBoard();
  CHECK(PanelBoard.RunKernelPrepare(NULL)==EFI_INVALID_PARAMETER);
  CHECK(PanelBoard.RunKernelEnable(&PanelBoard)==EFI_NOT_READY);
  CHECK(PanelBoard.RunKernelUnprepare(&PanelBoard)==EFI_NOT_READY);
  CHECK(!PanelWrites && !PanelLeaseAcquires);
  for(unsigned Cycle=0;Cycle<3;Cycle++) {
    unsigned TraceStart=PanelTraceCount;
    CHECK(PanelBoard.RunKernelPrepare(&PanelBoard)==EFI_SUCCESS);
    CHECK(PanelCore.State==MtkPanelStateKernelPrepared && !PanelLeaseHeld &&
          PanelLeaseAcquires==Cycle*3+1 && PanelLeaseReleases==Cycle*3+1 &&
          PanelWrites==Cycle*51+24 && TimeNs==Cycle*1097000000ULL+2000000ULL);
    // Output latch is preloaded before switching each pin to output.
    CHECK(PanelTrace[TraceStart].Kind=='M' && PanelTrace[TraceStart].Pin==43);
    CHECK(PanelTrace[TraceStart+1].Kind=='V' && PanelTrace[TraceStart+1].Value==1);
    CHECK(PanelTrace[TraceStart+2].Kind=='D' && PanelTrace[TraceStart+2].Value==1);
    CHECK(!PanelOutputs[43] && PanelOutputs[3] && PanelOutputs[155] &&
          PanelOutputs[49] && PanelOutputs[153] && PanelOutputs[36] &&
          PanelOutputs[15] && Out152==(1U<<24));
    unsigned Before=PanelWrites,Acquires=PanelLeaseAcquires;
    CHECK(PanelBoard.RunKernelPrepare(&PanelBoard)==EFI_SUCCESS &&
          PanelWrites==Before && PanelLeaseAcquires==Acquires);
    CHECK(PanelBoard.RunKernelEnable(&PanelBoard)==EFI_SUCCESS);
    CHECK(PanelCore.State==MtkPanelStateKernelEnabled && !PanelLeaseHeld &&
          PanelOutputs[37] && !PanelOutputs[43] && PanelWrites==Cycle*51+27 &&
          TimeNs==Cycle*1097000000ULL+502000000ULL);
    Before=PanelWrites;Acquires=PanelLeaseAcquires;
    CHECK(PanelBoard.RunKernelEnable(&PanelBoard)==EFI_SUCCESS &&
          PanelWrites==Before && PanelLeaseAcquires==Acquires);
    CHECK(PanelBoard.RunKernelPrepare(&PanelBoard)==EFI_NOT_READY &&
          PanelWrites==Before && PanelLeaseAcquires==Acquires);
    CHECK(PanelBoard.RunKernelUnprepare(&PanelBoard)==EFI_SUCCESS);
    CHECK(PanelCore.State==MtkPanelStateKernelUnprepared && !PanelLeaseHeld &&
          PanelLeaseAcquires==(Cycle+1)*3 && PanelLeaseReleases==(Cycle+1)*3 &&
          PanelWrites==(Cycle+1)*51 && TimeNs==(Cycle+1)*1097000000ULL);
    const UINT32 OffPins[]={37,15,43,36,153,49,155,3};
    for(unsigned I=0;I<ARRAY_SIZE(OffPins);I++) {
      CHECK(PanelDirections[OffPins[I]] && !PanelOutputs[OffPins[I]]);
    }
    CHECK(Out152==(1U<<24));
    Before=PanelWrites;Acquires=PanelLeaseAcquires;
    CHECK(PanelBoard.RunKernelUnprepare(&PanelBoard)==EFI_SUCCESS &&
          PanelWrites==Before && PanelLeaseAcquires==Acquires);
    CHECK(PanelBoard.RunKernelEnable(&PanelBoard)==EFI_NOT_READY &&
          PanelWrites==Before && PanelLeaseAcquires==Acquires);
  }
  for(unsigned I=0;I<PanelTraceCount;I++)CHECK(PanelTrace[I].Pin!=152);

  {
    MTK_PANEL_BOARD_RUN_SEQUENCE Run[]={
      PanelBoard.RunKernelPrepare,PanelBoard.RunKernelEnable,PanelBoard.RunKernelUnprepare
    };
    MTK_PANEL_OPERATION Operation[]={
      MtkPanelOperationKernelPrepare,MtkPanelOperationKernelEnable,MtkPanelOperationKernelUnprepare
    };
    MTK_PANEL_STATE Complete[]={
      MtkPanelStateKernelPrepared,MtkPanelStateKernelEnabled,MtkPanelStateKernelUnprepared
    };
    for(unsigned Stage=0;Stage<ARRAY_SIZE(Run);Stage++) {
      Reset();StartPanelBoard();
      for(unsigned I=0;I<Stage;I++)CHECK(Run[I](&PanelBoard)==EFI_SUCCESS);
      unsigned Before=PanelWrites,Acquires=PanelLeaseAcquires,Releases=PanelLeaseReleases;
      PanelFailSetterAt=PanelSetterCalls+2;
      CHECK(Run[Stage](&PanelBoard)==EFI_DEVICE_ERROR);
      CHECK(PanelLeaseHeld && GpioLeaseHeld && GpioLeaseCount==ARRAY_SIZE(mPanelPins) &&
            PanelLeaseOperation==Operation[Stage] && PanelCore.PendingOperation==Operation[Stage] &&
            PanelCore.ActionAttempted==1 && PanelWrites==Before+1 &&
            PanelLeaseAcquires==Acquires+1 && PanelLeaseReleases==Releases);
      CHECK(Run[(Stage+1)%3](&PanelBoard)==EFI_NOT_READY && PanelWrites==Before+1);
      PanelFailSetterAt=0;
      CHECK(Run[Stage](&PanelBoard)==EFI_SUCCESS);
      CHECK(!PanelLeaseHeld && !GpioLeaseHeld && PanelCore.State==Complete[Stage] &&
            PanelLeaseAcquires==Acquires+1 && PanelLeaseReleases==Releases+1);

      // Successful GPIO work with a failed lease release retries release only.
      Reset();StartPanelBoard();
      for(unsigned I=0;I<Stage;I++)CHECK(Run[I](&PanelBoard)==EFI_SUCCESS);
      GpioReleaseFailCall=GpioReleaseCalls+1;
      CHECK(Run[Stage](&PanelBoard)==EFI_DEVICE_ERROR);
      CHECK(PanelLeaseHeld && PanelCore.State==Complete[Stage]);
      Before=PanelWrites;Acquires=PanelLeaseAcquires;Releases=PanelLeaseReleases;
      CHECK(Run[Stage](&PanelBoard)==EFI_SUCCESS);
      CHECK(!PanelLeaseHeld && PanelWrites==Before && PanelLeaseAcquires==Acquires &&
            PanelLeaseReleases==Releases+1);

      // EBS at any of the three operations stops before the next GPIO read.
      Reset();StartPanelBoard();
      for(unsigned I=0;I<Stage;I++)CHECK(Run[I](&PanelBoard)==EFI_SUCCESS);
      Before=PanelWrites;Acquires=PanelLeaseAcquires;Releases=PanelLeaseReleases;
      PanelExitDuringSetter=TRUE;
      CHECK(Run[Stage](&PanelBoard)==EFI_NOT_READY);
      CHECK(Exiting && PanelExitInjected && PanelLeaseHeld && GpioLeaseHeld &&
            PanelWrites==Before+2 && PanelLeaseAcquires==Acquires+1 &&
            PanelLeaseReleases==Releases && !TouchHandoff[0]);
      Before=PanelWrites;unsigned ReadsBefore=PanelGpioReads,ReleaseCalls=GpioReleaseCalls;
      CHECK(Run[Stage](&PanelBoard)==EFI_ABORTED);
      CHECK(PanelWrites==Before && PanelGpioReads==ReadsBefore && GpioReleaseCalls==ReleaseCalls);
    }
  }

  // The EDK2 compare-exchange contract is (value, compare, exchange). A
  // nested callback must observe the outer call's busy value and return
  // without acquiring/releasing the outer guard or issuing extra GPIO work.
  Reset();StartPanelBoard();PanelReenterOnSetter=TRUE;
  CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_SUCCESS);
  CHECK(PanelReentryCalls==1 && PanelReentryObservedBusy &&
        PanelReentryStatus==EFI_NOT_READY && PanelCallBusy==0U &&
        PanelWrites==27 && PanelLeaseAcquires==1 && PanelLeaseReleases==1);

  // A current touch owner of GPIO152 makes the all-nine-pin claim fail as a
  // unit; the panel does not make partial claims or write any pin.
  Reset();StartPanelBoard();
  {
    UINT32 Pin=152;UINT64 Token=0;
    CHECK(GpioProtocol.AcquirePins(&Pin,1,&Token)==EFI_SUCCESS && Token==GpioLeaseToken);
    CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_NOT_READY);
    CHECK(GpioLeaseCount==1 && GpioLeasePins[152] && !GpioLeasePins[43] &&
          !PanelLeaseAcquires && !PanelWrites);
    CHECK(GpioProtocol.ReleasePins(Token)==EFI_SUCCESS && !GpioLeaseHeld);
  }

  // A failed value write leaves the exact operation, action index, and one
  // non-reused nine-pin lease in place. The same operation resumes in place;
  // touch GPIO152 acquisition and legacy writes stay blocked meanwhile.
  Reset();StartPanelBoard();PanelFailSetterAt=3;
  CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_DEVICE_ERROR);
  CHECK(PanelLeaseHeld && GpioLeaseHeld && GpioLeaseCount==ARRAY_SIZE(mPanelPins) &&
        PanelLeaseOperation==MtkPanelOperationLkInit &&
        PanelCore.PendingOperation==MtkPanelOperationLkInit &&
        PanelCore.ActionIndex==2 && PanelCore.ActionAttempted==1 && PanelWrites==2);
  CHECK(PanelBoard.RunSuspend(&PanelBoard)==EFI_NOT_READY && PanelWrites==2);
  {
    UINT32 Pin=152;UINT64 Token=0;
    CHECK(GpioProtocol.AcquirePins(&Pin,1,&Token)==EFI_ACCESS_DENIED && !Token);
    CHECK(GpioProtocol.SetState(Pin,FALSE)==EFI_ACCESS_DENIED);
  }
  PanelFailSetterAt=0;
  CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_SUCCESS);
  CHECK(!PanelLeaseHeld && PanelLeaseAcquires==1 && PanelLeaseReleases==1 &&
        PanelCore.State==MtkPanelStateLkInitComplete && PanelOutputs[43]==FALSE);

  // If the sequence completed but pin-lease release failed, the next call
  // retries release only; it neither reacquires pins nor replays GPIO actions.
  Reset();StartPanelBoard();GpioReleaseFailCall=1;
  CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_DEVICE_ERROR);
  CHECK(PanelCore.State==MtkPanelStateLkInitComplete && PanelLeaseHeld &&
        PanelLeaseAcquires==1 && PanelLeaseReleases==1 && PanelWrites==27);
  {
    unsigned Writes=PanelWrites,Acquires=PanelLeaseAcquires;
    CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_SUCCESS);
    CHECK(!PanelLeaseHeld && PanelWrites==Writes && PanelLeaseAcquires==Acquires &&
          PanelLeaseReleases==2);
  }

  // The callback may be interrupted by EBS after a setter. It stops before a
  // readback/next action, retains the lease for safety, and future calls abort.
  // The EBS callback itself reads two GPIO registers per IRQ (Hall and charger).
  Reset();StartPanelBoard();PanelExitDuringSetter=TRUE;
  CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_NOT_READY);
  CHECK(PanelExitInjected && Exiting && PanelLeaseHeld && GpioLeaseHeld &&
        PanelWrites==3 && GpioReleaseCalls==0 &&
        PanelExitReadsAfter-PanelExitReadsBefore==4 && ChargerIrqHandoff==1 && !TouchHandoff[0]);
  {
    unsigned Writes=PanelWrites,ReadsBefore=PanelGpioReads,Releases=GpioReleaseCalls;
    CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_ABORTED);
    CHECK(PanelWrites==Writes && PanelGpioReads==ReadsBefore && GpioReleaseCalls==Releases);
  }

  // The FDT and GCD must prove the board before the first atomic acquisition.
  Reset();StartPanelBoard();MapFailure=0x11E60000;
  CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_NOT_READY);
  CHECK(!GpioAcquireCalls && !PanelWrites && !PanelLeaseHeld);

  Reset();
  {
    UINT8 *Work=malloc((size_t)Bytes+4096);INT32 Length;UINT8 *P;
    CHECK(Work && !fdt_open_into(Tree,Work,(int)Bytes+4096));
    P=(UINT8 *)fdt_getprop_w(Work,fdt_path_offset(Work,"/soc/dsi@14013000/panel1@0"),"compatible",&Length);
    CHECK(P && Length>1);P[0]^=1;Tree=Work;Bytes=fdt_totalsize(Work);
    StartPanelBoard();CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_UNSUPPORTED);
    CHECK(!GpioAcquireCalls && !PanelWrites);
    free(Work);Tree=Original;Bytes=Size;
  }

  Reset();StartPanelBoard();CurrentTpl=TPL_NOTIFY;
  CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_NOT_READY);
  CHECK(!GpioAcquireCalls && !PanelWrites);CurrentTpl=TPL_CALLBACK;
  Exiting=TRUE;
  CHECK(PanelBoard.RunInit(&PanelBoard)==EFI_ABORTED && !GpioAcquireCalls && !PanelWrites);

  // A failed protocol publication closes its EBS event when possible. If the
  // close itself fails, the driver remains resident and exposes no protocol.
  Tree=Original;Bytes=Size;
  Reset();PanelInstallStatus=EFI_OUT_OF_RESOURCES;
  CHECK(T70GpioPlatformEntry((EFI_HANDLE)0x99,NULL)==EFI_OUT_OF_RESOURCES);
  CHECK(EventCloseCalls==1 && ExitEvent==NULL && !PanelInstalled &&
        PanelHandle==NULL && !GpioAcquireCalls && !PanelWrites);
  Reset();PanelInstallStatus=EFI_DEVICE_ERROR;CloseEventStatus=EFI_DEVICE_ERROR;
  CHECK(T70GpioPlatformEntry((EFI_HANDLE)0x99,NULL)==EFI_SUCCESS);
  CHECK(EventCloseCalls==1 && ExitEvent==(EFI_EVENT)1 && !PanelInstalled &&
        PanelHandle==NULL && TouchTimer==NULL && !GpioAcquireCalls && !PanelWrites);
  Tree=Original;Bytes=Size;
}
int main(int argc,char **argv) {
  CHECK(argc==2);FILE *F=fopen(argv[1],"rb");CHECK(F);
  CHECK(fseek(F,0,SEEK_END)==0);long Size=ftell(F);CHECK(Size>0);rewind(F);
  UINT8 *Original=malloc(Size),*Work=malloc(Size+4096);CHECK(Original && Work);
  CHECK(fread(Original,1,Size,F)==(size_t)Size);fclose(F);Tree=Original;Bytes=Size;
  Dxe.GetMemorySpaceDescriptor=Descriptor;Services.CreateEventEx=Create;Services.CreateEvent=TimerCreate;Services.SetTimer=TimerSet;
  Services.LocateHandleBuffer=Locate;Services.LocateProtocol=ProtocolLocate;Services.HandleProtocol=Handle;Services.OpenProtocol=Open;
  Services.CloseProtocol=Close;Services.InstallMultipleProtocolInterfaces=PublishTouch;Services.ConnectController=Connect;
  Services.RaiseTPL=RaiseTpl;Services.RestoreTPL=RestoreTpl;Services.CloseEvent=CloseEvent;
  Reset();CHECK(ValidateTree(Tree,Bytes)==EFI_SUCCESS);CHECK(LiveReady());Export(TRUE);
  CHECK(Reads==25 && ChargerIrqHandoff==1 && HallHandoff[0]==1 && !HallHandoff[1] && !HallHandoff[2] && HallHandoff[3]==1);
  for(unsigned Pin=0;Pin<160;Pin++) {
    Reset();Masks[Pin/32]&=~(1U<<(Pin%32));Export(FALSE);
  }
  for(unsigned Pin=0;Pin<126;Pin++) {
    Reset();Domains[Pin/32]&=~(1U<<(Pin%32));Export(FALSE);
  }
  Reset();Domains[3]=0x3FFFFFFF;Domains[4]=0;Export(TRUE);
  for(unsigned I=1;I<=4;I++) { Reset();BadHandoff=I;Export(FALSE);CHECK(!Reads); }
  for(unsigned I=1;I<=7;I++) { Reset();MapMode=I;Export(FALSE);CHECK(!Reads); }
  const unsigned Bases[]={0x10005000,0x1000B000,0x11C30000,0x11D10000,0x11D40000,0x11E20000,0x11E60000,0x11EA0000};
  for(unsigned I=0;I<ARRAY_SIZE(Bases);I++) { Reset();MapFailure=Bases[I];Export(FALSE);CHECK(!Reads); }
  Reset();TableStatus=EFI_NOT_FOUND;Export(FALSE);CHECK(!Reads);
  Reset();Tree=NULL;Export(FALSE);CHECK(!Reads);Tree=Original;
  Reset();Export(TRUE);CHECK(ChargerIrqHandoff==1);
  Masks[4]=0;Export(FALSE);CHECK(Handoff[0]==0 && Handoff[1]==0 && ChargerIrqHandoff==0 && CacheCleans==8);
  const struct { const CHAR8 *Node,*Prop;unsigned Byte; } Mutations[]={
    {"/","compatible",0},{"/","#address-cells",3},{"/","#size-cells",3},{"/soc","#address-cells",3},{"/soc","#size-cells",3},
    {"/soc/pinctrl","compatible",0},{"/soc/pinctrl","reg",7},{"/soc/pinctrl","reg",15},
    {"/soc/pinctrl","gpio-ranges",3},{"/soc/pinctrl","gpio-ranges",7},{"/soc/pinctrl","gpio-ranges",11},{"/soc/pinctrl","gpio-ranges",15},
    {"/soc/pinctrl","#gpio-cells",3},{"/soc/pinctrl","#interrupt-cells",3},{"/soc/pinctrl","mediatek,eint",3},
    {"/soc/apirq@1000b000","compatible",0},{"/soc/apirq@1000b000","reg",7},{"/soc/apirq@1000b000","reg",15},
    {"/soc/apirq@1000b000","mediatek,total-pin-number",3},{"/soc/apirq@1000b000","mediatek,instance-num",3},
    {"/soc/apirq@1000b000","interrupts",3},{"/soc/apirq@1000b000","interrupts",7},{"/soc/apirq@1000b000","interrupts",11},{"/soc/apirq@1000b000","interrupts",15},
    {"/interrupt-controller","reg",7},{"/interrupt-controller","reg",15},{"/interrupt-controller","reg",23},{"/interrupt-controller","reg",31},
    {"/interrupt-controller","compatible",0},{"/interrupt-controller","#interrupt-cells",3},{"/","interrupt-parent",3}
  };
  for(unsigned I=0;I<ARRAY_SIZE(Mutations);I++) {
    CopyMem(Work,Original,Size);INT32 Length;
    UINT8 *P=(UINT8 *)FdtGetProp(Work,FdtPathOffset(Work,Mutations[I].Node),Mutations[I].Prop,&Length);
    CHECK(P && Mutations[I].Byte<(unsigned)Length);P[Mutations[I].Byte]^=1;
    Tree=Work;Reset();Export(FALSE);CHECK(!Reads);
  }
  const CHAR8 *Nodes[]={"/","/soc","/soc/pinctrl","/soc/apirq@1000b000","/interrupt-controller"};
  for(unsigned I=0;I<ARRAY_SIZE(Nodes);I++) {
    CHECK(!fdt_open_into(Original,Work,Size+4096));
    CHECK(!fdt_setprop_string(Work,fdt_path_offset(Work,Nodes[I]),"status","disabled"));
    Tree=Work;Bytes=fdt_totalsize(Work);Reset();Export(FALSE);CHECK(!Reads);
  }
  for(unsigned I=0;I<4;I++) {
    CHECK(!fdt_open_into(Original,Work,Size+4096));
    INT32 N=fdt_path_offset(Work,I==3?"/soc":"/soc/apirq@1000b000");
    const CHAR8 *Names[]={"mediatek,pins","interrupts-extended","interrupt-parent","ranges"};
    fdt32_t Value=cpu_to_fdt32(I==2?0:1);
    CHECK(!fdt_setprop(Work,N,Names[I],&Value,sizeof(Value)));
    Tree=Work;Bytes=fdt_totalsize(Work);Reset();Export(FALSE);CHECK(!Reads);
  }
  const CHAR8 *BooleanNodes[]={"/interrupt-controller","/soc/pinctrl","/soc/pinctrl"};
  for(unsigned I=0;I<3;I++) {
    CHECK(!fdt_open_into(Original,Work,Size+4096));
    CHECK(!fdt_delprop(Work,fdt_path_offset(Work,BooleanNodes[I]),I==2?"gpio-controller":"interrupt-controller"));
    Tree=Work;Bytes=fdt_totalsize(Work);Reset();Export(FALSE);CHECK(!Reads);
  }
  Tree=Original;Bytes=Size;Reset();CHECK(ChargerIrqReady(Tree));
  PanelModes[12]=1;Export(TRUE);CHECK(!ChargerIrqHandoff);
  Reset();PanelDirections[12]=TRUE;Export(TRUE);CHECK(!ChargerIrqHandoff);
  const CHAR8 *ChargerPath="/soc/i2c@1101a000/sc8989x@6A";
  for(unsigned I=0;I<5;I++) {
    CHECK(!fdt_open_into(Original,Work,Size+4096));
    INT32 Node=fdt_path_offset(Work,ChargerPath),Length;
    if(I==0)CHECK(!fdt_setprop_string(Work,Node,"status","disabled"));
    else if(I==1)CHECK(!fdt_delprop(Work,Node,"sc,intr-gpio"));
    else {
      fdt32_t *Pin=fdt_getprop_w(Work,Node,"sc,intr-gpio",&Length);
      CHECK(Pin && Length==12);
      Pin[I-2]=cpu_to_fdt32(15);
    }
    Tree=Work;Bytes=fdt_totalsize(Work);Reset();Export(TRUE);CHECK(!ChargerIrqHandoff);
  }
  Tree=Original;Bytes=Size;Reset();CHECK(ValidateHallTree(Tree)==EFI_SUCCESS && LiveHallReady());
  for(unsigned I=0;I<3;I++) {
    Reset();if(I==0)Mode0=1;else if(I==1)Dir9=1;else Ies0=0;
    Export(TRUE);CHECK(!HallHandoff[0] && HallHandoff[3]==1);
  }
  const struct {const CHAR8 *Prop;unsigned Byte;} HallChanges[]={
    {"hall_name",0},{"hall_int_port",3},{"hall_int_port",7},{"hall_int_port",11},{"hall_far_gpvalue",3}
  };
  for(unsigned I=0;I<ARRAY_SIZE(HallChanges);I++) {
    CHECK(!fdt_open_into(Original,Work,Size+4096));
    INT32 Node=fdt_path_offset(Work,"/soc/odm/hall_para"),Length;
    UINT8 *Value=fdt_getprop_w(Work,Node,HallChanges[I].Prop,&Length);CHECK(Value && (unsigned)Length>HallChanges[I].Byte);
    Value[HallChanges[I].Byte]^=1;Tree=Work;Bytes=fdt_totalsize(Work);Reset();Export(TRUE);CHECK(!HallHandoff[0]);
  }
  const CHAR8 *HallRequired[]={"hall_name","hall_int_port","hall_far_gpvalue","wakeup-source"};
  for(unsigned I=0;I<ARRAY_SIZE(HallRequired);I++) {
    CHECK(!fdt_open_into(Original,Work,Size+4096));
    CHECK(!fdt_delprop(Work,fdt_path_offset(Work,"/soc/odm/hall_para"),HallRequired[I]));
    Tree=Work;Bytes=fdt_totalsize(Work);Reset();Export(TRUE);CHECK(!HallHandoff[0]);
  }
  for(unsigned I=0;I<2;I++) {
    CHECK(!fdt_open_into(Original,Work,Size+4096));
    CHECK(!fdt_setprop_string(Work,fdt_path_offset(Work,I?"/soc/odm":"/soc/odm/hall_para"),"status","disabled"));
    Tree=Work;Bytes=fdt_totalsize(Work);Reset();Export(TRUE);CHECK(!HallHandoff[0]);
  }
  Tree=Original;Bytes=Size;Reset();CHECK(ValidateTouchTree(Tree)==EFI_SUCCESS && LiveTouchReady());
  Reset();GpioExternalBusy=TRUE;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !Opens && !Requests && !GpioAcquireCalls && !GpioLeaseHeld);
  Reset();GpioAvailable=FALSE;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !Opens && !Requests && !GpioAcquireCalls);
  Reset();GpioProtocol.IsBusy=NULL;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !Opens && !Requests && !GpioAcquireCalls);
  Reset();GpioBusyFailCall=1;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !Opens && !Requests && !GpioAcquireCalls);
  Reset();GpioLocateStatus=EFI_DEVICE_ERROR;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !Opens && !Requests && !GpioAcquireCalls);
  Reset();MapFailure=0x11E20000;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !Opens && !Requests && !GpioAcquireCalls);
  CHECK(TableLookups==0);
  Reset();Masks[0]=0;
  for(unsigned I=0;I<100;I++)ObserveTouch(NULL,NULL);
  CHECK(!TableLookups && !TouchHandle && !Opens && !Requests && !GpioAcquireCalls);
  Reset();GpioBusyFailCall=3;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && Opens==1 && Requests==10 && Closes==1 && !GpioLeaseHeld && GpioReleaseCalls==1);
  Reset();GpioAcquireStatus=EFI_DEVICE_ERROR;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !Opens && !Requests && GpioAcquireCalls==1 && !GpioLeaseHeld);
  Reset();GpioReturnedToken=0x153;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !Opens && !Requests && GpioLeaseHeld && ProbePinLeaseHeld &&
        ProbePinLeaseOwner==&GpioProtocol && ProbePinLeaseToken==0x153 && !TouchPinCleanupPending && GpioReleaseCalls==1);
  ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !Opens && !Requests && GpioLeaseHeld && ProbePinLeaseHeld &&
        ProbePinLeaseToken==0x153 && !TouchPinCleanupPending && GpioReleaseCalls==2 && GpioAcquireCalls==1);
  Reset();GpioReturnedToken=0;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !Opens && !Requests && GpioLeaseHeld && ProbePinLeaseHeld &&
        !ProbePinLeaseToken && !TouchPinCleanupPending && GpioReleaseCalls==1);
  ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !Opens && !Requests && GpioLeaseHeld && ProbePinLeaseHeld &&
        !ProbePinLeaseToken && !TouchPinCleanupPending && GpioReleaseCalls==2 && GpioAcquireCalls==1);
  Reset();GpioValidateStatus=EFI_DEVICE_ERROR;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !Opens && !Requests && !GpioLeaseHeld && GpioReleaseCalls==1);
  Reset();GpioReleaseFailCall=1;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && Opens==1 && Requests==10 && Closes==1 && GpioLeaseHeld &&
        ProbePinLeaseHeld && ProbePinLeaseToken==GpioLeaseToken && !TouchPinCleanupPending);
  GpioReleaseFailCall=0;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !ProbeOwned && !ProbePinLeaseHeld && !GpioLeaseHeld && Requests==10 && Closes==1);
  ObserveTouch(NULL,NULL);
  CHECK(TouchHandle==(VOID *)3 && !ProbePinLeaseHeld && !TouchPinCleanupPending && !GpioLeaseHeld && Requests==20);
  Reset();ObserveTouch(NULL,NULL);
  CHECK(TouchHandle==(VOID *)3 && Requests==10 && GpioReleaseCalls==2 && !GpioLeaseHeld);
  GpioReleaseFailCall=GpioReleaseCalls+1;
  CHECK(!TouchReady(&TouchBoard) && TouchPinCleanupPending && GpioLeaseHeld &&
        TouchPinLeaseOwner==&GpioProtocol && TouchPinCleanupToken==GpioLeaseToken);
  GpioReleaseFailCall=0;
  CHECK(TouchReady(&TouchBoard) && !TouchPinCleanupPending && !GpioLeaseHeld);
  Reset();ChangePinDuringProbe=TRUE;ObserveTouch(NULL,NULL);
  CHECK(!TouchHandle && !ProbeOwned && !GpioLeaseHeld && Requests==10 && Closes==1 && !Installs);
  Reset();ProbeLeaseConflict=TRUE;ObserveTouch(NULL,NULL);
  CHECK(TouchHandle==(VOID *)3 && ConflictAttempted && PanelAcquireDenied && PanelWriteDenied &&
        Requests==10 && !ProbePinLeaseHeld && !GpioLeaseHeld && Installs==1);
  Reset();BusyPinDuringProbe=TRUE;ObserveTouch(NULL,NULL);
  CHECK(TouchHandle==(VOID *)3 && !ProbeOwned && !GpioLeaseHeld && Requests==10 && Installs==1);
  // A late first bind can fail after BDS has finished ConnectAll.
  Reset();ConnectFailures=1;ObserveTouch(NULL,NULL);
  CHECK(TouchHandle && !PointerPresent && Connects==1 && Installs==1);
  for(unsigned Retry=0;Retry<120;Retry++)ObserveTouch(NULL,NULL);
  CHECK(PointerPresent && Connects==2 && Installs==1 && Requests==10);
  Reset();ConnectReenter=TRUE;ObserveTouch(NULL,NULL);
  CHECK(PointerPresent && Connects==1 && Requests==10 && !TouchConnectBusy);
  Reset();ConnectExit=TRUE;ObserveTouch(NULL,NULL);
  CHECK(Exiting && Connects==1 && !PointerPresent && !TouchConnectBusy);
  ObserveTouch(NULL,NULL);CHECK(Connects==1);
  Reset();ConnectNoPointer=TRUE;ObserveTouch(NULL,NULL);
  for(unsigned Retry=0;Retry<2000;Retry++)ObserveTouch(NULL,NULL);
  CHECK(Connects==TOUCH_CONNECT_LIMIT && Installs==1 && Requests==10 && !PointerPresent);
  // Never restart a connected driver or reconnect after EBS/lease uncertainty.
  Reset();ObserveTouch(NULL,NULL);
  for(unsigned Retry=0;Retry<120;Retry++)ObserveTouch(NULL,NULL);
  CHECK(PointerPresent && Connects==1 && Requests==10);
  Reset();ConnectFailures=1;ObserveTouch(NULL,NULL);Exiting=TRUE;
  for(unsigned Retry=0;Retry<120;Retry++)ObserveTouch(NULL,NULL);
  CHECK(Connects==1);
  Reset();ConnectFailures=1;ObserveTouch(NULL,NULL);PanelLeaseUncertain=TRUE;
  for(unsigned Retry=0;Retry<120;Retry++)ObserveTouch(NULL,NULL);
  CHECK(Connects==1);
  Reset();CHECK(ValidateTouchTree(Tree)==EFI_SUCCESS && LiveTouchReady());
  ObserveTouch(NULL,NULL);CHECK(TouchHandle==(VOID *)3 && !ProbeOwned && Installs==1 && Requests==10 && Connects==1);
  CHECK(TouchBoard.Revision==3 && TouchBoard.ValidateGpioLease && TouchReady(&TouchBoard) && !TouchPending(&TouchBoard));
  PanelCallBusy=1;CHECK(!TouchReady(&TouchBoard));PanelCallBusy=0;
  PanelLeaseHeld=TRUE;CHECK(!TouchReady(&TouchBoard));PanelLeaseHeld=FALSE;
  PanelLeaseUncertain=TRUE;CHECK(!TouchReady(&TouchBoard));PanelLeaseUncertain=FALSE;

  {
    UINT32 Pin=152;UINT64 Token=0;
    CHECK(GpioProtocol.AcquirePins(&Pin,1,&Token)==EFI_SUCCESS && Token && GpioLeaseHeld);
    CHECK(TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));
    PanelCallBusy=1;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));PanelCallBusy=0;
    PanelLeaseHeld=TRUE;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));PanelLeaseHeld=FALSE;
    PanelLeaseUncertain=TRUE;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));PanelLeaseUncertain=FALSE;

    CHECK(!TouchValidateGpioLease(NULL,&GpioProtocol,Token));
    CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,0));
    CHECK(!TouchValidateGpioLease(&TouchBoard,(MTK_GPIO_PROTOCOL *)(UINTN)1,Token));
    CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token+1));
    MapFailure=0x11EA0000;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));MapFailure=0;
    Masks[0]=0;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));Masks[0]=MAX_UINT32;
    Domains[3]=0;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));Domains[3]=MAX_UINT32;
    Ies9=0;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));Ies9=1;
    Mode152=1;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));Mode152=0;
    Dir152=0;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));Dir152=1U<<24;
    Out152=0;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));Out152=1U<<24;
    In152=0;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));In152=1U<<24;
    Ies152=0;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));Ies152=1U<<22;
    Ies9=0;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));Ies9=1;
    Mode9=0x10;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));Mode9=0;
    Dir9=1U<<9;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));Dir9=0;
    GpioLocateStatus=EFI_DEVICE_ERROR;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));GpioLocateStatus=EFI_SUCCESS;
    Exiting=TRUE;CHECK(!TouchValidateGpioLease(&TouchBoard,&GpioProtocol,Token));Exiting=FALSE;
    CHECK(GpioProtocol.ReleasePins(Token)==EFI_SUCCESS && !GpioLeaseHeld);
  }
  TouchStateUpdate(&TouchBoard,HIMAX_TOUCH_REPORT_VALID);CHECK(TouchState==HIMAX_TOUCH_STOPPED);ExportToAcpi(NULL,NULL);CHECK(!TouchHandoff[0]);
  Reset();ObserveTouch(NULL,NULL);TouchStateUpdate(&TouchBoard,HIMAX_TOUCH_STARTING);
  CHECK(TouchState==HIMAX_TOUCH_STARTING);
  TouchStateUpdate(NULL,HIMAX_TOUCH_REPORT_VALID);CHECK(TouchState==HIMAX_TOUCH_STARTING);
  In152=0;TouchStateUpdate(&TouchBoard,HIMAX_TOUCH_REPORT_VALID);CHECK(TouchState==HIMAX_TOUCH_REPORT_INVALID);
  In152=1U<<24;TouchStateUpdate(&TouchBoard,HIMAX_TOUCH_READY);
  CurrentTpl=TPL_NOTIFY;ExportToAcpi(NULL,NULL);CurrentTpl=TPL_CALLBACK;
  CHECK(TouchHandoff[0]==1 && TouchHandoff[1]==0x48 && TouchHandoff[2]==9 && TouchHandoff[3]==1);
  unsigned Previous=Requests;ObserveTouch(NULL,NULL);CHECK(Requests==Previous && !TouchReady(&TouchBoard));
  TouchStateUpdate(&TouchBoard,HIMAX_TOUCH_REPORT_INVALID);CHECK(TouchState==HIMAX_TOUCH_READY);
  ExportToAcpi(NULL,NULL);CHECK(!TouchHandoff[0]);
  for(unsigned I=0;I<4;I++) {
    Reset();ObserveTouch(NULL,NULL);TouchStateUpdate(&TouchBoard,HIMAX_TOUCH_STARTING);
    TouchStateUpdate(&TouchBoard,HIMAX_TOUCH_REPORT_VALID);
    if(I==0)TouchStateUpdate(&TouchBoard,HIMAX_TOUCH_REPORT_INVALID);
    if(I==1)TouchStateUpdate(&TouchBoard,HIMAX_TOUCH_STOPPED);
    if(I==2)Out152=0;
    if(I==3)GpioExternalBusy=TRUE;
    CurrentTpl=TPL_NOTIFY;ExportToAcpi(NULL,NULL);CurrentTpl=TPL_CALLBACK;
    CHECK(!TouchHandoff[0]);
  }
  Reset();In9=0;ObserveTouch(NULL,NULL);
  CHECK(TouchHandle && TouchPending(&TouchBoard) && In9==0 && Requests==10);
  for(unsigned I=1;I<15;I++) {
    Reset();
    switch(I) {
      case 1:Mode9=0x10;break;case 2:Mode152=1;break;
      case 3:Dir9=1U<<9;break;case 4:Dir152=0;break;case 5:Out152=0;break;
      case 6:In152=0;break;case 7:DuplicateBus=TRUE;break;case 8:ExistingBoard=TRUE;break;
      case 9:OpenStatus=EFI_ACCESS_DENIED;break;case 10:Bus.Flags=0;break;
      case 11:Bus.IommuBase=1;break;case 12:Bus.SourceClockHz=999999;break;
      case 13:Ies9=0;break;case 14:Ies152=0;break;
    }
    ObserveTouch(NULL,NULL);CHECK(!Installs && !Requests && !ProbeOwned);
    Bus.Flags=3;Bus.IommuBase=0;Bus.SourceClockHz=136000000;
  }
  for(unsigned I=0;I<6;I++) {
    Reset();
    switch(I) {
      case 0:ChipIdValid=FALSE;break;case 1:FirmwareState=0x0c;break;
      case 2:IoStatus=EFI_TIMEOUT;break;case 3:FrequencyStatus=EFI_UNSUPPORTED;break;
      case 4:ActualFrequency=400001;break;case 5:InstallStatus=EFI_OUT_OF_RESOURCES;break;
    }
    ObserveTouch(NULL,NULL);CHECK(!TouchHandle && !ProbeOwned && Closes==1 && !Connects);
    if(I==3 || I==4)CHECK(!Requests);
  }
  Reset();ActualFrequency=399999;ObserveTouch(NULL,NULL);CHECK(TouchHandle && Requests==10 && Installs==1);
  Reset();ActualFrequency=100000;ObserveTouch(NULL,NULL);CHECK(TouchHandle && Requests==10 && Installs==1);
  Reset();ActualFrequency=99999;ObserveTouch(NULL,NULL);CHECK(!TouchHandle && !Requests && !Installs);
  Reset();CloseStatus=EFI_DEVICE_ERROR;ObserveTouch(NULL,NULL);
  CHECK(ProbeOwned==(VOID *)3 && ProbePinLeaseHeld && GpioLeaseHeld && !TouchHandle && !Installs && Requests==10);
  ObserveTouch(NULL,NULL);CHECK(Requests==10 && Closes==2 && ProbePinLeaseHeld && GpioLeaseHeld && !Installs);
  CloseStatus=EFI_SUCCESS;ObserveTouch(NULL,NULL);
  CHECK(!ProbeOwned && !ProbePinLeaseHeld && !GpioLeaseHeld && Requests==10 && !Installs && Closes==3);
  Reset();ExitDuringOpen=TRUE;ObserveTouch(NULL,NULL);
  CHECK(ExitOpenInjected && Exiting && Opens==1 && FrequencyCalls==0 && Requests==0 && Closes==1 &&
        !ProbeOwned && !ProbePinLeaseHeld && !GpioLeaseHeld && !Installs && !Connects);
  Reset();ExitDuringProbe=TRUE;ObserveTouch(NULL,NULL);
  CHECK(ExitInjected && Exiting && !TouchHandle && !Installs && !Connects && Requests==1 && Closes==1 &&
        !ProbeOwned && !ProbePinLeaseHeld && !GpioLeaseHeld && !TouchHandoff[0]);
  Reset();ExitDuringProbe=TRUE;CloseStatus=EFI_DEVICE_ERROR;ObserveTouch(NULL,NULL);
  CHECK(ExitInjected && Exiting && !TouchHandle && !Installs && Requests==1 && ProbeOwned==(VOID *)3 &&
        ProbePinLeaseHeld && GpioLeaseHeld);
  CloseStatus=EFI_SUCCESS;ObserveTouch(NULL,NULL);
  CHECK(!ProbeOwned && !ProbePinLeaseHeld && !GpioLeaseHeld && Requests==1 && !Installs && !Connects);
  Reset();ExitDuringPublish=TRUE;ObserveTouch(NULL,NULL);
  CHECK(ExitPublished && Exiting && TouchHandle==(VOID *)3 && Installs==1 && !Connects &&
        !TouchHandoff[0] && !ProbeOwned && !ProbePinLeaseHeld && !GpioLeaseHeld);
  TestPanelBoard(Original,(UINTN)Size);
  Tree=Original;Bytes=Size;
  Reset();ChipIdValid=FALSE;
  for(unsigned I=0;I<5;I++)ObserveTouch(NULL,NULL);
  CHECK(ProbeAttempts==3 && Opens==3 && Closes==3 && !Installs && Requests==75);
  const CHAR8 *TouchProps[]={"reg","report_type","himax,display-coords","himax,panel-coords","himax,irq-gpio","himax,rst-gpio","interrupt-parent","interrupts","compatible"};
  for(unsigned I=0;I<ARRAY_SIZE(TouchProps);I++) {
    CopyMem(Work,Original,Size);INT32 N=fdt_path_offset(Work,"/soc/i2c@11e00000/himax_ts@48"),Length;
    UINT8 *P=(UINT8 *)FdtGetProp(Work,N,TouchProps[I],&Length);CHECK(P && Length>0);P[Length-1]^=1;
    Tree=Work;Reset();ObserveTouch(NULL,NULL);CHECK(!Installs && !Requests && !Opens);
  }
  for(unsigned I=0;I<4;I++) {
    CHECK(!fdt_open_into(Original,Work,Size+4096));INT32 N=fdt_path_offset(Work,"/soc/i2c@11e00000/himax_ts@48");
    if(I==0)CHECK(!fdt_setprop_string(Work,N,"status","disabled"));
    else if(I==1)CHECK(!fdt_delprop(Work,N,"himax,irq-gpio"));
    else if(I==2) { fdt32_t V=cpu_to_fdt32(9);CHECK(!fdt_setprop(Work,N,"interrupts-extended",&V,4)); }
    else { INT32 Length;fdt32_t *P=(fdt32_t *)fdt_getprop_w(Work,N,"interrupts",&Length);CHECK(P && Length==16);P[1]=cpu_to_fdt32(4); }
    Tree=Work;Bytes=fdt_totalsize(Work);Reset();ObserveTouch(NULL,NULL);CHECK(!Requests && !Opens);
  }
  Tree=Original;Bytes=Size;CHECK(ValidateTree(Tree,Bytes-1)==EFI_COMPROMISED_DATA);
  unsigned EventCallsBefore=EventCalls;
  Reset();EventStatus=EFI_OUT_OF_RESOURCES;Notify=NULL;CHECK(T70GpioPlatformEntry(NULL,NULL)==EFI_OUT_OF_RESOURCES && !Notify && !Reads);
  EventStatus=EFI_SUCCESS;CHECK(T70GpioPlatformEntry(NULL,NULL)==EFI_SUCCESS && Notify==ExportToAcpi && !Reads);
  CurrentTpl=TPL_NOTIFY;Notify(NULL,NULL);CurrentTpl=TPL_CALLBACK;
  CHECK(Handoff[0]==1 && CacheCleans==4 && EventCalls==EventCallsBefore+2);
  free(Work);free(Original);
  printf("GPIO board handoff: %u checks passed (stock DTB, MH248 cover input, GPIO/reset/IRQ guards, live Himax proof, I2C ownership, handoff, passive publication, explicit panel pin leases)\n",Checks);return 0;
}
