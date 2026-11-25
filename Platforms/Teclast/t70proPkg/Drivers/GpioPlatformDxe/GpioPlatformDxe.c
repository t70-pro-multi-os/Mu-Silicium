// SPDX-License-Identifier: BSD-2-Clause-Patent
// Keep GPIO152 leased across the touch probe; readiness snapshots never change pad state.
#include <Uefi.h>
#include <PiDxe.h>
#include <Guid/Fdt.h>
#include <Guid/EventGroup.h>
#include <Protocol/I2cMaster.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/MtkI2cController.h>
#include <Protocol/MtkGpio.h>
#include <Protocol/MtkPanelBoard.h>
#include <Protocol/HimaxTouch.h>
#include <Library/HimaxCore.h>
#include <Library/MtkPanelCore.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/BaseLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FdtLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#define HANDOFF 0x43F7F090U
#define CHARGER_IRQ_HANDOFF 0x43F7F144U
static EFI_EVENT ExitEvent,TouchTimer;
static EFI_HANDLE Image,TouchHandle,ProbeOwned;
static const VOID *TouchFdt;
static UINTN TouchBytes;
static UINT32 TouchState,ProbeAttempts;
#define TOUCH_CONNECT_LIMIT 16U
static UINT32 TouchConnectAttempts,TouchConnectDelay,TouchConnectCountdown;
static BOOLEAN TouchConnectBusy;
static BOOLEAN Exiting;
static BOOLEAN TouchPinCleanupPending;
static BOOLEAN ProbePinLeaseHeld;
static HIMAX_TOUCH_BOARD TouchBoard;
static MTK_GPIO_PROTOCOL *TouchPinLeaseOwner;
static UINT64 TouchPinCleanupToken;
static MTK_GPIO_PROTOCOL *ProbePinLeaseOwner;
static UINT64 ProbePinLeaseToken;
static MTK_PANEL_CORE PanelCore;
static BOOLEAN PanelCoreInitialized;
static volatile UINT32 PanelCallBusy;
static MTK_GPIO_PROTOCOL *PanelPinOwner;
static UINT64 PanelLeaseToken;
static BOOLEAN PanelLeaseHeld,PanelLeaseUncertain;
static MTK_PANEL_OPERATION PanelLeaseOperation;
static VOID *PanelFdt;
static UINTN PanelFdtBytes;
static EFI_HANDLE PanelHandle;
static EFI_STATUS EFIAPI PanelRunInit(MTK_PANEL_BOARD_PROTOCOL *This);
static EFI_STATUS EFIAPI PanelRunSuspend(MTK_PANEL_BOARD_PROTOCOL *This);
static EFI_STATUS EFIAPI PanelRunKernelPrepare(MTK_PANEL_BOARD_PROTOCOL *This);
static EFI_STATUS EFIAPI PanelRunKernelEnable(MTK_PANEL_BOARD_PROTOCOL *This);
static EFI_STATUS EFIAPI PanelRunKernelUnprepare(MTK_PANEL_BOARD_PROTOCOL *This);
static MTK_PANEL_BOARD_PROTOCOL PanelBoard = {
  MTK_PANEL_BOARD_PROTOCOL_REVISION,
  PanelRunInit,
  PanelRunSuspend,
  PanelRunKernelPrepare,
  PanelRunKernelEnable,
  PanelRunKernelUnprepare
};

#define PANEL_PINCTRL_BASE             0x10005000U
#define PANEL_PINCTRL_DIR_OFFSET       0x000U
#define PANEL_PINCTRL_DATA_OUT_OFFSET  0x100U
#define PANEL_PINCTRL_MODE_OFFSET      0x300U
#define PANEL_GPIO_BANK_STRIDE         0x010U

typedef struct {
  UINT32 Mode;
  UINT32 Direction;
  UINT32 Latch;
  UINT32 Input;
  UINT32 Ies;
} TOUCH_GPIO_SNAPSHOT;
static UINT64 Cells(const VOID *Value,UINTN Bytes) {
  const UINT8 *P=Value;UINT64 V=0;
  for(UINTN I=0;I<Bytes;I++)V=(V<<8)|P[I];
  return V;
}
static BOOLEAN Enabled(const VOID *Fdt,INT32 Node) {
  INT32 Length;const VOID *P=FdtGetProp(Fdt,Node,"status",&Length);
  return !P || (Length==5 && CompareMem(P,"okay",5)==0) || (Length==3 && CompareMem(P,"ok",3)==0);
}
static BOOLEAN Compatible(const VOID *Fdt,INT32 Node,const CHAR8 *Name) {
  INT32 Length;const CHAR8 *P=FdtGetProp(Fdt,Node,"compatible",&Length);BOOLEAN Match=FALSE;
  UINTN Wanted=AsciiStrLen(Name);
  if(!P || Length<=0)return FALSE;
  for(UINTN Offset=0;Offset<(UINTN)Length;) {
    UINTN End=Offset;while(End<(UINTN)Length && P[End])End++;
    if(End==(UINTN)Length)return FALSE;
    if(End-Offset==Wanted && CompareMem(P+Offset,Name,Wanted)==0)Match=TRUE;
    Offset=End+1;
  }
  return Match;
}
static BOOLEAN Property(const VOID *Fdt,INT32 Node,const CHAR8 *Name,UINT32 Value) {
  INT32 Length;const VOID *P=FdtGetProp(Fdt,Node,Name,&Length);
  return P && Length==4 && Cells(P,4)==Value;
}
static EFI_STATUS ValidateTree(const VOID *Fdt,UINTN Bytes) {
  INT32 Soc,Gpio,Eint,Gic,Length;const UINT8 *P;
  const UINT64 Banks[]={0x10005000,0x11C30000,0x11D10000,0x11D40000,0x11E20000,0x11E60000,0x11EA0000};
  if(!Fdt || Bytes<sizeof(FDT_HEADER) || Bytes>0x200000 ||
      FdtCheckFull(Fdt,Bytes) || FdtTotalSize(Fdt)!=Bytes)return EFI_COMPROMISED_DATA;
  Soc=FdtPathOffset(Fdt,"/soc");Gpio=FdtPathOffset(Fdt,"/soc/pinctrl");
  Eint=FdtPathOffset(Fdt,"/soc/apirq@1000b000");Gic=FdtPathOffset(Fdt,"/interrupt-controller");
  if(Soc<0 || Gpio<0 || Eint<0 || Gic<0 || !Compatible(Fdt,0,"mediatek,MT6789") ||
      !Enabled(Fdt,0) || !Enabled(Fdt,Soc) || !Enabled(Fdt,Gpio) || !Enabled(Fdt,Eint) || !Enabled(Fdt,Gic) ||
      FdtAddressCells(Fdt,0)!=2 || FdtSizeCells(Fdt,0)!=2 ||
      FdtAddressCells(Fdt,Soc)!=2 || FdtSizeCells(Fdt,Soc)!=2 ||
      !Compatible(Fdt,Gpio,"mediatek,mt6789-pinctrl") || !Compatible(Fdt,Eint,"mediatek,mt6983-eint") ||
      !Compatible(Fdt,Gic,"arm,gic-v3"))return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Gic,"reg",&Length);
  if(!P || Length!=32 || Cells(P,8)!=0x0C000000 || Cells(P+8,8)!=0x40000 ||
      Cells(P+16,8)!=0x0C040000 || Cells(P+24,8)!=0x200000)return EFI_UNSUPPORTED;
  if(!FdtGetProp(Fdt,Gic,"interrupt-controller",&Length) || Length!=0 ||
      !FdtGetProp(Fdt,Gpio,"interrupt-controller",&Length) || Length!=0)return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Soc,"ranges",&Length);if(!P || Length!=0)return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Gpio,"reg",&Length);
  if(!P || Length!=sizeof(Banks)*2)return EFI_UNSUPPORTED;
  for(UINTN I=0;I<ARRAY_SIZE(Banks);I++)if(Cells(P+I*16,8)!=Banks[I] || Cells(P+I*16+8,8)!=0x1000)return EFI_UNSUPPORTED;
  if(!FdtGetProp(Fdt,Gpio,"gpio-controller",&Length) || Length!=0 ||
      !Property(Fdt,Gpio,"#gpio-cells",2) || !Property(Fdt,Gpio,"#interrupt-cells",2))return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Gpio,"gpio-ranges",&Length);
  if(!P || Length!=16 || FdtNodeOffsetByPhandle(Fdt,(UINT32)Cells(P,4))!=Gpio ||
      Cells(P+4,4)!=0 || Cells(P+8,4)!=0 || Cells(P+12,4)!=188)return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Gpio,"mediatek,eint",&Length);
  if(!P || Length!=4 || FdtNodeOffsetByPhandle(Fdt,(UINT32)Cells(P,4))!=Eint)return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Eint,"reg",&Length);
  if(!P || Length!=16 || Cells(P,8)!=0x1000B000 || Cells(P+8,8)!=0x1000 ||
      !Property(Fdt,Eint,"mediatek,total-pin-number",160) || !Property(Fdt,Eint,"mediatek,instance-num",1) ||
      FdtGetProp(Fdt,Eint,"mediatek,pins",&Length) || FdtGetProp(Fdt,Eint,"interrupts-extended",&Length))return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Eint,"interrupts",&Length);
  if(!P || Length!=16 || Cells(P,4)!=0 || Cells(P+4,4)!=244 || Cells(P+8,4)!=4 || Cells(P+12,4)!=0 ||
      !Property(Fdt,Gic,"#interrupt-cells",4))return EFI_UNSUPPORTED;
  // Resolve the inherited parent; do not assume a phandle number from stock.
  for(INT32 Node=Eint;Node>=0;Node=FdtParentOffset(Fdt,Node)) {
    P=FdtGetProp(Fdt,Node,"interrupt-parent",&Length);
    if(P) {
      if(Length!=4 || FdtNodeOffsetByPhandle(Fdt,(UINT32)Cells(P,4))!=Gic)return EFI_UNSUPPORTED;
      return EFI_SUCCESS;
    }
  }
  return EFI_UNSUPPORTED;
}
static BOOLEAN Mapped(UINT64 Base) {
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  if(EFI_ERROR(gDS->GetMemorySpaceDescriptor(Base,&D)) || D.GcdMemoryType!=EfiGcdMemoryTypeMemoryMappedIo ||
      Base<D.BaseAddress || Base-D.BaseAddress>D.Length || 0x1000>D.Length-(Base-D.BaseAddress) ||
      !(D.Attributes&EFI_MEMORY_UC) ||
      (D.Attributes&(EFI_MEMORY_WB|EFI_MEMORY_WT|EFI_MEMORY_WC|EFI_MEMORY_RO|EFI_MEMORY_RP)))return FALSE;
  return TRUE;
}
static BOOLEAN LiveReady(VOID) {
  const UINT64 Bases[]={0x10005000,0x1000B000,0x11C30000,0x11D10000,0x11D40000,0x11E20000,0x11E60000,0x11EA0000};
  for(UINTN I=0;I<ARRAY_SIZE(Bases);I++)if(!Mapped(Bases[I]))return FALSE;
  for(UINTN I=0;I<5;I++) {
    UINT32 Required=I<3?MAX_UINT32:I==3?0x3FFFFFFFU:0;
    if(MmioRead32(0x1000B080+I*4)!=MAX_UINT32 ||
        (MmioRead32(0x1000B400+I*4)&Required)!=Required)return FALSE;
  }
  return TRUE;
}
// The first interrupt tuple in stock is GPIO 9, IRQ_TYPE_EDGE_FALLING.
// The trailing tuple is retained verbatim, not reinterpreted as a GIC IRQ.
static EFI_STATUS ValidateTouchTree(const VOID *Fdt) {
  INT32 Bus=FdtPathOffset(Fdt,"/soc/i2c@11e00000");
  INT32 Node=FdtPathOffset(Fdt,"/soc/i2c@11e00000/himax_ts@48");
  INT32 Gpio=FdtPathOffset(Fdt,"/soc/pinctrl"),Length;const UINT8 *P;
  if(Bus<0 || Node<0 || Gpio<0 || !Enabled(Fdt,Bus) || !Enabled(Fdt,Node) ||
      !Compatible(Fdt,Bus,"mediatek,mt6983-i2c") || !Compatible(Fdt,Node,"himax,hxcommon") ||
      !Property(Fdt,Node,"reg",0x48) || !Property(Fdt,Node,"report_type",1) ||
      FdtGetProp(Fdt,Node,"interrupts-extended",&Length))return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Bus,"reg",&Length);
  if(!P || Length!=32 || Cells(P,8)!=0x11E00000 || Cells(P+8,8)!=0x1000 ||
      Cells(P+16,8)!=0x10217080 || Cells(P+24,8)!=0x80)return EFI_UNSUPPORTED;
  const CHAR8 *Coords[]={"himax,display-coords","himax,panel-coords"};
  for(UINTN I=0;I<ARRAY_SIZE(Coords);I++) {
    P=FdtGetProp(Fdt,Node,Coords[I],&Length);
    if(!P || Length!=16 || Cells(P,4)!=0 || Cells(P+4,4)!=1400 ||
       Cells(P+8,4)!=0 || Cells(P+12,4)!=2240)return EFI_UNSUPPORTED;
  }
  const CHAR8 *Pins[]={"himax,irq-gpio","himax,rst-gpio"};
  for(UINTN I=0;I<ARRAY_SIZE(Pins);I++) {
    P=FdtGetProp(Fdt,Node,Pins[I],&Length);
    if(!P || Length!=12 || FdtNodeOffsetByPhandle(Fdt,(UINT32)Cells(P,4))!=Gpio ||
       Cells(P+4,4)!=(I?152:9) || Cells(P+8,4)!=0)return EFI_UNSUPPORTED;
  }
  P=FdtGetProp(Fdt,Node,"interrupt-parent",&Length);
  if(!P || Length!=4 || FdtNodeOffsetByPhandle(Fdt,(UINT32)Cells(P,4))!=Gpio)return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Node,"interrupts",&Length);
  if(!P || Length!=16 || Cells(P,4)!=9 || Cells(P+4,4)!=2 ||
      Cells(P+8,4)!=9 || Cells(P+12,4)!=0)return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
