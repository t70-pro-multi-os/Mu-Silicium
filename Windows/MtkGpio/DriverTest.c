/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Execute production Driver.c ARM64 callbacks with WDF/GpioClx/ACPI/MMIO mocks.
 */
#include <ntddk.h>
ULONG TestReadRegister(PULONG Address);
VOID TestWriteRegister(PULONG Address,ULONG Value);
PVOID TestMap(PHYSICAL_ADDRESS Address,SIZE_T Length,ULONG Flags);
VOID TestUnmap(PVOID Base,SIZE_T Length);
VOID TestInitializeFastMutex(PFAST_MUTEX Mutex);
VOID TestAcquireFastMutex(PFAST_MUTEX Mutex);
VOID TestReleaseFastMutex(PFAST_MUTEX Mutex);
VOID TestInitializeMutex(PKMUTEX Mutex,ULONG Level);
NTSTATUS TestWaitForSingleObject(PVOID Object,KWAIT_REASON Reason,KPROCESSOR_MODE Mode,
    BOOLEAN Alertable,PLARGE_INTEGER Timeout);
LONG TestReleaseMutex(PKMUTEX Mutex,BOOLEAN Wait);
LONG TestInterlockedCompareExchange(volatile LONG *Target,LONG Exchange,LONG Compare);
LONG TestInterlockedExchange(volatile LONG *Target,LONG Value);
LONG TestInterlockedIncrement(volatile LONG *Target);
LONG TestInterlockedDecrement(volatile LONG *Target);
VOID TestStallProcessor(ULONG Microseconds);
KIRQL TestCurrentIrql(void);
VOID TestInitializeSpinLock(PKSPIN_LOCK Lock);
VOID TestAcquireSpinLock(PKSPIN_LOCK Lock,PKIRQL OldIrql);
VOID TestReleaseSpinLock(PKSPIN_LOCK Lock,KIRQL OldIrql);
#undef KeGetCurrentIrql
#undef KeInitializeSpinLock
#undef KeAcquireSpinLock
#undef KeReleaseSpinLock
#define KeGetCurrentIrql TestCurrentIrql
#define KeInitializeSpinLock TestInitializeSpinLock
#define KeAcquireSpinLock TestAcquireSpinLock
#define KeReleaseSpinLock TestReleaseSpinLock
#undef InterlockedCompareExchange
#undef InterlockedExchange
#undef InterlockedIncrement
#undef InterlockedDecrement
#undef KeStallExecutionProcessor
#define InterlockedCompareExchange TestInterlockedCompareExchange
#define InterlockedExchange TestInterlockedExchange
#define InterlockedIncrement TestInterlockedIncrement
#define InterlockedDecrement TestInterlockedDecrement
#define KeStallExecutionProcessor TestStallProcessor
#undef ExInitializeFastMutex
#undef ExAcquireFastMutex
#undef ExReleaseFastMutex
#define ExInitializeFastMutex TestInitializeFastMutex
#define ExAcquireFastMutex TestAcquireFastMutex
#define ExReleaseFastMutex TestReleaseFastMutex
#undef KeInitializeMutex
#undef KeWaitForSingleObject
#undef KeReleaseMutex
#define KeInitializeMutex TestInitializeMutex
#define KeWaitForSingleObject TestWaitForSingleObject
#define KeReleaseMutex TestReleaseMutex
#undef READ_REGISTER_ULONG
#undef WRITE_REGISTER_ULONG
#define READ_REGISTER_ULONG TestReadRegister
#define WRITE_REGISTER_ULONG TestWriteRegister
#define MmMapIoSpaceEx TestMap
#define MmUnmapIoSpace TestUnmap
#include "Driver.c"
static unsigned Failure,Checks,LockDepth,LockMask,LockCalls,FastMutexDepth,Maps,Unmaps,MapFailure,AcpiVariant,Ioctls;
static unsigned MutexInitialized,ConfigurationMutexDepth,ConfigurationMutexAcquires;
static unsigned SpinDepth,FrameworkReferences,StopRemoveCalls,AddFailure,QueryCalls,DeviceInterfaceCalls;
static BOOLEAN Stoppable,HardwareAccessible;
static PFN_WDFDEVICE_WDM_IRP_PREPROCESS Preprocess;
static unsigned PreprocessCalls,DispatchCalls,DrainDirqlOnStall,Stalls;
static IRP RemovalIrp;
static IO_STACK_LOCATION RemovalStack[2];
static KIRQL CurrentIrql,MutexIrql[4],ConfigurationMutexIrql,BankIrql[MG_BANKS];
static FAST_MUTEX *MutexStack[4];
static MG_INTERFACE_CONTEXT Lna;
static MTK_GPS_LNA_INTERFACE LnaInterface;
static MTK_CONN_PINS_INTERFACE ConnPinsInterface;
static unsigned RawCount,TranslatedCount;
static NTSTATUS AcpiStatus;
static MG_DEVICE Device;
static CM_PARTIAL_RESOURCE_DESCRIPTOR RawResources[MG_SPACES+MG_BANKS],TranslatedResources[MG_SPACES+MG_BANKS];
static WDFFUNC Table[WdfFunctionTableNumEntries];
const WDFFUNC *WdfFunctions=Table;
PWDF_DRIVER_GLOBALS WdfDriverGlobals;
BOOLEAN WdfClientVersionHigherThanFramework;
ULONG WdfStructureCount;
WDF_STRUCT_INFO WdfStructures;
PGPIO_CLX_EXPORTED_INTERFACES GpioClxExportedInterfaces[GPIO_CLX_TOTAL_EXPORTS];
#define CHECK(X) do { Checks++; if (!(X) && !Failure) Failure=__LINE__; } while (0)
#include "TestMmio.h"
#pragma function(memcpy,memset)
void *memcpy(void *To,const void *From,size_t N) {
  volatile UCHAR *A=To; const volatile UCHAR *B=From; while (N--) *A++=*B++; return To;
}
void *memset(void *To,int V,size_t N) {
  volatile UCHAR *A=To; while (N--) *A++=(UCHAR)V; return To;
}
static unsigned SpaceFor(PULONG Address,unsigned *Offset) {
  unsigned S;
  for (S=0;S<MG_SPACES;S++) if ((ULONG_PTR)Address>=(ULONG_PTR)Registers[S] &&
      (ULONG_PTR)Address<(ULONG_PTR)Registers[S]+MG_MMIO_LENGTH) {
    *Offset=(unsigned)((ULONG_PTR)Address-(ULONG_PTR)Registers[S]); return S;
  }
  CHECK(0); *Offset=0; return 0;
}
ULONG TestReadRegister(PULONG Address) {
  unsigned Offset,Space=SpaceFor(Address,&Offset); uint32_t Value=0;
  CHECK(HardwareAccessible);
  CHECK(FakeRead(NULL,Space,Offset,&Value)==MgOk); return Value;
}
VOID TestWriteRegister(PULONG Address,ULONG Value) {
  unsigned Offset,Space=SpaceFor(Address,&Offset);
  CHECK(HardwareAccessible);
  CHECK(FakeWrite(NULL,Space,Offset,Value)==MgOk);
}
PVOID TestMap(PHYSICAL_ADDRESS Address,SIZE_T Length,ULONG Flags) {
  Maps++; CHECK(Length==MG_MMIO_LENGTH && Flags==(PAGE_READWRITE|PAGE_NOCACHE));
  if (Maps==MapFailure) return NULL;
  const ULONGLONG Bases[MG_SPACES]={0x10005000,0x1000B000,0x11C30000,0x11D10000,0x11D40000,0x11E20000,0x11E60000,0x11EA0000};
  CHECK(Maps<=MG_SPACES && Address.QuadPart==Bases[Maps-1]);
  return Registers[Maps-1];
}
VOID TestUnmap(PVOID Base,SIZE_T Length) {
  unsigned I;
  CHECK(Length==MG_MMIO_LENGTH);
  for (I=0;I<MG_SPACES;I++) if (Base==Registers[I]) break;
  CHECK(I<MG_SPACES); Unmaps++;
}
VOID TestInitializeFastMutex(PFAST_MUTEX Mutex) {
  CHECK(!FastMutexDepth && Mutex==&Lna.Gate);
  MutexInitialized|=1U;
}
VOID TestAcquireFastMutex(PFAST_MUTEX Mutex) {
  unsigned I;
  CHECK(Mutex==&Lna.Gate && (MutexInitialized&1U));
  CHECK(CurrentIrql<=APC_LEVEL && FastMutexDepth<4);
  for (I=0;I<FastMutexDepth;I++) CHECK(MutexStack[I]!=Mutex);
  MutexStack[FastMutexDepth]=Mutex; MutexIrql[FastMutexDepth++]=CurrentIrql; CurrentIrql=APC_LEVEL;
}
VOID TestReleaseFastMutex(PFAST_MUTEX Mutex) {
  CHECK(Mutex==&Lna.Gate && FastMutexDepth && MutexStack[FastMutexDepth-1]==Mutex && CurrentIrql==APC_LEVEL);
  CurrentIrql=MutexIrql[--FastMutexDepth];
}
VOID TestInitializeMutex(PKMUTEX Mutex,ULONG Level) {
  CHECK(Mutex==&Device.ConfigurationLock && Level==0 && !ConfigurationMutexDepth);
  MutexInitialized|=2U;
}
NTSTATUS TestWaitForSingleObject(PVOID Object,KWAIT_REASON Reason,KPROCESSOR_MODE Mode,
    BOOLEAN Alertable,PLARGE_INTEGER Timeout) {
  CHECK(Object==&Device.ConfigurationLock && (MutexInitialized&2U));
  CHECK(Reason==Executive && Mode==KernelMode && !Alertable && Timeout==NULL);
  CHECK(CurrentIrql<=APC_LEVEL && !ConfigurationMutexDepth);
  ConfigurationMutexDepth=1; ConfigurationMutexIrql=CurrentIrql; ConfigurationMutexAcquires++;
  return STATUS_SUCCESS;
}
LONG TestReleaseMutex(PKMUTEX Mutex,BOOLEAN Wait) {
  CHECK(Mutex==&Device.ConfigurationLock && !Wait && ConfigurationMutexDepth==1 &&
        CurrentIrql==ConfigurationMutexIrql);
  ConfigurationMutexDepth=0;
  return 0;
}
KIRQL TestCurrentIrql(void) { return CurrentIrql; }
VOID TestInitializeSpinLock(PKSPIN_LOCK Lock) { CHECK(Lock==&Lna.ReferenceLock); SpinDepth=0; }
VOID TestAcquireSpinLock(PKSPIN_LOCK Lock,PKIRQL OldIrql) {
  CHECK(Lock==&Lna.ReferenceLock && !SpinDepth && CurrentIrql<=DISPATCH_LEVEL);
  *OldIrql=CurrentIrql; CurrentIrql=DISPATCH_LEVEL; SpinDepth++;
}
VOID TestReleaseSpinLock(PKSPIN_LOCK Lock,KIRQL OldIrql) {
  CHECK(Lock==&Lna.ReferenceLock && SpinDepth==1 && CurrentIrql==DISPATCH_LEVEL);
  SpinDepth--; CurrentIrql=OldIrql;
}
LONG TestInterlockedCompareExchange(volatile LONG *Target,LONG Exchange,LONG Compare) {
  LONG Old=*Target; if (Old==Compare) *Target=Exchange; return Old;
}
LONG TestInterlockedExchange(volatile LONG *Target,LONG Value) {
  LONG Old=*Target; *Target=Value; return Old;
}
LONG TestInterlockedIncrement(volatile LONG *Target) { return ++*Target; }
LONG TestInterlockedDecrement(volatile LONG *Target) { return --*Target; }
VOID TestStallProcessor(ULONG Microseconds) {
  CHECK(Microseconds==1); Stalls++;
  if (DrainDirqlOnStall) {
    CHECK(Device.PowerTransition && Device.ActiveDirql==1);
    Device.ActiveDirql=0; DrainDirqlOnStall=0;
  }
}
static VOID Acquire(PVOID Context,BANK_ID Bank) {
  CHECK(Context==&Device && Bank<MG_BANKS && !(LockMask&(1U<<Bank)));
  BankIrql[Bank]=CurrentIrql; CurrentIrql=5;
  LockMask|=1U<<Bank; LockDepth++; LockCalls++;
}
static VOID Release(PVOID Context,BANK_ID Bank) {
  CHECK(Context==&Device && Bank<MG_BANKS && (LockMask&(1U<<Bank)) && LockDepth);
  LockMask&=~(1U<<Bank); LockDepth--; CurrentIrql=BankIrql[Bank];
}
static ULONG ResourceCount(PWDF_DRIVER_GLOBALS G,WDFCMRESLIST List) {
  UNREFERENCED_PARAMETER(G); return List==(WDFCMRESLIST)1?RawCount:TranslatedCount;
}
static PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource(PWDF_DRIVER_GLOBALS G,WDFCMRESLIST List,ULONG Index) {
  UNREFERENCED_PARAMETER(G); CHECK(Index<MG_SPACES+MG_BANKS); return List==(WDFCMRESLIST)1?&RawResources[Index]:&TranslatedResources[Index];
}
static WDFIOTARGET IoTarget(PWDF_DRIVER_GLOBALS G,WDFDEVICE D) {
  UNREFERENCED_PARAMETER(G); CHECK(D==(WDFDEVICE)7); return (WDFIOTARGET)8;
}
static NTSTATUS Ioctl(PWDF_DRIVER_GLOBALS G,WDFIOTARGET Target,WDFREQUEST Request,ULONG Code,
    PWDF_MEMORY_DESCRIPTOR In,PWDF_MEMORY_DESCRIPTOR Out,PWDF_REQUEST_SEND_OPTIONS Options,ULONG_PTR *Returned) {
  PACPI_EVAL_INPUT_BUFFER Input; PACPI_EVAL_OUTPUT_BUFFER Output;
  UNREFERENCED_PARAMETER(G); UNREFERENCED_PARAMETER(Request); CHECK(TestCurrentIrql()==PASSIVE_LEVEL); Ioctls++;
  CHECK(Target==(WDFIOTARGET)8 && Code==IOCTL_ACPI_EVAL_METHOD && Options->Timeout==WDF_REL_TIMEOUT_IN_SEC(2));
  CHECK(In->Type==WdfMemoryDescriptorTypeBuffer && Out->Type==WdfMemoryDescriptorTypeBuffer);
  Input=In->u.BufferType.Buffer; Output=Out->u.BufferType.Buffer;
  CHECK(Input->Signature==ACPI_EVAL_INPUT_BUFFER_SIGNATURE && Input->MethodName[0]=='G' &&
        Input->MethodName[1]=='R' && Input->MethodName[2]=='D' && Input->MethodName[3]=='Y');
  if (!NT_SUCCESS(AcpiStatus)) return AcpiStatus;
  Output->Signature=ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE; Output->Count=1; Output->Length=sizeof(*Output);
  Output->Argument[0].Type=ACPI_METHOD_ARGUMENT_INTEGER;
  Output->Argument[0].DataLength=sizeof(ULONG); Output->Argument[0].Argument=1;
  *Returned=sizeof(*Output);
  switch (AcpiVariant) {
    case 1: Output->Argument[0].Argument=0; break;
    case 2: Output->Signature=0; break;
    case 3: Output->Count=2; break;
    case 4: Output->Length=1; break;
    case 5: *Returned=1; break;
    case 6: Output->Argument[0].DataLength=8; break;
    case 7: Output->Argument[0].Type=ACPI_METHOD_ARGUMENT_BUFFER; break;
    case 8: *Returned=Out->u.BufferType.Length+1; break;
    case 9:
      Output->Argument[0].DataLength=sizeof(ULONGLONG);
      Output->Length=FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER,Argument[0].Data)+sizeof(ULONGLONG);
      *Returned=Output->Length;
      break;
  }
  return STATUS_SUCCESS;
}
static NTSTATUS AddPre(WDFDRIVER Driver,PWDFDEVICE_INIT Init,PWDF_OBJECT_ATTRIBUTES Attributes) {
  CHECK(Driver==(WDFDRIVER)9 && Init==(PWDFDEVICE_INIT)10);
  WDF_OBJECT_ATTRIBUTES_INIT(Attributes);
  return AddFailure==1?STATUS_INSUFFICIENT_RESOURCES:STATUS_SUCCESS;
}
static NTSTATUS AssignPreprocess(PWDF_DRIVER_GLOBALS G,PWDFDEVICE_INIT Init,
    PFN_WDFDEVICE_WDM_IRP_PREPROCESS Callback,UCHAR Major,PUCHAR Minor,ULONG Count) {
  UNREFERENCED_PARAMETER(G); PreprocessCalls++;
  CHECK(Init==(PWDFDEVICE_INIT)10 && Major==IRP_MJ_PNP && Count==1 && Minor[0]==IRP_MN_SURPRISE_REMOVAL);
  if (AddFailure==6) return STATUS_INVALID_DEVICE_REQUEST;
  Preprocess=Callback; return STATUS_SUCCESS;
}
static NTSTATUS DispatchPreprocessed(PWDF_DRIVER_GLOBALS G,WDFDEVICE D,PIRP Irp) {
  UNREFERENCED_PARAMETER(G); DispatchCalls++;
  CHECK(D==(WDFDEVICE)7 && Irp==&RemovalIrp && Irp->CurrentLocation==2 &&
        Irp->Tail.Overlay.CurrentStackLocation==&RemovalStack[1]);
  return STATUS_PENDING;
}
static void RemoveUnexpectedly(void) {
  memset(&RemovalIrp,0,sizeof(RemovalIrp)); memset(RemovalStack,0,sizeof(RemovalStack));
  RemovalIrp.CurrentLocation=1; RemovalIrp.Tail.Overlay.CurrentStackLocation=RemovalStack;
  RemovalStack[0].MajorFunction=IRP_MJ_PNP; RemovalStack[0].MinorFunction=IRP_MN_SURPRISE_REMOVAL;
  CHECK(Preprocess && Preprocess((WDFDEVICE)7,&RemovalIrp)==STATUS_PENDING);
}
static NTSTATUS DeviceCreate(PWDF_DRIVER_GLOBALS G,PWDFDEVICE_INIT *Init,PWDF_OBJECT_ATTRIBUTES Attributes,WDFDEVICE *Out) {
  UNREFERENCED_PARAMETER(G);
  CHECK(*Init==(PWDFDEVICE_INIT)10 && Attributes->Size==sizeof(*Attributes));
  if (AddFailure==2) return STATUS_INSUFFICIENT_RESOURCES;
  *Init=NULL; *Out=(WDFDEVICE)7; return STATUS_SUCCESS;
}
static NTSTATUS AllocateContext(PWDF_DRIVER_GLOBALS G,WDFOBJECT Object,PWDF_OBJECT_ATTRIBUTES Attributes,PVOID *Out) {
  UNREFERENCED_PARAMETER(G);
  CHECK(Object==(WDFOBJECT)7 && Attributes->ContextTypeInfo->ContextSize==sizeof(Lna));
  if (AddFailure==3) return STATUS_INSUFFICIENT_RESOURCES;
  memset(&Lna,0,sizeof(Lna)); *Out=&Lna; return STATUS_SUCCESS;
}
static PVOID GetContext(PWDF_DRIVER_GLOBALS G,WDFOBJECT Object,PCWDF_OBJECT_CONTEXT_TYPE_INFO Type) {
  UNREFERENCED_PARAMETER(G);
  CHECK(Object==(WDFOBJECT)7 && Type->ContextSize==sizeof(Lna)); return &Lna;
}
static NTSTATUS AddPost(WDFDRIVER Driver,WDFDEVICE D) {
  CHECK(Driver==(WDFDRIVER)9 && D==(WDFDEVICE)7 && Lna.Device==D && (MutexInitialized&1U));
  return AddFailure==4?STATUS_INSUFFICIENT_RESOURCES:STATUS_SUCCESS;
}
static NTSTATUS AddQuery(PWDF_DRIVER_GLOBALS G,WDFDEVICE D,PWDF_QUERY_INTERFACE_CONFIG Q) {
  UNREFERENCED_PARAMETER(G); QueryCalls++;
  CHECK(D==(WDFDEVICE)7 && Q->Size==sizeof(*Q) && !Q->ImportInterface && !Q->SendQueryToParentStack);
  if (QueryCalls==2) {
    MTK_CONN_PINS_INTERFACE *I=(MTK_CONN_PINS_INTERFACE *)Q->Interface;
    CHECK(Q->InterfaceType->Data1==GUID_MTK_CONN_PINS_INTERFACE_V1.Data1 &&
          Q->InterfaceType->Data4[7]==GUID_MTK_CONN_PINS_INTERFACE_V1.Data4[7]);
    CHECK(I->Header.Size==sizeof(*I) && I->Header.Version==1 && I->Header.Context==&Lna &&
          I->Header.InterfaceReference && I->Header.InterfaceDereference && I->Acquire &&
          I->Read && I->Write && I->Release);
    ConnPinsInterface=*I;
    return AddFailure==7?STATUS_INSUFFICIENT_RESOURCES:STATUS_SUCCESS;
  }
  MTK_GPS_LNA_INTERFACE *I=(MTK_GPS_LNA_INTERFACE *)Q->Interface;
  CHECK(QueryCalls==1);
  CHECK(Q->InterfaceType->Data1==GUID_MTK_GPS_LNA_INTERFACE_V1.Data1 &&
        Q->InterfaceType->Data4[7]==GUID_MTK_GPS_LNA_INTERFACE_V1.Data4[7]);
  CHECK(I->Header.Size==sizeof(*I) && I->Header.Version==1 && I->Header.Context==&Lna &&
        I->Header.InterfaceReference && I->Header.InterfaceDereference && I->Acquire && I->Set && I->Release);
  LnaInterface=*I;
  return AddFailure==5?STATUS_INSUFFICIENT_RESOURCES:STATUS_SUCCESS;
}
static NTSTATUS AddDeviceInterface(PWDF_DRIVER_GLOBALS G,WDFDEVICE D,const GUID *Id,
    PCUNICODE_STRING Reference) {
  UNREFERENCED_PARAMETER(G); DeviceInterfaceCalls++;
  CHECK(D==(WDFDEVICE)7 && QueryCalls==2 && Reference==NULL &&
        Id->Data1==GUID_DEVINTERFACE_MTK_CONN_PINS.Data1 &&
        Id->Data4[7]==GUID_DEVINTERFACE_MTK_CONN_PINS.Data4[7]);
  return AddFailure==8?STATUS_INSUFFICIENT_RESOURCES:STATUS_SUCCESS;
}
static VOID ObjectReference(PWDF_DRIVER_GLOBALS G,WDFOBJECT Object,PVOID Tag,LONG Line,PCCH File) {
  UNREFERENCED_PARAMETER(G); UNREFERENCED_PARAMETER(Tag); UNREFERENCED_PARAMETER(Line); UNREFERENCED_PARAMETER(File);
  CHECK(Object==(WDFOBJECT)7 && CurrentIrql<=DISPATCH_LEVEL && !LockDepth); FrameworkReferences++;
}
static VOID ObjectDereference(PWDF_DRIVER_GLOBALS G,WDFOBJECT Object,PVOID Tag,LONG Line,PCCH File) {
  UNREFERENCED_PARAMETER(G); UNREFERENCED_PARAMETER(Tag); UNREFERENCED_PARAMETER(Line); UNREFERENCED_PARAMETER(File);
  CHECK(Object==(WDFOBJECT)7 && FrameworkReferences && CurrentIrql<=DISPATCH_LEVEL && !LockDepth);
  FrameworkReferences--;
}
static VOID StopRemove(PWDF_DRIVER_GLOBALS G,WDFDEVICE D,BOOLEAN Allowed) {
  UNREFERENCED_PARAMETER(G);
  CHECK(D==(WDFDEVICE)7 && SpinDepth==1 && Stoppable!=Allowed); Stoppable=Allowed; StopRemoveCalls++;
}
static void Init(void) {
  unsigned I;
  CurrentIrql=PASSIVE_LEVEL; MutexInitialized=ConfigurationMutexDepth=SpinDepth=FrameworkReferences=StopRemoveCalls=AddFailure=QueryCalls=0;
  DeviceInterfaceCalls=0;
  HardwareAccessible=TRUE; Stoppable=TRUE; Preprocess=NULL; PreprocessCalls=DispatchCalls=DrainDirqlOnStall=Stalls=0; memset(&Lna,0,sizeof(Lna)); memset(&LnaInterface,0,sizeof(LnaInterface));
  memset(&Device,0,sizeof(Device)); memset(RawResources,0,sizeof(RawResources));
  memset(TranslatedResources,0,sizeof(TranslatedResources)); ResetRegisters();
  for (I=0;I<24;I++) Registers[0][0x300/4+I*4]=0;
  Maps=Unmaps=MapFailure=LockCalls=LockDepth=LockMask=FastMutexDepth=Ioctls=AcpiVariant=0; RawCount=TranslatedCount=MG_SPACES+MG_BANKS; AcpiStatus=STATUS_SUCCESS;
  const ULONGLONG Bases[MG_SPACES]={0x10005000,0x1000B000,0x11C30000,0x11D10000,0x11D40000,0x11E20000,0x11E60000,0x11EA0000};
  for (I=0;I<MG_SPACES;I++) {
    RawResources[I].Type=CmResourceTypeMemory; RawResources[I].u.Memory.Start.QuadPart=Bases[I];
    RawResources[I].u.Memory.Length=MG_MMIO_LENGTH;
  }
  for (I=MG_SPACES;I<MG_SPACES+MG_BANKS;I++) {
    RawResources[I].Type=CmResourceTypeInterrupt; RawResources[I].u.Interrupt.Vector=276;
    RawResources[I].ShareDisposition=CmResourceShareShared;
  }
  CHECK(MgDeviceAdd((WDFDRIVER)9,(PWDFDEVICE_INIT)10)==STATUS_SUCCESS &&
        QueryCalls==2 && DeviceInterfaceCalls==1);
  memcpy(TranslatedResources,RawResources,sizeof(RawResources));
  for (I=MG_SPACES;I<MG_SPACES+MG_BANKS;I++) TranslatedResources[I].u.Interrupt.Vector=0x1234;
}
static void Prepare(void) {
  HardwareAccessible=TRUE;
  CHECK(MgPrepare((WDFDEVICE)7,&Device,(WDFCMRESLIST)1,(WDFCMRESLIST)2)==STATUS_SUCCESS);
  CHECK(Maps==MG_SPACES && Writes==0);
}
static void Start(void) { Init(); Prepare(); CHECK(MgControllerStart(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS); LockCalls=0; }
/* GpioClx calls release only after hardware access has ended. */
static NTSTATUS ReleaseController(void) {
  unsigned Before=Operations; NTSTATUS S;
  HardwareAccessible=FALSE;
  S=MgRelease((WDFDEVICE)7,&Device);
  CHECK(Operations==Before);
  return S;
}
static NTSTATUS StopAndReleaseController(void) {
  NTSTATUS S=MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final);
  CHECK(S==STATUS_SUCCESS);
  return NT_SUCCESS(S)?ReleaseController():S;
}
static void ReleaseLifetimeMachineCode(void) {
  GPIO_CONNECT_IO_PINS_PARAMETERS Connect={0};
  GPIO_ENABLE_INTERRUPT_PARAMETERS Enable={0};
  GPIO_READ_PINS_MASK_PARAMETERS Read={0};
  PIN_NUMBER Pin=6; ULONG64 Values; ULONG Before,BeforeMaps,BeforeIoctls;
  NTSTATUS S; PVOID C; ULONGLONG A,B; unsigned I;
  /* A clean stop restores the pad before access ends, then release only unmaps. */
  Start(); Connect.PinNumberTable=&Pin; Connect.PinCount=1;
  Connect.ConnectMode=ConnectModeOutput;
  CHECK(MgConnectIo(&Device,&Connect)==STATUS_SUCCESS && Device.Core.Pin[6].Saved);
  CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS &&
        !Device.Core.Pin[6].Saved && !Device.SeenStart);
  Before=Operations;
  CHECK(ReleaseController()==STATUS_SUCCESS && Operations==Before &&
        Unmaps==MG_SPACES && !Lna.Controller && Device.PowerTransition);
  CHECK(ReleaseController()==STATUS_SUCCESS && Unmaps==MG_SPACES && Operations==Before);
  Maps=Unmaps=0; ResetRegisters(); Prepare();
  CHECK(MgControllerStart(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
  CHECK(StopAndReleaseController()==STATUS_SUCCESS);
  /* Missing stop, failed pad restore, suspended context, and failed virtual
   * PMIC cleanup must never trigger late hardware retries or erase the journal. */
  for (I=0;I<4;I++) {
    Start();
    if (I==3) {
      Enable.BankId=MG_PMIC_IRQ_BANK; Enable.PinNumber=MG_PMIC_IRQ_NUMBER;
      Enable.InterruptMode=LevelSensitive; Enable.Polarity=InterruptActiveHigh;
      CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_SUCCESS && Device.Core.PmicInterrupt.Saved);
      DropEintWrite=1;
      CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_IO_DEVICE_ERROR &&
            Device.Core.PmicInterrupt.Saved);
    } else {
      Connect.PullConfiguration=1;
      CHECK(MgConnectIo(&Device,&Connect)==STATUS_SUCCESS && Device.Core.Pin[6].Saved);
      if (I==1) {
        DropPadWrite=1;
        CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_IO_DEVICE_ERROR &&
              Device.Core.Pin[6].Saved && Device.SeenStart);
      } else if (I==2) {
        MgAcquireConfigurationLock(&Device); BeginTransition(&Device);
        CHECK(MgSuspend(&Device.Core)==MgOk && Device.Core.Suspended &&
              Device.Core.Pin[6].ContextValid);
        MgReleaseConfigurationLock(&Device);
      }
    }
    Before=Operations; BeforeMaps=Maps; BeforeIoctls=Ioctls;
    S=ReleaseController();
    CHECK(S==STATUS_IO_DEVICE_ERROR && Operations==Before && Unmaps==MG_SPACES &&
          !Lna.Controller && !Device.Core.Active && !Device.SeenStart && Device.PowerTransition);
    if (S!=STATUS_IO_DEVICE_ERROR) return; /* Keep old-code failure reproduction bounded. */
    CHECK((I==3?Device.Core.PmicInterrupt.Saved:Device.Core.Pin[6].Saved)!=0);
    CHECK(ReleaseController()==STATUS_IO_DEVICE_ERROR && Unmaps==MG_SPACES && Operations==Before);
    CHECK(MgPrepare((WDFDEVICE)7,&Device,(WDFCMRESLIST)1,(WDFCMRESLIST)2)==STATUS_DEVICE_NOT_READY &&
          Maps==BeforeMaps && Operations==Before);
    CHECK(MgControllerStart(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_DEVICE_NOT_READY &&
          Ioctls==BeforeIoctls && Operations==Before);
    CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_DEVICE_NOT_READY &&
          Operations==Before);
    Read.PinValues=&Values;
    CHECK(MgReadPins(&Device,&Read)==STATUS_DEVICE_NOT_READY && Operations==Before);
  }
  /* Revoked LNA leases drop only their references after detach; saved pads
   * survive, stale tokens and retained interface callbacks cannot touch MMIO. */
  Start(); C=LnaInterface.Header.Context; LnaInterface.Header.InterfaceReference(C);
  CHECK(LnaInterface.Acquire(C,25,&A)==STATUS_SUCCESS && LnaInterface.Acquire(C,26,&B)==STATUS_SUCCESS);
  CHECK(Lna.References==3 && FrameworkReferences==3);
  Before=Operations;
  CHECK(ReleaseController()==STATUS_IO_DEVICE_ERROR && Operations==Before &&
        !Lna.Controller && !Device.LnaToken[0] && !Device.LnaToken[1] &&
        Device.Core.Pin[25].Saved && Device.Core.Pin[26].Saved &&
        Lna.References==1 && FrameworkReferences==1);
  CHECK(LnaInterface.Set(C,A,MtkGpsLnaOn)==STATUS_DEVICE_NOT_READY &&
        LnaInterface.Release(C,B)==STATUS_DEVICE_NOT_READY && Operations==Before);
  CHECK(ReleaseController()==STATUS_IO_DEVICE_ERROR && FrameworkReferences==1 && Unmaps==MG_SPACES);
  LnaInterface.Header.InterfaceDereference(C);
  CHECK(!Lna.References && !FrameworkReferences && Stoppable);
}
static unsigned TestRawField(const MG_FIELD *F) {
  return (Registers[F->Space][F->Offset/4U]>>F->Bit)&((1U<<F->Width)-1U);
}
/* Exercise the ARM64 core with mocked powered registers. This deliberately
 * bypasses CLIENT_StopController and does not simulate a real D3 transition. */
