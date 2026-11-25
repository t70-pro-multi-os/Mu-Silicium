// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <PiDxe.h>
#include "MediaTekTest.h"
#include <Library/FdtLib.h>
#include "../Drivers/I2cPlatformDxe/I2cPlatformDxe.c"
EFI_GUID gMediaTekI2cControllerProtocolGuid,gEfiDevicePathProtocolGuid,gMediaTekGpioProtocolGuid;
EFI_GUID gFdtTableGuid,gMediaTekClockProtocolGuid,gEfiI2cMasterProtocolGuid;
EFI_GUID gEfiEventExitBootServicesGuid,gEfiEventReadyToBootGuid;
static VOID *EntryTree;
static UINT64 EntryBytes;
static BOOLEAN BadHandoff;
UINT64 EFIAPI MmioRead64(UINTN Address) {
  switch(Address) {
    case 0x43f7f000:return BadHandoff?0:0x0000000148303754ULL;
    case 0x43f7f048:return (UINT64)(UINTN)EntryTree;
    case 0x43f7f050:return EntryBytes;
    case 0x43f7f058:return EFI_SUCCESS;
    default:CHECK(0);return 0;
  }
}
static unsigned EventCalls,EventFailure,TreeLookups;
static EFI_EVENT_NOTIFY EventFunctions[3];
static EFI_STATUS EFIAPI EventCreate(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Group,EFI_EVENT *Event) {
  unsigned N=EventCalls++;
  CHECK(N<3 && Type==EVT_NOTIFY_SIGNAL && Context==NULL);
  CHECK(Tpl==TPL_CALLBACK);
  CHECK(Group==(N==0?&gEfiEventExitBootServicesGuid:N==1?&gFdtTableGuid:&gEfiEventReadyToBootGuid));
  if(EventFailure==N+1)return EFI_OUT_OF_RESOURCES;
  EventFunctions[N]=Notify;*Event=(VOID *)(UINTN)(0x100+N);return EFI_SUCCESS;
}
EFI_STATUS EFIAPI EfiGetSystemConfigurationTable(EFI_GUID *Guid,VOID **Tree) {
  CHECK(Guid==&gFdtTableGuid);TreeLookups++;*Tree=EntryTree;return EFI_SUCCESS;
}
static BOOLEAN Existing,PublishFailure,Masters[I2cBusCount],NoMaster,Reenter;
static unsigned ConnectCalls[I2cBusCount],ConnectFailures,TimerFailure,TimerCancels,EventCloses;
static EFI_STATUS EFIAPI TimerCreate(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,VOID *Context,EFI_EVENT *Event) {
  CHECK(Type==(EVT_TIMER|EVT_NOTIFY_SIGNAL) && Tpl==TPL_CALLBACK && Notify==BindControllers && !Context);
  if(TimerFailure==1) { *Event=(VOID *)0xdead;return EFI_OUT_OF_RESOURCES; }
  *Event=(VOID *)0x200;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TimerSet(EFI_EVENT Event,EFI_TIMER_DELAY Type,UINT64 Delay) {
  CHECK(Event==(VOID *)0x200);
  if(Type==TimerCancel) { CHECK(Delay==0);TimerCancels++;return EFI_SUCCESS; }
  CHECK(Type==TimerPeriodic && Delay==100000);
  return TimerFailure==2?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventClose(EFI_EVENT Event) { CHECK(Event==(VOID *)0x200);EventCloses++;return EFI_SUCCESS; }
static UINTN HandleIndex(EFI_HANDLE Handle) {
  for(UINTN I=0;I<I2cBusCount;I++)if(Handle && Handle==Handles[I])return I;
  CHECK(0);return 0;
}
static EFI_STATUS EFIAPI Connect(EFI_HANDLE Handle,EFI_HANDLE *Drivers,EFI_DEVICE_PATH_PROTOCOL *Path,BOOLEAN Recursive) {
  UINTN I=HandleIndex(Handle);
  CHECK(Handle && Handle==Handles[I] && !Drivers && !Path && !Recursive && BindingBusy && !Exiting);
  ConnectCalls[I]++;
  if(Reenter)BindControllers(NULL,NULL);
  if(ConnectFailures) { ConnectFailures--;return EFI_DEVICE_ERROR; }
  if(!NoMaster)Masters[I]=TRUE;
  return EFI_SUCCESS;
}
static MTK_I2C_CONTROLLER Other;
UINT16 EFIAPI SetDevicePathNodeLength(VOID *Node,UINTN Length) {
  EFI_DEVICE_PATH_PROTOCOL *D=Node;D->Length[0]=Length;D->Length[1]=Length>>8;return Length;
}
static EFI_STATUS EFIAPI Find(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Guid,VOID *Key,UINTN *Count,EFI_HANDLE **List) {
  CHECK(Type==ByProtocol && Guid==&gMediaTekI2cControllerProtocolGuid);
  if(!Existing)return EFI_NOT_FOUND;
  *List=malloc(sizeof(EFI_HANDLE));CHECK(*List);(*List)[0]=(VOID *)0x123;*Count=1;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Interface(EFI_HANDLE Handle,EFI_GUID *Guid,VOID **Value) {
  if(Guid==&gEfiI2cMasterProtocolGuid) {
    UINTN I=HandleIndex(Handle);
    CHECK(Handle && Handle==Handles[I]);*Value=Masters[I]?&Other:NULL;
    return Masters[I]?EFI_SUCCESS:EFI_UNSUPPORTED;
  }
  *Value=&Other;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI PublishInstall(EFI_HANDLE *Handle,...) {
  Installs++;*Handle=(VOID *)(UINTN)(0x456+Installs);
  return PublishFailure?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_DXE_SERVICES Dxe;
EFI_DXE_SERVICES *gDS=&Dxe;
static BOOLEAN BadMap,ClockOff;
static UINT64 Frequency=124800000;
static UINT32 Values[16],Handoff[4],I2c1HandoffHz;
static const UINTN ModeReg[]={2,3,2},StartReg[]={5,6,9},DmaReg[]={7,8,10};
static const UINT32 ModeMask[]={0x1100,0x11000000,0x110000},ClockId[]={6,7,10};
static BOOLEAN ClockStates[11],PinDirections[I2cBusCount][2];
static UINT32 ClockCallOrder[16];
static UINT32 ClockFailMask,ClockFailAfterMask,ClockNoEffectMask,ModeNoEffectMask;
static UINTN ClockSetCalls,ModeSetCalls,ModeFailAt;
static UINTN ControllerMmioReads;
static BOOLEAN ClockParentOn;
static BOOLEAN ModeFailAfterEffect;
static unsigned CacheCleans;
VOID *EFIAPI WriteBackDataCacheRange(VOID *Address,UINTN Length) {
  if((UINTN)Address==0x43f7f140){CHECK(Length==4);return Address;}
  CHECK((UINTN)Address==0x43f7f080 && Length==16 && Handoff[3]==1);
  CacheCleans++;return Address;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value) {
  if(Address==0x43f7f140)return I2c1HandoffHz=Value;
  CHECK(Address>=0x43f7f080 && Address<=0x43f7f08c && !(Address&3));
  Handoff[(Address-0x43f7f080)/4]=Value;return Value;
}
VOID EFIAPI MemoryFence(VOID) { __sync_synchronize(); }
static const UINTN Addresses[]={0x11e02e00,0x1101be00,0x10005400,0x10005410,0x10005240,
  0x11e00024,0x1101a024,0x10217088,0x10217488,0x11e01024,0x10217108};
static EFI_STATUS EFIAPI Descriptor(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D) {
  ZeroMem(D,sizeof(*D));D->BaseAddress=A&~0xfffULL;D->Length=0x1000;
  D->GcdMemoryType=BadMap?EfiGcdMemoryTypeSystemMemory:EfiGcdMemoryTypeMemoryMappedIo;
  D->Attributes=EFI_MEMORY_UC;return EFI_SUCCESS;
}
UINT32 EFIAPI MmioRead32(UINTN A) {
  for(unsigned I=0;I<ARRAY_SIZE(Addresses);I++)if(Addresses[I]==A){
    if(I>=5)ControllerMmioReads++;
    return Values[I];
  }
  fprintf(stderr,"unexpected MMIO %lx\n",(unsigned long)A);abort();
}
UINT16 EFIAPI MmioRead16(UINTN A) {CHECK(0);return 0;}
static EFI_STATUS EFIAPI Id(CONST CHAR8 *Name,UINT32 *Result) {
  const CHAR8 *Names[]={"TOP_I2C_SEL","TOP_AXI_SEL","IFRAO_I2C_PSEUDO","IFRAO_APDMA_PSEUDO","IFRAO_AP_DMA","TOP_I2C_PSEUDO","IMPW_AP_CLOCK_I2C0","IMPC_AP_CLOCK_I2C6","TOP_APDMA","mock-parent","IMPW_AP_CLOCK_I2C1"};
  for(unsigned I=0;I<ARRAY_SIZE(Names);I++)if(!strcmp(Name,Names[I])){*Result=I;return EFI_SUCCESS;}
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI On(UINT32 Id,BOOLEAN *Result) {
  if(!Result)return EFI_INVALID_PARAMETER;
  if(Id==9U)*Result=ClockParentOn;
  else if(Id==8U)*Result=!ClockOff && ClockStates[3];
  else if(Id==5U)*Result=!ClockOff && ClockStates[2];
  else *Result=(Id<ARRAY_SIZE(ClockStates) && !ClockOff && ClockStates[Id]);
  return Id<ARRAY_SIZE(ClockStates)?EFI_SUCCESS:EFI_INVALID_PARAMETER;
}
static EFI_STATUS EFIAPI SetClock(UINT32 Id,BOOLEAN Enable) {
  UINTN Call=ClockSetCalls++;
  CHECK(Call<ARRAY_SIZE(ClockCallOrder) && Id<ARRAY_SIZE(ClockStates));
  ClockCallOrder[Call]=(Id<<1)|(Enable?1U:0U);
  BOOLEAN Fail=Call<32U && (ClockFailMask&(1U<<Call));
  if(Fail && !(ClockFailAfterMask&(1U<<Call)))return EFI_DEVICE_ERROR;
  if(Call>=32U || !(ClockNoEffectMask&(1U<<Call)))ClockStates[Id]=Enable;
  return Fail?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Parent(UINT32 Id,UINT32 *Result) {CHECK(Id<2);*Result=9;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Rate(UINT32 Id,UINT64 *Result) {CHECK(Id==5);*Result=Frequency;return EFI_SUCCESS;}
static UINTN PinBus(UINT32 Pin,UINTN *Slot) {
  if(Pin==130U || Pin==131U){*Slot=Pin-130U;return 0U;}
  if(Pin==142U || Pin==143U){*Slot=Pin-142U;return 1U;}
  if(Pin==132U || Pin==133U){*Slot=Pin-132U;return I2c1Index;}
  CHECK(0);return 0U;
}
static EFI_STATUS EFIAPI GpioGetDir(UINT32 Pin,BOOLEAN *Direction) {
  UINTN Slot,Bus=PinBus(Pin,&Slot);if(!Direction)return EFI_INVALID_PARAMETER;
  *Direction=PinDirections[Bus][Slot];return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GpioGetState(UINT32 Pin,BOOLEAN *State) {
  UINTN Slot,Bus=PinBus(Pin,&Slot);if(!State)return EFI_INVALID_PARAMETER;
  (VOID)Slot;*State=(Values[4]&(1U<<(Pin%32)))!=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GpioSetMode(UINT32 Pin,UINT32 Mode) {
  UINTN Slot,Bus=PinBus(Pin,&Slot),Call=ModeSetCalls++;UINT32 Shift=(Pin%8U)*4U,Mask=0xfU<<Shift;
  (VOID)Slot;CHECK(Mode<=7U && Call<8U);
  if(Call==ModeFailAt && !ModeFailAfterEffect)return EFI_DEVICE_ERROR;
  if(!(ModeNoEffectMask&(1U<<Call)))Values[ModeReg[Bus]]=(Values[ModeReg[Bus]]&~Mask)|(Mode<<Shift);
  return Call==ModeFailAt?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static MTK_CLOCK_PROTOCOL Clock={.GetId=Id,.SetEnable=SetClock,.IsEnabled=On,.GetFrequency=Rate,.GetParent=Parent};
static MTK_GPIO_PROTOCOL Gpio={.GetDir=GpioGetDir,.GetState=GpioGetState,.SetMode=GpioSetMode};
static EFI_STATUS EFIAPI ProtocolLocate(EFI_GUID *Guid,VOID *Registration,VOID **Protocol) {
  CHECK(Registration==NULL);
  if(Guid==&gMediaTekClockProtocolGuid)*Protocol=&Clock;
  else if(Guid==&gMediaTekGpioProtocolGuid)*Protocol=&Gpio;
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}
static EFI_TPL PlatformTpl=TPL_APPLICATION;
static EFI_TPL EFIAPI Raise(EFI_TPL Tpl) {
  EFI_TPL Old=PlatformTpl; CHECK(Tpl>=Old); PlatformTpl=Tpl; return Old;
}
static VOID EFIAPI Restore(EFI_TPL Tpl) { PlatformTpl=Tpl; }
static VOID FinishControllerStops(VOID) {
  for (UINTN I=0;I<I2cBusCount;I++) {
    Boards[I].DriverStartPhase=MTK_I2C_DRIVER_HANDOFF;
    Boards[I].DriverStartStatus=EFI_SUCCESS;
  }
}
static VOID ResetLive(void);
static VOID ResetEntry(VOID *Tree,unsigned Fail) {
  ResetLive();
  EntryTree=Tree;EventCalls=TreeLookups=0;EventFailure=Fail;BadHandoff=FALSE;
  ZeroMem(EventFunctions,sizeof(EventFunctions));ZeroMem(Handles,sizeof(Handles));
  CachedClock=NULL;Exiting=FALSE;Existing=PublishFailure=FALSE;Installs=0;
  BindingEvent=NULL;BindingBusy=FALSE;NoMaster=Reenter=FALSE;
  ZeroMem(Masters,sizeof(Masters));ZeroMem(ConnectCalls,sizeof(ConnectCalls));
  ZeroMem(BindingAttempts,sizeof(BindingAttempts));ZeroMem(BindingDelay,sizeof(BindingDelay));
  ZeroMem(BindingCountdown,sizeof(BindingCountdown));
  ConnectFailures=TimerFailure=TimerCancels=EventCloses=0;
}
static VOID ResetLive(void) {
  ZeroMem(Values,sizeof(Values));Values[2]=0x111100;Values[3]=0x11000000;Values[4]=0xc03c;
  ZeroMem(ClockStates,sizeof(ClockStates));for(UINTN I=0;I<ARRAY_SIZE(ClockStates);I++)ClockStates[I]=TRUE;
  ZeroMem(PinDirections,sizeof(PinDirections));ZeroMem(ClockCallOrder,sizeof(ClockCallOrder));
  ControllerMmioReads=0;
  ClockParentOn=TRUE;
  ClockSetCalls=ModeSetCalls=0;ClockFailMask=ClockFailAfterMask=ClockNoEffectMask=ModeNoEffectMask=0;
  ModeFailAt=MAX_UINTN;ModeFailAfterEffect=FALSE;
  ClockOff=BadMap=FALSE;Frequency=124800000;
}
static VOID Corrupt(UINT8 *Tree,CONST UINT8 *Original,UINTN Bytes,CONST CHAR8 *Node,CONST CHAR8 *Property,UINTN Byte,UINT8 NewValue,UINTN Index) {
  INT32 Length;CopyMem(Tree,Original,Bytes);
  UINT8 *P=(UINT8 *)FdtGetProp(Tree,FdtPathOffset(Tree,Node),Property,&Length);
  CHECK(P && Byte<(UINTN)Length);P[Byte]=NewValue;
  CHECK(EFI_ERROR(ValidateTree(Tree,Bytes,Index)));
}
int main(int argc,char **argv) {
  CHECK(argc==2);FILE *F=fopen(argv[1],"rb");CHECK(F);
  CHECK(fseek(F,0,SEEK_END)==0);long Bytes=ftell(F);CHECK(Bytes>0);
  rewind(F);UINT8 *Original=malloc(Bytes),*Tree=malloc(Bytes);CHECK(Original && Tree);
  CHECK(fread(Original,1,Bytes,F)==(size_t)Bytes);fclose(F);
  CHECK(ValidateTree(Original,Bytes,0)==EFI_SUCCESS);
  CHECK(ValidateTree(Original,Bytes,1)==EFI_SUCCESS);
  CHECK(EFI_ERROR(ValidateTree(Original,Bytes-1,0)));
  CHECK(ValidateTree(Original,Bytes,I2c1Index)==EFI_SUCCESS);
  CHECK(EFI_ERROR(ValidateTree(Original,Bytes,I2cBusCount)));
  Corrupt(Tree,Original,Bytes,Paths[I2c1Index],"clocks",7,0,I2c1Index);
  Corrupt(Tree,Original,Bytes,"/soc","dma-ranges",23,1,0);
  Corrupt(Tree,Original,Bytes,"/soc","dma-ranges",7,1,1);
  Corrupt(Tree,Original,Bytes,"/soc/i2c@11e00000","reg",7,1,0);
  Corrupt(Tree,Original,Bytes,"/soc/i2c@1101a000","reg",23,1,1);
  Corrupt(Tree,Original,Bytes,"/soc/i2c@11e00000","clock-div",3,2,0);
  Corrupt(Tree,Original,Bytes,"/soc/i2c@1101a000","clocks",7,1,1);
  Corrupt(Tree,Original,Bytes,"/soc/i2c@11e00000","clock-names",0,'x',0);
  Dxe.GetMemorySpaceDescriptor=Descriptor;
  UINT32 Hz=0;
  for(UINTN I=0;I<I2cBusCount;I++) {
    ResetLive();CHECK(LiveReady(I,&Clock,&Hz)==EFI_SUCCESS && Hz==124800000);
    ClockOff=TRUE;CHECK(LiveReady(I,&Clock,&Hz)==EFI_NOT_READY);
    ResetLive();BadMap=TRUE;CHECK(LiveReady(I,&Clock,&Hz)==EFI_UNSUPPORTED);
    ResetLive();Frequency=0;CHECK(LiveReady(I,&Clock,&Hz)==EFI_UNSUPPORTED);
    ResetLive();ClockStates[ClockId[I]]=FALSE;CHECK(LiveReady(I,&Clock,&Hz)==EFI_NOT_READY);
    ResetLive();Values[ModeReg[I]]=0;CHECK(LiveReady(I,&Clock,&Hz)==EFI_NOT_READY);
    ResetLive();Values[4]=0;CHECK(LiveReady(I,&Clock,&Hz)==EFI_NOT_READY);
    ResetLive();Values[StartReg[I]]=1;CHECK(LiveReady(I,&Clock,&Hz)==EFI_NOT_READY);
    ResetLive();Values[DmaReg[I]]=1;CHECK(LiveReady(I,&Clock,&Hz)==EFI_NOT_READY);
  }
  CHECK(LiveReady(I2cBusCount,&Clock,&Hz)==EFI_INVALID_PARAMETER);
  // Reject missing upstream selectors, parents, and pseudo gates before any
  // controller/APDMA register is read or endpoint gate is changed.
  for(UINTN I=0;I<5;I++) {
    ResetLive();Values[2]=0;
    if(I==4)ClockParentOn=FALSE;else ClockStates[I]=FALSE;
    CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_NOT_READY && ClockSetCalls==0 && ControllerMmioReads==0);
  }
  for(UINTN I=0;I<I2cBusCount;I++) {
    ResetLive();Values[ModeReg[I]]=0;ClockStates[4]=FALSE;ClockStates[ClockId[I]]=FALSE;
    CHECK(PrepareBus(&Clock,&Gpio,I)==EFI_SUCCESS);
    CHECK(ClockSetCalls==2 && ClockCallOrder[0]==((4U<<1)|1U) && ClockCallOrder[1]==((ClockId[I]<<1)|1U));
    CHECK(ClockStates[4] && ClockStates[ClockId[I]] && Values[ModeReg[I]]==ModeMask[I]);
    CHECK(LiveReady(I,&Clock,&Hz)==EFI_SUCCESS);
  }
  ResetLive();Values[3]=0x50000000U;ClockStates[4]=FALSE;ClockStates[7]=FALSE;
  CHECK(PrepareBus(&Clock,&Gpio,1)==EFI_UNSUPPORTED && ClockSetCalls==0 && ModeSetCalls==0);
  ResetLive();Values[2]=0;PinDirections[0][0]=TRUE;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_ACCESS_DENIED && ClockSetCalls==0 && ModeSetCalls==0);
  ResetLive();Values[2]=0;Values[4]&=~BIT2;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_NOT_READY && ClockSetCalls==0 && ModeSetCalls==0);
  ResetLive();Values[2]=0;ClockStates[4]=FALSE;ClockStates[6]=FALSE;ClockFailMask=BIT0;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_DEVICE_ERROR && ClockSetCalls==1 && !ClockStates[4] && !ClockStates[6] && Values[2]==0);
  ResetLive();Values[2]=0;ClockStates[4]=FALSE;ClockStates[6]=FALSE;ClockFailMask=BIT0;ClockFailAfterMask=BIT0;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_DEVICE_ERROR && ClockSetCalls==1 && ClockStates[4] && !ClockStates[6] && Values[2]==0 && ControllerMmioReads==0);
  ResetLive();Values[2]=0;ClockStates[4]=FALSE;ClockStates[6]=FALSE;ClockNoEffectMask=BIT0;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_NOT_READY && ClockSetCalls==1 && !ClockStates[4] && !ClockStates[6] && Values[2]==0);
  ResetLive();Values[2]=0;ClockStates[4]=FALSE;ClockStates[6]=FALSE;ClockFailMask=BIT1;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_DEVICE_ERROR && ClockSetCalls==2 && ClockStates[4] && !ClockStates[6] && Values[2]==0 && ControllerMmioReads==0);
  ResetLive();Values[2]=0;ClockStates[4]=FALSE;ClockStates[6]=FALSE;ClockFailMask=BIT1;ClockFailAfterMask=BIT1;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_DEVICE_ERROR && ClockSetCalls==4 && !ClockStates[4] && !ClockStates[6] && Values[2]==0);
  ResetLive();Values[2]=0;ClockStates[4]=TRUE;ClockStates[6]=FALSE;ClockFailMask=BIT0;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_DEVICE_ERROR && ClockSetCalls==1 && ClockStates[4] && !ClockStates[6] && ControllerMmioReads==0);
  ResetLive();Values[2]=0;ClockStates[4]=FALSE;ClockStates[6]=FALSE;ClockFailMask=BIT1|BIT2;ClockFailAfterMask=BIT1|BIT2;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_DEVICE_ERROR && ClockSetCalls==3 && ClockStates[4] && !ClockStates[6] && Values[2]==0);
  ResetLive();Values[2]=0;ClockStates[4]=FALSE;ClockStates[6]=FALSE;ModeFailAt=1;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_DEVICE_ERROR && ModeSetCalls==3 && Values[2]==0 && !ClockStates[4] && !ClockStates[6]);
  ResetLive();Values[2]=0;ClockStates[4]=FALSE;ClockStates[6]=FALSE;ModeFailAt=1;ModeFailAfterEffect=TRUE;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_DEVICE_ERROR && ModeSetCalls==4 && Values[2]==0 && !ClockStates[4] && !ClockStates[6]);
  ResetLive();Values[2]=0;ClockStates[4]=FALSE;ClockStates[6]=FALSE;ModeNoEffectMask=BIT1;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_DEVICE_ERROR && ModeSetCalls==3 && Values[2]==0 && !ClockStates[4] && !ClockStates[6]);
  ResetLive();Values[2]=0;ClockStates[4]=FALSE;ClockStates[6]=FALSE;Values[7]=1;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_DEVICE_ERROR && Values[2]==0 && ClockStates[4] && ClockStates[6]);
  // Inherited, already-enabled endpoint gates must not be written again.
  ResetLive();Values[2]=0;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_SUCCESS && ClockSetCalls==0 && ClockStates[4] && ClockStates[6]);
  ResetLive();Values[2]=0;ClockStates[4]=FALSE;
  CHECK(PrepareBus(&Clock,&Gpio,0)==EFI_SUCCESS && ClockSetCalls==1 && ClockCallOrder[0]==((4U<<1)|1U) && ClockStates[4] && ClockStates[6]);
  Services.LocateHandleBuffer=Find;Services.HandleProtocol=Interface;
  Services.RaiseTPL=Raise;Services.RestoreTPL=Restore;
  Services.InstallMultipleProtocolInterfaces=PublishInstall;
  ResetLive();PublishFailure=TRUE;
  CHECK(Publish(Original,Bytes,&Clock,&Gpio,0)==EFI_DEVICE_ERROR && Handles[0]==NULL);
  PublishFailure=FALSE;
  CHECK(Publish(Original,Bytes,&Clock,&Gpio,0)==EFI_SUCCESS && Handles[0]);
  CHECK(Boards[0].ControllerBase==0x11e00000 && Boards[0].DmaBase==0x10217080 &&
    Boards[0].Flags==MTK_I2C_REQUIRED_FLAGS && Boards[0].SourceClockHz==124800000 && Boards[0].IommuBase==0);
  CHECK(Publish(Original,Bytes,&Clock,&Gpio,0)==EFI_ALREADY_STARTED && Installs==2);
  Handles[0]=NULL;Existing=TRUE;Other.ControllerBase=Bases[0];
  CHECK(Publish(Original,Bytes,&Clock,&Gpio,0)==EFI_ALREADY_STARTED && Installs==2 && !Handles[0]);
  Existing=FALSE;
  CHECK(Publish(Original,Bytes,&Clock,&Gpio,1)==EFI_SUCCESS && Handles[1]);
  CHECK(DevicePaths[0].Bus==0 && DevicePaths[1].Bus==6);
  CHECK(Publish(Original,Bytes,&Clock,&Gpio,I2c1Index)==EFI_SUCCESS && Handles[I2c1Index]);
  CHECK(DevicePaths[I2c1Index].Bus==1 && Boards[I2c1Index].ControllerBase==0x11e01000 &&
    Boards[I2c1Index].DmaBase==0x10217100 && Boards[I2c1Index].SourceClockHz==124800000);
  CHECK(Publish(Original,Bytes,&Clock,&Gpio,I2cBusCount)==EFI_INVALID_PARAMETER);
  ResetLive();CachedClock=&Clock;Handles[0]=(VOID *)0x456;
  ExportToAcpi(NULL,NULL);
  CHECK(Handoff[0]==0);
  FinishControllerStops();
  Boards[0].DriverStartStatus=EFI_DEVICE_ERROR;
  ExportToAcpi(NULL,NULL);
  CHECK(Handoff[0]==6);
  Boards[0].DriverStartStatus=EFI_SUCCESS;
  CacheCleans=0;
  ExportToAcpi(NULL,NULL);
  CHECK(CacheCleans==1 && Exiting && Handoff[0]==7 && Handoff[1]==124800000 && Handoff[2]==124800000 && Handoff[3]==1 && I2c1HandoffHz==124800000);
  Values[7]=1; // DMA still active: Windows must not enumerate this bus.
  ExportToAcpi(NULL,NULL);CHECK(Handoff[0]==6 && Handoff[1]==0 && Handoff[2]==124800000);
  Values[DmaReg[I2c1Index]]=1;
  ExportToAcpi(NULL,NULL);CHECK(Handoff[0]==2 && I2c1HandoffHz==0);
  ClockOff=TRUE;ExportToAcpi(NULL,NULL);CHECK(Handoff[0]==0 && Handoff[1]==0 && Handoff[2]==0 && I2c1HandoffHz==0);
  EntryBytes=(UINT64)Bytes;
  Services.CreateEventEx=EventCreate;Services.LocateProtocol=ProtocolLocate;
  Services.ConnectController=Connect;Services.CreateEvent=TimerCreate;
  Services.SetTimer=TimerSet;Services.CloseEvent=EventClose;
  ResetEntry(Original,1);
  CHECK(T70I2cPlatformEntry(NULL,NULL)==EFI_OUT_OF_RESOURCES);
  CHECK(EventCalls==1 && !TreeLookups && !Installs && !Handles[0] && !CachedClock);
  ResetEntry(Original,3);
  CHECK(T70I2cPlatformEntry(NULL,NULL)==EFI_SUCCESS);
  CHECK(EventCalls==3 && TreeLookups==1 && Installs==I2cBusCount && Handles[0] && Handles[1] && Handles[2]);
  CHECK(EventFunctions[0]==ExportToAcpi && EventFunctions[1]==Observe && !EventFunctions[2]);
  FinishControllerStops();
  EventFunctions[0](ExitEvent,NULL);CHECK(Exiting && Handoff[0]==7);
  Observe(NULL,NULL);CHECK(TreeLookups==1 && Installs==I2cBusCount);
  ResetEntry(Original,0);
  CHECK(T70I2cPlatformEntry(NULL,NULL)==EFI_SUCCESS && EventCalls==3 && Installs==I2cBusCount);
  CHECK(EventFunctions[1]==Observe && EventFunctions[2]==Observe);
  EventFunctions[2](RetryEvent,NULL);CHECK(TreeLookups==2 && Installs==I2cBusCount);
  FinishControllerStops();
  EventFunctions[0](ExitEvent,NULL);EventFunctions[2](RetryEvent,NULL);
  CHECK(Exiting && TreeLookups==2 && Installs==I2cBusCount && Handoff[0]==7);
  // FDT may be installed after this image: its configuration-table change
  // event must publish handles before BDS starts binding controllers.
  ResetEntry(NULL,0);
  CHECK(T70I2cPlatformEntry(NULL,NULL)==EFI_SUCCESS && !Installs);
  EntryTree=Original;EventFunctions[1](FdtEvent,NULL);
  CHECK(Installs==I2cBusCount && Handles[0] && Handles[1] && Handles[2]);
  ResetEntry(Original,2);
  CHECK(T70I2cPlatformEntry(NULL,NULL)==EFI_SUCCESS && EventCalls==3 && Installs==I2cBusCount);
  CHECK(!EventFunctions[1] && EventFunctions[0] && EventFunctions[2]);
  FinishControllerStops();
  EventFunctions[0](ExitEvent,NULL);CHECK(Handoff[0]==7);
  ResetEntry(Original,0);BadHandoff=TRUE;
  CHECK(T70I2cPlatformEntry(NULL,NULL)==EFI_SUCCESS && !Installs);
  BadHandoff=FALSE;EntryBytes=Bytes-1;EventFunctions[1](FdtEvent,NULL);CHECK(!Installs);
  EntryBytes=Bytes;EventFunctions[1](FdtEvent,NULL);CHECK(Installs==I2cBusCount);
  // Late publication after BDS must bind both buses without another ConnectAll.
  ResetEntry(NULL,0);CHECK(T70I2cPlatformEntry(NULL,NULL)==EFI_SUCCESS);
  CHECK(!ConnectCalls[0] && !ConnectCalls[1] && !ConnectCalls[2]);
  EntryTree=Original;Observe(NULL,NULL);
  CHECK(Masters[0] && Masters[1] && Masters[2] && ConnectCalls[0]==1 && ConnectCalls[1]==1 && ConnectCalls[2]==1);
  for(unsigned I=0;I<120;I++)BindControllers(NULL,NULL);
  CHECK(ConnectCalls[0]==1 && ConnectCalls[1]==1 && ConnectCalls[2]==1 && Installs==I2cBusCount);
  ResetEntry(Original,0);ConnectFailures=I2cBusCount;Reenter=TRUE;
  CHECK(T70I2cPlatformEntry(NULL,NULL)==EFI_SUCCESS && !Masters[0] && !Masters[1] && !Masters[2]);
  for(unsigned I=0;I<120;I++)BindControllers(NULL,NULL);
  CHECK(Masters[0] && Masters[1] && Masters[2] && ConnectCalls[0]==2 && ConnectCalls[1]==2 && ConnectCalls[2]==2 && Installs==I2cBusCount);
  ResetEntry(Original,0);NoMaster=TRUE;
  CHECK(T70I2cPlatformEntry(NULL,NULL)==EFI_SUCCESS);
  for(unsigned I=0;I<2000;I++)BindControllers(NULL,NULL);
  CHECK(ConnectCalls[0]==16 && ConnectCalls[1]==16 && ConnectCalls[2]==16 && Installs==I2cBusCount);
  ExportToAcpi(NULL,NULL);CHECK(TimerCancels==1);
  BindControllers(NULL,NULL);CHECK(ConnectCalls[0]==16 && ConnectCalls[1]==16 && ConnectCalls[2]==16);
  for(unsigned Failure=1;Failure<=2;Failure++) {
    ResetEntry(Original,0);TimerFailure=Failure;
    CHECK(T70I2cPlatformEntry(NULL,NULL)==EFI_SUCCESS && Masters[0] && Masters[1] && Masters[2]);
    CHECK(!BindingEvent && EventCloses==(Failure==2));
    ExportToAcpi(NULL,NULL);CHECK(!TimerCancels);
  }
  free(Original);free(Tree);
  printf("I2C platform: %u checks passed (actual stock DT, translated-DMA rejection, live clock/pin/idle gates, ACPI handoff and event lifetime)\n",Checks);
}