// T70Pro MH248: hall_sensor_p reads raw GPIO0; far/open is raw zero.
// The reference phone's hall_key/GPIO7 is unrelated to this board.
static EFI_STATUS ValidateHallTree(const VOID *Fdt) {
  INT32 Odm=FdtPathOffset(Fdt,"/soc/odm"),Node=FdtPathOffset(Fdt,"/soc/odm/hall_para");
  INT32 Gpio=FdtPathOffset(Fdt,"/soc/pinctrl"),Length;const UINT8 *P;
  if(Odm<0 || Node<0 || Gpio<0 || !Enabled(Fdt,Odm) || !Enabled(Fdt,Node) ||
      !Property(Fdt,Node,"hall_far_gpvalue",0))return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Node,"hall_name",&Length);
  if(!P || Length!=6 || CompareMem(P,"MH248",6))return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Node,"hall_int_port",&Length);
  if(!P || Length!=12 || FdtNodeOffsetByPhandle(Fdt,(UINT32)Cells(P,4))!=Gpio ||
      Cells(P+4,4)!=0 || Cells(P+8,4)!=1)return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Node,"wakeup-source",&Length);
  return P && !Length?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static BOOLEAN LiveHallReady(VOID) {
  // GPIO0: RT IES+0x70 bit20, GPIO mode zero, input. No pad/MMIO writes.
  return !(MmioRead32(0x10005300)&7U) && !(MmioRead32(0x10005000)&1U) &&
    (MmioRead32(0x11EA0070)&(1U<<20));
}
static BOOLEAN ChargerIrqReady(const VOID *Fdt) {
  INT32 Bus=FdtPathOffset(Fdt,"/soc/i2c@1101a000");
  INT32 Charger=FdtPathOffset(Fdt,"/soc/i2c@1101a000/sc8989x@6A");
  INT32 Gpio=FdtPathOffset(Fdt,"/soc/pinctrl"),Length;
  if(Bus<0 || Charger<0 || Gpio<0 || !Enabled(Fdt,Bus) || !Enabled(Fdt,Charger) ||
     !Compatible(Fdt,Charger,"southchip,sc89890h") || !Property(Fdt,Charger,"reg",0x6a))
    return FALSE;

  const UINT8 *Pin=FdtGetProp(Fdt,Charger,"sc,intr-gpio",&Length);
  if(!Pin || Length!=12 || FdtNodeOffsetByPhandle(Fdt,(UINT32)Cells(Pin,4))!=Gpio ||
     Cells(Pin+4,4)!=12 || Cells(Pin+8,4)!=2)
    return FALSE;

  // The SC89890H INT output pulses low. Its idle level is not a readiness test.
  return !(MmioRead32(0x10005310U)&(0xFU<<16)) && !(MmioRead32(0x10005000U)&BIT12);
}
static BOOLEAN RuntimeTree(VOID **Fdt,UINTN *Size) {
  UINT64 Bytes;
  if(EFI_ERROR(EfiGetSystemConfigurationTable(&gFdtTableGuid,Fdt)) || !*Fdt ||
      MmioRead64(0x43F7F000)!=0x0000000148303754ULL ||
      MmioRead64(0x43F7F048)!=(UINT64)(UINTN)*Fdt || MmioRead64(0x43F7F058)!=EFI_SUCCESS)return FALSE;
  Bytes=MmioRead64(0x43F7F050);
  if(Bytes<sizeof(FDT_HEADER) || Bytes>0x200000)return FALSE;
  *Size=(UINTN)Bytes;return TRUE;
}
static BOOLEAN LocateTouchGpioProtocol(MTK_GPIO_PROTOCOL **Gpio) {
  EFI_STATUS Status;
  MTK_GPIO_PROTOCOL *Protocol=NULL;
  if(!Gpio || !gBS || !gBS->LocateProtocol)return FALSE;
  Status=gBS->LocateProtocol(&gMediaTekGpioProtocolGuid,NULL,(VOID **)&Protocol);
  if(Status!=EFI_SUCCESS || !Protocol || !Protocol->IsBusy || !Protocol->AcquirePins ||
      !Protocol->ReleasePins || !Protocol->ValidateLease)return FALSE;
  *Gpio=Protocol;
  return TRUE;
}

