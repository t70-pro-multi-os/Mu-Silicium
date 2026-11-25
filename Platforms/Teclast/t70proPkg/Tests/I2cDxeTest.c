// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#include <Library/MtkI2cCore.h>
#include "../../../../Silicon/MediaTek/MediaTekPkg/Drivers/I2cDxe/I2cDxe.c"
EFI_GUID gMediaTekI2cControllerProtocolGuid={1},gEfiI2cMasterProtocolGuid={2},
         gMediaTekI2cExclusiveProtocolGuid={4},gEfiEventExitBootServicesGuid={3};
static EFI_DXE_SERVICES DxeServices;
EFI_DXE_SERVICES *gDS=&DxeServices;
VOID EFIAPI MemoryFence(VOID) {}
UINT8 EFIAPI MmioRead8(UINTN A) { CHECK(0); return 0; }
UINT16 EFIAPI MmioRead16(UINTN A) { CHECK(0); return 0; }
UINT32 EFIAPI MmioRead32(UINTN A) { CHECK(0); return 0; }
UINT8 EFIAPI MmioWrite8(UINTN A,UINT8 V) { CHECK(0); return V; }
UINT16 EFIAPI MmioWrite16(UINTN A,UINT16 V) { CHECK(0); return V; }
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V) { CHECK(0); return V; }
VOID *EFIAPI AllocateZeroPool(UINTN N) { return calloc(1,N); }
static unsigned DmaAllocs,DmaFrees,Closed;
static MTK_I2C_CONTROLLER Board;
static EFI_I2C_MASTER_PROTOCOL *Installed;
static MTK_I2C_EXCLUSIVE_PROTOCOL *InstalledExclusive;
static EFI_EVENT_NOTIFY ExitNotify;
static VOID *ExitNotifyContext;
static EFI_STATUS InstallStatus,UninstallStatus,GcdStatus,DmaAllocationStatus,DmaMappingStatus;
static UINT64 GcdBlockedAttributes;
static BOOLEAN FailEventClose;
static I2C_CONTEXT *LastContext;
EFI_STATUS EFIAPI DmaAllocateBuffer(EFI_MEMORY_TYPE Type,UINTN Pages,VOID **B) {
  CHECK(Type==EfiRuntimeServicesData&&Pages==PAGES);
  if(EFI_ERROR(DmaAllocationStatus))return DmaAllocationStatus;
  *B=calloc(Pages,4096);DmaAllocs++;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI DmaMap(DMA_MAP_OPERATION Op,VOID *B,UINTN *N,UINT64 Io,UINT32 Id,PHYSICAL_ADDRESS *A,VOID **M) {
  CHECK(Op==MapOperationBusMasterCommonBuffer&&*N==HALF*2);
  if(EFI_ERROR(DmaMappingStatus))return DmaMappingStatus;
  *A=0x10000;*M=B;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI DmaUnmap(VOID *M) { return EFI_SUCCESS; }
EFI_STATUS EFIAPI DmaFreeBuffer(UINTN Pages,VOID *B) { DmaFrees++; free(B); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D) {
  *D=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){.BaseAddress=A&~0xfffULL,.Length=0x1000,
      .GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo,.Attributes=EFI_MEMORY_UC|GcdBlockedAttributes}; return GcdStatus;
}
static EFI_STATUS EFIAPI Open(EFI_HANDLE H,EFI_GUID *G,VOID **Out,EFI_HANDLE Agent,EFI_HANDLE Controller,UINT32 Flags) {
  *Out=&Board; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_HANDLE H,EFI_GUID *G,EFI_HANDLE Agent,EFI_HANDLE Controller) { Closed++; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Event(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY Notify,VOID *Context,EFI_EVENT *Out) { LastContext=Context; *Out=(VOID *)0x1111; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI EventEx(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Group,EFI_EVENT *Out) { ExitNotify=Notify; ExitNotifyContext=(VOID *)Context; *Out=(VOID *)0x2222; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI CloseEvent(EFI_EVENT E) { return FailEventClose && E==(VOID *)0x1111?EFI_DEVICE_ERROR:EFI_SUCCESS; }
static EFI_STATUS EFIAPI InstallMaster(EFI_HANDLE *H,...) {
  EFI_GUID *Guid; va_list A; va_start(A,H);
  while ((Guid=va_arg(A,EFI_GUID *))!=NULL) {
    VOID *Interface=va_arg(A,VOID *);
    if (Guid==&gEfiI2cMasterProtocolGuid) Installed=Interface;
    else if (Guid==&gMediaTekI2cExclusiveProtocolGuid) InstalledExclusive=Interface;
    else CHECK(0);
  }
  va_end(A);
  if (EFI_ERROR(InstallStatus)) Installed=NULL,InstalledExclusive=NULL;
  return InstallStatus;
}
static EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H,...) {
  if (!EFI_ERROR(UninstallStatus)) Installed=NULL,InstalledExclusive=NULL;
  return UninstallStatus;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **Out) {
  if (G==&gEfiI2cMasterProtocolGuid) *Out=Installed;
  else if (G==&gMediaTekI2cExclusiveProtocolGuid) *Out=InstalledExclusive;
  else return EFI_NOT_FOUND;
  return *Out?EFI_SUCCESS:EFI_NOT_FOUND;
}
static MI_STATUS EngineStatus;
static unsigned Transfers, Stops, Timers, Signals;
static EFI_STATUS TimerStatus;
static EFI_TPL CurrentTpl=TPL_APPLICATION;
static I2C_CONTEXT *ReentryContext;
static EFI_I2C_REQUEST_PACKET *ReentryPacket;
static UINT64 ReentryToken;
static BOOLEAN ProbeReentry;
static unsigned ObserverCalls;
static EFI_I2C_REQUEST_PACKET *ObserverPacket;
static EFI_STATUS ObserverPublicStatus,ObserverI2cStatus,ObserverUnregisterStatus;
static EFI_TPL EFIAPI Raise(EFI_TPL T) { EFI_TPL Old=CurrentTpl; CHECK(T>=Old); CurrentTpl=T; return Old; }
static VOID EFIAPI Restore(EFI_TPL T) { CurrentTpl=T; }
static EFI_STATUS EFIAPI Timer(EFI_EVENT E,EFI_TIMER_DELAY Type,UINT64 Trigger) { Timers++; return TimerStatus; }
static EFI_STATUS EFIAPI Signal(EFI_EVENT E) { CHECK(E==(VOID *)0x1234); Signals++; return EFI_SUCCESS; }
MI_STATUS MiInitialize(MI_CONTROLLER *C,const MI_IO *I,MI_U32 Hz,MI_U32 Speed) {
  C->Io=*I; C->SourceHz=Hz; C->RequestedHz=Speed; C->Ready=EngineStatus==MiOk;
  C->DmaSafe=C->Ready; return EngineStatus;
}
MI_STATUS MiStop(MI_CONTROLLER *C) { Stops++; C->Ready=0; C->DmaSafe=EngineStatus!=MiDmaStuck; return EngineStatus==MiDmaStuck?MiDmaStuck:MiOk; }
MI_STATUS MiSetFrequency(MI_CONTROLLER *C,MI_U32 Speed,MI_U32 *Actual) {
  if (EngineStatus!=MiOk) return EngineStatus;
  *Actual=Speed-1; C->RequestedHz=Speed; return MiOk;
}
MI_STATUS MiTransfer(MI_CONTROLLER *C,unsigned Address,const MI_U8 *Tx,MI_U32 TxLen,MI_U64 TxDma,
                    MI_U8 *Rx,MI_U32 RxLen,MI_U64 RxDma) {
  Transfers++; CHECK(Address==0x48); CHECK(TxLen==2&&Tx&&Tx[0]==0x12&&Tx[1]==0x34);
  CHECK(Rx&&RxLen==3&&RxDma==TxDma+HALF);
  if (ProbeReentry) {
    UINT64 NestedToken=0;
    ProbeReentry=FALSE;
    CHECK(StartRequest(&ReentryContext->Master,0x48,ReentryPacket,NULL,NULL)==EFI_ACCESS_DENIED);
    CHECK(AcquireExclusive(&ReentryContext->Exclusive,&NestedToken)==EFI_ACCESS_DENIED&&NestedToken==0);
    CHECK(TransferExclusive(&ReentryContext->Exclusive,ReentryToken,0x48,ReentryPacket,NULL)==EFI_ALREADY_STARTED);
    CHECK(ReleaseExclusive(&ReentryContext->Exclusive,ReentryToken)==EFI_ALREADY_STARTED);
  }
  Rx[0]=0xa1; Rx[1]=0xb2; Rx[2]=0xc3; return EngineStatus;
}
MI_STATUS MiTransferTimed(MI_CONTROLLER *C,unsigned Address,const MI_U8 *Tx,MI_U32 TxLen,
                          MI_U64 TxDma,MI_U8 *Rx,MI_U32 RxLen,MI_U64 RxDma,MI_U32 TimeoutUs) {
  CHECK(TimeoutUs==5000 && CurrentTpl==TPL_APPLICATION);
  return MiTransfer(C,Address,Tx,TxLen,TxDma,Rx,RxLen,RxDma);
}
MI_STATUS MiSequence(MI_CONTROLLER *C,unsigned Address,const MI_MESSAGE *M,unsigned Count,unsigned *Done) {
  unsigned I; Transfers++; CHECK(Address==0x48 && Count<=MI_MAX_MESSAGES);
  *Done=EngineStatus==MiOk?Count:0;
  for (I=0;I<Count;I++) {
    CHECK(M[I].Buffer && M[I].Length && M[I].Dma>=0x10000 && !M[I].DelayUs);
    if (M[I].Read) for (unsigned J=0;J<M[I].Length;J++) M[I].Buffer[J]=(UINT8)(0xc0+I+J);
    else CHECK(M[I].Buffer[0]==0x12);
  }
  return EngineStatus;
}
static VOID EFIAPI OtherObserver(EFI_EVENT Event,VOID *Context) { (void)Event; (void)Context; }
static VOID EFIAPI SampleObserver(EFI_EVENT Event,VOID *Context) {
  I2C_CONTEXT *C=Context; MTK_I2C_EXCLUSIVE_PROTOCOL *X=&C->Exclusive;
  UINT64 Token=0; UINTN Hz=100000; unsigned StopCount=Stops;
  ObserverCalls++;
  CHECK(Event==(VOID *)0x2222&&CurrentTpl==TPL_NOTIFY);
  CHECK(C->ExitObserving&&C->ExitObserverRunning&&!C->Quiescing&&C->Core.Ready);
  CHECK(!C->Busy && !C->LeaseActive && !C->Packet && !C->Completion);
  ObserverPublicStatus=StartRequest(&C->Master,0x48,ObserverPacket,NULL,NULL);
  CHECK(ObserverPublicStatus==EFI_NOT_READY);
  CHECK(Frequency(&C->Master,&Hz)==EFI_NOT_READY);
  CHECK(Reset(&C->Master)==EFI_NOT_READY);
  ObserverUnregisterStatus=X->UnregisterExitObserver(X,SampleObserver,Context);
  CHECK(ObserverUnregisterStatus==EFI_NOT_READY);
  CHECK(X->Acquire(X,&Token)==EFI_SUCCESS&&Token!=0&&C->ExitObserverToken==Token);
  CHECK(X->Transfer(X,Token+1,0x48,ObserverPacket,NULL)==EFI_ACCESS_DENIED);
  CHECK(X->Transfer(X,Token,0x48,ObserverPacket,&ObserverI2cStatus)==EFI_SUCCESS);
  CHECK(ObserverI2cStatus==EFI_SUCCESS);
  CHECK(X->Release(X,Token)==EFI_SUCCESS);
  CHECK(X->Transfer(X,Token,0x48,ObserverPacket,NULL)==EFI_ACCESS_DENIED);
  ExitBootServices(Event,C);
  CHECK(ObserverCalls==1&&Stops==StopCount&&C->ExitObserverRunning&&!C->Quiescing&&C->Core.Ready);
}
int main(void) {
  I2C_CONTEXT C={.Signature=SIGN}; MTK_I2C_CONTROLLER B={.SourceClockHz=136000000};
  I2C_CONTEXT ObserverC={.Signature=SIGN};
  struct { UINTN OperationCount; EFI_I2C_OPERATION Operation[MI_MAX_MESSAGES]; } Packet;
  EFI_I2C_REQUEST_PACKET *P=(VOID *)&Packet;
  UINT8 Tx[]={0x12,0x34},Rx[3]={0}; static UINT8 Buffer[HALF*2];
  EFI_STATUS S=0; UINTN Hz=100000; UINT64 Token=0,StaleToken=0;
  Services.RaiseTPL=Raise; Services.RestoreTPL=Restore; Services.SetTimer=Timer; Services.SignalEvent=Signal;
  C.Board=&B; C.Buffer=Buffer; C.Dma=0x10000; C.Configured=TRUE; C.Core.Ready=1; C.Core.DmaSafe=1;
  C.Core.RequestedHz=400000;
  C.Master=(EFI_I2C_MASTER_PROTOCOL){Frequency,Reset,StartRequest,&mCapabilities};
  C.Exclusive=(MTK_I2C_EXCLUSIVE_PROTOCOL){MTK_I2C_EXCLUSIVE_PROTOCOL_REVISION,
      AcquireExclusive,TransferExclusive,ReleaseExclusive,RegisterExitObserver,UnregisterExitObserver,TransferTimed};
  Packet.OperationCount=2; Packet.Operation[0]=(EFI_I2C_OPERATION){0,2,Tx};
  Packet.Operation[1]=(EFI_I2C_OPERATION){I2C_FLAG_READ,3,Rx};
  CHECK(Frequency(NULL,&Hz)==EFI_INVALID_PARAMETER);
  CHECK(Reset(NULL)==EFI_INVALID_PARAMETER);
  CHECK(StartRequest(NULL,0x48,P,NULL,NULL)==EFI_INVALID_PARAMETER);
  CHECK(Validate(0x48,P)==EFI_SUCCESS);
  CHECK(Validate(0x48,NULL)==EFI_INVALID_PARAMETER);
  CHECK(Validate(0x148,P)==EFI_NOT_FOUND);
  Packet.OperationCount=MI_MAX_MESSAGES+1; CHECK(Validate(0x48,P)==EFI_UNSUPPORTED); Packet.OperationCount=2;
  Packet.Operation[0].Flags=I2C_FLAG_READ; CHECK(Validate(0x48,P)==EFI_SUCCESS); Packet.Operation[0].Flags=0;
  Packet.Operation[1].Flags=I2C_FLAG_SMBUS_PEC; CHECK(Validate(0x48,P)==EFI_UNSUPPORTED); Packet.Operation[1].Flags=I2C_FLAG_READ;
  Packet.Operation[0].LengthInBytes=65536; CHECK(Validate(0x48,P)==EFI_BAD_BUFFER_SIZE); Packet.Operation[0].LengthInBytes=2;
  CHECK(StartRequest(&C.Master,0x48,P,NULL,&S)==EFI_SUCCESS&&S==EFI_SUCCESS);
  CHECK(Transfers==1&&!C.Busy&&Rx[0]==0xa1&&Rx[2]==0xc3&&CurrentTpl==TPL_APPLICATION);
  memset(Rx,0,sizeof(Rx)); EngineStatus=MiNack;
  CHECK(StartRequest(&C.Master,0x48,P,NULL,&S)==EFI_DEVICE_ERROR&&S==EFI_DEVICE_ERROR);
  CHECK(!C.Busy&&Rx[0]==0&&Rx[2]==0);
  EngineStatus=MiOk;
  CHECK(StartRequest(&C.Master,0x48,P,(VOID *)0x1234,&S)==EFI_SUCCESS&&S==EFI_NOT_READY);
  CHECK(C.Busy&&Timers==1&&Transfers==2&&Signals==0);
  CHECK(StartRequest(&C.Master,0x48,P,NULL,NULL)==EFI_ALREADY_STARTED);
  Token=123; CHECK(AcquireExclusive(&C.Exclusive,&Token)==EFI_ALREADY_STARTED&&Token==0);
  CHECK(Frequency(&C.Master,&Hz)==EFI_ALREADY_STARTED);
  Worker(NULL,&C); CHECK(!C.Busy&&Transfers==3&&Signals==1&&S==EFI_SUCCESS);
  TimerStatus=EFI_OUT_OF_RESOURCES;
  CHECK(StartRequest(&C.Master,0x48,P,(VOID *)0x1234,&S)==EFI_OUT_OF_RESOURCES);
  CHECK(!C.Busy&&!C.Packet&&!C.Completion&&!C.CompletionStatus&&Transfers==3);
  TimerStatus=EFI_SUCCESS;
  CHECK(AcquireExclusive(&C.Exclusive,&Token)==EFI_SUCCESS&&Token==1);
  CHECK(StartRequest(&C.Master,0x48,P,NULL,NULL)==EFI_ACCESS_DENIED);
  CHECK(TransferExclusive(&C.Exclusive,Token+1,0x48,P,NULL)==EFI_ACCESS_DENIED);
  CHECK(ReleaseExclusive(&C.Exclusive,Token+1)==EFI_ACCESS_DENIED);
  ReentryContext=&C; ReentryPacket=P; ReentryToken=Token; ProbeReentry=TRUE;
  CHECK(TransferExclusive(&C.Exclusive,Token,0x48,P,NULL)==EFI_SUCCESS&&!ProbeReentry&&!C.Busy);
  CHECK(TransferTimed(&C.Exclusive,Token,0x48,P,5000)==EFI_SUCCESS);
  StaleToken=Token;
  CHECK(ReleaseExclusive(&C.Exclusive,Token)==EFI_SUCCESS);
  CHECK(TransferExclusive(&C.Exclusive,StaleToken,0x48,P,NULL)==EFI_ACCESS_DENIED);
  CHECK(ReleaseExclusive(&C.Exclusive,StaleToken)==EFI_ACCESS_DENIED);
  CHECK(RegisterExitObserver(&C.Exclusive,NULL,&C)==EFI_INVALID_PARAMETER);
  CHECK(AcquireExclusive(&C.Exclusive,&Token)==EFI_SUCCESS);
  CHECK(RegisterExitObserver(&C.Exclusive,OtherObserver,&C)==EFI_ACCESS_DENIED);
  CHECK(ReleaseExclusive(&C.Exclusive,Token)==EFI_SUCCESS);
  C.Busy=TRUE; CHECK(RegisterExitObserver(&C.Exclusive,SampleObserver,&C)==EFI_ALREADY_STARTED); C.Busy=FALSE;
  CHECK(RegisterExitObserver(&C.Exclusive,SampleObserver,&C)==EFI_SUCCESS);
  CHECK(RegisterExitObserver(&C.Exclusive,SampleObserver,&C)==EFI_ALREADY_STARTED);
  CHECK(RegisterExitObserver(&C.Exclusive,OtherObserver,&C)==EFI_ACCESS_DENIED);
  CHECK(UnregisterExitObserver(&C.Exclusive,SampleObserver,NULL)==EFI_ACCESS_DENIED);
  CHECK(AcquireExclusive(&C.Exclusive,&Token)==EFI_SUCCESS);
  CHECK(UnregisterExitObserver(&C.Exclusive,SampleObserver,&C)==EFI_ACCESS_DENIED);
  CHECK(ReleaseExclusive(&C.Exclusive,Token)==EFI_SUCCESS);
  C.Busy=TRUE; CHECK(UnregisterExitObserver(&C.Exclusive,SampleObserver,&C)==EFI_ALREADY_STARTED); C.Busy=FALSE;
  CHECK(UnregisterExitObserver(&C.Exclusive,SampleObserver,&C)==EFI_SUCCESS);
  CHECK(UnregisterExitObserver(&C.Exclusive,SampleObserver,&C)==EFI_NOT_FOUND);
  C.TokenSequence=MAX_UINT64-1;
  CHECK(AcquireExclusive(&C.Exclusive,&Token)==EFI_SUCCESS&&Token==MAX_UINT64);
  CHECK(ReleaseExclusive(&C.Exclusive,Token)==EFI_SUCCESS);
  Token=123; CHECK(AcquireExclusive(&C.Exclusive,&Token)==EFI_OUT_OF_RESOURCES&&Token==0&&C.TokensExhausted);
  C.Core.Ready=0; CHECK(StartRequest(&C.Master,0x48,P,NULL,NULL)==EFI_NOT_READY);
  CHECK(Reset(&C.Master)==EFI_SUCCESS&&!C.Configured&&C.Core.Ready);
  CHECK(StartRequest(&C.Master,0x48,P,NULL,NULL)==EFI_NOT_READY);
  CHECK(Frequency(&C.Master,&Hz)==EFI_SUCCESS&&Hz==99999&&C.Configured);
  EngineStatus=MiUnsupported; Hz=1000000;
  CHECK(Frequency(&C.Master,&Hz)==EFI_UNSUPPORTED&&Hz==1000000&&!C.Busy);
  CHECK(Frequency(&C.Master,NULL)==EFI_INVALID_PARAMETER);
  EngineStatus=MiOk;
  CHECK(StartRequest(&C.Master,0x48,P,(VOID *)0x1234,&S)==EFI_SUCCESS && C.Busy);
  C.Core.Io.Read=ReadRegister;
  unsigned BeforeSignals=Signals;
  ExitBootServices(NULL,&C);
  CHECK(S==EFI_ABORTED && !C.Packet && !C.Completion && !C.CompletionStatus && !C.Configured);
  S=EFI_ACCESS_DENIED;Worker(NULL,&C);CHECK(S==EFI_ACCESS_DENIED && Signals==BeforeSignals);
  CHECK(C.Quiescing&&!C.Busy&&!C.Core.Ready&&C.Core.DmaSafe&&Stops==1);
  CHECK(Reset(&C.Master)==EFI_NOT_READY);
  ObserverC.Board=&B; ObserverC.Buffer=Buffer; ObserverC.Dma=0x10000;
  ObserverC.Configured=TRUE; ObserverC.Core.Ready=1; ObserverC.Core.DmaSafe=1;
  ObserverC.Core.Io.Read=ReadRegister;
  ObserverC.Master=(EFI_I2C_MASTER_PROTOCOL){Frequency,Reset,StartRequest,&mCapabilities};
  ObserverC.Exclusive=(MTK_I2C_EXCLUSIVE_PROTOCOL){MTK_I2C_EXCLUSIVE_PROTOCOL_REVISION,
      AcquireExclusive,TransferExclusive,ReleaseExclusive,RegisterExitObserver,UnregisterExitObserver,TransferTimed};
  CHECK(RegisterExitObserver(&ObserverC.Exclusive,SampleObserver,&ObserverC)==EFI_SUCCESS);
  CHECK(RegisterExitObserver(&ObserverC.Exclusive,SampleObserver,&ObserverC)==EFI_ALREADY_STARTED);
  CHECK(RegisterExitObserver(&ObserverC.Exclusive,OtherObserver,&ObserverC)==EFI_ACCESS_DENIED);
  ObserverPacket=P;
  ObserverC.Worker=(VOID *)0x1111;
  CHECK(StartRequest(&ObserverC.Master,0x48,P,(VOID *)0x1234,&S)==EFI_SUCCESS);
  CurrentTpl=TPL_NOTIFY; EngineStatus=MiOk;
  ExitBootServices((VOID *)0x2222,&ObserverC);
  CHECK(S==EFI_ABORTED);
  CHECK(ObserverCalls==1&&ObserverPublicStatus==EFI_NOT_READY&&ObserverUnregisterStatus==EFI_NOT_READY);
  CHECK(ObserverC.ExitObserving&&ObserverC.ExitObserverNotified&&!ObserverC.ExitObserverRunning);
  CHECK(ObserverC.Quiescing&&!ObserverC.Configured&&!ObserverC.Busy&&!ObserverC.LeaseActive&&!ObserverC.Core.Ready);
  CHECK(TransferExclusive(&ObserverC.Exclusive,1,0x48,P,NULL)==EFI_NOT_READY);
  CHECK(AcquireExclusive(&ObserverC.Exclusive,&Token)==EFI_NOT_READY);
  unsigned ObserverStops=Stops; ExitBootServices((VOID *)0x2222,&ObserverC); CHECK(Stops==ObserverStops&&ObserverCalls==1);
  CurrentTpl=TPL_APPLICATION;
  C.Quiescing=FALSE; C.Configured=TRUE; C.Core.Ready=1; C.ExitObserving=FALSE;
  C.ExitObserverNotified=FALSE; C.ExitObserverRunning=FALSE; C.ExitObserverToken=0;
  EngineStatus=MiOk;
  Packet.OperationCount=3; Packet.Operation[2]=(EFI_I2C_OPERATION){0,2,Tx}; memset(Rx,0,3);
  unsigned Prior=Transfers;
  CHECK(StartRequest(&C.Master,0x48,P,NULL,&S)==EFI_SUCCESS && Transfers==Prior+1 && Rx[0]==0xc1 && !C.Busy);
  EngineStatus=MiNack; memset(Rx,0,3);
  CHECK(StartRequest(&C.Master,0x48,P,NULL,&S)==EFI_DEVICE_ERROR && !Rx[0] && !C.Busy);
  EngineStatus=MiOk; Packet.OperationCount=2;
  Services.OpenProtocol=Open; Services.CloseProtocol=Close; DxeServices.GetMemorySpaceDescriptor=Gcd;
  Services.CreateEvent=Event; Services.CreateEventEx=EventEx; Services.CloseEvent=CloseEvent;
  Services.InstallMultipleProtocolInterfaces=InstallMaster; Services.UninstallMultipleProtocolInterfaces=Uninstall;
  Services.HandleProtocol=Handle;
  Board=(MTK_I2C_CONTROLLER){.Revision=MTK_I2C_CONTROLLER_REVISION,.Flags=3,.SourceClockHz=136000000,
      .ControllerBase=0x11e00000,.ControllerSize=0x1000,.DmaBase=0x10217080,.DmaSize=0x80};
  CHECK(BoardValid(NULL)==EFI_UNSUPPORTED);
  CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS);
  CHECK(Board.DriverStartPhase==1U&&Board.DriverStartStatus==EFI_SUCCESS);
  Board.Flags=0; CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_UNSUPPORTED&&DmaAllocs==0); Board.Flags=3;
  GcdStatus=EFI_NOT_FOUND; CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_UNSUPPORTED&&DmaAllocs==0); GcdStatus=EFI_SUCCESS;
  CONST UINT64 Protected[]={EFI_MEMORY_RO,EFI_MEMORY_RP,EFI_MEMORY_WP};
  for(UINTN I=0U;I<ARRAY_SIZE(Protected);++I) {
    GcdBlockedAttributes=Protected[I];
    CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_UNSUPPORTED&&DmaAllocs==0);
  }
  GcdBlockedAttributes=0U;
  CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS&&Installed&&InstalledExclusive&&DmaAllocs==1);
  CHECK(Board.DriverStartPhase==0x1aU&&Board.DriverStartStatus==EFI_SUCCESS);
  CHECK(CTX(Installed)->Core.RequestedHz==400000U);
  CHECK(ExitNotify==ExitBootServices&&ExitNotifyContext==CTX(Installed));
  CHECK(InstalledExclusive->RegisterExitObserver(InstalledExclusive,SampleObserver,Installed)==EFI_SUCCESS);
  CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_NOT_READY&&Installed);
  CHECK(InstalledExclusive->UnregisterExitObserver(InstalledExclusive,SampleObserver,(VOID *)2)==EFI_ACCESS_DENIED);
  CHECK(InstalledExclusive->UnregisterExitObserver(InstalledExclusive,SampleObserver,Installed)==EFI_SUCCESS);
  CHECK(InstalledExclusive->Acquire(InstalledExclusive,&Token)==EFI_SUCCESS);
  CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_NOT_READY&&Installed);
  CHECK(InstalledExclusive->Release(InstalledExclusive,Token)==EFI_SUCCESS);
  UninstallStatus=EFI_ACCESS_DENIED;
  CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_ACCESS_DENIED&&Installed&&DmaFrees==0);
  CHECK(!CTX(Installed)->Core.Ready&&!CTX(Installed)->Configured&&CTX(Installed)->Quiescing);
  UninstallStatus=EFI_SUCCESS; EngineStatus=MiDmaStuck;
  CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_DEVICE_ERROR&&Installed&&DmaFrees==0);
  EngineStatus=MiOk;
  CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_SUCCESS&&!Installed&&DmaFrees==1);
  InstallStatus=EFI_OUT_OF_RESOURCES;
  CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_OUT_OF_RESOURCES&&DmaAllocs==2&&DmaFrees==2);
  CHECK(Board.DriverStartPhase==0x19U&&Board.DriverStartStatus==EFI_OUT_OF_RESOURCES);
  InstallStatus=EFI_SUCCESS;
  CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS && DmaAllocs==3);
  I2C_CONTEXT *Held=CTX(Installed);FailEventClose=TRUE;TimerStatus=EFI_DEVICE_ERROR;
  CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_DEVICE_ERROR && Installed && DmaFrees==2);
  CHECK(Held->Quiescing && !Held->Configured && Held->Worker && !Held->Exit);
  Worker(NULL,Held);CHECK(!Held->Busy);
  FailEventClose=FALSE;TimerStatus=EFI_SUCCESS;
  CHECK(Stop(&mBinding,(VOID *)1,0,NULL)==EFI_SUCCESS && !Installed && DmaFrees==3);
  FailEventClose=TRUE;InstallStatus=EFI_OUT_OF_RESOURCES;
  CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_OUT_OF_RESOURCES && !Installed && DmaAllocs==4 && DmaFrees==3);
  Held=LastContext;CHECK(Held && Held->Quiescing && Held->Buffer && Held->Worker);
  Worker(NULL,Held);ExitBootServices(NULL,Held);CHECK(!Held->Busy && !Held->CompletionStatus);
  FailEventClose=FALSE;CHECK(CloseEvents(Held)==EFI_SUCCESS);
  DmaUnmap(Held->Mapping);DmaFreeBuffer(PAGES,Held->Buffer);FreePool(Held);
  InstallStatus=EFI_SUCCESS;EngineStatus=MiOk;
  Board.IommuBase=1U;CHECK(BoardValid(&Board)==EFI_UNSUPPORTED);Board.IommuBase=0U;
  Board.DmaId=1U;CHECK(BoardValid(&Board)==EFI_UNSUPPORTED);Board.DmaId=0U;
  DmaAllocationStatus=EFI_UNSUPPORTED;
  CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_UNSUPPORTED&&!Installed);
  CHECK(Board.DriverStartPhase==0x13U&&Board.DriverStartStatus==EFI_UNSUPPORTED);
  CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_SUCCESS&&Board.DriverStartPhase==0x13U);
  DmaAllocationStatus=EFI_SUCCESS;DmaMappingStatus=EFI_OUT_OF_RESOURCES;
  CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_OUT_OF_RESOURCES&&!Installed);
  CHECK(Board.DriverStartPhase==0x14U&&Board.DriverStartStatus==EFI_OUT_OF_RESOURCES);
  DmaMappingStatus=EFI_SUCCESS;EngineStatus=MiUnsupported;
  CHECK(Start(&mBinding,(VOID *)1,NULL)==EFI_UNSUPPORTED&&!Installed);
  CHECK(Board.DriverStartPhase==0x18U&&Board.DriverStartStatus==EFI_UNSUPPORTED);
  EngineStatus=MiOk;Board.Revision=1U;Board.DriverStartPhase=0U;
  CHECK(Supported(&mBinding,(VOID *)1,NULL)==EFI_UNSUPPORTED&&Board.DriverStartPhase==0U);
  Board.Revision=MTK_I2C_CONTROLLER_REVISION;
  printf("I2C DXE: %u checks passed (validation, async completion, retry/error state, OS handoff, failed event close and stale callback)\n",Checks);
}
