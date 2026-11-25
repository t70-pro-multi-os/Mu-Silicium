// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute the actual board producer against the original DTB and fake services.
#include "MediaTekTest.h"
STATIC UINTN EFIAPI TracedUsbDelay(UINTN Us);
#define MicroSecondDelay TracedUsbDelay
#include "../Drivers/MusbHandoffDxe/MusbHandoffDxe.c"
#undef MicroSecondDelay

EFI_GUID gFdtTableGuid, gMediaTekClockProtocolGuid, gMediaTekGpioProtocolGuid;
EFI_GUID gMediaTekI2cControllerProtocolGuid, gMtkMusbHostPlatformReadyProtocolGuid;
EFI_GUID gMediaTekI2cExclusiveProtocolGuid;
EFI_GUID gStockPowerReadOnlyProtocolGuid = STOCK_POWER_READ_ONLY_PROTOCOL_GUID;
EFI_GUID gStockPowerBoardReadyProtocolGuid = STOCK_POWER_BOARD_READY_PROTOCOL_GUID;
EFI_GUID gEfiI2cMasterProtocolGuid = EFI_I2C_MASTER_PROTOCOL_GUID;
EFI_GUID gEfiEventExitBootServicesGuid, gEfiEventReadyToBootGuid;
EFI_GUID gEfiEndOfDxeEventGroupGuid, gEfiEventBeforeExitBootServicesGuid;
EFI_GUID gEfiDxeSmmReadyToLockProtocolGuid;
EFI_GUID gEfiUsbIoProtocolGuid;
EFI_GUID gEfiUsb2HcProtocolGuid =
  { 0x3e745226, 0x9818, 0x45b6, { 0xa2, 0xac, 0xd7, 0xcd, 0x0e, 0x8b, 0xa2, 0xbc } };
