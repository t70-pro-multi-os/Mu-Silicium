// SPDX-License-Identifier: BSD-2-Clause-Patent
// Prepare DT-confirmed AP I2C clock gates and pin functions.
#include <Uefi.h>
#include <PiDxe.h>
#include <Guid/Fdt.h>
#include <Guid/EventGroup.h>
#include <Protocol/DevicePath.h>
#include <Protocol/I2cMaster.h>
#include <Protocol/MtkClock.h>
#include <Protocol/MtkGpio.h>
#include <Protocol/MtkI2cController.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
// Keep indices 0 and 1 for the existing OS handoff readiness bits.
enum { I2c0Index, I2c6Index, I2c1Index, I2cBusCount };
static const UINT64 Bases[I2cBusCount]={0x11e00000,0x1101a000,0x11e01000};
static const UINT64 Dmas[I2cBusCount]={0x10217080,0x10217480,0x10217100};
static const UINT64 Wraps[I2cBusCount]={0x11e02000,0x1101b000,0x11e02000};
// AP SCL/SDA use mode 1 on adjacent pins in the stock MT6789 pinctrl table.
static const UINT32 Pins[I2cBusCount]={130,142,132};
static const CHAR8 *MainClockNames[I2cBusCount]={
  "IMPW_AP_CLOCK_I2C0","IMPC_AP_CLOCK_I2C6","IMPW_AP_CLOCK_I2C1"
};
static const CHAR8 DmaClockName[]="IFRAO_AP_DMA";
static const CHAR8 *Paths[I2cBusCount]={
  "/soc/i2c@11e00000","/soc/i2c@1101a000","/soc/i2c@11e01000"
};
static const CHAR8 *WrapCompatible[I2cBusCount]={
  "mediatek,mt6789-imp_iic_wrap_w","mediatek,mt6789-imp_iic_wrap_c",
  "mediatek,mt6789-imp_iic_wrap_w"
};
static const UINT32 WrapIds[I2cBusCount]={0,2,1};
static const UINT32 BusNumbers[I2cBusCount]={0,6,1};
static MTK_I2C_CONTROLLER Boards[I2cBusCount];
static EFI_HANDLE Handles[I2cBusCount];
static EFI_EVENT RetryEvent,ExitEvent,FdtEvent;
static MTK_CLOCK_PROTOCOL *CachedClock;
static BOOLEAN Exiting,BindingBusy;
static EFI_EVENT BindingEvent;
static UINT32 PrepareCountdown;
static VOID EFIAPI Observe(EFI_EVENT Event,VOID *Context);
static UINT32 BindingAttempts[I2cBusCount],BindingDelay[I2cBusCount],BindingCountdown[I2cBusCount];
// Publication can happen after BDS ConnectAll. Retry binding only, never bus
// preparation, at CALLBACK TPL; a successful ConnectController is not proof
// that the expected driver actually installed its master protocol.
static VOID EFIAPI BindControllers(EFI_EVENT Event,VOID *Context) {
  EFI_TPL OldTpl;
  (VOID)Event;(VOID)Context;
  OldTpl=gBS->RaiseTPL(TPL_NOTIFY);
  if(Exiting || BindingBusy) { gBS->RestoreTPL(OldTpl);return; }
  BindingBusy=TRUE;
  gBS->RestoreTPL(OldTpl);
  // A bus that was not idle or clocked when the FDT appeared stays worth
  // another try. No event announces that state change on a cold boot.
  if(Event && (!Handles[I2c6Index]) && PrepareCountdown--==0U) {
    PrepareCountdown=20U;
    Observe(NULL,&BindingBusy);
  }
  for(UINTN I=0;I<I2cBusCount && !Exiting;I++) {
    VOID *Master=NULL;EFI_STATUS S;
    if(!Handles[I])continue;
    S=gBS->HandleProtocol(Handles[I],&gEfiI2cMasterProtocolGuid,&Master);
    if(S==EFI_SUCCESS && Master)continue;
    if(S!=EFI_NOT_FOUND && S!=EFI_UNSUPPORTED)continue;
    if(BindingAttempts[I]>=16U)continue;
    if(BindingCountdown[I]) { BindingCountdown[I]--;continue; }
    BindingAttempts[I]++;
    (VOID)gBS->ConnectController(Handles[I],NULL,NULL,FALSE);
    if(Exiting)break;
    BindingDelay[I]=BindingDelay[I]?MIN(BindingDelay[I]*2U,50U):1U;
    BindingCountdown[I]=BindingDelay[I];
  }
  OldTpl=gBS->RaiseTPL(TPL_NOTIFY);
  BindingBusy=FALSE;
  gBS->RestoreTPL(OldTpl);
}
#pragma pack(1)
typedef struct { VENDOR_DEVICE_PATH Vendor; UINT32 Bus; EFI_DEVICE_PATH_PROTOCOL End; } BOARD_PATH;
#pragma pack()
static BOARD_PATH DevicePaths[I2cBusCount];
static UINT64 Cells(const VOID *P,UINTN N) {
  const UINT8 *B=P;UINT64 V=0;
  for(UINTN I=0;I<N;I++)V=(V<<8)|B[I];
  return V;
}
static BOOLEAN Enabled(const VOID *Fdt,INT32 Node) {
  INT32 N;const VOID *P=FdtGetProp(Fdt,Node,"status",&N);
  return !P || (N==5 && CompareMem(P,"okay",5)==0) || (N==3 && CompareMem(P,"ok",3)==0);
}
static BOOLEAN Compatible(const VOID *Fdt,INT32 Node,const CHAR8 *Name) {
  INT32 Length;const CHAR8 *P=FdtGetProp(Fdt,Node,"compatible",&Length);
  UINTN Wanted=AsciiStrLen(Name);
  if(!P || Length<=0)return FALSE;
  for(UINTN Offset=0;Offset<(UINTN)Length;) {
    UINTN End=Offset;
    while(End<(UINTN)Length && P[End])End++;
    if(End==(UINTN)Length)return FALSE;
    if(End-Offset==Wanted && CompareMem(P+Offset,Name,Wanted)==0)return TRUE;
    Offset=End+1;
  }
  return FALSE;
}
static EFI_STATUS ValidateTree(const VOID *Fdt,UINTN Bytes,UINTN Index) {
  INT32 Node,Soc,Length,Provider;const UINT8 *P;
  if(Index>=I2cBusCount || !Fdt || Bytes<sizeof(FDT_HEADER) || Bytes>0x200000 ||
     FdtCheckFull(Fdt,Bytes) || FdtTotalSize(Fdt)!=Bytes)return EFI_COMPROMISED_DATA;
  if(!Compatible(Fdt,0,"mediatek,MT6789"))return EFI_UNSUPPORTED;
  Soc=FdtPathOffset(Fdt,"/soc");Node=FdtPathOffset(Fdt,Paths[Index]);
  if(Soc<0 || Node<0 || !Enabled(Fdt,Soc) || !Enabled(Fdt,Node) ||
     FdtAddressCells(Fdt,Soc)!=2 || FdtSizeCells(Fdt,Soc)!=2 ||
     !Compatible(Fdt,Node,"mediatek,mt6983-i2c"))return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Node,"reg",&Length);
  if(!P || Length!=32 || Cells(P,8)!=Bases[Index] || Cells(P+8,8)!=0x1000 ||
     Cells(P+16,8)!=Dmas[Index] || Cells(P+24,8)!=0x80)return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Node,"clock-div",&Length);
  if(!P || Length!=4 || Cells(P,4)!=1)return EFI_UNSUPPORTED;
  // APDMA is a direct 36-bit bus in this board tree. Reject translation,
  // streaming IOMMUs, coherent assumptions and inherited IOMMU maps.
  P=FdtGetProp(Fdt,Soc,"dma-ranges",&Length);
  if(!P || Length!=24 || Cells(P,8)!=0 || Cells(P+8,8)!=0 ||
     Cells(P+16,8)!=(1ULL<<36))return EFI_UNSUPPORTED;
  for(INT32 Level=Node;Level>=0;Level=FdtParentOffset(Fdt,Level)) {
    if(FdtGetProp(Fdt,Level,"iommus",&Length) ||
       FdtGetProp(Fdt,Level,"iommu-map",&Length) ||
       FdtGetProp(Fdt,Level,"dma-coherent",&Length))return EFI_UNSUPPORTED;
    if(Level!=Soc && Level!=Node && FdtGetProp(Fdt,Level,"dma-ranges",&Length))return EFI_UNSUPPORTED;
  }
  P=FdtGetProp(Fdt,Node,"clock-names",&Length);
  if(!P || Length!=9 || CompareMem(P,"main\0dma",9)!=0)return EFI_UNSUPPORTED;
  P=FdtGetProp(Fdt,Node,"clocks",&Length);
  if(!P || Length!=16 || Cells(P+4,4)!=WrapIds[Index] || Cells(P+12,4)!=61)return EFI_UNSUPPORTED;
  Provider=FdtNodeOffsetByPhandle(Fdt,(UINT32)Cells(P,4));
  if(Provider<0 || !Enabled(Fdt,Provider) || !Compatible(Fdt,Provider,WrapCompatible[Index]))return EFI_UNSUPPORTED;
  const UINT8 *Reg=FdtGetProp(Fdt,Provider,"reg",&Length);
  if(!Reg || Length!=16 || Cells(Reg,8)!=Wraps[Index] || Cells(Reg+8,8)!=0x1000)return EFI_UNSUPPORTED;
  Provider=FdtNodeOffsetByPhandle(Fdt,(UINT32)Cells(P+8,4));
  Reg=(Provider<0 || !Enabled(Fdt,Provider))?NULL:FdtGetProp(Fdt,Provider,"reg",&Length);
  if(!Reg || Length!=16 || Cells(Reg,8)!=0x10001000 || Cells(Reg+8,8)!=0x1000)return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
