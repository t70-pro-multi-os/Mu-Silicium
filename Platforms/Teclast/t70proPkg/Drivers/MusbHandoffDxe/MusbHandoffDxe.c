// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// T70 USB board role, power and OS handoff.
#include <Uefi.h>
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Protocol/I2cMaster.h>
#include <Protocol/UsbIo.h>
#include <Protocol/Usb2HostController.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DxeSmmReadyToLock.h>
#include <Protocol/T70UsbStorageDiagnostics.h>
#include <Protocol/T70UsbTypecDiagnostics.h>
#include <Protocol/MtkClock.h>
#include <Protocol/MtkGpio.h>
#include <Protocol/MtkI2cController.h>
#include <Protocol/MtkI2cExclusive.h>
#include <Protocol/MtkMusbHostPlatformReady.h>
#include <Protocol/StockPowerReadOnly.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MtkMusbPlatformCore.h>
#include <Library/MtkSc8989xOtgCore.h>
#include <Library/MtkHusb311TypecCore.h>
#include <Library/StockPowerCore.h>
#include <Library/MtkUsbPhyPlatformLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#define HANDOFF_MAGIC       0x0000000148303754ULL
#define HANDOFF_PAGE        0x43F7F000ULL
#define HANDOFF_RECORD      0x43F7F100ULL
#define HANDOFF_BYTES       64U
#define HANDOFF_REVISION    2U
#define MAX_FDT_BYTES       0x200000U
#define USB_ROLE_PERIOD (MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS * 10000ULL)
#define USB_RETRY_TICKS 40U
#define USB_SETTLE_TICKS 200U
// A device added behind the shared 5 V (pogo keyboard, hub port) can pull
// VBUS down for a moment. Only a state that persists is a detach.
#define USB_TYPEC_HOLD_TICKS (500U / MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS)
#define USB_SOURCE_WAIT_TICKS 20U
#define USB_SENSE_WAIT_TICKS (20U / MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS)
#define DRIVER_SIGNATURE    SIGNATURE_32('M','H','O','F')

#define USB_NODE            "/soc/usb0@11200000"
#define PHY_BUS_NODE        "/soc/usb-phy@11f40000"
#define PHY_NODE            "/soc/usb-phy@11f40000/usb-phy@11f40000"
#define I2C_NODE            "/soc/i2c@1101a000"
#define EXTCON_NODE         "/soc/extcon_usb"
#define SC_NODE             "/soc/i2c@1101a000/sc8989x@6A"
#define TCPC_NODE           "/soc/i2c@1101a000/husb311@4e"
#define PINCTRL_BASE        0x10005000ULL
#define PINCTRL_BYTES       0x1000ULL
#define I2C_BASE            0x1101A000ULL
#define I2C_BYTES           0x1000ULL
#define I2C_DMA_BASE        0x10217480ULL
#define I2C_DMA_BYTES       0x80ULL
#define I2C_WRAP_BASE       0x1101B000ULL
#define I2C_WRAP_BYTES      0x1000ULL
#define I2C_PIN_PAIR        142U
#define I2C_WRAP_CLOCK_GATED_BIT  (1U << 2)
#define SC_ADDRESS          0x6AU
#define TCPC_ADDRESS        0x4EU

#define MUSB_INTRTX         0x02U
#define MUSB_INTRRX         0x04U
#define MUSB_DMA_INTR       0x200U
#define MUSB_DEVCTL_SESSION       0x01U
#define MUSB_DEVCTL_HOSTMODE      0x04U
#define MUSB_DEVCTL_VBUS_VALID    0x18U
#define MUSB_DEVCTL_BDEVICE       0x80U
#define MUSB_RETAINED_HOST_MASK   (MUSB_DEVCTL_SESSION | MUSB_DEVCTL_HOSTMODE | \
                                   MUSB_DEVCTL_VBUS_VALID | MUSB_DEVCTL_BDEVICE)
#define MUSB_RETAINED_HOST_READY  (MUSB_DEVCTL_SESSION | MUSB_DEVCTL_HOSTMODE | \
                                   MUSB_DEVCTL_VBUS_VALID)
#define PHY_ACR1            0x304U
#define PHY_ACR6            0x318U
#define PHY_DTM0            0x368U
#define PHY_DTM1            0x36CU
#define PIN_MODE_OFFSET(P)  (0x300U + (((P) / 8U) * 0x10U))
#define PIN_MODE_SHIFT(P)   (((P) % 8U) * 4U)

typedef struct {
  UINT32 Pin;
  UINT8 Mode, Direction, Output;
  UINT8 ExpectedMode, ExpectedDirection, ExpectedOutput;
  UINT8 Owned;
} COLD_USB_PIN;

typedef struct {
  UINT8 Stage, Register, Saved, Expected, Live;
  EFI_STATUS Status;
} HUSB_SENSE_FAILURE;

typedef struct {
  BOOLEAN Valid;
  UINT8 ReadMask, BoardStep, PhyStatus, Devctl, Power;
  EFI_STATUS BoardStatus;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE State;
  UINT32 Phy[4]; // ACR1, ACR6, DTM0, DTM1 before cleanup.
} COLD_HOST_FAILURE;

typedef enum {
  UsbDeviceRoleStable,
  UsbDeviceRoleConfirming,
  UsbDeviceRoleNeutral
} USB_DEVICE_ROLE_PHASE;

typedef struct {
  UINT32 Signature;
  EFI_EVENT ExitNotifyEvent;
  EFI_EVENT ExitPublishEvent;
  EFI_EVENT EndOfDxeEvent;
  EFI_EVENT BeforeExitEvent;
  EFI_EVENT RetryTimerEvent;
  BOOLEAN HostBusConnected;
  BOOLEAN BootServicesLeaving;
  VOID *ValidatedBoardTree;
  UINTN ValidatedBoardTreeBytes;
  BOOLEAN RouteRetiring, HostChildrenDisconnected;
  UINT8 RouteCcPin;
  UINT8 LastId, BoardRetryTicks, HostRetryTicks;
  UINT8 PowerBusRetryTicks, RouteTicks, SettleTicks, AdoptTicks;
  BOOLEAN DeviceSettled, PogoKnown, PogoEnabled;
  BOOLEAN RetainedHostPending, RetainedHostOwned;
  BOOLEAN TypecVbus, TypecReleasing;
  BOOLEAN RetainedTypecSource;
  UINT8 TypecReadFailures, TypecPowerTicks, TypecSenseTicks;
  UINT8 TypecLossTicks;
  EFI_STATUS TypecStatus;
  UINT8 DiagnosticReason, DiagnosticDevctl, DiagnosticId;
  EFI_STATUS DiagnosticStatus;
  UINT16 DiagnosticBusStep, DiagnosticPreparationStep;
  EFI_HANDLE I2cHandle;
  EFI_HANDLE ReadyHandle;
  MTK_I2C_CONTROLLER *I2cController;
  MTK_I2C_EXCLUSIVE_PROTOCOL *I2cExclusive;
  UINT64 I2cToken;
  BOOLEAN OtgBusy;
  BOOLEAN OtgCleaning;
  BOOLEAN OtgBound;
  BOOLEAN I2cExitObserverRegistered;
  MTK_SC8989X_OTG_CORE OtgCore;
  MTK_CLOCK_PROTOCOL *Clock;
  MTK_GPIO_PROTOCOL *Gpio;
  STOCK_POWER_BOARD_READY_PROTOCOL BoardReady;
  MTK_MUSB_HOST_PLATFORM_READY_PROTOCOL Ready;
  EFI_LOCK IoLock;
  MTK_MUSB_PLATFORM_IO Io;
  BOOLEAN BoardReadyInstalled;
  BOOLEAN ReadyInstalled;
  BOOLEAN ExitingBootServices;
  BOOLEAN EbsFinalized;
  BOOLEAN EbsPowerSampleValid;
  BOOLEAN EbsScBoostSampleValid;
  BOOLEAN EbsPowerCaptured;
  BOOLEAN ScBoostEnabled;
  UINT8 HusbPowerStatus;
  SP_HUSB311_CC_STATUS HusbCcStatus;
  BOOLEAN ConnectInProgress;
  BOOLEAN EndOfDxeSeen;
  BOOLEAN EndOfDxeObserved;
  BOOLEAN ImageMustRemainResident;
  BOOLEAN AllowAckWrites;
  BOOLEAN AllowFifoWrites;
  BOOLEAN PreparationFailed;
  BOOLEAN ColdInProgress;
  BOOLEAN ColdProbePending;
  USB_DEVICE_ROLE_PHASE DeviceRolePhase;
  UINT8 DeviceId;
  UINT32 DevicePowerSample;
  BOOLEAN DeviceDisconnectInProgress;
  BOOLEAN DeviceDisconnectAttempted, DeviceDisconnectComplete;
  BOOLEAN DeviceResetAttempted;
  BOOLEAN DeviceRetiring, DeviceRetirementAttempted, PassiveDeviceRetiring;
  BOOLEAN SenseAttempted;
  UINT8 SenseOwnedMask, SenseSaved[3], SenseExpected[3];
  HUSB_SENSE_FAILURE DiagnosticSenseFailure, DiagnosticSenseCleanup;
  UINT8 DiagnosticDeviceStop;
  UINT8 DiagnosticPower[6], DiagnosticPowerValid;
  MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC DiagnosticIdle;
  BOOLEAN DeviceHubSampled;
  UINT8 DeviceHubPhysical[3];
  COLD_USB_PIN DeviceHubPins[3];
  BOOLEAN TypecOnly;
  BOOLEAN HostFirst;
  UINT32 TypecRetryTicks;
  BOOLEAN IdLowBound;
  UINT8 DiagnosticTypecGuardStep;
  UINT64 IdLowRoleToken;
  BOOLEAN ColdClockBound;
  BOOLEAN ColdPinBound;
  UINT8 ColdClockOwnedMask;
  UINT32 ColdClockId[3];
  UINT64 ColdClockHz[3];
  UINT32 ColdClockParent;
  COLD_USB_PIN ColdPins[6];
  BOOLEAN ColdBound;
  BOOLEAN ColdTypecBound;
  BOOLEAN ColdTypecBusy;
  BOOLEAN AllowColdWrites;
  UINT32 ColdPhyUsbMode;
  UINT64 ColdOwnerToken;
  UINT64 ColdSampleSequence;
  MTK_HUSB311_TYPEC_CORE ColdTypecCore;
  MTK_HUSB311_TOKEN ColdTypecToken;
  MTK_USB_PHY_IO ColdPhyIo;
  MTK_USB_PHY_CONFIG ColdPhyConfig;
  MTK_USB_PHY_HOST_TUNING ColdPhyTuning;
  MTK_USB_PHY_MUSB_SESSION ColdPhySession;
  MTK_MUSB_PLATFORM_COLD_HOST_STATE ColdHost;
  UINT8 ColdGuardStep, ColdPhyStatus;
  EFI_STATUS ColdGuardStatus;
  COLD_HOST_FAILURE DiagnosticColdHost;
  BOOLEAN PhyBound;
  MTK_USB_PHY_CORE PhyCore;
} MUSB_HANDOFF_CONTEXT;

STATIC MUSB_HANDOFF_CONTEXT mHandoff;