static EFI_STATUS
PanelLocateGpioProtocol (
  OUT MTK_GPIO_PROTOCOL **Gpio
  )
{
  EFI_STATUS Status;
  EFI_TPL OldTpl;

  if (Exiting || (Gpio == NULL) || (gBS == NULL) ||
      (gBS->RaiseTPL == NULL) || (gBS->RestoreTPL == NULL)) {
    return EFI_ABORTED;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Exiting) {
    gBS->RestoreTPL (OldTpl);
    return EFI_ABORTED;
  }
  Status = LocateTouchGpioProtocol (Gpio) ? EFI_SUCCESS : EFI_NOT_READY;
  gBS->RestoreTPL (OldTpl);
  if (Exiting) {
    return EFI_ABORTED;
  }
  return Status;
}

static const UINT32 mPanelPins[] = {
  MTK_PANEL_GPIO_EDP_ENABLE,
  MTK_PANEL_GPIO_EDP_RESET,
  MTK_PANEL_GPIO_AVDD,
  MTK_PANEL_GPIO_VDD18,
  MTK_PANEL_GPIO_VDD33,
  MTK_PANEL_GPIO_LK_UNNAMED_152,
  MTK_PANEL_GPIO_AVEE,
  MTK_PANEL_GPIO_VDD12,
  MTK_PANEL_GPIO_BACKLIGHT
};

