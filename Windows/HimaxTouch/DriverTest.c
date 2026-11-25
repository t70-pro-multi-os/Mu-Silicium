/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Execute production Himax KMDF callbacks as ARM64 PE code under mocks.
 * This is not a Windows kernel or tablet test.
 */
#include <ntddk.h>
VOID HmTestInitializeSpinLock(PKSPIN_LOCK Lock);
VOID HmTestAcquireSpinLock(PKSPIN_LOCK Lock,PKIRQL OldIrql);
VOID HmTestReleaseSpinLock(PKSPIN_LOCK Lock,KIRQL OldIrql);
#undef KeAcquireSpinLock
#undef KeReleaseSpinLock
#define KeInitializeSpinLock(Lock) HmTestInitializeSpinLock(Lock)
#define KeAcquireSpinLock(Lock,OldIrql) HmTestAcquireSpinLock((Lock),(OldIrql))
#define KeReleaseSpinLock(Lock,OldIrql) HmTestReleaseSpinLock((Lock),(OldIrql))
NTSTATUS HmTestDelayExecutionThread(KPROCESSOR_MODE Mode,BOOLEAN Alertable,PLARGE_INTEGER Interval);
#define KeDelayExecutionThread HmTestDelayExecutionThread
VOID HmTestInitUnicodeString(PUNICODE_STRING Name,PCWSTR Text);
#define RtlInitUnicodeString HmTestInitUnicodeString
#include "Driver.c"
#undef RtlInitUnicodeString
#undef KeDelayExecutionThread
typedef SPB_TRANSFER_LIST_AND_ENTRIES(2) HM_SPB_SEQUENCE;

static unsigned Failure, IoctlCalls, SequenceCalls, DisableCalls, ZeroByteWriteFailures;
__declspec(dllexport) unsigned HimaxTouchAssertionCount;
#define Checks HimaxTouchAssertionCount
static BOOLEAN InterruptObjectAlive, ReleaseInProgress;
static unsigned FlushCalls, EnqueueCalls, SubmitCalls, AcceptedReports, DelayCalls;
static unsigned SyncReady, PacketValid, PacketX, PacketY, PacketPressure, AcpiReady, QueueOnSubmit;
static unsigned SelectedAddress, EventReads, SenseOnWrites, SystemResetWrites;
static ULONG AcpiResumeFlags, AcpiResumeCalls, LastAcpiSequence;
static BOOLEAN AcpiResumeEchoValid,AcpiWideInteger;
static BOOLEAN ChipIdValid,InterfaceValid;
static UCHAR FirmwareState;
static unsigned TargetCloses,TargetDeletes,InterruptCreates;
static NTSTATUS TargetCreateStatus,TargetOpenStatus,InterruptCreateStatus;
static CM_PARTIAL_RESOURCE_DESCRIPTOR RawResources[2],TranslatedResources[2];
static ULONG RawCount=2,TranslatedCount=2;
static BOOLEAN MissingResource;
static NTSTATUS SubmitStatus;
static HM_HID_REPORT Reports[8];
static HM_FRAME FrameOnSubmit;
static DEVICE_CONTEXT D;
static WDFFUNC Table[WdfFunctionTableNumEntries];
const WDFFUNC *WdfFunctions=Table;
PWDF_DRIVER_GLOBALS WdfDriverGlobals;
BOOLEAN WdfClientVersionHigherThanFramework;
ULONG WdfStructureCount;
WDF_STRUCT_INFO WdfStructures;
#pragma function(memcpy,memset)

#define CHECK(X) do { Checks++; if (!(X) && !Failure) Failure=__LINE__; } while (0)

void *memcpy(void *To,const void *From,size_t N) {
  volatile UCHAR *A=To; const volatile UCHAR *B=From;
  while (N--) *A++=*B++;
  return To;
}
void *memset(void *To,int V,size_t N) {
  volatile UCHAR *A=To; while (N--) *A++=(UCHAR)V;
  return To;
}
VOID HmTestInitializeSpinLock(PKSPIN_LOCK Lock) { *Lock=0; }
VOID HmTestAcquireSpinLock(PKSPIN_LOCK Lock,PKIRQL OldIrql) {
  *OldIrql=PASSIVE_LEVEL; CHECK(*Lock==0); *Lock=1;
}
VOID HmTestReleaseSpinLock(PKSPIN_LOCK Lock,KIRQL OldIrql) {
  UNREFERENCED_PARAMETER(OldIrql); CHECK(*Lock==1); *Lock=0;
}
NTSTATUS HmTestDelayExecutionThread(KPROCESSOR_MODE Mode,BOOLEAN Alertable,PLARGE_INTEGER Interval) {
  DelayCalls++;
  CHECK(Mode==KernelMode&&!Alertable&&Interval&&(Interval->QuadPart==-200000 || Interval->QuadPart==-100000 || Interval->QuadPart==-10000));
  return STATUS_SUCCESS;
}