enum {
  UsbDiagUnknown, UsbDiagEndOfDxe, UsbDiagFdt, UsbDiagPowerBus,
  UsbDiagDeviceRole, UsbDiagActiveSession, UsbDiagPowerWait, UsbDiagIdWait,
  UsbDiagPreparationFailed, UsbDiagEvidence, UsbDiagHostReady,
  UsbDiagHostConnected, UsbDiagHostConnectFailed, UsbDiagDeviceHandoff,
  UsbDiagRouteCleanupFailed
};
STATIC VOID UsbWait (UINT8 Reason,EFI_STATUS Status) {
  mHandoff.DiagnosticReason=Reason; mHandoff.DiagnosticStatus=Status;
}
// Retain the precise failed preparation check until the next pass.
STATIC EFI_STATUS BusFailure (UINT16 Step,EFI_STATUS Status) {
  if(!mHandoff.DiagnosticSenseFailure.Stage&&!mHandoff.DiagnosticSenseCleanup.Stage)
    mHandoff.DiagnosticBusStep=Step;
  return Status;
}
STATIC UINT32 ReadBe32 (CONST UINT8 *P) {
  return ((UINT32)P[0] << 24) | ((UINT32)P[1] << 16) |
         ((UINT32)P[2] << 8) | P[3];
}
STATIC UINT64 ReadBeCells (CONST UINT8 *P, UINT32 Cells) {
  UINT64 V=0; for (UINT32 I=0; I<Cells; ++I) V=(V<<32)|ReadBe32(P+4U*I); return V;
}
STATIC BOOLEAN NodeEnabled (CONST VOID *F, INT32 N) {
  INT32 L; CONST CHAR8 *P;
  if (!F || N<0) return FALSE;
  P=FdtGetProp(F,N,"status",&L);
  return !P || (L==5 && CompareMem(P,"okay",5)==0) ||
         (L==3 && CompareMem(P,"ok",3)==0);
}
STATIC BOOLEAN NodeHasCompatible (CONST VOID *F, INT32 N, CONST CHAR8 *Expected) {
  INT32 L; CONST CHAR8 *P=FdtGetProp(F,N,"compatible",&L);
  UINTN O=0, W; BOOLEAN Found=FALSE;
  if (!P || L<=0 || !Expected) return FALSE;
  W=AsciiStrLen(Expected);
  while(O<(UINTN)L) {
    UINTN E=O;
    while(E<(UINTN)L && P[E]) ++E;
    if(E==(UINTN)L || E==O) return FALSE;
    if(E-O==W && CompareMem(P+O,Expected,W)==0) Found=TRUE;
    O=E+1;
  }
  return Found;
}
STATIC EFI_STATUS ReadU32Property (CONST VOID *F, INT32 N, CONST CHAR8 *Name, UINT32 *V) {
  INT32 L; CONST UINT8 *P;
  if(!F || N<0 || !Name || !V) return EFI_INVALID_PARAMETER;
  P=FdtGetProp(F,N,Name,&L);
  if(!P || L!=4) return EFI_UNSUPPORTED;
  *V=ReadBe32(P); return EFI_SUCCESS;
}
STATIC EFI_STATUS ReadExactString (CONST VOID *F, INT32 N, CONST CHAR8 *Name, CONST CHAR8 *Expected) {
  INT32 L; CONST CHAR8 *P=FdtGetProp(F,N,Name,&L); UINTN W=AsciiStrLen(Expected);
  if(!P || L!=(INT32)(W+1U) || CompareMem(P,Expected,W)!=0 || P[W]!=0) return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ReadStringList (CONST VOID *F, INT32 N, CONST CHAR8 *Name,
                                  CONST CHAR8 *CONST *Expected, UINTN Count) {
  INT32 L; CONST CHAR8 *P=FdtGetProp(F,N,Name,&L); UINTN O=0;
  if(!P || L<=0 || !Expected) return EFI_UNSUPPORTED;
  for(UINTN I=0; I<Count; ++I) {
    UINTN B=AsciiStrLen(Expected[I])+1U;
    if(O+B>(UINTN)L || CompareMem(P+O,Expected[I],B)!=0) return EFI_UNSUPPORTED;
    O+=B;
  }
  return O==(UINTN)L ? EFI_SUCCESS : EFI_UNSUPPORTED;
}
STATIC EFI_STATUS ReadExactReg (CONST VOID *F, INT32 N, INT32 Parent, UINT32 Tuple,
                                UINT64 Base, UINT64 Size) {
  UINT32 Ac,Sc; UINTN Tb,O; INT32 L; CONST UINT8 *P; EFI_STATUS S;
  S=ReadU32Property(F,Parent,"#address-cells",&Ac); if(EFI_ERROR(S)) return S;
  S=ReadU32Property(F,Parent,"#size-cells",&Sc); if(EFI_ERROR(S)) return S;
  if(!Ac || Ac>2 || !Sc || Sc>2) return EFI_UNSUPPORTED;
  Tb=(Ac+Sc)*4U; P=FdtGetProp(F,N,"reg",&L);
  if(!P || L<=0 || (UINTN)L%Tb || Tuple>=(UINTN)L/Tb) return EFI_UNSUPPORTED;
  O=Tuple*Tb;
  return ReadBeCells(P+O,Ac)==Base &&
         ReadBeCells(P+O+Ac*4U,Sc)==Size ? EFI_SUCCESS : EFI_UNSUPPORTED;
}
STATIC EFI_STATUS ReadI2cAddress (CONST VOID *F, INT32 N, UINT32 Address) {
  INT32 L; CONST UINT8 *P=FdtGetProp(F,N,"reg",&L);
  return P && L==4 && ReadBe32(P)==Address ? EFI_SUCCESS : EFI_UNSUPPORTED;
}
STATIC EFI_STATUS ReadGpioRef (CONST VOID *F, INT32 N, CONST CHAR8 *Prop, UINT32 Pin) {
  INT32 L,G; CONST UINT8 *P=FdtGetProp(F,N,Prop,&L); UINT32 Ph;
  if(!P || L!=12 || ReadBe32(P+4)!=Pin || ReadBe32(P+8)!=0) return EFI_UNSUPPORTED;
  Ph=ReadBe32(P); G=FdtNodeOffsetByPhandle(F,Ph);
  if(G<0 || !NodeEnabled(F,G) || !NodeHasCompatible(F,G,"mediatek,mt6789-pinctrl"))
    return EFI_UNSUPPORTED;
  return ReadExactReg(F,G,FdtParentOffset(F,G),0,PINCTRL_BASE,PINCTRL_BYTES);
}
STATIC EFI_STATUS ValidateClockProvider (CONST VOID *F, UINT32 Ph, UINT64 Base) {
  INT32 N,L; UINT32 Cells; CONST UINT8 *Reg; EFI_STATUS S;
  N=FdtNodeOffsetByPhandle(F,Ph); if(N<0 || !NodeEnabled(F,N)) return EFI_UNSUPPORTED;
  S=ReadU32Property(F,N,"#clock-cells",&Cells);
  if(EFI_ERROR(S) || Cells!=1) return EFI_UNSUPPORTED;
  Reg=FdtGetProp(F,N,"reg",&L);
  if(!Reg || L!=16 || ReadBeCells(Reg,2)!=Base || ReadBeCells(Reg+8,2)!=0x1000)
    return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ValidateUsbClocks (CONST VOID *F, INT32 Usb) {
  STATIC CONST CHAR8 *Names[3]={"sys_clk","ref_clk","src_clk"};
  STATIC CONST UINT32 Ids[3]={0x22,0x1D,0x0D};
  STATIC CONST UINT64 Bases[3]={0x10001000ULL,0x10000000ULL,0x1000C000ULL};
  INT32 L; CONST UINT8 *P=FdtGetProp(F,Usb,"clocks",&L);
  if(EFI_ERROR(ReadStringList(F,Usb,"clock-names",Names,3)) || !P || L!=24)
    return EFI_UNSUPPORTED;
  for(UINTN I=0; I<3; ++I)
    if(ReadBe32(P+8*I+4)!=Ids[I] ||
       EFI_ERROR(ValidateClockProvider(F,ReadBe32(P+8*I),Bases[I])))
      return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ValidateTphyClock (CONST VOID *F, INT32 Phy) {
  INT32 L,N; UINT32 Cells,Hz; CONST UINT8 *P; EFI_STATUS S;
  if(EFI_ERROR(ReadExactString(F,Phy,"clock-names","ref"))) return EFI_UNSUPPORTED;
  P=FdtGetProp(F,Phy,"clocks",&L); if(!P || L!=4) return EFI_UNSUPPORTED;
  N=FdtNodeOffsetByPhandle(F,ReadBe32(P));
  if(N<0 || !NodeEnabled(F,N) || !NodeHasCompatible(F,N,"fixed-clock")) return EFI_UNSUPPORTED;
  S=ReadU32Property(F,N,"#clock-cells",&Cells); if(EFI_ERROR(S)||Cells!=0) return EFI_UNSUPPORTED;
  S=ReadU32Property(F,N,"clock-frequency",&Hz);
  return !EFI_ERROR(S) && Hz==26000000U ? EFI_SUCCESS : EFI_UNSUPPORTED;
}
STATIC EFI_STATUS ValidateI2cClocks (CONST VOID *F, INT32 N) {
  STATIC CONST CHAR8 Names[]="main\0dma";
  INT32 L,Node; UINT32 Div; CONST UINT8 *P=FdtGetProp(F,N,"clock-names",&L);
  if(!P || L!=sizeof(Names) || CompareMem(P,Names,sizeof(Names))!=0 ||
     EFI_ERROR(ReadU32Property(F,N,"clock-div",&Div)) || Div!=1) return EFI_UNSUPPORTED;
  P=FdtGetProp(F,N,"clocks",&L); if(!P || L!=16 ||
     ReadBe32(P+4)!=2 || ReadBe32(P+12)!=61) return EFI_UNSUPPORTED;
  Node=FdtNodeOffsetByPhandle(F,ReadBe32(P));
  if(Node<0 || !NodeEnabled(F,Node) ||
     !NodeHasCompatible(F,Node,"mediatek,mt6789-imp_iic_wrap_c") ||
     EFI_ERROR(ReadExactReg(F,Node,FdtParentOffset(F,Node),0,I2C_WRAP_BASE,I2C_WRAP_BYTES)))
    return EFI_UNSUPPORTED;
  Node=FdtNodeOffsetByPhandle(F,ReadBe32(P+8));
  if(Node<0 || !NodeEnabled(F,Node) ||
     EFI_ERROR(ReadExactReg(F,Node,FdtParentOffset(F,Node),0,0x10001000ULL,0x1000ULL)))
    return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ValidateUsbLinks (CONST VOID *F, INT32 Usb, INT32 Ext, INT32 Sc) {
  INT32 Length;
  INT32 UsbPort=FdtSubnodeOffset(F,Usb,"port"),ExtPort=FdtSubnodeOffset(F,Ext,"port");
  INT32 UsbEndpoint,ExtEndpoint,Supply;
  UINT32 Phandle;
  if(!FdtGetProp(F,Usb,"usb-role-switch",&Length)||Length!=0||
     UsbPort<0||ExtPort<0||!NodeEnabled(F,UsbPort)||!NodeEnabled(F,ExtPort))
    return EFI_UNSUPPORTED;
  UsbEndpoint=FdtSubnodeOffset(F,UsbPort,"endpoint@0");
  ExtEndpoint=FdtSubnodeOffset(F,ExtPort,"endpoint@0");
  Supply=FdtSubnodeOffset(F,Sc,"usb-otg-vbus");
  if(UsbEndpoint<0||ExtEndpoint<0||Supply<0||
     !NodeEnabled(F,UsbEndpoint)||!NodeEnabled(F,ExtEndpoint)||!NodeEnabled(F,Supply)||
     EFI_ERROR(ReadU32Property(F,UsbEndpoint,"remote-endpoint",&Phandle))||
     FdtNodeOffsetByPhandle(F,Phandle)!=ExtEndpoint||
     EFI_ERROR(ReadU32Property(F,ExtEndpoint,"remote-endpoint",&Phandle))||
     FdtNodeOffsetByPhandle(F,Phandle)!=UsbEndpoint||
     EFI_ERROR(ReadU32Property(F,Ext,"vbus-supply",&Phandle))||
     FdtNodeOffsetByPhandle(F,Phandle)!=Supply||
     EFI_ERROR(ReadExactString(F,Supply,"regulator-compatible","sc8989x,otg-vbus")))
    return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
STATIC BOOLEAN BoardTreeMatches (CONST VOID *F,UINTN Bytes) {
  return F!=NULL&&mHandoff.ValidatedBoardTree!=NULL&&
    Bytes==mHandoff.ValidatedBoardTreeBytes&&
    CompareMem(F,mHandoff.ValidatedBoardTree,Bytes)==0;
}
STATIC EFI_STATUS ValidateBoardTree (CONST VOID *F, UINTN Bytes) {
  INT32 Soc,Usb,Bus,Phy,I2c,Ext,Sc,Tcpc,Pinctrl,L; UINT32 V,Ph; CONST UINT8 *P;
  if(BoardTreeMatches(F,Bytes)) return EFI_SUCCESS;
  if(!F || Bytes<sizeof(FDT_HEADER) || Bytes>MAX_FDT_BYTES ||
     FdtCheckFull(F,Bytes)!=0 || FdtTotalSize(F)!=Bytes ||
     !NodeHasCompatible(F,0,"mediatek,MT6789")) return EFI_COMPROMISED_DATA;
  Soc=FdtPathOffset(F,"/soc"); Usb=FdtPathOffset(F,USB_NODE);
  Bus=FdtPathOffset(F,PHY_BUS_NODE); Phy=FdtPathOffset(F,PHY_NODE);
  I2c=FdtPathOffset(F,I2C_NODE); Ext=FdtPathOffset(F,EXTCON_NODE);
  Sc=FdtPathOffset(F,SC_NODE); Tcpc=FdtPathOffset(F,TCPC_NODE);
  if(Soc<0||Usb<0||Bus<0||Phy<0||I2c<0||Ext<0||Sc<0||Tcpc<0 ||
     !NodeEnabled(F,Soc)||!NodeEnabled(F,Usb)||!NodeEnabled(F,Bus)||!NodeEnabled(F,Phy)||
     !NodeEnabled(F,I2c)||!NodeEnabled(F,Ext)||!NodeEnabled(F,Sc)||!NodeEnabled(F,Tcpc)||
     FdtParentOffset(F,Usb)!=Soc||FdtParentOffset(F,Bus)!=Soc||
     FdtParentOffset(F,Phy)!=Bus||FdtParentOffset(F,I2c)!=Soc||
     FdtParentOffset(F,Sc)!=I2c||FdtParentOffset(F,Tcpc)!=I2c||
     !NodeHasCompatible(F,Usb,"mediatek,mt6789-usb20")||
     !NodeHasCompatible(F,Bus,"mediatek,generic-tphy-v2")||
     !NodeHasCompatible(F,I2c,"mediatek,mt6983-i2c")||
     !NodeHasCompatible(F,Ext,"mediatek,extcon-usb")||
     !NodeHasCompatible(F,Sc,"southchip,sc89890h")||
     !NodeHasCompatible(F,Tcpc,"hynetek,husb311")) return EFI_UNSUPPORTED;
  if(EFI_ERROR(ReadU32Property(F,Soc,"#address-cells",&V))||V!=2||
     EFI_ERROR(ReadU32Property(F,Soc,"#size-cells",&V))||V!=2) return EFI_UNSUPPORTED;
  if(EFI_ERROR(ReadExactReg(F,Usb,Soc,0,0x11200000ULL,0x10000ULL))||
     EFI_ERROR(ReadExactReg(F,Usb,Soc,1,0x11F40000ULL,0x10000ULL))||
     EFI_ERROR(ReadExactReg(F,Phy,Bus,0,0x11F40000ULL,0x700ULL))||
     EFI_ERROR(ReadExactReg(F,I2c,Soc,0,I2C_BASE,I2C_BYTES))||
     EFI_ERROR(ReadExactReg(F,I2c,Soc,1,I2C_DMA_BASE,I2C_DMA_BYTES))||
     EFI_ERROR(ReadI2cAddress(F,Sc,SC_ADDRESS))||
     EFI_ERROR(ReadI2cAddress(F,Tcpc,TCPC_ADDRESS))||
     EFI_ERROR(ReadU32Property(F,Sc,"sc,sc8989x,iboost",&V))||V!=3U||
     EFI_ERROR(ReadU32Property(F,Sc,"sc,sc8989x,votg",&V))||V!=12U) return EFI_UNSUPPORTED;
  if(EFI_ERROR(ReadExactString(F,Usb,"dr_mode","otg"))||
     EFI_ERROR(ReadU32Property(F,Usb,"mode",&V))||V!=2||
     EFI_ERROR(ReadU32Property(F,Phy,"#phy-cells",&V))||V!=1||
     EFI_ERROR(ReadExactString(F,Tcpc,"tcpc,name","type_c_port0"))||
     EFI_ERROR(ReadExactString(F,Ext,"tcpc","type_c_port0"))) return EFI_UNSUPPORTED;
  P=FdtGetProp(F,Bus,"ranges",&L); if(!P||L!=0) return EFI_UNSUPPORTED;
  P=FdtGetProp(F,Usb,"phys",&L);
  if(!P||L!=8||ReadBe32(P+4)!=3||FdtNodeOffsetByPhandle(F,ReadBe32(P))!=Phy)
    return EFI_UNSUPPORTED;
  P=FdtGetProp(F,Usb,"interrupts",&L);
  if(!P||L!=16||ReadBe32(P)!=0||ReadBe32(P+4)!=0x80||
     ReadBe32(P+8)!=4||ReadBe32(P+12)!=0) return EFI_UNSUPPORTED;
  P=FdtGetProp(F,Sc,"sc,sc8989x,otg-gpio",&L);
  if(!P||L!=12||ReadBe32(P+4)!=86||ReadBe32(P+8)!=0) return EFI_UNSUPPORTED;
  Ph=ReadBe32(P); Pinctrl=FdtNodeOffsetByPhandle(F,Ph);
  if(Pinctrl<0||!NodeHasCompatible(F,Pinctrl,"mediatek,mt6789-pinctrl")||
     EFI_ERROR(ReadExactReg(F,Pinctrl,FdtParentOffset(F,Pinctrl),0,PINCTRL_BASE,PINCTRL_BYTES))
    ) return EFI_UNSUPPORTED;
  P=FdtGetProp(F,Ext,"charger",&L);
  if(!P||L!=4||FdtNodeOffsetByPhandle(F,ReadBe32(P))!=Sc) return EFI_UNSUPPORTED;
  if(EFI_ERROR(ValidateUsbLinks(F,Usb,Ext,Sc))||
     EFI_ERROR(ReadGpioRef(F,Ext,"id-gpios",10))||
     EFI_ERROR(ReadGpioRef(F,Ext,"usb_msel-gpios",99))||
     EFI_ERROR(ReadGpioRef(F,Ext,"usb_5v_en-gpios",4))||
     EFI_ERROR(ReadGpioRef(F,Ext,"hub_reset-gpios",104))||
     EFI_ERROR(ReadGpioRef(F,Sc,"sc,sc8989x,en-gpio",48))||
     EFI_ERROR(ValidateUsbClocks(F,Usb))||
     EFI_ERROR(ValidateTphyClock(F,Phy))||
     EFI_ERROR(ValidateI2cClocks(F,I2c))) return EFI_UNSUPPORTED;
  // A live table can be edited in place. Reuse validation only for identical
  // bytes; GPIO, power and clock observations are still sampled at use.
  if(!mHandoff.BootServicesLeaving&&!mHandoff.ExitingBootServices) {
    VOID *Copy=AllocateCopyPool(Bytes,F);
    if(Copy!=NULL) {
      if(mHandoff.ValidatedBoardTree!=NULL)FreePool(mHandoff.ValidatedBoardTree);
      mHandoff.ValidatedBoardTree=Copy;
      mHandoff.ValidatedBoardTreeBytes=Bytes;
    }
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS GetRuntimeFdt (CONST VOID **F, UINTN *Bytes) {
  VOID *Table; UINT64 N; EFI_STATUS S;
  if(!F||!Bytes) return EFI_INVALID_PARAMETER;
  *F=NULL; *Bytes=0;
  S=EfiGetSystemConfigurationTable(&gFdtTableGuid,&Table);
  if(EFI_ERROR(S)||!Table) return EFI_NOT_FOUND;
  if(MmioRead64(HANDOFF_PAGE)!=HANDOFF_MAGIC||
     MmioRead64(HANDOFF_PAGE+0x48)!=(UINT64)(UINTN)Table||
     MmioRead64(HANDOFF_PAGE+0x58)!=EFI_SUCCESS) return EFI_COMPROMISED_DATA;
  N=MmioRead64(HANDOFF_PAGE+0x50);
  if(N<sizeof(FDT_HEADER)||N>MAX_FDT_BYTES||
     (!BoardTreeMatches(Table,(UINTN)N)&&
      (FdtCheckFull(Table,(UINTN)N)!=0||FdtTotalSize(Table)!=N))) return EFI_COMPROMISED_DATA;
  *F=Table; *Bytes=(UINTN)N; return EFI_SUCCESS;
}
STATIC EFI_STATUS ValidateGcd (UINT64 Base, UINT64 Bytes) {
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D; UINT64 O; EFI_STATUS S;
  if(!gDS||!Bytes) return EFI_NOT_READY;
  S=gDS->GetMemorySpaceDescriptor(Base,&D);
  if(EFI_ERROR(S)||D.GcdMemoryType!=EfiGcdMemoryTypeMemoryMappedIo||Base<D.BaseAddress)
    return EFI_UNSUPPORTED;
  O=Base-D.BaseAddress;
  if(O>D.Length||Bytes>D.Length-O||(D.Attributes&EFI_MEMORY_UC)==0||
     (D.Attributes&(EFI_MEMORY_RO|EFI_MEMORY_RP|EFI_MEMORY_WP))) return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ValidateController (MTK_I2C_CONTROLLER *C) {
  if(!C) return EFI_INVALID_PARAMETER;
  return C->Revision==MTK_I2C_CONTROLLER_REVISION &&
    C->Flags==MTK_I2C_REQUIRED_FLAGS &&
    C->ControllerBase==I2C_BASE && C->ControllerSize==I2C_BYTES &&
    C->DmaBase==I2C_DMA_BASE && C->DmaSize==I2C_DMA_BYTES &&
    C->IommuBase==0 && C->DmaId==0 && C->SourceClockHz ?
    EFI_SUCCESS : EFI_UNSUPPORTED;
}
STATIC EFI_STATUS ClockGateReady (MTK_CLOCK_PROTOCOL *C, CONST CHAR8 *Name, BOOLEAN Mux) {
  UINT32 Id,Parent; BOOLEAN On; EFI_STATUS S;
  if(!C||!C->GetId||!C->IsEnabled) return EFI_NOT_READY;
  S=C->GetId(Name,&Id); if(EFI_ERROR(S)) return S;
  S=C->IsEnabled(Id,&On); if(EFI_ERROR(S)||!On) return EFI_NOT_READY;
  if(!Mux) return EFI_SUCCESS;
  if(!C->GetParent) return EFI_NOT_READY;
  S=C->GetParent(Id,&Parent); if(EFI_ERROR(S)) return S;
  S=C->IsEnabled(Parent,&On); return EFI_ERROR(S)||!On ? EFI_NOT_READY : EFI_SUCCESS;
}
STATIC EFI_STATUS ValidateI2cLive (VOID) {
  STATIC CONST CHAR8 *Names[6]={"TOP_I2C_SEL","TOP_AXI_SEL","IFRAO_I2C_PSEUDO",
                                 "IFRAO_APDMA_PSEUDO","IFRAO_AP_DMA",
                                 "IMPC_AP_CLOCK_I2C6"};
  UINT32 Id,Mode,Lines,Mask; UINT64 Hz; EFI_STATUS S;
  if(!mHandoff.Clock||!mHandoff.Clock->GetId||!mHandoff.Clock->GetFrequency)
    return BusFailure(0x10U,EFI_NOT_READY);
  if(EFI_ERROR(ValidateGcd(I2C_BASE,I2C_BYTES))||
     EFI_ERROR(ValidateGcd(I2C_DMA_BASE,I2C_DMA_BYTES))||
     EFI_ERROR(ValidateGcd(I2C_WRAP_BASE,I2C_WRAP_BYTES))||
     EFI_ERROR(ValidateGcd(PINCTRL_BASE,PINCTRL_BYTES)))
    return BusFailure(0x27U,EFI_UNSUPPORTED);
  for(UINTN I=0;I<ARRAY_SIZE(Names);++I) {
    S=ClockGateReady(mHandoff.Clock,Names[I],I<2);
    if(EFI_ERROR(S))return BusFailure((UINT16)(0x20U+I),S);
  }
  S=mHandoff.Clock->GetId("TOP_I2C_PSEUDO",&Id);
  if(EFI_ERROR(S))return BusFailure(0x26U,S);
  S=mHandoff.Clock->GetFrequency(Id,&Hz);
  if(EFI_ERROR(S)||Hz<1000000ULL||Hz>1000000000ULL)
    return BusFailure(0x26U,EFI_UNSUPPORTED);
  // The stock wrapper uses normal SET/CLR clock gates: status 1 is
  // gated, enable writes CLR(+E04). This is a clock status, not a reset.
  if(MmioRead32((UINTN)I2C_WRAP_BASE+0xE00)&I2C_WRAP_CLOCK_GATED_BIT)
    return BusFailure(0x28U,EFI_NOT_READY);
  Mode=MmioRead32((UINTN)PINCTRL_BASE+0x300+((I2C_PIN_PAIR/8)*0x10));
  if(((Mode>>((I2C_PIN_PAIR%8)*4))&0xFF)!=0x11)
    return BusFailure(0x29U,EFI_NOT_READY);
  Mask=3U<<(I2C_PIN_PAIR%32);
  Lines=MmioRead32((UINTN)PINCTRL_BASE+0x200+((I2C_PIN_PAIR/32)*0x10));
  if((Lines&Mask)!=Mask)return BusFailure(0x2aU,EFI_NOT_READY);
  for(UINTN I=0;I<2;++I) {
    if(MmioRead16((UINTN)I2C_BASE+0x24)&1U)return BusFailure(0x2bU,EFI_NOT_READY);
    if(MmioRead32((UINTN)I2C_DMA_BASE+8)&1U)return BusFailure(0x2cU,EFI_NOT_READY);
    if(!I)MicroSecondDelay(10);
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS FindController (EFI_HANDLE *Out, MTK_I2C_CONTROLLER **Meta) {
  EFI_HANDLE *Hs,Match=NULL; UINTN Count=0,Matches=0; MTK_I2C_CONTROLLER *Found=NULL;
  EFI_STATUS S;
  if(!Out||!Meta) return EFI_INVALID_PARAMETER;
  *Out=NULL; *Meta=NULL;
  S=gBS->LocateHandleBuffer(ByProtocol,&gMediaTekI2cControllerProtocolGuid,NULL,&Count,&Hs);
  if(EFI_ERROR(S)) return S;
  for(UINTN I=0;I<Count;++I) {
    MTK_I2C_CONTROLLER *C=NULL;
    S=gBS->HandleProtocol(Hs[I],&gMediaTekI2cControllerProtocolGuid,(VOID **)&C);
    if(EFI_ERROR(S)||!C) continue;
    if(C->ControllerBase==I2C_BASE||C->DmaBase==I2C_DMA_BASE) {
      S=ValidateController(C);
      if(EFI_ERROR(S)) { FreePool(Hs); return S; }
      Match=Hs[I]; Found=C; ++Matches;
    }
  }
  FreePool(Hs);
  if(Matches!=1) return Matches ? EFI_COMPROMISED_DATA : EFI_NOT_FOUND;
  *Out=Match; *Meta=Found; return EFI_SUCCESS;
}
STATIC EFI_STATUS ValidateI2cMaster (EFI_HANDLE H, MTK_I2C_CONTROLLER **C,
                                     EFI_I2C_MASTER_PROTOCOL **M) {
  EFI_STATUS S; EFI_I2C_MASTER_PROTOCOL *Master=NULL; MTK_I2C_CONTROLLER *Meta=NULL;
  if(!H||!C||!M) return EFI_INVALID_PARAMETER;
  S=gBS->HandleProtocol(H,&gMediaTekI2cControllerProtocolGuid,(VOID **)&Meta);
  if(EFI_ERROR(S)||EFI_ERROR(ValidateController(Meta)))return BusFailure(3U,EFI_NOT_READY);
  S=gBS->HandleProtocol(H,&gEfiI2cMasterProtocolGuid,(VOID **)&Master);
  if(EFI_ERROR(S)||!Master||!Master->StartRequest)return BusFailure(4U,EFI_NOT_READY);
  S=ValidateI2cLive(); if(EFI_ERROR(S)) return S;
  *C=Meta; *M=Master; return EFI_SUCCESS;
}
STATIC VOID EFIAPI CapturePowerBeforeI2cExit (EFI_EVENT Event,VOID *Context);
STATIC EFI_STATUS RegisterPowerExitObserver (VOID) {
  MTK_I2C_EXCLUSIVE_PROTOCOL *P=NULL; EFI_STATUS S;
  if(mHandoff.I2cExitObserverRegistered) return EFI_SUCCESS;
  S=gBS->HandleProtocol(mHandoff.I2cHandle,&gMediaTekI2cExclusiveProtocolGuid,(VOID **)&P);
  if(EFI_ERROR(S)||!P||P->Revision!=MTK_I2C_EXCLUSIVE_PROTOCOL_REVISION||
     !P->Acquire||!P->Transfer||!P->Release||!P->RegisterExitObserver||
     !P->UnregisterExitObserver)return BusFailure(7U,EFI_NOT_READY);
  if(mHandoff.I2cExclusive&&mHandoff.I2cExclusive!=P)return BusFailure(0x42U,EFI_ACCESS_DENIED);
  S=P->RegisterExitObserver(P,CapturePowerBeforeI2cExit,&mHandoff);
  if(EFI_ERROR(S)&&S!=EFI_ALREADY_STARTED)return BusFailure(8U,S);
  mHandoff.I2cExclusive=P; mHandoff.I2cExitObserverRegistered=TRUE;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EnsureBoardReady (CONST VOID *F, UINTN Bytes) {
  EFI_HANDLE H; MTK_I2C_CONTROLLER *C; EFI_I2C_MASTER_PROTOCOL *M;
  STOCK_POWER_BOARD_READY_PROTOCOL *Old=NULL; EFI_STATUS S;
  if(EFI_ERROR(ValidateBoardTree(F,Bytes)))return BusFailure(1U,EFI_UNSUPPORTED);
  if(mHandoff.BoardReadyInstalled) {
    S=ValidateI2cMaster(mHandoff.I2cHandle,&C,&M); if(EFI_ERROR(S)) return S;
    S=gBS->HandleProtocol(mHandoff.I2cHandle,&gStockPowerBoardReadyProtocolGuid,(VOID **)&Old);
    if(EFI_ERROR(S)||Old!=&mHandoff.BoardReady)return BusFailure(5U,EFI_ACCESS_DENIED);
    mHandoff.I2cController=C; return RegisterPowerExitObserver();
  }
  S=FindController(&H,&C);if(EFI_ERROR(S))return BusFailure(2U,S);
  mHandoff.I2cController=C;
  S=ValidateI2cMaster(H,&C,&M); if(EFI_ERROR(S)) return S;
  S=gBS->HandleProtocol(H,&gStockPowerBoardReadyProtocolGuid,(VOID **)&Old);
  if(!EFI_ERROR(S))return BusFailure(5U,EFI_ACCESS_DENIED);
  if(S!=EFI_UNSUPPORTED)return BusFailure(5U,S);
  mHandoff.I2cHandle=H; mHandoff.I2cController=C;
  mHandoff.BoardReady=(STOCK_POWER_BOARD_READY_PROTOCOL){
    STOCK_POWER_BOARD_READY_SIGNATURE,STOCK_POWER_BOARD_READY_REVISION,
    STOCK_POWER_BOARD_READY_ALLOW_STATUS_READ,H,STOCK_POWER_SC8989X_ADDRESS,
    STOCK_POWER_HUSB311_ADDRESS
  };
  S=gBS->InstallProtocolInterface(&H,&gStockPowerBoardReadyProtocolGuid,
                                  EFI_NATIVE_INTERFACE,&mHandoff.BoardReady);
  if(EFI_ERROR(S))return BusFailure(6U,S);
  mHandoff.BoardReadyInstalled=TRUE; mHandoff.I2cHandle=H; return RegisterPowerExitObserver();
}
STATIC EFI_STATUS ReadClockEvidence (CONST VOID *F, MTK_MUSB_PLATFORM_EVIDENCE *E) {
  INT32 Usb,L; CONST UINT8 *P; UINT32 Id[3],Ref,Parent,Top;
  STATIC CONST CHAR8 *Names[3]={"IFRAO_SSUSB","TOP_USB_SEL","APMIXED_USBPLL"};
  CONST CHAR8 *Name; BOOLEAN On; UINT64 Hz,ParentHz; EFI_STATUS S;
  if(!mHandoff.Clock||!mHandoff.Clock->GetName||!mHandoff.Clock->GetId||
     !mHandoff.Clock->GetFrequency||!mHandoff.Clock->IsEnabled||!mHandoff.Clock->GetParent)
    return EFI_NOT_READY;
  Usb=FdtPathOffset(F,USB_NODE); P=FdtGetProp(F,Usb,"clocks",&L);
  if(Usb<0||!P||L!=24) return EFI_UNSUPPORTED;
  E->ClockReadMask=E->ClockEnabledMask=0;
  for(UINTN I=0;I<3;++I) {
    // DT IDs belong to separate clock providers. The UEFI protocol uses a
    // single enum, so translate the verified provider/name through GetId.
    S=mHandoff.Clock->GetId(Names[I],&Id[I]); if(EFI_ERROR(S)) return S;
    S=mHandoff.Clock->GetName(Id[I],&Name);
    if(EFI_ERROR(S)||!Name||AsciiStrCmp(Name,Names[I])) return EFI_UNSUPPORTED;
    S=mHandoff.Clock->IsEnabled(Id[I],&On);
    if(EFI_ERROR(S)) return S;
    if(!On) return EFI_NOT_STARTED;
    S=mHandoff.Clock->GetFrequency(Id[I],&Hz); if(EFI_ERROR(S)||!Hz) return EFI_NOT_READY;
    E->ClockReadMask|=1U<<I; E->ClockEnabledMask|=1U<<I;
    E->ClockHz[I]=Hz; E->ExpectedClockHz[I]=Hz;
  }
  S=mHandoff.Clock->GetId("TOP_CLK26M",&Ref); if(EFI_ERROR(S)) return S;
  S=mHandoff.Clock->IsEnabled(Ref,&On); if(EFI_ERROR(S)||!On) return EFI_NOT_READY;
  S=mHandoff.Clock->GetFrequency(Ref,&Hz);
  if(EFI_ERROR(S)||Hz!=26000000ULL) return EFI_UNSUPPORTED;
  E->RefClockEnabled=1; E->RefClockHz=Hz;
  Top=Id[1]; S=mHandoff.Clock->GetParent(Top,&Parent); if(EFI_ERROR(S)) return S;
  S=mHandoff.Clock->GetName(Parent,&Name);
  if(EFI_ERROR(S)||!Name||
     (AsciiStrCmp(Name,"TOP_TCK_26M_MX9")&&AsciiStrCmp(Name,"TOP_UNIVPLL_D5_D4")&&
      AsciiStrCmp(Name,"TOP_UNIVPLL_D6_D4")&&AsciiStrCmp(Name,"TOP_UNIVPLL_D5_D2")))
    return EFI_UNSUPPORTED;
  S=mHandoff.Clock->IsEnabled(Parent,&On); if(EFI_ERROR(S)||!On) return EFI_NOT_READY;
  S=mHandoff.Clock->GetFrequency(Parent,&ParentHz);
  if(EFI_ERROR(S)||!ParentHz) return EFI_NOT_READY;
  S=mHandoff.Clock->GetFrequency(Top,&Hz); if(EFI_ERROR(S)||Hz!=ParentHz) return EFI_NOT_READY;
  E->TopUsbParent=E->ExpectedTopUsbParent=Parent;
  E->TopUsbFrequencyHz=E->ExpectedTopUsbFrequencyHz=Hz;
  if(mHandoff.ColdClockBound) {
    if(Parent!=mHandoff.ColdClockParent) return EFI_NOT_READY;
    for(UINTN I=0U;I<3U;++I)
      if(E->ClockHz[I]!=mHandoff.ColdClockHz[I]) return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ReadGpioSample (UINT32 Pin, MTK_MUSB_PLATFORM_GPIO_SAMPLE *Out) {
  BOOLEAN Dir,State;
  if(!mHandoff.Gpio||!mHandoff.Gpio->GetDir||!mHandoff.Gpio->GetState||!Out)
    return EFI_NOT_READY;
  if(EFI_ERROR(mHandoff.Gpio->GetDir(Pin,&Dir))||
     EFI_ERROR(mHandoff.Gpio->GetState(Pin,&State))) return EFI_DEVICE_ERROR;
  Out->Number=Pin; Out->Direction=Dir?MTK_MUSB_PLATFORM_GPIO_OUTPUT:MTK_MUSB_PLATFORM_GPIO_INPUT;
  Out->PhysicalInput=State?1:0;
  Out->Mode=(MmioRead32((UINTN)PINCTRL_BASE+PIN_MODE_OFFSET(Pin))>>PIN_MODE_SHIFT(Pin))&0xF;
  // LK does not enable the input buffer of every output pad. A driven GPIO is
  // described by its output latch; its input register may read zero.
  if(Dir&&Out->Mode==0U)
    Out->PhysicalInput=(MmioRead32((UINTN)PINCTRL_BASE+0x100U+(Pin/32U)*0x10U)>>(Pin%32U))&1U;
  return EFI_SUCCESS;
}
// These three USB-specific gates have known parents. A selected shared
// parent must already be enabled: no shared PLL, mux parent or frequency is
// changed here. The retained USBPLL rate is checked before binding these gates.
STATIC EFI_STATUS ColdClockSnapshotMatches (CONST VOID *F) {
  MTK_MUSB_PLATFORM_EVIDENCE E={0};
  if(!mHandoff.ColdClockBound||EFI_ERROR(ReadClockEvidence(F,&E))||
     E.TopUsbParent!=mHandoff.ColdClockParent) return EFI_NOT_READY;
  for(UINTN I=0;I<3U;++I)
    if(E.ClockHz[I]!=mHandoff.ColdClockHz[I]) return EFI_NOT_READY;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ReleaseColdClocks (VOID) {
  CONST VOID *F; UINTN Bytes; BOOLEAN On; UINT32 Parent; UINT64 Hz;
  EFI_STATUS S;
  if(!mHandoff.ColdClockBound) return EFI_SUCCESS;
  if(mHandoff.ReadyInstalled||mHandoff.ExitingBootServices||
     !mHandoff.Clock||!mHandoff.Clock->SetEnable||
     EFI_ERROR(GetRuntimeFdt(&F,&Bytes))||
     EFI_ERROR(ValidateBoardTree(F,Bytes))) return EFI_NOT_READY;
  // Do not gate a controller with unconfirmed live transfers/session.
  S=mHandoff.Clock->IsEnabled(mHandoff.ColdClockId[0],&On);
  if(EFI_ERROR(S)) return S;
  if(On&&(EFI_ERROR(ValidateGcd(MTK_MUSB_PLATFORM_CONTROLLER_BASE,MTK_MUSB_PLATFORM_CONTROLLER_BYTES))||
          MtkMusbPlatformCheckQuiescent(&mHandoff.Io,1U)!=MtkMusbPlatformSuccess))
    return EFI_NOT_READY;
  S=mHandoff.Clock->GetParent(mHandoff.ColdClockId[1],&Parent);
  if(EFI_ERROR(S)||Parent!=mHandoff.ColdClockParent) return EFI_NOT_READY;
  // Verify the complete dependency chain before turning off its first gate.
  // A parent/rate change belongs to another owner and blocks all our writes.
  for(UINTN I=0U;I<3U;++I) {
    S=mHandoff.Clock->GetFrequency(mHandoff.ColdClockId[I],&Hz);
    if(EFI_ERROR(S)||Hz!=mHandoff.ColdClockHz[I]) return EFI_NOT_READY;
  }
  STATIC CONST UINT8 Reverse[]={0U,2U,1U};
  for(UINTN N=0;N<ARRAY_SIZE(Reverse);++N) {
    UINTN I=Reverse[N]; UINT8 Bit=(UINT8)(1U<<I);
    if(!(mHandoff.ColdClockOwnedMask&Bit)) continue;
    S=mHandoff.Clock->GetFrequency(mHandoff.ColdClockId[I],&Hz);
    if(EFI_ERROR(S)||Hz!=mHandoff.ColdClockHz[I]) return EFI_NOT_READY;
    S=mHandoff.Clock->IsEnabled(mHandoff.ColdClockId[I],&On);
    if(EFI_ERROR(S)) return S;
    if(On) {
      S=mHandoff.Clock->SetEnable(mHandoff.ColdClockId[I],FALSE);
      if(EFI_ERROR(S)) return S;
      S=mHandoff.Clock->IsEnabled(mHandoff.ColdClockId[I],&On);
      if(EFI_ERROR(S)||On) return EFI_DEVICE_ERROR;
    }
    mHandoff.ColdClockOwnedMask&=(UINT8)~Bit;
  }
  mHandoff.ColdClockBound=FALSE; return EFI_SUCCESS;
}
STATIC EFI_STATUS PrepareColdClocks (CONST VOID *F) {
  STATIC CONST CHAR8 *Names[]={"IFRAO_SSUSB","TOP_USB_SEL","APMIXED_USBPLL"};
  STATIC CONST UINT8 Order[]={1U,2U,0U};
  UINT32 Parent,Ref; UINT64 Hz; BOOLEAN On,WasOn[3];
  CONST CHAR8 *Name; EFI_STATUS S;
  if(!F||!mHandoff.ColdInProgress||mHandoff.ColdClockBound||
     mHandoff.ReadyInstalled||mHandoff.ExitingBootServices||
     !mHandoff.Clock||!mHandoff.Clock->GetId||!mHandoff.Clock->GetName||
     !mHandoff.Clock->GetParent||!mHandoff.Clock->GetFrequency||
     !mHandoff.Clock->IsEnabled||!mHandoff.Clock->SetEnable)
    return BusFailure(0x6200U,EFI_NOT_READY);
  ZeroMem(mHandoff.ColdClockHz,sizeof(mHandoff.ColdClockHz));
  if(EFI_ERROR(ValidateGcd(0x10001000ULL,0x1000U)))
    return BusFailure(0x6201U,EFI_UNSUPPORTED);
  if(EFI_ERROR(ValidateGcd(0x10000000ULL,0x1000U)))
    return BusFailure(0x6202U,EFI_UNSUPPORTED);
  if(EFI_ERROR(ValidateGcd(0x1000c000ULL,0x1000U)))
    return BusFailure(0x6203U,EFI_UNSUPPORTED);
  for(UINTN I=0;I<3U;++I) {
    S=mHandoff.Clock->GetId(Names[I],&mHandoff.ColdClockId[I]);
    if(EFI_ERROR(S)) return BusFailure((UINT16)(0x6210U+I),S);
    S=mHandoff.Clock->GetName(mHandoff.ColdClockId[I],&Name);
    if(EFI_ERROR(S)||!Name||AsciiStrCmp(Name,Names[I]))
      return BusFailure((UINT16)(0x6220U+I),EFI_UNSUPPORTED);
    S=mHandoff.Clock->IsEnabled(mHandoff.ColdClockId[I],&WasOn[I]);
    if(EFI_ERROR(S)) return BusFailure((UINT16)(0x6230U+I),S);
    S=mHandoff.Clock->GetFrequency(mHandoff.ColdClockId[I],&mHandoff.ColdClockHz[I]);
    if(EFI_ERROR(S)||(!mHandoff.ColdClockHz[I]&&(I!=2U||WasOn[0]||WasOn[1]||WasOn[2])))
      return BusFailure((UINT16)(0x6240U+I),EFI_NOT_READY);
  }
  S=mHandoff.Clock->GetId("TOP_CLK26M",&Ref); if(EFI_ERROR(S)) return BusFailure(0x6251U,S);
  S=mHandoff.Clock->IsEnabled(Ref,&On);
  if(EFI_ERROR(S)||!On) return BusFailure(0x6252U,EFI_NOT_READY);
  S=mHandoff.Clock->GetFrequency(Ref,&Hz);
  if(EFI_ERROR(S)||Hz!=26000000ULL) return BusFailure(0x6253U,EFI_NOT_READY);
  S=mHandoff.Clock->GetParent(mHandoff.ColdClockId[1],&Parent);
  if(EFI_ERROR(S)) return BusFailure(0x6254U,S);
  S=mHandoff.Clock->GetName(Parent,&Name);
  if(EFI_ERROR(S)||!Name||
     (AsciiStrCmp(Name,"TOP_TCK_26M_MX9")&&AsciiStrCmp(Name,"TOP_UNIVPLL_D5_D4")&&
      AsciiStrCmp(Name,"TOP_UNIVPLL_D6_D4")&&AsciiStrCmp(Name,"TOP_UNIVPLL_D5_D2")))
    return BusFailure(0x6255U,EFI_UNSUPPORTED);
  S=mHandoff.Clock->IsEnabled(Parent,&On);
  if(EFI_ERROR(S)||!On) return BusFailure(0x6256U,EFI_NOT_READY);
  S=mHandoff.Clock->GetFrequency(Parent,&Hz);
  if(EFI_ERROR(S)||!Hz||Hz!=mHandoff.ColdClockHz[1]) return BusFailure(0x6257U,EFI_NOT_READY);
  // On a true cold boot without LK USB init, dedicated USBPLL can still be
  // blank (PCW=0) while all three USB clocks are closed. Program its stock
  // 2496 MHz rate before enabling.
  if(!mHandoff.ColdClockHz[2]) {
    if(!mHandoff.Clock->SetFrequency) return BusFailure(0x6242U,EFI_NOT_READY);
    S=mHandoff.Clock->SetFrequency(mHandoff.ColdClockId[2],2496000000ULL);
    if(EFI_ERROR(S)) return BusFailure(0x6248U,S);
    S=mHandoff.Clock->IsEnabled(mHandoff.ColdClockId[2],&On);
    if(EFI_ERROR(S)||On) return BusFailure(0x6249U,EFI_DEVICE_ERROR);
    S=mHandoff.Clock->GetFrequency(mHandoff.ColdClockId[2],&mHandoff.ColdClockHz[2]);
    if(EFI_ERROR(S)||mHandoff.ColdClockHz[2]!=2496000000ULL)
      return BusFailure(0x624aU,EFI_DEVICE_ERROR);
  }
  // Trial87 reports 2496MHz through the shipped MT6789 USBPLL descriptor.
  if(mHandoff.ColdClockHz[2]!=2496000000ULL) return BusFailure(0x6250U,EFI_UNSUPPORTED);
  mHandoff.ColdClockParent=Parent;
  mHandoff.ColdClockOwnedMask=0U; mHandoff.ColdClockBound=TRUE;
  for(UINTN N=0;N<ARRAY_SIZE(Order);++N) {
    UINTN I=Order[N];
    // Recheck parent/frequency immediately before each attempted enable.
    S=mHandoff.Clock->GetParent(mHandoff.ColdClockId[1],&Parent);
    if(EFI_ERROR(S)||Parent!=mHandoff.ColdClockParent)
      return BusFailure((UINT16)(0x6260U+I),EFI_NOT_READY);
    S=mHandoff.Clock->GetFrequency(mHandoff.ColdClockId[I],&Hz);
    if(EFI_ERROR(S)||Hz!=mHandoff.ColdClockHz[I])
      return BusFailure((UINT16)(0x6270U+I),EFI_NOT_READY);
    S=mHandoff.Clock->IsEnabled(mHandoff.ColdClockId[I],&On);
    if(EFI_ERROR(S)||On!=WasOn[I]) return BusFailure((UINT16)(0x6280U+I),EFI_NOT_READY);
    if(WasOn[I]) continue;
    mHandoff.ColdClockOwnedMask|=(UINT8)(1U<<I); // A failing call may have acted.
    S=mHandoff.Clock->SetEnable(mHandoff.ColdClockId[I],TRUE);
    if(EFI_ERROR(S)) return BusFailure((UINT16)(0x6290U+I),S);
    S=mHandoff.Clock->IsEnabled(mHandoff.ColdClockId[I],&On);
    if(EFI_ERROR(S)||!On) return BusFailure((UINT16)(0x62a0U+I),EFI_DEVICE_ERROR);
  }
  S=ColdClockSnapshotMatches(F);
  return EFI_ERROR(S)?BusFailure(0x62b0U,S):S;
}
#define COLD_PIN_MODE 1U
#define COLD_PIN_DIR  2U
#define COLD_PIN_DO   4U
STATIC EFI_STATUS ReadColdPin (CONST COLD_USB_PIN *Pin,COLD_USB_PIN *Out) {
  MTK_MUSB_PLATFORM_GPIO_SAMPLE G;
  if(!Pin||!Out||EFI_ERROR(ReadGpioSample(Pin->Pin,&G))||G.Mode>7U)
    return EFI_DEVICE_ERROR;
  *Out=*Pin; Out->Mode=(UINT8)G.Mode; Out->Direction=(UINT8)G.Direction;
  Out->Output=(UINT8)((MmioRead32((UINTN)PINCTRL_BASE+0x100U+
                    (Pin->Pin/32U)*0x10U)>>(Pin->Pin%32U))&1U);
  return EFI_SUCCESS;
}
STATIC BOOLEAN ColdPinFieldsMatch (CONST COLD_USB_PIN *Pin,CONST COLD_USB_PIN *Live) {
  return Live->Mode==Pin->ExpectedMode&&
         Live->Direction==Pin->ExpectedDirection&&Live->Output==Pin->ExpectedOutput;
}
STATIC EFI_STATUS SetColdPinField (COLD_USB_PIN *P,UINT8 Field,UINT8 Value) {
  COLD_USB_PIN Live; EFI_STATUS S;
  if(EFI_ERROR(ReadColdPin(P,&Live))||!ColdPinFieldsMatch(P,&Live)) return EFI_NOT_READY;
  UINT8 *Expected=Field==COLD_PIN_MODE?&P->ExpectedMode:
                  (Field==COLD_PIN_DIR?&P->ExpectedDirection:&P->ExpectedOutput);
  if(*Expected==Value) return EFI_SUCCESS;
  P->Owned|=Field; *Expected=Value;
  if(Field==COLD_PIN_MODE) S=mHandoff.Gpio->SetMode(P->Pin,Value);
  else if(Field==COLD_PIN_DIR) S=mHandoff.Gpio->SetDir(P->Pin,Value!=0U);
  else S=mHandoff.Gpio->SetState(P->Pin,Value!=0U);
  if(EFI_ERROR(S)) return S;
  if(EFI_ERROR(ReadColdPin(P,&Live))||!ColdPinFieldsMatch(P,&Live)) return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS RestoreColdPinField (COLD_USB_PIN *P,UINT8 Field) {
  COLD_USB_PIN Live,Target; EFI_STATUS S; UINT8 Original,Current,Expected;
  if(!(P->Owned&Field)) return EFI_SUCCESS;
  if(EFI_ERROR(ReadColdPin(P,&Live))) return EFI_DEVICE_ERROR;
  Original=Field==COLD_PIN_MODE?P->Mode:(Field==COLD_PIN_DIR?P->Direction:P->Output);
  Current=Field==COLD_PIN_MODE?Live.Mode:(Field==COLD_PIN_DIR?Live.Direction:Live.Output);
  Expected=Field==COLD_PIN_MODE?P->ExpectedMode:
           (Field==COLD_PIN_DIR?P->ExpectedDirection:P->ExpectedOutput);
  // A failed write may either have acted or left the original value. Every
  // other field must remain at its last recorded value before we restore.
  if(Current!=Expected&&Current!=Original) return EFI_ACCESS_DENIED;
  if(Field!=COLD_PIN_MODE&&Live.Mode!=P->ExpectedMode) return EFI_ACCESS_DENIED;
  if(Field!=COLD_PIN_DIR&&Live.Direction!=P->ExpectedDirection) return EFI_ACCESS_DENIED;
  if(Field!=COLD_PIN_DO&&Live.Output!=P->ExpectedOutput) return EFI_ACCESS_DENIED;
  if(Current!=Original) {
    if(Field==COLD_PIN_MODE) S=mHandoff.Gpio->SetMode(P->Pin,Original);
    else if(Field==COLD_PIN_DIR) S=mHandoff.Gpio->SetDir(P->Pin,Original!=0U);
    else S=mHandoff.Gpio->SetState(P->Pin,Original!=0U);
    if(EFI_ERROR(S)) return S;
  }
  // Do not discard the last observed value when a restore write is ignored.
  // Only a confirmed full-field readback advances the saved expectation.
  Target=*P;
  if(Field==COLD_PIN_MODE) Target.ExpectedMode=Original;
  else if(Field==COLD_PIN_DIR) Target.ExpectedDirection=Original;
  else Target.ExpectedOutput=Original;
  if(EFI_ERROR(ReadColdPin(P,&Live))||!ColdPinFieldsMatch(&Target,&Live)) return EFI_DEVICE_ERROR;
  P->ExpectedMode=Target.ExpectedMode; P->ExpectedDirection=Target.ExpectedDirection;
  P->ExpectedOutput=Target.ExpectedOutput;
  P->Owned&=(UINT8)~Field; return EFI_SUCCESS;
}
// A failed write may leave an owned field at either our requested value or
// its saved original value. Reconcile all fields before ordered restoration:
// otherwise an ignored MODE write could block a safe direction restore.
STATIC EFI_STATUS ReconcileColdPin (COLD_USB_PIN *P) {
  COLD_USB_PIN L;
  if(EFI_ERROR(ReadColdPin(P,&L))) return EFI_DEVICE_ERROR;
  if((L.Mode!=P->ExpectedMode&&(!(P->Owned&COLD_PIN_MODE)||L.Mode!=P->Mode))||
     (L.Direction!=P->ExpectedDirection&&(!(P->Owned&COLD_PIN_DIR)||L.Direction!=P->Direction))||
     (L.Output!=P->ExpectedOutput&&(!(P->Owned&COLD_PIN_DO)||L.Output!=P->Output)))
    return EFI_ACCESS_DENIED;
  P->ExpectedMode=L.Mode; P->ExpectedDirection=L.Direction; P->ExpectedOutput=L.Output;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ReadBoardId (UINT8 *Id) {
  MTK_MUSB_PLATFORM_GPIO_SAMPLE First,Second;
  if(EFI_ERROR(ValidateGcd(PINCTRL_BASE,PINCTRL_BYTES))||
     EFI_ERROR(ReadGpioSample(10U,&First))||First.Mode!=0U||
     First.Direction!=MTK_MUSB_PLATFORM_GPIO_INPUT||First.PhysicalInput>1U||
     EFI_ERROR(ReadGpioSample(10U,&Second))||Second.Mode!=First.Mode||
     Second.Direction!=First.Direction||Second.PhysicalInput!=First.PhysicalInput)
    return EFI_NOT_READY;
  *Id=(UINT8)First.PhysicalInput;
  return EFI_SUCCESS;
}

STATIC BOOLEAN DeviceRequestMatches (VOID) {
  MTK_MUSB_PLATFORM_GPIO_SAMPLE G;
  return !EFI_ERROR(ReadGpioSample(10U,&G))&&G.Mode==0U&&
    G.Direction==MTK_MUSB_PLATFORM_GPIO_INPUT&&G.PhysicalInput==mHandoff.DeviceId;
}
STATIC EFI_STATUS ReadPowerRegister (UINTN Address,UINT8 Register,UINT8 *Out,UINTN Bytes);
STATIC EFI_STATUS AcquirePowerBus (VOID);
STATIC EFI_STATUS ReleasePowerBus (VOID);
STATIC BOOLEAN Husb311SinkAttached (UINT8 Cc,UINT8 Role,UINT8 Power);
STATIC BOOLEAN Husb311OpenCcPogo (UINT8 Cc,UINT8 Role);
STATIC BOOLEAN ValidateColdTypecOnLease (VOID);
// Call only after fresh clocks-on and controller mapping validation. A cold
// start owns the host POWER value: ENSUSPEND/HSENAB set and inherited
// peripheral SUSPENDM/SOFTCONN clear. Ready/EBS must still match all four.
STATIC BOOLEAN ColdOwnedPowerMatches (VOID) {
  if(!mHandoff.ColdBound) return TRUE;
  if(mHandoff.ColdHost.Kind!=MtkMusbPlatformColdStateStarted||
     !mHandoff.ColdHost.Signature) return FALSE;
  return (MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+1U)&0x63U)==
         mHandoff.ColdHost.ExpectedPower;
}
STATIC EFI_STATUS CollectEvidence (BOOLEAN EbsPower, MTK_MUSB_PLATFORM_EVIDENCE *E) {
  CONST VOID *F; UINTN Bytes; EFI_STATUS S;
  SP_HUSB311_CC_STATUS C;
  UINT32 Pins[MTK_MUSB_PLATFORM_GPIO_COUNT]={
    MTK_MUSB_PLATFORM_GPIO_ID_PIN,MTK_MUSB_PLATFORM_GPIO_MUX_PIN,
    MTK_MUSB_PLATFORM_GPIO_5V_PIN,MTK_MUSB_PLATFORM_GPIO_RESET_PIN,
    MTK_MUSB_PLATFORM_GPIO_OTG_PIN};
  if(!E) return EFI_INVALID_PARAMETER;
  ZeroMem(E,sizeof(*E));
  S=GetRuntimeFdt(&F,&Bytes); if(EFI_ERROR(S)) return S;
  S=ValidateBoardTree(F,Bytes); if(EFI_ERROR(S)) return S;
  if(EFI_ERROR(ValidateGcd(MTK_MUSB_PLATFORM_CONTROLLER_BASE,MTK_MUSB_PLATFORM_CONTROLLER_BYTES))||
     EFI_ERROR(ValidateGcd(MTK_MUSB_PLATFORM_PHY_BASE,MTK_MUSB_PLATFORM_PHY_BYTES))||
     EFI_ERROR(ValidateGcd(PINCTRL_BASE,PINCTRL_BYTES))) return EFI_UNSUPPORTED;
  E->ControllerBase=MTK_MUSB_PLATFORM_CONTROLLER_BASE;
  E->ControllerLength=MTK_MUSB_PLATFORM_CONTROLLER_BYTES;
  E->PhyBase=MTK_MUSB_PLATFORM_PHY_BASE; E->PhyLength=MTK_MUSB_PLATFORM_PHY_BYTES;
  E->ControllerMapped=E->PhyMapped=1;
  S=ReadClockEvidence(F,E); if(EFI_ERROR(S)) return S;
  if(mHandoff.ColdBound&&mHandoff.ColdHost.Kind==MtkMusbPlatformColdStateStarted&&
     !ColdOwnedPowerMatches()) return EFI_NOT_READY;
  for(UINTN I=0;I<MTK_MUSB_PLATFORM_GPIO_COUNT;++I) {
    S=ReadGpioSample(Pins[I],&E->Gpio[I]); if(EFI_ERROR(S)) return S;
  }
  E->PhyState=MTK_MUSB_PLATFORM_PHY_STATE_UNOBSERVED; E->PhyReadMask=0xF;
  E->PhyAcr1=MmioRead32((UINTN)MTK_MUSB_PLATFORM_PHY_BASE+PHY_ACR1);
  E->PhyAcr6=MmioRead32((UINTN)MTK_MUSB_PLATFORM_PHY_BASE+PHY_ACR6);
  E->PhyDtm0=MmioRead32((UINTN)MTK_MUSB_PLATFORM_PHY_BASE+PHY_DTM0);
  E->PhyDtm1=MmioRead32((UINTN)MTK_MUSB_PLATFORM_PHY_BASE+PHY_DTM1);
  if(mHandoff.HostFirst) {
    // Connector power is observed and controlled by StepTypec only.
    ZeroMem(&C,sizeof(C));
  } else if(EbsPower) {
    MemoryFence();
    if(!mHandoff.EbsPowerSampleValid && !mHandoff.EbsScBoostSampleValid) return EFI_NOT_READY;
    E->Sc8989xBoostReadValid=1U;
    E->Sc8989xBoostEnabled=mHandoff.ScBoostEnabled?1U:0U;
    if(mHandoff.EbsPowerSampleValid) {
      E->Husb311PowerStatusReadValid=1U;
      E->Husb311PowerStatus=mHandoff.HusbPowerStatus;
      C=mHandoff.HusbCcStatus;
    } else {
      E->Husb311PowerStatusReadValid=0U;
      E->Husb311PowerStatus=0U;
      ZeroMem(&C,sizeof(C));
    }
  } else {
    UINT8 Sc, Power = 0U, Cc = 0U, Role = 0U;
    BOOLEAN OwnLease = mHandoff.I2cToken == 0U;
    EFI_STATUS Release;
    if (OwnLease) {
      S = AcquirePowerBus();
      if (EFI_ERROR(S)) return S;
    }
    S = ReadPowerRegister(SC_ADDRESS, 3U, &Sc, 1U);
    if (!EFI_ERROR(S) && !mHandoff.IdLowBound) {
      S = ReadPowerRegister(TCPC_ADDRESS, 0x1eU, &Power, 1U);
      if (!EFI_ERROR(S)) S = ReadPowerRegister(TCPC_ADDRESS, 0x1dU, &Cc, 1U);
      if (!EFI_ERROR(S)) S = ReadPowerRegister(TCPC_ADDRESS, 0x1aU, &Role, 1U);
      if (!EFI_ERROR(S) && mHandoff.BootServicesLeaving) {
        BOOLEAN SourceValid = FALSE;
        if (mHandoff.ColdTypecBound) {
          SourceValid = ValidateColdTypecOnLease();
        } else if (mHandoff.RetainedTypecSource) {
          SourceValid = MtkMusbPlatformHusb311SourceAttached(Cc, Role) &&
            (Power & (MTK_MUSB_PLATFORM_HUSB311_TCPC_INITIALIZING |
                      MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED |
                      MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT)) ==
                     (MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED |
                      MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT) &&
            (((Cc & 3U) == 2U) ? 1U : 2U) == mHandoff.RouteCcPin;
        }
        if (!SourceValid) S = EFI_NOT_READY;
      }
    }
    Release = OwnLease ? ReleasePowerBus() : EFI_SUCCESS;
    if (EFI_ERROR(S)) return S;
    if (EFI_ERROR(Release)) return Release;
    E->Sc8989xBoostReadValid = 1U;
    E->Sc8989xBoostEnabled = (Sc & 0x20U) != 0U;
    E->Husb311PowerStatusReadValid = !mHandoff.IdLowBound;
    E->Husb311PowerStatus = Power;
    ZeroMem(&C, sizeof(C));
    C.CcStatus = Cc;
    C.RoleControl = Role;
    C.RoleControlWasRead = !mHandoff.IdLowBound;
  }
  E->Husb311CcStatusReadValid=C.RoleControlWasRead==1U;
  E->Husb311CcStatus=C.CcStatus; E->Husb311RoleControl=C.RoleControl;
  E->InterruptVector=MTK_MUSB_PLATFORM_GSIV; E->SupportedEventMask=MTK_MUSB_PLATFORM_EVENTS;
  if(mHandoff.ReadyInstalled) {
    MemoryFence(); E->HostPortSnapshotValid=mHandoff.Ready.HostSnapshotValid==1;
    E->HostPortStatus=mHandoff.Ready.HostPortStatus;
    E->HostPortChangeStatus=mHandoff.Ready.HostPortChangeStatus;
  }
  E->HostPowerPath=mHandoff.HostFirst?MTK_MUSB_PLATFORM_POWER_PATH_HOST_FIRST:mHandoff.IdLowBound?MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB:
    mHandoff.TypecOnly?MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY:MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE;
  E->IdLowRoleToken=mHandoff.IdLowBound?mHandoff.IdLowRoleToken:0U;
  return EFI_SUCCESS;
}
typedef struct {
  UINTN OperationCount;
  EFI_I2C_OPERATION Operation[2];
} OTG_I2C_REQUEST;

STATIC EFI_STATUS AcquirePowerBusEx (BOOLEAN *Contended) {
  MTK_I2C_EXCLUSIVE_PROTOCOL *P=NULL; EFI_STATUS S;
  if(Contended) *Contended=FALSE;
  if(mHandoff.I2cToken||!mHandoff.I2cHandle)return BusFailure(0x40U,EFI_ACCESS_DENIED);
  S=gBS->HandleProtocol(mHandoff.I2cHandle,&gMediaTekI2cExclusiveProtocolGuid,(VOID **)&P);
  if(EFI_ERROR(S)||!P||P->Revision!=MTK_I2C_EXCLUSIVE_PROTOCOL_REVISION||
     !P->Acquire||!P->Transfer||!P->Release)return BusFailure(0x41U,EFI_NOT_READY);
  if(mHandoff.I2cExclusive&&mHandoff.I2cExclusive!=P)return BusFailure(0x42U,EFI_ACCESS_DENIED);
  mHandoff.I2cExclusive=P;
  S=P->Acquire(P,&mHandoff.I2cToken);
  if(EFI_ERROR(S)) {
    if(Contended) *Contended=S==EFI_ALREADY_STARTED||S==EFI_ACCESS_DENIED;
    mHandoff.I2cToken=0;
    return BusFailure(0x43U,S);
  }
  return mHandoff.I2cToken?EFI_SUCCESS:BusFailure(0x44U,EFI_DEVICE_ERROR);
}
STATIC EFI_STATUS AcquirePowerBus (VOID) {
  return AcquirePowerBusEx(NULL);
}
STATIC EFI_STATUS ReleasePowerBus (VOID) {
  EFI_STATUS S;
  if(!mHandoff.I2cToken) return EFI_SUCCESS;
  S=mHandoff.I2cExclusive->Release(mHandoff.I2cExclusive,mHandoff.I2cToken);
  if(EFI_ERROR(S)){mHandoff.ImageMustRemainResident=TRUE;return BusFailure(0x45U,S);}
  mHandoff.I2cToken=0; return EFI_SUCCESS;
}
STATIC EFI_STATUS PowerTransfer (UINTN Address,OTG_I2C_REQUEST *Request) {
  EFI_STATUS S,Completion=EFI_NOT_READY;
  if(!mHandoff.I2cExclusive||!mHandoff.I2cToken||
     (Address!=SC_ADDRESS&&Address!=TCPC_ADDRESS)) return EFI_ACCESS_DENIED;
  S=mHandoff.I2cExclusive->Transfer(mHandoff.I2cExclusive,mHandoff.I2cToken,
                                  Address,(EFI_I2C_REQUEST_PACKET *)Request,&Completion);
  return EFI_ERROR(S)?S:Completion;
}
STATIC EFI_STATUS ReadPowerRegister (UINTN Address,UINT8 Register,UINT8 *Out,UINTN Bytes) {
  OTG_I2C_REQUEST R;
  if(!Out||!Bytes||Bytes>2U||(UINTN)Register+Bytes>256U) return EFI_INVALID_PARAMETER;
  R.OperationCount=2;
  R.Operation[0]=(EFI_I2C_OPERATION){0U,1U,&Register};
  R.Operation[1]=(EFI_I2C_OPERATION){I2C_FLAG_READ,(UINT32)Bytes,Out};
  EFI_STATUS S=PowerTransfer(Address,&R);
  if(!EFI_ERROR(S)&&Bytes==1U&&mHandoff.DeviceDisconnectInProgress&&
     mHandoff.DiagnosticDeviceStop==2U) {
    UINTN Slot=6U;
    if(Address==SC_ADDRESS)Slot=Register==0x14U?0U:Register==3U?1U:6U;
    if(Address==TCPC_ADDRESS)Slot=Register==0x1eU?2U:Register==0x1dU?3U:
                                  Register==0x1aU?4U:Register==0x97U?5U:6U;
    if(Slot<6U) {
      mHandoff.DiagnosticPower[Slot]=Out[0];
      mHandoff.DiagnosticPowerValid|=(UINT8)(1U<<Slot);
    }
  }
  return EFI_ERROR(S)?BusFailure((UINT16)(0x8000U|(Address<<8)|Register),S):S;
}

// Only this boot-services producer writes these TCPCI fields. No PD transmit,
// reset, discharge or command register is exposed by this adapter.
STATIC MTK_HUSB311_TYPEC_STATUS ColdTypecRead (
  VOID *Context, UINT8 Register, UINT8 Length, UINT8 *Out) {
  if (Context != &mHandoff || !mHandoff.ColdTypecBusy || !mHandoff.I2cToken ||
      !mHandoff.DeviceSettled || mHandoff.ExitingBootServices)
    return MtkHusb311TypecNotReady;
  return EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS, Register, Out, Length)) ?
    MtkHusb311TypecIoError : MtkHusb311TypecSuccess;
}
STATIC MTK_HUSB311_TYPEC_STATUS ColdTypecWrite (
  VOID *Context, UINT8 Register, UINT8 Value) {
  OTG_I2C_REQUEST Request;
  UINT8 Pair[2] = {Register, Value};
  if (Context != &mHandoff || !mHandoff.ColdTypecBusy || !mHandoff.I2cToken ||
      !mHandoff.DeviceSettled || mHandoff.ExitingBootServices ||
      !((Register == MTK_HUSB311_TYPEC_REG_VENDOR_ALERT &&
         Value == MTK_HUSB311_TYPEC_ALERT_VBUS_80_MASK) ||
        Register == 0x1aU || Register == 0x2eU || Register == 0x2fU))
    return MtkHusb311TypecNotReady;
  Request.OperationCount = 1U;
  Request.Operation[0] = (EFI_I2C_OPERATION){0U, 2U, Pair};
  return EFI_ERROR(PowerTransfer(TCPC_ADDRESS, &Request)) ?
    MtkHusb311TypecIoError : MtkHusb311TypecSuccess;
}
STATIC MTK_HUSB311_TYPEC_STATUS ColdTypecUpdate (
  VOID *Context,UINT8 Register,UINT8 Mask,UINT8 Value) {
  UINT8 Old;
  if(Register==MTK_HUSB311_TYPEC_REG_VENDOR_ALERT) {
    if(Mask!=MTK_HUSB311_TYPEC_ALERT_VBUS_80_MASK||Value!=Mask)
      return MtkHusb311TypecInvalid;
    // W1C must send only the requested event bit, even while source is active.
    return ColdTypecWrite(Context,Register,Value);
  }
  if(!Mask||(Value&~Mask)!=0U||
     (Register!=0x1aU&&Register!=0x2eU&&Register!=0x2fU))
    return MtkHusb311TypecInvalid;
  if(ColdTypecRead(Context,Register,1U,&Old)!=MtkHusb311TypecSuccess)
    return MtkHusb311TypecIoError;
  return ColdTypecWrite(Context,Register,(UINT8)((Old&~Mask)|(Value&Mask)));
}
STATIC MTK_HUSB311_TYPEC_STATUS ColdTypecDelay (VOID *Context,UINT32 Ms) {
  if(Context!=&mHandoff||!mHandoff.ColdTypecBusy||!mHandoff.I2cToken||
     mHandoff.ExitingBootServices||Ms>5000U) return MtkHusb311TypecNotReady;
  MicroSecondDelay((UINTN)Ms*1000U); return MtkHusb311TypecSuccess;
}
STATIC MTK_HUSB311_TYPEC_STATUS TypecGuardFailure (UINT8 Step,MTK_HUSB311_TYPEC_STATUS Status) {
  if(!mHandoff.ColdTypecCore.FirstFailure.Step&&!mHandoff.DiagnosticTypecGuardStep)
    mHandoff.DiagnosticTypecGuardStep=Step;
  return Status;
}
STATIC MTK_HUSB311_TYPEC_STATUS ColdTypecGuards (
  VOID *Context,MTK_HUSB311_TYPEC_GUARDS *G) {
  UINT8 Sc, Power, Vendor;
  if (Context != &mHandoff || !G || !mHandoff.ColdTypecBusy || !mHandoff.I2cToken ||
      !mHandoff.DeviceSettled || mHandoff.ExitingBootServices ||
      EFI_ERROR(ValidateI2cLive()))
    return TypecGuardFailure(1U, MtkHusb311TypecNotReady);
  ZeroMem(G,sizeof(*G));
  if(EFI_ERROR(ReadPowerRegister(SC_ADDRESS,3U,&Sc,1U)))
    return TypecGuardFailure(7U,MtkHusb311TypecIoError);
  if(EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1eU,&Power,1U)))
    return TypecGuardFailure(8U,MtkHusb311TypecIoError);
  if(EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x97U,&Vendor,1U)))
    return TypecGuardFailure(9U,MtkHusb311TypecIoError);
  G->PowerBusLeaseHeld=1U;
  G->TcpcInitialized=(Power&0x40U)==0U;
  G->CcDetectionEnabled=(Power&0x08U)!=0U;
  G->ScBoostEnabled=(Sc&0x20U)!=0U;
  G->HusbVbusSafe0V=SpHusb311VbusSafe0V(Power,Vendor)?1U:0U;
  // This board's pogo hub requests the MUSB host role independently.
  // CC changes do not read, stop, or reconfigure the host controller.
  G->IndependentControllerRole = 1U;
  return MtkHusb311TypecSuccess;
}
// Validate on the same held lease as the source/controller phase sample.
// CC.Rd and an old GoodCRC header alone never mint a data-role token.
STATIC BOOLEAN ValidateColdTypecOnLease (VOID) {
  MTK_HUSB311_TYPEC_STATUS Ts;
  if(!mHandoff.ColdTypecBound||!mHandoff.I2cToken||
     !MtkHusb311TypecTokenActive(&mHandoff.ColdTypecCore,mHandoff.ColdTypecToken))
    return FALSE;
  mHandoff.ColdTypecBusy=TRUE;
  Ts=MtkHusb311TypecValidateSource(&mHandoff.ColdTypecCore,mHandoff.ColdTypecToken);
  mHandoff.ColdTypecBusy=FALSE;
  return Ts==MtkHusb311TypecSuccess;
}
STATIC EFI_STATUS PrepareColdTypec (VOID) {
  MTK_HUSB311_TYPEC_CORE *Core = &mHandoff.ColdTypecCore;
  MTK_HUSB311_TYPEC_IO Io = {&mHandoff, ColdTypecRead, ColdTypecUpdate,
                            ColdTypecDelay, ColdTypecGuards};
  MTK_HUSB311_TYPEC_STATUS State;
  EFI_STATUS Status = AcquirePowerBus();
  if (EFI_ERROR(Status)) return Status;
  mHandoff.ColdTypecBusy = TRUE;
  if (mHandoff.ColdProbePending) {
    State = MtkHusb311TypecPollSourceProbe(Core, &mHandoff.ColdTypecToken);
  } else {
    State = Core->Signature ? MtkHusb311TypecSuccess : MtkHusb311TypecInitialize(Core, &Io);
    if (State == MtkHusb311TypecSuccess) {
      Core->FirstFailure = (MTK_HUSB311_TYPEC_DIAGNOSTIC){0};
      mHandoff.DiagnosticTypecGuardStep = 0U;
      mHandoff.ColdTypecBound = TRUE;
      State = MtkHusb311TypecBeginSourceProbe(Core);
    }
  }
  mHandoff.ColdTypecBusy = FALSE;
  Status = ReleasePowerBus();
  mHandoff.ColdProbePending = State == MtkHusb311TypecPending;
  if (EFI_ERROR(Status)) return Status;
  return State == MtkHusb311TypecSuccess ? EFI_SUCCESS : EFI_NOT_READY;
}
STATIC EFI_STATUS ReleaseColdTypec (VOID) {
  MTK_HUSB311_TYPEC_CORE *Core = &mHandoff.ColdTypecCore;
  MTK_HUSB311_TYPEC_STATUS State;
  EFI_STATUS Status;

  if (!mHandoff.ColdTypecBound) return EFI_SUCCESS;
  if (mHandoff.ExitingBootServices || mHandoff.ColdTypecBusy) return EFI_NOT_READY;
  Status = mHandoff.I2cToken ? EFI_SUCCESS : AcquirePowerBus();
  if (EFI_ERROR(Status)) return Status;
  mHandoff.ColdTypecBusy = TRUE;
  State = MtkHusb311TypecStepCleanup(Core, mHandoff.ColdTypecToken);
  mHandoff.ColdTypecBusy = FALSE;
  Status = ReleasePowerBus();
  if (EFI_ERROR(Status)) return Status;
  if (Core->State == MtkHusb311TypecStateIdle && Core->OwnedFields == 0U) {
    mHandoff.ColdTypecBound = FALSE;
    if (mHandoff.ColdProbePending) mHandoff.TypecRetryTicks = USB_RETRY_TICKS;
    mHandoff.ColdProbePending = FALSE;
    mHandoff.ColdTypecToken = 0U;
    return State == MtkHusb311TypecSuccess ? EFI_SUCCESS : EFI_DEVICE_ERROR;
  }
  return Core->ConflictActive ? EFI_ACCESS_DENIED : EFI_NOT_READY;
}

STATIC MTK_SC8989X_OTG_STATUS OtgFreshEvidence (
  VOID *Context,MTK_SC8989X_OTG_EVIDENCE *Out) {
  UINT8 Id[2];
  EFI_STATUS S;
  MTK_MUSB_PLATFORM_GPIO_SAMPLE G;
  if (Context != &mHandoff || !Out || !mHandoff.OtgBusy || !mHandoff.I2cToken ||
      !mHandoff.DeviceSettled || mHandoff.ExitingBootServices ||
      EFI_ERROR(ValidateI2cLive()) ||
      EFI_ERROR(ValidateGcd(PINCTRL_BASE, PINCTRL_BYTES))) return MtkSc8989xOtgNotReady;
  if (!mHandoff.OtgCleaning && !ValidateColdTypecOnLease()) return MtkSc8989xOtgNotReady;
  ZeroMem(Out,sizeof(*Out));
  if(EFI_ERROR(ReadPowerRegister(SC_ADDRESS,0x14U,&Out->ScPartIdRegister,1U))||
     EFI_ERROR(ReadPowerRegister(SC_ADDRESS,0x00U,&Out->ScReg00,1U))||
     EFI_ERROR(ReadPowerRegister(SC_ADDRESS,0x03U,&Out->ScReg03,1U))||
     EFI_ERROR(ReadPowerRegister(SC_ADDRESS,0x0aU,&Out->ScReg0A,1U)))
    return MtkSc8989xOtgIoError;
  S=ReadPowerRegister(TCPC_ADDRESS,0U,Id,2U);
  if(EFI_ERROR(S)) return MtkSc8989xOtgIoError;
  Out->Husb311VendorId=(UINT16)(Id[0]|((UINT16)Id[1]<<8));
  S=ReadPowerRegister(TCPC_ADDRESS,2U,Id,2U);
  if(EFI_ERROR(S)) return MtkSc8989xOtgIoError;
  Out->Husb311DeviceId=(UINT16)(Id[0]|((UINT16)Id[1]<<8));
  if(EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1aU,&Out->Husb311RoleControl,1U))||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1dU,&Out->Husb311CcStatus,1U))||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1eU,&Out->Husb311PowerStatus,1U))||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x97U,&Out->Husb311VendorStatusReg97,1U)))
    return MtkSc8989xOtgIoError;
  if(EFI_ERROR(ReadGpioSample(48U,&G))) return MtkSc8989xOtgIoError;
  Out->Gpio48=(MTK_SC8989X_OTG_GPIO_STATE){G.Mode,(UINT8)G.Direction,(UINT8)G.PhysicalInput};
  if(EFI_ERROR(ReadGpioSample(86U,&G))) return MtkSc8989xOtgIoError;
  Out->Gpio86=(MTK_SC8989X_OTG_GPIO_STATE){G.Mode,(UINT8)G.Direction,(UINT8)G.PhysicalInput};
  return MtkSc8989xOtgSuccess;
}
STATIC MTK_SC8989X_OTG_STATUS OtgUpdateRegister (
  VOID *Context,UINT8 Register,UINT8 Mask,UINT8 Value) {
  OTG_I2C_REQUEST R; UINT8 Pair[2],Old;
  BOOLEAN Profile=Register==MTK_SC8989X_OTG_REG_BOOST_LIMIT&&
    Mask==MTK_SC8989X_OTG_BOOST_PROFILE_MASK;
  if(Context!=&mHandoff||!mHandoff.OtgBusy||!mHandoff.I2cToken||
     mHandoff.ExitingBootServices||!Mask||
     ((Register!=0U||Mask!=0x80U)&&
      (Register!=3U||(Mask!=0x10U&&Mask!=0x20U))&&!Profile)||(Value&~Mask)!=0U)
    return MtkSc8989xOtgInvalid;
  if(Profile) {
    UINT8 Boost;
    if(EFI_ERROR(ReadPowerRegister(SC_ADDRESS,MTK_SC8989X_OTG_REG_BOOST_CONTROL,&Boost,1U)))
      return MtkSc8989xOtgIoError;
    if((Boost&MTK_SC8989X_OTG_REG03_BOOST_MASK)!=0U) return MtkSc8989xOtgNotReady;
  }
  if(EFI_ERROR(ReadPowerRegister(SC_ADDRESS,Register,&Old,1U))) return MtkSc8989xOtgIoError;
  if(Register==3U&&Mask==0x20U&&Value!=0U) {
    UINT8 Role,Cc,Power,VendorStatus;
    // The core sample precedes this SC RMW read. Recheck asynchronous
    // connector state after that read, immediately before the boost write.
    if(EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1aU,&Role,1U))||
       EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1dU,&Cc,1U))||
       EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1eU,&Power,1U))||
       EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x97U,&VendorStatus,1U)))
      return MtkSc8989xOtgIoError;
    if(!ValidateColdTypecOnLease()) return MtkSc8989xOtgNotReady;
    if(Role!=mHandoff.OtgCore.InitialHusb311RoleControl||
       Cc!=mHandoff.OtgCore.InitialHusb311CcStatus||
       !MtkMusbPlatformHusb311SourceAttached(Cc,Role)||
       (Power&0x48U)!=0x08U||!SpHusb311VbusSafe0V(Power,VendorStatus))
      return MtkSc8989xOtgNotReady;
  }
  Pair[0]=Register; Pair[1]=(UINT8)((Old&~Mask)|(Value&Mask));
  R.OperationCount=1; R.Operation[0]=(EFI_I2C_OPERATION){0U,2U,Pair};
  return EFI_ERROR(PowerTransfer(SC_ADDRESS,&R))?MtkSc8989xOtgIoError:MtkSc8989xOtgSuccess;
}
STATIC MTK_SC8989X_OTG_STATUS OtgWriteGpio (VOID *Context,UINT8 Pin,UINT8 Value) {
  MTK_MUSB_PLATFORM_GPIO_SAMPLE G;
  if(Context!=&mHandoff||!mHandoff.OtgBusy||!mHandoff.I2cToken||
     mHandoff.ExitingBootServices||
     (Pin!=48U&&Pin!=86U)||Value>1U||!mHandoff.Gpio||!mHandoff.Gpio->SetState||
     EFI_ERROR(ValidateGcd(PINCTRL_BASE,PINCTRL_BYTES))||
     EFI_ERROR(ReadGpioSample(Pin,&G))||G.Mode!=0||
     G.Direction!=MTK_MUSB_PLATFORM_GPIO_OUTPUT) return MtkSc8989xOtgNotReady;
  return EFI_ERROR(mHandoff.Gpio->SetState(Pin,Value!=0U))?
         MtkSc8989xOtgIoError:MtkSc8989xOtgSuccess;
}
STATIC EFI_STATUS ReleaseOwnedSourcePower (VOID) {
  MTK_SC8989X_OTG_STATUS State;
  EFI_STATUS Status;
  if (!mHandoff.OtgBound) return ReleasePowerBus();
  if (mHandoff.OtgBusy || mHandoff.ExitingBootServices) return EFI_NOT_READY;
  Status = mHandoff.I2cToken ? EFI_SUCCESS : AcquirePowerBus();
  if (EFI_ERROR(Status)) return Status;
  mHandoff.OtgBusy = mHandoff.OtgCleaning = TRUE;
  State = MtkSc8989xOtgIsQuarantined(&mHandoff.OtgCore) ?
    MtkSc8989xOtgRetryCleanup(&mHandoff.OtgCore) : MtkSc8989xOtgDisable(&mHandoff.OtgCore);
  mHandoff.OtgBusy = mHandoff.OtgCleaning = FALSE;
  Status = ReleasePowerBus();
  if (EFI_ERROR(Status)) return Status;
  if (State != MtkSc8989xOtgSuccess) return EFI_DEVICE_ERROR;
  mHandoff.OtgBound = FALSE;
  ZeroMem(&mHandoff.OtgCore, sizeof(mHandoff.OtgCore));
  return EFI_SUCCESS;
}