EFI_GUID gEfiDevicePathProtocolGuid =
  { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
EFI_SYSTEM_TABLE *gST;
static EFI_PHYSICAL_ADDRESS ClockMapBadAddress;
static EFI_DXE_SERVICES Dxe;
EFI_DXE_SERVICES *gDS = &Dxe;

static VOID *Tree;
static UINTN TreeBytes;
static UINT8 Usb[0x10000], Phy[0x10000], Pins[0x1000];
static UINT32 HandoffWords[HANDOFF_BYTES/sizeof(UINT32)];
static UINT8 TxSize[16], RxSize[16];
static UINT16 TxAddress[16], RxAddress[16], TxCsr[16], RxCsr[16];
static UINT8 Index;
static UINTN Reads, UsbReads, Writes, IndexWrites, AckWrites, CacheCalls, PhyWrites, FifoWrites, Ep0FifoAccess;
static BOOLEAN ControllerMapBad;
static BOOLEAN IgnoreFifoWrites, ReenterOnPhy;
static BOOLEAN ColdFailHostPhy,ColdHostPhyFaultInjected;
static BOOLEAN ColdHostModeStuck,ColdCorruptGuardPhy;
static BOOLEAN ColdHostAfterPower,ColdPowerBeforeHostObserved;
static BOOLEAN ColdDropSessionAtReset,ColdSessionLostAtReset,ColdBlockSessionRearm;
static UINT16 ColdDmaControlAfterClear[8];
static UINTN ColdResetResponse,ColdResetMaskWrites;
static UINT32 ColdResetStickyMask,ColdResetMaskOffsets[24];
static BOOLEAN ColdResetResponseInjected,ColdResetAckBeforePower;
static UINTN PhysicalReplayMode,PhysicalReplayPowerReads;
static BOOLEAN PhysicalReplayPowerWritten;
typedef struct {UINT32 Space,Offset,Width,Value,ColdKind,ColdStep;} STARTUP_WRITE;
static STARTUP_WRITE PhysicalReplayTrace[128];
static UINTN PhysicalReplayTraceCount;
static UINT32 PhysicalReplayInitialDtm1;
static UINT16 PhysicalReplayInitialSwrst;
static UINT8 PhysicalReplayInitialDevctl,PhysicalReplayInitialPower;
static VOID TracePhysicalReplay(UINTN A,UINT32 Width,UINT32 Value) {
  if(!PhysicalReplayMode)return;
  UINT32 Space,Offset;
  if(A>=0x11200000U&&A<0x11210000U) {
    Offset=(UINT32)(A-0x11200000U);Space=0U;
    if(Offset!=1U&&Offset!=0x60U&&Offset!=0x74U&&
       !(Offset>=0x204U&&Offset<=0x274U&&((Offset-0x204U)%0x10U)==0U))return;
  } else if(A==0x11f4036cU) {Space=1U;Offset=0x6cU;}
  else return;
  CHECK(PhysicalReplayTraceCount<ARRAY_SIZE(PhysicalReplayTrace));
  PhysicalReplayTrace[PhysicalReplayTraceCount++]=(STARTUP_WRITE){
    Space,Offset,Width,Value,mHandoff.ColdHost.Kind,mHandoff.ColdHost.StartStep};
}
STATIC UINTN EFIAPI TracedUsbDelay(UINTN Us) {
  if(PhysicalReplayMode&&mHandoff.ColdHost.Kind==MtkMusbPlatformColdStateStarting&&
     mHandoff.ColdHost.StartStep==MtkMusbPlatformColdStartSettle) {
    CHECK(Us==100000U||Us==5000U);
    if(Us==100000U) {
      memcpy(&PhysicalReplayInitialDtm1,Phy+0x36cU,sizeof(PhysicalReplayInitialDtm1));
      memcpy(&PhysicalReplayInitialSwrst,Usb+0x74U,sizeof(PhysicalReplayInitialSwrst));
      PhysicalReplayInitialDevctl=Usb[0x60U];PhysicalReplayInitialPower=Usb[1U];
    }
    CHECK(PhysicalReplayTraceCount<ARRAY_SIZE(PhysicalReplayTrace));
    PhysicalReplayTrace[PhysicalReplayTraceCount++]=(STARTUP_WRITE){
      2U,0U,0U,(UINT32)Us,mHandoff.ColdHost.Kind,mHandoff.ColdHost.StartStep};
  }
  return MicroSecondDelay(Us);
}
static MTK_USB_PHY_READ32 SavedPhyRead;
static MTK_USB_PHY_WRITE32 SavedPhyWrite;
static MTK_USB_PHY_STATUS ColdFailPhyWrite(VOID *Context,UINT32 Offset,UINT32 Value) {
  CHECK(SavedPhyWrite); (VOID)SavedPhyWrite(Context,Offset,Value); return MtkUsbPhyIoError;
}
static MTK_USB_PHY_STATUS ColdFailPhyRead(VOID *Context,UINT32 Offset,UINT32 *Value) {
  (VOID)Context; (VOID)Offset; (VOID)Value; return MtkUsbPhyIoError;
}
static UINTN PowerReads, Connects, EventCount, EventFail, CloseCalls, CloseFail;
static UINTN RegistrationCalls, RegistrationFail, ReadyInstalls, BoardInstalls;
static UINTN RetryTimers, ProbeTimers, RetryCancels, UsbHostConnects;
static EFI_STATUS RetryTimerStatus, ProbeTimerStatus, UsbHostConnectStatus, I2cConnectStatus;
static EFI_STATUS HostDisconnectStatus, HostReleaseStatus, ReadyUninstallStatus;
static UINTN HostDisconnects, HostReleases, ReadyUninstalls;
static BOOLEAN I2cConnectNoMaster;
static BOOLEAN UsbHostReenter;
static BOOLEAN UsbHostPublishOnFailure;
static BOOLEAN UsbHostRetainProtocolAfterRelease;
static UINT8 UsbHostProtocolMask;
static BOOLEAN UsbBusStartFails;
static EFI_STATUS UsbHostProtocolStatus;
static UINTN FailInstall, LocateFailure, ControllerCount, LiveBad, MapBad;
static BOOLEAN LateReadyToLock, PogoDocked;
static UINTN BoostFailure, TcpcFailure, ClockFailure, CcFailure;
static BOOLEAN ColdPllBlank, ColdPllSetFail;
static UINTN ColdPllSetCalls;
static UINT64 ColdPllProgrammedHz;
static UINT8 ColdUsbOffMask, PinDirections[188];
static UINTN ColdClockWrites, ColdClockFailure, ColdClockIgnore;
static UINTN ColdPinWrites, ColdPinFailure, ColdPinIgnore;
static UINT32 ColdClockTrace[64];
static BOOLEAN ColdTcpcModel,ColdAutoAttach,ColdIdDropsOnAttach,ColdUsbIgnore,ColdHeaderDriftOnSession;
static BOOLEAN IdLowHubVbus,IdLowDropSinkBeforePreflight;
static BOOLEAN InheritedDeviceModel,DeviceStopReenter,DeviceStopIdDrift,DeviceBadTcpcId;
static BOOLEAN DeviceStopHubDrift,DeviceBadTcpcProduct;
static UINTN DeviceStopWrites,DeviceStopIgnore;
static UINTN DeviceResetWrites,DeviceResetFault,DeviceResetDrift;
static UINT16 DeviceResetValue;
static UINT64 DeviceResetAt;
static UINTN DeviceIrqWrites,DeviceIrqIgnore,DeviceIrqDriftAt,DeviceIrqDriftKind;
static UINT32 DeviceIrqOffsets[3],DeviceIrqWidths[3];
static UINT64 DeviceIrqFirstWriteNs;
static UINTN DeviceCsrWrites,DeviceCsrIgnore;
static UINT8 DeviceCsrEndpoint[24];
static UINT16 DeviceCsrOffset[24],DeviceCsrValue[24];

static UINTN SenseWrites,SenseWriteFailure,SenseWriteIgnore;
static UINTN SenseWriteCorruptAt,SenseReadFailAfterWrite;
static UINT8 SenseWriteCorruptValue;
static BOOLEAN SenseStuck,SenseWakeVbus;
static UINTN Sense90Reads,SenseOscillatorAt;
static UINT8 SenseRmwReadFailReg;
static BOOLEAN VconnSupply,VconnStatusStuck,VconnWriteFailure,VconnWriteIgnore,VconnReadFailAfterWrite;
static UINTN VconnWrites,VconnCheckReads,VconnDriftAfterWrite,VconnSettleReads;
static UINT8 VconnValue;
static UINT64 VconnWriteAt;
static UINT8 SenseTraceReg[16],SenseTraceValue[16];
static UINT8 ColdAttachCc,ColdPowerDriftOnFifo,TcpcRegisters[256];
static UINTN ColdTcpcWrites,ColdTcpcFailure,ColdTcpcIgnore,ColdUsbWrites;
static UINT8 ColdTcpcTraceReg[64],ColdTcpcTraceValue[64];
static UINT8 ColdTcpcInitialRole,ColdTcpcInitialRx;
static UINT8 CcRaw, CcRole, HubLevels[3];
static UINTN HubWrites, HubWriteFailure, HubIgnoreWrite, HubFailOff;
static UINTN HubTracePin[16], HubTraceValue[16];
static BOOLEAN HubDropSource, HubBusyAfterWrite, HubReenter, HubChangeOnCc;
static BOOLEAN ColdHubMuxChangedDuringSession;
static UINTN HubMutateAfterWrite, HubMutatePin;
static UINT8 HubMutateValue;
static UINTN HubLowWrites,HubReassertAfterLow;
static BOOLEAN HubFinalReassert,PostHubEvidenceFailure;
static UINTN Sequence, PowerEventSequence, ConnectSequence;
static BOOLEAN MasterPresent, BadMagic, PowerOn, BoostOn, NoTable, StuckAck;
static EFI_HANDLE BusHandle = (VOID *)(UINTN)0x600;
static EFI_HANDLE UsbHandle = (VOID *)(UINTN)0x700;
static MTK_I2C_CONTROLLER Controller;
static EFI_I2C_MASTER_PROTOCOL Master;
static MTK_I2C_EXCLUSIVE_PROTOCOL Exclusive;
static EFI_EVENT_NOTIFY ExitObserver;
static VOID *ExitObserverContext;
static UINT64 LeaseToken,NextToken;
static UINTN LeaseAcquires,LeaseReleases,RawReads,SourceWrites,SourceWriteFailure,RawReadFailure;
static UINT8 ScRegisters[256],SourcePin48,SourcePin86,VendorStatusReg97;
static UINTN VsafeReads,VsafeUnsafeAt,DrainReads,DrainReleaseAt;
static BOOLEAN DrainBounceOnce;
static UINT64 DrainReadTimes[256];
static BOOLEAN FailVsafeRead,FailVsafeDrainRead,ReassertPinOnDrain,FailVsafeReadOnce;
static UINTN FinalBoostMutation;
static BOOLEAN FinalBoostMutationObserved;
static UINT32 SourceTrace[32];
static BOOLEAN ExclusivePresent,BusQuiescing,InsideExitObserver,AutoVbus,VbusDetectEnabled;
static EFI_STATUS LeaseAcquireStatus;
static BOOLEAN FailLeaseRelease,FailObserver,SourceReenter,SourceDropAttach;

static STOCK_POWER_BOARD_READY_PROTOCOL *InstalledBoard;
static MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL *InstalledReady;
typedef struct {
  EFI_TPL Tpl;
  EFI_EVENT_NOTIFY Notify;
  CONST VOID *Context;
  CONST EFI_GUID *Group;
} TEST_EVENT;
static TEST_EVENT Events[8];

static UINT16 U16(CONST VOID *P) { UINT16 V; memcpy(&V,P,2); return V; }
static UINT32 U32(CONST VOID *P) { UINT32 V; memcpy(&V,P,4); return V; }
static VOID Put16(VOID *P,UINT16 V) { memcpy(P,&V,2); }
static VOID Put32(VOID *P,UINT32 V) { memcpy(P,&V,4); }

UINT8 EFIAPI MmioRead8(UINTN A) {
  ++Reads; ++UsbReads;
  CHECK(A>=0x11200000 && A<0x11210000);
  UINT32 O=(UINT32)(A-0x11200000);
  if(O==1U&&PhysicalReplayPowerWritten) {
    if(PhysicalReplayMode==2U||
       (PhysicalReplayMode==3U&&PhysicalReplayPowerReads>=1U))Usb[1]|=2U;
    ++PhysicalReplayPowerReads;
  }
  if(O==0x60U&&ColdDropSessionAtReset&&
     mHandoff.ColdHost.Kind==MtkMusbPlatformColdStateStarting&&
     mHandoff.ColdHost.StartStep==MtkMusbPlatformColdStartReset) {
    ColdDropSessionAtReset=FALSE;ColdSessionLostAtReset=TRUE;
    Usb[0x60U]=0x98U;Usb[1U]=0x71U;
  }
  if(!Index&&(O==0x62U||O==0x63U)) { ++Ep0FifoAccess; return 0U; }
  if(O==0x0e) return Index;
  if(O==0x62) return TxSize[Index];
  if(O==0x63) return RxSize[Index];
  return Usb[O];
}
UINT16 EFIAPI MmioRead16(UINTN A) {
  ++Reads;
  if(A==0x1101a024) return LiveBad==2?1:0;
  CHECK(A>=0x11200000 && A+2<=0x11210000 && !(A&1));
  ++UsbReads;
  switch(A-0x11200000) {
    case 0x12:
      if(DeviceResetAt&&TimeNs>=DeviceResetAt&&DeviceResetFault!=2U) {
        // Modeled reset completion; neither the request nor its write ACK
        // proves this physical postcondition.
        TxCsr[0]&=(UINT16)~8U;Put16(Usb+0x74U,U16(Usb+0x74U)&(UINT16)~1U);
        DeviceResetAt=0U;
      }
      return TxCsr[Index];
    case 0x16:return RxCsr[Index];
    case 0x64:if(!Index){++Ep0FifoAccess;return 0U;} return TxAddress[Index];
    case 0x66:if(!Index){++Ep0FifoAccess;return 0U;} return RxAddress[Index];
    default:return U16(Usb+(A-0x11200000));
  }
}
UINT32 EFIAPI MmioRead32(UINTN A) {
  ++Reads;
  if(A>=0x11200000 && A+4<=0x11210000) { ++UsbReads; return U32(Usb+(A-0x11200000)); }
  if(A>=0x11f40000 && A+4<=0x11f50000) return U32(Phy+(A-0x11f40000));
  if(A>=0x10005000 && A+4<=0x10006000) return U32(Pins+(A-0x10005000));
  if(A==0x1101be00) return LiveBad==1?4:0;
  if(A==0x10217488) return LiveBad==3?1:0;
  CHECK(0); return 0;
}
UINT64 EFIAPI MmioRead64(UINTN A) {
  ++Reads;
  switch(A) {
    case HANDOFF_PAGE:return BadMagic?0:HANDOFF_MAGIC;
    case HANDOFF_PAGE+0x48:return (UINT64)(UINTN)Tree;
    case HANDOFF_PAGE+0x50:return TreeBytes;
    case HANDOFF_PAGE+0x58:return EFI_SUCCESS;
    default:CHECK(0); return 0;
  }
}
static BOOLEAN DeviceRetirementPowerValid(VOID) {
  if(mHandoff.DeviceId != 0U)
    return VbusDetectEnabled&&!PowerOn&&(VendorStatusReg97&2U)!=0U;
  return SenseWrites == 0U || (VbusDetectEnabled &&
    (PowerOn ? Husb311SinkAttached(CcRaw, CcRole, 0x0cU) : (VendorStatusReg97 & 2U) != 0U));
}
static BOOLEAN DeviceRetirementWriteAllowed(VOID) {
  if (mHandoff.PassiveDeviceRetiring)
    return mHandoff.EndOfDxeSeen && !mHandoff.ReadyInstalled &&
      !mHandoff.ExitingBootServices && (Usb[0x60] & 5U) == 0U && (Usb[1] & 0x4eU) == 0U;
  return mHandoff.DeviceRetiring && mHandoff.DeviceDisconnectInProgress &&
    mHandoff.EndOfDxeSeen && LeaseToken && !mHandoff.ReadyInstalled &&
    !mHandoff.ExitingBootServices && DeviceRetirementPowerValid();
}
UINT8 EFIAPI MmioWrite8(UINTN A,UINT8 V) {
  ++Writes;TracePhysicalReplay(A,8U,V);
  if(A==0x1120000e) { CHECK(V<16); Index=V; ++IndexWrites; return V; }
  if(A==0x1120000b) {
    if(CanColdReset()) {
      CHECK(V==0U&&mHandoff.ColdHost.ResetCheck==MtkMusbPlatformResetFinalIdle);
      CHECK(ColdResetMaskWrites<ARRAY_SIZE(ColdResetMaskOffsets)&&ColdResetMaskWrites%3U==0U);
      ColdResetMaskOffsets[ColdResetMaskWrites++]=0x0bU;
      if(ColdResetStickyMask!=0x0bU)Usb[0x0bU]=0U;
      return V;
    }
    CHECK(DeviceRetirementWriteAllowed() && V == 0U);
    CHECK(DeviceIrqWrites<3U);
    if(!DeviceIrqWrites)DeviceIrqFirstWriteNs=TimeNs;
    DeviceIrqOffsets[DeviceIrqWrites]=0x0bU;DeviceIrqWidths[DeviceIrqWrites]=8U;
    if(++DeviceIrqWrites!=DeviceIrqIgnore)Usb[0x0b]=0U;
    return V;
  }
  if(A==0x11200060 || A==0x11200001) {
    if(mHandoff.DeviceDisconnectInProgress) {
      UINTN O=A-0x11200000U;
      CHECK(mHandoff.DeviceDisconnectAttempted&&!mHandoff.ReadyInstalled&&!mHandoff.ExitingBootServices);
      CHECK(V==(UINT8)(Usb[O]&~((O==1U)?0x40U:1U)));
      ++DeviceStopWrites;
      if(DeviceStopWrites!=DeviceStopIgnore) Usb[O]=V;
      if(DeviceStopWrites==2U) {
        if(DeviceResetDrift==1U)Put16(Usb+0x274U,1U);
        if(DeviceResetDrift==2U)Put32(Usb+0x300U,0x10000U);
        if(DeviceResetDrift==3U)RxCsr[8]=1U;
        if(DeviceResetDrift==4U)PogoDocked=FALSE;
        if(DeviceResetDrift==5U) { CcRaw=2U;CcRole=0x0aU; }
        if(DeviceResetDrift==6U)Usb[1]|=8U;
        if(DeviceResetDrift==7U)TxCsr[0]|=0x10U;
        if(DeviceResetDrift==8U)TcpcRegisters[0x1f]=1U;
        if(DeviceResetDrift==9U)TcpcRegisters[0x19]=2U;
        if(DeviceResetDrift==10U)TcpcRegisters[0x1c]=1U;
        if(DeviceResetDrift==11U)Put16(Usb+6U,0x8000U);
      }
      if(DeviceStopIdDrift) { DeviceStopIdDrift=FALSE;PogoDocked=FALSE; }
      if(DeviceStopHubDrift) { DeviceStopHubDrift=FALSE;HubLevels[0]^=1U; }
      return V;
    }
    CHECK((ColdTcpcModel||mHandoff.RetainedTypecSource)&&
          mHandoff.ColdBound&&mHandoff.ColdInProgress&&
          mHandoff.AllowColdWrites&&!mHandoff.ReadyInstalled&&!mHandoff.ExitingBootServices);
    ++ColdUsbWrites;
    if(A==0x11200001U&&ColdResetResponseInjected&&
       mHandoff.ColdHost.StartStep==MtkMusbPlatformColdStartPower) {
      CHECK(ColdResetMaskWrites>0U&&ColdResetMaskWrites%3U==0U&&
            Usb[0x0bU]==0U&&U16(Usb+6U)==0U&&U16(Usb+8U)==0U);
      CHECK(U16(Usb+2U)==0U&&U16(Usb+4U)==0U&&Usb[0x200U]==0U);
      CHECK(U16(Usb+0x274U)==0U&&TxCsr[0]==0U&&U32(Usb+0x300U)==0U);
      ColdResetAckBeforePower=TRUE;
    }
    if(!ColdUsbIgnore) {
      if(A==0x11200060) {
        // SESSION is writable. HOSTMODE/VBUS are hardware observations.
        Usb[0x60]=(UINT8)((Usb[0x60]&~1U)|(V&1U));
        if(ColdBlockSessionRearm&&ColdSessionLostAtReset&&
           mHandoff.ColdHost.StartStep==MtkMusbPlatformColdStartReset)Usb[0x60]&=(UINT8)~1U;
        if((V&1U)&&!ColdHostModeStuck&&!ColdHostAfterPower) Usb[0x60]|=4U; else Usb[0x60]&=(UINT8)~4U;
        if(ColdHeaderDriftOnSession&&(V&1U)) {
          ColdHeaderDriftOnSession=FALSE; TcpcRegisters[0x2e]&=(UINT8)~8U;
        }
      } else {
        if(ColdHostAfterPower&&mHandoff.ColdHost.PhyHostVerified&&
           Usb[0x60]==0x99U&&Usb[1]==0x63U&&V==0x21U) {
          ColdPowerBeforeHostObserved=TRUE;
          if(!ColdHostModeStuck) Usb[0x60]=0x1dU;
        }
        if(ColdSessionLostAtReset&&ColdHostAfterPower&&!ColdHostModeStuck&&
           (Usb[0x60]&1U)&&Usb[1]==0x71U&&V==0x31U)Usb[0x60]=0x1dU;
        Usb[1]=V;
        if(PhysicalReplayMode) {
          PhysicalReplayPowerWritten=TRUE;PhysicalReplayPowerReads=0U;
          if(PhysicalReplayMode==1U)Usb[1]|=2U;
        }
      }
    }
    return V;
  }
  if(A==0x11200074U) {
    CHECK(CanColdReset()&&V==(UINT8)((Usb[0x74U]|0x80U)&~0x40U));
    Usb[0x74U]=V;return V;
  }
  if(A==0x11200062 || A==0x11200063) {
    CHECK((mHandoff.ColdInProgress||mHandoff.RetainedHostPending||
           mHandoff.RetainedHostOwned)&&mHandoff.AllowFifoWrites&&
          !mHandoff.ExitingBootServices);
    if(!Index) { ++Ep0FifoAccess; return V; }
    ++FifoWrites;
    if(ColdPowerDriftOnFifo) { Usb[1]&=(UINT8)~ColdPowerDriftOnFifo; ColdPowerDriftOnFifo=0U; }
    if(!IgnoreFifoWrites) { if(A==0x11200062) TxSize[Index]=V; else RxSize[Index]=V; }
    return V;
  }
  CHECK(A==0x11200200);
  ++AckWrites; if(!StuckAck) Usb[0x200]&=(UINT8)~V; return V;
}
UINT16 EFIAPI MmioWrite16(UINTN A,UINT16 V) {
  ++Writes;TracePhysicalReplay(A,16U,V);
  if(CanColdReset()&&(A==0x11200074U||
     (A>=0x11200204U&&A<=0x11200274U&&((A-0x11200204U)%16U)==0U))) {
    UINTN O=A-0x11200000U;
    CHECK((Usb[0x60]&1U)&&mHandoff.ColdHost.PhyHostVerified);
    CHECK(A==0x11200074U?V==(UINT16)(U16(Usb+O)|3U):
          (V==0U&&(U16(Usb+O)&1U)==0U));
    if(A!=0x11200074U)V|=ColdDmaControlAfterClear[(O-0x204U)/16U];
    Put16(Usb+O,V);
    if(A==0x11200074U&&ColdResetResponse) {
      UINTN Model=ColdResetResponse;
      ColdResetResponseInjected=TRUE;
      if(Model==1U||Model==3U) {
        Usb[0x0bU]=0x12U;Put16(Usb+6U,0x1ffU);Put16(Usb+8U,0x1feU);
      }
      if(Model>=2U) {Put16(Usb+2U,0x1234U);Put16(Usb+4U,0x42U);Usb[0x200U]=0xa5U;}
      if(Model==4U)Put16(Usb+0x274U,0x13U);
      if(Model==5U)TxCsr[0]=8U;
      if(Model==6U)Put32(Usb+0x300U,0x10000U);
      if(Model>=7U) {
        ColdResetStickyMask=Model==7U?0x0bU:Model==8U?6U:8U;
        if(Model==7U)Usb[0x0bU]=0x12U;
        else Put16(Usb+ColdResetStickyMask,Model==8U?0x1ffU:0x1feU);
      }
    }
    return V;
  }
  if(CanColdReset()&&(A==0x11200006U||A==0x11200008U)) {
    UINT32 O=(UINT32)(A-0x11200000U);
    CHECK(V==0U&&mHandoff.ColdHost.ResetCheck==MtkMusbPlatformResetFinalIdle);
    CHECK(ColdResetMaskWrites<ARRAY_SIZE(ColdResetMaskOffsets)&&
          ColdResetMaskWrites%3U==(O==6U?1U:2U));
    ColdResetMaskOffsets[ColdResetMaskWrites++]=O;
    if(ColdResetStickyMask!=O)Put16(Usb+O,0U);
    return V;
  }
  if(A==0x11200074U) {
    CHECK(mHandoff.DeviceRetiring&&mHandoff.DeviceDisconnectInProgress&&
      mHandoff.DeviceDisconnectAttempted&&mHandoff.DeviceResetAttempted&&
      mHandoff.EndOfDxeSeen&&LeaseToken&&!mHandoff.ReadyInstalled&&
      !mHandoff.ExitingBootServices&&DeviceStopWrites==2U&&
      (Usb[1]&0x4eU)==0U&&(Usb[0x60]&0x85U)==0x80U&&
      Usb[0x0b]==0U&&U16(Usb+6U)==0U&&U16(Usb+8U)==0U&&
      V==(UINT16)(U16(Usb+0x74U)|3U)&&TxCsr[0]==8U&&!DeviceResetWrites);
    DeviceResetWrites++;DeviceResetValue=V;
    if(DeviceResetFault!=1U) {
      Put16(Usb+0x74U,V);DeviceResetAt=TimeNs+1000000U;
    }
    return V;
  }
  if(A==0x11200012 || A==0x11200016) {
    CHECK(DeviceRetirementWriteAllowed() && Index>0U&&Index<MTK_MUSB_PLATFORM_ENDPOINTS&&
          Usb[0x0b]==0U&&U16(Usb+6U)==0U&&U16(Usb+8U)==0U&&
          DeviceStopWrites==0U&&DeviceCsrWrites<24U);
    UINT16 *Csr=A==0x11200012?&TxCsr[Index]:&RxCsr[Index];
    UINT16 Wzc=A==0x11200012?0x00a6U:0x0045U;
    CHECK((V&(A==0x11200012?0x0048U:0x0090U))==0U);
    DeviceCsrEndpoint[DeviceCsrWrites]=Index;DeviceCsrOffset[DeviceCsrWrites]=(UINT16)(A-0x11200000U);
    DeviceCsrValue[DeviceCsrWrites]=V;
    if(++DeviceCsrWrites!=DeviceCsrIgnore)*Csr=(UINT16)((V&~Wzc)|(*Csr&V&Wzc));
    return V;
  }
  if(A==0x11200006 || A==0x11200008) {
    CHECK(DeviceRetirementWriteAllowed() && V == 0U);
    CHECK(DeviceIrqWrites<3U);
    if(!DeviceIrqWrites)DeviceIrqFirstWriteNs=TimeNs;
    DeviceIrqOffsets[DeviceIrqWrites]=(UINT32)(A-0x11200000U);
    DeviceIrqWidths[DeviceIrqWrites]=16U;
    if(++DeviceIrqWrites!=DeviceIrqIgnore)Put16(Usb+(A-0x11200000U),0U);
    return V;
  }
  if(A==0x11200064 || A==0x11200066) {
    CHECK((mHandoff.ColdInProgress||mHandoff.RetainedHostPending||
           mHandoff.RetainedHostOwned)&&mHandoff.AllowFifoWrites&&
          !mHandoff.ExitingBootServices);
    if(!Index) { ++Ep0FifoAccess; return V; }
    ++FifoWrites;
    if(ColdPowerDriftOnFifo) { Usb[1]&=(UINT8)~ColdPowerDriftOnFifo; ColdPowerDriftOnFifo=0U; }
    if(!IgnoreFifoWrites) { if(A==0x11200064) TxAddress[Index]=V; else RxAddress[Index]=V; }
    return V;
  }
  CHECK(A==0x11200002 || A==0x11200004);
  ++AckWrites;
  if(!StuckAck) Put16(Usb+(A-0x11200000),U16(Usb+(A-0x11200000))&(UINT16)~V);
  return V;
}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V) {
  TracePhysicalReplay(A,32U,V);
  if(A>=HANDOFF_RECORD && A+4<=HANDOFF_RECORD+HANDOFF_BYTES) {
    CHECK(!(A&3)); HandoffWords[(A-HANDOFF_RECORD)/4]=V; return V;
  }
  if(A==0x11200204U) {
    UINT32 Current=U32(Usb+0x204U);
    CHECK(CanColdReset()&&!(Current&1U)&&
          (V==(Current|0x600U)||V==(Current|0x4000U)));
    Put32(Usb+0x204U,V);return V;
  }
  CHECK(A>=0x11f40000 && A+4<=0x11f40700 && !(A&3));
  CHECK(mHandoff.PhyBound&&!mHandoff.ExitingBootServices);
  ++PhyWrites; Put32(Phy+(A-0x11f40000),V);
  if(InheritedDeviceModel&&A==0x11f4036cU) {
    if((V&0x202U)==0x200U&&
       !(mHandoff.ColdHost.Kind==MtkMusbPlatformColdStateStarting&&
         mHandoff.ColdHost.StartStep==MtkMusbPlatformColdStartPhyInvalid)) {
      if(ColdHostAfterPower&&mHandoff.ColdHost.PhyHostAttempted) {
        // Trial99 observation injected only after the owned HOST PHY write.
        // Readiness is a declared model response to POWER initialization.
        Usb[0x60]|=0x80U;Usb[0x60]&=(UINT8)~4U;Usb[1]|=0x42U;
      } else if(!ColdHostModeStuck) {
        Usb[0x60]&=(UINT8)~0x80U;
        if(Usb[0x60]&1U) Usb[0x60]|=4U;
      }
    } else {
      Usb[0x60]|=0x80U;Usb[0x60]&=(UINT8)~4U;
    }
  }
  if(ColdCorruptGuardPhy&&A==0x11f4036cU&&
     MtkUsbPhyCoreIsPowered(&mHandoff.PhyCore)&&!mHandoff.ColdHost.PhyInvalidAttempted) {
    ColdCorruptGuardPhy=FALSE;Put32(Phy+0x304U,U32(Phy+0x304U)^0x100U);
  }
  if(ColdFailHostPhy&&!ColdHostPhyFaultInjected&&mHandoff.ColdHost.PhyHostAttempted) {
    CHECK(MtkUsbPhyCoreIsPowered(&mHandoff.PhyCore));
    SavedPhyRead=mHandoff.PhyCore.Io.Read32; SavedPhyWrite=mHandoff.PhyCore.Io.Write32;
    mHandoff.PhyCore.Io.Read32=ColdFailPhyRead; mHandoff.PhyCore.Io.Write32=ColdFailPhyWrite;
    ColdHostPhyFaultInjected=TRUE;
  }
  if(ReenterOnPhy) { ReenterOnPhy=FALSE; UsbRoleTick(mHandoff.RetryTimerEvent,&mHandoff); CHECK(!InstalledReady); }
  return V;
}
VOID EFIAPI MemoryFence(VOID) { __sync_synchronize(); }
VOID *EFIAPI WriteBackDataCacheRange(VOID *A,UINTN N) {
  CHECK((UINTN)A==HANDOFF_RECORD);
  CHECK(N==64 || N==4);
  if(N==4) CHECK(HandoffWords[0]==1);
  else CHECK(HandoffWords[0]==0);
  ++CacheCalls; return A;
}
EFI_STATUS EFIAPI EfiGetSystemConfigurationTable(EFI_GUID *G,VOID **F) {
  CHECK(G==&gFdtTableGuid);
  if(NoTable) return EFI_NOT_FOUND;
  *F=Tree; return EFI_SUCCESS;
}
EFI_LOCK *EFIAPI EfiInitializeLock(EFI_LOCK *L,EFI_TPL T) {
  L->Tpl=T; L->OwnerTpl=TPL_APPLICATION; L->Lock=EfiLockReleased; return L;
}
EFI_STATUS EFIAPI EfiAcquireLockOrFail(EFI_LOCK *L) {
  if(L->Lock==EfiLockAcquired) return EFI_ACCESS_DENIED;
  L->Lock=EfiLockAcquired; return EFI_SUCCESS;
}
VOID EFIAPI EfiReleaseLock(EFI_LOCK *L) {
  CHECK(L->Lock==EfiLockAcquired); L->Lock=EfiLockReleased;
}
static EFI_STATUS EFIAPI Descriptor(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D) {
  memset(D,0,sizeof(*D)); D->BaseAddress=A; D->Length=0x10000;
  D->GcdMemoryType=(MapBad==1||(ClockMapBadAddress&&A==ClockMapBadAddress)||(ControllerMapBad&&A==0x11200000U))?
    EfiGcdMemoryTypeSystemMemory:EfiGcdMemoryTypeMemoryMappedIo;
  D->Attributes=MapBad==2?0:EFI_MEMORY_UC;
  if(MapBad==3) D->Attributes|=EFI_MEMORY_RO;
  if(MapBad==4) D->Length=1;
  if(MapBad==5) D->Attributes|=EFI_MEMORY_WP;
  return EFI_SUCCESS;
}
enum {ClkSs=701,ClkUsb=702,ClkPll=703,ClkRef=704,ClkParent=705,
      ClkI2c=710,ClkAxi,ClkI2cPseudo,ClkDmaPseudo,ClkDma,ClkI2cRate,ClkI2c6Main};