VOID HmTestInitUnicodeString(PUNICODE_STRING Name,PCWSTR Text) {
  USHORT Bytes=0;while(Text[Bytes/sizeof(WCHAR)])Bytes+=sizeof(WCHAR);
  Name->Buffer=(PWSTR)Text;Name->Length=Bytes;Name->MaximumLength=Bytes+sizeof(WCHAR);
}
static NTSTATUS TargetCreate(PWDF_DRIVER_GLOBALS G,WDFDEVICE Device,PWDF_OBJECT_ATTRIBUTES Attributes,WDFIOTARGET *Target) {
  UNREFERENCED_PARAMETER(G);CHECK(Device==(WDFDEVICE)1 && Attributes->ParentObject==(WDFOBJECT)1);
  if(NT_SUCCESS(TargetCreateStatus))*Target=(WDFIOTARGET)4;return TargetCreateStatus;
}
static NTSTATUS TargetOpen(PWDF_DRIVER_GLOBALS G,WDFIOTARGET Target,PWDF_IO_TARGET_OPEN_PARAMS Params) {
  static const WCHAR Expected[]=L"\\Device\\RESOURCE_HUB\\0000567800001234";
  UNREFERENCED_PARAMETER(G);CHECK(Target==(WDFIOTARGET)4 && Params->Type==WdfIoTargetOpenByName);
  CHECK(Params->TargetDeviceName.Length==sizeof(Expected)-sizeof(WCHAR));
  if(Params->TargetDeviceName.Length==sizeof(Expected)-sizeof(WCHAR)) {
    for(unsigned I=0;I<RTL_NUMBER_OF(Expected)-1;I++)CHECK(Params->TargetDeviceName.Buffer[I]==Expected[I]);
  }
  return TargetOpenStatus;
}
static VOID TargetClose(PWDF_DRIVER_GLOBALS G,WDFIOTARGET Target) {
  UNREFERENCED_PARAMETER(G);CHECK(Target==(WDFIOTARGET)4);
  if(ReleaseInProgress) CHECK(!D.Active && !D.ReadyValidated && !D.Interrupt && FlushCalls>0);
  TargetCloses++;
}
static VOID ObjectDelete(PWDF_DRIVER_GLOBALS G,WDFOBJECT Object) {
  UNREFERENCED_PARAMETER(G);CHECK(Object==(WDFOBJECT)4);TargetDeletes++;
}
static NTSTATUS InterruptCreate(PWDF_DRIVER_GLOBALS G,WDFDEVICE Device,PWDF_INTERRUPT_CONFIG Config,PWDF_OBJECT_ATTRIBUTES Attributes,WDFINTERRUPT *Interrupt) {
  UNREFERENCED_PARAMETER(G);CHECK(Device==(WDFDEVICE)1 && Attributes->ParentObject==(WDFOBJECT)1);
  CHECK(Config->PassiveHandling && Config->EvtInterruptIsr==HmInterruptIsr && Config->InterruptRaw==&RawResources[1] && Config->InterruptTranslated==&TranslatedResources[1]);
  CHECK(Config->EvtInterruptEnable==HmInterruptEnable && Config->EvtInterruptDisable==HmInterruptDisable);
  CHECK(!InterruptObjectAlive);
  InterruptCreates++;
  if(NT_SUCCESS(InterruptCreateStatus)) { *Interrupt=(WDFINTERRUPT)2; InterruptObjectAlive=TRUE; }
  return InterruptCreateStatus;
}
static PVOID Context(PWDF_DRIVER_GLOBALS Globals,WDFOBJECT Object,
                     PCWDF_OBJECT_CONTEXT_TYPE_INFO TypeInfo) {
  UNREFERENCED_PARAMETER(Globals); UNREFERENCED_PARAMETER(Object); UNREFERENCED_PARAMETER(TypeInfo);
  return &D;
}
static WDFOBJECT WorkParent(PWDF_DRIVER_GLOBALS Globals,WDFWORKITEM Work) {
  UNREFERENCED_PARAMETER(Globals); UNREFERENCED_PARAMETER(Work); return (WDFOBJECT)1;
}
static VOID WorkEnqueue(PWDF_DRIVER_GLOBALS Globals,WDFWORKITEM Work) {
  UNREFERENCED_PARAMETER(Globals); UNREFERENCED_PARAMETER(Work); EnqueueCalls++;
}
static VOID WorkFlush(PWDF_DRIVER_GLOBALS Globals,WDFWORKITEM Work) {
  UNREFERENCED_PARAMETER(Globals); UNREFERENCED_PARAMETER(Work); FlushCalls++;
}
static VOID InterruptDisable(PWDF_DRIVER_GLOBALS Globals,WDFINTERRUPT Interrupt) {
  UNREFERENCED_PARAMETER(Globals); CHECK(InterruptObjectAlive && Interrupt==(WDFINTERRUPT)2); DisableCalls++;
}
static WDFDEVICE InterruptDevice(PWDF_DRIVER_GLOBALS Globals,WDFINTERRUPT Interrupt) {
  UNREFERENCED_PARAMETER(Globals); CHECK(InterruptObjectAlive && Interrupt==(WDFINTERRUPT)2); return (WDFDEVICE)1;
}
static WDFIOTARGET DeviceIoTarget(PWDF_DRIVER_GLOBALS Globals,WDFDEVICE Device) {
  UNREFERENCED_PARAMETER(Globals); UNREFERENCED_PARAMETER(Device); return (WDFIOTARGET)4;
}