static EFI_STATUS
ValidatePanelTree (
  IN CONST VOID *Fdt
  )
{
  INT32 Dsi;
  INT32 Panel;

  Dsi = FdtPathOffset (Fdt, "/soc/dsi@14013000");
  Panel = FdtPathOffset (Fdt, "/soc/dsi@14013000/panel1@0");
  if ((Dsi < 0) || (Panel < 0) || (FdtParentOffset (Fdt, Panel) != Dsi) ||
      !Enabled (Fdt, Dsi) || !Enabled (Fdt, Panel) ||
      !Compatible (Fdt, Panel, "cyx,cv101wu_lh1038b") ||
      !Property (Fdt, Panel, "reg", 0)) {
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

static BOOLEAN
PanelInterruptReadyRaw (
  VOID
  )
{
  UINT32 Index;

  for (Index = 0; Index < 5; ++Index) {
    UINT32 Required;

    Required = (Index < 3) ? MAX_UINT32 : ((Index == 3) ? 0x3FFFFFFFU : 0U);
    if ((MmioRead32 (0x1000B080U + Index * sizeof (UINT32)) != MAX_UINT32) ||
        ((MmioRead32 (0x1000B400U + Index * sizeof (UINT32)) & Required) != Required)) {
      return FALSE;
    }
  }

  return TRUE;
}

static BOOLEAN
PanelRuntimeProofRaw (
  IN  CONST VOID *ExpectedFdt,
  IN  UINTN       ExpectedBytes,
  IN  BOOLEAN     CheckInterrupts,
  OUT VOID      **ObservedFdt OPTIONAL,
  OUT UINTN      *ObservedBytes OPTIONAL
  )
{
  VOID *Fdt;
  EFI_STATUS Status;
  UINT64 Bytes;
  if (Exiting) {
    return FALSE;
  }
  Fdt = NULL;
  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Fdt);
  Bytes = MmioRead64 (0x43F7F050U);
  if ((Status != EFI_SUCCESS) || (Fdt == NULL) ||
      (MmioRead64 (0x43F7F000U) != 0x0000000148303754ULL) ||
      (MmioRead64 (0x43F7F048U) != (UINT64)(UINTN)Fdt) ||
      (MmioRead64 (0x43F7F058U) != EFI_SUCCESS) ||
      (Bytes < sizeof (FDT_HEADER)) || (Bytes > 0x200000U) ||
      (Fdt != ExpectedFdt) || ((ExpectedBytes != 0U) && (Bytes != ExpectedBytes)) ||
      (CheckInterrupts && !PanelInterruptReadyRaw ())) {
    return FALSE;
  }
  if ((ObservedFdt != NULL) && (ObservedBytes != NULL)) {
    *ObservedFdt = Fdt;
    *ObservedBytes = (UINTN)Bytes;
  }
  return TRUE;
}

static EFI_STATUS
PanelReadRuntimeProof (
  IN  CONST VOID *ExpectedFdt,
  IN  UINTN       ExpectedBytes,
  IN  BOOLEAN     CheckInterrupts,
  OUT VOID      **ObservedFdt OPTIONAL,
  OUT UINTN      *ObservedBytes OPTIONAL
  )
{
  EFI_TPL OldTpl;
  BOOLEAN Ready;

  if (Exiting || (gBS == NULL) || (gBS->RaiseTPL == NULL) ||
      (gBS->RestoreTPL == NULL)) {
    return EFI_ABORTED;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Exiting) {
    gBS->RestoreTPL (OldTpl);
    return EFI_ABORTED;
  }
  Ready = PanelRuntimeProofRaw (ExpectedFdt, ExpectedBytes, CheckInterrupts,
                                ObservedFdt, ObservedBytes);
  gBS->RestoreTPL (OldTpl);
  if (Exiting) {
    return EFI_ABORTED;
  }
  if (!Ready) {
    return EFI_NOT_READY;
  }

  return EFI_SUCCESS;
}

static EFI_STATUS
PanelCheckBoard (
  OUT VOID **ObservedFdt OPTIONAL,
  OUT UINTN *ObservedBytes OPTIONAL
  )
{
  EFI_STATUS Status;
  VOID *Fdt;
  UINTN Bytes;
  CONST UINT64 Bases[] = {
    0x10005000U, 0x1000B000U, 0x11C30000U, 0x11D10000U,
    0x11D40000U, 0x11E20000U, 0x11E60000U, 0x11EA0000U
  };

  if (Exiting) {
    return EFI_ABORTED;
  }

  Fdt = NULL;
  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Fdt);
  if (Exiting) {
    return EFI_ABORTED;
  }
  if ((Status != EFI_SUCCESS) || (Fdt == NULL)) {
    return EFI_NOT_READY;
  }

  Status = PanelReadRuntimeProof (Fdt, PanelLeaseHeld ? PanelFdtBytes : 0U,
                                  FALSE, &Fdt, &Bytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Exiting) {
    return EFI_ABORTED;
  }
  if ((Bytes < sizeof (FDT_HEADER)) || (Bytes > 0x200000U) ||
      EFI_ERROR (ValidateTree (Fdt, Bytes)) ||
      EFI_ERROR (ValidatePanelTree (Fdt)) ||
      EFI_ERROR (ValidateTouchTree (Fdt))) {
    return EFI_UNSUPPORTED;
  }

  if (PanelLeaseHeld &&
      ((PanelFdt != Fdt) || (PanelFdtBytes != Bytes))) {
    return EFI_MEDIA_CHANGED;
  }

  if (Exiting || (gDS == NULL) || (gDS->GetMemorySpaceDescriptor == NULL)) {
    return EFI_ABORTED;
  }

  for (UINTN Index = 0; Index < ARRAY_SIZE (Bases); ++Index) {
    if (!Mapped (Bases[Index])) {
      return EFI_NOT_READY;
    }
    if (Exiting) {
      return EFI_ABORTED;
    }
  }

  Status = PanelReadRuntimeProof (Fdt, Bytes, TRUE, ObservedFdt, ObservedBytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Exiting ? EFI_ABORTED : EFI_SUCCESS;
}

static EFI_STATUS
PanelCheckLease (
  VOID
  )
{
  MTK_GPIO_PROTOCOL *ActiveGpio;

  if (Exiting) {
    return EFI_ABORTED;
  }
  if (PanelLeaseUncertain) {
    return EFI_COMPROMISED_DATA;
  }
  if (!PanelLeaseHeld || (PanelLeaseToken == 0) || (PanelPinOwner == NULL)) {
    return EFI_NOT_READY;
  }

  ActiveGpio = NULL;
  {
    EFI_STATUS Status;
    Status = PanelLocateGpioProtocol (&ActiveGpio);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  if (Exiting) {
    return EFI_ABORTED;
  }
  if (ActiveGpio != PanelPinOwner) {
    return EFI_MEDIA_CHANGED;
  }

  for (UINTN Index = 0; Index < ARRAY_SIZE (mPanelPins); ++Index) {
    if (Exiting) {
      return EFI_ABORTED;
    }
    if (PanelPinOwner->ValidateLease (PanelLeaseToken, mPanelPins[Index]) != EFI_SUCCESS) {
      return Exiting ? EFI_ABORTED : EFI_ACCESS_DENIED;
    }
    if (Exiting) {
      return EFI_ABORTED;
    }
  }

  return EFI_SUCCESS;
}

static EFI_STATUS
PanelCheckContext (
  VOID
  )
{
  EFI_STATUS Status;

  Status = PanelCheckBoard (NULL, NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return PanelCheckLease ();
}

static BOOLEAN
PanelRoleMatches (
  IN UINT32              Pin,
  IN MTK_PANEL_GPIO_ROLE Role
  )
{
  static CONST struct {
    UINT32 Pin;
    MTK_PANEL_GPIO_ROLE Role;
  } Roles[] = {
    { MTK_PANEL_GPIO_EDP_ENABLE,      MtkPanelGpioEdpEnable },
    { MTK_PANEL_GPIO_EDP_RESET,       MtkPanelGpioEdpReset },
    { MTK_PANEL_GPIO_AVDD,            MtkPanelGpioAvdd },
    { MTK_PANEL_GPIO_VDD18,           MtkPanelGpioVdd18 },
    { MTK_PANEL_GPIO_VDD33,           MtkPanelGpioVdd33 },
    { MTK_PANEL_GPIO_LK_UNNAMED_152,  MtkPanelGpioLkUnnamed152 },
    { MTK_PANEL_GPIO_AVEE,            MtkPanelGpioAvee },
    { MTK_PANEL_GPIO_VDD12,           MtkPanelGpioVdd12 },
    { MTK_PANEL_GPIO_BACKLIGHT,       MtkPanelGpioBacklight }
  };

  for (UINTN Index = 0; Index < ARRAY_SIZE (Roles); ++Index) {
    if ((Roles[Index].Pin == Pin) && (Roles[Index].Role == Role)) {
      return TRUE;
    }
  }
  return FALSE;
}

static BOOLEAN
PanelPinKnown (
  IN UINT32 Pin
  )
{
  for (UINTN Index = 0; Index < ARRAY_SIZE (mPanelPins); ++Index) {
    if (mPanelPins[Index] == Pin) {
      return TRUE;
    }
  }
  return FALSE;
}

static MTK_PANEL_STATUS
PanelCoreStatusFromEfi (
  IN EFI_STATUS Status
  )
{
  if (Status == EFI_SUCCESS) {
    return MtkPanelSuccess;
  }
  if ((Status == EFI_UNSUPPORTED) || (Status == EFI_COMPROMISED_DATA) ||
      (Status == EFI_MEDIA_CHANGED)) {
    return MtkPanelMappingError;
  }
  if ((Status == EFI_NOT_READY) || (Status == EFI_ACCESS_DENIED) ||
      (Status == EFI_ABORTED)) {
    return MtkPanelNotReady;
  }
  if (Status == EFI_INVALID_PARAMETER) {
    return MtkPanelInvalid;
  }
  return MtkPanelIoError;
}

static MTK_PANEL_STATUS
PanelValidateGpio (
  IN VOID                *Context,
  IN MTK_PANEL_U32        Pin,
  IN MTK_PANEL_GPIO_ROLE  Role
  )
{
  EFI_STATUS Status;

  if ((Context != &PanelBoard) || !PanelRoleMatches (Pin, Role)) {
    return MtkPanelMappingError;
  }
  Status = PanelCheckContext ();
  return PanelCoreStatusFromEfi (Status);
}

static EFI_STATUS
PanelReadPinRaw (
  IN  UINT32  Pin,
  OUT UINT32 *Mode,
  OUT UINT32 *Direction,
  OUT UINT32 *OutputLatch
  )
{
  UINT32 ModeOffset;
  UINT32 ModeShift;
  UINT32 BankOffset;
  UINT32 Bit;
  EFI_TPL OldTpl;

  if ((Pin > 187U) || (Mode == NULL) || (Direction == NULL) ||
      (OutputLatch == NULL) || (gBS == NULL) || (gBS->RaiseTPL == NULL) ||
      (gBS->RestoreTPL == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Exiting) {
    return EFI_ABORTED;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Exiting) {
    gBS->RestoreTPL (OldTpl);
    return EFI_ABORTED;
  }

  if (!PanelRuntimeProofRaw (PanelFdt, PanelFdtBytes, TRUE, NULL, NULL)) {
    gBS->RestoreTPL (OldTpl);
    return Exiting ? EFI_ABORTED : EFI_NOT_READY;
  }

  ModeOffset = PANEL_PINCTRL_MODE_OFFSET + ((Pin / 8U) * PANEL_GPIO_BANK_STRIDE);
  ModeShift = (Pin % 8U) * 4U;
  BankOffset = (Pin / 32U) * PANEL_GPIO_BANK_STRIDE;
  Bit = 1U << (Pin % 32U);
  *Mode = (MmioRead32 (PANEL_PINCTRL_BASE + ModeOffset) >> ModeShift) & 0xFU;
  *Direction = (MmioRead32 (PANEL_PINCTRL_BASE + PANEL_PINCTRL_DIR_OFFSET + BankOffset) & Bit) ? 1U : 0U;
  *OutputLatch = (MmioRead32 (PANEL_PINCTRL_BASE + PANEL_PINCTRL_DATA_OUT_OFFSET + BankOffset) & Bit) ? 1U : 0U;

  gBS->RestoreTPL (OldTpl);
  return Exiting ? EFI_ABORTED : EFI_SUCCESS;
}

static MTK_PANEL_STATUS
PanelReadGpio (
  IN  VOID          *Context,
  IN  MTK_PANEL_U32  Pin,
  OUT MTK_PANEL_U32 *Mode,
  OUT MTK_PANEL_U32 *Direction,
  OUT MTK_PANEL_U32 *OutputLatch
  )
{
  EFI_STATUS Status;
  UINT32 ObservedMode;
  UINT32 ObservedDirection;
  UINT32 ObservedLatch;

  if ((Context != &PanelBoard) || !PanelPinKnown (Pin) ||
      (Mode == NULL) || (Direction == NULL) || (OutputLatch == NULL)) {
    return MtkPanelInvalid;
  }

  Status = PanelCheckContext ();
  if (EFI_ERROR (Status)) {
    return PanelCoreStatusFromEfi (Status);
  }

  Status = PanelReadPinRaw (Pin, &ObservedMode, &ObservedDirection, &ObservedLatch);
  if (EFI_ERROR (Status)) {
    return PanelCoreStatusFromEfi (Status);
  }

  Status = PanelCheckContext ();
  if (EFI_ERROR (Status)) {
    return PanelCoreStatusFromEfi (Status);
  }

  *Mode = ObservedMode;
  *Direction = ObservedDirection;
  *OutputLatch = ObservedLatch;
  return MtkPanelSuccess;
}

typedef enum {
  PanelWriteMode,
  PanelWriteDirection,
  PanelWriteValue
} PANEL_WRITE_KIND;

static MTK_PANEL_STATUS
PanelWriteGpio (
  IN UINT32          Pin,
  IN UINT32          Value,
  IN PANEL_WRITE_KIND Kind
  )
{
  EFI_STATUS Status;
  MTK_GPIO_PROTOCOL *Gpio;
  EFI_TPL OldTpl;

  if (!PanelPinKnown (Pin)) {
    return MtkPanelInvalid;
  }

  Status = PanelCheckContext ();
  if (EFI_ERROR (Status)) {
    return PanelCoreStatusFromEfi (Status);
  }

  Gpio = PanelPinOwner;
  if (Exiting || (gBS == NULL) || (gBS->RaiseTPL == NULL) ||
      (gBS->RestoreTPL == NULL)) {
    return MtkPanelNotReady;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Exiting) {
    gBS->RestoreTPL (OldTpl);
    return MtkPanelNotReady;
  }
  if (Kind == PanelWriteMode) {
    Status = (Gpio->SetModeWithLease != NULL)
               ? Gpio->SetModeWithLease (PanelLeaseToken, Pin, Value)
               : EFI_UNSUPPORTED;
  } else if (Kind == PanelWriteDirection) {
    Status = (Gpio->SetDirWithLease != NULL)
               ? Gpio->SetDirWithLease (PanelLeaseToken, Pin, (BOOLEAN)Value)
               : EFI_UNSUPPORTED;
  } else {
    Status = (Gpio->SetStateWithLease != NULL)
               ? Gpio->SetStateWithLease (PanelLeaseToken, Pin, (BOOLEAN)Value)
               : EFI_UNSUPPORTED;
  }
  gBS->RestoreTPL (OldTpl);

  if (Exiting) {
    return MtkPanelNotReady;
  }

  {
    EFI_STATUS VerifyStatus;

    VerifyStatus = PanelCheckContext ();
    if (EFI_ERROR (VerifyStatus)) {
      return PanelCoreStatusFromEfi (VerifyStatus);
    }
  }

  return PanelCoreStatusFromEfi (Status);
}

static MTK_PANEL_STATUS
PanelSetMode (
  IN VOID          *Context,
  IN MTK_PANEL_U32  Pin,
  IN MTK_PANEL_U32  Mode
  )
{
  if (Context != &PanelBoard) {
    return MtkPanelInvalid;
  }
  return PanelWriteGpio (Pin, Mode, PanelWriteMode);
}

static MTK_PANEL_STATUS
PanelSetDirection (
  IN VOID          *Context,
  IN MTK_PANEL_U32  Pin,
  IN MTK_PANEL_U32  Direction
  )
{
  if (Context != &PanelBoard) {
    return MtkPanelInvalid;
  }
  return PanelWriteGpio (Pin, Direction, PanelWriteDirection);
}

static MTK_PANEL_STATUS
PanelSetValue (
  IN VOID          *Context,
  IN MTK_PANEL_U32  Pin,
  IN MTK_PANEL_U32  Value
  )
{
  if (Context != &PanelBoard) {
    return MtkPanelInvalid;
  }
  return PanelWriteGpio (Pin, Value, PanelWriteValue);
}

static MTK_PANEL_STATUS
PanelDelayUs (
  IN VOID          *Context,
  IN MTK_PANEL_U32  Microseconds
  )
{
  EFI_STATUS Status;
  UINTN Actual;

  if ((Context != &PanelBoard) || (Microseconds == 0U)) {
    return MtkPanelInvalid;
  }
  Status = PanelCheckContext ();
  if (EFI_ERROR (Status)) {
    return PanelCoreStatusFromEfi (Status);
  }

  // Keep the caller at its original TPL during the bounded LK delay.
  Actual = MicroSecondDelay (Microseconds);
  if (Exiting) {
    return MtkPanelNotReady;
  }
  if (Actual < Microseconds) {
    return MtkPanelDelayError;
  }

  Status = PanelCheckContext ();
  return EFI_ERROR (Status) ? PanelCoreStatusFromEfi (Status) : MtkPanelSuccess;
}

static EFI_STATUS
PanelReleaseLease (
  VOID
  )
{
  EFI_STATUS Status;
  MTK_GPIO_PROTOCOL *ActiveGpio;

  if (!PanelLeaseHeld) {
    return PanelLeaseUncertain ? EFI_COMPROMISED_DATA : EFI_SUCCESS;
  }
  if (Exiting) {
    return EFI_ABORTED;
  }
  if (PanelLeaseUncertain || (PanelLeaseToken == 0) ||
      (PanelPinOwner == NULL) || (PanelPinOwner->ReleasePins == NULL) ||
      (PanelPinOwner->ValidateLease == NULL)) {
    return EFI_COMPROMISED_DATA;
  }

  ActiveGpio = NULL;
  {
    EFI_STATUS LocateStatus;
    LocateStatus = PanelLocateGpioProtocol (&ActiveGpio);
    if (EFI_ERROR (LocateStatus)) {
      return LocateStatus;
    }
    if (ActiveGpio != PanelPinOwner) {
      return EFI_MEDIA_CHANGED;
    }
  }
  if (Exiting) {
    return EFI_ABORTED;
  }
  for (UINTN Index = 0; Index < ARRAY_SIZE (mPanelPins); ++Index) {
    if (Exiting) {
      return EFI_ABORTED;
    }
    if (PanelPinOwner->ValidateLease (PanelLeaseToken, mPanelPins[Index]) != EFI_SUCCESS) {
      return Exiting ? EFI_ABORTED : EFI_ACCESS_DENIED;
    }
    if (Exiting) {
      return EFI_ABORTED;
    }
  }

  {
    EFI_TPL OldTpl;

    if (Exiting || (gBS == NULL) || (gBS->RaiseTPL == NULL) ||
        (gBS->RestoreTPL == NULL)) {
      return EFI_ABORTED;
    }
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    if (Exiting) {
      gBS->RestoreTPL (OldTpl);
      return EFI_ABORTED;
    }
    Status = PanelPinOwner->ReleasePins (PanelLeaseToken);
    gBS->RestoreTPL (OldTpl);
  }
  if (Status == EFI_SUCCESS) {
    PanelLeaseHeld = FALSE;
    PanelLeaseUncertain = FALSE;
    PanelLeaseToken = 0;
    PanelPinOwner = NULL;
    PanelLeaseOperation = MtkPanelOperationNone;
    PanelFdt = NULL;
    PanelFdtBytes = 0;
  }
  return Status;
}

static EFI_STATUS
PanelAcquireLease (
  IN MTK_PANEL_OPERATION Operation
  )
{
  EFI_STATUS Status;
  MTK_GPIO_PROTOCOL *Gpio;
  VOID *Fdt;
  UINTN Bytes;
  UINT64 Token;

  Status = PanelCheckBoard (&Fdt, &Bytes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Gpio = NULL;
  Status = PanelLocateGpioProtocol (&Gpio);
  if (EFI_ERROR (Status) ||
      (Gpio->SetDirWithLease == NULL) || (Gpio->SetStateWithLease == NULL) ||
      (Gpio->SetModeWithLease == NULL)) {
    return Exiting ? EFI_ABORTED : (EFI_ERROR (Status) ? Status : EFI_NOT_READY);
  }
  if (Exiting) {
    return EFI_ABORTED;
  }

  Token = 0;
  Status = Gpio->AcquirePins (mPanelPins, ARRAY_SIZE (mPanelPins), &Token);
  if (Status != EFI_SUCCESS) {
    return (Status == EFI_ACCESS_DENIED) ? EFI_NOT_READY : Status;
  }

  // Record a successful claim before doing any more service calls: EBS can be
  // dispatched while AcquirePins restores TPL, and the token must not vanish.
  PanelPinOwner = Gpio;
  PanelLeaseToken = Token;
  PanelLeaseHeld = TRUE;
  PanelLeaseOperation = Operation;
  PanelFdt = Fdt;
  PanelFdtBytes = Bytes;
  if (Token == 0) {
    PanelLeaseUncertain = TRUE;
    return EFI_COMPROMISED_DATA;
  }
  if (Exiting) {
    return EFI_ABORTED;
  }

  return PanelCheckContext ();
}

static BOOLEAN
PanelOperationComplete (
  IN MTK_PANEL_OPERATION Operation
  )
{
  return (BOOLEAN)(((Operation == MtkPanelOperationLkInit) &&
                    (PanelCore.State == MtkPanelStateLkInitComplete)) ||
                   ((Operation == MtkPanelOperationLkSuspend) &&
                    (PanelCore.State == MtkPanelStateLkSuspendComplete)) ||
                   ((Operation == MtkPanelOperationKernelPrepare) &&
                    (PanelCore.State == MtkPanelStateKernelPrepared)) ||
                   ((Operation == MtkPanelOperationKernelEnable) &&
                    (PanelCore.State == MtkPanelStateKernelEnabled)) ||
                   ((Operation == MtkPanelOperationKernelUnprepare) &&
                    (PanelCore.State == MtkPanelStateKernelUnprepared)));
}

static BOOLEAN
PanelOperationAllowed (
  IN MTK_PANEL_OPERATION Operation
  )
{
  if (PanelCore.State == MtkPanelStateQuarantined) {
    return (BOOLEAN)(PanelCore.PendingOperation == Operation);
  }
  if (PanelCore.PendingOperation != MtkPanelOperationNone) {
    return (BOOLEAN)(PanelCore.PendingOperation == Operation);
  }
  if (Operation == MtkPanelOperationLkInit) {
    return (BOOLEAN)(PanelCore.State == MtkPanelStateIdle);
  }
  if (Operation == MtkPanelOperationLkSuspend) {
    return (BOOLEAN)(PanelCore.State == MtkPanelStateLkInitComplete);
  }
  if (Operation == MtkPanelOperationKernelPrepare) {
    return (BOOLEAN)((PanelCore.State == MtkPanelStateIdle) ||
                     (PanelCore.State == MtkPanelStateLkInitComplete) ||
                     (PanelCore.State == MtkPanelStateLkSuspendComplete) ||
                     (PanelCore.State == MtkPanelStateKernelUnprepared));
  }
  if (Operation == MtkPanelOperationKernelEnable) {
    return (BOOLEAN)(PanelCore.State == MtkPanelStateKernelPrepared);
  }
  if (Operation == MtkPanelOperationKernelUnprepare) {
    return (BOOLEAN)((PanelCore.State == MtkPanelStateKernelPrepared) ||
                     (PanelCore.State == MtkPanelStateKernelEnabled) ||
                     (PanelCore.State == MtkPanelStateLkInitComplete) ||
                     (PanelCore.State == MtkPanelStateLkSuspendComplete));
  }
  return FALSE;
}

static EFI_STATUS
PanelRunOperation (
  IN MTK_PANEL_BOARD_PROTOCOL *This,
  IN MTK_PANEL_OPERATION      Operation
  )
{
  EFI_STATUS Status;
  EFI_TPL PreviousTpl;
  MTK_PANEL_STATUS CoreStatus;

  if ((This != &PanelBoard) || (This->Revision != MTK_PANEL_BOARD_PROTOCOL_REVISION) ||
      ((Operation < MtkPanelOperationLkInit) ||
       (Operation > MtkPanelOperationKernelUnprepare))) {
    return EFI_INVALID_PARAMETER;
  }
  if (Exiting || !PanelCoreInitialized || (gBS == NULL) ||
      (gBS->RaiseTPL == NULL) || (gBS->RestoreTPL == NULL)) {
    return EFI_ABORTED;
  }

  // Raise only long enough to inspect the caller's current TPL. GPIO protocol
  // methods run at or below TPL_CALLBACK; no lease callback runs at NOTIFY.
  PreviousTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  gBS->RestoreTPL (PreviousTpl);
  if (Exiting) {
    return EFI_ABORTED;
  }
  if (PreviousTpl > TPL_CALLBACK) {
    return EFI_NOT_READY;
  }
  if (InterlockedCompareExchange32 (&PanelCallBusy, 0U, 1U) != 0U) {
    return EFI_NOT_READY;
  }

  if (Exiting) {
    Status = EFI_ABORTED;
    goto Done;
  }
  // The input driver must first stop polling and revoke its live report.
  // A temporary gap between report pin leases is not permission to reset it.
  if (TouchHandle && TouchState != HIMAX_TOUCH_STOPPED) {
    Status = EFI_NOT_READY;
    goto Done;
  }
  if (PanelLeaseUncertain) {
    Status = EFI_COMPROMISED_DATA;
    goto Done;
  }

  if (PanelLeaseHeld && PanelOperationComplete (PanelLeaseOperation)) {
    Status = PanelReleaseLease ();
    if (EFI_ERROR (Status)) {
      goto Done;
    }
    if (Exiting) {
      Status = EFI_ABORTED;
      goto Done;
    }
  }

  if (PanelOperationComplete (Operation)) {
    Status = EFI_SUCCESS;
    goto Done;
  }
  if (!PanelOperationAllowed (Operation)) {
    Status = EFI_NOT_READY;
    goto Done;
  }

  if (PanelLeaseHeld) {
    if (PanelLeaseOperation != Operation) {
      Status = EFI_NOT_READY;
      goto Done;
    }
  } else {
    if (PanelCore.PendingOperation != MtkPanelOperationNone) {
      Status = EFI_COMPROMISED_DATA;
      goto Done;
    }
    Status = PanelAcquireLease (Operation);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  Status = PanelCheckContext ();
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  switch (Operation) {
    case MtkPanelOperationLkInit:
      CoreStatus = MtkPanelCoreRunLkInit (&PanelCore);
      break;
    case MtkPanelOperationLkSuspend:
      CoreStatus = MtkPanelCoreRunLkSuspend (&PanelCore);
      break;
    case MtkPanelOperationKernelPrepare:
      CoreStatus = MtkPanelCoreRunKernelPrepare (&PanelCore);
      break;
    case MtkPanelOperationKernelEnable:
      CoreStatus = MtkPanelCoreRunKernelEnable (&PanelCore);
      break;
    case MtkPanelOperationKernelUnprepare:
      CoreStatus = MtkPanelCoreRunKernelUnprepare (&PanelCore);
      break;
    default:
      CoreStatus = MtkPanelInvalid;
      break;
  }
  if (CoreStatus != MtkPanelSuccess) {
    if (CoreStatus == MtkPanelInvalid) {
      Status = EFI_INVALID_PARAMETER;
    } else if ((CoreStatus == MtkPanelNotReady) || (CoreStatus == MtkPanelBusy)) {
      Status = EFI_NOT_READY;
    } else if (CoreStatus == MtkPanelMappingError) {
      Status = EFI_UNSUPPORTED;
    } else {
      Status = EFI_DEVICE_ERROR;
    }
    goto Done;
  }

  if (Exiting) {
    Status = EFI_ABORTED;
    goto Done;
  }
  Status = PanelCheckContext ();
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  Status = PanelReleaseLease ();

Done:
  (VOID)InterlockedCompareExchange32 (&PanelCallBusy, 1U, 0U);
  return Status;
}

static EFI_STATUS
EFIAPI
PanelRunInit (
  IN MTK_PANEL_BOARD_PROTOCOL *This
  )
{
  return PanelRunOperation (This, MtkPanelOperationLkInit);
}

static EFI_STATUS
EFIAPI
PanelRunSuspend (
  IN MTK_PANEL_BOARD_PROTOCOL *This
  )
{
  return PanelRunOperation (This, MtkPanelOperationLkSuspend);
}

static EFI_STATUS
EFIAPI
PanelRunKernelPrepare (
  IN MTK_PANEL_BOARD_PROTOCOL *This
  )
{
  return PanelRunOperation (This, MtkPanelOperationKernelPrepare);
}

static EFI_STATUS
EFIAPI
PanelRunKernelEnable (
  IN MTK_PANEL_BOARD_PROTOCOL *This
  )
{
  return PanelRunOperation (This, MtkPanelOperationKernelEnable);
}

static EFI_STATUS
EFIAPI
PanelRunKernelUnprepare (
  IN MTK_PANEL_BOARD_PROTOCOL *This
  )
{
  return PanelRunOperation (This, MtkPanelOperationKernelUnprepare);
}

static EFI_STATUS
InitializePanelCore (
  VOID
  )
{
  MTK_PANEL_IO Io;
  MTK_PANEL_STATUS Status;

  if (PanelCoreInitialized) {
    return EFI_SUCCESS;
  }
  ZeroMem (&Io, sizeof (Io));
  Io.Context = &PanelBoard;
  Io.ValidateGpio = PanelValidateGpio;
  Io.ReadGpio = PanelReadGpio;
  Io.SetMode = PanelSetMode;
  Io.SetDirection = PanelSetDirection;
  Io.SetValue = PanelSetValue;
  Io.DelayUs = PanelDelayUs;
  Status = MtkPanelCoreInitialize (&PanelCore, &Io);
  if (Status != MtkPanelSuccess) {
    return EFI_DEVICE_ERROR;
  }
  PanelCoreInitialized = TRUE;
  return EFI_SUCCESS;
}

static BOOLEAN ReadTouchGpioSnapshotWithLease(MTK_GPIO_PROTOCOL *Gpio,UINT64 Token,
                                              TOUCH_GPIO_SNAPSHOT *Snapshot) {
  TOUCH_GPIO_SNAPSHOT Observed;
  BOOLEAN Busy=TRUE;
  if(!Snapshot || !Gpio || !Gpio->IsBusy || !Gpio->ValidateLease || !Token)return FALSE;
  if(Gpio->ValidateLease(Token,152)!=EFI_SUCCESS)return FALSE;
  Observed.Mode=MmioRead32(0x10005430)&0xFU;
  Observed.Direction=MmioRead32(0x10005040)&(1U<<24);
  Observed.Latch=MmioRead32(0x10005140)&(1U<<24);
  Observed.Input=MmioRead32(0x10005240)&(1U<<24);
  Observed.Ies=MmioRead32(0x11E200D0)&(1U<<22);
  if(Gpio->ValidateLease(Token,152)!=EFI_SUCCESS ||
      Gpio->IsBusy(152,&Busy)!=EFI_SUCCESS || !Busy ||
      Observed.Mode!=0 || Observed.Direction!=(1U<<24) ||
      Observed.Latch!=(1U<<24) || Observed.Input!=(1U<<24) ||
      Observed.Ies!=(1U<<22))return FALSE;
  *Snapshot=Observed;
  return TRUE;
}
static BOOLEAN ReleaseTouchPinCleanup(VOID) {
  MTK_GPIO_PROTOCOL *Gpio=TouchPinLeaseOwner;
  BOOLEAN Busy=TRUE;
  if(!TouchPinCleanupPending)return TRUE;
  if(!Gpio || !Gpio->ReleasePins || !Gpio->IsBusy ||
      Gpio->ReleasePins(TouchPinCleanupToken)!=EFI_SUCCESS)return FALSE;
  TouchPinCleanupPending=FALSE;
  TouchPinCleanupToken=0;
  TouchPinLeaseOwner=NULL;
  return Gpio->IsBusy(152,&Busy)==EFI_SUCCESS && !Busy;
}
static BOOLEAN ReleaseProbePinLease(VOID) {
  MTK_GPIO_PROTOCOL *Gpio=ProbePinLeaseOwner;
  BOOLEAN Busy=TRUE;
  if(!ProbePinLeaseHeld)return TRUE;
  if(!Gpio || !Gpio->ReleasePins || !Gpio->IsBusy ||
      Gpio->ReleasePins(ProbePinLeaseToken)!=EFI_SUCCESS)return FALSE;
  ProbePinLeaseHeld=FALSE;
  ProbePinLeaseToken=0;
  ProbePinLeaseOwner=NULL;
  return Gpio->IsBusy(152,&Busy)==EFI_SUCCESS && !Busy;
}
static BOOLEAN AcquireProbePinLease(MTK_GPIO_PROTOCOL **Gpio) {
  MTK_GPIO_PROTOCOL *Protocol=NULL;
  EFI_STATUS Status;
  BOOLEAN Busy=TRUE;
  UINT32 Pin=152;
  UINT64 Token=0;
  if(!Gpio || !LocateTouchGpioProtocol(&Protocol))return FALSE;
  if(Protocol->IsBusy(Pin,&Busy)!=EFI_SUCCESS || Busy)return FALSE;
  Status=Protocol->AcquirePins(&Pin,1,&Token);
  if(Status!=EFI_SUCCESS)return FALSE;
  ProbePinLeaseOwner=Protocol;
  ProbePinLeaseToken=Token;
  ProbePinLeaseHeld=TRUE;
  *Gpio=Protocol;
  return Token && Protocol->ValidateLease(Token,Pin)==EFI_SUCCESS;
}
static BOOLEAN ReadTouchGpioSnapshot(TOUCH_GPIO_SNAPSHOT *Snapshot) {
  MTK_GPIO_PROTOCOL *Gpio=NULL;
  TOUCH_GPIO_SNAPSHOT Observed;
  EFI_STATUS Status;
  UINT32 Pin=152;
  UINT64 Token=0;
  BOOLEAN Busy=TRUE,Valid=FALSE;
  if(!Snapshot || !LiveReady() || !LocateTouchGpioProtocol(&Gpio))return FALSE;
  if(TouchPinCleanupPending) {
    if(Gpio!=TouchPinLeaseOwner || !ReleaseTouchPinCleanup())return FALSE;
  }
  Status=Gpio->IsBusy(Pin,&Busy);
  if(Status!=EFI_SUCCESS || Busy)return FALSE;
  Status=Gpio->AcquirePins(&Pin,1,&Token);
  if(Status!=EFI_SUCCESS)return FALSE;
  if(!Token) {
    TouchPinLeaseOwner=Gpio;
    TouchPinCleanupPending=TRUE;
    if(Gpio->ReleasePins(Token)==EFI_SUCCESS) {
      TouchPinCleanupPending=FALSE;
      TouchPinLeaseOwner=NULL;
    }
    return FALSE;
  }
  Valid=ReadTouchGpioSnapshotWithLease(Gpio,Token,&Observed);
  Status=Gpio->ReleasePins(Token);
  if(Status!=EFI_SUCCESS) {
    TouchPinLeaseOwner=Gpio;
    TouchPinCleanupToken=Token;
    TouchPinCleanupPending=TRUE;
    Valid=FALSE;
  }
  Busy=TRUE;
  if(Status==EFI_SUCCESS && (Gpio->IsBusy(Pin,&Busy)!=EFI_SUCCESS || Busy))Valid=FALSE;
  if(Valid)*Snapshot=Observed;
  return Valid;
}
static BOOLEAN TouchGpioReady(TOUCH_GPIO_SNAPSHOT *Snapshot) {
  if(!ReadTouchGpioSnapshot(Snapshot))return FALSE;
  // GPIO152 is sampled under a temporary lease; reset is a pre-existing
  // deasserted output and is never changed by this observer.
  // GPIO9 remains an inherited input and is never offered as an output.
  return (MmioRead32(0x11EA0080)&1U) &&
    !(MmioRead32(0x10005310)&0xF0) && !(MmioRead32(0x10005000)&(1U<<9));
}
static BOOLEAN TouchGpioReadyWithLease(MTK_GPIO_PROTOCOL *Gpio,UINT64 Token,
                                       TOUCH_GPIO_SNAPSHOT *Snapshot) {
  if(!ReadTouchGpioSnapshotWithLease(Gpio,Token,Snapshot))return FALSE;
  return (MmioRead32(0x11EA0080)&1U) &&
    !(MmioRead32(0x10005310)&0xF0) && !(MmioRead32(0x10005000)&(1U<<9));
}
static BOOLEAN EFIAPI TouchValidateGpioLease(VOID *Context,MTK_GPIO_PROTOCOL *Gpio,
                                              UINT64 Token) {
  VOID *Fdt=NULL;
  UINTN Bytes=0;
  MTK_GPIO_PROTOCOL *ActiveGpio=NULL;
  TOUCH_GPIO_SNAPSHOT Snapshot;
  return Context==&TouchBoard && !Exiting && !PanelCallBusy && !PanelLeaseHeld &&
    !PanelLeaseUncertain && TouchHandle && Token && Gpio &&
    LocateTouchGpioProtocol(&ActiveGpio) && ActiveGpio==Gpio &&
    RuntimeTree(&Fdt,&Bytes) && Fdt==TouchFdt && Bytes==TouchBytes &&
    LiveReady() &&
    TouchGpioReadyWithLease(Gpio,Token,&Snapshot);
}
static BOOLEAN LiveTouchReady(VOID) {
  TOUCH_GPIO_SNAPSHOT Snapshot;
  return TouchGpioReady(&Snapshot);
}
static BOOLEAN EFIAPI TouchReady(VOID *Context) {
  VOID *Fdt;UINTN Bytes;
  return Context==&TouchBoard && !Exiting && !PanelCallBusy && !PanelLeaseHeld &&
    !PanelLeaseUncertain && TouchHandle && RuntimeTree(&Fdt,&Bytes) &&
    Fdt==TouchFdt && Bytes==TouchBytes && LiveTouchReady();
}
static BOOLEAN EFIAPI TouchPending(VOID *Context) {
  return TouchReady(Context) && !(MmioRead32(0x10005200)&(1U<<9));
}
static VOID EFIAPI TouchStateUpdate(VOID *Context,UINT32 State) {
  if(Context!=&TouchBoard || Exiting)return;
  if(State==HIMAX_TOUCH_STOPPED || State==HIMAX_TOUCH_STARTING)TouchState=State;
  else if(TouchState!=HIMAX_TOUCH_STOPPED)
    TouchState=((State==HIMAX_TOUCH_READY || State==HIMAX_TOUCH_REPORT_VALID) &&
      TouchReady(Context))?State:HIMAX_TOUCH_REPORT_INVALID;
}
static HM_STATUS ProbeTransfer(VOID *Context,const unsigned char *Write,unsigned WriteBytes,
                               unsigned char *Read,unsigned ReadBytes) {
  EFI_I2C_MASTER_PROTOCOL *Master=Context;
  struct { UINTN OperationCount;EFI_I2C_OPERATION Operation[2]; } Packet;
  if(Exiting || !ProbePinLeaseHeld || !ProbePinLeaseOwner ||
      ProbePinLeaseOwner->ValidateLease(ProbePinLeaseToken,152)!=EFI_SUCCESS)return HmIoError;
  if(!Master || !Master->StartRequest || !Write || !WriteBytes ||
     WriteBytes>HM_BUS_WRITE_MAX_BODY+1 || ReadBytes>HM_BUS_READ_MAX_BYTES ||
     (ReadBytes && !Read))return HmInvalid;
  ZeroMem(&Packet,sizeof(Packet));Packet.OperationCount=ReadBytes?2:1;
  Packet.Operation[0].LengthInBytes=WriteBytes;Packet.Operation[0].Buffer=(UINT8 *)Write;
  if(ReadBytes) { Packet.Operation[1].Flags=I2C_FLAG_READ;
    Packet.Operation[1].LengthInBytes=ReadBytes;Packet.Operation[1].Buffer=Read; }
  if(EFI_ERROR(Master->StartRequest(Master,0x48,(EFI_I2C_REQUEST_PACKET *)&Packet,NULL,NULL)) ||
     Exiting || ProbePinLeaseOwner->ValidateLease(ProbePinLeaseToken,152)!=EFI_SUCCESS)return HmIoError;
  return HmOk;
}
static VOID ProbeDelay(VOID *Context,unsigned MinimumUs,unsigned MaximumUs) {
  (VOID)Context;(VOID)MaximumUs;MicroSecondDelay(MinimumUs);
}
static BOOLEAN BusMatches(MTK_I2C_CONTROLLER *B) {
  return B && B->Revision==MTK_I2C_CONTROLLER_REVISION && B->Flags==MTK_I2C_REQUIRED_FLAGS &&
    B->ControllerBase==0x11E00000 && B->ControllerSize==0x1000 &&
    B->DmaBase==0x10217080 && B->DmaSize==0x80 && B->SourceClockHz>=1000000 &&
    B->SourceClockHz<=1000000000 && !B->IommuBase && !B->DmaId;
}
// Board publication is not driver binding. In particular, a timer can
// publish after BDS's last ConnectAll, or Start can fail transiently. Retry
// only binding (never reprobe/reset), and stop once the pointer is present.
static VOID ConnectTouch(VOID) {
  VOID *Pointer=NULL;
  EFI_STATUS Status;
  if(Exiting || !TouchHandle || TouchConnectBusy || ProbeOwned || ProbePinLeaseHeld ||
     TouchPinCleanupPending || PanelCallBusy || PanelLeaseHeld || PanelLeaseUncertain)return;
  Status=gBS->HandleProtocol(TouchHandle,&gEfiAbsolutePointerProtocolGuid,&Pointer);
  if(Status==EFI_SUCCESS && Pointer)return;
  if(Status!=EFI_NOT_FOUND && Status!=EFI_UNSUPPORTED)return;
  if(TouchConnectAttempts>=TOUCH_CONNECT_LIMIT)return;
  if(TouchConnectCountdown) { TouchConnectCountdown--;return; }
  // Supported/Start revalidate the board and acquire their own leases.
  TouchConnectBusy=TRUE;
  TouchConnectAttempts++;
  (VOID)gBS->ConnectController(TouchHandle,NULL,NULL,FALSE);
  TouchConnectBusy=FALSE;
  if(Exiting)return;
  // A success status alone does not establish an operational consumer.
  Pointer=NULL;
  Status=gBS->HandleProtocol(TouchHandle,&gEfiAbsolutePointerProtocolGuid,&Pointer);
  if(Status==EFI_SUCCESS && Pointer)return;
  TouchConnectDelay=TouchConnectDelay?MIN(TouchConnectDelay*2U,50U):1U;
  TouchConnectCountdown=TouchConnectDelay;
}
static VOID EFIAPI ObserveTouch(EFI_EVENT Event,VOID *Context) {
  VOID *Fdt,*Existing;UINTN Bytes,Count=0;EFI_HANDLE *Handles=NULL,Candidate=NULL;
  EFI_I2C_MASTER_PROTOCOL *Master;MTK_GPIO_PROTOCOL *Gpio=NULL;
  EFI_STATUS S;EFI_TPL OldTpl;BOOLEAN Valid=FALSE,HadOutstanding=FALSE;
  TOUCH_GPIO_SNAPSHOT Before,After;
  (VOID)Event;(VOID)Context;
  // Cleanup always precedes new work. In particular, a failed ExitBootServices
  // attempt may return to DXE with both protocol and pin ownership retained.
  if(ProbeOwned) {
    if(gBS->CloseProtocol(ProbeOwned,&gEfiI2cMasterProtocolGuid,Image,ProbeOwned)!=EFI_SUCCESS)return;
    ProbeOwned=NULL;
    HadOutstanding=TRUE;
  }
  if(ProbePinLeaseHeld) {
    if(!ReleaseProbePinLease())return;
    HadOutstanding=TRUE;
  }
  if(TouchPinCleanupPending && !ReleaseTouchPinCleanup())return;
  if(HadOutstanding || Exiting || PanelCallBusy || PanelLeaseHeld || PanelLeaseUncertain)return;
  if(TouchHandle) { ConnectTouch();return; }
  if(ProbeAttempts>=3)return;
  // A missing live EINT mapping or masked-domain state cannot host touch.
  // Reject that state before re-scanning the full DTB on every 10 ms tick.
  // A ready controller still revalidates the complete tree before probing.
  if(!LiveReady() || !RuntimeTree(&Fdt,&Bytes) ||
     EFI_ERROR(ValidateTree(Fdt,Bytes)) || EFI_ERROR(ValidateTouchTree(Fdt)))return;
  S=gBS->LocateHandleBuffer(ByProtocol,&gEfiI2cMasterProtocolGuid,NULL,&Count,&Handles);
  if(S!=EFI_SUCCESS)return;
  for(UINTN I=0;I<Count;I++) {
    MTK_I2C_CONTROLLER *B;
    if(!EFI_ERROR(gBS->HandleProtocol(Handles[I],&gMediaTekI2cControllerProtocolGuid,(VOID **)&B)) && BusMatches(B)) {
      if(Candidate) { FreePool(Handles);return; }
      Candidate=Handles[I];
    }
  }
  FreePool(Handles);
  if(!Candidate || Exiting)return;
  S=gBS->HandleProtocol(Candidate,&gHimaxTouchBoardProtocolGuid,&Existing);
  if(S!=EFI_NOT_FOUND && S!=EFI_UNSUPPORTED)return;
  if(!AcquireProbePinLease(&Gpio)) {
    if(ProbePinLeaseHeld)(VOID)ReleaseProbePinLease();
    return;
  }
  // The same token protects both GPIO snapshots and every I2C operation.
  // A panel owner cannot acquire or change GPIO152 until the probe is closed.
  if(Exiting || !TouchGpioReadyWithLease(Gpio,ProbePinLeaseToken,&Before))goto ReleasePin;
  S=gBS->OpenProtocol(Candidate,&gEfiI2cMasterProtocolGuid,(VOID **)&Master,Image,Candidate,EFI_OPEN_PROTOCOL_BY_DRIVER);
  if(S!=EFI_SUCCESS)goto ReleasePin;
  ProbeOwned=Candidate;ProbeAttempts++;
  if(Master && Master->SetBusFrequency && Master->StartRequest) {
    UINTN Hertz=400000;HM_IO Io={Master,ProbeTransfer,ProbeDelay};
    S=Exiting?EFI_ABORTED:Master->SetBusFrequency(Master,&Hertz);
    // Read identity and firmware state without consuming a pending input report.
    if(!Exiting && S==EFI_SUCCESS && Hertz>=100000 && Hertz<=400000 &&
        HmEnter(&Io,HmEntryRetainedResume)==HmOk && !Exiting &&
        TouchGpioReadyWithLease(Gpio,ProbePinLeaseToken,&After) &&
        CompareMem(&Before,&After,sizeof(Before))==0)Valid=TRUE;
  }
  // Do not continue issuing requests after an EBS callback interrupted this
  // probe. A failed EBS call returns here, allowing normal ownership cleanup.
  if(gBS->CloseProtocol(Candidate,&gEfiI2cMasterProtocolGuid,Image,Candidate)!=EFI_SUCCESS)return;
  ProbeOwned=NULL;
ReleasePin:
  if(!ReleaseProbePinLease())return;
  if(!Valid || Exiting)return;
  // Serialize final publication against the TPL_NOTIFY ExitBootServices
  // callback. If EBS has started, the probe remains unadvertised.
  OldTpl=gBS->RaiseTPL(TPL_NOTIFY);
  if(Exiting) {
    gBS->RestoreTPL(OldTpl);
    return;
  }
  TouchFdt=Fdt;TouchBytes=Bytes;TouchState=HIMAX_TOUCH_STOPPED;TouchHandle=Candidate;
  TouchBoard=(HIMAX_TOUCH_BOARD){HIMAX_TOUCH_REVISION,HIMAX_TOUCH_REQUIRED_FLAGS,0x48,10,1400,2240,
    &TouchBoard,TouchReady,TouchPending,TouchStateUpdate,TouchValidateGpioLease};
  gBS->RestoreTPL(OldTpl);
  if(Exiting)return;
  S=gBS->InstallMultipleProtocolInterfaces(&TouchHandle,&gHimaxTouchBoardProtocolGuid,&TouchBoard,NULL);
  if(S!=EFI_SUCCESS)TouchHandle=NULL;
  if(S!=EFI_SUCCESS || Exiting)return;
  // At TPL_CALLBACK, as in EDK2 USB enumeration. No probe ownership remains.
  ConnectTouch();
}
static VOID EFIAPI ExportToAcpi(EFI_EVENT Event,VOID *Context) {
  VOID *Fdt=NULL;UINT64 Bytes;BOOLEAN Ready=FALSE,Touch=FALSE,Hall=FALSE,WasExiting=Exiting;
  BOOLEAN PanelPending,ChargerIrq=FALSE;
  Exiting=TRUE;
  PanelPending=PanelLeaseHeld || PanelLeaseUncertain || (PanelCallBusy!=0U);
  (VOID)Event;(VOID)Context;
  // Clear first, including on repeated ExitBootServices attempts.
  MmioWrite32(HANDOFF,0);MmioWrite32(HANDOFF+12,0);
  MmioWrite32(0x43F7F0F0,0);MmioWrite32(0x43F7F0FC,0);MemoryFence();
  if(!EFI_ERROR(EfiGetSystemConfigurationTable(&gFdtTableGuid,&Fdt)) && Fdt &&
      MmioRead64(0x43F7F000)==0x0000000148303754ULL &&
      MmioRead64(0x43F7F048)==(UINT64)(UINTN)Fdt && MmioRead64(0x43F7F058)==EFI_SUCCESS) {
    Bytes=MmioRead64(0x43F7F050);
    Ready=!EFI_ERROR(ValidateTree(Fdt,(UINTN)Bytes)) && LiveReady();
    Hall=Ready && !EFI_ERROR(ValidateHallTree(Fdt)) && LiveHallReady();
    ChargerIrq=Ready && ChargerIrqReady(Fdt);
  }
  if(!WasExiting && !PanelPending && Ready && TouchHandle && !ProbeOwned && !ProbePinLeaseHeld &&
      !TouchPinCleanupPending &&
      (TouchState==HIMAX_TOUCH_READY || TouchState==HIMAX_TOUCH_REPORT_VALID) &&
      Fdt==TouchFdt && Bytes==TouchBytes && !EFI_ERROR(ValidateTouchTree(Fdt)) && LiveTouchReady())Touch=TRUE;
  MmioWrite32(0x43F7F0A0,0);MmioWrite32(0x43F7F0AC,0);MemoryFence();
  MmioWrite32(0x43F7F0A4,Touch?0x48:0);MmioWrite32(0x43F7F0A8,Touch?9:0);
  MmioWrite32(0x43F7F0AC,1);MemoryFence();MmioWrite32(0x43F7F0A0,Touch?1:0);MemoryFence();
  WriteBackDataCacheRange((VOID *)(UINTN)0x43F7F0A0,16);
  MmioWrite32(0x43F7F0F4,0);MmioWrite32(0x43F7F0F8,0);
  MmioWrite32(0x43F7F0FC,1);MemoryFence();MmioWrite32(0x43F7F0F0,Hall?1:0);MemoryFence();
  WriteBackDataCacheRange((VOID *)(UINTN)0x43F7F0F0,16);
  MmioWrite32(CHARGER_IRQ_HANDOFF,ChargerIrq?1:0);MemoryFence();
  WriteBackDataCacheRange((VOID *)(UINTN)CHARGER_IRQ_HANDOFF,sizeof(UINT32));
  MmioWrite32(HANDOFF+4,Ready?188:0);MmioWrite32(HANDOFF+8,Ready?126:0);
  MmioWrite32(HANDOFF+12,1);MemoryFence();MmioWrite32(HANDOFF,Ready?1:0);MemoryFence();
  WriteBackDataCacheRange((VOID *)(UINTN)HANDOFF,16);
}
EFI_STATUS EFIAPI T70GpioPlatformEntry(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS S;
  (VOID)SystemTable;Image=ImageHandle;
  S=InitializePanelCore();
  if(EFI_ERROR(S))return S;
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExportToAcpi,NULL,&gEfiEventExitBootServicesGuid,&ExitEvent);
  if(EFI_ERROR(S))return S;
  S=gBS->InstallMultipleProtocolInterfaces(&PanelHandle,&gMtkPanelBoardProtocolGuid,&PanelBoard,NULL);
  if(EFI_ERROR(S)) {
    if(gBS->CloseEvent && gBS->CloseEvent(ExitEvent)==EFI_SUCCESS) {
      ExitEvent=NULL;
      PanelHandle=NULL;
      return S;
    }
    // Keep the image resident because the registered EBS callback still points
    // into it. Absence of the protocol prevents a display consumer from acting.
    return EFI_SUCCESS;
  }
  S=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,ObserveTouch,NULL,&TouchTimer);
  if(!EFI_ERROR(S)) (VOID)gBS->SetTimer(TouchTimer,TimerPeriodic,100000);
  // The EBS callback already references this image, so keep it resident even
  // when optional observation is unavailable. No touch readiness is invented.
  return EFI_SUCCESS;
}