static EFI_STATUS Mapped(UINT64 Base,UINT64 Bytes) {
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;EFI_STATUS S=gDS->GetMemorySpaceDescriptor(Base,&D);
  if(EFI_ERROR(S) || D.GcdMemoryType!=EfiGcdMemoryTypeMemoryMappedIo ||
     Base<D.BaseAddress || Base-D.BaseAddress>D.Length || Bytes>D.Length-(Base-D.BaseAddress) ||
     !(D.Attributes&EFI_MEMORY_UC))return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
static EFI_STATUS ClockReady(MTK_CLOCK_PROTOCOL *Clock,const CHAR8 *Name,BOOLEAN Mux) {
  UINT32 Id,Parent;BOOLEAN On;EFI_STATUS S;
  if(!Clock || !Clock->GetId || !Clock->IsEnabled || (Mux && !Clock->GetParent))return EFI_INVALID_PARAMETER;
  S=Clock->GetId(Name,&Id);
  if(EFI_ERROR(S))return S;
  S=Clock->IsEnabled(Id,&On);if(EFI_ERROR(S) || !On)return EFI_NOT_READY;
  if(Mux) {
    S=Clock->GetParent(Id,&Parent);if(EFI_ERROR(S))return S;
    S=Clock->IsEnabled(Parent,&On);if(EFI_ERROR(S) || !On)return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS ValidateUpstreamClocks(MTK_CLOCK_PROTOCOL *Clock) {
  CONST CHAR8 *Names[]={"TOP_I2C_SEL","TOP_AXI_SEL","IFRAO_I2C_PSEUDO",
    "IFRAO_APDMA_PSEUDO","TOP_APDMA","TOP_I2C_PSEUDO"};
  EFI_STATUS S;
  if(!Clock || !Clock->GetId || !Clock->IsEnabled || !Clock->GetParent)return EFI_INVALID_PARAMETER;
  // The selectors' selected parents and both pseudo-clock chains must be
  // live before any controller or APDMA MMIO is touched.
  for(UINTN I=0;I<ARRAY_SIZE(Names);I++) {
    S=ClockReady(Clock,Names[I],I<2);
    if(EFI_ERROR(S))return S;
  }
  return EFI_SUCCESS;
}
// LK leaves these gates on only when it used I2C itself. Open a closed gate
// the way the stock clock framework does before the controller is prepared.
static VOID EnableUpstreamGates(MTK_CLOCK_PROTOCOL *Clock) {
  static CONST CHAR8 *Names[]={"IFRAO_I2C_PSEUDO","IFRAO_APDMA_PSEUDO","TOP_APDMA","TOP_I2C_PSEUDO"};
  if(!Clock || !Clock->GetId || !Clock->IsEnabled || !Clock->SetEnable)return;
  for(UINTN I=0;I<ARRAY_SIZE(Names);I++) {
    UINT32 Id;BOOLEAN On;
    if(EFI_ERROR(Clock->GetId(Names[I],&Id)) || EFI_ERROR(Clock->IsEnabled(Id,&On)) || On)continue;
    (VOID)Clock->SetEnable(Id,TRUE);
  }
}
static EFI_STATUS ValidateMapped(UINTN Index) {
  if(Index>=I2cBusCount)return EFI_INVALID_PARAMETER;
  if(EFI_ERROR(Mapped(Bases[Index],0x1000)) || EFI_ERROR(Mapped(Dmas[Index],0x80)) ||
     EFI_ERROR(Mapped(Wraps[Index],0x1000)) || EFI_ERROR(Mapped(0x10005000,0x1000)))return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
static UINT32 PinMode(UINT32 Pin) {
  return (MmioRead32(0x10005000+0x300+(Pin/8)*0x10)>>((Pin%8)*4))&0xf;
}
static EFI_STATUS PinsHigh(MTK_GPIO_PROTOCOL *Gpio,UINTN Index) {
  BOOLEAN State;
  if(!Gpio || !Gpio->GetState || Index>=I2cBusCount)return EFI_INVALID_PARAMETER;
  for(UINTN I=0;I<2;I++) {
    EFI_STATUS S=Gpio->GetState(Pins[Index]+I,&State);
    if(EFI_ERROR(S))return S;
    if(!State)return EFI_NOT_READY;
  }
  MicroSecondDelay(10);
  for(UINTN I=0;I<2;I++) {
    EFI_STATUS S=Gpio->GetState(Pins[Index]+I,&State);
    if(EFI_ERROR(S))return S;
    if(!State)return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS PinsHighMmio(UINTN Index) {
  UINT32 Pin=Pins[Index],Mask=3U<<(Pin%32);
  return (MmioRead32(0x10005000+0x200+(Pin/32)*0x10)&Mask)==Mask ? EFI_SUCCESS : EFI_NOT_READY;
}
static EFI_STATUS ControllerIdle(UINTN Index) {
  for(UINTN I=0;I<2;I++) {
    if((MmioRead32(Bases[Index]+0x24)&1) || (MmioRead32(Dmas[Index]+8)&1))return EFI_NOT_READY;
    if(I==0)MicroSecondDelay(10);
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EnableBusClocks(MTK_CLOCK_PROTOCOL *Clock,UINTN Index,
  UINT32 *DmaId,UINT32 *MainId,BOOLEAN *DmaWasOn,BOOLEAN *MainWasOn,BOOLEAN *SnapshotValid) {
  EFI_STATUS S;
  if(!Clock || !Clock->GetId || !Clock->IsEnabled || !Clock->SetEnable || Index>=I2cBusCount ||
     !DmaId || !MainId || !DmaWasOn || !MainWasOn || !SnapshotValid)return EFI_INVALID_PARAMETER;
  *SnapshotValid=FALSE;*DmaId=*MainId=0;*DmaWasOn=*MainWasOn=FALSE;
  S=Clock->GetId(DmaClockName,DmaId);if(EFI_ERROR(S))return S;
  S=Clock->GetId(MainClockNames[Index],MainId);if(EFI_ERROR(S))return S;
  S=Clock->IsEnabled(*DmaId,DmaWasOn);if(EFI_ERROR(S))return S;
  S=Clock->IsEnabled(*MainId,MainWasOn);if(EFI_ERROR(S))return S;
  *SnapshotValid=TRUE;
  // Match the stock driver: APDMA first, then the controller wrapper gate.
  if(!*DmaWasOn) {S=Clock->SetEnable(*DmaId,TRUE);if(EFI_ERROR(S))return S;}
  BOOLEAN On;
  S=Clock->IsEnabled(*DmaId,&On);if(EFI_ERROR(S))return S;if(!On)return EFI_NOT_READY;
  if(!*MainWasOn) {S=Clock->SetEnable(*MainId,TRUE);if(EFI_ERROR(S))return S;}
  S=Clock->IsEnabled(*MainId,&On);if(EFI_ERROR(S))return S;if(!On)return EFI_NOT_READY;
  return EFI_SUCCESS;
}
static EFI_STATUS RestoreClock(MTK_CLOCK_PROTOCOL *Clock,UINT32 Id,BOOLEAN WasOn) {
  BOOLEAN On;EFI_STATUS S;
  if(WasOn)return EFI_SUCCESS;
  S=Clock->IsEnabled(Id,&On);if(EFI_ERROR(S) || !On)return S;
  S=Clock->SetEnable(Id,FALSE);if(EFI_ERROR(S))return S;
  S=Clock->IsEnabled(Id,&On);if(EFI_ERROR(S))return S;
  return On?EFI_NOT_READY:EFI_SUCCESS;
}
static EFI_STATUS RestorePins(MTK_GPIO_PROTOCOL *Gpio,UINTN Index,
  const UINT32 OriginalMode[2],UINT32 ChangedMask) {
  EFI_STATUS S;
  S=PinsHigh(Gpio,Index);if(EFI_ERROR(S))return S;
  S=ControllerIdle(Index);if(EFI_ERROR(S))return S;
  if(ChangedMask==0)return EFI_SUCCESS;
  for(UINTN I=2;I>0;I--) {
    UINTN J=I-1;
    if(!(ChangedMask&(1U<<J)))continue;
    UINT32 Pin=Pins[Index]+J,Current=PinMode(Pin);
    if(Current==OriginalMode[J])continue;
    if(Current!=1 || OriginalMode[J]!=0)return EFI_NOT_READY;
    S=Gpio->SetMode(Pin,OriginalMode[J]);
    if(EFI_ERROR(S) || PinMode(Pin)!=OriginalMode[J])return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS RollbackPreparation(MTK_CLOCK_PROTOCOL *Clock,MTK_GPIO_PROTOCOL *Gpio,
  UINTN Index,UINT32 DmaId,UINT32 MainId,BOOLEAN DmaWasOn,BOOLEAN MainWasOn,
  const UINT32 OriginalMode[2],UINT32 ChangedMask) {
  EFI_STATUS S;BOOLEAN DmaOn=FALSE,MainOn=FALSE;
  BOOLEAN NeedRestore=(ChangedMask!=0);
  S=Clock->IsEnabled(DmaId,&DmaOn);if(EFI_ERROR(S))return EFI_DEVICE_ERROR;
  S=Clock->IsEnabled(MainId,&MainOn);if(EFI_ERROR(S))return EFI_DEVICE_ERROR;
  if(!NeedRestore && (DmaWasOn || !DmaOn) && (MainWasOn || !MainOn))return EFI_SUCCESS;
  // Never touch the controller while an upstream or endpoint gate is off or
  // uncertain. Keep any partially enabled resource for an explicit retry.
  S=ValidateUpstreamClocks(Clock);if(EFI_ERROR(S))return EFI_DEVICE_ERROR;
  if(!DmaOn || !MainOn)return EFI_DEVICE_ERROR;
  S=PinsHigh(Gpio,Index);if(EFI_ERROR(S))return EFI_DEVICE_ERROR;
  S=ControllerIdle(Index);if(EFI_ERROR(S))return EFI_DEVICE_ERROR;
  S=RestorePins(Gpio,Index,OriginalMode,ChangedMask);
  // Keep clocks on if the pin route could not be safely restored.
  if(EFI_ERROR(S))return EFI_DEVICE_ERROR;
  if(EFI_ERROR(RestoreClock(Clock,MainId,MainWasOn)))return EFI_DEVICE_ERROR;
  if(EFI_ERROR(RestoreClock(Clock,DmaId,DmaWasOn)))return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
static EFI_STATUS LiveReady(UINTN Index,MTK_CLOCK_PROTOCOL *Clock,UINT32 *Hz);
static EFI_STATUS PrepareBus(MTK_CLOCK_PROTOCOL *Clock,MTK_GPIO_PROTOCOL *Gpio,UINTN Index) {
  EFI_STATUS S,Rollback;UINT32 DmaId,MainId,Mode[2],ChangedMask=0,ReadyHz;
  BOOLEAN DmaWasOn,MainWasOn,SnapshotValid;
  if(Index>=I2cBusCount || !Gpio || !Gpio->GetDir || !Gpio->GetState || !Gpio->SetMode)return EFI_INVALID_PARAMETER;
  S=ValidateMapped(Index);if(EFI_ERROR(S))return S;
  EnableUpstreamGates(Clock);
  S=ValidateUpstreamClocks(Clock);if(EFI_ERROR(S))return S;
  for(UINTN I=0;I<2;I++) {
    UINT32 Pin=Pins[Index]+I;BOOLEAN Direction;
    Mode[I]=PinMode(Pin);
    // GPIO mode 5 on I2C6 overlaps USB ID/VBUS and is never repurposed here.
    if(Mode[I]>1)return EFI_UNSUPPORTED;
    if(Mode[I]==0) {
      S=Gpio->GetDir(Pin,&Direction);if(EFI_ERROR(S))return S;
      if(Direction)return EFI_ACCESS_DENIED;
    }
  }
  S=PinsHigh(Gpio,Index);if(EFI_ERROR(S))return S;
  S=EnableBusClocks(Clock,Index,&DmaId,&MainId,&DmaWasOn,&MainWasOn,&SnapshotValid);
  if(EFI_ERROR(S)) {
    if(!SnapshotValid)return S;
    // A gate may have changed despite a failing provider status; undo only
    // the endpoint gates that were off before this preparation attempt.
    Rollback=RollbackPreparation(Clock,Gpio,Index,DmaId,MainId,DmaWasOn,MainWasOn,Mode,0);
    return EFI_ERROR(Rollback)?EFI_DEVICE_ERROR:S;
  }
  S=ControllerIdle(Index);if(EFI_ERROR(S))goto Fail;
  for(UINTN I=0;I<2;I++) {
    UINT32 Pin=Pins[Index]+I;
    if(Mode[I]!=0)continue;
    S=PinsHigh(Gpio,Index);if(EFI_ERROR(S))goto Fail;
    S=Gpio->SetMode(Pin,1);
    if(PinMode(Pin)==1)ChangedMask|=1U<<I;
    if(EFI_ERROR(S))goto Fail;
    if(PinMode(Pin)!=1){S=EFI_DEVICE_ERROR;goto Fail;}
  }
  S=LiveReady(Index,Clock,&ReadyHz);if(EFI_ERROR(S))goto Fail;
  return EFI_SUCCESS;
Fail:
  Rollback=RollbackPreparation(Clock,Gpio,Index,DmaId,MainId,DmaWasOn,MainWasOn,Mode,ChangedMask);
  return EFI_ERROR(Rollback)?EFI_DEVICE_ERROR:S;
}
static EFI_STATUS LiveReady(UINTN Index,MTK_CLOCK_PROTOCOL *Clock,UINT32 *Hz) {
  UINT32 Id;UINT64 Rate;EFI_STATUS S;
  if(Index>=I2cBusCount || !Clock || !Clock->GetId || !Clock->GetFrequency || !Clock->IsEnabled ||
     !Clock->GetParent || !Hz)return EFI_INVALID_PARAMETER;
  S=ValidateMapped(Index);if(EFI_ERROR(S))return S;
  S=ValidateUpstreamClocks(Clock);if(EFI_ERROR(S))return S;
  S=ClockReady(Clock,DmaClockName,FALSE);if(EFI_ERROR(S))return S;
  S=ClockReady(Clock,MainClockNames[Index],FALSE);if(EFI_ERROR(S))return S;
  S=Clock->GetId("TOP_I2C_PSEUDO",&Id);if(EFI_ERROR(S))return S;
  S=Clock->GetFrequency(Id,&Rate);
  if(EFI_ERROR(S) || Rate<1000000 || Rate>1000000000)return EFI_UNSUPPORTED;
  // Both AP pins must already be I2C mode and the sampled bus must be idle.
  UINT32 Pin=Pins[Index];
  UINT32 Mode=MmioRead32(0x10005000+0x300+(Pin/8)*0x10);
  if(((Mode>>((Pin%8)*4))&0xff)!=0x11)return EFI_NOT_READY;
  S=ControllerIdle(Index);if(EFI_ERROR(S))return S;
  for(UINTN I=0;I<2;I++) {
    S=PinsHighMmio(Index);if(EFI_ERROR(S))return S;
    if(I==0)MicroSecondDelay(10);
  }
  *Hz=(UINT32)Rate;return EFI_SUCCESS;
}
static EFI_STATUS Publish(const VOID *Fdt,UINTN Bytes,MTK_CLOCK_PROTOCOL *Clock,MTK_GPIO_PROTOCOL *Gpio,UINTN Index) {
  EFI_STATUS S;UINT32 Hz;EFI_HANDLE *Found=NULL;UINTN Count=0;
  if(Index>=I2cBusCount)return EFI_INVALID_PARAMETER;
  if(Handles[Index])return EFI_ALREADY_STARTED;
  S=gBS->LocateHandleBuffer(ByProtocol,&gMediaTekI2cControllerProtocolGuid,NULL,&Count,&Found);
  if(S!=EFI_NOT_FOUND && EFI_ERROR(S))return S;
  if(!EFI_ERROR(S)) {
    for(UINTN I=0;I<Count;I++) {
      MTK_I2C_CONTROLLER *Other;
      S=gBS->HandleProtocol(Found[I],&gMediaTekI2cControllerProtocolGuid,(VOID **)&Other);
      if(EFI_ERROR(S)) { FreePool(Found);return S; }
      if(Other && (Other->ControllerBase==Bases[Index] || Other->DmaBase==Dmas[Index])) {
        FreePool(Found);return EFI_ALREADY_STARTED;
      }
    }
    FreePool(Found);
  }
  S=ValidateTree(Fdt,Bytes,Index);if(EFI_ERROR(S))return S;
  S=PrepareBus(Clock,Gpio,Index);if(EFI_ERROR(S))return S;
  S=LiveReady(Index,Clock,&Hz);if(EFI_ERROR(S))return S;
  Boards[Index]=(MTK_I2C_CONTROLLER){MTK_I2C_CONTROLLER_REVISION,MTK_I2C_REQUIRED_FLAGS,
    Bases[Index],0x1000,Dmas[Index],0x80,Hz,0,0,0,EFI_NOT_READY};
  BOARD_PATH *D=&DevicePaths[Index];ZeroMem(D,sizeof(*D));
  D->Vendor.Header.Type=HARDWARE_DEVICE_PATH;D->Vendor.Header.SubType=HW_VENDOR_DP;
  SetDevicePathNodeLength(&D->Vendor.Header,sizeof(D->Vendor)+sizeof(D->Bus));
  D->Vendor.Guid=(EFI_GUID){0xb160f749,0x7d34,0x4763,{0x91,0x2b,0x4d,0x99,0xda,0x65,0x38,0x8d}};
  D->Bus=BusNumbers[Index];D->End.Type=END_DEVICE_PATH_TYPE;D->End.SubType=END_ENTIRE_DEVICE_PATH_SUBTYPE;
  SetDevicePathNodeLength(&D->End,sizeof(D->End));
  S=gBS->InstallMultipleProtocolInterfaces(&Handles[Index],&gMediaTekI2cControllerProtocolGuid,
    &Boards[Index],&gEfiDevicePathProtocolGuid,D,NULL);
  if(EFI_ERROR(S))Handles[Index]=NULL;
  return S;
}
// I2C1 extends the existing ACPI ready mask. Its rate is stored after the MUSB record.
// Re-check the final hardware state; advertise no bus with an active transfer.
static VOID EFIAPI ExportToAcpi(EFI_EVENT Event,VOID *Context) {
  UINT32 Ready=0,Hz[I2cBusCount]={0};
  (VOID)Event;(VOID)Context;Exiting=TRUE;
  if(BindingEvent)(VOID)gBS->SetTimer(BindingEvent,TimerCancel,0);
  MmioWrite32(0x43f7f080,0);MmioWrite32(0x43f7f08c,0);
  if(CachedClock)for(UINTN I=0;I<I2cBusCount;I++) {
    if(Handles[I] && Boards[I].DriverStartPhase==MTK_I2C_DRIVER_HANDOFF &&
       Boards[I].DriverStartStatus==EFI_SUCCESS &&
       !EFI_ERROR(LiveReady(I,CachedClock,&Hz[I])))Ready|=1U<<I;
  }
  MmioWrite32(0x43f7f084,Hz[I2c0Index]);MmioWrite32(0x43f7f088,Hz[I2c6Index]);
  MmioWrite32(0x43f7f140,Hz[I2c1Index]);
  WriteBackDataCacheRange((VOID *)(UINTN)0x43f7f140,sizeof(UINT32));
  MmioWrite32(0x43f7f08c,1);MemoryFence();
  MmioWrite32(0x43f7f080,Ready);MemoryFence();
  WriteBackDataCacheRange((VOID *)(UINTN)0x43f7f080,16);
}
static VOID EFIAPI Observe(EFI_EVENT Event,VOID *Context) {
  VOID *Fdt;MTK_CLOCK_PROTOCOL *Clock;MTK_GPIO_PROTOCOL *Gpio;EFI_STATUS S;UINT64 Bytes;
  (VOID)Event;
  if(Exiting)return;
  if(EFI_ERROR(EfiGetSystemConfigurationTable(&gFdtTableGuid,&Fdt)) || !Fdt)return;
  // Accept the exact runtime copy validated and published by BootStatusDxe.
  if(MmioRead64(0x43f7f000)!=0x0000000148303754ULL ||
     MmioRead64(0x43f7f048)!=(UINT64)(UINTN)Fdt || MmioRead64(0x43f7f058)!=EFI_SUCCESS)return;
  Bytes=MmioRead64(0x43f7f050);
  if(Bytes<sizeof(FDT_HEADER) || Bytes>0x200000 || FdtCheckFull(Fdt,(UINTN)Bytes) ||
     FdtTotalSize(Fdt)!=Bytes || EFI_ERROR(gBS->LocateProtocol(&gMediaTekClockProtocolGuid,NULL,(VOID **)&Clock)) ||
     EFI_ERROR(gBS->LocateProtocol(&gMediaTekGpioProtocolGuid,NULL,(VOID **)&Gpio)))return;
  CachedClock=Clock;
  for(UINTN I=0;I<I2cBusCount;I++) {
    S=Publish(Fdt,(UINTN)Bytes,Clock,Gpio,I);
    if(EFI_ERROR(S) && S!=EFI_ALREADY_STARTED)DEBUG((DEBUG_INFO,"T70 I2C inherited bus %u not ready: %r\n",BusNumbers[I],S));
  }
  if(Context==NULL)BindControllers(NULL,NULL);
}
EFI_STATUS EFIAPI T70I2cPlatformEntry(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS S;
  (VOID)ImageHandle;(VOID)SystemTable;
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,ExportToAcpi,NULL,&gEfiEventExitBootServicesGuid,&ExitEvent);
  if(EFI_ERROR(S))return S;
  // Once an event references this image, keep the image resident on an
  // optional retry-event failure rather than returning an unloadable error.
  // DxeCore signals the table GUID when BootStatusDxe publishes the runtime
  // FDT. Observe it before BDS connects the controller handles, regardless of
  // the two images' dispatch order.
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Observe,NULL,&gFdtTableGuid,&FdtEvent);
  if(EFI_ERROR(S))DEBUG((DEBUG_WARN,"T70 I2C FDT change notification unavailable: %r\n",S));
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Observe,NULL,&gEfiEventReadyToBootGuid,&RetryEvent);
  if(EFI_ERROR(S))DEBUG((DEBUG_WARN,"T70 I2C ReadyToBoot retry unavailable: %r\n",S));
  Observe(NULL,NULL);
  S=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,BindControllers,NULL,&BindingEvent);
  if(EFI_ERROR(S))BindingEvent=NULL;
  else {
    S=gBS->SetTimer(BindingEvent,TimerPeriodic,100000);
    if(EFI_ERROR(S)) { (VOID)gBS->CloseEvent(BindingEvent);BindingEvent=NULL; }
  }
  if(EFI_ERROR(S))DEBUG((DEBUG_WARN,"T70 I2C binding retry unavailable: %r\n",S));
  return EFI_SUCCESS;
}