// Type-C diagnostic: SC03, POWER, VENDOR, GPIO48, GPIO86, stage, count, OTG state.
STATIC UINT8 mDiagPower[8];
STATIC UINT8 mDiagBind[4];     // first fail (index+1)|kind<<4, tries, bound late, last fail

STATIC EFI_STATUS PrepareSourcePower (VOID) {
  MTK_SC8989X_OTG_IO Io = {&mHandoff, OtgFreshEvidence, OtgUpdateRegister, OtgWriteGpio};
  MTK_SC8989X_OTG_STATUS State;
  mHandoff.OtgBusy = TRUE;
  State = MtkSc8989xOtgInitialize(&mHandoff.OtgCore, &Io);
  if (State == MtkSc8989xOtgSuccess) State = MtkSc8989xOtgEnableCold(&mHandoff.OtgCore);
  mDiagPower[7] = (UINT8)State;
  mHandoff.OtgBound = State == MtkSc8989xOtgSuccess ||
    mHandoff.OtgCore.OwnedMask != 0U || MtkSc8989xOtgIsQuarantined(&mHandoff.OtgCore);
  if (!mHandoff.OtgBound) ZeroMem(&mHandoff.OtgCore, sizeof(mHandoff.OtgCore));
  mHandoff.OtgBusy = FALSE;
  return State == MtkSc8989xOtgSuccess ? EFI_SUCCESS : EFI_NOT_READY;
}
STATIC BOOLEAN ReadyResultComplete (CONST MTK_MUSB_PLATFORM_READY_RESULT *R) {
  if(!R||R->MissingFlags) return FALSE;
  if(R->HostPowerPath==MTK_MUSB_PLATFORM_POWER_PATH_HOST_FIRST)
    return R->Flags==MTK_MUSB_PLATFORM_READY_HOST_FIRST_REQUIRED;
  if(R->HostPowerPath==MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE||
     R->HostPowerPath==MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY)
    return R->Flags==MTK_MUSB_PLATFORM_READY_REQUIRED;
  return R->HostPowerPath==MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB&&
         R->Flags==MTK_MUSB_PLATFORM_READY_ID_LOW_REQUIRED;
}
STATIC EFI_STATUS ReleaseOwnedPhy (VOID) {
  MTK_USB_PHY_STATUS Ps;
  if(!mHandoff.PhyBound) return EFI_SUCCESS;
  if(mHandoff.PhyCore.Signature!=0) {
    if(MtkUsbPhyCoreIsPowered(&mHandoff.PhyCore)||mHandoff.PhyCore.PowerCleanupPending) {
      Ps=MtkUsbPhyCorePowerOff(&mHandoff.PhyCore);
      if(Ps!=MtkUsbPhySuccess) return EFI_DEVICE_ERROR;
    }
    Ps=MtkUsbPhyCoreExit(&mHandoff.PhyCore);
    if(Ps!=MtkUsbPhySuccess) return EFI_DEVICE_ERROR;
  }
  if(EFI_ERROR(MtkUsbPhyPlatformUnbind())) return EFI_DEVICE_ERROR;
  mHandoff.PhyBound=FALSE;
  ZeroMem(&mHandoff.ColdPhySession,sizeof(mHandoff.ColdPhySession));
  return EFI_SUCCESS;
}

