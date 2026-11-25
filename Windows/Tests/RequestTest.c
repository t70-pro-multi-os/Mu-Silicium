// SPDX-License-Identifier: BSD-2-Clause-Patent
// Runs the production Driver.c as real Windows ARM64 code in an emulator.
// WDF/SpbCx and kernel objects below are mocks, not a running Windows kernel.
#include <ntddk.h>
UCHAR TestRead8(PUCHAR Address);
USHORT TestRead16(PUSHORT Address);
ULONG TestRead32(PULONG Address);
VOID TestWrite8(PUCHAR Address,UCHAR Value);
VOID TestWrite16(PUSHORT Address,USHORT Value);
VOID TestWrite32(PULONG Address,ULONG Value);
PVOID TestMap(PHYSICAL_ADDRESS Address,SIZE_T Length,ULONG Flags);
VOID TestUnmap(PVOID Base,SIZE_T Length);
#undef READ_REGISTER_UCHAR
#undef READ_REGISTER_USHORT
#undef READ_REGISTER_ULONG
#undef WRITE_REGISTER_UCHAR
#undef WRITE_REGISTER_USHORT
#undef WRITE_REGISTER_ULONG
#define READ_REGISTER_UCHAR TestRead8
#define READ_REGISTER_USHORT TestRead16
#define READ_REGISTER_ULONG TestRead32
#define WRITE_REGISTER_UCHAR TestWrite8
#define WRITE_REGISTER_USHORT TestWrite16
#define WRITE_REGISTER_ULONG TestWrite32
#define MmMapIoSpaceEx TestMap
#define MmUnmapIoSpace TestUnmap
#include "../MtkI2c/Driver.c"
static unsigned Failure,Transfers,Completions,Locks,References;
__declspec(dllexport) unsigned MtkI2cAssertionCount;
#define Checks MtkI2cAssertionCount
static BOOLEAN InterruptObjectAlive;
#define CHECK(X) do { Checks++; if (!(X) && !Failure) Failure=__LINE__; } while(0)
static DEVICE_CONTEXT D;
static TARGET_CONTEXT T;
static REQUEST_CONTEXT R;
static UCHAR Bounce[HALF*2],Tx[2],Rx[3];
static MDL TxMdl,RxMdl,RxTail;
static SPB_REQUEST_PARAMETERS Params;
static SPB_TRANSFER_DESCRIPTOR Descriptors[MI_MAX_MESSAGES];
static PMDL Mdls[MI_MAX_MESSAGES];
static unsigned SequenceCalls,SequenceDone;
static NTSTATUS Result,EngineResult,MarkResult;
static ULONG_PTR Information;
static int CancelDuringTransfer,CancelOnUnmark;
static PFN_WDF_REQUEST_CANCEL CancelCallback;
static UCHAR Registers[0x1000],DmaRegisters[0x80];
static CM_PARTIAL_RESOURCE_DESCRIPTOR RawResources[3],TranslatedResources[3];
static unsigned RawCount,TranslatedCount,Maps,Unmaps,MapFailure,Deleted,IrqDepth,IrqCreates;
static unsigned AcpiVariant,AcpiCalls,InitializeCalls,StopCalls,Waits,WakeSignals,CancelSignals;
static ULONG LiveClock, ResumeClock;
static unsigned LiveClockCalls, AcpiFaultMethod, ResumeCalls;
static NTSTATUS ResumeStatus;
static ULONGLONG DmaAddress;
static NTSTATUS EnablerStatus,BufferStatus,IrqStatus,AcpiStatus;
static MI_STATUS InitializeResult,StopResult;
static int UseWait,WaitCancel,NullDescriptor,CancelOnMark;
static BOOLEAN Fire(void);
static WDF_INTERRUPT_CONFIG InterruptConfig;
static UCHAR ConnectionStorage[128];
static PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Connection=(PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER)ConnectionStorage;
static WDFFUNC Table[WdfFunctionTableNumEntries];
const WDFFUNC *WdfFunctions=Table;
PWDF_DRIVER_GLOBALS WdfDriverGlobals;
SPBFUNC SpbFunctions[SpbFunctionTableNumEntries];
PSPB_DRIVER_GLOBALS SpbDriverGlobals;
BOOLEAN WdfClientVersionHigherThanFramework;
ULONG WdfStructureCount;
WDF_STRUCT_INFO WdfStructures;
#pragma function(memcpy,memset)
void *memcpy(void *To,const void *From,size_t N) {
  volatile UCHAR *A=To; const volatile UCHAR *B=From;
  while (N--) *A++=*B++; return To;
}
void *memset(void *To,int V,size_t N) {
  volatile UCHAR *A=To; while (N--) *A++=(UCHAR)V; return To;
}
VOID KeInitializeEvent(PRKEVENT E,EVENT_TYPE Type,BOOLEAN State) { memset(E,0,sizeof(*E)); E->Header.Type=(UCHAR)Type; E->Header.SignalState=State; }
VOID KeClearEvent(PRKEVENT E) { E->Header.SignalState=0; }
LONG KeSetEvent(PRKEVENT E,KPRIORITY Priority,BOOLEAN Wait) {
  if (E==&D.TransferEvent) WakeSignals++;
  if (E==&R.CancelDone) { CHECK(WakeSignals>0); CancelSignals++; }
  E->Header.SignalState=1; return 0;
}
NTSTATUS KeWaitForSingleObject(PVOID E,KWAIT_REASON Reason,KPROCESSOR_MODE Mode,BOOLEAN Alertable,PLARGE_INTEGER Timeout) {
  PRKEVENT Event=E;
  CHECK(Reason==Executive && Mode==KernelMode && !Alertable && !IrqDepth);
  if (E==&D.TransferEvent) {
    Waits++; CHECK(Timeout && Timeout->QuadPart<0 && Timeout->QuadPart>=-50000);
    if (WaitCancel) { WaitCancel=0; CancelCallback((WDFREQUEST)3); CHECK(Event->Header.SignalState==1); }
    if (!Event->Header.SignalState) return STATUS_TIMEOUT;
    CHECK(Event->Header.Type==SynchronizationEvent); Event->Header.SignalState=0;
  } else CHECK(E==&R.CancelDone && !Timeout && Event->Header.SignalState==1);
  return STATUS_SUCCESS;
}
ULONGLONG KeQueryInterruptTimePrecise(PULONGLONG T) { *T=1000; return 1000; }
VOID KeStallExecutionProcessor(ULONG Us) { CHECK(Us<=10); }
static unsigned OffsetOf(const void *P,const void *Base,unsigned Length) {
  CHECK((ULONG_PTR)P>=(ULONG_PTR)Base && (ULONG_PTR)P<(ULONG_PTR)Base+Length);
  return (unsigned)((ULONG_PTR)P-(ULONG_PTR)Base);
}
UCHAR TestRead8(PUCHAR P) { CHECK(P==Registers); return *P; }
/* The shipped mt6983_compat uses 32-bit controller MMIO, 8-bit FIFO. */
USHORT TestRead16(PUSHORT P) { CHECK(0); return 0; }
ULONG TestRead32(PULONG P) {
  if ((ULONG_PTR)P>=(ULONG_PTR)Registers && (ULONG_PTR)P<(ULONG_PTR)Registers+sizeof(Registers)) {
    unsigned Off=OffsetOf(P,Registers,sizeof(Registers));
    if (Off==8 || Off==12) CHECK(IrqDepth==1);
  } else OffsetOf(P,DmaRegisters,sizeof(DmaRegisters));
  return *P;
}
VOID TestWrite8(PUCHAR P,UCHAR V) { CHECK(P==Registers); *P=V; }
VOID TestWrite16(PUSHORT P,USHORT V) { CHECK(0); }
VOID TestWrite32(PULONG P,ULONG V) {
  if ((ULONG_PTR)P>=(ULONG_PTR)Registers && (ULONG_PTR)P<(ULONG_PTR)Registers+sizeof(Registers)) {
    unsigned Off=OffsetOf(P,Registers,sizeof(Registers));
    if (Off==8 || Off==12) CHECK(IrqDepth==1);
    if (Off==12) { *P&=~V; return; }
  } else OffsetOf(P,DmaRegisters,sizeof(DmaRegisters));
  *P=V;
}
PVOID TestMap(PHYSICAL_ADDRESS Address,SIZE_T Length,ULONG Flags) {
  Maps++; CHECK(Maps<=2 && Length==(Maps==1?0x1000:0x80));
  CHECK(Address.QuadPart==TranslatedResources[Maps-1].u.Memory.Start.QuadPart && Flags==(PAGE_READWRITE|PAGE_NOCACHE));
  if (Maps==MapFailure) return NULL;
  return Maps==1?Registers:DmaRegisters;
}
VOID TestUnmap(PVOID Base,SIZE_T Length) {
  CHECK(!IrqDepth); CHECK((Base==Registers && Length==0x1000)||(Base==DmaRegisters && Length==0x80)); Unmaps++;
}
#undef MmMapLockedPagesSpecifyCache
PVOID MmMapLockedPagesSpecifyCache(PMDL M,KPROCESSOR_MODE Mode,MEMORY_CACHING_TYPE Cache,PVOID Base,ULONG Bug,ULONG Priority) { return NULL; }
static PVOID Context(PWDF_DRIVER_GLOBALS G,WDFOBJECT H,PCWDF_OBJECT_CONTEXT_TYPE_INFO Info) {
  if (H==(WDFOBJECT)1) return &D; if (H==(WDFOBJECT)2) return &T; if (H==(WDFOBJECT)3) return &R;
  CHECK(0); return NULL;
}
static NTSTATUS Acquire(PWDF_DRIVER_GLOBALS G,WDFWAITLOCK L,PLONGLONG Timeout) { CHECK(!Locks); Locks++; return STATUS_SUCCESS; }
static VOID Unlock(PWDF_DRIVER_GLOBALS G,WDFWAITLOCK L) { CHECK(Locks==1); Locks--; }
static PVOID Virtual(PWDF_DRIVER_GLOBALS G,WDFCOMMONBUFFER B) { return Bounce; }
static PHYSICAL_ADDRESS Logical(PWDF_DRIVER_GLOBALS G,WDFCOMMONBUFFER B) { PHYSICAL_ADDRESS A; A.QuadPart=DmaAddress; return A; }
static VOID SetInfo(PWDF_DRIVER_GLOBALS G,WDFREQUEST Q,ULONG_PTR N) { Information=N; }
static NTSTATUS Mark(PWDF_DRIVER_GLOBALS G,WDFREQUEST Q,PFN_WDF_REQUEST_CANCEL C) {
  CancelCallback=C; if (CancelOnMark) C(Q); return MarkResult;
}
static NTSTATUS Unmark(PWDF_DRIVER_GLOBALS G,WDFREQUEST Q) {
  if (CancelOnUnmark || R.Cancelled) { CancelCallback(Q); return STATUS_CANCELLED; } return STATUS_SUCCESS;
}
static VOID Reference(PWDF_DRIVER_GLOBALS G,WDFOBJECT H,PVOID Tag,LONG Line,PCCH File) { References++; }
static VOID Parameters(PSPB_DRIVER_GLOBALS G,SPBREQUEST Q,SPB_REQUEST_PARAMETERS *P) { *P=Params; }
static VOID Descriptor(PSPB_DRIVER_GLOBALS G,SPBREQUEST Q,ULONG I,SPB_TRANSFER_DESCRIPTOR *X,PMDL *M) {
  CHECK(I<MI_MAX_MESSAGES); *X=Descriptors[I]; *M=Mdls[I];
}
static VOID Complete(PSPB_DRIVER_GLOBALS G,SPBREQUEST Q,NTSTATUS S) { CHECK(!Locks); Completions++; Result=S; }
MI_STATUS MiInitialize(MI_CONTROLLER *C,const MI_IO *I,MI_U32 Hz,MI_U32 Speed) {
  InitializeCalls++; CHECK(Hz==(LiveClock ? LiveClock : ResumeClock) && Speed==400000 && I->Context==&D && I->WaitForEvent==WaitForEvent);
  C->Io=*I; C->SourceHz=Hz; C->DmaSafe=InitializeResult==MiOk; C->Ready=C->DmaSafe; return InitializeResult;
}
MI_STATUS MiStop(MI_CONTROLLER *C) { StopCalls++; C->Ready=0; C->DmaSafe=StopResult==MiOk; return StopResult; }
MI_STATUS MiSetFrequency(MI_CONTROLLER *C,MI_U32 Hz,MI_U32 *Actual) { *Actual=Hz; return MiOk; }
MI_STATUS MiTransfer(MI_CONTROLLER *C,unsigned Address,const MI_U8 *TxData,MI_U32 TxLen,MI_U64 TxDma,
                    MI_U8 *RxData,MI_U32 RxLen,MI_U64 RxDma) {
  Transfers++; CHECK(Address==0x48&&TxLen==2&&RxLen==3);
  CHECK(TxData[0]==0x12&&TxData[1]==0x34&&TxDma==0x10000&&RxDma==0x20000);
  RxData[0]=0xa1; RxData[1]=0xb2; RxData[2]=0xc3;
  if (UseWait) {
    CHECK(R.Wake==&D.TransferEvent && !D.TransferEvent.Header.SignalState);
    if (UseWait==2) {
      /* IRQ between the owner's status check and its wait must stay signalled. */
      WriteRegister(&D,0,8,4,15); *(ULONG *)(Registers+12)=1;
      CHECK(Fire() && D.TransferEvent.Header.SignalState==1);
    }
    WaitForEvent(&D,9000); /* The production hook caps this to 5 ms. */
    CHECK(!D.TransferEvent.Header.SignalState);
  }
  if (CancelDuringTransfer) CancelCallback((WDFREQUEST)3);
  if (EngineResult==MiDmaStuck) { C->DmaSafe=0; C->Ready=0; }
  return EngineResult;
}
MI_STATUS MiSequence(MI_CONTROLLER *C,unsigned Address,const MI_MESSAGE *Messages,unsigned Count,unsigned *Completed) {
  unsigned I,J,Offset=0;
  Transfers++; SequenceCalls++; CHECK(Address==0x48 && Count<=MI_MAX_MESSAGES && R.Wake==&D.TransferEvent);
  *Completed=EngineResult==MiOk?Count:SequenceDone;
  CHECK(*Completed<=Count);
  for (I=0;I<Count;I++) {
    const MI_MESSAGE *M=&Messages[I];
    CHECK(M->Buffer==Bounce+Offset && M->Dma==DmaAddress+Offset);
    CHECK(M->Length==Descriptors[I].TransferLength && M->DelayUs==Descriptors[I].DelayInUs);
    CHECK(M->Read==(Descriptors[I].Direction==SpbTransferDirectionFromDevice));
    if (!M->Read) CHECK(M->Buffer[0]==Tx[0]);
    else for (J=0;J<M->Length;J++) M->Buffer[J]=(UCHAR)(0xa0+I+J);
    Offset=(Offset+M->Length+3)&~3U;
  }
  if (CancelDuringTransfer) CancelCallback((WDFREQUEST)3);
  if (EngineResult==MiDmaStuck) { C->DmaSafe=0; C->Ready=0; }
  return EngineResult;
}
static void Init(void) {
  memset(&D,0,sizeof(D)); memset(&R,0,sizeof(R)); memset(Rx,0,sizeof(Rx)); memset(Bounce,0,4); memset(Bounce+HALF,0,4);
  InterruptObjectAlive=TRUE;
  D.Device=(WDFDEVICE)1; D.Buffer=(WDFCOMMONBUFFER)4; D.Enabler=(WDFDMAENABLER)5;
  D.Lock=(WDFWAITLOCK)6; D.IrqOnline=1; D.Active=TRUE;
  KeInitializeEvent(&D.TransferEvent,SynchronizationEvent,FALSE); D.Core.Ready=1; D.Core.DmaSafe=1;
  T.Address=0x48; T.Speed=400000; Tx[0]=0x12; Tx[1]=0x34;
  memset(&TxMdl,0,sizeof(TxMdl)); memset(&RxMdl,0,sizeof(RxMdl)); memset(&RxTail,0,sizeof(RxTail));
  TxMdl.MdlFlags=MDL_MAPPED_TO_SYSTEM_VA; TxMdl.MappedSystemVa=Tx; TxMdl.ByteCount=2;
  RxMdl.MdlFlags=MDL_MAPPED_TO_SYSTEM_VA; RxMdl.MappedSystemVa=Rx; RxMdl.ByteCount=3;
  Mdls[0]=&TxMdl; Mdls[1]=&RxMdl;
  SPB_REQUEST_PARAMETERS_INIT(&Params); Params.Position=SpbRequestSequencePositionSingle;
  Params.Type=SpbRequestTypeSequence; Params.SequenceTransferCount=2;
  SPB_TRANSFER_DESCRIPTOR_INIT(&Descriptors[0]); SPB_TRANSFER_DESCRIPTOR_INIT(&Descriptors[1]);
  Descriptors[0].Direction=SpbTransferDirectionToDevice; Descriptors[0].TransferLength=2;
  Descriptors[1].Direction=SpbTransferDirectionFromDevice; Descriptors[1].TransferLength=3;
  Information=99; Transfers=Completions=Locks=References=0; SequenceCalls=SequenceDone=0;
  EngineResult=MiOk; MarkResult=STATUS_SUCCESS; CancelDuringTransfer=CancelOnUnmark=0;
  UseWait=WaitCancel=CancelOnMark=0; Waits=WakeSignals=CancelSignals=0; DmaAddress=0x10000;
}
static ULONG ResourceCount(PWDF_DRIVER_GLOBALS G,WDFCMRESLIST List) { return List==(WDFCMRESLIST)10?RawCount:TranslatedCount; }
static PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource(PWDF_DRIVER_GLOBALS G,WDFCMRESLIST List,ULONG I) {
  CHECK(I<3); if (NullDescriptor && I==1) return NULL;
  return List==(WDFCMRESLIST)10?&RawResources[I]:&TranslatedResources[I];
}
static NTSTATUS CreateEnabler(PWDF_DRIVER_GLOBALS G,WDFDEVICE Device,PWDF_DMA_ENABLER_CONFIG Config,PWDF_OBJECT_ATTRIBUTES A,WDFDMAENABLER *E) {
  CHECK(Device==(WDFDEVICE)1 && Config->Profile==WdfDmaProfileScatterGather64Duplex && Config->MaximumLength==HALF*2 && Config->AddressWidthOverride==36);
  if (NT_SUCCESS(EnablerStatus)) *E=(WDFDMAENABLER)5; return EnablerStatus;
}
static NTSTATUS CreateBuffer(PWDF_DRIVER_GLOBALS G,WDFDMAENABLER E,SIZE_T Length,PWDF_OBJECT_ATTRIBUTES A,WDFCOMMONBUFFER *B) {
  CHECK(E==(WDFDMAENABLER)5 && Length==HALF*2);
  if (NT_SUCCESS(BufferStatus)) *B=(WDFCOMMONBUFFER)4; return BufferStatus;
}
static NTSTATUS CreateInterrupt(PWDF_DRIVER_GLOBALS G,WDFDEVICE Device,PWDF_INTERRUPT_CONFIG C,PWDF_OBJECT_ATTRIBUTES A,WDFINTERRUPT *I) {
  CHECK(!InterruptObjectAlive);
  IrqCreates++; CHECK(Device==(WDFDEVICE)1 && !A);
  CHECK(C->PassiveHandling && !C->AutomaticSerialization && C->EvtInterruptIsr==InterruptIsr && !C->EvtInterruptDpc);
  CHECK(C->InterruptRaw==&RawResources[2] && C->InterruptTranslated==&TranslatedResources[2]);
  CHECK(C->EvtInterruptEnable==InterruptEnable && C->EvtInterruptDisable==InterruptDisable);
  InterruptConfig=*C;
  if (NT_SUCCESS(IrqStatus)) { *I=(WDFINTERRUPT)7; InterruptObjectAlive=TRUE; }
  return IrqStatus;
}
static VOID Delete(PWDF_DRIVER_GLOBALS G,WDFOBJECT Object) {
  /* WDF forbids deleting an interrupt; common buffer must be gone first. */
  CHECK((Object==(WDFOBJECT)4 && Deleted==0) || (Object==(WDFOBJECT)5 && (Deleted==0 || Deleted==1)));
  Deleted|=Object==(WDFOBJECT)4?1:2;
}
static VOID IrqAcquire(PWDF_DRIVER_GLOBALS G,WDFINTERRUPT I) { CHECK(InterruptObjectAlive && I==(WDFINTERRUPT)7 && !IrqDepth); IrqDepth++; }
static VOID IrqRelease(PWDF_DRIVER_GLOBALS G,WDFINTERRUPT I) { CHECK(InterruptObjectAlive && I==(WDFINTERRUPT)7 && IrqDepth==1); IrqDepth--; }
static WDFDEVICE InterruptDevice(PWDF_DRIVER_GLOBALS G,WDFINTERRUPT I) { CHECK(InterruptObjectAlive && I==(WDFINTERRUPT)7); return (WDFDEVICE)1; }
static WDFIOTARGET IoTarget(PWDF_DRIVER_GLOBALS G,WDFDEVICE Device) { CHECK(Device==(WDFDEVICE)1); return (WDFIOTARGET)8; }
static NTSTATUS Ioctl(PWDF_DRIVER_GLOBALS G,WDFIOTARGET Target,WDFREQUEST Request,ULONG Code,
    PWDF_MEMORY_DESCRIPTOR In,PWDF_MEMORY_DESCRIPTOR Out,PWDF_REQUEST_SEND_OPTIONS Options,ULONG_PTR *Returned) {
  PACPI_EVAL_INPUT_BUFFER Input=In->u.BufferType.Buffer;
  PACPI_EVAL_OUTPUT_BUFFER Output=Out->u.BufferType.Buffer; ULONG Value,J; ULONGLONG WideValue;
  AcpiCalls++; CHECK(Target==(WDFIOTARGET)8 && Code==IOCTL_ACPI_EVAL_METHOD && Options->Timeout==WDF_REL_TIMEOUT_IN_SEC(2));
  CHECK(In->Type==WdfMemoryDescriptorTypeBuffer && Out->Type==WdfMemoryDescriptorTypeBuffer && Input->Signature==ACPI_EVAL_INPUT_BUFFER_SIGNATURE);
  if (!NT_SUCCESS(AcpiStatus)) return AcpiStatus;
  CHECK(Input->MethodName[0]=='I');
  if (Input->MethodName[1]=='R' && Input->MethodName[2]=='D') {
    CHECK(Input->MethodName[3]=='Y'); Value=AcpiVariant==1?0:1;
  } else if (Input->MethodName[2]=='C') {
    CHECK(Input->MethodName[1]=='R' && Input->MethodName[3]=='K');
    LiveClockCalls++; Value=LiveClock;
  } else {
    CHECK(Input->MethodName[1]=='R' && Input->MethodName[2]=='R' && Input->MethodName[3]=='S');
    CHECK(!LiveClock && !D.Active && !D.Pinned && !D.Core.Ready);
    CHECK(!D.Core.Io.Read || D.Core.DmaSafe);
    ResumeCalls++;
    if (!NT_SUCCESS(ResumeStatus)) return ResumeStatus;
    Value=ResumeClock;
  }
  Output->Signature=ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE; Output->Count=1; Output->Length=sizeof(*Output);
  Output->Argument[0].Type=ACPI_METHOD_ARGUMENT_INTEGER; Output->Argument[0].DataLength=sizeof(ULONG); Output->Argument[0].Argument=Value;
  if (AcpiVariant==9) {
    WideValue=Value;
    Output->Argument[0].DataLength=sizeof(WideValue);
    for (J=0;J<sizeof(WideValue);J++) Output->Argument[0].Data[J]=((UCHAR *)&WideValue)[J];
    Output->Length=(ULONG)(FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+sizeof(WideValue));
  }
  *Returned=Output->Length;
  switch (AcpiFaultMethod && Input->MethodName[2]!='C'?0:AcpiVariant) {
  case 2: Output->Signature=0; break;
  case 3: Output->Count=2; break;
  case 4: Output->Length=1; break;
  case 5: *Returned=1; break;
  case 6: Output->Argument[0].DataLength=8; break;
  case 7: Output->Argument[0].Type=ACPI_METHOD_ARGUMENT_BUFFER; break;
  case 8: *Returned=Out->u.BufferType.Length+1; break;
  }
  return STATUS_SUCCESS;
}
static VOID ConnectionParameters(PSPB_DRIVER_GLOBALS G,SPBTARGET Target,SPB_CONNECTION_PARAMETERS *P) {
  CHECK(Target==(SPBTARGET)2); P->ConnectionParameters=Connection;
}
static void ConnectionInit(void) {
  I2C_DESCRIPTOR *I; const char Source[]="\\_SB.I2T0";
  memset(ConnectionStorage,0,sizeof(ConnectionStorage));
  Connection->Version=RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_VERSION;
  Connection->PropertiesLength=sizeof(*I)+sizeof(Source);
  I=(I2C_DESCRIPTOR *)Connection->ConnectionProperties;
  I->Bus.Tag=SERIAL_BUS_DESCRIPTOR; I->Bus.Length=(USHORT)(Connection->PropertiesLength-3);
  I->Bus.RevisionId=SERIAL_BUS_DESCRIPTOR_REVISION_V2; I->Bus.SerialBusType=SERIAL_BUS_I2C_DESCRIPTOR_TYPE;
  I->Bus.GeneralFlags=2; I->Bus.TypeSpecificRevisionId=SERIAL_BUS_DESCRIPTOR_REVISION; I->Bus.TypeDataLength=6;
  I->Address=0x48; I->Speed=400000; memcpy(I+1,Source,sizeof(Source));
  T.Address=0x55; T.Speed=100000;
}
static void HardwareInit(unsigned Instance) {
  unsigned I; Init(); InterruptObjectAlive=FALSE; D.Active=FALSE; D.IrqOnline=0; D.Buffer=NULL; D.Enabler=NULL;
  D.Core.Ready=D.Core.DmaSafe=0; memset(Registers,0,sizeof(Registers)); memset(DmaRegisters,0,sizeof(DmaRegisters));
  memset(RawResources,0,sizeof(RawResources)); RawCount=TranslatedCount=3;
  Maps=Unmaps=MapFailure=Deleted=IrqDepth=IrqCreates=AcpiVariant=AcpiCalls=InitializeCalls=StopCalls=0;
  LiveClock=136000000; LiveClockCalls=AcpiFaultMethod=0;
  ResumeClock=ResumeCalls=0; ResumeStatus=STATUS_SUCCESS;
  NullDescriptor=0; EnablerStatus=BufferStatus=IrqStatus=AcpiStatus=STATUS_SUCCESS; InitializeResult=StopResult=MiOk;
  const ULONGLONG Controller[]={0x11e00000,0x1101a000,0x11e01000};
  const ULONGLONG Dma[]={0x10217080,0x10217480,0x10217100};
  const ULONG Irq[]={176,182,177};
  CHECK(Instance<RTL_NUMBER_OF(Controller));
  for (I=0;I<2;I++) {
    RawResources[I].Type=CmResourceTypeMemory;
    RawResources[I].u.Memory.Start.QuadPart=I?Dma[Instance]:Controller[Instance];
    RawResources[I].u.Memory.Length=I?0x80:0x1000;
  }
  RawResources[2].Type=CmResourceTypeInterrupt; RawResources[2].ShareDisposition=CmResourceShareDeviceExclusive;
  RawResources[2].u.Interrupt.Vector=Irq[Instance];
  memcpy(TranslatedResources,RawResources,sizeof(RawResources));
  TranslatedResources[2].u.Interrupt.Vector=0x1234; TranslatedResources[2].u.Interrupt.Level=0x55;
}
#define PREPARE() PrepareHardware((WDFDEVICE)1,(WDFCMRESLIST)10,(WDFCMRESLIST)11)
static NTSTATUS FrameworkRelease(void) {
  NTSTATUS S=ReleaseHardware((WDFDEVICE)1,(WDFCMRESLIST)11);
  CHECK(!D.Interrupt && !IrqDepth);
  InterruptObjectAlive=FALSE;
  return S;
}
#define RELEASE() FrameworkRelease()
static void Start(void) {
  CHECK(PREPARE()==STATUS_SUCCESS); CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
  IrqDepth++; CHECK(InterruptEnable((WDFINTERRUPT)7,(WDFDEVICE)1)==STATUS_SUCCESS); IrqDepth--;
}
static BOOLEAN Fire(void) { BOOLEAN Claimed; IrqDepth++; Claimed=InterruptIsr((WDFINTERRUPT)7,0); IrqDepth--; return Claimed; }
#define CALL(N) Transfer((WDFDEVICE)1,(SPBTARGET)2,(SPBREQUEST)3,(N))
int TestMain(void) {
  unsigned I;
  UCHAR IntegerStorage[sizeof(ACPI_METHOD_ARGUMENT)+sizeof(ULONGLONG)]={0};
  PACPI_METHOD_ARGUMENT Integer=(PACPI_METHOD_ARGUMENT)IntegerStorage;
  ULONG Parsed,Small=0x89abcdef; ULONGLONG Wide;
#define WF(N,F) Table[N##TableIndex]=(WDFFUNC)(F)
#define SF(N,F) SpbFunctions[N##TableIndex]=(SPBFUNC)(F)
  WF(WdfObjectGetTypedContextWorker,Context); WF(WdfWaitLockAcquire,Acquire); WF(WdfWaitLockRelease,Unlock);
  WF(WdfCommonBufferGetAlignedVirtualAddress,Virtual); WF(WdfCommonBufferGetAlignedLogicalAddress,Logical);
  WF(WdfRequestSetInformation,SetInfo); WF(WdfRequestMarkCancelableEx,Mark); WF(WdfRequestUnmarkCancelable,Unmark);
  WF(WdfObjectReferenceActual,Reference); WF(WdfCmResourceListGetCount,ResourceCount); WF(WdfCmResourceListGetDescriptor,Resource);
  WF(WdfDmaEnablerCreate,CreateEnabler); WF(WdfCommonBufferCreate,CreateBuffer); WF(WdfInterruptCreate,CreateInterrupt);
  WF(WdfObjectDelete,Delete); WF(WdfInterruptAcquireLock,IrqAcquire); WF(WdfInterruptReleaseLock,IrqRelease);
  WF(WdfInterruptGetDevice,InterruptDevice); WF(WdfDeviceGetIoTarget,IoTarget); WF(WdfIoTargetSendIoctlSynchronously,Ioctl);
  SF(SpbTargetGetConnectionParameters,ConnectionParameters);
  SF(SpbRequestGetParameters,Parameters); SF(SpbRequestGetTransferParameters,Descriptor); SF(SpbRequestComplete,Complete);
  Integer->Type=ACPI_METHOD_ARGUMENT_INTEGER;Integer->DataLength=sizeof(Small);for (ULONG J=0;J<sizeof(Small);++J) Integer->Data[J]=((const UCHAR *)&Small)[J];
  CHECK(FirmwareInteger32(Integer,&Parsed)&&Parsed==Small);
  Wide=MAXULONG;Integer->DataLength=sizeof(Wide);for (ULONG J=0;J<sizeof(Wide);++J) Integer->Data[J]=((const UCHAR *)&Wide)[J];
  CHECK(FirmwareInteger32(Integer,&Parsed)&&Parsed==MAXULONG);
  Wide=(ULONGLONG)MAXULONG+1;for (ULONG J=0;J<sizeof(Wide);++J) Integer->Data[J]=((const UCHAR *)&Wide)[J];
  CHECK(!FirmwareInteger32(Integer,&Parsed));
  Integer->DataLength=6;CHECK(!FirmwareInteger32(Integer,&Parsed));
  Integer->Type=ACPI_METHOD_ARGUMENT_BUFFER;Integer->DataLength=sizeof(Small);
  CHECK(!FirmwareInteger32(Integer,&Parsed));
  Init(); CALL(2); CHECK(Result==STATUS_SUCCESS&&Information==5&&Transfers==1&&Completions==1);
  CHECK(Rx[0]==0xa1&&Rx[1]==0xb2&&Rx[2]==0xc3&&!D.ActiveRequest);
  Init(); RxMdl.ByteCount=1; RxMdl.Next=&RxTail;
  RxTail.MdlFlags=MDL_MAPPED_TO_SYSTEM_VA; RxTail.MappedSystemVa=Rx+1; RxTail.ByteCount=2;
  CALL(2); CHECK(Result==STATUS_SUCCESS&&Rx[2]==0xc3&&Information==5);
  Init(); RxMdl.ByteCount=2; CALL(2); CHECK(Result==STATUS_INVALID_BUFFER_SIZE&&!Transfers&&!Information);
  Init(); RxMdl.MdlFlags=0; CALL(2); CHECK(Result==STATUS_INSUFFICIENT_RESOURCES&&!Transfers&&!Information);
  Init(); TxMdl.ByteCount=1; CALL(2); CHECK(Result==STATUS_INVALID_BUFFER_SIZE&&!Transfers&&!Information);
  Init(); Descriptors[1].DelayInUs=1; CALL(2); CHECK(Result==STATUS_SUCCESS&&SequenceCalls==1&&Information==5);
  Init(); Descriptors[0].Direction=SpbTransferDirectionFromDevice; CALL(2); CHECK(Result==STATUS_SUCCESS&&SequenceCalls==1&&Information==5);
  Init(); Descriptors[0].TransferLength=65536; CALL(2); CHECK(Result==STATUS_NOT_SUPPORTED&&!Transfers);
  Init(); Params.Type=SpbRequestTypeRead; Params.Position=SpbRequestSequencePositionFirst;
  CALL(1); CHECK(Result==STATUS_NOT_SUPPORTED&&!Transfers);
  Init(); CALL(3); CHECK(Result==STATUS_NOT_SUPPORTED&&!Transfers);
  Init(); D.Active=FALSE; CALL(2); CHECK(Result==STATUS_DEVICE_NOT_READY&&!Transfers&&!Information);
  Init(); MarkResult=STATUS_CANCELLED; CALL(2); CHECK(Result==STATUS_CANCELLED&&!Transfers&&!Information&&!R.Wake);
  Init(); CancelDuringTransfer=1; CALL(2); CHECK(Result==STATUS_CANCELLED&&Transfers==1&&Completions==1&&!Information&&Rx[0]==0);
  Init(); CancelOnUnmark=1; CALL(2); CHECK(Result==STATUS_CANCELLED&&Transfers==1&&Completions==1&&!Information&&Rx[0]==0);
  Init(); EngineResult=MiNack; CALL(2); CHECK(Result==STATUS_NO_SUCH_DEVICE&&Transfers==1&&!Information&&Rx[0]==0);
  Init(); EngineResult=MiDmaStuck; CALL(2);
  CHECK(Result==STATUS_IO_DEVICE_ERROR&&!D.Active&&D.Pinned&&References==2&&!Information);
  CALL(2); CHECK(Result==STATUS_DEVICE_NOT_READY&&Transfers==1&&References==2&&Completions==2);
  Init(); CancelOnMark=1; CALL(2);
  CHECK(Result==STATUS_CANCELLED && !Information && !Rx[0] && !R.Wake && CancelSignals==2);
  Init(); D.Registers=Registers; D.DmaRegisters=DmaRegisters; D.Interrupt=(WDFINTERRUPT)7;
  UseWait=2; CALL(2); CHECK(Result==STATUS_SUCCESS && Waits==1 && WakeSignals==1 && !*(ULONG *)(Registers+8));
  Init(); UseWait=1; CALL(2); CHECK(Result==STATUS_SUCCESS && Waits==1 && !WakeSignals && !R.Wake);
  Init(); UseWait=WaitCancel=1; CALL(2);
  CHECK(Result==STATUS_CANCELLED && Waits==1 && WakeSignals==2 && CancelSignals==2 && !Information && !R.Wake);
  /* The mock Unmark invokes the callback again: exercise idempotent wake-up. */
  Init(); KeSetEvent(&D.TransferEvent,0,FALSE); CALL(2); CHECK(!D.TransferEvent.Header.SignalState);
  for (I=0;I<2;I++) {
    HardwareInit(I); CHECK(PREPARE()==STATUS_SUCCESS && Maps==2 && IrqCreates==1 && !D.Core.Io.Read);
    CHECK(PREPARE()==STATUS_INVALID_DEVICE_STATE && Maps==2);
    CHECK(RELEASE()==STATUS_SUCCESS && Unmaps==2 && Deleted==3 && !D.Interrupt && !InterruptObjectAlive);
    CHECK(!D.Registers && !D.DmaRegisters && !D.Buffer && !D.Enabler);
    /* WDF revokes the old object even when the same device receives resources again. */
    Maps=Deleted=0;
    TranslatedResources[2].u.Interrupt.Vector++;
    CHECK(PREPARE()==STATUS_SUCCESS && Maps==2 && IrqCreates==2 && InterruptObjectAlive);
    CHECK(RELEASE()==STATUS_SUCCESS && Unmaps==4 && Deleted==3 && !D.Interrupt);
    CHECK(RELEASE()==STATUS_SUCCESS && Unmaps==4 && Deleted==3 && !D.Interrupt);
  }
  for (I=0;I<13;I++) {
    HardwareInit(0);
    switch (I) {
    case 0: RawCount--; break;
    case 1: TranslatedCount++; break;
    case 2: RawResources[0].u.Memory.Start.QuadPart++; break;
    case 3: TranslatedResources[0].u.Memory.Start.QuadPart++; break;
    case 4: RawResources[1].u.Memory.Start.QuadPart=0x10217480; break;
    case 5: TranslatedResources[1].u.Memory.Length--; break;
    case 6: RawResources[2].u.Interrupt.Vector++; break;
    case 7: TranslatedResources[2].Flags=CM_RESOURCE_INTERRUPT_LATCHED; break;
    case 8: RawResources[2].Flags=CM_RESOURCE_INTERRUPT_MESSAGE; break;
    case 9: TranslatedResources[2].ShareDisposition=CmResourceShareShared; break;
    case 10: RawResources[1].Flags=CM_RESOURCE_MEMORY_READ_ONLY; break;
    case 11: TranslatedResources[2].Type=CmResourceTypeConnection; break;
    case 12: NullDescriptor=1; break;
    }
    CHECK(PREPARE()==STATUS_DEVICE_CONFIGURATION_ERROR && !Maps && !IrqCreates);
  }
  HardwareInit(0); CHECK(PrepareHardware((WDFDEVICE)1,NULL,(WDFCMRESLIST)11)==STATUS_DEVICE_CONFIGURATION_ERROR && !Maps);
  for (I=1;I<=2;I++) {
    HardwareInit(0); MapFailure=I; CHECK(PREPARE()==STATUS_INSUFFICIENT_RESOURCES && Maps==I && Unmaps==I-1 && !Deleted);
    CHECK(!D.Registers && !D.DmaRegisters && !D.Enabler && !D.Buffer);
  }
  for (I=0;I<6;I++) {
    HardwareInit(0);
    switch (I) {
    case 0: EnablerStatus=STATUS_INSUFFICIENT_RESOURCES; break;
    case 1: BufferStatus=STATUS_INSUFFICIENT_RESOURCES; break;
    case 2: DmaAddress=0; break;
    case 3: DmaAddress=(1ULL<<36)-HALF; break;
    case 4: DmaAddress=0xffffffffffffffffULL; break;
    case 5: IrqStatus=STATUS_INSUFFICIENT_RESOURCES; break;
    }
    CHECK(!NT_SUCCESS(PREPARE()) && Maps==2 && Unmaps==2);
    CHECK(!D.Registers && !D.DmaRegisters && !D.Enabler && !D.Buffer && !D.Interrupt);
    CHECK(Deleted==(I==0?0:I==1?2:3));
  }
  for (I=1;I<=8;I++) {
    HardwareInit(0); CHECK(PREPARE()==STATUS_SUCCESS); AcpiVariant=I;
    CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_DEVICE_NOT_READY && !D.Active && !InitializeCalls);
    CHECK(RELEASE()==STATUS_SUCCESS && Deleted==3 && !D.Pinned);
  }
  for (I=2;I<=8;I++) {
    HardwareInit(0); CHECK(PREPARE()==STATUS_SUCCESS); AcpiVariant=I; AcpiFaultMethod=1;
    CHECK(!NT_SUCCESS(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3Final)) && !D.Active && !InitializeCalls && LiveClockCalls==1);
    CHECK(RELEASE()==STATUS_SUCCESS && Deleted==3 && !D.Pinned);
  }
  HardwareInit(0); CHECK(PREPARE()==STATUS_SUCCESS); AcpiVariant=9;
  CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_SUCCESS && D.Active && InitializeCalls==1);
  CHECK(D0Exit((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_SUCCESS && RELEASE()==STATUS_SUCCESS);
  HardwareInit(0); CHECK(PREPARE()==STATUS_SUCCESS); AcpiStatus=STATUS_IO_TIMEOUT;
  CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_DEVICE_NOT_READY && !InitializeCalls); RELEASE();
  HardwareInit(0); CHECK(PREPARE()==STATUS_SUCCESS);
  IrqDepth++; CHECK(InterruptEnable((WDFINTERRUPT)7,(WDFDEVICE)1)==STATUS_DEVICE_NOT_READY); IrqDepth--;
  CHECK(!D.IrqOnline); RELEASE();
  Init(); Params.SequenceTransferCount=3;
  Descriptors[2]=Descriptors[0]; Mdls[2]=&TxMdl;
  CALL(3); CHECK(Result==STATUS_SUCCESS && SequenceCalls==1 && Information==7 && Rx[0]==0xa1);
  Init(); Params.SequenceTransferCount=3; Descriptors[2]=Descriptors[0]; Mdls[2]=&TxMdl;
  EngineResult=MiNack; SequenceDone=2;
  CALL(3); CHECK(Result==STATUS_SUCCESS && Information==5 && SequenceCalls==1 && Rx[0]==0xa1);
  Init(); Params.SequenceTransferCount=3; Descriptors[2]=Descriptors[0]; Mdls[2]=&TxMdl;
  EngineResult=MiNack; SequenceDone=0;
  CALL(3); CHECK(Result==STATUS_NO_SUCH_DEVICE && Information==0 && SequenceCalls==1);
  {
    const MI_STATUS Engines[]={MiArbitration,MiTimeout,MiBusError};
    const NTSTATUS Expected[]={STATUS_DEVICE_BUSY,STATUS_IO_TIMEOUT,STATUS_IO_DEVICE_ERROR};
    for (I=0;I<RTL_NUMBER_OF(Engines);I++) {
      Init(); Params.SequenceTransferCount=3; Descriptors[2]=Descriptors[0]; Mdls[2]=&TxMdl;
      EngineResult=Engines[I]; SequenceDone=1; CALL(3);
      CHECK(Result==Expected[I] && Information==2 && SequenceCalls==1);
    }
  }
  Init(); Params.SequenceTransferCount=3; Descriptors[2]=Descriptors[0]; Mdls[2]=NULL;
  CALL(3); CHECK(Result==STATUS_INVALID_BUFFER_SIZE && !Transfers && !Information);
  Init(); Params.SequenceTransferCount=3; Descriptors[2]=Descriptors[0]; Mdls[2]=&TxMdl;
  CancelDuringTransfer=1; CALL(3);
  CHECK(Result==STATUS_CANCELLED && SequenceCalls==1 && !Information && !Rx[0]);
  Init(); Params.SequenceTransferCount=MI_MAX_MESSAGES;
  for (I=0;I<MI_MAX_MESSAGES;I++) {
    SPB_TRANSFER_DESCRIPTOR_INIT(&Descriptors[I]); Descriptors[I].TransferLength=1;
    Descriptors[I].Direction=I&1?SpbTransferDirectionFromDevice:SpbTransferDirectionToDevice;
    Mdls[I]=I&1?&RxMdl:&TxMdl;
  }
  CALL(MI_MAX_MESSAGES); CHECK(Result==STATUS_SUCCESS && SequenceCalls==1 && Information==MI_MAX_MESSAGES);
  Init(); Params.SequenceTransferCount=MI_MAX_MESSAGES+1; CALL(MI_MAX_MESSAGES+1);
  CHECK(Result==STATUS_NOT_SUPPORTED && !Transfers && !Information);
  Init(); Descriptors[1].DelayInUs=1000001; CALL(2); CHECK(Result==STATUS_NOT_SUPPORTED && !Transfers);
  Init(); Params.Type=SpbRequestTypeRead; EngineResult=MiNack;
  Descriptors[0].Direction=SpbTransferDirectionFromDevice; Descriptors[0].TransferLength=3; Mdls[0]=&RxMdl;
  /* The simple-request stub has the combined transfer fixture; type semantics
   * are covered using one invalid request before any mocked wire activity. */
  Params.Position=SpbRequestSequencePositionFirst; CALL(1);
  CHECK(Result==STATUS_NOT_SUPPORTED && !Transfers);
  HardwareInit(1); Start(); CHECK(D.Active && D.IrqOnline && InitializeCalls==1 && AcpiCalls==2);
  CHECK(!Fire() && !D.TransferEvent.Header.SignalState);
  *(ULONG *)(Registers+8)=15; *(ULONG *)(Registers+12)=0x10;
  CHECK(!Fire() && *(ULONG *)(Registers+8)==15);
  *(ULONG *)(Registers+8)=31;
  CHECK(Fire() && !*(ULONG *)(Registers+8) && *(ULONG *)(Registers+12)==16);
  KeClearEvent(&D.TransferEvent); *(ULONG *)(Registers+8)=15;
  *(ULONG *)(Registers+12)=3; CHECK(Fire() && *(ULONG *)(Registers+8)==0 && *(ULONG *)(Registers+12)==3);
  CHECK(D.TransferEvent.Header.SignalState==1 && !Fire());
  WaitForEvent(&D,0); CHECK(!Waits);
  WaitForEvent(&D,100); CHECK(Waits==1 && !D.TransferEvent.Header.SignalState);
  WriteRegister(&D,0,12,4,3); CHECK(!ReadRegister(&D,0,12,4));
  CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_INVALID_DEVICE_STATE && InitializeCalls==1);
  CHECK(LiveClockCalls==1);
  CHECK(D0Exit((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_SUCCESS && !D.Active && StopCalls==1);
  IrqDepth++; CHECK(InterruptDisable((WDFINTERRUPT)7,(WDFDEVICE)1)==STATUS_SUCCESS); IrqDepth--;
  CHECK(!D.IrqOnline && !Fire()); WriteRegister(&D,0,8,4,15); CHECK(!*(ULONG *)(Registers+8));
  CHECK(RELEASE()==STATUS_SUCCESS && Deleted==3 && Unmaps==2 && !D.Core.Io.Read && !References);
  Maps=Deleted=0;
  CHECK(PREPARE()==STATUS_SUCCESS && Maps==2 && IrqCreates==2);
  CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_SUCCESS && InitializeCalls==2 && LiveClockCalls==2);
  CHECK(D0Exit((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_SUCCESS && StopCalls==2);
  CHECK(RELEASE()==STATUS_SUCCESS && Deleted==3 && Unmaps==4 && !D.Interrupt);
  for (I=0;I<2;I++) {
    unsigned J;
    HardwareInit(I); Start();
    for (J=0;J<8;J++) {
      IrqDepth++; CHECK(InterruptDisable((WDFINTERRUPT)7,(WDFDEVICE)1)==STATUS_SUCCESS); IrqDepth--;
      CHECK(D0Exit((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS && !D.Active && D.Core.DmaSafe);
      /* A denied live check never initializes or writes the controller. */
      AcpiVariant=1;
      CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY && InitializeCalls==J+1);
      AcpiVariant=0; LiveClock=J&1?26000000:124800000;
      CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS && D.Active && InitializeCalls==J+2);
      CHECK(D.Core.SourceHz==LiveClock);
      IrqDepth++; CHECK(InterruptEnable((WDFINTERRUPT)7,(WDFDEVICE)1)==STATUS_SUCCESS); IrqDepth--;
      CALL(2); CHECK(Result==STATUS_SUCCESS && Transfers==J+1 && Information==5);
    }
    CHECK(LiveClockCalls==9 && AcpiCalls==26);
    IrqDepth++; InterruptDisable((WDFINTERRUPT)7,(WDFDEVICE)1); IrqDepth--;
    CHECK(D0Exit((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    CHECK(RELEASE()==STATUS_SUCCESS && !D.Pinned && Deleted==3);
  }
  for (I=0;I<4;I++) {
    HardwareInit(0); CHECK(PREPARE()==STATUS_SUCCESS);
    LiveClock=I==0?0:I==1?999999:I==2?1000000001:0xffffffffU;
    CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_DEVICE_NOT_READY && !InitializeCalls && !D.Active);
    CHECK(ResumeCalls==(I==0) && RELEASE()==STATUS_SUCCESS && Deleted==3);
  }
  for (I=0;I<3;I++) {
    HardwareInit(I);
    LiveClock=0; ResumeClock=26000000;
    Start();
    CHECK(ResumeCalls==1 && D.Active && InitializeCalls==1 && D.Core.SourceHz==26000000);
    IrqDepth++; CHECK(InterruptDisable((WDFINTERRUPT)7,(WDFDEVICE)1)==STATUS_SUCCESS); IrqDepth--;
    CHECK(D0Exit((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS);
    ResumeCalls=ResumeClock=0;
    ResumeStatus=STATUS_OBJECT_NAME_NOT_FOUND;
    CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY && ResumeCalls==1);
    ResumeStatus=STATUS_IO_TIMEOUT;
    CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_IO_TIMEOUT && ResumeCalls==2);
    ResumeStatus=STATUS_SUCCESS;
    CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY && ResumeCalls==3);
    ResumeClock=1000000001;
    CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY && ResumeCalls==4);
    CHECK(!D.Active && InitializeCalls==1);
    ResumeClock=26000000;
    CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS && ResumeCalls==5);
    CHECK(D.Active && InitializeCalls==2 && D.Core.SourceHz==26000000);
    IrqDepth++; CHECK(InterruptEnable((WDFINTERRUPT)7,(WDFDEVICE)1)==STATUS_SUCCESS); IrqDepth--;
    CALL(2); CHECK(Result==STATUS_SUCCESS && Transfers==1 && Information==5);
    IrqDepth++; CHECK(InterruptDisable((WDFINTERRUPT)7,(WDFDEVICE)1)==STATUS_SUCCESS); IrqDepth--;
    CHECK(D0Exit((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_SUCCESS && RELEASE()==STATUS_SUCCESS);
    Maps=Deleted=0;
    CHECK(PREPARE()==STATUS_SUCCESS);
    CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_SUCCESS && ResumeCalls==6);
    CHECK(D.Active && D.Core.SourceHz==26000000 && InitializeCalls==3);
    IrqDepth++; CHECK(InterruptEnable((WDFINTERRUPT)7,(WDFDEVICE)1)==STATUS_SUCCESS); IrqDepth--;
    CALL(2); CHECK(Result==STATUS_SUCCESS && Transfers==2 && Information==5);
    IrqDepth++; CHECK(InterruptDisable((WDFINTERRUPT)7,(WDFDEVICE)1)==STATUS_SUCCESS); IrqDepth--;
    CHECK(D0Exit((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    CHECK(RELEASE()==STATUS_SUCCESS && !D.Pinned);
  }
  HardwareInit(0); Start();
  /* ReleaseHardware is outside D0: no register access, even on bad ordering. */
  CHECK(RELEASE()==STATUS_SUCCESS && !StopCalls && D.Pinned && References==2 && !Deleted && Unmaps==2);
  CHECK(PREPARE()==STATUS_DEVICE_NOT_READY && Maps==2 && IrqCreates==1);

  HardwareInit(0); CHECK(PREPARE()==STATUS_SUCCESS); InitializeResult=MiDmaStuck;
  CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_IO_DEVICE_ERROR && D.Pinned && References==2);
  {
    unsigned Calls=AcpiCalls;
    CHECK(D0Entry((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_DEVICE_NOT_READY && AcpiCalls==Calls && InitializeCalls==1);
  }
  CHECK(RELEASE()==STATUS_SUCCESS && !Deleted && Unmaps==2 && D.Buffer && D.Enabler && References==2);
  HardwareInit(0); Start(); StopResult=MiDmaStuck;
  CHECK(D0Exit((WDFDEVICE)1,WdfPowerDeviceD3Final)==STATUS_IO_DEVICE_ERROR && D.Pinned && References==2);
  CHECK(RELEASE()==STATUS_SUCCESS && !Deleted && Unmaps==2 && References==2);
  for (I=0;I<17;I++) {
    I2C_DESCRIPTOR *C; ConnectionInit(); C=(I2C_DESCRIPTOR *)Connection->ConnectionProperties;
    switch(I) {
    case 1: Connection->PropertiesLength=sizeof(*C)-1; break;
    case 2: Connection->Version++; break;
    case 3: C->Bus.Tag=0; break;
    case 4: C->Bus.RevisionId=1; break;
    case 5: C->Bus.GeneralFlags=6; break; /* shared bus */
    case 6: C->Bus.TypeSpecificFlags=1; break; /* 10-bit address */
    case 7: C->Bus.TypeDataLength=7; break;
    case 8: C->Bus.Length=0xffff; break;
    case 9: C->Bus.Length=sizeof(*C)-3; break;
    case 10: Connection->ConnectionProperties[Connection->PropertiesLength-1]=1; break;
    case 11: ((UCHAR *)(C+1))[0]=0; break;
    case 12: ((UCHAR *)(C+1))[2]=0; break;
    case 13: C->Address=0x78; break;
    case 14: C->Speed=1000000; break;
    case 15: C->Bus.SerialBusType=2; break;
    case 16: C->Bus.TypeSpecificRevisionId=2; break;
    }
    NTSTATUS S=TargetConnect((WDFDEVICE)1,(SPBTARGET)2);
    if (!I) CHECK(S==STATUS_SUCCESS && T.Address==0x48 && T.Speed==400000);
    else CHECK(!NT_SUCCESS(S) && T.Address==0x55 && T.Speed==100000);
  }
  enum { Tcpc, Charger, WrongController, Unprepared, Released, WrongPath,
         WrongAddress, WrongTcpcRate, WrongChargerRate, PeripheralInitiated, WrongPathLength, PaddedPath };
  for (I=Tcpc;I<=PaddedPath;I++) {
    const char Path[]="\\_SB.I2C6";
    const char PaddedPathName[]="\\_SB_.I2C6";
    I2C_DESCRIPTOR *C;
    NTSTATUS S;
    HardwareInit(I==WrongController?0:1);
    if (I!=Unprepared) CHECK(PREPARE()==STATUS_SUCCESS);
    if (I==Released) CHECK(RELEASE()==STATUS_SUCCESS);
    ConnectionInit();
    C=(I2C_DESCRIPTOR *)Connection->ConnectionProperties;
    memcpy(C+1,Path,sizeof(Path));
    C->Bus.GeneralFlags=6;
    if (I==PaddedPath) {
      memcpy(C+1,PaddedPathName,sizeof(PaddedPathName));
      Connection->PropertiesLength+=sizeof(PaddedPathName)-sizeof(Path);
      C->Bus.Length=(USHORT)(Connection->PropertiesLength-3);
    }
    C->Address=I==Charger || I==WrongChargerRate?0x6a:0x4e;
    C->Speed=I==Charger || I==WrongTcpcRate?100000:400000;
    switch(I) {
    case WrongPath: ((UCHAR *)(C+1))[8]='0'; break;
    case WrongAddress: C->Address=0x48; break;
    case PeripheralInitiated: C->Bus.GeneralFlags|=1; break;
    case WrongPathLength:
      ((UCHAR *)(C+1))[sizeof(Path)-1]='A';
      Connection->PropertiesLength++;
      C->Bus.Length++;
      break;
    }
    S=TargetConnect((WDFDEVICE)1,(SPBTARGET)2);
    if (I==Tcpc || I==Charger || I==PaddedPath) CHECK(S==STATUS_SUCCESS && T.Address==C->Address && T.Speed==C->Speed);
    else CHECK(S==STATUS_NOT_SUPPORTED && T.Address==0x55 && T.Speed==100000);
    if (I!=Unprepared && I!=Released) CHECK(RELEASE()==STATUS_SUCCESS);
  }
  CHECK(sizeof(ULONG)==4&&sizeof(void *)==8&&Checks>=400);
  return Failure;
}