static ULONG ResourceCount(PWDF_DRIVER_GLOBALS G,WDFCMRESLIST List) {
  UNREFERENCED_PARAMETER(G);
  return List==(WDFCMRESLIST)5?RawCount:TranslatedCount;
}
static PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource(PWDF_DRIVER_GLOBALS G,WDFCMRESLIST List,ULONG I) {
  UNREFERENCED_PARAMETER(G);
  if (MissingResource || I>=2) return NULL;
  return List==(WDFCMRESLIST)5?&RawResources[I]:&TranslatedResources[I];
}
static VOID TestResources(void) {
  PCM_PARTIAL_RESOURCE_DESCRIPTOR A,B; ULONG Low=0xaaaa,High=0xbbbb;
  memset(RawResources,0,sizeof(RawResources));
  RawResources[0].Type=CmResourceTypeConnection;
  RawResources[0].u.Connection.Class=CM_RESOURCE_CONNECTION_CLASS_SERIAL;
  RawResources[0].u.Connection.Type=CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C;
  RawResources[0].u.Connection.IdLowPart=0x1234; RawResources[0].u.Connection.IdHighPart=0x5678;
  RawResources[1].Type=CmResourceTypeInterrupt;
  RawResources[1].ShareDisposition=CmResourceShareDeviceExclusive;
  RawResources[1].Flags=CM_RESOURCE_INTERRUPT_LATCHED;
  memcpy(TranslatedResources,RawResources,sizeof(RawResources));
  CHECK(HmResources((WDFCMRESLIST)5,(WDFCMRESLIST)6,&Low,&High,&A,&B)==STATUS_SUCCESS);
  CHECK(Low==0x1234&&High==0x5678&&A==&RawResources[1]&&B==&TranslatedResources[1]);
  for (ULONG Pass=0;Pass<2;Pass++) {
    CM_PARTIAL_RESOURCE_DESCRIPTOR *R=Pass?TranslatedResources:RawResources;
    UCHAR Old; USHORT Flags;
    { CM_PARTIAL_RESOURCE_DESCRIPTOR Saved=R[1];
      R[1]=R[0];
      CHECK(HmResources((WDFCMRESLIST)5,(WDFCMRESLIST)6,&Low,&High,&A,&B)==STATUS_DEVICE_CONFIGURATION_ERROR);
      R[1]=Saved; Saved=R[0]; R[0]=R[1];
      CHECK(HmResources((WDFCMRESLIST)5,(WDFCMRESLIST)6,&Low,&High,&A,&B)==STATUS_DEVICE_CONFIGURATION_ERROR);
      R[0]=Saved;
    }
    Old=R[0].Type; R[0].Type=CmResourceTypeMemory;
    CHECK(HmResources((WDFCMRESLIST)5,(WDFCMRESLIST)6,&Low,&High,&A,&B)==STATUS_DEVICE_CONFIGURATION_ERROR); R[0].Type=Old;
    Old=R[0].u.Connection.Type; R[0].u.Connection.Type=CM_RESOURCE_CONNECTION_TYPE_SERIAL_SPI;
    CHECK(HmResources((WDFCMRESLIST)5,(WDFCMRESLIST)6,&Low,&High,&A,&B)==STATUS_DEVICE_CONFIGURATION_ERROR); R[0].u.Connection.Type=Old;
    Old=R[1].ShareDisposition; R[1].ShareDisposition=CmResourceShareShared;
    CHECK(HmResources((WDFCMRESLIST)5,(WDFCMRESLIST)6,&Low,&High,&A,&B)==STATUS_NOT_SUPPORTED); R[1].ShareDisposition=Old;
    Flags=R[1].Flags; R[1].Flags=CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
    CHECK(HmResources((WDFCMRESLIST)5,(WDFCMRESLIST)6,&Low,&High,&A,&B)==STATUS_NOT_SUPPORTED);
    R[1].Flags=CM_RESOURCE_INTERRUPT_MESSAGE;
    CHECK(HmResources((WDFCMRESLIST)5,(WDFCMRESLIST)6,&Low,&High,&A,&B)==STATUS_NOT_SUPPORTED); R[1].Flags=Flags;
  }
  TranslatedResources[0].u.Connection.IdHighPart++;
  CHECK(HmResources((WDFCMRESLIST)5,(WDFCMRESLIST)6,&Low,&High,&A,&B)==STATUS_DEVICE_CONFIGURATION_ERROR);
  TranslatedResources[0].u.Connection.IdHighPart--;
  RawCount=1; CHECK(HmResources((WDFCMRESLIST)5,(WDFCMRESLIST)6,&Low,&High,&A,&B)==STATUS_DEVICE_CONFIGURATION_ERROR); RawCount=2;
  TranslatedCount=3; CHECK(HmResources((WDFCMRESLIST)5,(WDFCMRESLIST)6,&Low,&High,&A,&B)==STATUS_DEVICE_CONFIGURATION_ERROR); TranslatedCount=2;
  MissingResource=TRUE; CHECK(HmResources((WDFCMRESLIST)5,(WDFCMRESLIST)6,&Low,&High,&A,&B)==STATUS_DEVICE_CONFIGURATION_ERROR); MissingResource=FALSE;
  CHECK(HmResources(NULL,(WDFCMRESLIST)6,&Low,&High,&A,&B)==STATUS_DEVICE_CONFIGURATION_ERROR);
  D.ConnectionIdLow=0xaaaa; D.ConnectionIdHigh=0xbbbb; RawCount=3;
  CHECK(HmPrepare((WDFDEVICE)1,(WDFCMRESLIST)5,(WDFCMRESLIST)6)==STATUS_DEVICE_CONFIGURATION_ERROR);
  CHECK(!D.Spb&&!D.Interrupt&&!IoctlCalls&&D.ConnectionIdLow==0xaaaa&&D.ConnectionIdHigh==0xbbbb);
  RawCount=2;
}

static void MakePacket(UCHAR *Raw) {
  ULONG Sum=0,I;
  memset(Raw,0,EVENT_BYTES);
  if (PacketValid) {
    for (I=0;I<HM_MAX_CONTACTS;I++) {
      Raw[I*4]=Raw[I*4+1]=Raw[I*4+2]=Raw[I*4+3]=0xff;
    }
    Raw[0]=(UCHAR)(PacketX>>8); Raw[1]=(UCHAR)PacketX;
    Raw[2]=(UCHAR)(PacketY>>8); Raw[3]=(UCHAR)PacketY;
    Raw[40]=(UCHAR)PacketPressure; Raw[52]=1;
  }
  for (I=0;I<EVENT_BYTES-1;I++) Sum+=Raw[I];
  Raw[EVENT_BYTES-1]=(UCHAR)(0u-Sum);
}