STATIC int ColdControllerTimeUs (VOID *Context, UINT64 *Us) {
  if (Context != &mHandoff) return 0;
  *Us = GetTimeInNanoSecond(GetPerformanceCounter()) / 1000U;
  return 1;
}
STATIC int ColdSetPhyMode (VOID *Context,UINT32 Mode) {
  CONST VOID *F; UINTN Bytes; MTK_USB_PHY_STATUS Ps;
  if(Context!=&mHandoff||!mHandoff.ColdInProgress||!mHandoff.PhyBound||
     mHandoff.ExitingBootServices||mHandoff.ReadyInstalled||
     EFI_ERROR(GetRuntimeFdt(&F,&Bytes))||EFI_ERROR(ColdClockSnapshotMatches(F))||
     MtkMusbPlatformCheckQuiescent(&mHandoff.Io,0U)!=MtkMusbPlatformSuccess)
    { mHandoff.ColdPhyStatus=0xfeU; return 0; }
  if(Mode==MTK_MUSB_PLATFORM_PHY_USB_MODE_INVALID)
    Ps=MtkUsbPhyCoreSetMusbSession(&mHandoff.PhyCore,&mHandoff.ColdPhySession,MtkUsbPhyMusbInvalid);
  else if(Mode==MTK_MUSB_PLATFORM_PHY_USB_MODE_HOST) {
    Ps=MtkUsbPhyCoreSetMusbSession(&mHandoff.PhyCore,&mHandoff.ColdPhySession,MtkUsbPhyMusbAcquire);
    if(Ps==MtkUsbPhySuccess)
      Ps=MtkUsbPhyCoreSetUsbMode(&mHandoff.PhyCore,MtkUsbPhyModeHost,&mHandoff.ColdPhyTuning);
    if(Ps==MtkUsbPhySuccess)
      Ps=MtkUsbPhyCoreSetMusbSession(&mHandoff.PhyCore,&mHandoff.ColdPhySession,MtkUsbPhyMusbHost);
  } else if(Mode==MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG) {
    Ps=mHandoff.ColdPhySession.Owned?
      MtkUsbPhyCoreSetMusbSession(&mHandoff.PhyCore,&mHandoff.ColdPhySession,MtkUsbPhyMusbRestore):
      MtkUsbPhyCoreSetUsbMode(&mHandoff.PhyCore,MtkUsbPhyModeOtg,NULL);
  }
  else { mHandoff.ColdPhyStatus=0xfdU; return 0; }
  mHandoff.ColdPhyStatus=(UINT8)Ps;
  if(Ps!=MtkUsbPhySuccess) return 0;
  mHandoff.ColdPhyUsbMode=Mode; return 1;
}
STATIC int ColdGuardFailure (UINT8 Step,EFI_STATUS Status) {
  mHandoff.ColdGuardStep=Step;mHandoff.ColdGuardStatus=Status;return 0;
}
STATIC int ColdControllerGuard (
  VOID *Context,UINT32 Phase,MTK_MUSB_PLATFORM_COLD_EVIDENCE *Out) {
  CONST VOID *F; UINTN Bytes; EFI_STATUS S,Released;
  BOOLEAN Stop=Phase==MtkMusbPlatformColdBeforeStop||Phase==MtkMusbPlatformColdAfterStop;
  BOOLEAN Powered=MtkUsbPhyCoreIsPowered(&mHandoff.PhyCore)?TRUE:FALSE;
  BOOLEAN PhyFault=mHandoff.PhyCore.State==MTK_USB_TPHY_STATE_FAULTED&&
                   mHandoff.PhyCore.PowerCleanupPending==1U;
  if(Context!=&mHandoff||!Out||!mHandoff.ColdBound||!mHandoff.ColdInProgress||
     !mHandoff.ColdOwnerToken||mHandoff.ExitingBootServices||mHandoff.ReadyInstalled||
     !mHandoff.PhyBound||(!Powered&&(!Stop||!PhyFault))||
     mHandoff.ColdSampleSequence==MAX_UINT64) return ColdGuardFailure(1U,EFI_NOT_READY);
  S=GetRuntimeFdt(&F,&Bytes);if(EFI_ERROR(S))return ColdGuardFailure(2U,S);
  S=ValidateBoardTree(F,Bytes);if(EFI_ERROR(S))return ColdGuardFailure(3U,S);
  S=ColdClockSnapshotMatches(F);if(EFI_ERROR(S))return ColdGuardFailure(4U,S);
  ZeroMem(Out,sizeof(*Out));
  if(Stop) {
    MTK_MUSB_PLATFORM_EVIDENCE *E=&Out->Platform;
    if(EFI_ERROR(ValidateGcd(MTK_MUSB_PLATFORM_CONTROLLER_BASE,MTK_MUSB_PLATFORM_CONTROLLER_BYTES))||
       EFI_ERROR(ValidateGcd(MTK_MUSB_PLATFORM_PHY_BASE,MTK_MUSB_PLATFORM_PHY_BYTES))||
       EFI_ERROR(ReadClockEvidence(F,E))) return ColdGuardFailure(6U,EFI_NOT_READY);
    E->ControllerBase=MTK_MUSB_PLATFORM_CONTROLLER_BASE; E->ControllerLength=MTK_MUSB_PLATFORM_CONTROLLER_BYTES;
    E->PhyBase=MTK_MUSB_PLATFORM_PHY_BASE; E->PhyLength=MTK_MUSB_PLATFORM_PHY_BYTES;
    E->ControllerMapped=E->PhyMapped=1U; E->PhyReadMask=0x0fU;
    E->PhyAcr1=MmioRead32((UINTN)MTK_MUSB_PLATFORM_PHY_BASE+PHY_ACR1);
    E->PhyAcr6=MmioRead32((UINTN)MTK_MUSB_PLATFORM_PHY_BASE+PHY_ACR6);
    E->PhyDtm0=MmioRead32((UINTN)MTK_MUSB_PLATFORM_PHY_BASE+PHY_DTM0);
    E->PhyDtm1=MmioRead32((UINTN)MTK_MUSB_PLATFORM_PHY_BASE+PHY_DTM1);
  } else if(mHandoff.HostFirst) {
    S=CollectEvidence(FALSE,&Out->Platform);
    if(EFI_ERROR(S))return ColdGuardFailure(9U,S);
    Out->UsbDataRole=MTK_MUSB_PLATFORM_USB_DATA_ROLE_DFP;
    Out->DataRoleToken=mHandoff.ColdOwnerToken;
  } else {
    S=AcquirePowerBus(); if(EFI_ERROR(S)) return ColdGuardFailure(7U,S);
    UINT8 Step=8U;
    S=(mHandoff.IdLowBound||ValidateColdTypecOnLease())?EFI_SUCCESS:EFI_NOT_READY;
    if(!EFI_ERROR(S)) { Step=9U;S=CollectEvidence(FALSE,&Out->Platform); }
    Released=ReleasePowerBus();
    if(EFI_ERROR(S))return ColdGuardFailure(Step,S);
    if(EFI_ERROR(Released))return ColdGuardFailure(12U,Released);
    Out->UsbDataRole=mHandoff.IdLowBound?MTK_MUSB_PLATFORM_USB_DATA_ROLE_HOST_ID_LOW:
                                        MTK_MUSB_PLATFORM_USB_DATA_ROLE_DFP;
    Out->DataRoleToken=mHandoff.IdLowBound?mHandoff.IdLowRoleToken:
      (mHandoff.RetainedTypecSource?mHandoff.ColdOwnerToken:mHandoff.ColdTypecToken);
  }
  Out->Platform.PhyState=PhyFault?MTK_MUSB_PLATFORM_PHY_STATE_FAULTED:
                                  MTK_MUSB_PLATFORM_PHY_POWERED;
  Out->Platform.PhyPowerCleanupPending=PhyFault?1U:0U;
  Out->ExclusiveOwnerToken=mHandoff.ColdOwnerToken;
  Out->SampleSequence=++mHandoff.ColdSampleSequence;
  Out->PhyUsbMode=PhyFault?MTK_MUSB_PLATFORM_PHY_USB_MODE_UNKNOWN:mHandoff.ColdPhyUsbMode;
  mHandoff.ColdGuardStep=0U;mHandoff.ColdGuardStatus=EFI_SUCCESS;
  return 1;
}
STATIC MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS ColdControllerCallbacks (VOID) {
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS C={&mHandoff,ColdControllerGuard,
                                          ColdSetPhyMode,ColdControllerTimeUs};
  return C;
}
// Preserve the first controller failure before Stop/PowerOff changes its
// readbacks. Render only this RAM snapshot, never clock-gated hardware.
STATIC VOID CaptureColdHostFailure (MTK_MUSB_PLATFORM_STATUS Status) {
  COLD_HOST_FAILURE *D=&mHandoff.DiagnosticColdHost;
  CONST VOID *F;UINTN Bytes;MTK_MUSB_PLATFORM_EVIDENCE E={0};
  if(D->Valid)return;
  D->State=mHandoff.ColdHost;D->State.LastStatus=(UINT32)Status;
  D->BoardStep=mHandoff.ColdGuardStep;D->BoardStatus=mHandoff.ColdGuardStatus;
  D->PhyStatus=mHandoff.ColdPhyStatus;
  if(!mHandoff.ReadyInstalled&&!mHandoff.ExitingBootServices&&mHandoff.ColdClockBound&&
     !EFI_ERROR(GetRuntimeFdt(&F,&Bytes))&&!EFI_ERROR(ValidateBoardTree(F,Bytes))&&
     !EFI_ERROR(ReadClockEvidence(F,&E))&&
     !EFI_ERROR(ValidateGcd(MTK_MUSB_PLATFORM_CONTROLLER_BASE,MTK_MUSB_PLATFORM_CONTROLLER_BYTES))&&
     !EFI_ERROR(ValidateGcd(MTK_MUSB_PLATFORM_PHY_BASE,MTK_MUSB_PLATFORM_PHY_BYTES))) {
    if(EFI_ERROR(EfiAcquireLockOrFail(&mHandoff.IoLock))) { D->Valid=TRUE;return; }
    D->Devctl=MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x60U);
    D->Power=MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+1U);
    D->Phy[0]=MmioRead32((UINTN)MTK_MUSB_PLATFORM_PHY_BASE+PHY_ACR1);
    D->Phy[1]=MmioRead32((UINTN)MTK_MUSB_PLATFORM_PHY_BASE+PHY_ACR6);
    D->Phy[2]=MmioRead32((UINTN)MTK_MUSB_PLATFORM_PHY_BASE+PHY_DTM0);
    D->Phy[3]=MmioRead32((UINTN)MTK_MUSB_PLATFORM_PHY_BASE+PHY_DTM1);
    D->ReadMask=0x3fU;
    EfiReleaseLock(&mHandoff.IoLock);
  }
  D->Valid=TRUE;
}
STATIC EFI_STATUS StartColdController (VOID) {
  MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS C=ColdControllerCallbacks();
  MTK_MUSB_PLATFORM_STATUS Ms;
  mHandoff.ColdGuardStep=0U;mHandoff.ColdGuardStatus=EFI_SUCCESS;
  mHandoff.ColdPhyStatus=0xffU; // Host mode callback not attempted.
  mHandoff.AllowColdWrites = mHandoff.AllowAckWrites = TRUE;
  Ms = MtkMusbPlatformAdvanceColdHost(&mHandoff.Io, &C, &mHandoff.ColdHost);
  mHandoff.AllowColdWrites = mHandoff.AllowAckWrites = FALSE;
  if (Ms == MtkMusbPlatformPending) return EFI_NOT_READY;
  if (Ms == MtkMusbPlatformSuccess) return EFI_SUCCESS;
  CaptureColdHostFailure(Ms);
  if(Ms==MtkMusbPlatformIoError)return EFI_DEVICE_ERROR;
  if(Ms==MtkMusbPlatformTimeout)return EFI_TIMEOUT;
  if(Ms==MtkMusbPlatformConflict)return EFI_ACCESS_DENIED;
  if(Ms==MtkMusbPlatformUnsupported)return EFI_UNSUPPORTED;
  return EFI_NOT_READY;
}
STATIC EFI_STATUS ReleaseColdController (VOID) {
  MTK_MUSB_PLATFORM_STATUS Ms; MTK_MUSB_PLATFORM_COLD_HOST_CALLBACKS C;
  if(mHandoff.ColdHost.Signature==0U) return EFI_SUCCESS;
  if(!mHandoff.ColdInProgress||mHandoff.ReadyInstalled||mHandoff.ExitingBootServices)
    return EFI_NOT_READY;
  C=ColdControllerCallbacks();
  mHandoff.AllowColdWrites=mHandoff.AllowAckWrites=TRUE;
  Ms=MtkMusbPlatformStopColdHost(&mHandoff.Io,&C,&mHandoff.ColdHost);
  mHandoff.AllowColdWrites=mHandoff.AllowAckWrites=FALSE;
  return Ms==MtkMusbPlatformSuccess?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC BOOLEAN Husb311SinkAttached (UINT8 Cc,UINT8 Role,UINT8 Power) {
  UINT8 Cc1=Cc&3U,Cc2=(Cc>>2)&3U,Pair=Role&0x0fU;
  if((Power&0x4cU)!=0x0cU||(Cc&0xe0U)||(Cc&0x10U)==0U||
     (Role&0x80U)||(Role&0x30U)==0x30U) return FALSE;
  if((Role&0x40U)?(Pair!=0x05U&&Pair!=0x0aU):(Pair!=0x0aU)) return FALSE;
  return (Cc1!=0U&&Cc2==0U)||(Cc2!=0U&&Cc1==0U);
}
STATIC BOOLEAN Husb311OpenCcPogo (UINT8 Cc,UINT8 Role) {
  // Both CC pins explicitly open, no DRP/toggling, no reserved Rp value.
  // Open CC is not a Type-C sink attachment and does not prove vSafe0V.
  return (Cc&0xefU)==0U&&(Role&0xcfU)==0x0fU&&(Role&0x30U)!=0x30U;
}
// Shipped HUSB311 wake sets IDLE_CTRL=38 and the vSafe0V alert mask.
// POWER_STATUS[3] reflects 0x90[1] (Hynetek datasheet p17). Only own
// that sensing bit: oscillator bit0 changes automatically on an interrupt.
// This does not source/discharge VBUS, change CC, or reset the chip.
STATIC CONST UINT8 SenseRegister[]={0x9bU,0x99U,0x90U};
STATIC CONST UINT8 SenseMask[]={0xffU,0x02U,0x02U};
STATIC EFI_STATUS WriteHusb311Sense (UINTN Index,UINT8 Value,BOOLEAN Restore) {
  OTG_I2C_REQUEST R;UINT8 Pair[2];
  if(Index>=ARRAY_SIZE(SenseRegister)||!mHandoff.I2cToken||
     mHandoff.ExitingBootServices||mHandoff.ColdTypecBound||mHandoff.OtgBound||
     (!Restore&&!mHandoff.DeviceDisconnectInProgress&&!mHandoff.DeviceSettled)||
     !(mHandoff.SenseOwnedMask&(1U<<Index)))return EFI_ACCESS_DENIED;
  if(Restore&&(Value&SenseMask[Index])!=(mHandoff.SenseSaved[Index]&SenseMask[Index]))
    return EFI_ACCESS_DENIED;
  Pair[0]=SenseRegister[Index];Pair[1]=Value;
  R.OperationCount=1U;R.Operation[0]=(EFI_I2C_OPERATION){0U,2U,Pair};
  return PowerTransfer(TCPC_ADDRESS,&R);
}
// Wake: 1 snapshot, 2 write, 3 verify read, 4 mismatch, 5 power read, 6 timeout, 7 RMW read.
// Cleanup: 1 read, 2 foreign value, 3 write, 4 verify read, 5 mismatch, 6/7 lease.
// Keep both first failures intact when cleanup is retried by the timer.
STATIC EFI_STATUS SenseFailure (BOOLEAN Cleanup,UINT8 Stage,UINT8 Register,
                                UINT8 Saved,UINT8 Expected,UINT8 Live,EFI_STATUS S) {
  HUSB_SENSE_FAILURE *D=Cleanup?&mHandoff.DiagnosticSenseCleanup:&mHandoff.DiagnosticSenseFailure;
  if(!D->Stage) {
    BusFailure(Cleanup?0x50dU:Stage==6U?0x50cU:0x50bU,S);
    *D=(HUSB_SENSE_FAILURE){Stage,Register,Saved,Expected,Live,S};
  }
  return S;
}
STATIC EFI_STATUS SenseRegisterFailure (BOOLEAN Cleanup,UINT8 Stage,UINTN Index,
                                        UINT8 Live,EFI_STATUS S) {
  return SenseFailure(Cleanup,Stage,SenseRegister[Index],mHandoff.SenseSaved[Index],
                      mHandoff.SenseExpected[Index],Live,S);
}
STATIC EFI_STATUS RestoreHusb311SenseOnLease (VOID) {
  EFI_STATUS S;UINT8 Live,Mask,Before,Expected,Value;
  if(!mHandoff.I2cToken||mHandoff.ExitingBootServices)
    return EFI_NOT_READY;
  for(UINTN N=ARRAY_SIZE(SenseRegister);N>0U;--N) {
    UINTN I=N-1U;if(!(mHandoff.SenseOwnedMask&(1U<<I)))continue;
    Mask=SenseMask[I];Before=mHandoff.SenseSaved[I]&Mask;
    Expected=mHandoff.SenseExpected[I]&Mask;
    S=ReadPowerRegister(TCPC_ADDRESS,SenseRegister[I],&Live,1U);
    if(EFI_ERROR(S))return SenseRegisterFailure(TRUE,1U,I,0xffU,S);
    // A failed transfer may have reached hardware or may have done nothing.
    // Accept those two states only; do not overwrite a foreign update.
    if((Live&Mask)!=Before) {
      if((Live&Mask)!=Expected)return SenseRegisterFailure(TRUE,2U,I,Live,EFI_ACCESS_DENIED);
      Value=(UINT8)((Live&~Mask)|Before);
      S=WriteHusb311Sense(I,Value,TRUE);
      if(EFI_ERROR(S))return SenseRegisterFailure(TRUE,3U,I,Live,S);
      S=ReadPowerRegister(TCPC_ADDRESS,SenseRegister[I],&Live,1U);
      if(EFI_ERROR(S))return SenseRegisterFailure(TRUE,4U,I,0xffU,S);
      if((Live&Mask)!=Before)return SenseRegisterFailure(TRUE,5U,I,Live,EFI_DEVICE_ERROR);
    }
    mHandoff.SenseOwnedMask&=(UINT8)~(1U<<I);
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS SnapshotHusb311SenseOnLease (VOID) {
  for (UINTN Index = 0; Index < ARRAY_SIZE(SenseRegister); ++Index) {
    EFI_STATUS Status = ReadPowerRegister(
      TCPC_ADDRESS, SenseRegister[Index], &mHandoff.SenseSaved[Index], 1U);
    if (EFI_ERROR(Status))
      return SenseFailure(FALSE, 1U, SenseRegister[Index], 0xffU, 0xffU, 0xffU, Status);
    mHandoff.SenseExpected[Index] = Index == 0U ? 0x38U :
      (UINT8)(mHandoff.SenseSaved[Index] | SenseMask[Index]);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EnableHusb311SenseRegister (UINTN Index, BOOLEAN ForceWrite) {
  UINT8 Mask = SenseMask[Index];
  UINT8 Value = mHandoff.SenseExpected[Index];
  UINT8 Live = mHandoff.SenseSaved[Index];
  EFI_STATUS Status;

  if (Mask != 0xffU) {
    // Keep unrelated bits, including the oscillator bit that can change on an IRQ.
    Status = ReadPowerRegister(TCPC_ADDRESS, SenseRegister[Index], &Live, 1U);
    if (EFI_ERROR(Status)) return SenseRegisterFailure(FALSE, 7U, Index, 0xffU, Status);
    Value = (UINT8)((Live & ~Mask) | (Value & Mask));
  }
  mHandoff.SenseExpected[Index] = Value;
  if ((Live & Mask) == (Value & Mask) && !ForceWrite) return EFI_SUCCESS;

  // A failed I2C transfer can still have reached the chip.
  mHandoff.SenseOwnedMask |= (UINT8)(1U << Index);
  Status = WriteHusb311Sense(Index, Value, FALSE);
  if (EFI_ERROR(Status)) return SenseRegisterFailure(FALSE, 2U, Index, 0xffU, Status);
  Status = ReadPowerRegister(TCPC_ADDRESS, SenseRegister[Index], &Live, 1U);
  if (EFI_ERROR(Status)) return SenseRegisterFailure(FALSE, 3U, Index, 0xffU, Status);
  if ((Live & Mask) != (Value & Mask))
    return SenseRegisterFailure(FALSE, 4U, Index, Live, EFI_DEVICE_ERROR);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EnablePassiveVbusSenseOnLease (BOOLEAN ForceIdleWrite) {
  EFI_STATUS Status = EFI_SUCCESS;

  for (UINTN Index = 0; Index < ARRAY_SIZE(SenseRegister) && !EFI_ERROR(Status); ++Index)
    Status = EnableHusb311SenseRegister(Index, Index == 0U && ForceIdleWrite);
  if (!EFI_ERROR(Status)) mHandoff.TypecSenseTicks = USB_SENSE_WAIT_TICKS;
  return Status;
}

// Passive sensing is available before the inherited controller is retired.
STATIC EFI_STATUS BeginPassiveVbusSenseOnLease (VOID) {
  EFI_STATUS Status = SnapshotHusb311SenseOnLease();

  if (!EFI_ERROR(Status)) Status = EnablePassiveVbusSenseOnLease(FALSE);
  if (EFI_ERROR(Status) && mHandoff.SenseOwnedMask == 0U)
    mHandoff.TypecRetryTicks = USB_RETRY_TICKS;
  return Status;
}

STATIC EFI_STATUS PollPassiveVbusSense (UINT8 Power) {
  if ((Power & 0x4aU) == 0x08U) {
    mHandoff.TypecSenseTicks = 0U;
    mHandoff.SenseOwnedMask = 0U;
    return EFI_SUCCESS;
  }
  if (--mHandoff.TypecSenseTicks == 0U) {
    (VOID)SenseFailure(FALSE, 6U, 0x1eU, 0U, 0x08U, Power, EFI_NOT_READY);
    if (mHandoff.SenseOwnedMask == 0U) mHandoff.TypecRetryTicks = USB_RETRY_TICKS;
  }
  return EFI_NOT_READY;
}

STATIC EFI_STATUS CheckIdleDeviceOnLease (UINT8 Power,UINT8 Cc,UINT8 Role) {
  MTK_MUSB_PLATFORM_STATUS Idle;
  UINT8 Sc,LivePower,LiveCc,LiveRole,Control,Fault,Vendor;
  if(!mHandoff.DeviceDisconnectInProgress||!mHandoff.EndOfDxeSeen||
     !mHandoff.I2cToken||mHandoff.ReadyInstalled||mHandoff.ExitingBootServices||
     mHandoff.ColdBound||mHandoff.PreparationFailed||
     (Power==2U&&!Husb311OpenCcPogo(Cc,Role)))return EFI_NOT_READY;
  if(!DeviceRequestMatches()||
     EFI_ERROR(ReadPowerRegister(SC_ADDRESS,3U,&Sc,1U))||(Sc&0x20U)||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1eU,&LivePower,1U))||LivePower!=Power||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1dU,&LiveCc,1U))||LiveCc!=Cc||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1aU,&LiveRole,1U))||LiveRole!=Role||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x19U,&Control,1U))||(Control&2U)||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1cU,&Control,1U))||
     (Control&1U)!=(Power==2U?1U:0U)||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1fU,&Fault,1U))||Fault||
     !DeviceRequestMatches()||!mHandoff.I2cToken||mHandoff.ReadyInstalled||
     mHandoff.ExitingBootServices)
    return BusFailure(0x515U,EFI_NOT_READY);
  if(mHandoff.DeviceId!=0U&&(Power&8U)&&
     (EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x97U,&Vendor,1U))||
      !SpHusb311VbusSafe0V(Power,Vendor)))
    return BusFailure(0x505U,EFI_NOT_READY);
  Idle=MtkMusbPlatformCheckDeviceForDisconnect(&mHandoff.Io,&mHandoff.DiagnosticIdle);
  if(Idle!=MtkMusbPlatformSuccess)
    return BusFailure(Idle==MtkMusbPlatformQuiescenceFailed?0x511U:0x514U,EFI_NOT_READY);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS RetireIdleDeviceOnLease (UINT8 Power,UINT8 Cc,UINT8 Role) {
  MTK_MUSB_PLATFORM_STATUS Idle;EFI_STATUS S;
  if(mHandoff.DeviceId!=0U&&(Power&0x0cU)!=0x08U)
    return BusFailure(0x504U,EFI_NOT_READY);
  S=CheckIdleDeviceOnLease(Power,Cc,Role);if(EFI_ERROR(S))return S;
  mHandoff.DeviceRetiring=TRUE;
  Idle=MtkMusbPlatformRetireDeviceForDisconnect(&mHandoff.Io,&mHandoff.DiagnosticIdle);
  mHandoff.DeviceRetiring=FALSE;
  if(Idle!=MtkMusbPlatformSuccess) {
    // Never re-enable LK's masks or retry a partially applied retirement.
    if(mHandoff.DeviceRetirementAttempted)mHandoff.PreparationFailed=TRUE;
    return BusFailure(0x516U,EFI_NOT_READY);
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS DisableHusb311VconnOnLease (UINT8 *Control) {
  OTG_I2C_REQUEST Request;
  UINT8 Pair[2] = {0x1cU, 0U};
  EFI_STATUS Status = ReadPowerRegister(TCPC_ADDRESS, Pair[0], Control, 1U);

  if (EFI_ERROR(Status))
    return SenseFailure(FALSE, 7U, Pair[0], 0xffU, 0U, 0xffU, Status);
  if ((*Control & 1U) == 0U)
    return SenseFailure(FALSE, 4U, Pair[0], *Control, 1U, *Control, EFI_NOT_READY);
  // Follow stock set_vconn(false): clear EN_VCONN, then write IDLE_CTRL
  // before reading back either register. Keep VCONN off on cleanup.
  Pair[1] = *Control & ~1U;
  Request.OperationCount = 1U;
  Request.Operation[0] = (EFI_I2C_OPERATION){0U, sizeof(Pair), Pair};
  Status = PowerTransfer(TCPC_ADDRESS, &Request);
  return EFI_ERROR(Status) ?
    SenseFailure(FALSE, 2U, Pair[0], *Control, Pair[1], 0xffU, Status) : Status;
}

STATIC EFI_STATUS WaitForHusb311VconnOff (UINT8 Control) {
  EFI_STATUS S;
  UINT8 Live,Power,Expected=Control&~1U;
  for(UINTN N=0U;;++N) {
    S=ReadPowerRegister(TCPC_ADDRESS,0x1cU,&Live,1U);
    if(EFI_ERROR(S))return SenseFailure(FALSE,3U,0x1cU,Control,Expected,0xffU,S);
    if(Live!=Expected&&Live!=Control)
      return SenseFailure(FALSE,4U,0x1cU,Control,Expected,Live,EFI_DEVICE_ERROR);
    S=ReadPowerRegister(TCPC_ADDRESS,0x1eU,&Power,1U);
    if(EFI_ERROR(S))return SenseFailure(FALSE,5U,0x1eU,2U,0U,0xffU,S);
    if(Power!=0U&&Power!=2U)
      return SenseFailure(FALSE,4U,0x1eU,2U,0U,Power,EFI_NOT_READY);
    if(Live==Expected&&Power==0U)return EFI_SUCCESS;
    if(N==20U) {
      if(Live!=Expected)return SenseFailure(FALSE,6U,0x1cU,Control,Expected,Live,EFI_NOT_READY);
      return SenseFailure(FALSE,6U,0x1eU,2U,0U,Power,EFI_NOT_READY);
    }
    MicroSecondDelay(1000U);
  }
}
STATIC EFI_STATUS WakeHusb311SenseOnLease (UINT8 Power,UINT8 Cc,UINT8 Role) {
  EFI_STATUS S=EFI_NOT_READY;
  UINT8 Control,Fault,Live;UINT16 Csr;UINT8 Saved;
  MTK_MUSB_PLATFORM_STATUS Idle;
  mHandoff.DiagnosticIdle=(MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC){0xffffffffU,0U,0U,0xffU};
  if(!mHandoff.DeviceDisconnectInProgress||!mHandoff.I2cToken||
     mHandoff.SenseAttempted||mHandoff.ColdBound||
     mHandoff.ReadyInstalled||mHandoff.ExitingBootServices||
     (Power&~2U)!=0U||(Cc&0xefU)!=0U||(Role&0xcfU)!=0x0fU||
     (Role&0x30U)==0x30U||
     !DeviceRequestMatches())
    return BusFailure(0x509U,EFI_NOT_READY);
  Live=MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+1U);
  if(Live&0x0eU) {
    mHandoff.DiagnosticIdle=(MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC){1U,Live,0x0eU,0xffU};
    return BusFailure(0x512U,EFI_NOT_READY);
  }
  Saved=MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x0eU);
  MmioWrite8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x0eU,0U);
  Csr=MmioRead16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x12U);
  MmioWrite8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x0eU,Saved);
  if(Csr&0x10U) {
    mHandoff.DiagnosticIdle=(MTK_MUSB_PLATFORM_IDLE_DIAGNOSTIC){0x12U,Csr,0x10U,0U};
    return BusFailure(0x513U,EFI_NOT_READY);
  }
  if(EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x19U,&Control,1U))||(Control&2U))
    return BusFailure(0x50aU,EFI_NOT_READY);
  S=ReadPowerRegister(TCPC_ADDRESS,0x1cU,&Control,1U);
  if(EFI_ERROR(S))return SenseFailure(FALSE,1U,0x1cU,0xffU,Power>>1U,0xffU,S);
  if((Control&1U)!=(Power>>1U))
    return SenseFailure(FALSE,4U,0x1cU,Control,(Control&0xfeU)|(Power>>1U),Control,EFI_NOT_READY);
  if(EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1fU,&Fault,1U))||Fault)
    return BusFailure(0x50aU,EFI_NOT_READY);
  // On the Type-C-only route, waking the detector can reveal an external
  // source. Leave LK's USB transport intact until the awake detector proves
  // vSafe0V on the retirement lease.
  S=mHandoff.DeviceId!=0U?CheckIdleDeviceOnLease(Power,Cc,Role):
                        RetireIdleDeviceOnLease(Power,Cc,Role);
  if(EFI_ERROR(S))return S;
  // DATAEND is an incomplete status stage; defer its idle/ACK gate until
  // SOFTCONN and SESSION are down and the controller reset has completed.
  if(mHandoff.DeviceId==0U&&(Csr&8U)==0U) {
    mHandoff.AllowAckWrites=TRUE;
    Idle=MtkMusbPlatformAcknowledgeIdleInterruptsWithDiagnostic(&mHandoff.Io,&mHandoff.DiagnosticIdle);
    mHandoff.AllowAckWrites=FALSE;
    if(Idle!=MtkMusbPlatformSuccess) {
      if(mHandoff.DeviceRetirementAttempted)mHandoff.PreparationFailed=TRUE;
      return BusFailure(Idle==MtkMusbPlatformQuiescenceFailed?0x511U:0x514U,EFI_NOT_READY);
    }
  }
  S=SnapshotHusb311SenseOnLease();
  if(EFI_ERROR(S)) {
    if(mHandoff.SenseAttempted||mHandoff.DeviceRetirementAttempted)mHandoff.PreparationFailed=TRUE;
    return S;
  }
  if(Power==2U) {
    S=CheckIdleDeviceOnLease(Power,Cc,Role);if(EFI_ERROR(S))goto Failed;
    mHandoff.SenseAttempted=TRUE;
    S=DisableHusb311VconnOnLease(&Control);if(EFI_ERROR(S))goto Failed;
  }
  mHandoff.SenseAttempted=TRUE;
  for(UINTN I=0U;I<ARRAY_SIZE(SenseRegister);++I) {
    BOOLEAN RetiringVconn=I==0U&&Power==2U;
    S=EnableHusb311SenseRegister(I,RetiringVconn);
    if(EFI_ERROR(S))goto Failed;
    if(RetiringVconn) {
      S=WaitForHusb311VconnOff(Control);if(EFI_ERROR(S))goto Failed;
      S=CheckIdleDeviceOnLease(0U,Cc,Role);if(EFI_ERROR(S))goto Failed;
    }
  }
  for(UINTN N=0U;N<=20U;++N) {
    S=ReadPowerRegister(TCPC_ADDRESS,0x1eU,&Live,1U);
    if(EFI_ERROR(S)){SenseFailure(FALSE,5U,0x1eU,Power,8U,0xffU,S);goto Failed;}
    if((Live&0xf8U)==8U)return EFI_SUCCESS;
    if(N!=20U)MicroSecondDelay(1000U);
  }
  S=SenseFailure(FALSE,6U,0x1eU,Power,8U,Live,EFI_NOT_READY);