static void WakeMachineCode(void) {
  GPIO_ENABLE_INTERRUPT_PARAMETERS Enable={0};
  GPIO_MASK_INTERRUPT_PARAMETERS Mask={0};
  GPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS Query={0};
  uint32_t Wake[MG_BANKS]={1,0,0,0,0,0}; unsigned Before;
  Start(); Enable.InterruptMode=LevelSensitive; Enable.Polarity=InterruptActiveHigh;
  CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_SUCCESS);
  Enable.BankId=1; Enable.PinNumber=1;
  CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_SUCCESS);
  Mask.PinMask=1; CHECK(MgInterruptMask(&Device,&Mask)==STATUS_SUCCESS);
  Registers[1][0]=1; Registers[1][1]=2;
  MgAcquireConfigurationLock(&Device); BeginTransition(&Device);
  CHECK(MgSuspendWake(&Device.Core,Wake)==MgOk);
  MgReleaseConfigurationLock(&Device);
  CHECK(!(Registers[1][0x80/4]&1) && (Registers[1][0x80/4+1]&2));
  Before=Operations; Query.EnabledMask=UINT32_MAX;
  CHECK(MgActiveInterrupts(&Device,&Query)==STATUS_DEVICE_NOT_READY && Operations==Before);
  {
    unsigned IoctlsBefore=Ioctls,WritesBefore=Writes;
    CHECK(MgControllerStart(&Device,TRUE,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY &&
          Device.Core.Suspended && !Device.Core.Active && Device.PowerTransition &&
          Ioctls==IoctlsBefore+1 && Writes==WritesBefore);
  }
  MgAcquireConfigurationLock(&Device);
  CHECK(MgResume(&Device.Core)==MgOk); EndTransition(&Device);
  MgReleaseConfigurationLock(&Device);
  CHECK((Registers[1][0x80/4]&1) && !(Registers[1][0x80/4+1]&2));
  CHECK(Registers[1][0]==1 && Registers[1][1]==2);
  Query.BankId=1;
  CHECK(MgActiveInterrupts(&Device,&Query)==STATUS_SUCCESS && Query.ActiveMask==2);
  Enable.BankId=Enable.PinNumber=0;
  CHECK(MgInterruptUnmask(&Device,&Enable)==STATUS_SUCCESS);
  Query.BankId=0;
  CHECK(MgActiveInterrupts(&Device,&Query)==STATUS_SUCCESS && Query.ActiveMask==1);
  CHECK(StopAndReleaseController()==STATUS_SUCCESS);

  Start(); CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_SUCCESS);
  Wake[0]=0; DropEintWrite=1;
  MgAcquireConfigurationLock(&Device); BeginTransition(&Device);
  CHECK(MgSuspendWake(&Device.Core,Wake)==MgIoError && Device.Core.Active && !Device.Core.Suspended);
  EndTransition(&Device); MgReleaseConfigurationLock(&Device);
  CHECK(!(Registers[1][0x80/4]&1) && !Device.Core.Pin[0].ContextValid);
  CHECK(StopAndReleaseController()==STATUS_SUCCESS);
}
/* Exhaust the portable LNA core independently of the WDF interface below. */
static void LnaMachineCode(void) {
  static MG_CORE Core; static uint32_t Baseline[MG_SPACES][MG_MMIO_LENGTH/4];
  MG_IO Io={0,FakeRead,FakeWrite}; unsigned P,M,S,N,Count,Before,I; uint32_t Bit,V;
  for (P=25;P<=26;P++) for (M=0;M<8;M++) {
    ResetRegisters(); Bit=1U<<P;
    Registers[0][0x300/4+(P/8)*4]=(Registers[0][0x300/4+(P/8)*4]&~(7U<<((P%8)*4)))|(M<<((P%8)*4));
    memcpy(Baseline,Registers,sizeof(Registers));
    CHECK(MgInitialize(&Core,&Io)==MgOk && MgStart(&Core)==MgOk); Before=Writes;
    CHECK(MgGpsLnaAcquire(&Core,P)==MgOk && Writes==Before && Core.Pin[P].Saved);
    CHECK(MgConnect(&Core,P,2)==MgBusy && MgGpsLnaAcquire(&Core,P)==MgBusy);
    CHECK(MgWrite(&Core,0,Bit,0)==MgInvalid && MgDisconnect(&Core,P,1)==MgUnsupported);
    CHECK(MgRead(&Core,0,0,&V)==MgOk && V==0); Before=Operations;
    CHECK(MgSuspend(&Core)==MgUnsupported && Core.Active && Operations==Before);
    for (I=0;I<9;I++) {
      S=(I+M)%3; CHECK(MgGpsLnaSet(&Core,P,(MG_GPS_LNA_STATE)S)==MgOk);
      CHECK(((Registers[0][0x300/4+(P/8)*4]>>((P%8)*4))&7U)==(S==MgGpsLnaDsp?2U:0U));
      CHECK(!!(Registers[0][0]&Bit)==(S!=MgGpsLnaDsp));
      if (S!=MgGpsLnaDsp) CHECK(!!(Registers[0][0x100/4]&Bit)==(S==MgGpsLnaOn));
    }
    CHECK(MgStop(&Core)==MgOk && !Core.SharedCount && !Core.Pin[P].Saved);
    for (I=0;I<sizeof(Registers)/sizeof(uint32_t);I++)
      CHECK(((uint32_t *)Baseline)[I]==((uint32_t *)Registers)[I]);
    for (S=0;S<3;S++) {
      CHECK(MgStart(&Core)==MgOk && MgGpsLnaAcquire(&Core,P)==MgOk);
      Operations=0; CHECK(MgGpsLnaSet(&Core,P,(MG_GPS_LNA_STATE)S)==MgOk); Count=Operations;
      CHECK(MgStop(&Core)==MgOk);
      for (N=1;N<=Count;N++) {
        CHECK(MgStart(&Core)==MgOk && MgGpsLnaAcquire(&Core,P)==MgOk);
        Operations=0; FailOperation=N;
        CHECK(MgGpsLnaSet(&Core,P,(MG_GPS_LNA_STATE)S)==MgIoError && !Core.Active && Core.Pin[P].Saved);
        Before=Operations; CHECK(MgGpsLnaSet(&Core,P,MgGpsLnaOff)==MgNotReady && Operations==Before);
        FailOperation=0; CHECK(MgStop(&Core)==MgOk && !Core.Pin[P].Saved);
        for (I=0;I<sizeof(Registers)/sizeof(uint32_t);I++)
          CHECK(((uint32_t *)Baseline)[I]==((uint32_t *)Registers)[I]);
      }
    }
  }
}
static void LnaInterfaceMachineCode(void) {
  ULONGLONG A=0,B=0,Old=0,Last=0; unsigned I,S,Before,Calls; PVOID C;
  GPIO_CONNECT_IO_PINS_PARAMETERS Connect={0}; PIN_NUMBER Pin=25;
  Init(); C=LnaInterface.Header.Context; LnaInterface.Header.InterfaceReference(C);
  CHECK(Lna.References==1 && FrameworkReferences==1 && !Stoppable && StopRemoveCalls==1);
  Before=Operations; CHECK(LnaInterface.Acquire(C,25,&A)==STATUS_DEVICE_NOT_READY && !A && Operations==Before);
  Prepare(); CHECK(LnaInterface.Acquire(C,25,&A)==STATUS_DEVICE_NOT_READY && !A);
  CHECK(MgControllerStart(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
  for (I=APC_LEVEL;I<=DISPATCH_LEVEL;I++) {
    CurrentIrql=(KIRQL)I; Before=Operations; A=123;
    CHECK(LnaInterface.Acquire(C,25,&A)==STATUS_INVALID_DEVICE_STATE && A==123);
    CHECK(LnaInterface.Set(C,123,MtkGpsLnaOn)==STATUS_INVALID_DEVICE_STATE);
    CHECK(LnaInterface.Release(C,123)==STATUS_INVALID_DEVICE_STATE && Operations==Before);
  }
  CurrentIrql=DISPATCH_LEVEL;
  LnaInterface.Header.InterfaceReference(C); LnaInterface.Header.InterfaceDereference(C);
  CHECK(Lna.References==1 && FrameworkReferences==1 && StopRemoveCalls==1);
  CurrentIrql=PASSIVE_LEVEL; Before=Operations;
  CHECK(LnaInterface.Acquire(NULL,25,&A)==STATUS_INVALID_PARAMETER && Operations==Before);
  CHECK(LnaInterface.Acquire(C,25,NULL)==STATUS_INVALID_PARAMETER && Operations==Before);
  CHECK(LnaInterface.Acquire(C,24,&A)==STATUS_INVALID_PARAMETER && !A && Operations==Before);
  CHECK(LnaInterface.Set(C,0,MtkGpsLnaOn)==STATUS_INVALID_PARAMETER && Operations==Before);
  CHECK(LnaInterface.Release(C,0)==STATUS_INVALID_PARAMETER && Operations==Before);
  Before=Writes; CHECK(LnaInterface.Acquire(C,25,&A)==STATUS_SUCCESS && A && Writes==Before);
  CHECK(Lna.References==2 && FrameworkReferences==2 && Device.LnaToken[0]==A && StopRemoveCalls==1);
  CHECK(LnaInterface.Acquire(C,25,&B)==STATUS_DEVICE_BUSY && !B);
  Connect.PinNumberTable=&Pin; Connect.PinCount=1; Connect.ConnectMode=ConnectModeOutput;
  CHECK(MgConnectIo(&Device,&Connect)==STATUS_INVALID_PARAMETER && Writes==Before);
  CHECK(LnaInterface.Acquire(C,26,&B)==STATUS_SUCCESS && B>A && Lna.References==3);
  for (I=0;I<2;I++) for (S=0;S<3;S++) {
    CHECK(LnaInterface.Set(C,I?B:A,(MTK_GPS_LNA_STATE)S)==STATUS_SUCCESS);
    CHECK(((Registers[0][0x300/4+3*4]>>((25+I)%8*4))&7U)==(S==MtkGpsLnaDsp?2U:0U));
    CHECK(!!(Registers[0][0]&(1U<<(25+I)))==(S!=MtkGpsLnaDsp));
  }
  Before=Operations; Calls=LockCalls;
  CHECK(LnaInterface.Set(C,A,(MTK_GPS_LNA_STATE)3)==STATUS_INVALID_PARAMETER && Operations==Before && LockCalls==Calls);
  CHECK(LnaInterface.Release(C,MAXULONGLONG)==STATUS_INVALID_PARAMETER && Operations==Before);
  CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_DEVICE_BUSY && !Device.PowerTransition && Operations==Before);
  Device.PowerTransition=1;
  CHECK(LnaInterface.Set(C,A,MtkGpsLnaOff)==STATUS_DEVICE_NOT_READY && Operations==Before);
  CHECK(LnaInterface.Release(C,A)==STATUS_DEVICE_NOT_READY && Operations==Before);
  CHECK(LnaInterface.Acquire(C,25,&Old)==STATUS_DEVICE_NOT_READY && !Old && Operations==Before);
  Device.PowerTransition=0;
  Old=A; CHECK(LnaInterface.Release(C,A)==STATUS_SUCCESS && !Device.LnaToken[0] && Lna.References==2);
  CHECK(LnaInterface.Acquire(C,25,&A)==STATUS_SUCCESS && A>B && A!=Old);
  Before=Operations; CHECK(LnaInterface.Set(C,Old,MtkGpsLnaOn)==STATUS_INVALID_PARAMETER && Operations==Before);
  CHECK(LnaInterface.Release(C,Old)==STATUS_INVALID_PARAMETER && Operations==Before);
  CHECK(LnaInterface.Release(C,A)==STATUS_SUCCESS && LnaInterface.Release(C,B)==STATUS_SUCCESS);
  Last=A; CHECK(Lna.References==1 && !Stoppable && StopRemoveCalls==1);
  CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
  CHECK(StopAndReleaseController()==STATUS_SUCCESS && !Lna.Controller && Unmaps==MG_SPACES);
  Before=Operations;
  CHECK(LnaInterface.Set(C,A,MtkGpsLnaOn)==STATUS_DEVICE_NOT_READY && Operations==Before);
  CHECK(LnaInterface.Release(C,A)==STATUS_DEVICE_NOT_READY && Operations==Before);
  /* Reprepare the same provider object; an old lease must not become valid. */
  Maps=Unmaps=0; ResetRegisters(); Prepare();
  CHECK(MgControllerStart(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
  CHECK(LnaInterface.Acquire(C,25,&A)==STATUS_SUCCESS && A>Last);
  Before=Operations; CHECK(LnaInterface.Set(C,Last,MtkGpsLnaOn)==STATUS_INVALID_PARAMETER && Operations==Before);
  CHECK(LnaInterface.Release(C,A)==STATUS_SUCCESS);
  Lna.NextToken=MAXULONGLONG; Before=Operations;
  CHECK(LnaInterface.Acquire(C,25,&A)==STATUS_INSUFFICIENT_RESOURCES && !A && Operations==Before);
  CHECK(StopAndReleaseController()==STATUS_SUCCESS);
  LnaInterface.Header.InterfaceDereference(C);
  CHECK(!FrameworkReferences && !Lna.References && Stoppable && StopRemoveCalls==2);

  Start(); C=LnaInterface.Header.Context; LnaInterface.Header.InterfaceReference(C);
  Registers[0][0]|=1U<<25;
  CHECK(LnaInterface.Acquire(C,25,&A)==STATUS_SUCCESS);
  DropGpioWrite=1;
  CHECK(LnaInterface.Set(C,A,MtkGpsLnaOff)==STATUS_IO_DEVICE_ERROR && !Device.Core.Active && Device.LnaToken[0]==A);
  Before=Operations;
  CHECK(LnaInterface.Set(C,A,MtkGpsLnaOn)==STATUS_DEVICE_NOT_READY && Operations==Before);
  DropGpioWrite=1;
  CHECK(LnaInterface.Release(C,A)==STATUS_IO_DEVICE_ERROR && Device.LnaToken[0]==A && Device.Core.Pin[25].Saved && Lna.References==2);
  CHECK(LnaInterface.Release(C,A)==STATUS_SUCCESS && !Device.LnaToken[0] && !Device.Core.Pin[25].Saved && Lna.References==1);
  CHECK(StopAndReleaseController()==STATUS_SUCCESS);
  LnaInterface.Header.InterfaceDereference(C);
  CHECK(!FrameworkReferences && Stoppable && StopRemoveCalls==2);
  /* A surprise-removal IRP drains calls, revokes both leases and is forwarded
   * exactly once. Stop/release must not touch powered-off or absent hardware. */
  for (I=0;I<3;I++) {
    if (!I) Init(); else if (I==1) { Init(); Prepare(); } else Start();
    C=LnaInterface.Header.Context; LnaInterface.Header.InterfaceReference(C);
    if (I==2) {
      CHECK(LnaInterface.Acquire(C,25,&A)==STATUS_SUCCESS && LnaInterface.Acquire(C,26,&B)==STATUS_SUCCESS);
      CHECK(LnaInterface.Set(C,A,MtkGpsLnaDsp)==STATUS_SUCCESS);
      Device.ActiveDirql=1; DrainDirqlOnStall=1;
    } else A=1;
    Before=Operations; RemoveUnexpectedly();
    CHECK(Stalls==(I==2) && !DrainDirqlOnStall);
    CHECK(Lna.SurpriseRemoved && Lna.References==1 && FrameworkReferences==1 && Operations==Before && DispatchCalls==1);
    CHECK(LnaInterface.Set(C,A,MtkGpsLnaOn)==STATUS_DEVICE_REMOVED && Operations==Before);
    CHECK(LnaInterface.Release(C,A)==STATUS_DEVICE_REMOVED && Operations==Before);
    CHECK(LnaInterface.Acquire(C,25,&A)==STATUS_DEVICE_REMOVED && !A && Operations==Before);
    RemoveUnexpectedly(); CHECK(Lna.References==1 && Operations==Before && DispatchCalls==2);
    if (I) {
      GPIO_ENABLE_INTERRUPT_PARAMETERS Enable={0}; GPIO_DISABLE_INTERRUPT_PARAMETERS Disable={0};
      GPIO_DISCONNECT_IO_PINS_PARAMETERS Disconnect={0};
      Connect.PinNumberTable=&Pin; Connect.PinCount=1; Connect.ConnectMode=ConnectModeOutput;
      Disconnect.PinNumberTable=&Pin; Disconnect.PinCount=1;
      CHECK(MgConnectIo(&Device,&Connect)==STATUS_DEVICE_REMOVED && Operations==Before);
      CHECK(MgDisconnectIo(&Device,&Disconnect)==STATUS_DEVICE_REMOVED && Operations==Before);
      Enable.InterruptMode=LevelSensitive; Enable.Polarity=InterruptActiveHigh;
      CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_DEVICE_REMOVED && Operations==Before);
      CHECK(MgInterruptDisable(&Device,&Disable)==STATUS_DEVICE_REMOVED && Operations==Before);
      CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS && Operations==Before);
      CHECK(MgControllerStart(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_DEVICE_REMOVED && Operations==Before);
      CHECK(StopAndReleaseController()==STATUS_SUCCESS && Operations==Before && Unmaps==MG_SPACES);
    }
    CHECK(MgPrepare((WDFDEVICE)7,&Device,(WDFCMRESLIST)1,(WDFCMRESLIST)2)==STATUS_DEVICE_REMOVED && Operations==Before);
    LnaInterface.Header.InterfaceDereference(C);
    CHECK(!FrameworkReferences && !Lna.References && Stoppable && StopRemoveCalls==2);
  }
  Init(); RemoveUnexpectedly(); Before=Operations;
  CHECK(MgPrepare((WDFDEVICE)7,&Device,(WDFCMRESLIST)1,(WDFCMRESLIST)2)==STATUS_DEVICE_REMOVED);
  CHECK((MutexInitialized&2U) && ReleaseController()==STATUS_SUCCESS);
  CHECK(!Maps && !Unmaps && Operations==Before && !ConfigurationMutexDepth);
  for (I=1;I<=8;I++) {
    Init(); AddFailure=I; QueryCalls=DeviceInterfaceCalls=0;
    CHECK(MgDeviceAdd((WDFDRIVER)9,(PWDFDEVICE_INIT)10)==(I==6?STATUS_INVALID_DEVICE_REQUEST:STATUS_INSUFFICIENT_RESOURCES));
    CHECK(QueryCalls==(I>=7?2U:I==5?1U:0U) && DeviceInterfaceCalls==(I==8) &&
          Operations==0 && !FrameworkReferences);
  }
  Init(); Prepare(); AcpiVariant=9;
  CHECK(MgControllerStart(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS && Device.SeenStart && Device.Core.Active);
  CHECK(StopAndReleaseController()==STATUS_SUCCESS && Unmaps==MG_SPACES);
}
static void ConnPinsMachineCode(void) {
  static const struct {
    MTK_CONN_PIN_SPACE Space;
    ULONG Offset,Mask,Target;
  } Fields[] = {
    {MtkConnPinsGpio,0x380,0x000f0000,0},
    {MtkConnPinsGpio,0x020,0x00000010,0x10},
    {MtkConnPinsGpio,0x120,0x00000010,0x10},
    {MtkConnPinsGpio,0x460,0x77777770,0x11111110},
    {MtkConnPinsGpio,0x470,0x00000077,0x11},
    {MtkConnPinsIoCfg,0x010,0x3ffffff8,0x9048},
    {MtkConnPinsGpio,0x050,0x02000000,0x02000000},
    {MtkConnPinsGpio,0x150,0x02000000,0x02000000}
  };
  MTK_CONN_PINS_INTERFACE *P=&ConnPinsInterface;
  ULONGLONG Token=0,Other=0;
  ULONG Baseline[sizeof(Fields)/sizeof(Fields[0])],Value;
  PIN_NUMBER Pins[2]={16,17};
  GPIO_CONNECT_IO_PINS_PARAMETERS Connect={0};
  GPIO_DISCONNECT_IO_PINS_PARAMETERS Disconnect={0};
  GPIO_ENABLE_INTERRUPT_PARAMETERS Interrupt={0};
  unsigned Before;
  PVOID Context;

  Init();
  Context=P->Header.Context;
  P->Header.InterfaceReference(Context);
  CHECK(P->Acquire(Context,&Token)==STATUS_DEVICE_NOT_READY && !Token && !Operations);
  Prepare();
  CHECK(P->Acquire(Context,&Token)==STATUS_DEVICE_NOT_READY && !Token && !Operations);
  CHECK(MgControllerStart(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
  CurrentIrql=DISPATCH_LEVEL;
  Before=Operations;
  CHECK(P->Acquire(Context,&Token)==STATUS_INVALID_DEVICE_STATE && Operations==Before);
  CurrentIrql=PASSIVE_LEVEL;

  Connect.BankId=Disconnect.BankId=5;
  Connect.PinNumberTable=Disconnect.PinNumberTable=&Pins[1];
  Connect.PinCount=Disconnect.PinCount=1;
  Connect.ConnectMode=ConnectModeOutput;
  CHECK(MgConnectIo(&Device,&Connect)==STATUS_SUCCESS);
  Before=Operations;
  CHECK(P->Acquire(Context,&Token)==STATUS_DEVICE_BUSY && !Token && Operations==Before);
  CHECK(MgDisconnectIo(&Device,&Disconnect)==STATUS_SUCCESS);
  for (unsigned I=0;I<sizeof(Fields)/sizeof(Fields[0]);I++) {
    Baseline[I]=Registers[Fields[I].Space][Fields[I].Offset/4];
  }
  /* Exercise restoring a nonzero full GPIO68 mux field. */
  Baseline[0]|=0xb0000U;
  Registers[MtkConnPinsGpio][0x380/4]=Baseline[0];
  Before=Writes;
  CHECK(P->Acquire(Context,&Token)==STATUS_SUCCESS && Token && Writes==Before &&
        FrameworkReferences==2 && !Stoppable);
  Before=Operations;
  CHECK(P->Acquire(Context,&Other)==STATUS_DEVICE_BUSY && !Other && Operations==Before);
  Connect.PinNumberTable=Pins; Connect.PinCount=2;
  CHECK(MgConnectIo(&Device,&Connect)==STATUS_DEVICE_BUSY && Operations==Before &&
        !Device.Core.Pin[176].Saved && !Device.Core.Pin[177].Saved);
  Interrupt.BankId=2; Interrupt.PinNumber=4;
  Interrupt.InterruptMode=LevelSensitive; Interrupt.Polarity=InterruptActiveHigh;
  CHECK(MgInterruptEnable(&Device,&Interrupt)==STATUS_DEVICE_BUSY && Operations==Before);
  CHECK(P->Read(Context,Token,MtkConnPinsGpio,0x54,&Value)==STATUS_INVALID_PARAMETER);
  CHECK(P->Write(Context,Token,MtkConnPinsGpio,0x54,1)==STATUS_INVALID_PARAMETER);
  CHECK(P->Write(Context,Token,MtkConnPinsGpio,0x470,0x71)==STATUS_INVALID_PARAMETER);
  CHECK(P->Write(Context,Token,MtkConnPinsIoCfg,0x10,0x40009048)==STATUS_INVALID_PARAMETER);
  CHECK(P->Write(Context,Token,(MTK_CONN_PIN_SPACE)1,0x10,0)==STATUS_INVALID_PARAMETER &&
        Operations==Before);

  for (unsigned I=0;I<sizeof(Fields)/sizeof(Fields[0]);I++) {
    CHECK(P->Read(Context,Token,Fields[I].Space,Fields[I].Offset,&Value)==STATUS_SUCCESS &&
          Value==(Baseline[I]&Fields[I].Mask));
    ULONG Offset=Fields[I].Offset+(I>=6?4U:0U); /* Stock GPIO185 SET aliases. */
    CHECK(P->Write(Context,Token,Fields[I].Space,Offset,Fields[I].Target)==STATUS_SUCCESS);
    CHECK(Registers[Fields[I].Space][Fields[I].Offset/4]==
          ((Baseline[I]&~Fields[I].Mask)|Fields[I].Target));
  }
  DropGpioWrite=1;
  CHECK(P->Write(Context,Token,MtkConnPinsGpio,0x470,1)==STATUS_IO_DEVICE_ERROR &&
        Device.ConnPinsToken==Token && Device.Core.Active && FrameworkReferences==2);
  CHECK(P->Write(Context,Token,MtkConnPinsGpio,0x470,1)==STATUS_SUCCESS);
  CHECK(P->Release(Context,Token)==STATUS_DEVICE_BUSY && Device.ConnPinsToken==Token);
  Before=Operations;
  CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_DEVICE_BUSY &&
        Operations==Before && !Device.PowerTransition);
  CHECK(MgControllerStop(&Device,TRUE,WdfPowerDeviceD3)==STATUS_DEVICE_BUSY &&
        Operations==Before && !Device.PowerTransition);

  /* A neighboring pin may change during the lease. Restore only owned bits. */
  Baseline[3]^=3;
  Registers[MtkConnPinsGpio][0x460/4]^=3;
  for (unsigned I=sizeof(Fields)/sizeof(Fields[0]);I--;) {
    ULONG Saved=Baseline[I]&Fields[I].Mask;
    ULONG Offset=Fields[I].Offset+(I>=6?(Saved?4U:8U):0U);
    ULONG ValueToWrite=I>=6?Fields[I].Mask:Saved;
    CHECK(P->Write(Context,Token,Fields[I].Space,Offset,ValueToWrite)==STATUS_SUCCESS);
    CHECK(Registers[Fields[I].Space][Fields[I].Offset/4]==Baseline[I]);
  }
  CHECK(P->Release(Context,Token)==STATUS_SUCCESS && FrameworkReferences==1);
  Connect.PinNumberTable=&Pins[1]; Connect.PinCount=1;
  CHECK(MgConnectIo(&Device,&Connect)==STATUS_SUCCESS);
  CHECK(MgDisconnectIo(&Device,&Disconnect)==STATUS_SUCCESS);
  CHECK(P->Acquire(Context,&Other)==STATUS_SUCCESS && Other!=Token);
  Before=Operations;
  CHECK(P->Release(Context,Token)==STATUS_INVALID_PARAMETER && Operations==Before);
  CHECK(P->Write(Context,Other,MtkConnPinsGpio,0x470,0x11)==STATUS_SUCCESS);
  Before=Operations; HardwareAccessible=FALSE;
  RemoveUnexpectedly();
  CHECK(P->Read(Context,Other,MtkConnPinsGpio,0x470,&Value)==STATUS_DEVICE_REMOVED);
  CHECK(P->Release(Context,Other)==STATUS_DEVICE_REMOVED && !Device.ConnPinsToken &&
        Operations==Before && FrameworkReferences==1);
  CHECK(ReleaseController()==STATUS_SUCCESS && Unmaps==MG_SPACES && Operations==Before);
  P->Header.InterfaceDereference(Context);
  CHECK(!FrameworkReferences && Stoppable && StopRemoveCalls==2);

  Start();
  Context=P->Header.Context;
  P->Header.InterfaceReference(Context);
  CHECK(P->Acquire(Context,&Token)==STATUS_SUCCESS);
  CHECK(P->Write(Context,Token,MtkConnPinsGpio,0x470,0x11)==STATUS_SUCCESS);
  Before=Operations;
  CHECK(ReleaseController()==STATUS_IO_DEVICE_ERROR && Device.ReleaseFaulted &&
        Device.Core.ConnPinsOwned && !Device.ConnPinsToken && FrameworkReferences==1 &&
        Operations==Before && Unmaps==MG_SPACES);
  CHECK(P->Write(Context,Token,MtkConnPinsGpio,0x470,0)==STATUS_DEVICE_NOT_READY &&
        Operations==Before);
  P->Header.InterfaceDereference(Context);
  CHECK(!FrameworkReferences && Stoppable && StopRemoveCalls==2);
}
static void VirtualPmicRouteMachineCode(void) {
  GPIO_ENABLE_INTERRUPT_PARAMETERS Enable={0};
  GPIO_DISABLE_INTERRUPT_PARAMETERS Disable={0};
  GPIO_MASK_INTERRUPT_PARAMETERS Mask={0};
  GPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS Active={0};
  GPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS Enabled={0};
  GPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS Clear={0};
  GPIO_RECONFIGURE_INTERRUPTS_PARAMETERS Reconfigure={0};
  GPIO_CONNECT_IO_PINS_PARAMETERS Connect={0};
  GPIO_READ_PINS_MASK_PARAMETERS Read={0};
  GPIO_WRITE_PINS_MASK_PARAMETERS Write={0};
  PIN_NUMBER VirtualPin=MG_PMIC_IRQ_NUMBER;
  ULONG64 Values=~0ULL; unsigned Before; uint32_t Logical=1U<<MG_PMIC_IRQ_NUMBER;
  Start();
  Enable.BankId=MG_PMIC_IRQ_BANK; Enable.PinNumber=MG_PMIC_IRQ_NUMBER;
  Enable.InterruptMode=LevelSensitive; Enable.Polarity=InterruptActiveHigh;
  GpioOperations=0; Before=Writes;
  Enable.Polarity=InterruptActiveLow;
  CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_NOT_SUPPORTED && Writes==Before && GpioOperations==0);
  Enable.Polarity=InterruptActiveHigh; Enable.InterruptMode=Latched;
  CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_NOT_SUPPORTED && Writes==Before && GpioOperations==0);
  Enable.InterruptMode=LevelSensitive;
  CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_SUCCESS && Device.Core.PmicInterrupt.Saved &&
        GpioOperations==0 && Device.Core.Pin[137].Saved==0 && Device.Core.Owned[4]==0);
  CHECK(Writes>Before && (Registers[1][0x80/4+MG_PMIC_EINT_BANK]&MG_PMIC_EINT_BIT)==0);
  Registers[1][MG_PMIC_EINT_BANK]|=MG_PMIC_EINT_BIT;
  Active.BankId=MG_PMIC_IRQ_BANK; Active.EnabledMask=(ULONG64)Logical;
  CHECK(MgActiveInterrupts(&Device,&Active)==STATUS_SUCCESS && Active.ActiveMask==Logical && GpioOperations==0);
  Enabled.BankId=MG_PMIC_IRQ_BANK;
  CHECK(MgEnabledInterrupts(&Device,&Enabled)==STATUS_SUCCESS && Enabled.EnabledMask==Logical);
  Mask.BankId=MG_PMIC_IRQ_BANK; Mask.PinMask=Logical;
  CHECK(MgInterruptMask(&Device,&Mask)==STATUS_SUCCESS && Mask.FailedMask==0 &&
        (Registers[1][0x80/4+MG_PMIC_EINT_BANK]&MG_PMIC_EINT_BIT));
  CHECK(MgActiveInterrupts(&Device,&Active)==STATUS_SUCCESS && Active.ActiveMask==0);
  CHECK(MgInterruptUnmask(&Device,&Enable)==STATUS_SUCCESS &&
        !(Registers[1][0x80/4+MG_PMIC_EINT_BANK]&MG_PMIC_EINT_BIT));
  Clear.BankId=MG_PMIC_IRQ_BANK; Clear.ClearActiveMask=Logical;
  CHECK(MgClearInterrupts(&Device,&Clear)==STATUS_SUCCESS && Clear.FailedClearMask==0 &&
        !(Registers[1][MG_PMIC_EINT_BANK]&MG_PMIC_EINT_BIT));
  Reconfigure.BankId=MG_PMIC_IRQ_BANK; Reconfigure.PinNumber=MG_PMIC_IRQ_NUMBER;
  Reconfigure.InterruptMode=LevelSensitive; Reconfigure.Polarity=InterruptActiveHigh;
  Before=Writes; CHECK(MgReconfigure(&Device,&Reconfigure)==STATUS_SUCCESS && Writes==Before);
  Reconfigure.Polarity=InterruptActiveLow;
  CHECK(MgReconfigure(&Device,&Reconfigure)==STATUS_NOT_SUPPORTED && Writes==Before);

  Connect.BankId=MG_PMIC_IRQ_BANK; Connect.PinNumberTable=&VirtualPin; Connect.PinCount=1;
  Connect.ConnectMode=ConnectModeInput; Before=Writes;
  CHECK(MgConnectIo(&Device,&Connect)==STATUS_INVALID_PARAMETER && Writes==Before && GpioOperations==0);
  Read.BankId=MG_PMIC_IRQ_BANK; Read.PinValues=&Values;
  Registers[0][0x200/4+MG_PMIC_IRQ_BANK]|=Logical;
  CHECK(MgReadPins(&Device,&Read)==STATUS_SUCCESS && Values==0 && GpioOperations==1);
  Write.BankId=MG_PMIC_IRQ_BANK; Write.SetMask=(ULONG64)Logical; Before=Writes;
  CHECK(MgWritePins(&Device,&Write)==STATUS_INVALID_PARAMETER && Writes==Before);

  /* Physical GPIO137 is not the PMIC route: its EINT trigger remains
   * unsupported and no mux/direction/data register is touched. */
  Enable.BankId=4; Enable.PinNumber=9; Before=Writes; GpioOperations=0;
  CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_NOT_SUPPORTED && Writes==Before &&
        GpioOperations==0 && !Device.Core.Pin[137].Saved);
  Disable.BankId=MG_PMIC_IRQ_BANK; Disable.PinNumber=MG_PMIC_IRQ_NUMBER;
  GpioOperations=0;
  CHECK(MgInterruptDisable(&Device,&Disable)==STATUS_SUCCESS && !Device.Core.PmicInterrupt.Saved &&
        GpioOperations==0 && !Device.Core.Pin[137].Saved);
  CHECK(Registers[1][0x80/4+MG_PMIC_EINT_BANK]&MG_PMIC_EINT_BIT);
  CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
  CHECK(StopAndReleaseController()==STATUS_SUCCESS && Unmaps==MG_SPACES);

  Start(); Enable.BankId=MG_PMIC_IRQ_BANK; Enable.PinNumber=MG_PMIC_IRQ_NUMBER;
  Enable.InterruptMode=LevelSensitive; Enable.Polarity=InterruptActiveHigh;
  CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_SUCCESS);
  DropEintWrite=1;
  CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_IO_DEVICE_ERROR &&
        Device.Core.PmicInterrupt.Saved && !Device.Core.Active && Device.SeenStart);
  CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS &&
        !Device.Core.PmicInterrupt.Saved && !Device.SeenStart &&
        (Registers[1][0x80/4+MG_PMIC_EINT_BANK]&MG_PMIC_EINT_BIT));
  CHECK(StopAndReleaseController()==STATUS_SUCCESS && Unmaps==MG_SPACES);
}
int TestMain(void) {
  unsigned I,Before; ULONG64 Values;
  CLIENT_CONTROLLER_BASIC_INFORMATION Info;
  GPIO_ENABLE_INTERRUPT_PARAMETERS Enable={0}; GPIO_DISABLE_INTERRUPT_PARAMETERS Disable={0};
  GPIO_MASK_INTERRUPT_PARAMETERS Mask={0}; GPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS Active={0};
  GPIO_QUERY_ENABLED_INTERRUPTS_PARAMETERS Enabled={0}; GPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS Clear={0};
  GPIO_RECONFIGURE_INTERRUPTS_PARAMETERS Reconfigure={0};
  GPIO_CONNECT_IO_PINS_PARAMETERS Connect={0}; GPIO_DISCONNECT_IO_PINS_PARAMETERS Disconnect={0};
  GPIO_READ_PINS_MASK_PARAMETERS Read={0}; GPIO_WRITE_PINS_MASK_PARAMETERS Write={0}; PIN_NUMBER Pins[2]={24,25};
  Failure=Checks=0;
  {
    UCHAR Storage[sizeof(ACPI_METHOD_ARGUMENT)+sizeof(ULONGLONG)]={0};
    PACPI_METHOD_ARGUMENT A=(PACPI_METHOD_ARGUMENT)Storage;
    ULONG Small=0x89abcdef,Parsed;ULONGLONG Wide=MAXULONG;
    A->Type=ACPI_METHOD_ARGUMENT_INTEGER;A->DataLength=sizeof(Small);for(ULONG J=0;J<sizeof(Small);++J)A->Data[J]=((const UCHAR *)&Small)[J];
    CHECK(MgFirmwareInteger32(A,&Parsed)&&Parsed==Small);
    A->DataLength=sizeof(Wide);for(ULONG J=0;J<sizeof(Wide);++J)A->Data[J]=((const UCHAR *)&Wide)[J];
    CHECK(MgFirmwareInteger32(A,&Parsed)&&Parsed==MAXULONG);
    Wide=(ULONGLONG)MAXULONG+1;for(ULONG J=0;J<sizeof(Wide);++J)A->Data[J]=((const UCHAR *)&Wide)[J];
    CHECK(!MgFirmwareInteger32(A,&Parsed));A->DataLength=6;CHECK(!MgFirmwareInteger32(A,&Parsed));
    A->Type=ACPI_METHOD_ARGUMENT_BUFFER;A->DataLength=sizeof(Small);CHECK(!MgFirmwareInteger32(A,&Parsed));
  }
  Table[WdfDeviceInitAssignWdmIrpPreprocessCallbackTableIndex]=(WDFFUNC)AssignPreprocess;
  Table[WdfDeviceWdmDispatchPreprocessedIrpTableIndex]=(WDFFUNC)DispatchPreprocessed;
  Table[WdfDeviceCreateTableIndex]=(WDFFUNC)DeviceCreate;
  Table[WdfObjectAllocateContextTableIndex]=(WDFFUNC)AllocateContext;
  Table[WdfObjectGetTypedContextWorkerTableIndex]=(WDFFUNC)GetContext;
  Table[WdfDeviceAddQueryInterfaceTableIndex]=(WDFFUNC)AddQuery;
  Table[WdfDeviceCreateDeviceInterfaceTableIndex]=(WDFFUNC)AddDeviceInterface;
  Table[WdfObjectReferenceActualTableIndex]=(WDFFUNC)ObjectReference;
  Table[WdfObjectDereferenceActualTableIndex]=(WDFFUNC)ObjectDereference;
  Table[WdfDeviceSetStaticStopRemoveTableIndex]=(WDFFUNC)StopRemove;
  GpioClxExportedInterfaces[AddDevicePreDeviceCreateIndex]=(PGPIO_CLX_EXPORTED_INTERFACES)AddPre;
  GpioClxExportedInterfaces[AddDevicePostDeviceCreateIndex]=(PGPIO_CLX_EXPORTED_INTERFACES)AddPost;
  Table[WdfCmResourceListGetCountTableIndex]=(WDFFUNC)ResourceCount;
  Table[WdfCmResourceListGetDescriptorTableIndex]=(WDFFUNC)Resource;
  Table[WdfDeviceGetIoTargetTableIndex]=(WDFFUNC)IoTarget;
  Table[WdfIoTargetSendIoctlSynchronouslyTableIndex]=(WDFFUNC)Ioctl;
  GpioClxExportedInterfaces[AcquireInterruptLockIndex]=(PGPIO_CLX_EXPORTED_INTERFACES)Acquire;
  GpioClxExportedInterfaces[ReleaseInterruptLockIndex]=(PGPIO_CLX_EXPORTED_INTERFACES)Release;
  CHECK(MgInformation(NULL,&Info)==STATUS_SUCCESS && Info.TotalPins==MG_TOTAL_PINS &&
        Info.NumberOfPinsPerBank==32 && MG_BANKS==6 && MG_PINS==188);
  CHECK(Info.Flags.MemoryMappedController && Info.Flags.FormatIoRequestsAsMasks && Info.Flags.EmulateActiveBoth && Info.Flags.EmulateDebouncing);
  CHECK(!Info.Flags.DeviceIdlePowerMgmtSupported && !Info.Flags.BankIdlePowerMgmtSupported);
  ReleaseLifetimeMachineCode();
  for (I=0;I<7;I++) {
    Init();
    switch (I) {
      case 0: RawResources[0].u.Memory.Start.QuadPart++; break;
      case 1: TranslatedResources[1].u.Memory.Length--; break;
      case 2: RawResources[MG_SPACES].u.Interrupt.Vector++; break;
      case 3: RawResources[MG_SPACES].Flags=CM_RESOURCE_INTERRUPT_LATCHED; break;
      case 4: TranslatedResources[MG_SPACES].ShareDisposition=CmResourceShareDeviceExclusive; break;
      case 5: RawCount--; break;
      case 6: TranslatedResources[MG_SPACES+MG_BANKS-1].Type=CmResourceTypeMemory; break;
    }
    CHECK(MgPrepare((WDFDEVICE)7,&Device,(WDFCMRESLIST)1,(WDFCMRESLIST)2)==STATUS_DEVICE_CONFIGURATION_ERROR && Maps==0 && Writes==0);
  }
  Init(); MapFailure=2; CHECK(MgPrepare((WDFDEVICE)7,&Device,(WDFCMRESLIST)1,(WDFCMRESLIST)2)==STATUS_INSUFFICIENT_RESOURCES);
  CHECK(Maps==2 && Unmaps==1 && !Device.Registers[0] && !Device.Registers[1]);
  for (I=1;I<=MG_SPACES;I++) {
    Init(); MapFailure=I;
    CHECK(MgPrepare((WDFDEVICE)7,&Device,(WDFCMRESLIST)1,(WDFCMRESLIST)2)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(Maps==I && Unmaps==I-1 && Writes==0);
  }
  for (I=1;I<=8;I++) {
    Init(); Prepare(); AcpiVariant=I;
    CHECK(!NT_SUCCESS(MgControllerStart(&Device,FALSE,WdfPowerDeviceD3Final)) && !Device.Core.Active && !Device.SeenStart && Writes==0);
    CHECK(StopAndReleaseController()==STATUS_SUCCESS && Unmaps==MG_SPACES);
  }
  Init(); Prepare(); AcpiStatus=STATUS_IO_TIMEOUT;
  CHECK(MgControllerStart(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_IO_TIMEOUT && Writes==0);
  Start(); CHECK(Ioctls==1 && Device.SeenStart && Device.Core.Active);
  Before=Writes; Mask.BankId=5; Clear.BankId=5;
  CHECK(MgInterruptMask(&Device,&Mask)==STATUS_SUCCESS && MgClearInterrupts(&Device,&Clear)==STATUS_SUCCESS && Writes==Before);
  Mask.BankId=Clear.BankId=0;
  Enable.PinNumber=9; Enable.InterruptMode=LevelSensitive; Enable.Polarity=InterruptActiveLow;
  CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_SUCCESS && LockCalls==1 && LockDepth==0);
  CHECK(!(Registers[1][0x80/4]&(1U<<9)) && (Registers[1][0x140/4]&(1U<<9)) && !(Registers[1][0x300/4]&(1U<<9)));
  Registers[1][0]=(1U<<9)|(1U<<10); Active.EnabledMask=UINT32_MAX;
  CHECK(MgActiveInterrupts(&Device,&Active)==STATUS_SUCCESS && Active.ActiveMask==(1U<<9));
  CHECK(MgEnabledInterrupts(&Device,&Enabled)==STATUS_SUCCESS && Enabled.EnabledMask==(1U<<9));
  Mask.PinMask=1U<<9; CHECK(MgInterruptMask(&Device,&Mask)==STATUS_SUCCESS && !Mask.FailedMask);
  CHECK(MgActiveInterrupts(&Device,&Active)==STATUS_SUCCESS && Active.ActiveMask==0);
  CHECK(MgInterruptUnmask(&Device,&Enable)==STATUS_SUCCESS);
  Clear.ClearActiveMask=1U<<9; CHECK(MgClearInterrupts(&Device,&Clear)==STATUS_SUCCESS && !Clear.FailedClearMask && Registers[1][0]==(1U<<10));
  Reconfigure.PinNumber=9; Reconfigure.InterruptMode=Latched; Reconfigure.Polarity=InterruptActiveHigh;
  CHECK(MgReconfigure(&Device,&Reconfigure)==STATUS_SUCCESS && LockCalls==1);
  CHECK(!(Registers[1][0x140/4]&(1U<<9)) && (Registers[1][0x300/4]&(1U<<9)));
  Before=Writes; Enable.BankId=3; Enable.PinNumber=30;
  CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_NOT_SUPPORTED && Writes==Before);
  Enable.BankId=0; Enable.PinNumber=9; Enable.Polarity=InterruptActiveBoth;
  CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_NOT_SUPPORTED && Writes==Before);
  Mask.PinMask=1ULL<<32; CHECK(MgInterruptMask(&Device,&Mask)==STATUS_INVALID_PARAMETER && Mask.FailedMask==(1ULL<<32) && Writes==Before);
  Connect.BankId=4; Connect.PinNumberTable=Pins; Connect.PinCount=2; Connect.ConnectMode=ConnectModeOutput;
  CHECK(MgConnectIo(&Device,&Connect)==STATUS_SUCCESS && Device.Core.Pin[152].Kind==2 && Device.Core.Pin[153].Kind==2);
  Write.BankId=4; Write.SetMask=1U<<24; Write.ClearMask=1U<<25;
  CHECK(MgWritePins(&Device,&Write)==STATUS_SUCCESS);
  Read.BankId=4; Read.PinValues=&Values; Read.Flags.WriteConfiguredPins=1;
  CHECK(MgReadPins(&Device,&Read)==STATUS_SUCCESS && Values==(1U<<24));
  Before=Writes; Device.PowerTransition=1;
  CHECK(MgReadPins(&Device,&Read)==STATUS_DEVICE_NOT_READY && MgWritePins(&Device,&Write)==STATUS_DEVICE_NOT_READY && Writes==Before);
  Device.PowerTransition=0;
  Disconnect.BankId=4; Disconnect.PinNumberTable=Pins; Disconnect.PinCount=2;
  CHECK(MgDisconnectIo(&Device,&Disconnect)==STATUS_SUCCESS && !Device.Core.Pin[152].Saved);
  Before=Writes; Pins[1]=24;
  CHECK(MgConnectIo(&Device,&Connect)==STATUS_INVALID_PARAMETER && Writes==Before);
  Pins[1]=25; Connect.DriveStrength=100;
  CHECK(MgConnectIo(&Device,&Connect)==STATUS_NOT_SUPPORTED && Writes==Before);
  Disable.PinNumber=9; CHECK(MgInterruptDisable(&Device,&Disable)==STATUS_SUCCESS && !Device.Core.Owned[0]);
  CHECK(Registers[1][0x140/4]==0x55AA55AAU && Registers[1][0x300/4]==0xA55AA55AU);
  CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS && !Device.Core.Active);
  Before=Writes; CHECK(MgControllerStart(&Device,TRUE,WdfPowerDeviceD3)==STATUS_NOT_SUPPORTED && Writes==Before);
  CHECK(MgReadPins(&Device,&Read)==STATUS_DEVICE_NOT_READY && MgWritePins(&Device,&Write)==STATUS_DEVICE_NOT_READY);
  CHECK(StopAndReleaseController()==STATUS_SUCCESS && Unmaps==MG_SPACES && LockDepth==0 && FastMutexDepth==0);
  {
    static const ULONG Strengths[]={1,12,13,25,50,100,MAXULONG};
    ULONG I; unsigned BaselineEh;
    Start(); memset(&Connect,0,sizeof(Connect));
    Pins[0]=15; Connect.BankId=0; Connect.PinNumberTable=Pins; Connect.PinCount=1;
    Connect.ConnectMode=ConnectModeOutput;
    BaselineEh=TestRawField(&MgPads[15].Eh);
    for (I=0;I<sizeof(Strengths)/sizeof(Strengths[0]);I++) {
      Connect.DriveStrength=Strengths[I];
      Before=Writes;
      CHECK(MgConnectIo(&Device,&Connect)==STATUS_NOT_SUPPORTED && Writes==Before &&
            !Device.Core.Pin[15].Saved && TestRawField(&MgPads[15].Eh)==BaselineEh);
    }
    Pins[0]=15; Pins[1]=25; Connect.PinCount=2; Connect.DriveStrength=25; Before=Writes;
    CHECK(MgConnectIo(&Device,&Connect)==STATUS_NOT_SUPPORTED && Writes==Before && !Device.Core.Pin[15].Saved);
    Pins[0]=2; Connect.BankId=4; Connect.PinCount=1; Connect.DriveStrength=25; Before=Writes;
    CHECK(MgConnectIo(&Device,&Connect)==STATUS_NOT_SUPPORTED && Writes==Before && !Device.Core.Pin[130].Saved);
    CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS &&
          TestRawField(&MgPads[15].Eh)==BaselineEh && !Device.Core.Pin[15].Saved);
    CHECK(StopAndReleaseController()==STATUS_SUCCESS);
  }
  Start(); memset(&Connect,0,sizeof(Connect)); Pins[0]=6;
  Connect.BankId=0; Connect.PinNumberTable=Pins; Connect.PinCount=1;
  Connect.ConnectMode=ConnectModeInput; Connect.PullConfiguration=1;
  CHECK(MgConnectIo(&Device,&Connect)==STATUS_SUCCESS);
  memset(&Enable,0,sizeof(Enable)); Enable.PinNumber=0;
  Enable.InterruptMode=LevelSensitive; Enable.Polarity=InterruptActiveHigh;
  CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_SUCCESS);
  {
    unsigned IoctlsBefore=Ioctls,GpioBefore;
    Enable.BankId=MG_PMIC_IRQ_BANK; Enable.PinNumber=MG_PMIC_IRQ_NUMBER;
    CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_SUCCESS);
    CHECK(MgControllerStop(&Device,TRUE,WdfPowerDeviceD3)==STATUS_SUCCESS &&
          Device.Core.Suspended && !Device.Core.Active && Device.SeenStart &&
          Device.PowerTransition && Ioctls==IoctlsBefore);
    CHECK((Registers[1][0x80/4]&1) && Device.Core.Pin[0].ContextValid &&
          (Registers[1][0x80/4+MG_PMIC_EINT_BANK]&MG_PMIC_EINT_BIT));
    memset(&Read,0,sizeof(Read)); Read.PinValues=&Values;
    CHECK(MgReadPins(&Device,&Read)==STATUS_DEVICE_NOT_READY);
    Before=Writes;
    AcpiStatus=STATUS_IO_TIMEOUT;
    CHECK(MgControllerStart(&Device,TRUE,WdfPowerDeviceD3)==STATUS_IO_TIMEOUT);
    AcpiStatus=STATUS_SUCCESS;
    Registers[1][0x400/4]^=1U;
    CHECK(MgControllerStart(&Device,TRUE,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY);
    Registers[1][0x400/4]^=1U;
    Registers[0][0]^=1U<<6;
    CHECK(MgControllerStart(&Device,TRUE,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY);
    Registers[0][0]^=1U<<6;
    Registers[1][0x140/4+MG_PMIC_EINT_BANK]^=MG_PMIC_EINT_BIT;
    CHECK(MgControllerStart(&Device,TRUE,WdfPowerDeviceD3)==STATUS_DEVICE_NOT_READY);
    Registers[1][0x140/4+MG_PMIC_EINT_BANK]^=MG_PMIC_EINT_BIT;
    CHECK(Writes==Before && Device.Core.Suspended && Device.Core.Pin[6].ContextValid);
    Registers[1][0]|=1U;
    Registers[1][MG_PMIC_EINT_BANK]|=MG_PMIC_EINT_BIT;
    GpioBefore=GpioOperations;
    DropEintWrite=1;
    CHECK(MgControllerStart(&Device,TRUE,WdfPowerDeviceD3)==STATUS_IO_DEVICE_ERROR &&
          Device.PowerTransition && Device.Core.Suspended && (Registers[1][0x80/4]&1U) &&
          (Registers[1][0x80/4+MG_PMIC_EINT_BANK]&MG_PMIC_EINT_BIT));
    Before=Writes;
    CHECK(MgControllerStart(&Device,TRUE,WdfPowerDeviceD3)==STATUS_SUCCESS &&
          !Device.PowerTransition && Device.Core.Active && !Device.Core.Suspended &&
          (Registers[1][0x80/4]&1U)==0 && Writes==Before+2 &&
          !(Registers[1][0x80/4+MG_PMIC_EINT_BANK]&MG_PMIC_EINT_BIT));
    CHECK((Registers[1][0]&1U) && (Registers[1][MG_PMIC_EINT_BANK]&MG_PMIC_EINT_BIT) &&
          GpioOperations>GpioBefore && !Device.Core.Pin[6].ContextValid);
    CHECK(MgReadPins(&Device,&Read)==STATUS_SUCCESS);
    CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    CHECK(StopAndReleaseController()==STATUS_SUCCESS && Unmaps==MG_SPACES && FastMutexDepth==0);
  }
  for (I=0;I<4;I++) {
    Start(); memset(&Enable,0,sizeof(Enable));
    Enable.BankId=1; Enable.PinNumber=22; Enable.InterruptMode=Latched; Enable.Polarity=InterruptActiveLow; Enable.PullConfiguration=(UCHAR)I;
    CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_SUCCESS && Device.Core.Pin[54].Kind==3);
    Disable.BankId=1; Disable.PinNumber=22; CHECK(MgInterruptDisable(&Device,&Disable)==STATUS_SUCCESS);
    memset(&Connect,0,sizeof(Connect)); Pins[0]=6; Pins[1]=7;
    Connect.BankId=4; Connect.PinNumberTable=Pins; Connect.PinCount=2;
    Connect.ConnectMode=ConnectModeInput; Connect.PullConfiguration=(UCHAR)I;
    CHECK(MgConnectIo(&Device,&Connect)==STATUS_SUCCESS);
    CHECK(MgControllerStop(&Device,FALSE,WdfPowerDeviceD3Final)==STATUS_SUCCESS);
    CHECK(StopAndReleaseController()==STATUS_SUCCESS && Unmaps==MG_SPACES && LockDepth==0);
  }
  for (I=0;I<4;I++) {
    Start(); memset(&Enable,0,sizeof(Enable)); memset(&Disable,0,sizeof(Disable));
    memset(&Connect,0,sizeof(Connect)); memset(&Disconnect,0,sizeof(Disconnect)); memset(&Read,0,sizeof(Read));
    Pins[0]=0; Connect.PinNumberTable=Disconnect.PinNumberTable=Pins;
    Connect.PinCount=Disconnect.PinCount=1; Connect.ConnectMode=ConnectModeInput;
    Enable.InterruptMode=LevelSensitive; Enable.Polarity=InterruptActiveHigh;
    if (I&1) {
      CHECK(MgConnectIo(&Device,&Connect)==STATUS_SUCCESS);
      CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_SUCCESS);
    } else {
      CHECK(MgInterruptEnable(&Device,&Enable)==STATUS_SUCCESS); Before=Writes;
      CHECK(MgConnectIo(&Device,&Connect)==STATUS_SUCCESS && Writes==Before);
    }
    CHECK(Device.Core.Pin[0].IoAlias && Device.Core.Owned[0]==1);
    Before=Writes; Connect.PullConfiguration=1;
    CHECK(MgConnectIo(&Device,&Connect)==STATUS_INVALID_PARAMETER && Writes==Before);
    Connect.PullConfiguration=0; Read.PinValues=&Values;
    Registers[0][0x200/4]=0; CHECK(MgReadPins(&Device,&Read)==STATUS_SUCCESS && Values==0);
    Registers[0][0x200/4]=1; CHECK(MgReadPins(&Device,&Read)==STATUS_SUCCESS && Values==1);
    if (I&2) {
      CHECK(MgInterruptDisable(&Device,&Disable)==STATUS_SUCCESS && Device.Core.Pin[0].Kind==1 && !Device.Core.Owned[0]);
      CHECK(MgReadPins(&Device,&Read)==STATUS_SUCCESS && Values==1);
      CHECK(MgDisconnectIo(&Device,&Disconnect)==STATUS_SUCCESS && !Device.Core.Pin[0].Saved);
    } else {
      CHECK(MgDisconnectIo(&Device,&Disconnect)==STATUS_SUCCESS && Writes==Before && Device.Core.Owned[0]==1);
      CHECK(MgInterruptDisable(&Device,&Disable)==STATUS_SUCCESS && !Device.Core.Pin[0].Saved);
    }
    CHECK(StopAndReleaseController()==STATUS_SUCCESS && Unmaps==MG_SPACES);
  }
  Start(); memset(&Connect,0,sizeof(Connect)); Pins[0]=0;
  Connect.PinNumberTable=Pins; Connect.PinCount=1; Connect.ConnectMode=ConnectModeInput; Connect.PullConfiguration=4;
  Before=Writes; CHECK(MgConnectIo(&Device,&Connect)==STATUS_NOT_SUPPORTED && Writes==Before);
  CHECK(StopAndReleaseController()==STATUS_SUCCESS);
  WakeMachineCode(); VirtualPmicRouteMachineCode(); LnaMachineCode(); LnaInterfaceMachineCode();
  ConnPinsMachineCode();
  CHECK(LockDepth==0 && LockMask==0 && FastMutexDepth==0);
  CHECK(ConfigurationMutexDepth==0);
  CHECK(ConfigurationMutexAcquires>0);
  CHECK(SpinDepth==0);
  CHECK(CurrentIrql==PASSIVE_LEVEL);
  return Failure;
}