static NTSTATUS SendIoctl(PWDF_DRIVER_GLOBALS Globals,WDFIOTARGET Target,
                          WDFREQUEST Request,ULONG Ioctl,
                          PWDF_MEMORY_DESCRIPTOR Input,
                          PWDF_MEMORY_DESCRIPTOR Output,
                          PWDF_REQUEST_SEND_OPTIONS Options,
                          ULONG_PTR *BytesReturned) {
  UNREFERENCED_PARAMETER(Globals); UNREFERENCED_PARAMETER(Target);
  UNREFERENCED_PARAMETER(Request); IoctlCalls++;
  if (Ioctl==IOCTL_RH_QUERY_CONNECTION_PROPERTIES) {
    CHECK(FALSE); return STATUS_INVALID_DEVICE_REQUEST;
  }
  if (Ioctl==IOCTL_SPB_EXECUTE_SEQUENCE) {
    HM_SPB_SEQUENCE *List; SPB_TRANSFER_LIST_ENTRY *Write,*Read=NULL;
    UCHAR *Bytes; ULONG Length; ULONG_PTR ExpectedBytes;
    CHECK(Input && Input->Type==WdfMemoryDescriptorTypeBuffer);
    CHECK(Input->u.BufferType.Length==sizeof(HM_SPB_SEQUENCE));
    CHECK(Options && Options->Timeout==WDF_REL_TIMEOUT_IN_MS(50));
    if (!Input || Input->Type!=WdfMemoryDescriptorTypeBuffer ||
        Input->u.BufferType.Length<sizeof(SPB_TRANSFER_LIST)) return STATUS_INVALID_PARAMETER;
    List=(HM_SPB_SEQUENCE *)Input->u.BufferType.Buffer;
    CHECK(List->List.Size==sizeof(SPB_TRANSFER_LIST));
    CHECK(List->List.Reserved==0 && (List->List.TransferCount==1||List->List.TransferCount==2));
    Write=&List->List.Transfers[0];
    CHECK(Write->Direction==SpbTransferDirectionToDevice&&Write->DelayInUs==0);
    CHECK(Write->Buffer.Format==SpbTransferBufferFormatSimple);
    Bytes=(UCHAR *)Write->Buffer.Simple.Buffer; Length=Write->Buffer.Simple.BufferCb;
    CHECK(Bytes!=NULL&&Length>=1&&Length<=HM_BUS_WRITE_MAX_BODY+1);
    if (List->List.TransferCount==2) {
      Read=&List->List.Transfers[1];
      CHECK(Read->Direction==SpbTransferDirectionFromDevice&&Read->DelayInUs==0);
      CHECK(Read->Buffer.Format==SpbTransferBufferFormatSimple);
      CHECK(Read->Buffer.Simple.BufferCb>=1&&Read->Buffer.Simple.BufferCb<=HM_BUS_READ_MAX_BYTES);
    }
    if (!Bytes || Length<1) return STATUS_INVALID_PARAMETER;
    if (Bytes[0]==HM_EVENT_COMMAND) {
      CHECK(Length==1&&Read&&Read->Buffer.Simple.BufferCb==EVENT_BYTES);
      if (!Read || Read->Buffer.Simple.BufferCb!=EVENT_BYTES) return STATUS_INVALID_PARAMETER;
      MakePacket((UCHAR *)Read->Buffer.Simple.Buffer); EventReads++;
    } else if (Bytes[0]==0x13 || Bytes[0]==0x0d) {
      UCHAR Value=Bytes[0]==0x13?0x31:0x10;
      if (Read) {
        CHECK(Length==1&&Read->Buffer.Simple.BufferCb==1);
        ((UCHAR *)Read->Buffer.Simple.Buffer)[0]=InterfaceValid?Value:0;
      } else CHECK(Length==2&&Bytes[1]==Value);
    }
    else if (Bytes[0]==0x00) {
      CHECK(!Read&&(Length==5||Length==9));
      if (Length<5) return STATUS_INVALID_PARAMETER;
      SelectedAddress=(ULONG)Bytes[1]|((ULONG)Bytes[2]<<8)|((ULONG)Bytes[3]<<16)|((ULONG)Bytes[4]<<24);
      if (Length==9 && SelectedAddress==0x9000005c)
        CHECK(Bytes[5]==0&&Bytes[6]==0&&Bytes[7]==0&&Bytes[8]==0);
      else if (Length==9 && SelectedAddress==0x90000018) {
        CHECK(Bytes[5]==0x55&&Bytes[6]==0&&Bytes[7]==0&&Bytes[8]==0);
        SystemResetWrites++;
      } else if (Length==9) CHECK(0);
    } else if (Bytes[0]==0x0c) CHECK(Length==2&&Bytes[1]==0&&!Read);
    else if (Bytes[0]==0x08) {
      CHECK(Length==1&&Read&&Read->Buffer.Simple.BufferCb==4);
      if (!Read || Read->Buffer.Simple.BufferCb!=4) return STATUS_INVALID_PARAMETER;
      CHECK(SelectedAddress==0x900000a8 || SelectedAddress==0x900000d0);
      ((UCHAR *)Read->Buffer.Simple.Buffer)[0]=SelectedAddress==0x900000a8?FirmwareState:0x55;
      ((UCHAR *)Read->Buffer.Simple.Buffer)[1]=(UCHAR)(ChipIdValid?0x2c:0x2d);
      ((UCHAR *)Read->Buffer.Simple.Buffer)[2]=0x13;
      ((UCHAR *)Read->Buffer.Simple.Buffer)[3]=0x83;
    } else if (Bytes[0]==0x31) { CHECK(Length==2&&Bytes[1]==0&&!Read); SenseOnWrites++; }
    else CHECK(0);
    ExpectedBytes=Length+(Read?Read->Buffer.Simple.BufferCb:0);
    SequenceCalls++;
    if (!Read && ZeroByteWriteFailures) {
      ZeroByteWriteFailures--;
      if (BytesReturned) *BytesReturned=0;
      return STATUS_SUCCESS;
    }
    if (BytesReturned) *BytesReturned=ExpectedBytes;
    return STATUS_SUCCESS;
  }
  if (Ioctl==IOCTL_ACPI_EVAL_METHOD) {
    PACPI_EVAL_OUTPUT_BUFFER Out;
    ULONG_PTR ExpectedLength;
    CHECK(Input && Input->Type==WdfMemoryDescriptorTypeBuffer);
    CHECK(Options && Options->Timeout==WDF_REL_TIMEOUT_IN_SEC(1));
    CHECK(Output && Output->Type==WdfMemoryDescriptorTypeBuffer);
    if (!Input || !Output || !Output->u.BufferType.Buffer ||
        Output->u.BufferType.Length<sizeof(ACPI_EVAL_OUTPUT_BUFFER))
      return STATUS_INVALID_PARAMETER;
    Out=(PACPI_EVAL_OUTPUT_BUFFER)Output->u.BufferType.Buffer;
    memset(Output->u.BufferType.Buffer,0,Output->u.BufferType.Length);
    Out->Signature=ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE;
    Out->Length=Output->u.BufferType.Length; Out->Count=1;
    Out->Argument[0].Type=ACPI_METHOD_ARGUMENT_INTEGER;
    Out->Argument[0].DataLength=AcpiWideInteger?sizeof(ULONGLONG):sizeof(ULONG);
    if (Input->u.BufferType.Length==sizeof(ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER)) {
      PACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER In=(PACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER)Input->u.BufferType.Buffer;
      CHECK(In->Signature==ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE);
      CHECK(In->MethodName[0]=='H'&&In->MethodName[1]=='M'&&In->MethodName[2]=='R'&&In->MethodName[3]=='D');
      CHECK(In->IntegerArgument!=0 && In->IntegerArgument<=HM_RESUME_SEQUENCE_MASK);
      LastAcpiSequence=In->IntegerArgument; AcpiResumeCalls++;
      Out->Argument[0].Argument=(((ULONGLONG)(AcpiResumeEchoValid?In->IntegerArgument:In->IntegerArgument+1))<<8)|AcpiResumeFlags;
    } else {
      PACPI_EVAL_INPUT_BUFFER In=(PACPI_EVAL_INPUT_BUFFER)Input->u.BufferType.Buffer;
      CHECK(Input->u.BufferType.Length==sizeof(ACPI_EVAL_INPUT_BUFFER));
      CHECK(In->Signature==ACPI_EVAL_INPUT_BUFFER_SIGNATURE);
      CHECK(In->MethodName[0]=='H'&&In->MethodName[1]=='M'&&In->MethodName[2]=='X'&&In->MethodName[3]=='R');
      Out->Argument[0].Argument=AcpiReady?1:0;
    }
    ExpectedLength=Output->u.BufferType.Length;
    if (BytesReturned) *BytesReturned=ExpectedLength;
    return STATUS_SUCCESS;
  }
  CHECK(0); return STATUS_NOT_SUPPORTED;
}