Failed:
  mHandoff.PreparationFailed=TRUE;
  if(EFI_ERROR(RestoreHusb311SenseOnLease()))mHandoff.ImageMustRemainResident=TRUE;
  return S;
}
// The device stop uses the current ID level and electrical state. The
// target host route and its GPIO output requirements are selected later.
STATIC UINT32 DevicePowerSample (UINT8 Power, UINT8 Cc, UINT8 Role, UINT8 Vendor) {
  return ((UINT32)Power<<24)|((UINT32)Cc<<16)|((UINT32)Role<<8)|(Vendor&0x02U);
}

STATIC EFI_STATUS DeviceDisconnectGuard (CONST VOID *F,BOOLEAN Commit) {
  MTK_MUSB_PLATFORM_EVIDENCE E={0}; MTK_MUSB_PLATFORM_GPIO_SAMPLE G;
  STATIC CONST UINT32 HubPins[]={99U,4U,104U};
  EFI_STATUS S,Released; UINT8 Sc,Part,Power,Cc,Role,Vendor,Id[2],Saved;
  COLD_USB_PIN Hub[3];UINT8 Physical[3];UINT16 Csr;BOOLEAN OwnLease,NeedSense=FALSE;
  UINT32 Sample=0U;
  if(mHandoff.ExitingBootServices||mHandoff.ReadyInstalled||
     EFI_ERROR(ValidateGcd(MTK_MUSB_PLATFORM_CONTROLLER_BASE,MTK_MUSB_PLATFORM_CONTROLLER_BYTES))||
     EFI_ERROR(ValidateGcd(PINCTRL_BASE,PINCTRL_BYTES))||
     EFI_ERROR(ReadClockEvidence(F,&E))) return EFI_NOT_READY;
  mHandoff.DiagnosticDeviceStop=1U;
  S=ReadGpioSample(10U,&G);
  if(!EFI_ERROR(S))mHandoff.DiagnosticId=(UINT8)G.PhysicalInput;
  if(EFI_ERROR(S)||!DeviceRequestMatches())return EFI_NOT_READY;
  mHandoff.DiagnosticDeviceStop=3U;
  for(UINTN I=0;I<ARRAY_SIZE(HubPins);++I) {
    COLD_USB_PIN Seed={0};Seed.Pin=HubPins[I];
    S=ReadColdPin(&Seed,&Hub[I]);
    if(!EFI_ERROR(S))S=ReadGpioSample(HubPins[I],&G);
    if(EFI_ERROR(S))return EFI_NOT_READY;
    Physical[I]=(UINT8)G.PhysicalInput;
    Hub[I].ExpectedMode=Hub[I].Mode;Hub[I].ExpectedDirection=Hub[I].Direction;
    Hub[I].ExpectedOutput=Hub[I].Output;
    if (Hub[I].Mode == 0U && Hub[I].Direction == MTK_MUSB_PLATFORM_GPIO_OUTPUT &&
        Hub[I].Output != Physical[I]) return EFI_NOT_READY;
    if(mHandoff.DeviceHubSampled&&
       (!ColdPinFieldsMatch(&mHandoff.DeviceHubPins[I],&Hub[I])||
        mHandoff.DeviceHubPhysical[I]!=Physical[I]))return EFI_NOT_READY;
  }
  if(!mHandoff.DeviceHubSampled) {
    CopyMem(mHandoff.DeviceHubPins,Hub,sizeof(Hub));
    CopyMem(mHandoff.DeviceHubPhysical,Physical,sizeof(Physical));
    mHandoff.DeviceHubSampled=TRUE;
  }
  mHandoff.DiagnosticDeviceStop=2U;
  for(UINTN I=0U;I<ARRAY_SIZE(mHandoff.DiagnosticPower);++I)mHandoff.DiagnosticPower[I]=0xffU;
  mHandoff.DiagnosticPowerValid=0U;
  OwnLease=mHandoff.I2cToken==0U;
  if(OwnLease) { S=AcquirePowerBus();if(EFI_ERROR(S))return S; }
  S=ReadPowerRegister(SC_ADDRESS,0x14U,&Part,1U);
  if(!EFI_ERROR(S))S=ReadPowerRegister(SC_ADDRESS,3U,&Sc,1U);
  if(!EFI_ERROR(S))S=ReadPowerRegister(TCPC_ADDRESS,0U,Id,2U);
  if(!EFI_ERROR(S)&&(Id[0]!=0x99U||Id[1]!=0x2eU))S=BusFailure(0x507U,EFI_UNSUPPORTED);
  if(!EFI_ERROR(S))S=ReadPowerRegister(TCPC_ADDRESS,2U,Id,2U);
  if(!EFI_ERROR(S)&&(Id[0]!=0x11U||Id[1]!=0x03U))S=BusFailure(0x508U,EFI_UNSUPPORTED);
  if(!EFI_ERROR(S))S=ReadPowerRegister(TCPC_ADDRESS,0x1eU,&Power,1U);
  if(!EFI_ERROR(S))S=ReadPowerRegister(TCPC_ADDRESS,0x1dU,&Cc,1U);
  if(!EFI_ERROR(S))S=ReadPowerRegister(TCPC_ADDRESS,0x1aU,&Role,1U);
  if(!EFI_ERROR(S))S=ReadPowerRegister(TCPC_ADDRESS,0x97U,&Vendor,1U);
  if(!EFI_ERROR(S)) {
    Sample=DevicePowerSample(Power,Cc,Role,Vendor);
    if(Commit&&mHandoff.DeviceRolePhase==UsbDeviceRoleConfirming&&
       Sample!=mHandoff.DevicePowerSample)S=EFI_MEDIA_CHANGED;
  }
  if(mHandoff.TypecSenseTicks!=0U) {
    if(EFI_ERROR(S)) mHandoff.TypecSenseTicks=0U;
    else S=PollPassiveVbusSense(Power);
  }
  if(!EFI_ERROR(S)&&(0x94U&(1U<<((Part>>3)&7U)))!=0U&&!(Sc&0x20U)) {
    if(!Commit&&!mHandoff.SenseAttempted&&Power==0U) {
      // Waking the detector does not change CC, power, or the LK transport.
      S=BeginPassiveVbusSenseOnLease();
      if(!EFI_ERROR(S)) S=EFI_NOT_READY;
    }
    NeedSense=Power==2U&&Husb311OpenCcPogo(Cc,Role);
    if(!EFI_ERROR(S)&&NeedSense&&Commit) {
      S=WakeHusb311SenseOnLease(Power,Cc,Role);
      if(!EFI_ERROR(S))S=ReadPowerRegister(TCPC_ADDRESS,0x1eU,&Power,1U);
      if(!EFI_ERROR(S))S=ReadPowerRegister(TCPC_ADDRESS,0x1dU,&Cc,1U);
      if(!EFI_ERROR(S))S=ReadPowerRegister(TCPC_ADDRESS,0x1aU,&Role,1U);
      if(!EFI_ERROR(S))S=ReadPowerRegister(TCPC_ADDRESS,0x97U,&Vendor,1U);
    }
  }
  Released=OwnLease?ReleasePowerBus():EFI_SUCCESS;
  if(EFI_ERROR(S)||EFI_ERROR(Released))return EFI_ERROR(S)?S:Released;
  if((0x94U&(1U<<((Part>>3)&7U)))==0U)return BusFailure(0x501U,EFI_NOT_READY);
  if(Sc&0x20U)return BusFailure(0x502U,EFI_NOT_READY);
  if(Commit||!NeedSense) {
    if((Power&0xf8U)!=0x08U)return BusFailure(0x503U,EFI_NOT_READY);
    if(Power&4U) {
      // ID-low can request the independent hub while Type-C is a sink.
      if(mHandoff.DeviceId!=0U||
         (!Husb311SinkAttached(Cc,Role,Power)&&!Husb311OpenCcPogo(Cc,Role)))
        return BusFailure(0x504U,EFI_NOT_READY);
    } else {
      if(!SpHusb311VbusSafe0V(Power,Vendor))return BusFailure(0x505U,EFI_NOT_READY);
    }
  }
  mHandoff.DevicePowerSample=DevicePowerSample(Power,Cc,Role,Vendor);
  if(!Commit)return EFI_SUCCESS;
  mHandoff.DiagnosticDeviceStop=4U;
  if(MtkMusbPlatformCheckDeviceForDisconnect(&mHandoff.Io,&mHandoff.DiagnosticIdle)!=MtkMusbPlatformSuccess)
    return EFI_NOT_READY;
  // Peripheral SETUPEND is not part of the host CSR0 busy mask.
  Saved=MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x0eU);
  MmioWrite8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x0eU,0U);
  Csr=MmioRead16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x12U);
  MmioWrite8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x0eU,Saved);
  if(Csr&0x10U)return EFI_NOT_READY;
  mHandoff.DiagnosticDeviceStop=5U;
  if(MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+1U)&0x0eU)
    return EFI_NOT_READY;
  return mHandoff.ExitingBootServices?EFI_NOT_READY:EFI_SUCCESS;
}
STATIC EFI_STATUS QueueDeviceRolePhase (USB_DEVICE_ROLE_PHASE Phase) {
  mHandoff.DeviceRolePhase = Phase;
  return EFI_NOT_READY;
}

// Like the kernel role switch, stop the device before requesting a host.
// Confirm the detach across callbacks with no I2C lease held in between.
STATIC EFI_STATUS DisconnectInheritedDevice (CONST VOID *F) {
  EFI_STATUS S,Released;UINT8 Power,Devctl,TcpcPower,Cc,Role,Saved;UINT16 Csr;
  MTK_MUSB_PLATFORM_STATUS Idle;
  BOOLEAN Confirming=mHandoff.DeviceRolePhase==UsbDeviceRoleConfirming;
  if(mHandoff.DeviceDisconnectInProgress||mHandoff.DeviceDisconnectAttempted||
     mHandoff.PreparationFailed)return EFI_NOT_READY;
  if(!Confirming) {
    mHandoff.DeviceHubSampled=FALSE;
    S=ReadBoardId(&mHandoff.DeviceId);if(EFI_ERROR(S))return S;
  }
  mHandoff.DeviceDisconnectInProgress=TRUE;
  S=DeviceDisconnectGuard(F,Confirming);
  mHandoff.DeviceRolePhase=UsbDeviceRoleStable;
  if(EFI_ERROR(S)) {
    if(mHandoff.DeviceId!=0U&&!mHandoff.DeviceRetirementAttempted) {
      mHandoff.DeviceHubSampled=FALSE;
      if(mHandoff.I2cToken)mHandoff.PreparationFailed=TRUE;
    }
    mHandoff.DeviceDisconnectInProgress=FALSE;
    return S;
  }
  if(!Confirming) {
    mHandoff.DeviceDisconnectInProgress=FALSE;
    return QueueDeviceRolePhase(UsbDeviceRoleConfirming);
  }
  TcpcPower=(UINT8)(mHandoff.DevicePowerSample>>24);
  Cc=(UINT8)(mHandoff.DevicePowerSample>>16);
  Role=(UINT8)(mHandoff.DevicePowerSample>>8);
  S=AcquirePowerBus();if(EFI_ERROR(S))goto BeforeDisconnectFailed;
  S=RetireIdleDeviceOnLease(TcpcPower,Cc,Role);
  Released=ReleasePowerBus();
  if(EFI_ERROR(Released))goto Failed;
  if(EFI_ERROR(S))goto BeforeDisconnectFailed;
  mHandoff.DeviceDisconnectAttempted=TRUE;
  Power=MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+1U);
  mHandoff.DiagnosticDeviceStop=6U;
  if(Power&0x40U) {
    if(!mHandoff.Io.Write8(&mHandoff,1U,(UINT8)(Power&~0x40U))||
       (MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+1U)&0x40U)) goto Failed;
    MicroSecondDelay(1000U);
  }
  S=DeviceDisconnectGuard(F,TRUE);if(EFI_ERROR(S))goto Failed;
  mHandoff.DiagnosticDeviceStop=7U;
  Devctl=MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x60U);
  if((Devctl&1U)&&(!mHandoff.Io.Write8(&mHandoff,0x60U,(UINT8)(Devctl&~1U))||
       (MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x60U)&1U))) goto Failed;
  MicroSecondDelay(1000U);
  mHandoff.DiagnosticDeviceStop=8U;
  Saved=MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x0eU);
  MmioWrite8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x0eU,0U);
  Csr=MmioRead16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x12U);
  MmioWrite8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x0eU,Saved);
  if(Csr&8U) {
    S=AcquirePowerBus();if(EFI_ERROR(S))goto Failed;
    S=DeviceDisconnectGuard(F,TRUE);
    if(!EFI_ERROR(S))S=RetireIdleDeviceOnLease(TcpcPower,Cc,Role);
    if(!EFI_ERROR(S)) {
      mHandoff.DeviceRetiring=TRUE;
      Idle=MtkMusbPlatformRequestDisconnectedDeviceReset(&mHandoff.Io,&mHandoff.DiagnosticIdle);
      mHandoff.DeviceRetiring=FALSE;
      if(Idle!=MtkMusbPlatformSuccess)S=BusFailure(0x518U,EFI_DEVICE_ERROR);
    }
    Released=ReleasePowerBus();
    if(EFI_ERROR(S)||EFI_ERROR(Released))goto Failed;
    MicroSecondDelay(1000U);
  }
  Idle=MtkMusbPlatformCheckIdleDevice(&mHandoff.Io,&mHandoff.DiagnosticIdle);
  if(Idle!=MtkMusbPlatformSuccess) { BusFailure(0x519U,EFI_DEVICE_ERROR);goto Failed; }
  mHandoff.AllowAckWrites=TRUE;
  Idle=MtkMusbPlatformAcknowledgeIdleInterruptsWithDiagnostic(&mHandoff.Io,&mHandoff.DiagnosticIdle);
  mHandoff.AllowAckWrites=FALSE;
  if(Idle!=MtkMusbPlatformSuccess||
     MtkMusbPlatformCheckQuiescent(&mHandoff.Io,1U)!=MtkMusbPlatformSuccess)goto Failed;
  mHandoff.DiagnosticDevctl=MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x60U);
  mHandoff.DiagnosticDeviceStop=9U;
  mHandoff.DeviceDisconnectComplete=TRUE;
  mHandoff.DeviceDisconnectInProgress=FALSE;
  return QueueDeviceRolePhase(UsbDeviceRoleNeutral);
BeforeDisconnectFailed:
  // The next timer tick can resample if the inherited transport and sensing
  // are untouched and no lease is outstanding.
  if(!mHandoff.DeviceRetirementAttempted&&!mHandoff.SenseAttempted&&!mHandoff.I2cToken) {
    mHandoff.DeviceDisconnectInProgress=FALSE;
    return S;
  }
Failed:
  // Sensing may remain awake while we wait for a safe Type-C voltage. The
  // next pass may resample once the lease is returned and LK is untouched.
  if(mHandoff.DeviceId!=0U&&!mHandoff.DeviceRetirementAttempted&&
     !mHandoff.DeviceDisconnectAttempted&&!mHandoff.I2cToken) {
    mHandoff.DeviceDisconnectInProgress=FALSE;
    return S;
  }
  // The old LK transport will not resume. Keep a partially disconnected
  // controller stopped; never replay a failed handoff or reconnect its pullup.
  mHandoff.PreparationFailed=TRUE;
  mHandoff.DeviceDisconnectInProgress=FALSE;return EFI_DEVICE_ERROR;
}

// The caller has verified the mapping and clock before sampling MUSB.
STATIC EFI_STATUS SettlePassiveDevice (UINT8 Devctl, UINT8 Power) {
  MTK_MUSB_PLATFORM_STATUS Status;

  if ((Devctl & 1U) != 0U || (Power & 0x4eU) != 0U) return EFI_NOT_READY;
  Status = MtkMusbPlatformCheckQuiescent(&mHandoff.Io, 1U);
  if (Status == MtkMusbPlatformSuccess) return EFI_SUCCESS;
  if (Status != MtkMusbPlatformQuiescenceFailed || (Devctl & 4U) != 0U)
    return EFI_NOT_READY;

  // LK may have stopped the session but left interrupt configuration and
  // completion flags. Recheck the disconnected device on every guarded write.
  mHandoff.PassiveDeviceRetiring = TRUE;
  Status = MtkMusbPlatformRetireIdleDevice(&mHandoff.Io, &mHandoff.DiagnosticIdle);
  if (Status == MtkMusbPlatformSuccess) {
    mHandoff.AllowAckWrites = TRUE;
    Status = MtkMusbPlatformAcknowledgeIdleInterruptsWithDiagnostic(
      &mHandoff.Io, &mHandoff.DiagnosticIdle);
    mHandoff.AllowAckWrites = FALSE;
  }
  mHandoff.PassiveDeviceRetiring = FALSE;
  return Status == MtkMusbPlatformSuccess ? EFI_SUCCESS : EFI_NOT_READY;
}