static CONST CHAR8 *ClockNames[]={"IFRAO_SSUSB","TOP_USB_SEL","APMIXED_USBPLL",
  "TOP_CLK26M","TOP_UNIVPLL_D5_D4","TOP_I2C_SEL","TOP_AXI_SEL","IFRAO_I2C_PSEUDO",
  "IFRAO_APDMA_PSEUDO","IFRAO_AP_DMA","TOP_I2C_PSEUDO","IMPC_AP_CLOCK_I2C6"};
static CONST UINT32 ClockIds[]={ClkSs,ClkUsb,ClkPll,ClkRef,ClkParent,ClkI2c,ClkAxi,
  ClkI2cPseudo,ClkDmaPseudo,ClkDma,ClkI2cRate,ClkI2c6Main};
static EFI_STATUS EFIAPI ClockId(CONST CHAR8 *N,UINT32 *Id) {
  for(UINTN I=0;I<ARRAY_SIZE(ClockNames);++I) if(!strcmp(N,ClockNames[I])) {
    *Id=ClockIds[I]; return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI ClockName(UINT32 Id,CONST CHAR8 **N) {
  for(UINTN I=0;I<ARRAY_SIZE(ClockIds);++I) if(Id==ClockIds[I]) {
    *N=(ClockFailure==3 && Id==ClkParent)?"UNSUPPORTED_PARENT":ClockNames[I];
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI ClockOn(UINT32 Id,BOOLEAN *On) {
  *On=ClockFailure!=1 && !(ClockFailure==5 && Id==ClkI2c6Main);
  if(Id==ClkSs||Id==ClkUsb||Id==ClkPll) {
    UINTN I=Id==ClkSs?0U:(Id==ClkUsb?1U:2U); *On=*On&&!(ColdUsbOffMask&(1U<<I));
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ClockHz(UINT32 Id,UINT64 *Hz) {
  *Hz=(Id==ClkPll)?(ColdPllBlank?0ULL:ColdPllProgrammedHz):
    ((Id==ClkSs||Id==ClkUsb||Id==ClkParent||Id==ClkI2cRate)?124800000ULL:26000000ULL);
  if(ClockFailure==6 && Id==ClkPll) *Hz=480000000;
  if(ClockFailure==2 && Id==ClkRef) *Hz=24000000;
  if(ClockFailure==4 && Id==ClkUsb) ++*Hz;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ClockSetFreq(UINT32 Id,UINT64 Hz) {
  CHECK(Id==ClkPll);
  ++ColdPllSetCalls;
  if(ColdPllSetFail) return EFI_DEVICE_ERROR;
  ColdPllBlank=FALSE;
  ColdPllProgrammedHz=Hz;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ClockParent(UINT32 Id,UINT32 *Parent) {
  CHECK(Id==ClkUsb || Id==ClkI2c || Id==ClkAxi); *Parent=ClkParent; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ClockEnable(UINT32 Id,BOOLEAN On) {
  CHECK(Id==ClkSs||Id==ClkUsb||Id==ClkPll);
  CHECK(mHandoff.ColdInProgress||mHandoff.ColdClockBound);
  CHECK(ColdClockWrites<ARRAY_SIZE(ColdClockTrace));
  ColdClockTrace[ColdClockWrites++]=(Id<<1)|(On?1U:0U);
  UINTN I=Id==ClkSs?0U:(Id==ClkUsb?1U:2U);
  if(ColdClockWrites!=ColdClockIgnore) {
    if(On) ColdUsbOffMask&=(UINT8)~(1U<<I); else ColdUsbOffMask|=(UINT8)(1U<<I);
  }
  return ColdClockWrites==ColdClockFailure?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static MTK_CLOCK_PROTOCOL Clock={.GetId=ClockId,.GetName=ClockName,.SetEnable=ClockEnable,
  .IsEnabled=ClockOn,.GetFrequency=ClockHz,.SetFrequency=ClockSetFreq,.GetParent=ClockParent};
static EFI_STATUS EFIAPI GpioDir(UINT32 Pin,BOOLEAN *Dir) {
  CHECK(Pin<ARRAY_SIZE(PinDirections)); *Dir=PinDirections[Pin]!=0U; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GpioState(UINT32 Pin,BOOLEAN *State) {
  *State=Pin==48?SourcePin48:(Pin==86?SourcePin86:
    (Pin==99?HubLevels[0]:(Pin==4?HubLevels[1]:(Pin==104?HubLevels[2]:Pin!=10))));
  if(Pin==10) *State=!PogoDocked;
  return EFI_SUCCESS;
}
static VOID PinPhysicalFromOutput(UINT32 Pin) {
  UINT32 Mode=(U32(Pins+PIN_MODE_OFFSET(Pin))>>PIN_MODE_SHIFT(Pin))&0xfU;
  if(Mode||!PinDirections[Pin]) return;
  UINT8 State=(UINT8)((U32(Pins+0x100U+(Pin/32U)*0x10U)>>(Pin%32U))&1U);
  if(Pin==48U) SourcePin48=State; else if(Pin==86U) SourcePin86=State;
  else if(Pin==99U) HubLevels[0]=State; else if(Pin==4U) HubLevels[1]=State;
  else if(Pin==104U) HubLevels[2]=State;
}
static EFI_STATUS EFIAPI GpioSetDirection(UINT32 Pin,BOOLEAN Dir) {
  CHECK(mHandoff.ColdPinBound&&Pin<ARRAY_SIZE(PinDirections));
  ++ColdPinWrites;
  if(ColdPinWrites!=ColdPinIgnore) {
    PinDirections[Pin]=Dir?1U:0U;
    UINT32 O=(Pin/32U)*0x10U,M=1U<<(Pin%32U);
    Put32(Pins+O,(U32(Pins+O)&~M)|(Dir?M:0U)); PinPhysicalFromOutput(Pin);
  }
  return ColdPinWrites==ColdPinFailure?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GpioSetMode(UINT32 Pin,UINT32 Mode) {
  CHECK(mHandoff.ColdPinBound&&Pin<ARRAY_SIZE(PinDirections)&&Mode<8U);
  ++ColdPinWrites;
  if(ColdPinWrites!=ColdPinIgnore) {
    UINT32 O=PIN_MODE_OFFSET(Pin),Shift=PIN_MODE_SHIFT(Pin),M=0xfU<<Shift;
    Put32(Pins+O,(U32(Pins+O)&~M)|(Mode<<Shift)); PinPhysicalFromOutput(Pin);
  }
  return ColdPinWrites==ColdPinFailure?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GpioSet(UINT32 Pin,BOOLEAN State) {
  CHECK(mHandoff.ColdPinBound && Pin < ARRAY_SIZE(PinDirections));
  CHECK(!mHandoff.ExitingBootServices);
  UINT32 Offset = 0x100U + (Pin / 32U) * 0x10U;
  UINT32 Mask = 1U << (Pin % 32U);
  ++ColdPinWrites;
  if (ColdPinWrites != ColdPinIgnore) {
    Put32(Pins + Offset, (U32(Pins + Offset) & ~Mask) | (State ? Mask : 0U));
    PinPhysicalFromOutput(Pin);
  }
  if (Pin == 48U || Pin == 86U) {
    if (mHandoff.OtgBusy) {
      CHECK(LeaseToken && SourceWrites < ARRAY_SIZE(SourceTrace));
      SourceTrace[SourceWrites++] = (Pin << 8U) | (State ? 1U : 0U);
    }
  } else {
    CHECK(Pin == 99U || Pin == 4U || Pin == 104U);
    CHECK(!mHandoff.ReadyInstalled && !mHandoff.ColdBound);
    CHECK(HubWrites < ARRAY_SIZE(HubTracePin));
    HubTracePin[HubWrites] = Pin;
    HubTraceValue[HubWrites++] = State;
    if (IdLowHubVbus && HubLevels[0] && HubLevels[1] && HubLevels[2])
      Usb[0x60] |= 0x18U;
    else if (!PowerOn)
      Usb[0x60] &= (UINT8)~0x18U;
  }
  return ColdPinWrites == ColdPinFailure ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static MTK_GPIO_PROTOCOL Gpio={.GetDir=GpioDir,.GetState=GpioState,.SetState=GpioSet,
  .SetDir=GpioSetDirection,.SetMode=GpioSetMode};
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **P) {
  CHECK(R==NULL);
  if(G==&gMediaTekClockProtocolGuid) { if(LocateFailure==1)return EFI_NOT_FOUND; *P=&Clock; }
  else if(G==&gMediaTekGpioProtocolGuid) { if(LocateFailure==2)return EFI_NOT_FOUND; *P=&Gpio; }
  else if(G==&gEfiDxeSmmReadyToLockProtocolGuid&&LateReadyToLock) { *P=NULL; }
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handles(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *K,UINTN *N,EFI_HANDLE **H) {
  CHECK(T==ByProtocol && G==&gMediaTekI2cControllerProtocolGuid && K==NULL);
  if(!ControllerCount) return EFI_NOT_FOUND;
  *N=ControllerCount; *H=malloc(*N*sizeof(**H)); CHECK(*H);
  for(UINTN I=0;I<*N;++I) (*H)[I]=BusHandle;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI HandleProtocol(EFI_HANDLE H,EFI_GUID *G,VOID **P) {
  if(H==UsbHandle) {
    if(EFI_ERROR(UsbHostProtocolStatus))return UsbHostProtocolStatus;
    if(G==&gEfiUsb2HcProtocolGuid&&(UsbHostProtocolMask&1U)) { *P=(VOID *)(UINTN)0x701U;return EFI_SUCCESS; }
    if(G==&gEfiDevicePathProtocolGuid&&(UsbHostProtocolMask&2U)) { *P=(VOID *)(UINTN)0x702U;return EFI_SUCCESS; }
    STATIC CONST EFI_GUID UsbBusGuid = {0x240612b7, 0xa063, 0x11d4,
      {0x9a, 0x3a, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
    if ((UsbHostProtocolMask & 4U) && !memcmp(G, &UsbBusGuid, sizeof(*G))) {
      *P = (VOID *)(UINTN)0x703U;
      return EFI_SUCCESS;
    }
    return EFI_UNSUPPORTED;
  }
  if(H!=BusHandle) return EFI_UNSUPPORTED;
  if(G==&gMediaTekI2cControllerProtocolGuid) *P=&Controller;
  else if(G==&gEfiI2cMasterProtocolGuid && MasterPresent) *P=&Master;
  else if(G==&gMediaTekI2cExclusiveProtocolGuid && ExclusivePresent) *P=&Exclusive;
  else if(G==&gStockPowerBoardReadyProtocolGuid && InstalledBoard) *P=InstalledBoard;
  else return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI InstallHostDiagnostic(EFI_HANDLE *H,...) {
  VA_LIST A; VA_START(A,H);
  CHECK(VA_ARG(A,EFI_GUID *)==&mHostDiagnosticsGuid);
  CHECK(VA_ARG(A,VOID *)==&mHostDiagnostics);
  CHECK(VA_ARG(A,EFI_GUID *)==&mTypecDiagnosticsGuid);
  CHECK(VA_ARG(A,VOID *)==&mTypecDiagnostics);
  CHECK(VA_ARG(A,VOID *)==NULL); VA_END(A);
  CHECK(mHostDiagnostics.Version==T70_USB_HOST_DIAG_VERSION && mHostDiagnostics.Read==ReadHostDiagnostics);
  *H=(EFI_HANDLE)(UINTN)0x1234;
  return EFI_SUCCESS;
}
static EFI_TPL EFIAPI DiagRaise(EFI_TPL T) { CHECK(T==TPL_NOTIFY);return TPL_APPLICATION; }
static VOID EFIAPI DiagRestore(EFI_TPL T) { CHECK(T==TPL_APPLICATION); }
static EFI_STATUS EFIAPI InstallProtocol(EFI_HANDLE *H,EFI_GUID *G,EFI_INTERFACE_TYPE T,VOID *P) {
  CHECK(T==EFI_NATIVE_INTERFACE);
  if(G==&gStockPowerBoardReadyProtocolGuid) {
    CHECK(*H==BusHandle); ++BoardInstalls;
    if(FailInstall==1)return EFI_OUT_OF_RESOURCES;
    InstalledBoard=P;
  } else {
    CHECK(G==&gMtkMusbHostPlatformReadyProtocolGuid && *H==NULL);
    ++ReadyInstalls; if(FailInstall==2)return EFI_OUT_OF_RESOURCES;
    *H=UsbHandle; InstalledReady=P;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventCreate(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY N,CONST VOID *C,
                                     CONST EFI_GUID *G,EFI_EVENT *E) {
  CHECK(C==&mHandoff);
  CHECK(Type==(N==UsbRoleTick?(EVT_TIMER|EVT_NOTIFY_SIGNAL):EVT_NOTIFY_SIGNAL));
  if(N==UsbRoleTick) CHECK(Tpl==TPL_CALLBACK && G==NULL);
  CHECK(EventCount<ARRAY_SIZE(Events));
  ++EventCount; if(EventFail==EventCount)return EFI_OUT_OF_RESOURCES;
  Events[EventCount-1]=(TEST_EVENT){Tpl,N,C,G};
  *E=(VOID *)(UINTN)EventCount;
  if(N==EbsPowerNotify) PowerEventSequence=++Sequence;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventCreatePlain(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY N,VOID *C,EFI_EVENT *E) {
  return EventCreate(T,P,N,C,NULL,E);
}
static EFI_STATUS EFIAPI RetrySetTimer(EFI_EVENT Event,EFI_TIMER_DELAY Type,UINT64 Period) {
  CHECK(Event==mHandoff.RetryTimerEvent && Event!=NULL);
  if(Type==TimerCancel) { CHECK(Period==0);RetryCancels++;return EFI_SUCCESS; }
  CHECK(Type == TimerPeriodic && Period == USB_ROLE_PERIOD);
  ++RetryTimers;
  return RetryTimerStatus;
}
static EFI_STATUS EFIAPI Close(EFI_EVENT E) {
  CHECK(E!=NULL); ++CloseCalls;
  return CloseFail?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI LeaseAcquire(MTK_I2C_EXCLUSIVE_PROTOCOL *This,UINT64 *Token) {
  CHECK(This==&Exclusive&&Token);
  if (BusQuiescing) return EFI_NOT_READY;
  if (LeaseToken) return EFI_ACCESS_DENIED;
  if (EFI_ERROR(LeaseAcquireStatus)) return LeaseAcquireStatus;
  *Token=LeaseToken=++NextToken; ++LeaseAcquires;
  if(IdLowDropSinkBeforePreflight&&LeaseAcquires==2U) { PowerOn=FALSE; CcRaw=0U; }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI LeaseRelease(MTK_I2C_EXCLUSIVE_PROTOCOL *This,UINT64 Token) {
  CHECK(This==&Exclusive&&Token==LeaseToken&&Token);
  if(FailLeaseRelease) return EFI_DEVICE_ERROR;
  LeaseToken=0U; ++LeaseReleases; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI LeaseTransfer(MTK_I2C_EXCLUSIVE_PROTOCOL *This,UINT64 Token,
  UINTN Address,EFI_I2C_REQUEST_PACKET *P,EFI_STATUS *Status) {
  EFI_STATUS S=EFI_SUCCESS;
  CHECK(This==&Exclusive&&Token&&Token==LeaseToken&&!BusQuiescing);
  CHECK(Address==SC_ADDRESS||Address==TCPC_ADDRESS);
  CHECK(P&&P->Operation[0].Flags==0U&&P->Operation[0].Buffer);
  UINT8 Reg=P->Operation[0].Buffer[0];
  if(P->OperationCount==2U) {
    CHECK(P->Operation[0].LengthInBytes==1U&&P->Operation[1].Flags==I2C_FLAG_READ);
    UINTN Bytes=P->Operation[1].LengthInBytes; UINT8 *Out=P->Operation[1].Buffer;
    CHECK((Bytes==1U||Bytes==2U)&&Out); ++RawReads;
    if(DeviceIrqDriftAt&&RawReads==DeviceIrqDriftAt) {
      if(DeviceIrqDriftKind==0U)PogoDocked=FALSE;
      if(DeviceIrqDriftKind==1U)BoostOn=TRUE;
      if(DeviceIrqDriftKind==2U)PowerOn=TRUE;
      if(DeviceIrqDriftKind==3U)CcRaw=2U;
      if(DeviceIrqDriftKind==4U)CcRole=0x0aU;
      if(DeviceIrqDriftKind==5U)TcpcRegisters[0x1f]=1U;
      if(DeviceIrqDriftKind==6U)TxCsr[5]=1U;
    }
    if(Address==TCPC_ADDRESS&&Reg==0x1cU&&VconnWrites&&VconnReadFailAfterWrite) {
      VconnReadFailAfterWrite=FALSE;S=EFI_DEVICE_ERROR;
    } else if(Address==TCPC_ADDRESS&&Reg==0x97U&&FailVsafeReadOnce) {
      FailVsafeReadOnce=FALSE; S=EFI_DEVICE_ERROR;
    } else if(Address==TCPC_ADDRESS&&SenseRmwReadFailReg&&Reg==SenseRmwReadFailReg&&
              SenseWrites==(Reg==0x99U?1U:2U)) {
      SenseRmwReadFailReg=0U;S=EFI_DEVICE_ERROR;
    } else if(Address==TCPC_ADDRESS&&SenseReadFailAfterWrite&&
              SenseWrites==SenseReadFailAfterWrite&&Reg==SenseTraceReg[SenseWrites-1U]) {
      SenseReadFailAfterWrite=0U;S=EFI_DEVICE_ERROR;
    } else if(RawReads==RawReadFailure||(Address==SC_ADDRESS&&BoostFailure)||
       (Address==TCPC_ADDRESS&&TcpcFailure)||(Address==TCPC_ADDRESS&&Reg==0x1dU&&CcFailure)||
       (Address==TCPC_ADDRESS&&Reg==0x97U&&(FailVsafeRead||
        (FailVsafeDrainRead&&mHandoff.OtgCleaning&&
         mHandoff.OtgCore.State==MtkSc8989xOtgStateIdle))))
      S=EFI_DEVICE_ERROR;
    else if(Address==SC_ADDRESS) {
      CHECK(Bytes==1U);
      *Out=Reg==3U?(UINT8)((ScRegisters[3]&~0x20U)|(BoostOn?0x20U:0U)):ScRegisters[Reg];
      if(Reg==3U&&FinalBoostMutation&&SourceWrites==4U&&
         mHandoff.OtgCore.State==MtkSc8989xOtgStateTransition&&
         (mHandoff.OtgCore.OwnedMask&0x10U)!=0U) {
        if(FinalBoostMutation==1U) VendorStatusReg97&=(UINT8)~0x02U;
        if(FinalBoostMutation==2U) PowerOn=TRUE;
        if(FinalBoostMutation==3U) CcRaw=0U;
        if(FinalBoostMutation==4U) CcRole|=0x40U;
        if(FinalBoostMutation==5U) FailVsafeReadOnce=TRUE;
        FinalBoostMutation=0U; FinalBoostMutationObserved=TRUE;
      }
    } else if(Reg==0U||Reg==2U) {
      CHECK(Bytes==2U); Out[0]=((DeviceBadTcpcId&&Reg==0U)||(DeviceBadTcpcProduct&&Reg==2U))?0U:(Reg==0U?0x99U:0x11U); Out[1]=Reg==0U?0x2eU:0x03U;
    } else if(ColdTcpcModel&&Reg!=0x1aU&&Reg!=0x1dU&&Reg!=0x1eU&&Reg!=0x97U) {
      CHECK(Reg==0x10U||Reg==0x19U||Reg==0x1cU||Reg==0x1fU||
            Reg==0x2eU||Reg==0x2fU||Reg==0x98U||
            Reg==0x90U||Reg==0x99U||Reg==0x9bU);
      CHECK((Reg==0x10U&&Bytes==2U)||(Reg!=0x10U&&Bytes==1U));
      if(Reg==0x90U&&++Sense90Reads==SenseOscillatorAt)TcpcRegisters[Reg]|=1U;
      if(Reg==0x1cU&&VconnSettleReads&&SenseWrites&&TcpcRegisters[0x9b]==0x38U&&
         --VconnSettleReads==0U)TcpcRegisters[Reg]=VconnValue;
      for(UINTN N=0;N<Bytes;++N) Out[N]=TcpcRegisters[(UINTN)Reg+N];
    } else {
      CHECK(Bytes==1U&&(Reg==0x1aU||Reg==0x1dU||Reg==0x1eU||Reg==0x97U));
      if(Reg==0x97U) {
        ++VsafeReads;
        if(VsafeReads==VsafeUnsafeAt) VendorStatusReg97&=(UINT8)~0x02U;
        if(mHandoff.OtgCleaning&&mHandoff.OtgCore.State==MtkSc8989xOtgStateIdle) {
          ++DrainReads; CHECK(DrainReads<=ARRAY_SIZE(DrainReadTimes));
          DrainReadTimes[DrainReads-1U]=TimeNs;
          if(DrainReleaseAt&&DrainReads>=DrainReleaseAt) {
            PowerOn=FALSE; VendorStatusReg97|=0x02U; Usb[0x60]&=(UINT8)~0x18U;
          }
          if(DrainBounceOnce) {
            // Initial full sample unsafe; quick sample safe; first full
            // confirmation unsafe; next quick/full pair stable safe.
            PowerOn=FALSE;
            VendorStatusReg97=(DrainReads==1U||DrainReads==3U)?0U:0x02U;
          }
          if(ReassertPinOnDrain) SourcePin86=1U;
        }
        *Out=VendorStatusReg97;
      } else
        *Out=Reg==0x1aU?CcRole:(Reg==0x1dU?CcRaw:
          ((PowerOn?0x04U:0U)|(VbusDetectEnabled?0x08U:0U)|
           ((VconnStatusStuck||(VconnSupply&&(TcpcRegisters[0x1c]&1U)))?2U:0U)));
    }
  } else if(Address==TCPC_ADDRESS&&Reg==0x1cU) {
    CHECK(ColdTcpcModel&&P->OperationCount==1U&&P->Operation[0].LengthInBytes==2U&&
          ((mHandoff.DeviceDisconnectInProgress && mHandoff.SenseAttempted) ||
           (mHandoff.DeviceSettled && ColdUsbOffMask != 0U)) &&
          !mHandoff.ExitingBootServices&&!mHandoff.ReadyInstalled&&!SenseWrites);
    UINT8 V=P->Operation[0].Buffer[1];
    CHECK(VconnWrites==0U&&(V&1U)==0U&&
          (V&0xfeU)==(TcpcRegisters[Reg]&0xfeU)&&
          (!mHandoff.DeviceDisconnectInProgress || TimeNs>=20000000U));
    VconnValue=V;++VconnWrites;VconnWriteAt=TimeNs;
    if(!VconnWriteIgnore&&!VconnSettleReads)TcpcRegisters[Reg]=V;
    if(VconnDriftAfterWrite==1U)PogoDocked=FALSE;
    if(VconnDriftAfterWrite==2U)BoostOn=TRUE;
    if(VconnDriftAfterWrite==3U)PowerOn=TRUE;
    if(VconnDriftAfterWrite==4U)CcRaw=2U;
    if(VconnDriftAfterWrite==5U)CcRole=0x0aU;
    if(VconnDriftAfterWrite==6U)TcpcRegisters[0x1f]=1U;
    if(VconnDriftAfterWrite==7U)TxCsr[5]=1U;
    if(VconnWriteFailure)S=EFI_DEVICE_ERROR;
  } else if(Address==TCPC_ADDRESS&&(Reg==0x90U||Reg==0x99U||Reg==0x9bU)) {
    CHECK(ColdTcpcModel&&P->OperationCount==1U&&P->Operation[0].LengthInBytes==2U&&
          mHandoff.SenseOwnedMask&&!mHandoff.ExitingBootServices&&
          (mHandoff.DeviceDisconnectInProgress || mHandoff.DeviceSettled ||
           (mHandoff.SenseSaved[(Reg == 0x9bU) ? 0U : (Reg == 0x99U) ? 1U : 2U] &
            (Reg == 0x9bU ? 0xffU : 2U)) ==
           (P->Operation[0].Buffer[1] & (Reg == 0x9bU ? 0xffU : 2U))) &&
          !mHandoff.ColdTypecBound&&!mHandoff.OtgBound);
    CHECK(SenseWrites<ARRAY_SIZE(SenseTraceReg));
    UINT8 V=P->Operation[0].Buffer[1];
    SenseTraceReg[SenseWrites]=Reg;SenseTraceValue[SenseWrites]=V;++SenseWrites;
    if(SenseWrites!=SenseWriteIgnore)TcpcRegisters[Reg]=V;
    if(SenseWrites==SenseWriteCorruptAt)TcpcRegisters[Reg]=SenseWriteCorruptValue;
    if(Reg==0x90U&&SenseWrites!=SenseWriteIgnore) {
      VbusDetectEnabled=!SenseStuck&&(TcpcRegisters[Reg]&2U)!=0U;
      if(VbusDetectEnabled&&SenseWakeVbus)PowerOn=TRUE;
      VendorStatusReg97=(VbusDetectEnabled&&!PowerOn)?2U:0U;
    }
    if(SenseWrites==SenseWriteFailure)S=EFI_DEVICE_ERROR;
  } else if(Address==TCPC_ADDRESS) {
    CHECK(ColdTcpcModel&&P->OperationCount==1U&&P->Operation[0].LengthInBytes==2U&&
          mHandoff.ColdTypecBusy&&!mHandoff.ExitingBootServices&&
          (Reg==0x1aU||Reg==0x2eU||Reg==0x2fU||Reg==0x98U));
    CHECK(ColdTcpcWrites<ARRAY_SIZE(ColdTcpcTraceReg));
    if(!ColdTcpcWrites) {
      ColdTcpcInitialRole=CcRole;
      ColdTcpcInitialRx=TcpcRegisters[0x2fU];
    }
    UINT8 V=P->Operation[0].Buffer[1];
    ColdTcpcTraceReg[ColdTcpcWrites]=Reg; ColdTcpcTraceValue[ColdTcpcWrites]=V; ++ColdTcpcWrites;
    if(ColdTcpcWrites!=ColdTcpcIgnore) {
      if(Reg==0x98U) {
        CHECK(V==2U);TcpcRegisters[Reg]&=(UINT8)~V;
      } else TcpcRegisters[Reg]=V;
      if(Reg==0x1aU) {
        CcRole=V;
        CcRaw=(V&0x7fU)==0x05U&&ColdAutoAttach?ColdAttachCc:0U;
        if(CcRaw&&ColdIdDropsOnAttach)PogoDocked=TRUE;
      }
    }
    if(ColdTcpcWrites==ColdTcpcFailure) S=EFI_DEVICE_ERROR;
  } else {
    CHECK(P->OperationCount==1U&&Address==SC_ADDRESS&&P->Operation[0].LengthInBytes==2U);
    CHECK((Reg==0U||Reg==3U||Reg==0x0aU)&&mHandoff.OtgBusy&&!mHandoff.ExitingBootServices);
    if(Reg==0x0aU)
      CHECK(!BoostOn&&SourcePin86==0U&&mHandoff.DeviceSettled);
    CHECK(SourceWrites<ARRAY_SIZE(SourceTrace)); UINT8 V=P->Operation[0].Buffer[1];
    SourceTrace[SourceWrites++]=(UINT32)(0x1000000U|((UINT32)Reg<<8)|V);
    ScRegisters[Reg]=V;
    if(Reg==3U) {
      BoostOn=(V&0x20U)!=0U;
      if(AutoVbus) {
        PowerOn=BoostOn; VendorStatusReg97=PowerOn?0U:0x02U;
        BOOLEAN HubVbus = IdLowHubVbus && HubLevels[0] && HubLevels[1] && HubLevels[2];
        Usb[0x60] = (UINT8)((Usb[0x60] & ~0x18U) | ((PowerOn || HubVbus) ? 0x18U : 0U));
      }
    }
    if(SourceWrites==SourceWriteFailure) S=EFI_DEVICE_ERROR;
  }
  if(Status) *Status=S;
  return S;
}
static EFI_STATUS EFIAPI RegisterObserver(MTK_I2C_EXCLUSIVE_PROTOCOL *This,
 EFI_EVENT_NOTIFY Observer,VOID *Context) {
  CHECK(This==&Exclusive&&Observer&&Context==&mHandoff);
  if(FailObserver) return EFI_ACCESS_DENIED;
  if(ExitObserver) return ExitObserver==Observer&&ExitObserverContext==Context?
     EFI_ALREADY_STARTED:EFI_ACCESS_DENIED;
  ExitObserver=Observer; ExitObserverContext=Context; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI UnregisterObserver(MTK_I2C_EXCLUSIVE_PROTOCOL *This,
 EFI_EVENT_NOTIFY Observer,VOID *Context) {
  CHECK(This==&Exclusive&&Observer==ExitObserver&&Context==ExitObserverContext);
  ExitObserver=NULL; ExitObserverContext=NULL; return EFI_SUCCESS;
}
static VOID ControllerExit(VOID) {
  CHECK(ExitObserver&&!InsideExitObserver);
  InsideExitObserver=TRUE; ExitObserver(NULL,ExitObserverContext); InsideExitObserver=FALSE;
  BusQuiescing=TRUE;
}
static EFI_STATUS EFIAPI Request(CONST EFI_I2C_MASTER_PROTOCOL *This,UINTN Address,
 EFI_I2C_REQUEST_PACKET *Packet,EFI_EVENT E,EFI_STATUS *S) {
  (VOID)This;(VOID)Address;(VOID)Packet;(VOID)E;(VOID)S; CHECK(0); return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI ReleaseTestHost(VOID *Context) {
  CHECK(Context==UsbHandle&&InstalledReady&&mHandoff.ReadyInstalled&&
    mHandoff.HostChildrenDisconnected&&mHandoff.ConnectInProgress);
  ++HostReleases;
  if(EFI_ERROR(HostReleaseStatus))return HostReleaseStatus;
  mHandoff.Ready.ReleaseHost=NULL;mHandoff.Ready.HostContext=NULL;
  if(!UsbHostRetainProtocolAfterRelease)UsbHostProtocolMask=0U;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI DisconnectTestHost(EFI_HANDLE H,EFI_HANDLE D,EFI_HANDLE Child) {
  CHECK(H==UsbHandle&&!D&&!Child&&InstalledReady&&mHandoff.ConnectInProgress);
  ++HostDisconnects;
  UINTN Before=HostDisconnects,BeforeReady=ReadyInstalls;
  UsbRoleTick(mHandoff.RetryTimerEvent,&mHandoff);
  CHECK(HostDisconnects==Before&&ReadyInstalls==BeforeReady);
  return HostDisconnectStatus;
}
static EFI_STATUS EFIAPI UninstallTestReady(EFI_HANDLE H,EFI_GUID *Guid,VOID *Interface) {
  CHECK(H==UsbHandle&&Guid==&gMtkMusbHostPlatformReadyProtocolGuid&&
    Interface==&mHandoff.Ready&&
    !mHandoff.Ready.ReleaseHost&&!mHandoff.Ready.HostContext&&UsbHostProtocolMask==0U);
  ++ReadyUninstalls;
  if(!EFI_ERROR(ReadyUninstallStatus))InstalledReady=NULL;
  return ReadyUninstallStatus;
}
static EFI_STATUS EFIAPI Connect(EFI_HANDLE H,EFI_HANDLE *D,EFI_DEVICE_PATH_PROTOCOL *P,BOOLEAN Recursive) {
  if(H==UsbHandle) {
    CHECK(!D && !P && !Recursive && InstalledReady && mHandoff.ConnectInProgress);
    UsbHostConnects++;
    if(!EFI_ERROR(UsbHostConnectStatus)||UsbHostPublishOnFailure) {
      mHandoff.Ready.ReleaseHost=ReleaseTestHost;mHandoff.Ready.HostContext=UsbHandle;
      UsbHostProtocolMask = UsbBusStartFails ? 3U : 7U;
    }
    // A failed install cannot erase an interface that was already present.
    if(UsbHostReenter) {
      UsbHostReenter=FALSE;UsbRoleTick(mHandoff.RetryTimerEvent,&mHandoff);
      CHECK(UsbHostConnects==1);
    }
    return UsbHostConnectStatus;
  }
  CHECK(H==BusHandle && !D && !P && !Recursive);
  CHECK(PowerEventSequence != 0U);
  ++Connects;
  ConnectSequence = ++Sequence;
  if(EFI_ERROR(I2cConnectStatus))return I2cConnectStatus;
  if(!I2cConnectNoMaster)MasterPresent=TRUE;
  return I2cConnectStatus;
}
static VOID Reset(VOID) {
  if(mHandoff.PhyBound) { mHandoff.ExitingBootServices=FALSE; CHECK(ReleaseOwnedPhy()==EFI_SUCCESS); }
  FreePool(mHandoff.ValidatedBoardTree);
  memset(&mHandoff,0,sizeof(mHandoff));
  memset(&Services,0,sizeof(Services)); memset(&Dxe,0,sizeof(Dxe));
  memset(Usb,0,sizeof(Usb)); memset(Phy,0,sizeof(Phy)); memset(Pins,0,sizeof(Pins));
  memset(TxCsr,0,sizeof(TxCsr)); memset(RxCsr,0,sizeof(RxCsr));
  memset(Events,0,sizeof(Events)); memset(HandoffWords,0xa5,sizeof(HandoffWords));
  Reads=UsbReads=Writes=IndexWrites=AckWrites=CacheCalls=PowerReads=Connects=EventCount=0;
  ControllerMapBad=FALSE;
  RetryTimers=ProbeTimers=RetryCancels=UsbHostConnects=0;
  RetryTimerStatus=ProbeTimerStatus=UsbHostConnectStatus=I2cConnectStatus=EFI_SUCCESS;
  UsbHostReenter=I2cConnectNoMaster=FALSE;
  UsbHostPublishOnFailure=FALSE;UsbHostRetainProtocolAfterRelease=FALSE;
  UsbHostProtocolMask=0U;UsbHostProtocolStatus=EFI_SUCCESS;UsbBusStartFails=FALSE;
  HostDisconnectStatus=HostReleaseStatus=ReadyUninstallStatus=EFI_SUCCESS;
  HostDisconnects=HostReleases=ReadyUninstalls=0U;
  EventFail=CloseCalls=CloseFail=RegistrationCalls=RegistrationFail=0;
  ReadyInstalls=BoardInstalls=FailInstall=LocateFailure=ControllerCount=LiveBad=MapBad=0;
  LateReadyToLock=FALSE; PogoDocked=TRUE;
  BoostFailure=TcpcFailure=ClockFailure=CcFailure=0;
  ColdPllBlank=ColdPllSetFail=FALSE; ColdPllSetCalls=0U; ColdPllProgrammedHz=2496000000ULL;
  ColdUsbOffMask=0U; ColdClockWrites=ColdClockFailure=ColdClockIgnore=0U;
  ColdPinWrites=ColdPinFailure=ColdPinIgnore=0U;
  ColdTcpcModel=ColdUsbIgnore=ColdHeaderDriftOnSession=FALSE; ColdAutoAttach=TRUE; IdLowHubVbus=IdLowDropSinkBeforePreflight=FALSE;
  ColdAttachCc=2U; ColdIdDropsOnAttach=FALSE;
  ColdTcpcWrites=ColdTcpcFailure=ColdTcpcIgnore=ColdUsbWrites=0U;
  InheritedDeviceModel=DeviceStopReenter=DeviceStopIdDrift=DeviceBadTcpcId=DeviceStopHubDrift=DeviceBadTcpcProduct=FALSE;
  DeviceStopWrites=DeviceStopIgnore=0U;
  DeviceResetWrites=DeviceResetFault=DeviceResetDrift=0U;DeviceResetValue=0U;DeviceResetAt=0U;
  DeviceIrqWrites=DeviceIrqIgnore=DeviceIrqDriftAt=DeviceIrqDriftKind=0U;
  DeviceIrqFirstWriteNs=0U;
  DeviceCsrWrites=DeviceCsrIgnore=0U;
  memset(DeviceCsrEndpoint,0,sizeof(DeviceCsrEndpoint));
  memset(DeviceCsrOffset,0,sizeof(DeviceCsrOffset));memset(DeviceCsrValue,0,sizeof(DeviceCsrValue));
  memset(DeviceIrqOffsets,0,sizeof(DeviceIrqOffsets));memset(DeviceIrqWidths,0,sizeof(DeviceIrqWidths));
  SenseWrites=SenseWriteFailure=SenseWriteIgnore=SenseWriteCorruptAt=SenseReadFailAfterWrite=0U;
  SenseWriteCorruptValue=0U;SenseStuck=SenseWakeVbus=FALSE;Sense90Reads=SenseOscillatorAt=0U;SenseRmwReadFailReg=0U;
  VconnSupply=VconnStatusStuck=VconnWriteFailure=VconnWriteIgnore=VconnReadFailAfterWrite=FALSE;
  VconnWrites=VconnCheckReads=VconnDriftAfterWrite=VconnSettleReads=0U;VconnValue=0U;VconnWriteAt=0U;
  ClockMapBadAddress = 0U;
  ColdHostModeStuck = ColdCorruptGuardPhy = FALSE;
  ColdHostAfterPower=ColdPowerBeforeHostObserved=FALSE;
  ColdDropSessionAtReset=ColdSessionLostAtReset=ColdBlockSessionRearm=FALSE;
  memset(ColdDmaControlAfterClear,0,sizeof(ColdDmaControlAfterClear));
  ColdResetResponse=ColdResetMaskWrites=0U;ColdResetStickyMask=0U;
  memset(ColdResetMaskOffsets,0,sizeof(ColdResetMaskOffsets));
  ColdResetResponseInjected=ColdResetAckBeforePower=FALSE;
  PhysicalReplayMode=PhysicalReplayPowerReads=PhysicalReplayTraceCount=0U;
  PhysicalReplayPowerWritten=FALSE;
  memset(SenseTraceReg,0,sizeof(SenseTraceReg));memset(SenseTraceValue,0,sizeof(SenseTraceValue));
  memset(TcpcRegisters,0,sizeof(TcpcRegisters));
  memset(ColdTcpcTraceReg,0,sizeof(ColdTcpcTraceReg));
  memset(ColdTcpcTraceValue,0,sizeof(ColdTcpcTraceValue));
  memset(ColdClockTrace,0,sizeof(ColdClockTrace));
  memset(PinDirections,1,sizeof(PinDirections)); PinDirections[10]=0U;
  CcRaw=2; CcRole=5; memset(HubLevels,1,sizeof(HubLevels));
  HubWrites=HubWriteFailure=HubIgnoreWrite=HubFailOff=0;
  memset(HubTracePin,0,sizeof(HubTracePin)); memset(HubTraceValue,0,sizeof(HubTraceValue));
  HubDropSource=HubBusyAfterWrite=HubReenter=HubChangeOnCc=FALSE;
  ColdHubMuxChangedDuringSession=FALSE;
  HubMutateAfterWrite=HubMutatePin=HubMutateValue=0;
  HubLowWrites=HubReassertAfterLow=0; HubFinalReassert=PostHubEvidenceFailure=FALSE;
  Sequence=PowerEventSequence=ConnectSequence=0;
  PhyWrites=FifoWrites=Ep0FifoAccess=0; IgnoreFifoWrites=ReenterOnPhy=FALSE; TimeNs=0;
  ColdPowerDriftOnFifo=0U; ColdFailHostPhy=ColdHostPhyFaultInjected=FALSE; SavedPhyRead=NULL; SavedPhyWrite=NULL;
  MasterPresent=BadMagic=NoTable=StuckAck=FALSE;
  PowerOn=BoostOn=TRUE; ControllerCount=1; InstalledBoard=NULL; InstalledReady=NULL;
  ExitObserver=NULL; ExitObserverContext=NULL; LeaseToken=NextToken=0U;
  LeaseAcquires=LeaseReleases=RawReads=SourceWrites=SourceWriteFailure=RawReadFailure=0U;
  memset(ScRegisters,0,sizeof(ScRegisters)); ScRegisters[0]=0x80U; ScRegisters[3]=0x10U;
  ScRegisters[0x14]=0x10U; ScRegisters[0x0a]=0xc3U;
  memset(SourceTrace,0,sizeof(SourceTrace)); SourcePin48=SourcePin86=1U;
  VendorStatusReg97=0U; VsafeReads=VsafeUnsafeAt=DrainReads=DrainReleaseAt=0U;
  FailVsafeRead=FailVsafeDrainRead=ReassertPinOnDrain=FailVsafeReadOnce=FALSE;
  FinalBoostMutation=0U; FinalBoostMutationObserved=FALSE; DrainBounceOnce=FALSE;
  memset(DrainReadTimes,0,sizeof(DrainReadTimes));
  ExclusivePresent=AutoVbus=VbusDetectEnabled=TRUE; BusQuiescing=InsideExitObserver=FALSE;
  LeaseAcquireStatus=EFI_SUCCESS;
  FailLeaseRelease=FailObserver=SourceReenter=SourceDropAttach=FALSE;
  Exclusive=(MTK_I2C_EXCLUSIVE_PROTOCOL){MTK_I2C_EXCLUSIVE_PROTOCOL_REVISION,
    LeaseAcquire,LeaseTransfer,LeaseRelease,RegisterObserver,UnregisterObserver,NULL};
  Controller=(MTK_I2C_CONTROLLER){MTK_I2C_CONTROLLER_REVISION,MTK_I2C_REQUIRED_FLAGS,
    I2C_BASE,I2C_BYTES,I2C_DMA_BASE,I2C_DMA_BYTES,124800000,0,0,0,EFI_NOT_READY};
  Master.StartRequest=Request;
  Services.LocateProtocol=Locate; Services.LocateHandleBuffer=Handles;
  Services.InstallMultipleProtocolInterfaces=InstallHostDiagnostic;
  Services.RaiseTPL=DiagRaise; Services.RestoreTPL=DiagRestore;
  Services.HandleProtocol=HandleProtocol; Services.InstallProtocolInterface=InstallProtocol;
  Services.CreateEventEx=EventCreate; Services.CreateEvent=EventCreatePlain;
  Services.CloseEvent=Close; Services.SetTimer=RetrySetTimer;
  Services.ConnectController=Connect; Services.DisconnectController=DisconnectTestHost;
  Services.UninstallProtocolInterface=UninstallTestReady;
  Dxe.GetMemorySpaceDescriptor=Descriptor;
  Index=7; Usb[0x60]=0x1d;
  UINT16 Cursor=64;
  for(UINTN I=0;I<=8;++I) {
    if(!I) { TxSize[I]=RxSize[I]=0; TxAddress[I]=RxAddress[I]=0; continue; }
    TxSize[I]=6; RxSize[I]=I==8?3:6; TxAddress[I]=Cursor/8; Cursor+=512;
    RxAddress[I]=Cursor/8; Cursor+=I==8?64:512;
  }
  Put32(Phy+0x110,1); Put32(Phy+0x10c,1024);
  Put32(Phy+0x304,0x4400); Put32(Phy+0x318,0x50100075);
  Put32(Phy+0x368,0); Put32(Phy+0x36c,0x224);
  Put32(Pins+0x410,0x11000000); Put32(Pins+0x240,3U<<14);
}
static VOID Snapshot(VOID) {
  mHandoff.Ready.HostPortStatus=0x501; // connected, powered, high speed
  mHandoff.Ready.HostPortChangeStatus=1;
  mHandoff.Ready.HostSnapshotValid=1;
}
static VOID Ebs(VOID) {
  BeforeExitNotify(mHandoff.BeforeExitEvent, &mHandoff);
  EbsPowerNotify(NULL,NULL); ControllerExit(); Snapshot(); EbsPublishNotify(NULL,NULL);
}
static UINT32 Record(UINTN I) { CHECK(I<ARRAY_SIZE(HandoffWords)); return HandoffWords[I]; }
static VOID SetSyntheticDevinfo(UINT32 Count) {
  UINT8 Tag[440]={0}; Put32(Tag,Count); Put32(Tag+4+0x1b0,0xc3b158ae);
  INT32 Chosen=FdtPathOffset(Tree,"/chosen"); CHECK(Chosen>=0);
  CHECK(FdtSetProp(Tree,Chosen,"atag,devinfo",Tag,sizeof(Tag))==0);
}
static VOID SourceOffFixture(VOID) {
  Reset(); memset(HubLevels,0,sizeof(HubLevels)); MasterPresent=TRUE;
  SourcePin48=SourcePin86=0U; BoostOn=PowerOn=FALSE; VendorStatusReg97=0x02U;
  Usb[0x60]=0x05U;
}
static VOID SetPinInitial(UINT32 Pin,UINT8 Mode,UINT8 Dir,UINT8 Output) {
  UINT32 O=PIN_MODE_OFFSET(Pin),Shift=PIN_MODE_SHIFT(Pin),M=0xfU<<Shift;
  Put32(Pins+O,(U32(Pins+O)&~M)|((UINT32)Mode<<Shift));
  O=0x100U+(Pin/32U)*0x10U; M=1U<<(Pin%32U);
  Put32(Pins+O,(U32(Pins+O)&~M)|(Output?M:0U)); PinDirections[Pin]=Dir;
}
static VOID ColdBootFixture(VOID) {
  // Synthetic calibration only in the mutable test tree; never copy device
  // calibration into source or rewrite the original firmware/backup DTB.
  SetSyntheticDevinfo(109U);
  SourceOffFixture(); ColdTcpcModel=TRUE; ColdUsbOffMask=7U; Usb[0x60]=0U; Usb[1]=0U;
  CcRole=0x4aU; CcRaw=0U; TcpcRegisters[0x1a]=CcRole;
  TcpcRegisters[0x2e]=2U; TcpcRegisters[0x2f]=0x1fU;
  TxSize[1]=5U;
}
static VOID IdLowFixture(BOOLEAN Charging) {
  ColdBootFixture(); ColdAutoAttach=FALSE; IdLowHubVbus=TRUE;
  PowerOn=Charging;
  CcRole=Charging?0x0aU:0x4aU; CcRaw=Charging?0x11U:0U;
  TcpcRegisters[0x1a]=CcRole;
  if(Charging) { SetPinInitial(48U,0U,1U,1U); PinPhysicalFromOutput(48U); }
}

static VOID Tick (VOID) {
  TimeNs += USB_ROLE_PERIOD * 100U;
  UsbRoleTick(mHandoff.RetryTimerEvent, &mHandoff);
}

static VOID StartRoleTimer (VOID) {
  CHECK(MusbHandoffDxeEntryPoint(NULL, NULL) == EFI_SUCCESS);
  CHECK(EventCount == 5U && RetryTimers == 1U);
  CHECK(Events[3].Notify == UsbRoleTick && Events[3].Tpl == TPL_CALLBACK);
  EndOfDxeNotify(mHandoff.EndOfDxeEvent, &mHandoff);
}

static VOID StartLateRoleTimer (VOID) {
  LateReadyToLock=TRUE;
  CHECK(MusbHandoffDxeEntryPoint(NULL, NULL) == EFI_SUCCESS);
  CHECK(EventCount == 5U && RetryTimers == 1U);
  CHECK(mHandoff.EndOfDxeObserved);
}

static VOID TickUntilReady (VOID) {
  for (UINTN Count = 0U; Count < 100U && !mHandoff.HostBusConnected; ++Count) Tick();
  if (!mHandoff.HostBusConnected) {
    MTK_MUSB_PLATFORM_EVIDENCE Evidence;
    MTK_MUSB_PLATFORM_READY_RESULT Ready = {0};
    EFI_STATUS Sample = CollectEvidence(FALSE, &Evidence);
    if (!EFI_ERROR(Sample)) MtkMusbPlatformEvaluateReady(&mHandoff.Io, &Evidence, &Ready);
    fprintf(stderr, "evidence=%lx flags=%x missing=%x devctl=%x path=%u boost=%u\n",
            (unsigned long)Sample, Ready.Flags, Ready.MissingFlags,
            Ready.DeviceControl, Ready.HostPowerPath, Evidence.Sc8989xBoostEnabled);
    for (UINTN Index = 0; Index < MTK_MUSB_PLATFORM_GPIO_COUNT; ++Index)
      fprintf(stderr, "GPIO %u mode=%u dir=%u input=%u\n", Evidence.Gpio[Index].Number,
              Evidence.Gpio[Index].Mode, Evidence.Gpio[Index].Direction,
              Evidence.Gpio[Index].PhysicalInput);
    fprintf(stderr, "host init status=%lx step=%x guard=%x typec=%x/%x cold=%u/%u/%u owned=%x wait=%llu now=%llu phy=%x first=%u/%u/%u\n",
            (unsigned long)mHandoff.TypecStatus, mHandoff.DiagnosticBusStep,
            mHandoff.ColdGuardStep, mHandoff.ColdTypecCore.FirstFailure.Step,
            mHandoff.DiagnosticTypecGuardStep, mHandoff.ColdHost.Kind,
            mHandoff.ColdHost.StartStep, mHandoff.ColdHost.LastStatus,
            mHandoff.ColdHost.OwnedMask,
            (unsigned long long)mHandoff.ColdHost.WaitSinceUs,
            (unsigned long long)(TimeNs / 1000U), mHandoff.ColdPhyStatus,
            mHandoff.DiagnosticColdHost.Valid,
            mHandoff.DiagnosticColdHost.State.StartStep,
            mHandoff.DiagnosticColdHost.State.LastStatus);
    fprintf(stderr, "retained=%u/%u/%u settled=%u cold-bound=%u pogo=%u typec-vbus=%u retry=%u failed=%u reason=%u\n",
            mHandoff.RetainedHostPending, mHandoff.RetainedHostOwned,
            mHandoff.RetainedTypecSource,
            mHandoff.DeviceSettled, mHandoff.ColdBound, mHandoff.PogoEnabled,
            mHandoff.TypecVbus, mHandoff.HostRetryTicks,
            mHandoff.PreparationFailed, mHandoff.DiagnosticReason);
  }
  CHECK(mHandoff.HostBusConnected && InstalledReady && UsbHostConnects == 1U);
}

static VOID TickUntilHostStartStep (MTK_MUSB_PLATFORM_COLD_START_STEP Step) {
  for (UINTN Count = 0U; Count < 100U && mHandoff.ColdHost.StartStep != (UINT32)Step; ++Count)
    Tick();
  CHECK(mHandoff.ColdHost.StartStep == (UINT32)Step && !mHandoff.HostBusConnected);
}

static VOID CheckDeferredHostStart (VOID) {
  IdLowFixture(TRUE);
  StartRoleTimer();
  TickUntilHostStartStep(MtkMusbPlatformColdStartSettle);
  UINT64 Before = TimeNs;
  Tick();
  CHECK(TimeNs - Before == USB_ROLE_PERIOD * 100U);
  CHECK(mHandoff.ColdHost.StartStep == MtkMusbPlatformColdStartPhySettle &&
        mHandoff.ColdHost.OwnedMask == MTK_MUSB_PLATFORM_COLD_OWNS_SESSION &&
        !mHandoff.ColdInProgress && !InstalledReady);
  TickUntilHostStartStep(MtkMusbPlatformColdStartPhySettle);
  CHECK(!mHandoff.ColdHost.PhyInvalidAttempted && !mHandoff.ColdHost.SessionVerified);
  TickUntilReady();

  IdLowFixture(TRUE);
  StartRoleTimer();
  TickUntilHostStartStep(MtkMusbPlatformColdStartPhySettle);
  PogoDocked=FALSE;
  for (UINTN Count = 0U; Count < 20U && mHandoff.ColdBound; ++Count) Tick();
  CHECK(!mHandoff.ColdBound && !InstalledReady && UsbHostConnects == 0U);
  BeforeExitNotify(mHandoff.BeforeExitEvent, &mHandoff);
  CHECK(!mHandoff.ColdBound && !mHandoff.ColdHost.Signature &&
        !mHandoff.ColdClockBound && !mHandoff.PhyBound);

  IdLowFixture(TRUE);
  StartRoleTimer();
  TickUntilHostStartStep(MtkMusbPlatformColdStartPhySettle);
  BeforeExitNotify(mHandoff.BeforeExitEvent, &mHandoff);
  CHECK(mHandoff.BootServicesLeaving && !mHandoff.ColdBound &&
        !mHandoff.ColdHost.Signature && !mHandoff.ColdClockBound && !mHandoff.PhyBound);
  Tick();
  CHECK(!InstalledReady && !UsbHostConnects);
}

static VOID CheckLateColdDispatch (VOID) {
  IdLowFixture(TRUE);
  StartLateRoleTimer();
  TickUntilReady();
  CHECK(mHandoff.PogoEnabled && mHandoff.HostBusConnected &&
        InstalledReady->HostPowerPath==MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB);

  ColdBootFixture();
  PogoDocked=FALSE;
  StartLateRoleTimer();
  TickUntilReady();
  CHECK(mHandoff.PogoEnabled && mHandoff.TypecVbus && mHandoff.HostBusConnected &&
        InstalledReady->HostPowerPath==MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);
}

static VOID CheckIdLowHostLocked (VOID) {
  IdLowFixture(TRUE);
  StartRoleTimer();
  TickUntilReady();
  CHECK(mHandoff.PogoEnabled && !BoostOn && PowerOn);
  CHECK(HubWrites == 3U && HubTracePin[0] == 99U &&
        HubTracePin[1] == 4U && HubTracePin[2] == 104U);
  CHECK(InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB);

  // Unplug incoming Type-C charger and attach a Type-C OTG sink while pogo
  // stays docked: boost turns on and both ports work simultaneously.
  PowerOn = FALSE;
  CcRaw = 0U;
  VendorStatusReg97 = 2U;
  ColdAutoAttach = TRUE;
  for (UINTN Count = 0U; Count < 100U && !mHandoff.TypecVbus; ++Count) Tick();
  CHECK(mHandoff.TypecVbus && BoostOn && mHandoff.PogoEnabled);
  CHECK(UsbHostConnects == 1U && !HostDisconnects && HubWrites == 3U &&
        mHandoff.HostBusConnected);
  CHECK(InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);
  CHECK(LeaseToken == 0U && LeaseAcquires == LeaseReleases);
  Ebs();
  CHECK(Record(0) == 1U && Record(15) == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE &&
        CacheCalls == 3U);
}

static VOID SleepTypecDetector (VOID) {
  VbusDetectEnabled = FALSE;
  VendorStatusReg97 = 0U;
  TcpcRegisters[0x9b] = 0x3aU;
  TcpcRegisters[0x99] = 0x80U;
  TcpcRegisters[0x90] = 0x80U;
}

static VOID CheckColdTypecSense (VOID) {
  ColdBootFixture();
  PogoDocked=FALSE;
  SleepTypecDetector();
  // The cable can already carry incoming VBUS when sensing wakes.
  SenseWakeVbus = TRUE;
  StartRoleTimer();
  for (UINTN Count = 0; Count < 10U; ++Count) Tick();
  CHECK(VbusDetectEnabled && PowerOn && !BoostOn && !InstalledReady);
  CHECK(SenseWrites == 3U && ColdTcpcWrites == 0U && SourceWrites == 0U);
  CHECK(SenseTraceReg[0] == 0x9bU && SenseTraceValue[0] == 0x38U);
  CHECK(SenseTraceReg[1] == 0x99U && SenseTraceValue[1] == 0x82U);
  CHECK(SenseTraceReg[2] == 0x90U && SenseTraceValue[2] == 0x82U);
  CHECK(LeaseToken == 0U && LeaseAcquires == LeaseReleases);

  PowerOn = FALSE;
  VendorStatusReg97 = 2U;
  TickUntilReady();
  CHECK(mHandoff.TypecVbus && BoostOn && SenseWrites == 3U);
  CHECK(!mHandoff.DeviceDisconnectAttempted && mHandoff.SenseOwnedMask == 0U);
}

static VOID CheckColdTypecSenseTimeout (VOID) {
  ColdBootFixture();
  PogoDocked=FALSE;
  SleepTypecDetector();
  SenseStuck = TRUE;
  StartRoleTimer();
  for (UINTN Count = 0; Count < 12U; ++Count) Tick();
  CHECK(mHandoff.DiagnosticSenseFailure.Stage == 6U && !VbusDetectEnabled);
  CHECK(SenseWrites == 6U && mHandoff.SenseOwnedMask == 0U);
  CHECK(SenseTraceReg[3] == 0x90U && TcpcRegisters[0x90] == 0x80U);
  CHECK(SenseTraceReg[4] == 0x99U && TcpcRegisters[0x99] == 0x80U);
  CHECK(SenseTraceReg[5] == 0x9bU && TcpcRegisters[0x9b] == 0x3aU);
  CHECK(ColdTcpcWrites == 0U && SourceWrites == 0U && !InstalledReady);
  CHECK(LeaseToken == 0U && LeaseAcquires == LeaseReleases);
}

static VOID CheckColdHostTimeoutSnapshot (VOID) {
  T70_USB_HOST_SNAPSHOT Snapshot;

  IdLowFixture(FALSE);
  ColdHostModeStuck = TRUE;
  StartRoleTimer();
  TickUntilHostStartStep(MtkMusbPlatformColdStartWaitHost);
  for (UINTN TickCount = 0U; TickCount <= MTK_MUSB_PLATFORM_COLD_TIMEOUT_MS /
       MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS && !mHandoff.PreparationFailed; ++TickCount)
    Tick();
  CHECK(mHandoff.PreparationFailed && !InstalledReady);
  CHECK(ReadHostDiagnostics(&Snapshot) == EFI_SUCCESS);
  CHECK(Snapshot.Status == EFI_TIMEOUT && Snapshot.BusStep == 0x610bU);
  CHECK(Snapshot.ColdStep == MtkMusbPlatformColdStartWaitHost &&
        Snapshot.ColdPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB &&
        Snapshot.ColdRegistersValid && (Snapshot.ColdDevctl & 5U) == 1U &&
        Snapshot.ColdPower == 0x21U && (Snapshot.ColdDtm1 & 0x3f3fU) == 0x3f2dU);

  Tick();
  CHECK(!mHandoff.ColdBound);
  TickUntilHostStartStep(MtkMusbPlatformColdStartSettle);
  CHECK(ReadHostDiagnostics(&Snapshot) == EFI_SUCCESS);
  CHECK(Snapshot.Status == EFI_TIMEOUT && Snapshot.BusStep == 0x610bU &&
        Snapshot.ColdStep == MtkMusbPlatformColdStartWaitHost);
}

static VOID CheckTypecHostLocked (VOID) {
  ColdBootFixture();
  PogoDocked=FALSE;
  StartRoleTimer();
  TickUntilReady();
  CHECK(mHandoff.PogoEnabled && mHandoff.TypecVbus && BoostOn &&
        mHandoff.HostBusConnected);
  CHECK(InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);
  // The power bus can be occupied by normal traffic or an exclusive user.
  UINTN Writes = SourceWrites;
  for (UINTN Kind = 0U; Kind < 2U; ++Kind) {
    LeaseAcquireStatus = Kind == 0U ? EFI_ALREADY_STARTED : EFI_ACCESS_DENIED;
    for (UINTN Count = 0U; Count < 8U; ++Count) Tick();
    CHECK(BoostOn && SourcePin86 && mHandoff.TypecVbus && mHandoff.HostBusConnected &&
          !mHandoff.TypecReleasing && SourceWrites == Writes);
  }
  LeaseAcquireStatus = EFI_SUCCESS;

  // Detaching Type-C cleanly releases SC8989x boost while keeping the PC host
  // controller bound so Type-C OTG or pogo can re-attach without HCD teardown.
  ColdAutoAttach = FALSE;
  CcRaw = 0U;
  for (UINTN Count = 0U; Count < 40U && (BoostOn || mHandoff.TypecVbus); ++Count) Tick();
  CHECK(!BoostOn && !mHandoff.TypecVbus && InstalledReady &&
        HostDisconnects == 0U && HostReleases == 0U && mHandoff.HostBusConnected);
  CHECK(LeaseToken == 0U && LeaseAcquires == LeaseReleases);
}
static VOID CheckTypecBeforeHubIsReady (VOID) {
  IdLowFixture(FALSE);
  IdLowHubVbus = FALSE;
  ColdAutoAttach = TRUE;
  StartRoleTimer();
  TickUntilReady();
  CHECK(mHandoff.PogoEnabled && mHandoff.TypecVbus && BoostOn);
  CHECK(InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);
  CHECK(!mHandoff.DiagnosticColdHost.Valid);
  CHECK(HostDisconnects == 0U && LeaseToken == 0U);
}

static VOID CheckTypecIdChangeDuringAttach (VOID) {
  ColdBootFixture();
  PogoDocked=FALSE;
  ColdIdDropsOnAttach = TRUE;
  StartRoleTimer();
  TickUntilReady();
  CHECK(PogoDocked && mHandoff.PogoEnabled && mHandoff.TypecVbus && BoostOn);
  CHECK(InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);
  Ebs();
  CHECK(Record(0) == 1U);
}

static VOID SetTypecSinkAttached (BOOLEAN Attached) {
  ColdAutoAttach = Attached;
  if (CcRole == 0x05U) CcRaw = Attached ? ColdAttachCc : 0U;
}

static VOID CheckPogoFirstOtgHotplug (VOID) {
  IdLowFixture(FALSE);
  StartRoleTimer();
  TickUntilReady();
  MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL *Ready = InstalledReady;
  UINTN InitialHubWrites = HubWrites;
  CHECK(PogoDocked && mHandoff.HostBusConnected && !BoostOn);
  CHECK(Ready->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB);

  // Plug the sink while the pogo host is already active.
  CHECK(CcRaw == 0U);
  SetTypecSinkAttached(TRUE);
  for (UINTN Count = 0U; Count < 100U && !mHandoff.TypecVbus; ++Count) Tick();
  CHECK(mHandoff.TypecVbus && BoostOn && SourcePin86 && PogoDocked && mHandoff.HostBusConnected);
  CHECK(InstalledReady == Ready && Ready->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);

  SetTypecSinkAttached(FALSE);
  for (UINTN Count = 0U; Count < 100U && (mHandoff.TypecVbus || BoostOn); ++Count) Tick();
  CHECK(!mHandoff.TypecVbus && !BoostOn && !SourcePin86 && PogoDocked && mHandoff.HostBusConnected);
  CHECK(InstalledReady == Ready && Ready->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB);

  SetTypecSinkAttached(TRUE);
  for (UINTN Count = 0U; Count < 100U && !mHandoff.TypecVbus; ++Count) Tick();
  CHECK(mHandoff.TypecVbus && BoostOn && PogoDocked && mHandoff.HostBusConnected);
  CHECK(InstalledReady == Ready && UsbHostConnects == 1U && !HostDisconnects &&
        !HostReleases && !ReadyUninstalls && HubWrites == InitialHubWrites);
  Ebs();
  CHECK(Record(0) == 1U && Record(15) == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);
  CHECK(BoostOn && HubLevels[0] == 1U && HubLevels[1] == 1U && HubLevels[2] == 1U);
}

static VOID CheckPogoDockDuringTypecProbe (VOID) {
  ColdBootFixture();
  ColdAutoAttach = FALSE;
  IdLowHubVbus = TRUE;
  PogoDocked=FALSE;
  StartRoleTimer();
  for (UINTN Count = 0U; Count < 40U && !mHandoff.ColdTypecCore.ProbeWaitingForAttach; ++Count)
    Tick();
  CHECK(mHandoff.ColdProbePending && CcRole == 0x05U);
  PogoDocked=TRUE;
  Tick();
  Tick();
  CHECK(mHandoff.PogoEnabled && mHandoff.ColdProbePending && !BoostOn);
  TickUntilReady();
  // Host is ready on pogo, and Type-C OTG probe remains alive for hotplug.
  CHECK(mHandoff.HostBusConnected &&
        InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB);
  // On EBS with no Type-C attached, BeforeExitNotify cleans up the idle probe
  // and hands off cleanly on ID_LOW_HUB.
  Ebs();
  CHECK(TcpcRegisters[0x2fU] == 0x1fU && CcRole == 0x4aU);
  CHECK(!mHandoff.ColdTypecBound && !mHandoff.TypecReleasing);
  CHECK(Record(0) == 1U && Record(15) == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB);
}

static VOID CheckBeforeExitReleasesTypec (VOID) {
  for (UINTN Case = 0U; Case < 4U; ++Case) {
    ColdBootFixture();
    PogoDocked=FALSE;
    StartRoleTimer();
    if (Case == 3U) {
      ColdClockFailure = 1U;
      for (UINTN Count = 0U; Count < 100U && !mHandoff.ColdBound; ++Count) Tick();
      CHECK(mHandoff.ColdBound && mHandoff.ColdClockBound &&
            !mHandoff.ColdHost.Signature && !InstalledReady);
    } else {
      TickUntilHostStartStep(MtkMusbPlatformColdStartPhySettle);
    }
    if (Case < 2U) {
      FailInstall = 2U;
      Tick();
      CHECK(mHandoff.ColdHost.Kind == MtkMusbPlatformColdStateStarted && !InstalledReady);
      if (Case == 1U) mHandoff.ColdHost.Kind = MtkMusbPlatformColdStateNeedsStop;
    } else if (Case == 2U) {
      TickUntilReady();
      mHandoff.RouteRetiring = TRUE;
    }
    if (Case != 3U) {
      CHECK(mHandoff.OtgBound && BoostOn && mHandoff.ColdTypecBound &&
            CcRole == 0x05U && TcpcRegisters[0x2fU] == 0U);
    }
    Ebs();
    CHECK(!mHandoff.ColdBound && !mHandoff.ColdClockBound && !mHandoff.PhyBound &&
          !mHandoff.ColdHost.Signature && ColdUsbOffMask == 7U && !InstalledReady);
    CHECK(!BoostOn && !mHandoff.OtgBound && !mHandoff.ColdTypecBound &&
          mHandoff.ColdPins[4].Owned == 0U && mHandoff.ColdPins[5].Owned == 0U);
    CHECK(CcRole == 0x4aU && TcpcRegisters[0x2fU] == 0x1fU && (Usb[0x60] & 1U) == 0U);
    CHECK(LeaseToken == 0U && LeaseAcquires == LeaseReleases && Record(0) == 0U);
  }

  ColdBootFixture();
  PogoDocked=FALSE;
  SleepTypecDetector();
  SenseStuck = TRUE;
  StartRoleTimer();
  for (UINTN Count = 0U; Count < 10U && mHandoff.SenseOwnedMask == 0U; ++Count) Tick();
  CHECK(mHandoff.SenseOwnedMask != 0U && !InstalledReady);
  Ebs();
  CHECK(TcpcRegisters[0x90] == 0x80U && TcpcRegisters[0x99] == 0x80U &&
        TcpcRegisters[0x9b] == 0x3aU && mHandoff.SenseOwnedMask == 0U);
  CHECK(!BoostOn && !mHandoff.ColdTypecBound && Record(0) == 0U);
  CHECK(LeaseToken == 0U && LeaseAcquires == LeaseReleases);
}

static VOID CheckHostAndBusBinding (VOID) {
  for (UINTN Case = 0U; Case < 2U; ++Case) {
    IdLowFixture(TRUE);
    StartRoleTimer();
    TickUntilHostStartStep(MtkMusbPlatformColdStartPhySettle);
    Tick();
    CHECK(InstalledReady && !UsbHostConnects);
    if (Case == 0U) {
      UsbHostConnectStatus = EFI_DEVICE_ERROR;
      UsbHostPublishOnFailure = TRUE;
    } else {
      UsbBusStartFails = TRUE;
    }
    Tick();
    CHECK(UsbHostConnects == 1U && ReadyHostComplete() && !mHandoff.HostBusConnected);
    CHECK(mHandoff.DiagnosticStatus == (Case == 0U ? EFI_DEVICE_ERROR : EFI_NOT_READY));
    if (Case != 0U) {
      UsbBusStartFails = FALSE;
      for (UINTN Count = 0U; Count < 100U && !mHandoff.HostBusConnected; ++Count) Tick();
      CHECK(mHandoff.HostBusConnected && UsbHostConnects == 2U);
      CHECK(HostDisconnects == 0U && HostReleases == 0U && ReadyUninstalls == 0U);
    }
  }
}

static VOID CheckFixedHostMode (VOID) {
  ColdBootFixture();
  PogoDocked=FALSE;
  IdLowHubVbus = TRUE;
  StartRoleTimer();
  TickUntilReady();
  CHECK(InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);
  UINTN InitialHubWrites = HubWrites;
  MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL *InitialReady = InstalledReady;

  // GPIO10 changes only describe downstream hub exposure in PC mode. They do
  // not tear down the root HCD or switch the controller back to a USB client.
  PogoDocked=TRUE;
  for (UINTN Count = 0U; Count < 20U; ++Count) Tick();
  CHECK(UsbHostConnects == 1U && HostDisconnects == 0U && HostReleases == 0U &&
        ReadyUninstalls == 0U && InstalledReady == InitialReady);
  CHECK(HubWrites == InitialHubWrites && mHandoff.PogoEnabled &&
        mHandoff.HostBusConnected);

  PogoDocked=FALSE;
  for (UINTN Count = 0U; Count < 20U; ++Count) Tick();
  CHECK(UsbHostConnects == 1U && HostDisconnects == 0U && HostReleases == 0U &&
        ReadyUninstalls == 0U && InstalledReady == InitialReady);
  CHECK(HubWrites == InitialHubWrites && mHandoff.PogoEnabled && BoostOn &&
        mHandoff.TypecVbus && mHandoff.HostBusConnected);
  CHECK(LeaseToken == 0U && LeaseAcquires == LeaseReleases);
}

static VOID CheckFixedHostHandoffIdChange (VOID) {
  for (UINTN ChangeDuringExit = 0U; ChangeDuringExit < 2U; ++ChangeDuringExit) {
    ColdBootFixture();
    PogoDocked=FALSE;
    IdLowHubVbus = TRUE;
    StartRoleTimer();
    TickUntilReady();
    CHECK(InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);

    if (!ChangeDuringExit) PogoDocked=TRUE;
    BeforeExitNotify(mHandoff.BeforeExitEvent, &mHandoff);
    CHECK(mHandoff.HostBusConnected && BoostOn && HostDisconnects == 0U);
    if (ChangeDuringExit) PogoDocked=TRUE;
    EbsPowerNotify(NULL,NULL); ControllerExit(); Snapshot(); EbsPublishNotify(NULL,NULL);
    CHECK(Record(0) == 1U && Record(15) == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);
    CHECK(HubLevels[0] == 1U && HubLevels[1] == 1U && HubLevels[2] == 1U);
  }

  IdLowFixture(TRUE);
  StartRoleTimer();
  TickUntilReady();
  CHECK(InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB);
  PogoDocked=FALSE;
  Ebs();
  CHECK(Record(0) == 0U);
}

static VOID CheckTypecHandoffRetriesTransientRead (VOID) {
  ColdBootFixture();
  PogoDocked=FALSE;
  StartRoleTimer();
  TickUntilReady();
  CHECK(mHandoff.HostBusConnected && mHandoff.TypecVbus && BoostOn);
  UINTN Writes = SourceWrites;
  RawReadFailure = RawReads + 1U;
  BeforeExitNotify(mHandoff.BeforeExitEvent, &mHandoff);
  CHECK(mHandoff.HostBusConnected && BoostOn && HostDisconnects == 0U);

  // The final capture reads SC8989x twice before reading the TCPC identity.
  RawReadFailure = RawReads + 3U;
  EbsPowerNotify(NULL, NULL); ControllerExit(); Snapshot(); EbsPublishNotify(NULL, NULL);
  CHECK(Record(0) == 1U && Record(15) == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);
  CHECK(mHandoff.EbsPowerSampleValid && BoostOn && SourcePin86 && SourceWrites == Writes);
  CHECK(!HostDisconnects && !LeaseToken && LeaseAcquires == LeaseReleases);
}

static VOID CheckPogoEbsSurvivesTcpcFailure (VOID) {
  // 1) Pogo-only handoff: HUSB311 read failure at EBS must not suppress a
  //    valid ID_LOW_HUB handoff when SC8989x boost-off and hub/host are live.
  IdLowFixture(TRUE);
  StartRoleTimer();
  TickUntilReady();
  CHECK(InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB);
  TcpcFailure = TRUE;
  Ebs();
  CHECK(mHandoff.EbsScBoostSampleValid && !mHandoff.EbsPowerSampleValid);
  CHECK(Record(0) == 1U && Record(15) == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB);

  // 2) Simultaneous Pogo + Type-C: if Type-C boost is off and HUSB311 fails
  //    right before I2C exit while pogo remains docked, EBS must still reach
  //    the ID_LOW_HUB fallback and publish the shared host.
  IdLowFixture(FALSE);
  ColdAutoAttach = TRUE;
  StartRoleTimer();
  TickUntilReady();
  for (UINTN Count = 0U; Count < 100U && !mHandoff.TypecVbus; ++Count) Tick();
  CHECK(mHandoff.TypecVbus &&
        InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);
  BeforeExitNotify(mHandoff.BeforeExitEvent, &mHandoff);
  CHECK(mHandoff.HostBusConnected && HostDisconnects == 0U);
  BoostOn = FALSE;
  SourcePin86 = 0U;
  TcpcFailure = TRUE;
  EbsPowerNotify(NULL, NULL); ControllerExit(); Snapshot(); EbsPublishNotify(NULL, NULL);
  CHECK(mHandoff.EbsScBoostSampleValid && !mHandoff.EbsPowerSampleValid);
  CHECK(Record(0) == 1U && Record(15) == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB);

  // 3) Type-C only (no pogo): HUSB311 failure at EBS must still refuse handoff.
  ColdBootFixture();
  PogoDocked=FALSE;
  StartRoleTimer();
  TickUntilReady();
  CHECK(InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);
  BeforeExitNotify(mHandoff.BeforeExitEvent, &mHandoff);
  TcpcFailure = TRUE;
  EbsPowerNotify(NULL, NULL); ControllerExit(); Snapshot(); EbsPublishNotify(NULL, NULL);
  CHECK(mHandoff.EbsScBoostSampleValid && !mHandoff.EbsPowerSampleValid);
  CHECK(Record(0) == 0U);
}

static VOID CheckRetainedHostHandoff (VOID) {
  Reset();
  SetSyntheticDevinfo(109U);
  MasterPresent = TRUE;
  Usb[0x60U] = 0x5dU;
  Usb[1U] = 0x20U;
  StartRoleTimer();
  TickUntilReady();
  CHECK(mHandoff.RetainedHostOwned && !mHandoff.RetainedHostPending &&
        mHandoff.HostBusConnected && !mHandoff.ColdBound &&
        mHandoff.ColdHost.Signature == 0U && Usb[0x60U] == 0x5dU);
  CHECK(mHandoff.RetainedTypecSource && mHandoff.RouteCcPin==1U && InstalledReady != NULL &&
        InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE &&
        InstalledReady->Flags == MTK_MUSB_PLATFORM_READY_REQUIRED &&
        ColdUsbWrites == 0U && LeaseToken == 0U &&
        LeaseAcquires == LeaseReleases);

  Ebs();
  CHECK(Record(0)==1U && mHandoff.EbsPowerSampleValid &&
        mHandoff.RetainedTypecSource);

  Reset();
  SetSyntheticDevinfo(109U);
  MasterPresent = TRUE;
  Usb[0x60U] = 0x5dU;
  Usb[1U] = 0x20U;
  StartRoleTimer();
  TickUntilReady();
  CHECK(mHandoff.RetainedTypecSource && mHandoff.RouteCcPin==1U && InstalledReady != NULL &&
        InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE &&
        InstalledReady->Flags == MTK_MUSB_PLATFORM_READY_REQUIRED);
  SetHostPowerPath(FALSE,FALSE);
  CHECK(mHandoff.RouteCcPin==1U && mHandoff.RetainedTypecSource &&
        mHandoff.IdLowBound==FALSE && !mHandoff.RouteRetiring &&
        InstalledReady->HostPowerPath==MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY &&
        InstalledReady->Flags==MTK_MUSB_PLATFORM_READY_REQUIRED);

  Reset();
  SetSyntheticDevinfo(109U);
  MasterPresent = TRUE;
  Usb[0x60U] = 0x5dU;
  Usb[1U] = 0x20U;
  Usb[0x0bU] = 0x30U;
  StartRoleTimer();
  for (UINTN Count = 0U; Count < 10U; ++Count) Tick();
  CHECK(mHandoff.RetainedHostPending &&
        !mHandoff.RetainedHostOwned && !InstalledReady &&
        !mHandoff.PreparationFailed &&
        mHandoff.DiagnosticReason == UsbDiagActiveSession);
  Usb[0x0bU] = 0U;
  mHandoff.HostRetryTicks = 0U;
  TickUntilReady();
  CHECK(mHandoff.RetainedHostOwned && !mHandoff.RetainedHostPending &&
        mHandoff.HostBusConnected && Usb[0x60U] == 0x5dU);

  IdLowFixture(TRUE);
  ColdUsbOffMask = 0U;
  Usb[0x60U] = 0x5dU;
  Usb[1U] = 0x20U;
  StartRoleTimer();
  TickUntilReady();
  CHECK(mHandoff.RetainedHostOwned && !mHandoff.RetainedTypecSource &&
        InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB &&
        mHandoff.HostBusConnected && !mHandoff.ColdBound && Usb[0x60U] == 0x5dU);
}

static VOID CheckPassiveInheritedController (VOID) {
  for (UINTN Case = 0U; Case < 2U; ++Case) {
    IdLowFixture(TRUE);
    if (Case == 0U) ColdUsbOffMask = 0U;
    ControllerMapBad = TRUE;
    Usb[0x0b] = 0x27U;
    Put16(Usb + 6U, 0x1ffU);
    Put16(Usb + 8U, 0x1feU);
    Put16(Usb + 2U, 1U);
    Put16(Usb + 4U, 2U);
    Usb[0x200] = 0x80U;
    StartRoleTimer();
    for (UINTN Count = 0U; Count < 4U; ++Count) Tick();
    CHECK(UsbReads == 0U && DeviceIrqWrites == 0U);
    CHECK(ColdClockWrites == 0U && !InstalledReady);
    ControllerMapBad = FALSE;
    if (Case == 1U) mHandoff.BoardRetryTicks = 0U;
    TickUntilReady();
    CHECK(DeviceIrqWrites == 3U && AckWrites == 3U);
    CHECK(DeviceResetWrites == 0U && DeviceStopWrites == 0U && LeaseToken == 0U);
  }
}

static VOID CheckClockOffWithVconn (VOID) {
  ColdBootFixture();
  PogoDocked=FALSE;
  ColdAutoAttach = FALSE;
  SleepTypecDetector();
  CcRole = TcpcRegisters[0x1aU] = 0x0fU;
  VconnSupply = TRUE;
  TcpcRegisters[0x1cU] = 0x81U;
  VconnSettleReads = 2U;
  StartRoleTimer();
  for (UINTN Count = 0U; Count < 10U && !mHandoff.ColdProbePending; ++Count) Tick();
  CHECK(VconnWrites == 1U && TcpcRegisters[0x1cU] == 0x80U &&
        VbusDetectEnabled && mHandoff.ColdProbePending && mHandoff.SenseOwnedMask == 0U);
  CHECK(UsbReads == 0U && ColdClockWrites == 0U && !BoostOn && LeaseToken == 0U);
}

static VOID CheckEarlySenseRollback (BOOLEAN Timeout) {
  IdLowFixture(FALSE);
  ColdUsbOffMask = 0U;
  InheritedDeviceModel = TRUE;
  Usb[0x60U] = 0x81U;
  Usb[1U] = 0x40U;
  SleepTypecDetector();
  if (Timeout) SenseStuck = TRUE;
  else SenseWriteFailure = 2U;
  StartRoleTimer();
  for (UINTN Count = 0U; Count < 12U; ++Count) {
    Tick();
    if (mHandoff.DiagnosticSenseFailure.Stage != 0U && mHandoff.SenseOwnedMask == 0U)
      break;
  }
  CHECK(!mHandoff.DeviceSettled && !mHandoff.DeviceDisconnectInProgress &&
        mHandoff.DiagnosticSenseFailure.Stage == (Timeout ? 6U : 2U));
  CHECK(TcpcRegisters[0x90] == 0x80U && TcpcRegisters[0x99] == 0x80U &&
        TcpcRegisters[0x9b] == 0x3aU && mHandoff.SenseOwnedMask == 0U);
  CHECK(LeaseToken == 0U && LeaseAcquires == LeaseReleases);

  SenseStuck = FALSE;
  mHandoff.TypecRetryTicks = 0U;
  TickUntilReady();
  CHECK(DeviceStopWrites == 2U && mHandoff.DeviceSettled);
}

static VOID CheckInheritedDeviceSense (BOOLEAN Pogo) {
  if (Pogo) {
    IdLowFixture(FALSE);
  } else {
    ColdBootFixture();
    PogoDocked=FALSE;
  }
  ColdUsbOffMask = 0U;
  InheritedDeviceModel = TRUE;
  Usb[0x60U] = 0x81U;
  Usb[1U] = 0x40U;
  SleepTypecDetector();
  SenseWakeVbus = TRUE;
  if (Pogo) {
    CcRaw = 0x11U;
    TcpcRegisters[0x1aU] = CcRole = 0x0aU;
  }

  StartRoleTimer();
  for (UINTN Count = 0U; Count < 10U; ++Count) Tick();
  CHECK(VbusDetectEnabled && PowerOn && !BoostOn && SenseWrites == 3U);
  if (!Pogo) {
    CHECK(!InstalledReady && !mHandoff.DeviceSettled && DeviceStopWrites == 0U);
    PowerOn = FALSE;
    VendorStatusReg97 = 2U;
  }
  TickUntilReady();
  CHECK(mHandoff.DeviceDisconnectComplete && mHandoff.DeviceSettled && DeviceStopWrites == 2U);
  CHECK(mHandoff.PogoEnabled && mHandoff.TypecVbus == !Pogo);
  CHECK(LeaseToken == 0U && LeaseAcquires == LeaseReleases);
}

static VOID CheckColdBlankUsbPll (VOID) {
  // True cold boot with OTG-only and blank isolated APMIXED_USBPLL (0 Hz):
  // PrepareColdClocks must program 2496 MHz before enabling the USB clocks.
  ColdBootFixture();
  PogoDocked=FALSE;
  ColdPllBlank = TRUE;
  StartRoleTimer();
  TickUntilReady();
  CHECK(ColdPllSetCalls == 1U && ColdPllProgrammedHz == 2496000000ULL);
  CHECK(mHandoff.HostBusConnected && mHandoff.TypecVbus &&
        InstalledReady->HostPowerPath == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);
  Ebs();
  CHECK(Record(0) == 1U && Record(15) == MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE);

  // SetFrequency failure must fail cleanly without claiming clock ownership.
  ColdBootFixture();
  PogoDocked=FALSE;
  ColdPllBlank = TRUE;
  ColdPllSetFail = TRUE;
  StartRoleTimer();
  mHandoff.ColdInProgress = TRUE;
  CHECK(PrepareColdClocks(Tree) == EFI_DEVICE_ERROR);
  CHECK(ColdPllSetCalls == 1U && !mHandoff.ColdClockBound &&
        mHandoff.DiagnosticBusStep == 0x6248U);
  mHandoff.ColdInProgress = FALSE;

  // Blank USBPLL must NOT be reprogrammed if any USB clock gate is already open.
  ColdBootFixture();
  ColdUsbOffMask = 6U; // IFRAO_SSUSB already ON
  ColdPllBlank = TRUE;
  StartRoleTimer();
  mHandoff.ColdInProgress = TRUE;
  CHECK(PrepareColdClocks(Tree) == EFI_NOT_READY);
  CHECK(ColdPllSetCalls == 0U && !mHandoff.ColdClockBound &&
        mHandoff.DiagnosticBusStep == 0x6242U);
  mHandoff.ColdInProgress = FALSE;

  // Non-zero unexpected USBPLL rate must still be rejected without reprogramming.
  ColdBootFixture();
  ClockFailure = 6U;
  StartRoleTimer();
  mHandoff.ColdInProgress = TRUE;
  CHECK(PrepareColdClocks(Tree) == EFI_UNSUPPORTED);
  CHECK(ColdPllSetCalls == 0U && !mHandoff.ColdClockBound &&
        mHandoff.DiagnosticBusStep == 0x6250U);
  mHandoff.ColdInProgress = FALSE;
}

int main(int argc,char **argv) {
  (VOID)Install; (VOID)Installs;
  CHECK(argc == 2);
  FILE *F=fopen(argv[1],"rb"); CHECK(F);
  CHECK(fseek(F,0,SEEK_END)==0); long Size=ftell(F); CHECK(Size>0);
  CHECK(fseek(F,0,SEEK_SET)==0); TreeBytes=(UINTN)Size;
  Tree=malloc(TreeBytes+65536); CHECK(Tree); CHECK(fread(Tree,1,TreeBytes,F)==TreeBytes); CHECK(fclose(F)==0);
  TreeBytes+=65536; CHECK(FdtOpenInto(Tree,Tree,(INT32)TreeBytes)==0);
  INT32 Chosen=FdtPathOffset(Tree,"/chosen"); CHECK(Chosen>=0);
  // Remove the property without inspecting any source calibration contents.
  (VOID)FdtDelProp(Tree,Chosen,"atag,devinfo");
  CHECK(ValidateBoardTree(Tree,TreeBytes)==EFI_SUCCESS);
  STATIC CONST struct { CONST CHAR8 *Node; CONST CHAR8 *Property; } Links[]={
    {USB_NODE "/port/endpoint@0","remote-endpoint"},
    {EXTCON_NODE "/port/endpoint@0","remote-endpoint"},
    {EXTCON_NODE,"vbus-supply"},
  };
  for(UINTN I=0U;I<ARRAY_SIZE(Links);++I) {
    INT32 Node=FdtPathOffset(Tree,Links[I].Node),Length;
    UINT32 Original,Wrong=0U;
    CONST VOID *Ref=FdtGetProp(Tree,Node,Links[I].Property,&Length);
    CHECK(Node>=0&&Ref&&Length==sizeof(Original));
    CopyMem(&Original,Ref,sizeof(Original));
    CHECK(FdtSetProp(Tree,Node,Links[I].Property,&Wrong,sizeof(Wrong))==0);
    CHECK(ValidateBoardTree(Tree,TreeBytes)==EFI_UNSUPPORTED);
    CHECK(FdtSetProp(Tree,Node,Links[I].Property,&Original,sizeof(Original))==0);
  }
  INT32 UsbNode=FdtPathOffset(Tree,USB_NODE);
  CHECK(FdtDelProp(Tree,UsbNode,"usb-role-switch")==0);
  CHECK(ValidateBoardTree(Tree,TreeBytes)==EFI_UNSUPPORTED);
  CHECK(FdtSetProp(Tree,UsbNode,"usb-role-switch",NULL,0)==0);
  CHECK(ValidateBoardTree(Tree,TreeBytes)==EFI_SUCCESS);
  CHECK(ValidateBoardTree(Tree,TreeBytes-1)!=EFI_SUCCESS);
  CHECK(ValidateBoardTree(NULL,TreeBytes)!=EFI_SUCCESS);
  CheckLateColdDispatch();
  CheckDeferredHostStart();
  CheckColdHostTimeoutSnapshot();
  CheckIdLowHostLocked();
  CheckTypecHostLocked();
  CheckTypecBeforeHubIsReady();
  CheckTypecIdChangeDuringAttach();
  CheckColdTypecSense();
  CheckColdTypecSenseTimeout();
  CheckPogoFirstOtgHotplug();
  CheckPogoDockDuringTypecProbe();
  CheckBeforeExitReleasesTypec();
  CheckHostAndBusBinding();
  CheckRetainedHostHandoff();
  CheckPassiveInheritedController();
  CheckClockOffWithVconn();
  CheckEarlySenseRollback(FALSE);
  CheckEarlySenseRollback(TRUE);
  CheckFixedHostMode();
  CheckFixedHostHandoffIdChange();
  CheckTypecHandoffRetriesTransientRead();
  CheckPogoEbsSurvivesTcpcFailure();
  CheckInheritedDeviceSense(TRUE);
  CheckInheritedDeviceSense(FALSE);
  CheckColdBlankUsbPll();
  Reset();
  free(Tree);
  printf("T70 USB role transitions: %u checks PASS\n", Checks);
  return 0;
}