NTSTATUS VhfReadReportSubmit(VHFHANDLE Vhf,PHID_XFER_PACKET Packet) {
  UNREFERENCED_PARAMETER(Vhf); SubmitCalls++;
  CHECK(Packet!=NULL);
  if (Packet) {
    CHECK(Packet->reportId==1 && Packet->reportBufferLen==sizeof(HM_HID_REPORT));
    CHECK(Packet->reportBuffer!=NULL);
    if (Packet->reportBuffer && Packet->reportBufferLen==sizeof(HM_HID_REPORT) &&
        SubmitCalls<=RTL_NUMBER_OF(Reports)) {
      HM_HID_REPORT *Report=(HM_HID_REPORT *)Packet->reportBuffer;
      CHECK(Report->ReportId==1);
      Reports[SubmitCalls-1]=*Report;
    }
  }
  if (SubmitStatus==STATUS_SUCCESS) AcceptedReports++;
  if (QueueOnSubmit) { QueueOnSubmit=0; HmQueueFrame(&D,&FrameOnSubmit); }
  if (SyncReady) { SyncReady=0; HmVhfReady(&D); }
  return SubmitStatus;
}
NTSTATUS VhfAsyncOperationComplete(VHFOPERATIONHANDLE Operation,NTSTATUS Status) {
  UNREFERENCED_PARAMETER(Operation); return Status;
}
NTSTATUS VhfCreate(PVHF_CONFIG Config,VHFHANDLE *Handle) {
  UNREFERENCED_PARAMETER(Config); *Handle=(VHFHANDLE)5; return STATUS_SUCCESS;
}
NTSTATUS VhfStart(VHFHANDLE Handle) { UNREFERENCED_PARAMETER(Handle); return STATUS_SUCCESS; }
VOID VhfDelete(VHFHANDLE Handle,BOOLEAN Wait) {
  UNREFERENCED_PARAMETER(Handle); UNREFERENCED_PARAMETER(Wait);
}

static void Init(void) {
  memset(&D,0,sizeof(D)); memset(Reports,0,sizeof(Reports));
  D.Device=(WDFDEVICE)1; D.Spb=(WDFIOTARGET)4;
  D.Interrupt=(WDFINTERRUPT)2; D.Work=(WDFWORKITEM)3; D.Vhf=(VHFHANDLE)5;
  D.Address=0x48; D.Speed=400000; D.Active=1; D.ReadyValidated=1;
  D.HimaxIo.Context=&D; D.HimaxIo.Transfer=HmSpbTransfer; D.HimaxIo.Delay=HmTransportDelay;
  KeInitializeSpinLock(&D.FrameLock);
  IoctlCalls=SequenceCalls=DisableCalls=ZeroByteWriteFailures=0;
  FlushCalls=EnqueueCalls=SubmitCalls=AcceptedReports=DelayCalls=0;
  SyncReady=0; QueueOnSubmit=0; memset(&FrameOnSubmit,0,sizeof(FrameOnSubmit));
  SelectedAddress=0; EventReads=SenseOnWrites=SystemResetWrites=0; ChipIdValid=TRUE;
  PacketValid=1; PacketX=321; PacketY=456; PacketPressure=55;
  AcpiReady=1; AcpiResumeFlags=HM_RESUME_BOARD_READY; AcpiResumeCalls=0; FirmwareState=5;
  AcpiWideInteger=FALSE;
  LastAcpiSequence=0; AcpiResumeEchoValid=TRUE; SubmitStatus=STATUS_SUCCESS;
  InterfaceValid=TRUE; InterruptObjectAlive=TRUE; ReleaseInProgress=FALSE;
}
static HM_FRAME OneContact(unsigned X,unsigned Y,unsigned Pressure) {
  HM_FRAME F={0}; F.Count=1;
  F.Contacts[0]=(HM_CONTACT){(unsigned short)X,(unsigned short)Y,(unsigned char)Pressure,1};
  return F;
}
static void SetActiveState(unsigned X,unsigned Y) {
  D.HidState.ActiveMask=1;
  D.HidState.Last[0]=(HM_HID_CONTACT){1,3,(unsigned short)X,(unsigned short)Y,77};
}
static void QueuePending(unsigned X,unsigned Y) {
  D.PendingFrame=OneContact(X,Y,55); D.HasPendingFrame=1;
  D.VhfReady=1; D.WorkQueued=1;
}

static NTSTATUS FrameworkRelease(void) {
  unsigned OldFlush=FlushCalls;
  ReleaseInProgress=TRUE;
  NTSTATUS S=HmRelease((WDFDEVICE)1,(WDFCMRESLIST)6);
  CHECK(FlushCalls==OldFlush+1 && !D.Interrupt);
  InterruptObjectAlive=FALSE;
  ReleaseInProgress=FALSE;
  return S;
}