// Both power paths own the same controller and PHY. Only the power evidence
// that authorizes the start differs.
STATIC EFI_STATUS BindColdHost (CONST VOID *F,UINTN Bytes,UINT16 *Step) {
  EFI_STATUS S=ValidateGcd(MTK_MUSB_PLATFORM_CONTROLLER_BASE,MTK_MUSB_PLATFORM_CONTROLLER_BYTES);
  if(EFI_ERROR(S))return S;
  S=PrepareColdClocks(F);
  if(EFI_ERROR(S))return S;
  ++*Step;
  S=SettlePassiveDevice(MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x60U),
                        MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+1U));
  if(EFI_ERROR(S))return S;
  ++*Step;
  // Bind checks calibration and the PHY adapter without powering the PHY.
  S=MtkUsbPhyPlatformBind(F,Bytes,mHandoff.Clock,NULL,NULL,
    &mHandoff.ColdPhyIo,&mHandoff.ColdPhyConfig,&mHandoff.ColdPhyTuning);
  if(EFI_ERROR(S))return S;
  mHandoff.PhyBound=TRUE;
  ++*Step;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS PowerBoundHost (UINT16 *Step) {
  MTK_USB_PHY_STATUS P;
  BOOLEAN IdLow=mHandoff.IdLowBound;
  *Step=IdLow?0x6106U:0x6008U;
  P=MtkUsbPhyCoreInitialize(&mHandoff.PhyCore,&mHandoff.ColdPhyIo,&mHandoff.ColdPhyConfig);
  if(P!=MtkUsbPhySuccess)return EFI_DEVICE_ERROR;
  ++*Step;
  P=MtkUsbPhyCorePowerOn(&mHandoff.PhyCore);
  if(P!=MtkUsbPhySuccess)return EFI_DEVICE_ERROR;
  ++*Step;
  P=MtkUsbPhyCoreSetUsbMode(&mHandoff.PhyCore,MtkUsbPhyModeOtg,NULL);
  if(P!=MtkUsbPhySuccess)return EFI_DEVICE_ERROR;
  mHandoff.ColdPhyUsbMode=MTK_MUSB_PLATFORM_PHY_USB_MODE_OTG;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS PublishReadyHost (
  CONST MTK_MUSB_PLATFORM_EVIDENCE *E,CONST MTK_MUSB_PLATFORM_READY_RESULT *R) {
  EFI_STATUS S;
  ZeroMem(&mHandoff.Ready,sizeof(mHandoff.Ready));
  mHandoff.Ready.Signature=MTK_MUSB_HOST_PLATFORM_READY_SIGNATURE;
  mHandoff.Ready.Revision=MTK_MUSB_HOST_PLATFORM_READY_REVISION;
  mHandoff.Ready.Length=sizeof(mHandoff.Ready); mHandoff.Ready.Flags=R->Flags;
  mHandoff.Ready.HostPowerPath=R->HostPowerPath;
  mHandoff.Ready.ControllerBase=MTK_MUSB_PLATFORM_CONTROLLER_BASE;
  mHandoff.Ready.ControllerLength=MTK_MUSB_PLATFORM_CONTROLLER_BYTES;
  mHandoff.Ready.PhyBase=MTK_MUSB_PLATFORM_PHY_BASE;
  mHandoff.Ready.PhyLength=MTK_MUSB_PLATFORM_PHY_BYTES;
  mHandoff.Ready.InterruptVector=MTK_MUSB_PLATFORM_GSIV;
  mHandoff.Ready.EndpointCount=MTK_MUSB_PLATFORM_ENDPOINTS;
  mHandoff.Ready.PortCount=MTK_MUSB_PLATFORM_PORTS;
  mHandoff.Ready.FifoProfile=MTK_MUSB_PLATFORM_FIFO_PROFILE_T70_V1;
  mHandoff.Ready.InterruptMask=MTK_MUSB_PLATFORM_EVENTS;
  mHandoff.Ready.HostSnapshotValid=0;
  mHandoff.RouteCcPin=((E->Husb311CcStatus&3U)==2U)?1U:2U;
  mHandoff.HostChildrenDisconnected=FALSE;
  // A fresh handle has no DevicePath. MusbHostDxe adds USB2HC and its vendor
  // DevicePath to this same USB controller handle.
  mHandoff.ReadyHandle=NULL;
  S=gBS->InstallProtocolInterface(&mHandoff.ReadyHandle,
     &gMtkMusbHostPlatformReadyProtocolGuid,EFI_NATIVE_INTERFACE,&mHandoff.Ready);
  if(EFI_ERROR(S)) { mHandoff.ReadyHandle=NULL; return S; }
  mHandoff.ReadyInstalled=TRUE;UsbWait(UsbDiagHostReady,EFI_SUCCESS);return EFI_SUCCESS;
}

STATIC int Read8 (VOID *C,UINT32 O,UINT8 *V) {
  if(C!=&mHandoff||mHandoff.Signature!=DRIVER_SIGNATURE||!V||
     O>=MTK_MUSB_PLATFORM_CONTROLLER_BYTES) return 0;
  *V=MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O); return 1;
}
STATIC int Read16 (VOID *C,UINT32 O,UINT16 *V) {
  if(C!=&mHandoff||mHandoff.Signature!=DRIVER_SIGNATURE||!V||(O&1)||
     O>MTK_MUSB_PLATFORM_CONTROLLER_BYTES-sizeof(UINT16)) return 0;
  *V=MmioRead16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O); return 1;
}
STATIC int Read32 (VOID *C,UINT32 O,UINT32 *V) {
  if(C!=&mHandoff||mHandoff.Signature!=DRIVER_SIGNATURE||!V||(O&3)||
     O>MTK_MUSB_PLATFORM_CONTROLLER_BYTES-sizeof(UINT32)) return 0;
  *V=MmioRead32((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O); return 1;
}
STATIC BOOLEAN CanRetireDevice (VOID) {
  if (mHandoff.PassiveDeviceRetiring)
    return mHandoff.EndOfDxeSeen && !mHandoff.ReadyInstalled &&
      !mHandoff.ExitingBootServices && mHandoff.ColdHost.Signature == 0U &&
      (MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE + 0x60U) & 0x05U) == 0U &&
      (MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE + 1U) & 0x4eU) == 0U;

  return mHandoff.DeviceRetiring&&mHandoff.DeviceDisconnectInProgress&&
    mHandoff.EndOfDxeSeen&&mHandoff.I2cToken&&!mHandoff.ColdBound&&
    !mHandoff.ReadyInstalled&&!mHandoff.ExitingBootServices&&
    DeviceRequestMatches();
}
STATIC BOOLEAN CanColdReset (VOID) {
  return mHandoff.AllowColdWrites&&mHandoff.ColdInProgress&&mHandoff.ColdBound&&
    !mHandoff.ReadyInstalled&&!mHandoff.ExitingBootServices&&
    mHandoff.ColdHost.Kind==MtkMusbPlatformColdStateStarting&&
    mHandoff.ColdHost.StartStep==MtkMusbPlatformColdStartReset&&
    mHandoff.ColdHost.SessionVerified&&mHandoff.ColdHost.PhyHostVerified&&
    (mHandoff.ColdHost.OwnedMask&5U)==5U;
}
STATIC int Write8 (VOID *C,UINT32 O,UINT8 V) {
  if(C!=&mHandoff||mHandoff.Signature!=DRIVER_SIGNATURE) return 0;
  if (O == 0x74U) {
    UINT8 Expected;
    if (!CanColdReset()) return 0;
    Expected = MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE + O);
    Expected = (UINT8)((Expected | MTK_MUSB_PLATFORM_SWRST_BABBLE_SET) &
                       ~MTK_MUSB_PLATFORM_SWRST_BABBLE_CLEAR);
    if (V != Expected) return 0;
    MmioWrite8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE + O, V);
    return 1;
  }
  if(O==0x0bU) {
    if(V==0U&&CanColdReset()) {
      MmioWrite8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,0U);return 1;
    }
    if(V!=0U||!CanRetireDevice())return 0;
    if (!mHandoff.PassiveDeviceRetiring) mHandoff.DeviceRetirementAttempted=TRUE;
    MmioWrite8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,0U);return 1;
  }
  if(mHandoff.DeviceDisconnectInProgress&&(O==1U||O==0x60U)) {
    UINT8 Current;
    if(!mHandoff.DeviceDisconnectAttempted||mHandoff.ReadyInstalled||
       mHandoff.ExitingBootServices) return 0;
    Current=MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O);
    if(V!=(UINT8)(Current&~((O==1U)?0x40U:1U))) return 0;
    MmioWrite8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,V);return 1;
  }
  // Indexed register reads must select and restore INDEX, including the
  // initial read-only evidence pass. Completion W1C needs explicit ownership.
  if(O!=0x0EU && !(mHandoff.AllowFifoWrites&&(O==0x62U||O==0x63U)) &&
     !(mHandoff.AllowColdWrites&&mHandoff.ColdInProgress&&mHandoff.ColdBound&&
       !mHandoff.ReadyInstalled&&!mHandoff.ExitingBootServices&&(O==0x60U||O==0x01U)) &&
     (!mHandoff.AllowAckWrites||O!=MUSB_DMA_INTR)) return 0;
  if(O==0x0EU && V>=MTK_MUSB_PLATFORM_ENDPOINTS) return 0;
  if (O == MUSB_DMA_INTR && mHandoff.PassiveDeviceRetiring && !CanRetireDevice()) return 0;
  MmioWrite8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,V); return 1;
}
STATIC int Write16 (VOID *C,UINT32 O,UINT16 V) {
  if(C!=&mHandoff||mHandoff.Signature!=DRIVER_SIGNATURE) return 0;
  if(CanColdReset()&&(O==0x74U||(O>=0x204U&&O<=0x274U&&((O-0x204U)%16U)==0U))) {
    UINT16 Current=MmioRead16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O);
    if((O==0x74U&&V!=(UINT16)(Current|3U))||
       (O!=0x74U&&((Current&1U)||V!=0U)))return 0;
    MmioWrite16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,V);return 1;
  }
  if(O==0x74U) {
    UINT16 Current;
    if(mHandoff.PassiveDeviceRetiring||!CanRetireDevice()||
       !mHandoff.DeviceDisconnectAttempted||mHandoff.DeviceResetAttempted||
       (MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+1U)&0x4eU)||
       (MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x60U)&0x85U)!=0x80U||
       MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x0bU)||
       MmioRead16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+6U)||
       MmioRead16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+8U))return 0;
    Current=MmioRead16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O);
    if((Current&1U)||V!=(UINT16)(Current|3U))return 0;
    mHandoff.DeviceResetAttempted=TRUE;
    MmioWrite16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,V);return 1;
  }
  if(O==0x12U||O==0x16U) {
    UINT16 Current,Expected;UINT8 Endpoint;
    if(!CanRetireDevice())return 0;
    Endpoint=MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x0eU);
    if(Endpoint==0U||Endpoint>=MTK_MUSB_PLATFORM_ENDPOINTS||
       MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x0bU)!=0U||
       MmioRead16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+6U)!=0U||
       MmioRead16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+8U)!=0U)return 0;
    Current=MmioRead16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O);
    if(O==0x12U) {
      if(Current&0x004bU)return 0;
      // DMAENAB/AUTOSET must be down before DMAMODE changes.
      if(Current&0x9000U)Expected=(UINT16)((Current&~0x9000U)|0x00a6U);
      else if(Current&0x0400U)Expected=(UINT16)((Current&~0x0400U)|0x00a6U);
      else return 0;
    } else {
      if((Current&0x40b3U)||(Current&0xa800U)==0U)return 0;
      Expected=(UINT16)((Current&~0xa800U)|0x0045U);
    }
    if(V!=Expected)return 0;
    if (!mHandoff.PassiveDeviceRetiring) mHandoff.DeviceRetirementAttempted=TRUE;
    MmioWrite16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,V);return 1;
  }
  if(O==6U||O==8U) {
    if(V==0U&&CanColdReset()) {
      MmioWrite16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,0U);return 1;
    }
    if(V!=0U||!CanRetireDevice())return 0;
    if (!mHandoff.PassiveDeviceRetiring) mHandoff.DeviceRetirementAttempted=TRUE;
    MmioWrite16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,0U);return 1;
  }
  if(!(mHandoff.AllowFifoWrites&&(O==0x64U||O==0x66U))&&
     !(mHandoff.AllowAckWrites&&(O==MUSB_INTRTX||O==MUSB_INTRRX))) return 0;
  if (mHandoff.PassiveDeviceRetiring && !CanRetireDevice()) return 0;
  MmioWrite16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,V); return 1;
}
STATIC int Write32 (VOID *C,UINT32 O,UINT32 V) {
  UINT32 Current;
  if(C!=&mHandoff||mHandoff.Signature!=DRIVER_SIGNATURE||O!=0x204U||!CanColdReset())return 0;
  Current=MmioRead32((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O);
  if((Current&1U)||(V!=(Current|0x600U)&&V!=(Current|0x4000U)))return 0;
  MmioWrite32((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,V);return 1;
}
STATIC int TryLock (VOID *C) {
  return C==&mHandoff&&mHandoff.Signature==DRIVER_SIGNATURE&&
         EfiAcquireLockOrFail(&mHandoff.IoLock)==EFI_SUCCESS;
}
STATIC VOID Unlock (VOID *C) {
  if(C==&mHandoff&&mHandoff.Signature==DRIVER_SIGNATURE) EfiReleaseLock(&mHandoff.IoLock);
}
STATIC VOID SetupIo (VOID) {
  ZeroMem(&mHandoff.Io,sizeof(mHandoff.Io)); mHandoff.Io.Context=&mHandoff;
  mHandoff.Io.ControllerLength=MTK_MUSB_PLATFORM_CONTROLLER_BYTES;
  mHandoff.Io.Read8=Read8; mHandoff.Io.Read16=Read16; mHandoff.Io.Read32=Read32;
  mHandoff.Io.Write8=Write8; mHandoff.Io.Write16=Write16; mHandoff.Io.Write32=Write32;
  mHandoff.Io.TryLock=TryLock; mHandoff.Io.Unlock=Unlock;
  EfiInitializeLock(&mHandoff.IoLock,TPL_NOTIFY);
}
STATIC EFI_STATUS ReadyHostProtocolPresent (EFI_GUID *Guid, BOOLEAN *Present) {
  VOID *Interface;
  EFI_STATUS Status = gBS->HandleProtocol(mHandoff.ReadyHandle, Guid, &Interface);
  *Present = !EFI_ERROR(Status);
  return Status == EFI_UNSUPPORTED ? EFI_SUCCESS : Status;
}

STATIC BOOLEAN ReadyHostComplete (VOID) {
  BOOLEAN UsbHc, DevicePath;
  return mHandoff.Ready.ReleaseHost && mHandoff.Ready.HostContext &&
    !EFI_ERROR(ReadyHostProtocolPresent(&gEfiUsb2HcProtocolGuid, &UsbHc)) && UsbHc &&
    !EFI_ERROR(ReadyHostProtocolPresent(&gEfiDevicePathProtocolGuid, &DevicePath)) && DevicePath;
}

STATIC BOOLEAN ReadyBusBound (VOID) {
  // UsbBusDxe installs its FILE_GUID as the private protocol on this handle.
  STATIC EFI_GUID UsbBusProtocol = {0x240612b7, 0xa063, 0x11d4,
    {0x9a, 0x3a, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
  BOOLEAN Present;
  return !EFI_ERROR(ReadyHostProtocolPresent(&UsbBusProtocol, &Present)) && Present;
}

STATIC EFI_STATUS ReleaseTypecRoute (VOID);

STATIC EFI_STATUS StopBoardHost (VOID) {
  EFI_STATUS Status;
  MTK_MUSB_PLATFORM_STATUS PlatformStatus;

  if (mHandoff.ReadyInstalled) {
    if (mHandoff.Ready.ReleaseHost != NULL) {
      if (!mHandoff.HostChildrenDisconnected) {
        Status = gBS->DisconnectController(mHandoff.ReadyHandle, NULL, NULL);
        if (EFI_ERROR(Status) && Status != EFI_NOT_FOUND) return Status;
        mHandoff.HostChildrenDisconnected = TRUE;
        mHandoff.HostBusConnected = FALSE;
      }
      Status = mHandoff.Ready.ReleaseHost(mHandoff.Ready.HostContext);
      if (EFI_ERROR(Status)) return Status;
    }
    BOOLEAN UsbHc, DevicePath;
    Status = ReadyHostProtocolPresent(&gEfiUsb2HcProtocolGuid, &UsbHc);
    if (EFI_ERROR(Status)) return Status;
    Status = ReadyHostProtocolPresent(&gEfiDevicePathProtocolGuid, &DevicePath);
    if (EFI_ERROR(Status)) return Status;
    if (UsbHc || DevicePath || mHandoff.Ready.HostContext) return EFI_NOT_READY;
    Status = gBS->UninstallProtocolInterface(mHandoff.ReadyHandle,
      &gMtkMusbHostPlatformReadyProtocolGuid, &mHandoff.Ready);
    if (EFI_ERROR(Status)) return Status;
    mHandoff.ReadyInstalled = FALSE;
    mHandoff.ReadyHandle = NULL;
  }

  if (mHandoff.RetainedHostOwned) {
    mHandoff.AllowAckWrites = TRUE;
    PlatformStatus = MtkMusbPlatformAcknowledgeIdleInterruptsWithDiagnostic(
                       &mHandoff.Io, &mHandoff.DiagnosticIdle);
    mHandoff.AllowAckWrites = FALSE;
    if (PlatformStatus == MtkMusbPlatformSuccess) return EFI_SUCCESS;
    return PlatformStatus == MtkMusbPlatformIoError ? EFI_DEVICE_ERROR : EFI_NOT_READY;
  }

  mHandoff.ColdInProgress = TRUE;
  Status = ReleaseColdController();
  if (!EFI_ERROR(Status)) Status = ReleaseOwnedPhy();
  if (!EFI_ERROR(Status)) Status = ReleaseColdClocks();
  mHandoff.ColdInProgress = FALSE;
  if (EFI_ERROR(Status)) return Status;
  mHandoff.ColdBound = FALSE;
  mHandoff.IdLowBound = FALSE;
  mHandoff.ColdOwnerToken = 0U;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS BindBoardPins (VOID) {
  STATIC CONST UINT32 Pins[] = {10U, 99U, 4U, 104U, 48U, 86U};
  EFI_STATUS Status;

  if (mHandoff.ColdPinBound) return EFI_SUCCESS;
  if (mDiagBind[1] != 0xffU) ++mDiagBind[1];
  if (!mHandoff.DeviceSettled || !mHandoff.Gpio->SetMode ||
      !mHandoff.Gpio->SetDir || !mHandoff.Gpio->SetState) {
    mDiagBind[3] = 0xf0U;
    if (mDiagBind[0] == 0U) mDiagBind[0] = 0xf0U;
    return EFI_NOT_READY;
  }
  for (UINTN Index = 0U; Index < ARRAY_SIZE(Pins); ++Index) {
    COLD_USB_PIN *Pin = &mHandoff.ColdPins[Index];
    ZeroMem(Pin, sizeof(*Pin));
    Pin->Pin = Pins[Index];
    Status = ReadColdPin(Pin, Pin);
    if (EFI_ERROR(Status)) {
      mDiagBind[3] = (UINT8)((Index + 1U) | 0x10U);
      if (mDiagBind[0] == 0U) mDiagBind[0] = mDiagBind[3];
      return Status;
    }
    Pin->ExpectedMode = Pin->Mode;
    Pin->ExpectedDirection = Pin->Direction;
    Pin->ExpectedOutput = Pin->Output;
  }
  mHandoff.ColdPinBound = TRUE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS DriveBoardPin (UINTN Index, UINT8 Value) {
  COLD_USB_PIN *Pin = &mHandoff.ColdPins[Index];
  EFI_STATUS Status = ReconcileColdPin(Pin);
  if (EFI_ERROR(Status)) return Status;
  Status = SetColdPinField(Pin, COLD_PIN_DO, Value);
  if (!EFI_ERROR(Status)) Status = SetColdPinField(Pin, COLD_PIN_DIR, 1U);
  if (!EFI_ERROR(Status)) Status = SetColdPinField(Pin, COLD_PIN_MODE, 0U);
  // SetColdPinField has verified the mode, direction and output latch. The pad
  // input buffer is not enabled on every path out of LK.
  return Status;
}

STATIC EFI_STATUS SetPogoRoute (BOOLEAN Pogo) {
  EFI_STATUS Status;

  if (mHandoff.PogoKnown && mHandoff.PogoEnabled == Pogo) return EFI_SUCCESS;
  mHandoff.PogoKnown = FALSE;
  // GPIO10 owns the mux. The stock hub asserts mux, power, then reset.
  // On detach it drops reset, power, then mux.
  for (UINTN Offset = 0U; Offset < 3U; ++Offset) {
    Status = DriveBoardPin(Pogo ? 1U + Offset : 3U - Offset, Pogo ? 1U : 0U);
    if (EFI_ERROR(Status)) return Status;
    if (Pogo && Offset != 2U) MicroSecondDelay(1000U);
  }
  mHandoff.PogoEnabled = Pogo;
  mHandoff.PogoKnown = TRUE;
  return EFI_SUCCESS;
}

typedef struct {
  UINT8 Role;
  UINT8 Cc;
  UINT8 Power;
} T70_TYPEC_SAMPLE;

STATIC EFI_STATUS SampleTypec (T70_TYPEC_SAMPLE *Sample, BOOLEAN *Contended) {
  UINT8 PowerControl = 0U;
  EFI_STATUS Status = AcquirePowerBusEx(Contended);
  if (EFI_ERROR(Status)) return Status;
  Status = ReadPowerRegister(TCPC_ADDRESS, 0x1aU, &Sample->Role, 1U);
  if (!EFI_ERROR(Status))
    Status = ReadPowerRegister(TCPC_ADDRESS, 0x1dU, &Sample->Cc, 1U);
  if (!EFI_ERROR(Status) && mHandoff.TypecSenseTicks != 0U)
    Status = ReadPowerRegister(TCPC_ADDRESS, 0x1cU, &PowerControl, 1U);
  if (!EFI_ERROR(Status))
    Status = ReadPowerRegister(TCPC_ADDRESS, 0x1eU, &Sample->Power, 1U);
  if ((PowerControl & 1U) != 0U) Sample->Power |= 2U;
  EFI_STATUS Release = ReleasePowerBus();
  return EFI_ERROR(Status) ? Status : Release;
}

STATIC BOOLEAN TypecSameAttachment (CONST T70_TYPEC_SAMPLE *Sample) {
  CONST MTK_HUSB311_TYPEC_CORE *Typec = &mHandoff.ColdTypecCore;
  return Sample->Role == Typec->ExpectedRoleControl &&
    MtkMusbPlatformHusb311SourceAttached(Sample->Cc, Sample->Role) &&
    (((Sample->Cc & 3U) == 2U) ? 1U : 2U) == Typec->AttachedCcPin;
}

STATIC BOOLEAN RetainedSourceLive (CONST T70_TYPEC_SAMPLE *Sample) {
  return MtkMusbPlatformHusb311SourceAttached(Sample->Cc, Sample->Role) &&
    (Sample->Power & (MTK_MUSB_PLATFORM_HUSB311_TCPC_INITIALIZING |
                      MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED |
                      MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT)) ==
                     (MTK_MUSB_PLATFORM_HUSB311_VBUS_DETECTION_ENABLED |
                      MTK_MUSB_PLATFORM_HUSB311_VBUS_PRESENT);
}


STATIC EFI_STATUS RestoreSourcePins (VOID) {
  EFI_STATUS Status;
  for (UINTN Index = 5U; Index >= 4U; --Index) {
    COLD_USB_PIN *Pin = &mHandoff.ColdPins[Index];
    if (Pin->Owned == 0U) continue;
    Status = ReconcileColdPin(Pin);
    if (!EFI_ERROR(Status) && Pin->Direction == 0U)
      Status = RestoreColdPinField(Pin, COLD_PIN_DIR);
    if (!EFI_ERROR(Status)) Status = RestoreColdPinField(Pin, COLD_PIN_MODE);
    if (!EFI_ERROR(Status)) Status = RestoreColdPinField(Pin, COLD_PIN_DO);
    if (!EFI_ERROR(Status)) Status = RestoreColdPinField(Pin, COLD_PIN_DIR);
    if (EFI_ERROR(Status)) return Status;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS RestoreTypecSense (VOID) {
  EFI_STATUS Status;
  EFI_STATUS Release;

  if (mHandoff.SenseOwnedMask == 0U) return EFI_SUCCESS;
  Status = mHandoff.I2cToken ? EFI_SUCCESS : AcquirePowerBus();
  if (EFI_ERROR(Status)) return Status;
  Status = RestoreHusb311SenseOnLease();
  Release = ReleasePowerBus();
  return EFI_ERROR(Status) ? Status : Release;
}

STATIC EFI_STATUS ReleaseTypecRoute (VOID) {
  EFI_STATUS Status;
  EFI_STATUS Cleanup;

  if (mHandoff.RetainedTypecSource) {
    // The source was already active at firmware entry. Stop using it without
    // rewriting power or CC state that this driver did not establish.
    mHandoff.RetainedTypecSource = FALSE;
    mHandoff.TypecVbus = FALSE;
    mHandoff.TypecPowerTicks = 0U;
    mHandoff.TypecSenseTicks = 0U;
    mHandoff.TypecReleasing = FALSE;
    return EFI_SUCCESS;
  }
  mHandoff.TypecVbus = FALSE;
  mHandoff.TypecPowerTicks = 0U;
  mHandoff.TypecSenseTicks = 0U;
  mHandoff.TypecLossTicks = 0U;
  Status = ReleaseOwnedSourcePower();
  if (!EFI_ERROR(Status)) {
    Status = RestoreSourcePins();
    // A failed GPIO restore must not prevent withdrawing our Rp.
    Cleanup = ReleaseColdTypec();
    if (Cleanup == EFI_NOT_READY || !EFI_ERROR(Status)) Status = Cleanup;
    if (!mHandoff.ColdTypecBound) {
      Cleanup = RestoreTypecSense();
      if (!EFI_ERROR(Status)) Status = Cleanup;
    }
  }
  mHandoff.TypecReleasing = EFI_ERROR(Status);
  return Status;
}

// Live Type-C decision record for real hardware. Read it from the EFI Shell
// with "dmem 43F7F400 C0". The handoff page keeps 0x400..0x4FF unused.
#define TYPEC_DIAG_BASE   0x43F7F400U
#define TYPEC_DIAG_EXITS  32U
#define TYPEC_DIAG_RING   8U
STATIC UINT8 mDiagVconn[8];   // POWER, CC, ROLE, SC03, CONTROL, FAULT, stage, count
STATIC UINT8 mDiagWake[4];    // POWER, stage, count, status
STATIC UINT32 mDiagTicks;
STATIC UINT16 mDiagExits[TYPEC_DIAG_EXITS];
STATIC UINT8 mDiagFirst[4];
STATIC UINT32 mDiagRing[TYPEC_DIAG_RING];
STATIC UINT32 mDiagRingCount;
STATIC UINT64 mDiagStartNs;
STATIC UINT64 mDiagShownNs;

STATIC UINT8 DiagStatus (EFI_STATUS Status) {
  return (UINT8)((Status & 0x7fU) | (EFI_ERROR(Status) ? 0x80U : 0U));
}

STATIC UINT8 DiagPin (CONST COLD_USB_PIN *Pin) {
  return (UINT8)(((Pin->Mode & 0x0fU) << 4) | ((Pin->Direction & 1U) << 1) | (Pin->Output & 1U));
}

// Without a shell, show the record at the bottom of the console while the
// receptacle is not sourcing. Rows are placed above the last line to avoid
// scrolling, and the cursor is restored for the menu that owns the screen.
STATIC VOID TypecDiagShow (VOID) {
  STATIC CONST CHAR16 Hex[] = L"0123456789abcdef";
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Out;
  CHAR16 Line[96];
  UINTN Columns, Rows, PerLine, Lines;
  INT32 SavedColumn, SavedRow;

  if (gST == NULL || (Out = gST->ConOut) == NULL || Out->OutputString == NULL ||
      Out->SetCursorPosition == NULL || Out->QueryMode == NULL || Out->Mode == NULL ||
      Out->Mode->Mode < 0 ||
      EFI_ERROR(Out->QueryMode(Out, (UINTN)Out->Mode->Mode, &Columns, &Rows))) return;
  PerLine = Columns >= 80U ? 32U : (Columns >= 44U ? 16U : 0U);
  if (PerLine == 0U) return;
  Lines = 0xc0U / PerLine;
  if (Rows < Lines + 2U) return;
  SavedColumn = Out->Mode->CursorColumn;
  SavedRow = Out->Mode->CursorRow;
  for (UINTN L = 0U; L < Lines; ++L) {
    UINTN Offset = L * PerLine, P = 0U;
    Line[P++] = Hex[(Offset >> 8) & 15U];
    Line[P++] = Hex[(Offset >> 4) & 15U];
    Line[P++] = Hex[Offset & 15U];
    Line[P++] = L' ';
    for (UINTN I = 0U; I < PerLine; ++I) {
      UINT8 V = MmioRead8(TYPEC_DIAG_BASE + Offset + I);
      if (I != 0U && (I & 3U) == 0U) Line[P++] = L' ';
      Line[P++] = Hex[V >> 4];
      Line[P++] = Hex[V & 15U];
    }
    Line[P] = 0;
    if (EFI_ERROR(Out->SetCursorPosition(Out, 0U, Rows - Lines - 1U + L))) break;
    (VOID)Out->OutputString(Out, Line);
  }
  if (SavedColumn >= 0 && SavedRow >= 0)
    (VOID)Out->SetCursorPosition(Out, (UINTN)SavedColumn, (UINTN)SavedRow);
}

STATIC VOID TypecDiagWrite (UINT8 Exit, UINT8 Role, UINT8 Cc, UINT8 Power, BOOLEAN Valid) {
  CONST MTK_HUSB311_TYPEC_CORE *C = &mHandoff.ColdTypecCore;
  CONST UINTN B = TYPEC_DIAG_BASE;
  UINT32 Flags, Entry;

  if (Exit < TYPEC_DIAG_EXITS && mDiagExits[Exit] != 0xffffU) ++mDiagExits[Exit];
  ++mDiagTicks;
  if (Valid && mDiagFirst[3] == 0U) {
    mDiagFirst[0] = Role; mDiagFirst[1] = Cc; mDiagFirst[2] = Power; mDiagFirst[3] = 1U;
  }
  Entry = ((UINT32)Exit << 24) | ((UINT32)Power << 16) | ((UINT32)Cc << 8) | Role;
  if (!Valid) Entry = ((UINT32)Exit << 24) | 0x00eeeeeeU;
  if (mDiagRingCount == 0U || mDiagRing[(mDiagRingCount - 1U) % TYPEC_DIAG_RING] != Entry) {
    mDiagRing[mDiagRingCount % TYPEC_DIAG_RING] = Entry;
    ++mDiagRingCount;
  }
  Flags = (mHandoff.OtgBound ? 0x01U : 0U) | (mHandoff.ColdTypecBound ? 0x02U : 0U) |
    (mHandoff.ColdProbePending ? 0x04U : 0U) | (mHandoff.TypecVbus ? 0x08U : 0U) |
    (mHandoff.TypecReleasing ? 0x10U : 0U) | (mHandoff.RetainedTypecSource ? 0x20U : 0U) |
    (mHandoff.HostBusConnected ? 0x40U : 0U) | (mHandoff.PogoEnabled ? 0x80U : 0U) |
    ((UINT32)mHandoff.SenseOwnedMask << 8) | ((UINT32)mHandoff.DiagnosticId << 16) |
    ((UINT32)Exit << 24);
  MmioWrite32(B + 0x00U, 0x43303754U);
  MmioWrite32(B + 0x04U, mDiagTicks);
  MmioWrite32(B + 0x08U, Flags);
  MmioWrite8(B + 0x0cU, DiagStatus(mHandoff.TypecStatus));
  MmioWrite8(B + 0x0dU, (UINT8)C->State);
  MmioWrite8(B + 0x0eU, C->AttachedCcPin);
  MmioWrite8(B + 0x0fU, C->ProbeWaitingForAttach);
  MmioWrite8(B + 0x10U, Valid ? Role : 0xeeU);
  MmioWrite8(B + 0x11U, Valid ? Cc : 0xeeU);
  MmioWrite8(B + 0x12U, Valid ? Power : 0xeeU);
  MmioWrite8(B + 0x13U, Valid ? 1U : 0U);
  for (UINTN I = 0U; I < 4U; ++I) MmioWrite8(B + 0x14U + I, mDiagFirst[I]);
  MmioWrite8(B + 0x18U, mHandoff.TypecLossTicks);
  MmioWrite8(B + 0x19U, mHandoff.TypecReadFailures);
  MmioWrite8(B + 0x1aU, (UINT8)mHandoff.TypecRetryTicks);
  MmioWrite8(B + 0x1bU, mHandoff.TypecSenseTicks);
  MmioWrite8(B + 0x1cU, mHandoff.TypecPowerTicks);
  MmioWrite8(B + 0x1dU, (UINT8)C->LastError);
  MmioWrite8(B + 0x1eU, (UINT8)C->ProbeFailure);
  MmioWrite8(B + 0x1fU, mHandoff.DiagnosticTypecGuardStep);
  MmioWrite8(B + 0x20U, C->FirstFailure.Step);
  MmioWrite8(B + 0x21U, C->FirstFailure.Register);
  MmioWrite8(B + 0x22U, C->FirstFailure.GuardFlags);
  MmioWrite8(B + 0x23U, (UINT8)C->FirstFailure.Status);
  MmioWrite16(B + 0x24U, C->FirstFailure.Value);
  MmioWrite8(B + 0x26U, (UINT8)C->FirstFailure.ReadStatus);
  MmioWrite8(B + 0x27U, C->ExpectedRoleControl);
  MmioWrite8(B + 0x28U, C->Diagnostic.Step);
  MmioWrite8(B + 0x29U, C->Diagnostic.Register);
  MmioWrite8(B + 0x2aU, C->Diagnostic.GuardFlags);
  MmioWrite8(B + 0x2bU, (UINT8)C->Diagnostic.Status);
  MmioWrite16(B + 0x2cU, C->Diagnostic.Value);
  MmioWrite8(B + 0x2eU, (UINT8)C->Diagnostic.ReadStatus);
  MmioWrite8(B + 0x2fU, C->OriginalRoleControl);
  for (UINTN I = 0U; I < 8U; ++I) {
    MmioWrite8(B + 0x30U + I, mDiagVconn[I]);
    MmioWrite8(B + 0x38U + I, mDiagPower[I]);
  }
  for (UINTN I = 0U; I < 4U; ++I) MmioWrite8(B + 0x40U + I, mDiagWake[I]);
  MmioWrite32(B + 0x44U, mDiagRingCount);
  for (UINTN I = 0U; I < TYPEC_DIAG_EXITS; ++I) MmioWrite16(B + 0x48U + I * 2U, mDiagExits[I]);
  for (UINTN I = 0U; I < TYPEC_DIAG_RING; ++I) MmioWrite32(B + 0x88U + I * 4U, mDiagRing[I]);
  MmioWrite8(B + 0xa8U, (mHandoff.EndOfDxeSeen ? 0x01U : 0U) | (mHandoff.ColdPinBound ? 0x02U : 0U) |
    (mHandoff.DeviceSettled ? 0x04U : 0U) | (mHandoff.ReadyInstalled ? 0x08U : 0U) |
    (mHandoff.PreparationFailed ? 0x10U : 0U) | (mHandoff.OtgBound ? 0x20U : 0U) |
    (mHandoff.EndOfDxeObserved ? 0x40U : 0U) | (mHandoff.RouteRetiring ? 0x80U : 0U));
  MmioWrite8(B + 0xa9U, (UINT8)mHandoff.PowerBusRetryTicks);
  MmioWrite8(B + 0xaaU, (UINT8)mHandoff.BoardRetryTicks);
  MmioWrite8(B + 0xabU, (UINT8)mHandoff.RouteTicks);
  for (UINTN I = 0U; I < 4U; ++I) MmioWrite8(B + 0xacU + I, mDiagBind[I]);
  {
    STATIC CONST UINT8 DiagPins[] = {10U, 99U, 4U, 104U, 48U, 86U};
    UINT8 Dir = 0U, Out = 0U;
    for (UINTN I = 0U; I < ARRAY_SIZE(DiagPins); ++I) {
      UINT32 P = DiagPins[I];
      MmioWrite8(B + 0xb0U + I, (UINT8)((MmioRead32((UINTN)PINCTRL_BASE + PIN_MODE_OFFSET(P)) >>
        PIN_MODE_SHIFT(P)) & 0xFU));
      if ((MmioRead32((UINTN)PINCTRL_BASE + (P / 32U) * 0x10U) >> (P % 32U)) & 1U) Dir |= (UINT8)(1U << I);
      if ((MmioRead32((UINTN)PINCTRL_BASE + 0x100U + (P / 32U) * 0x10U) >> (P % 32U)) & 1U) Out |= (UINT8)(1U << I);
    }
    MmioWrite8(B + 0xb6U, Dir);
    MmioWrite8(B + 0xb7U, Out);
  }
  // Timer callbacks can be delayed, so use elapsed time: show after 10 s
  // without a sourced receptacle and refresh every 3 s.
  if (!mHandoff.TypecVbus && !mHandoff.BootServicesLeaving && !mHandoff.ExitingBootServices) {
    UINT64 Now = GetTimeInNanoSecond(GetPerformanceCounter());
    if (mDiagStartNs == 0U) mDiagStartNs = Now;
    if (Now - mDiagStartNs >= 10000000000ULL && Now - mDiagShownNs >= 3000000000ULL) {
      mDiagShownNs = Now;
      TypecDiagShow();
    }
  }
}

STATIC EFI_STATUS SetTypecPower (VOID) {
  UINT8 Control = 0xeeU, Power = 0xeeU, Vendor = 0xeeU, Stage = 0U;
  EFI_STATUS Status = AcquirePowerBus();
  if (EFI_ERROR(Status)) return Status;
  mDiagPower[3] = mDiagPower[4] = 0xeeU;
  if (!ValidateColdTypecOnLease()) {
    Stage = 1U;
  } else if (EFI_ERROR(ReadPowerRegister(SC_ADDRESS, 3U, &Control, 1U)) ||
      EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS, 0x1eU, &Power, 1U)) ||
      EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS, 0x97U, &Vendor, 1U))) {
    Stage = 2U;
  } else if ((Control & 0x20U) != 0U) {
    Stage = 3U;
  } else if (!SpHusb311VbusSafe0V(Power, Vendor)) {
    Stage = 4U;
  }
  if (Stage != 0U) Status = EFI_NOT_READY;
  for (UINTN Index = 4U; Index <= 5U && !EFI_ERROR(Status); ++Index) {
    COLD_USB_PIN *Pin = &mHandoff.ColdPins[Index];
    COLD_USB_PIN Live;
    Status = ReadColdPin(Pin, &Live);
    if (EFI_ERROR(Status)) { Stage = 5U; break; }
    mDiagPower[Index - 1U] = DiagPin(&Live);
    // GPIO48 can still be high from charging. Preserve it for restoration.
    if (Pin->Owned != 0U ||
        (Index == 5U && Live.Mode == 0U && Live.Direction != 0U && Live.Output != 0U)) {
      Stage = Pin->Owned != 0U ? 6U : 7U;
      Status = EFI_NOT_READY;
      break;
    }
    *Pin = Live;
    Pin->ExpectedMode = Live.Mode;
    Pin->ExpectedDirection = Live.Direction;
    Pin->ExpectedOutput = Live.Output;
  }
  for (UINTN Index = 4U; Index <= 5U && !EFI_ERROR(Status); ++Index) {
    Status = DriveBoardPin(Index, 0U);
    if (EFI_ERROR(Status)) Stage = 8U;
  }
  if (!EFI_ERROR(Status)) {
    Status = PrepareSourcePower();
    if (EFI_ERROR(Status)) Stage = 9U;
  }
  mDiagPower[0] = Control; mDiagPower[1] = Power; mDiagPower[2] = Vendor;
  mDiagPower[5] = Stage; ++mDiagPower[6];
  EFI_STATUS Release = ReleasePowerBus();
  return EFI_ERROR(Status) ? Status : Release;
}

STATIC BOOLEAN TypecHasOpenVconn (UINT8 Power, UINT8 Cc, UINT8 Role) {
  return (Power == 2U || Power == 0x0aU) && Husb311OpenCcPogo(Cc, Role);
}

STATIC EFI_STATUS DisableSettledVconnOnLease (UINT8 Power) {
  UINT8 Cc = 0xeeU, Role = 0xeeU, Sc = 0xeeU, Control = 0xeeU, Fault = 0xeeU;
  EFI_STATUS Status = ReadPowerRegister(TCPC_ADDRESS, 0x1dU, &Cc, 1U);
  if (!EFI_ERROR(Status)) Status = ReadPowerRegister(TCPC_ADDRESS, 0x1aU, &Role, 1U);
  if (!EFI_ERROR(Status)) Status = ReadPowerRegister(SC_ADDRESS, 3U, &Sc, 1U);
  if (!EFI_ERROR(Status)) Status = ReadPowerRegister(TCPC_ADDRESS, 0x19U, &Control, 1U);
  if (!EFI_ERROR(Status)) Status = ReadPowerRegister(TCPC_ADDRESS, 0x1fU, &Fault, 1U);
  mDiagVconn[0] = Power; mDiagVconn[1] = Cc; mDiagVconn[2] = Role; mDiagVconn[3] = Sc;
  mDiagVconn[4] = Control; mDiagVconn[5] = Fault; ++mDiagVconn[7];
  mDiagVconn[6] = 1U;
  if (EFI_ERROR(Status)) return Status;
  mDiagVconn[6] = !TypecHasOpenVconn(Power, Cc, Role) ? 2U : (Sc & 0x20U) != 0U ? 3U :
    (Control & 2U) != 0U ? 4U : Fault != 0U ? 5U : 6U;
  if (!TypecHasOpenVconn(Power, Cc, Role) || (Sc & 0x20U) != 0U ||
      (Control & 2U) != 0U || Fault != 0U) return EFI_NOT_READY;

  Status = SnapshotHusb311SenseOnLease();
  if (EFI_ERROR(Status)) return Status;
  mDiagVconn[6] = 7U;
  Status = DisableHusb311VconnOnLease(&Control);
  if (EFI_ERROR(Status)) return Status;
  mDiagVconn[6] = 8U;
  // Both the write and its verification use the power bus. The controller
  // may still be clock-gated. Sample POWER_STATUS on the following ticks.
  return EnablePassiveVbusSenseOnLease(TRUE);
}

STATIC EFI_STATUS WakeSettledTypecSense (VOID) {
  UINT8 Id[2], Power;
  EFI_STATUS Status;
  EFI_STATUS Release;

  ++mDiagWake[2];
  mDiagWake[0] = 0xeeU;
  mDiagWake[1] = 1U;
  if (!mHandoff.DeviceSettled || mHandoff.ColdTypecBound || mHandoff.OtgBound ||
      mHandoff.ExitingBootServices || mHandoff.SenseOwnedMask != 0U)
    return EFI_NOT_READY;

  mDiagWake[1] = 2U;
  Status = AcquirePowerBus();
  if (EFI_ERROR(Status)) return Status;
  Status = ReadPowerRegister(TCPC_ADDRESS, 0x00U, Id, sizeof(Id));
  if (!EFI_ERROR(Status) && (Id[0] != 0x99U || Id[1] != 0x2eU)) Status = EFI_UNSUPPORTED;
  if (!EFI_ERROR(Status)) Status = ReadPowerRegister(TCPC_ADDRESS, 0x02U, Id, sizeof(Id));
  if (!EFI_ERROR(Status) && (Id[0] != 0x11U || Id[1] != 0x03U)) Status = EFI_UNSUPPORTED;
  if (!EFI_ERROR(Status)) Status = ReadPowerRegister(TCPC_ADDRESS, 0x1eU, &Power, 1U);
  if (!EFI_ERROR(Status)) {
    mDiagWake[0] = Power;
    if (Power == 2U || Power == 0x0aU) { mDiagWake[1] = 4U; Status = DisableSettledVconnOnLease(Power); }
    else if ((Power & 0x4eU) == 0U) { mDiagWake[1] = 5U; Status = BeginPassiveVbusSenseOnLease(); }
    else { mDiagWake[1] = 6U; Status = EFI_NOT_READY; }
  } else {
    mDiagWake[1] = 3U;
  }
  mDiagWake[3] = DiagStatus(Status);

  Release = ReleasePowerBus();
  return EFI_ERROR(Status) ? Status : Release;
}

STATIC BOOLEAN DeferTypecSample (VOID) {
  if (mHandoff.TypecRetryTicks != 0U && !mHandoff.TypecReleasing) {
    --mHandoff.TypecRetryTicks;
    return TRUE;
  }
  if (mHandoff.SenseOwnedMask != 0U && mHandoff.TypecSenseTicks == 0U) {
    EFI_STATUS Status = RestoreTypecSense();
    if (EFI_ERROR(Status)) mHandoff.TypecStatus = Status;
    mHandoff.TypecRetryTicks = USB_RETRY_TICKS;
    return TRUE;
  }
  return FALSE;
}

#define TYPEC_EXIT(Code) do { Exit = (Code); goto Out; } while (0)
STATIC VOID StepTypec (BOOLEAN CanEnableSource) {
  T70_TYPEC_SAMPLE Sample = {0};
  EFI_STATUS Status;
  BOOLEAN Contended = FALSE;
  BOOLEAN SampleValid = FALSE;
  UINT8 Exit = 0U;

  if (mHandoff.TypecReleasing) {
    mHandoff.TypecStatus = ReleaseTypecRoute();
    TYPEC_EXIT(1);
  }
  if (DeferTypecSample()) TYPEC_EXIT(2);
  Status = SampleTypec(&Sample, &Contended);
  SampleValid = !EFI_ERROR(Status);
  if (EFI_ERROR(Status)) {
    mHandoff.TypecStatus = Status;
    if (mHandoff.TypecSenseTicks != 0U) {
      mHandoff.TypecSenseTicks = 0U;
      if (mHandoff.SenseOwnedMask == 0U) mHandoff.TypecRetryTicks = USB_RETRY_TICKS;
      TYPEC_EXIT(3);
    }
    // A failed or occupied read is no evidence about the powered attachment.
    if (Contended && mHandoff.HostBusConnected && mHandoff.TypecVbus) TYPEC_EXIT(4);
    if (!mHandoff.OtgBound) {
      mHandoff.TypecRetryTicks = USB_RETRY_TICKS;
      TYPEC_EXIT(5);
    }
    if (++mHandoff.TypecReadFailures < USB_TYPEC_HOLD_TICKS) TYPEC_EXIT(6);
    mHandoff.TypecReadFailures = USB_TYPEC_HOLD_TICKS;
    mHandoff.TypecReleasing = TRUE;
  } else {
    mHandoff.TypecReadFailures = 0U;
  }
  if (mHandoff.RetainedTypecSource) {
    if (RetainedSourceLive(&Sample)) {
      mHandoff.TypecVbus = TRUE;
      mHandoff.TypecStatus = EFI_SUCCESS;
      TYPEC_EXIT(7);
    }
    mHandoff.RetainedTypecSource = FALSE;
    mHandoff.TypecVbus = FALSE;
  }
  if (mHandoff.ColdTypecToken != 0U && !mHandoff.TypecReleasing) {
    BOOLEAN Attached = TypecSameAttachment(&Sample) && (Sample.Power & 0x48U) == 0x08U;
    if (Attached && mHandoff.OtgBound && mHandoff.TypecVbus && (Sample.Power & 0x04U) == 0U) {
      // VBUS dipped while the same sink stays attached. Keep sourcing it.
      if (++mHandoff.TypecLossTicks < USB_TYPEC_HOLD_TICKS) TYPEC_EXIT(8);
    } else if (!Attached && mHandoff.OtgBound && mHandoff.TypecVbus) {
      // CC can bounce while a second device is plugged in; debounce detach.
      if (++mHandoff.TypecLossTicks < USB_TYPEC_HOLD_TICKS) TYPEC_EXIT(9);
    } else {
      mHandoff.TypecLossTicks = 0U;
    }
    if (Attached && mHandoff.TypecLossTicks == 0U) {
      if (!mHandoff.OtgBound) {
        if (!CanEnableSource) TYPEC_EXIT(10);
        Status = SetTypecPower();
        if (!EFI_ERROR(Status)) {
          mHandoff.TypecPowerTicks = USB_SOURCE_WAIT_TICKS;
          TYPEC_EXIT(11);
        }
        mHandoff.TypecStatus = Status;
      } else if ((Sample.Power & 0x04U) != 0U) {
        mHandoff.TypecVbus = TRUE;
        mHandoff.TypecStatus = EFI_SUCCESS;
        TYPEC_EXIT(12);
      } else if (!mHandoff.TypecVbus && mHandoff.TypecPowerTicks != 0U) {
        --mHandoff.TypecPowerTicks;
        TYPEC_EXIT(13);
      }
    }
    mHandoff.TypecReleasing = TRUE;
  }
  if (mHandoff.TypecReleasing) {
    mHandoff.TypecStatus = ReleaseTypecRoute();
    TYPEC_EXIT(14);
  }
  if (mHandoff.TypecSenseTicks != 0U) {
    Status = PollPassiveVbusSense(Sample.Power);
    if (EFI_ERROR(Status)) {
      mHandoff.TypecStatus = Status;
      TYPEC_EXIT(15);
    }
  }
  if (!mHandoff.ColdTypecBound && ((Sample.Power & 0x4eU) == 0U ||
      TypecHasOpenVconn(Sample.Power, Sample.Cc, Sample.Role))) {
    Status = WakeSettledTypecSense();
    mHandoff.TypecStatus = EFI_ERROR(Status) ? Status : EFI_NOT_READY;
    if (EFI_ERROR(Status) && mHandoff.SenseOwnedMask == 0U)
      mHandoff.TypecRetryTicks = USB_RETRY_TICKS;
    TYPEC_EXIT(16);
  }
  // Never advertise as a source against incoming VBUS. The pogo path can
  // continue as host while the receptacle is a sink.
  if ((Sample.Power & 0x4eU) != 0x08U) {
    if (mHandoff.ColdTypecBound) mHandoff.TypecReleasing = TRUE;
    TYPEC_EXIT(17);
  }
  // Poll even with open CC so the probe can restore the initial role.
  Status = PrepareColdTypec();
  if (Status != EFI_NOT_READY || !mHandoff.ColdProbePending) mHandoff.TypecStatus = Status;
  if (!mHandoff.ColdProbePending && EFI_ERROR(Status)) {
    mHandoff.TypecReleasing = mHandoff.ColdTypecBound;
    mHandoff.TypecRetryTicks = USB_RETRY_TICKS;
  }
  Exit = 18U;
Out:
  TypecDiagWrite(Exit, Sample.Role, Sample.Cc, Sample.Power, SampleValid);
}
#undef TYPEC_EXIT

STATIC VOID SetHostPowerPath (BOOLEAN HubEnabled, BOOLEAN IdLowPower) {
  // PC mode is host-first. Pogo and Type-C sit behind one onboard hub, so a
  // connector that is absent, unpowered or unreadable never gates the host.
  (VOID)HubEnabled;
  (VOID)IdLowPower;
  mHandoff.HostFirst = TRUE;
  mHandoff.TypecOnly = FALSE;
  mHandoff.IdLowBound = FALSE;
  mHandoff.IdLowRoleToken = 0U;
  if (mHandoff.ReadyInstalled) {
    mHandoff.Ready.HostPowerPath = MTK_MUSB_PLATFORM_POWER_PATH_HOST_FIRST;
    mHandoff.Ready.Flags = MTK_MUSB_PLATFORM_READY_HOST_FIRST_REQUIRED;
    MemoryFence();
  }
}

// Used only to take the controller over from LK. The regular accessors admit
// single proven transitions; a stuck inherited state needs all of them.
STATIC int RawRead8 (VOID *C,UINT32 O,UINT8 *V) { return Read8(C,O,V); }
STATIC int RawRead16 (VOID *C,UINT32 O,UINT16 *V) { return Read16(C,O,V); }
STATIC int RawRead32 (VOID *C,UINT32 O,UINT32 *V) { return Read32(C,O,V); }
STATIC int RawWrite8 (VOID *C,UINT32 O,UINT8 V) {
  if(C!=&mHandoff||mHandoff.HostBusConnected||mHandoff.ExitingBootServices) return 0;
  MmioWrite8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,V); return 1;
}
STATIC int RawWrite16 (VOID *C,UINT32 O,UINT16 V) {
  if(C!=&mHandoff||mHandoff.HostBusConnected||mHandoff.ExitingBootServices) return 0;
  MmioWrite16((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,V); return 1;
}
STATIC int RawWrite32 (VOID *C,UINT32 O,UINT32 V) {
  if(C!=&mHandoff||mHandoff.HostBusConnected||mHandoff.ExitingBootServices) return 0;
  MmioWrite32((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+O,V); return 1;
}
STATIC EFI_STATUS TakeOverController (CONST VOID *F, BOOLEAN StopSession) {
  MTK_MUSB_PLATFORM_EVIDENCE Evidence = {0};
  MTK_MUSB_PLATFORM_IO Io = mHandoff.Io;
  EFI_STATUS Status = ReadClockEvidence(F, &Evidence);
  UINT8 Devctl;

  // The cold start binds the clocks itself before it touches the controller.
  if (EFI_ERROR(Status)) return Status;
  Io.Read8 = RawRead8; Io.Read16 = RawRead16; Io.Read32 = RawRead32;
  Io.Write8 = RawWrite8; Io.Write16 = RawWrite16; Io.Write32 = RawWrite32;
  if (StopSession) {
    Devctl = MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE + 0x60U);
    MmioWrite8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE + 0x60U,
               (UINT8)(Devctl & ~MUSB_DEVCTL_SESSION));
  }
  return MtkMusbPlatformTakeOverHost(&Io) == MtkMusbPlatformSuccess ?
    EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS AdoptRetainedHost (
  CONST VOID *F, BOOLEAN HubEnabled, BOOLEAN IdLowPower) {
  MTK_MUSB_PLATFORM_EVIDENCE Evidence;
  MTK_MUSB_PLATFORM_READY_RESULT Ready = {0};
  MTK_MUSB_PLATFORM_STATUS PlatformStatus;
  T70_TYPEC_SAMPLE Sample;
  EFI_STATUS Status;

  Status = mHandoff.EndOfDxeSeen ? SampleTypec(&Sample, NULL) : EFI_NOT_READY;
  if (!EFI_ERROR(Status) && RetainedSourceLive(&Sample)) {
    // Preserve a source that firmware already left running only when neither
    // the charger nor HUSB311 route is owned by this DXE instance.
    if (!mHandoff.OtgBound && !mHandoff.ColdTypecBound)
      mHandoff.RetainedTypecSource = TRUE;
    mHandoff.TypecVbus = TRUE;
    mHandoff.RouteCcPin = ((Sample.Cc & 3U) == 2U) ? 1U : 2U;
  }
  mHandoff.ColdOwnerToken = 1U;
  SetHostPowerPath(HubEnabled, IdLowPower);
  Status = CollectEvidence(FALSE, &Evidence);
  if (EFI_ERROR(Status)) {
    Status = BusFailure(0x6201U, Status);
    goto Done;
  }

  mHandoff.AllowAckWrites = TRUE;
  PlatformStatus = MtkMusbPlatformAcknowledgeIdleInterruptsWithDiagnostic(
                     &mHandoff.Io, &mHandoff.DiagnosticIdle);
  mHandoff.AllowAckWrites = FALSE;
  if (PlatformStatus != MtkMusbPlatformSuccess) {
    Status = BusFailure(0x6202U,
      PlatformStatus == MtkMusbPlatformIoError ? EFI_DEVICE_ERROR : EFI_NOT_READY);
    if (++mHandoff.AdoptTicks >= 5U && !EFI_ERROR(TakeOverController(F, FALSE)))
      mHandoff.AdoptTicks = 0U;
    goto Done;
  }

  PlatformStatus = MtkMusbPlatformEvaluateReady(&mHandoff.Io, &Evidence, &Ready);
  if ((PlatformStatus == MtkMusbPlatformNotReady) &&
      (Ready.MissingFlags == MTK_MUSB_PLATFORM_READY_FIFO)) {
    mHandoff.AllowFifoWrites = TRUE;
    PlatformStatus = MtkMusbPlatformConfigureInheritedHostFifo(&mHandoff.Io, &Evidence);
    mHandoff.AllowFifoWrites = FALSE;
    if (PlatformStatus == MtkMusbPlatformSuccess)
      PlatformStatus = MtkMusbPlatformEvaluateReady(&mHandoff.Io, &Evidence, &Ready);
  }
  if ((PlatformStatus != MtkMusbPlatformSuccess) || !ReadyResultComplete(&Ready)) {
    Status = BusFailure((UINT16)(0x6200U | (Ready.MissingFlags & 0xffU)),
      PlatformStatus == MtkMusbPlatformIoError ? EFI_DEVICE_ERROR : EFI_NOT_READY);
    goto Done;
  }

  Status = PublishReadyHost(&Evidence, &Ready);
  if (EFI_ERROR(Status)) {
    Status = BusFailure(0x6280U, Status);
    goto Done;
  }
  mHandoff.RetainedHostPending = FALSE;
  mHandoff.RetainedHostOwned = TRUE;
  return EFI_SUCCESS;

Done:
  if (!mHandoff.RetainedHostOwned) {
    mHandoff.ColdOwnerToken = 0U;
    mHandoff.IdLowRoleToken = 0U;
  }
  return Status;
}

STATIC EFI_STATUS StartBoardHost (
  CONST VOID *F, UINTN Bytes, BOOLEAN HubEnabled, BOOLEAN IdLowPower) {
  MTK_MUSB_PLATFORM_EVIDENCE Evidence;
  MTK_MUSB_PLATFORM_READY_RESULT Ready;
  MTK_MUSB_PLATFORM_STATUS PlatformStatus;
  EFI_STATUS Status;
  UINT16 Step = 0x6101U;

  if (mHandoff.RetainedHostPending || mHandoff.RetainedHostOwned)
    return AdoptRetainedHost(F, HubEnabled, IdLowPower);
  mHandoff.ColdInProgress = TRUE;
  Status = EFI_SUCCESS;
  if (!mHandoff.ColdBound) {
    mHandoff.ColdBound = TRUE;
    mHandoff.ColdOwnerToken = 1U;
    mHandoff.ColdSampleSequence = 0U;
    SetHostPowerPath(HubEnabled, IdLowPower);
    Status = BindColdHost(F, Bytes, &Step);
    if (!EFI_ERROR(Status)) Status = PowerBoundHost(&Step);
  }
  if (!EFI_ERROR(Status)) {
    Step = mHandoff.IdLowBound ? 0x610bU : 0x600bU;
    Status = StartColdController();
  }
  if (Status == EFI_NOT_READY && mHandoff.ColdHost.Kind == MtkMusbPlatformColdStateStarting) {
    mHandoff.ColdInProgress = FALSE;
    return EFI_NOT_READY;
  }
  if (!EFI_ERROR(Status)) {
    Step = 0x6110U;
    Status = CollectEvidence(FALSE, &Evidence);
    if (!EFI_ERROR(Status)) {
      PlatformStatus = MtkMusbPlatformEvaluateReady(&mHandoff.Io, &Evidence, &Ready);
      // LK or the MUSB reset can leave a FIFO layout that only fits device mode.
      // Reprogram it while the new host is idle, before the USB driver binds.
      if (PlatformStatus == MtkMusbPlatformNotReady &&
          Ready.MissingFlags == MTK_MUSB_PLATFORM_READY_FIFO) {
        mHandoff.AllowFifoWrites = TRUE;
        PlatformStatus = MtkMusbPlatformConfigureInheritedHostFifo(&mHandoff.Io, &Evidence);
        mHandoff.AllowFifoWrites = FALSE;
        if (PlatformStatus == MtkMusbPlatformSuccess)
          PlatformStatus = MtkMusbPlatformEvaluateReady(&mHandoff.Io, &Evidence, &Ready);
      }
      if (PlatformStatus != MtkMusbPlatformSuccess || !ReadyResultComplete(&Ready))
        Status = EFI_NOT_READY;
    }
  }
  mHandoff.ColdInProgress = FALSE;
  if (!EFI_ERROR(Status)) Status = PublishReadyHost(&Evidence, &Ready);
  if (EFI_ERROR(Status)) {
    mHandoff.DiagnosticPreparationStep = Step;
    return BusFailure(Step, Status);
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS SettleInheritedDevice (CONST VOID *F) {
  MTK_MUSB_PLATFORM_EVIDENCE Evidence = {0};
  EFI_STATUS Status;
  UINT8 Devctl, Power;

  if (mHandoff.DeviceSettled) return EFI_SUCCESS;
  if (mHandoff.DeviceDisconnectComplete) {
    mHandoff.DeviceRolePhase = UsbDeviceRoleStable;
    // The inherited-device path has already observed sensing enabled.
    mHandoff.SenseOwnedMask = 0U;
    mHandoff.DeviceSettled = TRUE;
    return EFI_SUCCESS;
  }
  if (!mHandoff.SenseAttempted && DeferTypecSample()) return EFI_NOT_READY;
  Status = ReadClockEvidence(F, &Evidence);
  if (Status != EFI_NOT_STARTED) {
    if (EFI_ERROR(Status)) return Status;
    Status = ValidateGcd(MTK_MUSB_PLATFORM_CONTROLLER_BASE, MTK_MUSB_PLATFORM_CONTROLLER_BYTES);
    if (EFI_ERROR(Status)) return Status;
    Devctl = MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE + 0x60U);
    Power = MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE + 1U);
    mHandoff.DiagnosticDevctl = Devctl;
    if ((Devctl & MUSB_RETAINED_HOST_MASK) == MUSB_RETAINED_HOST_READY) {
      mHandoff.RetainedHostPending = TRUE;
      mHandoff.DeviceSettled = TRUE;
      return EFI_SUCCESS;
    }
    if ((Devctl & 0x84U) == 0x80U && ((Devctl & 1U) != 0U || (Power & 0x40U) != 0U))
      return DisconnectInheritedDevice(F);
    Status = SettlePassiveDevice(Devctl, Power);
    if (EFI_ERROR(Status)) return Status;
  }
  // If LK left the controller clock off, the cold host binder checks idle
  // after taking the clocks, before any MUSB or PHY write.
  mHandoff.DeviceSettled = TRUE;
  return EFI_SUCCESS;
}

STATIC VOID EFIAPI EndOfDxeNotify (EFI_EVENT Event, VOID *Context) {
  (VOID)Event;
  (VOID)Context;
  mHandoff.EndOfDxeObserved = TRUE;
}

STATIC EFI_STATUS PreparePowerBus (CONST VOID *F, UINTN Bytes) {
  EFI_HANDLE Controller;
  MTK_I2C_CONTROLLER *Meta;
  EFI_I2C_MASTER_PROTOCOL *Master;
  EFI_STATUS Status = ValidateBoardTree(F, Bytes);
  if (EFI_ERROR(Status)) return Status;
  Status = FindController(&Controller, &Meta);
  if (EFI_ERROR(Status)) return Status;
  Status = ValidateI2cLive();
  if (EFI_ERROR(Status)) return Status;
  if (EFI_ERROR(ValidateI2cMaster(Controller, &Meta, &Master)))
    (VOID)gBS->ConnectController(Controller, NULL, NULL, FALSE);
  Status = EnsureBoardReady(F, Bytes);
  mHandoff.EndOfDxeSeen = !EFI_ERROR(Status);
  return Status;
}

STATIC VOID EFIAPI UsbRoleTick (EFI_EVENT Event, VOID *Context) {
  CONST VOID *F;
  UINTN Bytes;
  UINT8 Id;
  EFI_STATUS Status;
  BOOLEAN HubEnabled;
  BOOLEAN IdLowPower;
  (VOID)Event;

  if (Context != &mHandoff || !mHandoff.EndOfDxeObserved || mHandoff.BootServicesLeaving ||
      mHandoff.ExitingBootServices || mHandoff.ConnectInProgress ||
      mHandoff.ColdInProgress) return;
  if (mHandoff.BoardRetryTicks != 0U) {
    --mHandoff.BoardRetryTicks;
    return;
  }
  Status = GetRuntimeFdt(&F, &Bytes);
  if (EFI_ERROR(Status)) {
    UsbWait(UsbDiagFdt, Status);
    mHandoff.BoardRetryTicks = USB_RETRY_TICKS;
    return;
  }
  if (!mHandoff.EndOfDxeSeen) {
    // Only Type-C status and source power need I2C6. The hub host does not.
    if (mHandoff.PowerBusRetryTicks != 0U) {
      --mHandoff.PowerBusRetryTicks;
    } else {
      Status = PreparePowerBus(F, Bytes);
      if (EFI_ERROR(Status)) {
        UsbWait(UsbDiagPowerBus, Status);
        mHandoff.PowerBusRetryTicks = USB_RETRY_TICKS;
      }
    }
  }
  if (EFI_ERROR(ReadBoardId(&Id))) Id = 1U;
  mHandoff.DiagnosticId = Id;
  // Require two consecutive observations without sleeping in this callback.
  if (mHandoff.LastId != Id) {
    mHandoff.LastId = Id;
    return;
  }
  if (mHandoff.HostBusConnected) {
    // The pin snapshot is only read here. If it failed before the hub host
    // came up, retry it; otherwise the receptacle never gets source power.
    if (!mHandoff.ColdPinBound && !EFI_ERROR(BindBoardPins())) mDiagBind[2] = 1U;
    // Keep the onboard hub and host controller bound while allowing
    // independent Type-C OTG attach/detach alongside pogo.
    if (mHandoff.EndOfDxeSeen && mHandoff.ColdPinBound) StepTypec(TRUE);
    else TypecDiagWrite(30U, 0U, 0U, 0U, FALSE);
    return;
  }
  if (!mHandoff.DeviceSettled) {
    Status = SettleInheritedDevice(F);
    UsbWait(UsbDiagDeviceHandoff, Status);
    if (EFI_ERROR(Status)) {
      if (++mHandoff.SettleTicks < USB_SETTLE_TICKS) return;
      // Do not wait forever for LK's leftover peripheral state to go idle.
      Status = TakeOverController(F, TRUE);
      if (EFI_ERROR(Status)) {
        mHandoff.SettleTicks = USB_SETTLE_TICKS - USB_RETRY_TICKS;
        return;
      }
      mHandoff.DeviceRolePhase = UsbDeviceRoleStable;
      mHandoff.DeviceSettled = TRUE;
    }
  }
  // PC mode is host-first. The pogo contacts are downstream of the onboard
  // hub, so keep that hub enabled and let UsbBus handle device hotplug.
  HubEnabled = TRUE;
  IdLowPower = Id == 0U;
  if (!mHandoff.RouteRetiring) {
    Status = BindBoardPins();
    if (!EFI_ERROR(Status)) Status = SetPogoRoute(HubEnabled);
    if (EFI_ERROR(Status)) {
      UsbWait(UsbDiagIdWait, BusFailure(0x6105U, Status));
      if (++mHandoff.RouteTicks < USB_RETRY_TICKS) return;
      mHandoff.RouteTicks = USB_RETRY_TICKS;
    }
  }
  if (mHandoff.EndOfDxeSeen && mHandoff.ColdPinBound) StepTypec(!mHandoff.RouteRetiring);
  else TypecDiagWrite(31U, 0U, 0U, 0U, FALSE);
  if (mHandoff.RouteRetiring) {
    mHandoff.ConnectInProgress = TRUE;
    Status = StopBoardHost();
    mHandoff.ConnectInProgress = FALSE;
    mHandoff.RouteRetiring = EFI_ERROR(Status);
    if (EFI_ERROR(Status)) {
      UsbWait(UsbDiagRouteCleanupFailed, Status);
      mHandoff.BoardRetryTicks = USB_RETRY_TICKS;
    } else if (!mHandoff.PreparationFailed) {
      UsbWait(UsbDiagEvidence, Status);
    }
    return;
  }
  if (mHandoff.HostRetryTicks != 0U) {
    --mHandoff.HostRetryTicks;
    return;
  }
  if (mHandoff.ReadyInstalled) {
    SetHostPowerPath(HubEnabled, IdLowPower);
    // Bind only the controller. UsbBusDxe queues enumeration of its ports.
    Status = EFI_SUCCESS;
    if (!ReadyBusBound()) {
      mHandoff.ConnectInProgress = TRUE;
      Status = gBS->ConnectController(mHandoff.ReadyHandle, NULL, NULL, FALSE);
      mHandoff.ConnectInProgress = FALSE;
    }
    // ConnectController can succeed if any driver started. Check the USB
    // bus binding as well, and retry it on the already powered controller.
    mHandoff.HostBusConnected = !EFI_ERROR(Status) && ReadyHostComplete() && ReadyBusBound();
    mHandoff.PreparationFailed = !mHandoff.HostBusConnected;
    if (mHandoff.HostBusConnected) {
      UsbWait(UsbDiagHostConnected, EFI_SUCCESS);
    } else {
      mHandoff.HostRetryTicks = USB_RETRY_TICKS;
      UsbWait(UsbDiagHostConnectFailed, EFI_ERROR(Status) ? Status : EFI_NOT_READY);
    }
    return;
  }
  if (!mHandoff.ColdBound) mHandoff.DiagnosticBusStep = 0U;
  Status = StartBoardHost(F, Bytes, HubEnabled, IdLowPower);
  if ((mHandoff.RetainedHostPending || mHandoff.RetainedHostOwned) &&
      Status == EFI_NOT_READY) {
    mHandoff.HostRetryTicks = USB_RETRY_TICKS;
    UsbWait(mHandoff.DiagnosticBusStep == 0x6202U ?
      UsbDiagActiveSession : UsbDiagEvidence, Status);
    return;
  }
  if (!EFI_ERROR(Status)) mHandoff.PreparationFailed = FALSE;
  // Let the menu run before driver binding, even when host setup finished.
  if (!EFI_ERROR(Status) || mHandoff.ColdHost.Kind == MtkMusbPlatformColdStateStarting)
    return;

  mHandoff.PreparationFailed = TRUE;
  mHandoff.RouteRetiring = mHandoff.ColdBound || mHandoff.RetainedHostOwned;
  mHandoff.HostRetryTicks = USB_RETRY_TICKS;
  UsbWait(UsbDiagPreparationFailed, Status);
}

STATIC EFI_STATUS FinishTypecCleanupBeforeExit (VOID) {
  EFI_STATUS Status = ReleaseTypecRoute();
  // No more role ticks will run. Wait between samples with the bus released.
  for (UINTN Tick = 0U; Status == EFI_NOT_READY &&
       Tick < MTK_HUSB311_TYPEC_TX_QUIESCENCE_TIMEOUT_MS /
              MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS + USB_SOURCE_WAIT_TICKS; ++Tick) {
    if (mHandoff.I2cToken != 0U) return EFI_ACCESS_DENIED;
    MicroSecondDelay(MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS * 1000U);
    Status = ReleaseTypecRoute();
  }
  return Status;
}

STATIC MTK_MUSB_PLATFORM_STATUS AcknowledgeHostIdleInterrupts (VOID) {
  MTK_MUSB_PLATFORM_STATUS Status;

  mHandoff.AllowAckWrites = TRUE;
  Status = MtkMusbPlatformAcknowledgeIdleInterrupts(&mHandoff.Io);
  mHandoff.AllowAckWrites = FALSE;
  return Status;
}

STATIC VOID EFIAPI BeforeExitNotify (EFI_EVENT Event, VOID *Context) {
  EFI_STATUS Status;
  EFI_STATUS PowerStatus;

  if (Context != &mHandoff || Event != mHandoff.BeforeExitEvent) return;
  mHandoff.BootServicesLeaving = TRUE;
  if (mHandoff.RetryTimerEvent)
    (VOID)gBS->SetTimer(mHandoff.RetryTimerEvent, TimerCancel, 0U);

  // The OS loader has been using this host up to now. A sideband sample that
  // misses a profile value is no detach event, so the controller, PHY, hub and
  // VBUS of a connected host always stay up for the operating system.
  if (mHandoff.ReadyInstalled && mHandoff.HostBusConnected) return;

  // Nothing was attached to the USB bus driver. Stop an unpublished or
  // partially started host and return firmware-owned source power.
  mHandoff.RouteRetiring = TRUE;
  mHandoff.ConnectInProgress = TRUE;
  Status = StopBoardHost();
  mHandoff.ConnectInProgress = FALSE;
  PowerStatus = FinishTypecCleanupBeforeExit();
  if (!EFI_ERROR(Status)) Status = PowerStatus;
  if (EFI_ERROR(Status)) UsbWait(UsbDiagRouteCleanupFailed, Status);
}

STATIC BOOLEAN CaptureTcpcBeforeI2cExit (VOID) {
  UINT8 Id[2],Power,Cc,Role,Check;
  if(EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0U,Id,2U))||
     Id[0]!=0x99U||Id[1]!=0x2eU||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,2U,Id,2U))||
     Id[0]!=0x11U||Id[1]!=0x03U||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1dU,&Cc,1U))||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1aU,&Role,1U))||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1eU,&Power,1U))||
     EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1dU,&Check,1U))||Cc!=Check)
    return FALSE;
  if(mHandoff.ColdBound&&mHandoff.ColdTypecBound&&!mHandoff.IdLowBound&&!mHandoff.RouteRetiring) {
    UINT8 Rx,Header,Alert[2],VendorAlert,TcpcControl,PowerControl,Fault;
    CONST MTK_HUSB311_TYPEC_CORE *T=&mHandoff.ColdTypecCore;
    if(!MtkHusb311TypecTokenActive(T,mHandoff.ColdTypecToken)||
       Role!=T->ExpectedRoleControl||Role!=0x05U||
       !MtkMusbPlatformHusb311SourceAttached(Cc,Role)||
       (((Cc&3U)==2U)?1U:2U)!=T->AttachedCcPin||
       EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x2fU,&Rx,1U))||Rx!=0U||
       EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x2eU,&Header,1U))||
       Header!=T->ExpectedMessageHeader||(Header&0x09U)!=0x09U||
       EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x10U,Alert,2U))||
       (((UINT16)Alert[0]|((UINT16)Alert[1]<<8))&~0x0073U)!=0U||
       (Alert[0]&0x70U)!=T->TxResultBaseline||
       EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x98U,&VendorAlert,1U))||VendorAlert!=0U||
       EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x19U,&TcpcControl,1U))||(TcpcControl&0x02U)||
       EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1cU,&PowerControl,1U))||(PowerControl&0x01U)||
       EFI_ERROR(ReadPowerRegister(TCPC_ADDRESS,0x1fU,&Fault,1U))||Fault!=0U)
      return FALSE;
  }
  mHandoff.HusbPowerStatus=Power;
  ZeroMem(&mHandoff.HusbCcStatus,sizeof(mHandoff.HusbCcStatus));
  mHandoff.HusbCcStatus.CcStatus=Cc; mHandoff.HusbCcStatus.RoleControl=Role;
  mHandoff.HusbCcStatus.CcStatusBit5=(Cc>>5)&1U;
  mHandoff.HusbCcStatus.RoleControlWasRead=1U; // Both raw registers were explicitly read above.
  MemoryFence(); mHandoff.EbsPowerSampleValid=TRUE;
  return TRUE;
}

STATIC VOID EFIAPI CapturePowerBeforeI2cExit (EFI_EVENT Event,VOID *Context) {
  UINT8 ScId,Control;
  (VOID)Event;
  if(Context!=&mHandoff||mHandoff.EbsPowerCaptured) return;
  mHandoff.ExitingBootServices=TRUE; mHandoff.EbsPowerCaptured=TRUE;
  mHandoff.EbsPowerSampleValid=FALSE;
  mHandoff.EbsScBoostSampleValid=FALSE;
  if(!mHandoff.BoardReadyInstalled||!mHandoff.I2cExitObserverRegistered||
     mHandoff.OtgBusy||mHandoff.ColdInProgress) return;
  if(EFI_ERROR(AcquirePowerBus())) return;
  // The observer can take fresh read-only samples before the controller
  // quiesces. Keep the boost result independent for a valid pogo handoff.
  for (UINTN Attempt = 0; Attempt < 2U; ++Attempt) {
    if(EFI_ERROR(ReadPowerRegister(SC_ADDRESS,0x14U,&ScId,1U))||
       (0x94U&(1U<<((ScId>>3)&7U)))==0U||
       EFI_ERROR(ReadPowerRegister(SC_ADDRESS,3U,&Control,1U)))
      continue;
    mHandoff.ScBoostEnabled=(Control&0x20U)!=0U;
    MemoryFence(); mHandoff.EbsScBoostSampleValid=TRUE;
    break;
  }
  if (mHandoff.EbsScBoostSampleValid && !CaptureTcpcBeforeI2cExit())
    (VOID)CaptureTcpcBeforeI2cExit();
  if(EFI_ERROR(ReleasePowerBus())) {
    mHandoff.EbsPowerSampleValid=FALSE;
    mHandoff.EbsScBoostSampleValid=FALSE;
  }
}
STATIC VOID EFIAPI EbsPowerNotify (EFI_EVENT E,VOID *C) {
  (VOID)E;(VOID)C; mHandoff.ExitingBootServices=TRUE;
  if(mHandoff.RetryTimerEvent)
    (VOID)gBS->SetTimer(mHandoff.RetryTimerEvent,TimerCancel,0);
  // The I2C driver's observer owns the final sample. Independent event order
  // must neither reread a stopped bus nor erase a sample it already captured.
}
STATIC VOID InvalidateRecord (VOID) {
  MmioWrite32 (HANDOFF_RECORD, 0);
  MemoryFence(); WriteBackDataCacheRange((VOID *)(UINTN)HANDOFF_RECORD,HANDOFF_BYTES);
}
STATIC VOID PublishRecord (CONST MTK_MUSB_PLATFORM_READY_RESULT *R) {
  CONST UINT32 Words[] = {
    0, HANDOFF_REVISION, R->Flags,
    (UINT32)MTK_MUSB_PLATFORM_CONTROLLER_BASE, (UINT32)MTK_MUSB_PLATFORM_CONTROLLER_BYTES,
    (UINT32)MTK_MUSB_PLATFORM_PHY_BASE, (UINT32)MTK_MUSB_PLATFORM_PHY_BYTES,
    MTK_MUSB_PLATFORM_GSIV, MTK_MUSB_PLATFORM_ENDPOINTS, MTK_MUSB_PLATFORM_PORTS,
    MTK_MUSB_PLATFORM_FIFO_PROFILE_T70_V1, MTK_MUSB_PLATFORM_EVENTS,
    R->HostPortStatus, R->HostPortChangeStatus, R->HostPortSnapshotValid, R->HostPowerPath
  };
  for (UINTN Index = 0; Index < ARRAY_SIZE (Words); ++Index) {
    MmioWrite32 (HANDOFF_RECORD + Index * sizeof (UINT32), Words[Index]);
  }
  MemoryFence(); WriteBackDataCacheRange((VOID *)(UINTN)HANDOFF_RECORD,HANDOFF_BYTES);
  MemoryFence(); MmioWrite32 (HANDOFF_RECORD, 1); MemoryFence();
  WriteBackDataCacheRange((VOID *)(UINTN)HANDOFF_RECORD,sizeof(UINT32));
}
STATIC VOID EFIAPI EbsPublishNotify (EFI_EVENT E,VOID *C) {
  MTK_MUSB_PLATFORM_READY_RESULT R;
  UINT8 Devctl;
  (VOID)E;(VOID)C;
  if(mHandoff.EbsFinalized) return;
  mHandoff.EbsFinalized=TRUE; InvalidateRecord();
  if(!mHandoff.ReadyInstalled||!mHandoff.HostBusConnected) return;

  // Retire completion flags of the stopped host when that is possible. The
  // operating system quiesces the endpoints itself when it takes over.
  (VOID)AcknowledgeHostIdleInterrupts();
  ZeroMem(&R,sizeof(R));
  R.HostPowerPath=mHandoff.IdLowBound?MTK_MUSB_PLATFORM_POWER_PATH_ID_LOW_HUB:
    mHandoff.TypecOnly?MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_ONLY:
                       MTK_MUSB_PLATFORM_POWER_PATH_TYPEC_SOURCE;
  R.Flags=mHandoff.IdLowBound?MTK_MUSB_PLATFORM_READY_ID_LOW_REQUIRED:
                              MTK_MUSB_PLATFORM_READY_REQUIRED;
  R.HostPortSnapshotValid=1U;
  MemoryFence();
  if(mHandoff.Ready.HostSnapshotValid==1U) {
    R.HostPortStatus=mHandoff.Ready.HostPortStatus;
    R.HostPortChangeStatus=mHandoff.Ready.HostPortChangeStatus;
  }
  // Without a snapshot from the host driver, report the live root port.
  if((R.HostPortStatus&0x100U)==0U||(R.HostPortStatus&~0x0703U)!=0U||
     (R.HostPortStatus&0x600U)==0x600U||
     ((R.HostPortStatus&1U)==0U&&R.HostPortStatus!=0x100U)) {
    Devctl=MmioRead8((UINTN)MTK_MUSB_PLATFORM_CONTROLLER_BASE+0x60U);
    R.HostPortStatus=0x100U;
    if((Devctl&0x60U)!=0U) R.HostPortStatus|=1U;
    if((Devctl&0x60U)==0x20U) R.HostPortStatus|=0x200U;
  }
  R.HostPortChangeStatus&=0x1fU;
  PublishRecord(&R);
}