int TestMain(void) {
  HM_FRAME F;
  Failure=Checks=0;
  Table[WdfObjectGetTypedContextWorkerTableIndex]=(WDFFUNC)Context;
  Table[WdfWorkItemGetParentObjectTableIndex]=(WDFFUNC)WorkParent;
  Table[WdfWorkItemEnqueueTableIndex]=(WDFFUNC)WorkEnqueue;
  Table[WdfWorkItemFlushTableIndex]=(WDFFUNC)WorkFlush;
  Table[WdfInterruptDisableTableIndex]=(WDFFUNC)InterruptDisable;
  Table[WdfInterruptGetDeviceTableIndex]=(WDFFUNC)InterruptDevice;
  Table[WdfDeviceGetIoTargetTableIndex]=(WDFFUNC)DeviceIoTarget;
  Table[WdfIoTargetSendIoctlSynchronouslyTableIndex]=(WDFFUNC)SendIoctl;

  Table[WdfCmResourceListGetCountTableIndex]=(WDFFUNC)ResourceCount;
  Table[WdfCmResourceListGetDescriptorTableIndex]=(WDFFUNC)Resource;
  {
    UCHAR Storage[sizeof(ACPI_METHOD_ARGUMENT)+sizeof(ULONGLONG)]={0};
    PACPI_METHOD_ARGUMENT A=(PACPI_METHOD_ARGUMENT)Storage;
    ULONG Small=0x89abcdef,Parsed;ULONGLONG Wide=MAXULONG;
    A->Type=ACPI_METHOD_ARGUMENT_INTEGER;A->DataLength=sizeof(Small);for(ULONG J=0;J<sizeof(Small);++J)A->Data[J]=((const UCHAR *)&Small)[J];
    CHECK(HmFirmwareInteger32(A,&Parsed)&&Parsed==Small);
    A->DataLength=sizeof(Wide);for(ULONG J=0;J<sizeof(Wide);++J)A->Data[J]=((const UCHAR *)&Wide)[J];
    CHECK(HmFirmwareInteger32(A,&Parsed)&&Parsed==MAXULONG);
    Wide=(ULONGLONG)MAXULONG+1;for(ULONG J=0;J<sizeof(Wide);++J)A->Data[J]=((const UCHAR *)&Wide)[J];
    CHECK(!HmFirmwareInteger32(A,&Parsed));A->DataLength=6;CHECK(!HmFirmwareInteger32(A,&Parsed));
    A->Type=ACPI_METHOD_ARGUMENT_BUFFER;A->DataLength=sizeof(Small);CHECK(!HmFirmwareInteger32(A,&Parsed));
  }
  TestResources();

  /* A successful SPB status with zero bytes is an incomplete write and retries. */
  Init();
  { const UCHAR Zero=0; ZeroByteWriteFailures=1;
    CHECK(HmBusWrite(&D.HimaxIo,0x0c,HM_BUS_WRITE_SENTINEL,&Zero,1)==HmOk);
    CHECK(!ZeroByteWriteFailures && SequenceCalls==2 && DelayCalls==1);
  }

  Table[WdfIoTargetCreateTableIndex]=(WDFFUNC)TargetCreate;
  Table[WdfIoTargetOpenTableIndex]=(WDFFUNC)TargetOpen;
  Table[WdfIoTargetCloseTableIndex]=(WDFFUNC)TargetClose;
  Table[WdfObjectDeleteTableIndex]=(WDFFUNC)ObjectDelete;
  Table[WdfInterruptCreateTableIndex]=(WDFFUNC)InterruptCreate;
  /* The opened Resource Hub target reparses to MtkI2c. Its TargetConnect
   * validates the ACPI descriptor; the SPB client must not query that target. */
  Init();D.Spb=NULL;D.Interrupt=NULL;InterruptObjectAlive=FALSE;D.ConnectionIdLow=0x1234;D.ConnectionIdHigh=0x5678;
  TargetCloses=TargetDeletes=InterruptCreates=0;
  CHECK(HmPrepare((WDFDEVICE)1,(WDFCMRESLIST)5,(WDFCMRESLIST)6)==STATUS_SUCCESS);
  CHECK(D.Spb==(WDFIOTARGET)4&&D.Interrupt==(WDFINTERRUPT)2&&InterruptCreates==1);
  CHECK(D.Address==0x48&&D.Speed==400000&&IoctlCalls==0);
  CHECK(FrameworkRelease()==STATUS_SUCCESS&&!D.Spb&&!D.Interrupt&&!InterruptObjectAlive);
  CHECK(TargetCloses==1&&TargetDeletes==1);
  CHECK(HmPrepare((WDFDEVICE)1,(WDFCMRESLIST)5,(WDFCMRESLIST)6)==STATUS_SUCCESS&&
        InterruptCreates==2&&D.Interrupt==(WDFINTERRUPT)2&&InterruptObjectAlive);
  /* Recreated interrupt/SPB objects do not authorize reuse of initial HMXR. */
  D.InitialEntryCompleted=TRUE;
  AcpiResumeFlags=0;
  CHECK(HmD0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY&&!D.Active);
  AcpiResumeFlags=HM_RESUME_BOARD_READY;
  CHECK(HmD0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS&&D.Active&&
        AcpiResumeCalls==2&&!SenseOnWrites&&!SystemResetWrites);
  CHECK(HmD0Exit((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(FrameworkRelease()==STATUS_SUCCESS&&!D.Spb&&!D.Interrupt&&
        TargetCloses==2&&TargetDeletes==2&&!InterruptObjectAlive);
  for(unsigned Stage=0;Stage<3;Stage++) {
    Init();D.Spb=NULL;D.Interrupt=NULL;InterruptObjectAlive=FALSE;D.ConnectionIdLow=0x1234;D.ConnectionIdHigh=0x5678;
    TargetCloses=TargetDeletes=InterruptCreates=0;
    TargetCreateStatus=Stage==0?STATUS_UNSUCCESSFUL:STATUS_SUCCESS;
    TargetOpenStatus=Stage==1?STATUS_UNSUCCESSFUL:STATUS_SUCCESS;
    InterruptCreateStatus=Stage==2?STATUS_UNSUCCESSFUL:STATUS_SUCCESS;
    CHECK(!NT_SUCCESS(HmPrepare((WDFDEVICE)1,(WDFCMRESLIST)5,(WDFCMRESLIST)6)) && !D.Spb && !D.Interrupt);
    CHECK(TargetCloses==(Stage==2?1:0) && TargetDeletes==(Stage==0?0:1));
  }
  TargetCreateStatus=TargetOpenStatus=InterruptCreateStatus=STATUS_SUCCESS;
  /* A callback delivered synchronously from VhfReadReportSubmit must retain the next token. */
  Init(); QueuePending(321,456); SyncReady=1;
  HmReadWork((WDFWORKITEM)3);
  CHECK(SubmitCalls==1 && AcceptedReports==1);
  CHECK(Reports[0].ReportId==1 && Reports[0].ContactCount==1);
  CHECK(Reports[0].Contacts[0].X==321 && Reports[0].Contacts[0].Y==456);
  CHECK(D.HidState.ActiveMask==1 && D.HidState.Last[0].X==321);
  CHECK(D.VhfReady==1 && D.HasPendingFrame==0);

  /* Failed VHF submit consumes exactly one ready token and commits no HID state. */
  Init(); QueuePending(222,333); SubmitStatus=STATUS_UNSUCCESSFUL;
  HmReadWork((WDFWORKITEM)3);
  CHECK(SubmitCalls==1 && AcceptedReports==0);
  CHECK(D.VhfReady==0 && D.HasPendingFrame==1 && D.HidState.ActiveMask==0);
  SubmitStatus=STATUS_SUCCESS; HmVhfReady(&D);
  CHECK(D.VhfReady==1 && EnqueueCalls==1);
  HmReadWork((WDFWORKITEM)3);
  CHECK(SubmitCalls==2 && D.HidState.ActiveMask==1 && D.HidState.Last[0].X==222);
  CHECK(D.HasPendingFrame==0);

  /* Initial handoff is sampled fresh until it succeeds; it never authorizes a
   * later D0 return. Failed gates do no I2C access. */
  Init(); D.Active=0; D.ReadyValidated=0; AcpiReady=0;
  CHECK(HmD0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY);
  CHECK(D.Active==0 && D.ReadyValidated==0 && SequenceCalls==0 && !D.InitialEntryCompleted);
  AcpiReady=1; AcpiWideInteger=TRUE;
  CHECK(HmD0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(D.Active==1 && D.InitialEntryCompleted && IoctlCalls==17);
  CHECK(SequenceCalls==15&&EventReads==1&&SenseOnWrites==1&&SystemResetWrites==0&&DelayCalls==1);
  HmVhfReady(&D);
  HmReadWork((WDFWORKITEM)3);
  CHECK(AcceptedReports==1 && Reports[0].Contacts[0].X==PacketX);
  AcpiWideInteger=FALSE;
  Init(); D.Active=0; D.ReadyValidated=0; ChipIdValid=FALSE;
  CHECK(HmD0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_DEVICE_CONFIGURATION_ERROR);
  CHECK(D.Active==0&&D.ReadyValidated==0&&SequenceCalls==25&&SenseOnWrites==0&&SystemResetWrites==0&&DelayCalls==0);
  Init(); D.Active=0; D.ReadyValidated=0; InterfaceValid=FALSE;
  CHECK(HmD0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(D.Active&&D.ReadyValidated&&D.InitialEntryCompleted&&SequenceCalls==51&&SenseOnWrites==1&&SystemResetWrites==0&&DelayCalls==11);

  /* Board evidence gates SPB access. The live firmware state gates input. */
  Init(); D.Active=0; D.ReadyValidated=0;
  CHECK(HmD0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(HmD0Exit((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS);
  { unsigned BeforeIoctl=IoctlCalls,BeforeSpb=SequenceCalls,BeforeSense=SenseOnWrites;
    ULONG BeforeDelay=DelayCalls;
    AcpiResumeFlags=0;
    CHECK(HmD0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY);
    CHECK(AcpiResumeCalls==1&&D.D0Sequence==1&&SequenceCalls==BeforeSpb&&IoctlCalls==BeforeIoctl+1);
    AcpiResumeFlags=HM_RESUME_BOARD_READY; AcpiResumeEchoValid=FALSE;
    CHECK(HmD0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY);
    CHECK(AcpiResumeCalls==2&&D.D0Sequence==2&&SequenceCalls==BeforeSpb&&IoctlCalls==BeforeIoctl+2);
    AcpiResumeEchoValid=TRUE; FirmwareState=0x0c;
    CHECK(HmD0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY);
    CHECK(AcpiResumeCalls==3&&SequenceCalls==BeforeSpb+10&&!D.Active&&!D.ReadyValidated);
    FirmwareState=0x05; AcpiWideInteger=TRUE; PacketX=789;
    CHECK(HmD0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS);
    CHECK(AcpiResumeCalls==4&&D.D0Sequence==4&&LastAcpiSequence==4);
    AcpiWideInteger=FALSE;
    CHECK(SequenceCalls==BeforeSpb+21&&IoctlCalls==BeforeIoctl+25);
    CHECK(SenseOnWrites==BeforeSense&&DelayCalls==BeforeDelay&&SystemResetWrites==0);
    CHECK(D.Active&&D.ReadyValidated&&D.InitialEntryCompleted);
    HmVhfReady(&D);
    HmReadWork((WDFWORKITEM)3);
    CHECK(AcceptedReports==1 && Reports[0].Contacts[0].X==789);
  }

  Init();D.Active=0;D.ReadyValidated=0;PacketValid=0;
  CHECK(HmD0Entry((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(EventReads==1 && !D.FrameCount && !D.FailureCount && !D.InterruptFaulted);

  /* A valid packet resets the consecutive-error counter. */
  Init(); PacketValid=0;
  CHECK(HmInterruptIsr((WDFINTERRUPT)2,0) && D.FailureCount==1);
  PacketValid=1; CHECK(HmInterruptIsr((WDFINTERRUPT)2,0));
  CHECK(D.FailureCount==0);
  CHECK(D.InterruptFaulted==0);
  CHECK(SequenceCalls==2&&EventReads==2);

  /* Three bad IRQ reports produce an all-up report and disable the WDF interrupt. */
  Init(); SetActiveState(901,1002); D.VhfReady=1; PacketValid=0;
  CHECK(HmInterruptIsr((WDFINTERRUPT)2,0));
  CHECK(HmInterruptIsr((WDFINTERRUPT)2,0));
  CHECK(HmInterruptIsr((WDFINTERRUPT)2,0));
  CHECK(SequenceCalls==3 && D.FailureCount==3 && D.InterruptFaulted==1);
  CHECK(D.ReleasePending==1);
  HmReadWork((WDFWORKITEM)3);
  CHECK(DisableCalls==1 && AcceptedReports==1);
  CHECK(Reports[0].ContactCount==1 && Reports[0].Contacts[0].ContactId==1);
  CHECK(Reports[0].Contacts[0].Flags==0 && Reports[0].Contacts[0].Pressure==0);
  CHECK(Reports[0].Contacts[0].X==901 && Reports[0].Contacts[0].Y==1002);
  CHECK(D.HidState.ActiveMask==0 && D.ReleasePending==0 && D.VhfReady==0);
  CHECK(Checks>70);

  /* A newer overflow generation cannot be cleared by an older in-flight lift submit. */
  Init(); SetActiveState(111,222); D.VhfReady=1;
  D.FailureEpoch=10; D.ReleasePending=10;
  F=OneContact(10,20,30);
  for (unsigned I=0;I<HM_FRAME_QUEUE_DEPTH;I++) HmQueueFrame(&D,&F);
  {
    HM_FRAME Empty={0}; HM_HID_REPORT Candidate; HM_HID_STATE NextState;
    CHECK(HmBuildHidReport(&Empty,&D.HidState,&Candidate,&NextState));
    FrameOnSubmit=OneContact(777,888,90); QueueOnSubmit=1;
    CHECK(HmSubmitReport(&D,&Candidate,&NextState,TRUE,10)==STATUS_SUCCESS);
  }
  CHECK(D.FailureEpoch==11 && D.ReleasePending==11);
  CHECK(D.HidState.ActiveMask==0 && D.FrameCount==1);
  Init(); D.FailureEpoch=1; D.ReleasePending=1; D.VhfReady=1;
  F=OneContact(100,200,70);
  {
    HM_HID_REPORT Candidate; HM_HID_STATE NextState;
    CHECK(HmBuildHidReport(&F,&D.HidState,&Candidate,&NextState));
    CHECK(HmSubmitReport(&D,&Candidate,&NextState,FALSE,0)==STATUS_DEVICE_NOT_READY);
  }
  CHECK(SubmitCalls==0 && D.VhfReady==1 && D.HidState.ActiveMask==0);

  /* Queue overflow emits all-up, then resumes with the newest contact on next VHF token. */
  Init(); SetActiveState(50,60);
  for (unsigned I=0;I<5;I++) { PacketX=400+I; PacketY=500+I; CHECK(HmInterruptIsr((WDFINTERRUPT)2,0)); }
  CHECK(D.ReleasePending==1 && D.FrameCount==1 && D.Frames[D.FrameHead].Contacts[0].X==404);
  D.VhfReady=1; HmReadWork((WDFWORKITEM)3);
  CHECK(AcceptedReports==1 && Reports[0].Contacts[0].Flags==0);
  CHECK(D.HidState.ActiveMask==0 && D.HasPendingFrame==1 && D.VhfReady==0);
  HmVhfReady(&D); HmReadWork((WDFWORKITEM)3);
  CHECK(AcceptedReports==2 && Reports[1].ContactCount==1);
  CHECK(Reports[1].Contacts[0].Flags==3 && Reports[1].Contacts[0].X==404);
  CHECK(Reports[1].Contacts[0].Y==504 && D.HidState.ActiveMask==1);

  /* A queued lift cannot be overwritten by a later down while VHF has no token. */
  Init();
  F=OneContact(101,201,70); HmQueueFrame(&D,&F);
  memset(&F,0,sizeof(F)); HmQueueFrame(&D,&F);
  F=OneContact(303,404,80); HmQueueFrame(&D,&F);
  D.VhfReady=1; D.WorkQueued=1; HmReadWork((WDFWORKITEM)3);
  CHECK(SubmitCalls==1 && Reports[0].Contacts[0].Flags==3);
  CHECK(D.HasPendingFrame==1 && D.PendingFrame.Count==0);
  CHECK(D.FrameCount==1 && D.Frames[D.FrameHead].Contacts[0].X==303 && D.VhfReady==0);
  CHECK(EnqueueCalls==0);
  HmVhfReady(&D); HmReadWork((WDFWORKITEM)3);
  CHECK(SubmitCalls==2 && Reports[1].ContactCount==1);
  CHECK(Reports[1].Contacts[0].Flags==0 && Reports[1].Contacts[0].X==101 && Reports[1].Contacts[0].Y==201);
  CHECK(D.HasPendingFrame==1 && D.PendingFrame.Contacts[0].X==303 && D.VhfReady==0);
  CHECK(EnqueueCalls==1);
  HmVhfReady(&D); HmReadWork((WDFWORKITEM)3);
  CHECK(SubmitCalls==3 && Reports[2].Contacts[0].Flags==3);
  CHECK(Reports[2].Contacts[0].X==303 && Reports[2].Contacts[0].Y==404 && D.HasPendingFrame==0);
  CHECK(EnqueueCalls==2);

  /* Power exit submits a lift while a VHF token is available; without one it retains it. */
  Init(); SetActiveState(700,800); D.VhfReady=1;
  CHECK(HmD0Exit((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(FlushCalls==1 && AcceptedReports==1 && D.HidState.ActiveMask==0);
  CHECK(Reports[0].Contacts[0].X==700 && Reports[0].Contacts[0].Y==800);
  CHECK(Reports[0].Contacts[0].Flags==0 && D.ReleasePending==0);
  Init(); SetActiveState(700,800);
  CHECK(HmD0Exit((WDFDEVICE)1,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(FlushCalls==1 && AcceptedReports==0 && D.ReleasePending==1);
  CHECK(D.HidState.ActiveMask==1);

  CHECK(sizeof(HM_HID_REPORT)==72 && Checks>120);
  return Failure;
}