// Copy recorded preparation state only; inspection never retries power writes.
STATIC EFI_STATUS EFIAPI ReadHostDiagnostics (T70_USB_HOST_SNAPSHOT *D) {
  CONST HUSB_SENSE_FAILURE *Sense;
  EFI_TPL Old;
  if (D == NULL) return EFI_INVALID_PARAMETER;
  Old = gBS->RaiseTPL (TPL_NOTIFY);
  ZeroMem (D, sizeof (*D));
  D->Status = mHandoff.DiagnosticStatus;
  D->Reason = mHandoff.DiagnosticReason; D->Failed = mHandoff.PreparationFailed;
  D->BusStep = D->Reason == UsbDiagPreparationFailed ?
    mHandoff.DiagnosticPreparationStep : mHandoff.DiagnosticBusStep;
  D->Ready = mHandoff.ReadyInstalled; D->Connected = mHandoff.HostBusConnected;
  D->Devctl = mHandoff.DiagnosticDevctl; D->Id = mHandoff.DiagnosticId;
  if (D->Failed && mHandoff.DiagnosticColdHost.Valid) {
    CONST COLD_HOST_FAILURE *Cold = &mHandoff.DiagnosticColdHost;
    D->ColdStep = (UINT8)Cold->State.StartStep;
    D->ColdGuard = (UINT8)Cold->State.GuardReason;
    D->ColdPath = (UINT8)Cold->State.HostPowerPath;
    D->ColdReset = (UINT8)Cold->State.ResetCheck;
    D->ColdRegistersValid = Cold->ReadMask == 0x3fU;
    D->ColdDevctl = Cold->Devctl;
    D->ColdPower = Cold->Power;
    D->ColdDtm1 = Cold->Phy[3];
  }
  Sense = mHandoff.DiagnosticSenseFailure.Stage ?
    &mHandoff.DiagnosticSenseFailure : &mHandoff.DiagnosticSenseCleanup;
  D->SenseStage = Sense->Stage;
  D->SenseRegister = Sense->Register;
  D->SenseSaved = Sense->Saved;
  D->SenseExpected = Sense->Expected;
  D->SenseLive = Sense->Live;
  D->SenseStatus = Sense->Status;
  D->IdleOffset = mHandoff.DiagnosticIdle.Offset;
  if (D->IdleOffset != T70_USB_HOST_NO_IDLE_OFFSET) {
    D->IdleValue = mHandoff.DiagnosticIdle.Value;
    D->IdleMask = mHandoff.DiagnosticIdle.Mask;
    D->IdleEndpoint = mHandoff.DiagnosticIdle.Endpoint;
  }
  gBS->RestoreTPL (Old);
  return EFI_SUCCESS;
}
STATIC T70_USB_HOST_DIAGNOSTICS mHostDiagnostics = {T70_USB_HOST_DIAG_VERSION, ReadHostDiagnostics};
STATIC EFI_STATUS EFIAPI ReadTypecDiagnostics (
  MTK_HUSB311_TYPEC_DIAGNOSTIC *Failure, UINT8 *GuardStep
  ) {
  EFI_TPL Old;
  if (Failure == NULL || GuardStep == NULL) return EFI_INVALID_PARAMETER;
  Old = gBS->RaiseTPL (TPL_NOTIFY);
  *Failure = mHandoff.ColdTypecCore.FirstFailure;
  *GuardStep = mHandoff.DiagnosticTypecGuardStep;
  gBS->RestoreTPL (Old);
  return EFI_SUCCESS;
}
STATIC T70_USB_TYPEC_DIAGNOSTICS mTypecDiagnostics = {ReadTypecDiagnostics};
STATIC EFI_GUID mTypecDiagnosticsGuid = T70_USB_TYPEC_DIAGNOSTICS_GUID;
STATIC EFI_GUID mHostDiagnosticsGuid = T70_USB_HOST_DIAGNOSTICS_GUID;
STATIC EFI_HANDLE mHostDiagnosticsHandle;

EFI_STATUS EFIAPI MusbHandoffDxeEntryPoint(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS S,C; VOID *ReadyToLock;
  (VOID)ImageHandle;(VOID)SystemTable;
  if(mHandoff.Signature==DRIVER_SIGNATURE)
    return mHandoff.ImageMustRemainResident?EFI_SUCCESS:EFI_ALREADY_STARTED;
  ZeroMem(&mHandoff,sizeof(mHandoff)); mHandoff.Signature=DRIVER_SIGNATURE; SetupIo();
  mHandoff.DiagnosticIdle.Offset=T70_USB_HOST_NO_IDLE_OFFSET;
  mHandoff.LastId=0xffU;
  mHandoff.TypecStatus=EFI_NOT_READY;
  S=gBS->LocateProtocol(&gMediaTekClockProtocolGuid,NULL,(VOID **)&mHandoff.Clock);
  if(EFI_ERROR(S)||!mHandoff.Clock) return EFI_NOT_READY;
  S=gBS->LocateProtocol(&gMediaTekGpioProtocolGuid,NULL,(VOID **)&mHandoff.Gpio);
  if(EFI_ERROR(S)||!mHandoff.Gpio) return EFI_NOT_READY;

  // Notify blocks new preparation. The controller exit observer captures power
  // before stopping I2C; Callback combines that with the HCD host snapshot.
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,EbsPowerNotify,&mHandoff,
                       &gEfiEventExitBootServicesGuid,&mHandoff.ExitNotifyEvent);
  if(EFI_ERROR(S)) return S;
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,EbsPublishNotify,&mHandoff,
                       &gEfiEventExitBootServicesGuid,&mHandoff.ExitPublishEvent);
  if(EFI_ERROR(S)) {
    C=gBS->CloseEvent(mHandoff.ExitNotifyEvent);
    if(EFI_ERROR(C)) { mHandoff.ImageMustRemainResident=TRUE; return EFI_SUCCESS; }
    mHandoff.ExitNotifyEvent=NULL; return S;
  }
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,EndOfDxeNotify,&mHandoff,
                       &gEfiEndOfDxeEventGroupGuid,&mHandoff.EndOfDxeEvent);
  if(EFI_ERROR(S)) { mHandoff.ImageMustRemainResident=TRUE; return EFI_SUCCESS; }
  // A physical cold boot can satisfy the power-provider depex only after the
  // one-shot EndOfDxe event. ReadyToLock is installed immediately after that
  // event and distinguishes this late dispatch from a normal early dispatch.
  ReadyToLock=NULL;
  if(!EFI_ERROR(gBS->LocateProtocol(&gEfiDxeSmmReadyToLockProtocolGuid,
                                    NULL,&ReadyToLock)))
    mHandoff.EndOfDxeObserved=TRUE;
  S=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,UsbRoleTick,
                     &mHandoff,&mHandoff.RetryTimerEvent);
  if(!EFI_ERROR(S)) S=gBS->SetTimer(mHandoff.RetryTimerEvent,TimerPeriodic,USB_ROLE_PERIOD);
  if(EFI_ERROR(S)) {
    if(mHandoff.RetryTimerEvent&&!EFI_ERROR(gBS->CloseEvent(mHandoff.RetryTimerEvent)))
      mHandoff.RetryTimerEvent=NULL;
    mHandoff.ImageMustRemainResident=TRUE;
    return EFI_SUCCESS;
  }
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,BeforeExitNotify,&mHandoff,
                       &gEfiEventBeforeExitBootServicesGuid,&mHandoff.BeforeExitEvent);
  if(EFI_ERROR(S)) {
    mHandoff.ImageMustRemainResident=TRUE;
    return EFI_SUCCESS;
  }
  (VOID)gBS->InstallMultipleProtocolInterfaces (&mHostDiagnosticsHandle,
    &mHostDiagnosticsGuid, &mHostDiagnostics, &mTypecDiagnosticsGuid, &mTypecDiagnostics, NULL);
  return EFI_SUCCESS;
}
