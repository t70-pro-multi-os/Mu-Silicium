/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Real Host.c, Board.c and MFG controller. Only Windows objects, I/O manager,
   parent resource service and hardware are fixtures; no installed driver. */
#include "Soc.h"
static KIRQL HostIrql;
static void SocTestMutexInit(PRKMUTEX,ULONG);
static LONG SocTestMutexRelease(PRKMUTEX,BOOLEAN);
static NTSTATUS SocTestWait(void *,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER);
static PKTHREAD SocTestThread(void);
static void *SocTestMap(PHYSICAL_ADDRESS,SIZE_T,ULONG);
static void SocTestUnmap(void *,SIZE_T);
static ULONG SocTestRead(volatile ULONG *);
static void SocTestWrite(volatile ULONG *,ULONG);
static void SocTestStall(ULONG);
static void SocTestBuildMdl(PMDL);
static void SocTestFlush(PMDL,BOOLEAN,BOOLEAN);
#define KeInitializeMutex SocTestMutexInit
#define KeReleaseMutex SocTestMutexRelease
static NTSTATUS HostCreate(PDRIVER_OBJECT,ULONG,PUNICODE_STRING,DEVICE_TYPE,ULONG,BOOLEAN,PDEVICE_OBJECT *);
static NTSTATUS HostProperty(PDEVICE_OBJECT,DEVICE_REGISTRY_PROPERTY,ULONG,void *,PULONG);
static BOOLEAN HostEqual(PCUNICODE_STRING,PCUNICODE_STRING,BOOLEAN);
static void HostDelete(PDEVICE_OBJECT);
static NTSTATUS HostAttach(PDEVICE_OBJECT,PDEVICE_OBJECT,PDEVICE_OBJECT *);
static void HostDetach(PDEVICE_OBJECT);
static PIO_WORKITEM HostAllocateWork(PDEVICE_OBJECT);
static void HostFreeWork(PIO_WORKITEM);
static void HostQueueWork(PIO_WORKITEM,PIO_WORKITEM_ROUTINE,WORK_QUEUE_TYPE,void *);
static void HostLockInit(PIO_REMOVE_LOCK,ULONG,ULONG,ULONG);
static NTSTATUS HostLockAcquire(PIO_REMOVE_LOCK,void *);
static void HostLockRelease(PIO_REMOVE_LOCK,void *);
static void HostLockWait(PIO_REMOVE_LOCK,void *);
static LONG_PTR HostObjectReference(void *);
static LONG_PTR HostObjectDereference(void *);
static PDEVICE_OBJECT HostTarget(PDEVICE_OBJECT);
static PIRP HostBuild(ULONG,PDEVICE_OBJECT,void *,ULONG,PLARGE_INTEGER,PKEVENT,PIO_STATUS_BLOCK);
static NTSTATUS HostCall(PDEVICE_OBJECT,PIRP);
static NTSTATUS HostPower(PDEVICE_OBJECT,PIRP);
static POWER_STATE HostSetPower(PDEVICE_OBJECT,POWER_STATE_TYPE,POWER_STATE);
static void HostComplete(PIRP,CCHAR);
static void HostEvent(PRKEVENT,EVENT_TYPE,BOOLEAN);
static LONG HostSetEvent(PRKEVENT,KPRIORITY,BOOLEAN);
static NTSTATUS HostWait(void *,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER);
static NTSTATUS HostDelay(KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER);
static NTSTATUS HostRegister(PDEVICE_OBJECT,const GUID *,PUNICODE_STRING,PUNICODE_STRING);
static NTSTATUS HostSetInterface(PUNICODE_STRING,BOOLEAN);
static void HostFreeUnicode(PUNICODE_STRING);
#undef KeGetCurrentIrql
#define KeGetCurrentIrql() HostIrql
#define IoCreateDevice HostCreate
#define IoGetDeviceProperty HostProperty
#define RtlEqualUnicodeString HostEqual
#define IoDeleteDevice HostDelete
#define IoAttachDeviceToDeviceStackSafe HostAttach
#define IoDetachDevice HostDetach
#define IoAllocateWorkItem HostAllocateWork
#define IoFreeWorkItem HostFreeWork
#define IoQueueWorkItem HostQueueWork
#define IoRegisterDeviceInterface HostRegister
#define IoSetDeviceInterfaceState HostSetInterface
#define RtlFreeUnicodeString HostFreeUnicode
#undef IoInitializeRemoveLock
#define IoInitializeRemoveLock HostLockInit
#undef IoAcquireRemoveLock
#define IoAcquireRemoveLock HostLockAcquire
#undef IoReleaseRemoveLock
#define IoReleaseRemoveLock HostLockRelease
#undef IoReleaseRemoveLockAndWait
#define IoReleaseRemoveLockAndWait HostLockWait
#undef ObReferenceObject
#define ObReferenceObject HostObjectReference
#undef ObDereferenceObject
#define ObDereferenceObject HostObjectDereference
#define IoGetAttachedDeviceReference HostTarget
#define IoBuildSynchronousFsdRequest HostBuild
#undef IoCallDriver
#define IoCallDriver HostCall
#define PoCallDriver HostPower
#define PoSetPowerState HostSetPower
#undef IoCompleteRequest
#define IoCompleteRequest HostComplete
#define KeSetEvent HostSetEvent
#define KeInitializeEvent HostEvent
#define KeWaitForSingleObject HostWait
#define KeDelayExecutionThread HostDelay
#include "Host.c"
#undef KeDelayExecutionThread
static MALI_BOARD_HOST HostStore;
#define MALI_BOARD_TEST_PROVIDER (&HostStore.Board)
#define MtkMaliBoardAssertionCount MtkMaliHostAssertionCount
#define MALI_BOARD_TEST_ENTRY UnusedBoardTestEntry
#undef KeSetEvent
#include "BoardTests.c"
#undef TestMain
#undef KeGetCurrentIrql
#define KeGetCurrentIrql() HostIrql
#undef KeWaitForSingleObject
#define KeWaitForSingleObject SocTestWait
#undef KeGetCurrentThread
#define KeGetCurrentThread SocTestThread
#define MmMapIoSpaceEx SocTestMap
#define MmUnmapIoSpace SocTestUnmap
#undef READ_REGISTER_ULONG
#define READ_REGISTER_ULONG SocTestRead
#undef WRITE_REGISTER_ULONG
#define WRITE_REGISTER_ULONG SocTestWrite
#define KeStallExecutionProcessor SocTestStall
#define MmBuildMdlForNonPagedPool SocTestBuildMdl
#define KeFlushIoBuffers SocTestFlush
#include "Soc.c"
static ULONG SocPmicReads,SocPmicWrites;
static ULONG SocPmicMask(uint32_t Address) {
    if(Address==0x1508 || Address==0x1bc8)return 1;
    if(Address==0x1526 || Address==0x1bf4)return 0x7f;
    return 0;
}
MALI_RESULT MtkMaliPmicPrmRead16(uint32_t Address,uint16_t *Value) {
    if(!Value || Address>UINT16_MAX || (Address&1))return MaliInvalid;
    if(Address!=0x1508 && Address!=0x1520 && Address!=0x1526 && Address!=0x1bc8 &&
       Address!=0x1be2 && Address!=0x1be4 && Address!=0x1bf4)return MaliUnsupported;
    CHECK(Address/2<sizeof(BRegs)/sizeof(BRegs[0]));++SocPmicReads;
    *Value=BRegs[Address/2];return MaliOk;
}
MALI_RESULT MtkMaliPmicPrmWrite16(uint32_t Address,uint16_t Value) {
    ULONG Mask=SocPmicMask(Address);
    if(Address>UINT16_MAX || (Address&1))return MaliInvalid;
    if(!Mask)return MaliUnsupported;
    CHECK(Address/2<sizeof(BRegs)/sizeof(BRegs[0]));
    if(((BRegs[Address/2]^Value)&~Mask)!=0)return MaliInvalid;
    BRegs[Address/2]=Value;++SocPmicWrites;
    if(Address==0x1508)BRegs[0x1520/2]=(BRegs[0x1520/2]&~1u)|(Value&1u);
    if(Address==0x1bc8)BRegs[0x1be4/2]=(BRegs[0x1be4/2]&~1u)|(Value&1u);
    if(Address==0x1bf4)BRegs[0x1be2/2]=(BRegs[0x1be2/2]&~0x7f00u)|((Value&0x7f)<<8);
    return MaliOk;
}

static DRIVER_OBJECT HostDriver;
static DRIVER_EXTENSION HostExtension;
static DEVICE_OBJECT HostDevice;
static unsigned Created,Deleted,Attached,Detached,WorkAllocated,WorkFreed,WorkQueued,WorkRunning;
static unsigned ParentRefs,ParentLive,ParentOpens,ParentCloses,ParentQueries,QueryWaits;
static unsigned HostObjectRefs,PdoObjectRefs,TargetObjectRefs,WorkObjectRefs;
static unsigned LockCount,LockRemoved,LockDrained,HostCompleted,PowerCalls;
static unsigned FailCreate,FailWork,FailTarget,FailBuild,FailAttach,IdKind,Malformed,NoParentCookie;
static unsigned QueryPending,QueryOutstanding,InlineWork,NotifyDuringClose;
static NTSTATUS QueryResult,OpenResult,CloseResult;
static NTSTATUS ConnRegisterResult,ConnEnableResult;
static ULONG ConnRegistrations,ConnEnables,ConnDisables,ConnFrees;
static BOOLEAN ConnLinkLive,SocConnMock,SocBtifGateStuck;
static PIRP LockTags[16];
static PIO_WORKITEM_ROUTINE QueuedRoutine;
static void *QueuedContext;
static PKEVENT ParentEvent;
static PIO_STATUS_BLOCK ParentResult;
static IRP ParentIrp,PassIrp;
static IO_STACK_LOCATION ParentStacks[2],PassStacks[2];
static unsigned PassPending,PassOutstanding;
static NTSTATUS PassStatus;
static unsigned char WorkStorage;

static void ParentRef(void *X) {CHECK(X==&ParentLive && ParentRefs);++ParentRefs;}
static void ParentDrop(void *X) {CHECK(X==&ParentLive && ParentRefs && !ParentLive);--ParentRefs;}
static NTSTATUS ParentOpen(void *X,PDEVICE_OBJECT Pdo,MALI_BOARD_CONFIG *Out,void **Cookie) {
    CHECK(X==&ParentLive && Pdo==&PnpPdo && ParentRefs==1 && !ParentLive && Out==&HostStore.Config && Cookie==&HostStore.ParentCookie);
    ++ParentOpens;InitProvider(0,0);*Out=Config;RtlZeroMemory(&Provider,sizeof(Provider));
    if(OpenResult!=STATUS_SUCCESS && NoParentCookie)return OpenResult;
    if(!NoParentCookie){ParentLive=1;*Cookie=&ParentLive;}
    if(Malformed==8)Out->Resources.Enter=0;
    if(Malformed==9)Out->Lifetime.Reference=ParentRef;
    return OpenResult;
}
static NTSTATUS ParentClose(void *X,void *Cookie) {
    CHECK(X==&ParentLive && Cookie==&ParentLive && ParentLive && ParentRefs==1);
    CHECK(!Attached || (Detached && LockDrained && !LockCount));++ParentCloses;
    if(NotifyDuringClose) {
        NotifyDuringClose=0;BhRequestReap(&HostStore);return STATUS_DEVICE_BUSY;
    }
    if(CloseResult==STATUS_SUCCESS)ParentLive=0;return CloseResult;
}
static NTSTATUS HostProperty(PDEVICE_OBJECT Pdo,DEVICE_REGISTRY_PROPERTY Property,ULONG Bytes,void *Out,PULONG Used) {
    CHECK(Pdo==&PnpPdo && Property==DevicePropertyHardwareID && Bytes==256 && Used);
    static const WCHAR Good[]=L"ACPI\\HANA678E\0";
    static const WCHAR Wrong[]=L"ACPI\\HANA0000\0";
    static const WCHAR Lower[]=L"acpi\\hana678e\0";
    static const WCHAR Root[]=L"ACPI\\HANA678F\0";
    const WCHAR *Id=IdKind==1?Wrong:IdKind==2?Lower:IdKind==6?Root:Good;
    *Used=sizeof(Good);RtlCopyMemory(Out,Id,sizeof(Good));
    if(IdKind==3)*Used=3;
    if(IdKind==4)((WCHAR*)Out)[sizeof(Good)/sizeof(WCHAR)-1]=L'X';
    if(IdKind==5)return STATUS_BUFFER_TOO_SMALL;
    return STATUS_SUCCESS;
}
static BOOLEAN HostEqual(PCUNICODE_STRING A,PCUNICODE_STRING B,BOOLEAN Insensitive) {
    CHECK(Insensitive && A && B);if(A->Length!=B->Length)return FALSE;
    for(UINT I=0;I<A->Length/sizeof(WCHAR);I++) {
        WCHAR X=A->Buffer[I],Y=B->Buffer[I];
        if(X>=L'a' && X<=L'z')X-=32;if(Y>=L'a' && Y<=L'z')Y-=32;
        if(X!=Y)return FALSE;
    }
    return TRUE;
}
static NTSTATUS HostCreate(PDRIVER_OBJECT Object,ULONG Bytes,PUNICODE_STRING Name,DEVICE_TYPE Type,
    ULONG Characteristics,BOOLEAN Exclusive,PDEVICE_OBJECT *Out) {
    CHECK(Object==&HostDriver && Bytes==sizeof(HostStore) && !Name && Type==PnpPdo.DeviceType &&
        Characteristics==FILE_DEVICE_SECURE_OPEN && !Exclusive && Out && !Created);
    if(FailCreate)return STATUS_INSUFFICIENT_RESOURCES;
    ++Created;HostDevice.DeviceExtension=&HostStore;HostDevice.DriverObject=Object;
    HostDevice.Flags=DO_DEVICE_INITIALIZING;HostDriver.DeviceObject=&HostDevice;*Out=&HostDevice;
    return STATUS_SUCCESS;
}
static void HostDelete(PDEVICE_OBJECT Device) {
    CHECK(Device==&HostDevice && Created && !Deleted && !HostStore.Signature);
    CHECK(!ParentRefs && !ParentLive && !PdoObjectRefs && !TargetObjectRefs && WorkAllocated==WorkFreed);
    CHECK(!Attached || (Detached && LockDrained && !LockCount));
    CHECK(!HostStore.InterfacePins && !Provider.References[0] && !Provider.References[1]);
    CHECK(!ConnLinkLive && ConnFrees==ConnRegistrations);
    ++Deleted;HostDriver.DeviceObject=0;
}
static NTSTATUS HostRegister(PDEVICE_OBJECT Pdo,const GUID *Class,PUNICODE_STRING Reference,PUNICODE_STRING Link) {
    static WCHAR Instance[]=L"MT6789-ConnPower";
    CHECK(Pdo==&PnpPdo && Class && RtlEqualMemory(Class,&GUID_DEVINTERFACE_MTK_CONN_POWER,sizeof(*Class)) &&
        !Reference && Link==&HostStore.ConnLink && !Link->Buffer && !ConnRegistrations);
    if(NT_SUCCESS(ConnRegisterResult)) {
        ++ConnRegistrations;
        Link->Buffer=Instance;Link->Length=sizeof(Instance)-sizeof(WCHAR);Link->MaximumLength=sizeof(Instance);
    }
    return ConnRegisterResult;
}
static NTSTATUS HostSetInterface(PUNICODE_STRING Link,BOOLEAN Enable) {
    CHECK(Link==&HostStore.ConnLink && Link->Buffer && ConnRegistrations==1 && !ConnFrees);
    CHECK(Enable!=ConnLinkLive);
    if(Enable) {
        ++ConnEnables;
        if(NT_SUCCESS(ConnEnableResult))ConnLinkLive=TRUE;
        return ConnEnableResult;
    }
    ++ConnDisables;ConnLinkLive=FALSE;return STATUS_SUCCESS;
}
static void HostFreeUnicode(PUNICODE_STRING Link) {
    CHECK(Link==&HostStore.ConnLink && Link->Buffer && !ConnLinkLive && !ConnFrees);
    ++ConnFrees;RtlZeroMemory(Link,sizeof(*Link));
}
static NTSTATUS HostAttach(PDEVICE_OBJECT Device,PDEVICE_OBJECT Pdo,PDEVICE_OBJECT *Lower) {
    CHECK(Device==&HostDevice && Pdo==&PnpPdo && Lower==&HostStore.Lower &&
        (HostStore.Role?(HostStore.SocInitialized && !ParentRefs && !ParentLive):((ParentRefs==1 && ParentLive && HostStore.BoardInitialized) || (!ParentRefs && !ParentLive && !HostStore.BoardInitialized))));
    if(!HostStore.Role && HostStore.BoardInitialized)CHECK(HostStore.Config.Lifetime.Context==&HostStore && HostStore.Config.Lifetime.Reference==BhReference);
    if(FailAttach)return STATUS_NO_SUCH_DEVICE;
    ++Attached;*Lower=&PnpOther;HostDevice.StackSize=2;return STATUS_SUCCESS;
}
static void HostDetach(PDEVICE_OBJECT Lower) {
    CHECK(Lower==&PnpOther && Attached && !Detached && LockDrained && !LockCount && (HostStore.Role?HostStore.Soc.Removed:(!HostStore.BoardInitialized || Provider.Removed)));
    ++Detached;
}
static PIO_WORKITEM HostAllocateWork(PDEVICE_OBJECT Device) {
    CHECK(Device==&HostDevice && Created && !WorkAllocated);if(FailWork)return 0;
    ++WorkAllocated;return (PIO_WORKITEM)&WorkStorage;
}
static void HostFreeWork(PIO_WORKITEM Work) {
    CHECK(Work==(PIO_WORKITEM)&WorkStorage && WorkAllocated && !WorkFreed && !WorkQueued);
    ++WorkFreed;
}
static void RunHostWork(void) {
    CHECK(WorkQueued && QueuedRoutine && !WorkRunning && !Deleted);WorkQueued=0;WorkRunning=1;
    PIO_WORKITEM_ROUTINE Routine=QueuedRoutine;void *Context=QueuedContext;
    Routine(&HostDevice,Context);WorkRunning=0;CHECK(WorkObjectRefs);--WorkObjectRefs;
}
static void HostQueueWork(PIO_WORKITEM Work,PIO_WORKITEM_ROUTINE Routine,WORK_QUEUE_TYPE Type,void *Context) {
    CHECK(Work==(PIO_WORKITEM)&WorkStorage && Type==DelayedWorkQueue && Context==&HostStore && Routine==BhReap &&
        WorkAllocated && !WorkFreed && !WorkQueued && !Deleted && HostStore.RemoveDone);
    ++WorkQueued;++WorkObjectRefs;QueuedRoutine=Routine;QueuedContext=Context;
    if(InlineWork)RunHostWork();
}
static void HostLockInit(PIO_REMOVE_LOCK Lock,ULONG Tag,ULONG Minutes,ULONG HighWater) {
    CHECK(Lock==&HostStore.RemoveLock && Tag==BH_TAG && !Minutes && !HighWater && !LockCount);LockCount=1;
}
static NTSTATUS HostLockAcquire(PIO_REMOVE_LOCK Lock,void *Tag) {
    CHECK(Lock==&HostStore.RemoveLock && Tag);if(LockRemoved)return STATUS_DELETE_PENDING;
    for(UINT I=0;I<16;I++)if(!LockTags[I]){LockTags[I]=Tag;++LockCount;return STATUS_SUCCESS;}
    CHECK(0);return STATUS_INSUFFICIENT_RESOURCES;
}
static void HostLockRelease(PIO_REMOVE_LOCK Lock,void *Tag) {
    CHECK(Lock==&HostStore.RemoveLock && Tag && LockCount>1);
    for(UINT I=0;I<16;I++)if(LockTags[I]==Tag){LockTags[I]=0;--LockCount;return;}
    CHECK(0);
}
static void CompletePass(void) {
    CHECK(PassOutstanding && !Deleted);PassOutstanding=0;
    PIO_STACK_LOCATION Stack=&PassStacks[0];
    BOOLEAN RootPower=HostStore.Role && Stack->MajorFunction==IRP_MJ_POWER && Stack->MinorFunction==IRP_MN_SET_POWER;
    CHECK(Stack->CompletionRoutine && Stack->Context==(RootPower?(void *)ParentEvent:(void *)&HostStore));
    PassIrp.IoStatus.Status=PassStatus;PassIrp.PendingReturned=(BOOLEAN)PassPending;
    IoSkipCurrentIrpStackLocation(&PassIrp);
    KIRQL Previous=HostIrql;HostIrql=DISPATCH_LEVEL;
    CHECK(Stack->CompletionRoutine(&HostDevice,&PassIrp,Stack->Context)==
        (HostStore.Role && Stack->MajorFunction==IRP_MJ_POWER && Stack->MinorFunction==IRP_MN_SET_POWER?STATUS_MORE_PROCESSING_REQUIRED:STATUS_CONTINUE_COMPLETION));
    HostIrql=Previous;
    if(PassPending && !RootPower)CHECK(PassStacks[1].Control&SL_PENDING_RETURNED);
}
static void HostLockWait(PIO_REMOVE_LOCK Lock,void *Tag) {
    CHECK(Lock==&HostStore.RemoveLock && Tag==&PnpIrp && !LockRemoved && PnpWaits==1 && PnpSignals==1 && !Detached);
    LockRemoved=1;if(PassOutstanding)CompletePass();
    CHECK(LockCount==2);HostLockRelease(Lock,Tag);CHECK(LockCount==1);LockCount=0;LockDrained=1;
}
static LONG_PTR HostObjectReference(void *Object) {
    CHECK(!Deleted);
    if(Object==&HostDevice)return ++HostObjectRefs;
    if(Object==&PnpPdo)return ++PdoObjectRefs;
    CHECK(Object==&PnpOther);return ++TargetObjectRefs;
}
static LONG_PTR HostObjectDereference(void *Object) {
    if(Object==&HostDevice){CHECK(HostObjectRefs);return --HostObjectRefs;}
    if(Object==&PnpPdo){CHECK(PdoObjectRefs);return --PdoObjectRefs;}
    CHECK(Object==&PnpOther && TargetObjectRefs);return --TargetObjectRefs;
}
static PDEVICE_OBJECT HostTarget(PDEVICE_OBJECT Pdo) {
    CHECK(Pdo==&PnpPdo && PdoObjectRefs==1);if(FailTarget)return 0;
    HostObjectReference(&PnpOther);return &PnpOther;
}
static PIRP HostBuild(ULONG Major,PDEVICE_OBJECT Target,void *Buffer,ULONG Bytes,PLARGE_INTEGER Offset,
    PKEVENT Event,PIO_STATUS_BLOCK Result) {
    CHECK(Major==IRP_MJ_PNP && Target==&PnpOther && !Buffer && !Bytes && !Offset && Event==ParentEvent && Result);
    if(FailBuild)return 0;
    RtlZeroMemory(&ParentIrp,sizeof(ParentIrp));RtlZeroMemory(ParentStacks,sizeof(ParentStacks));
    ParentIrp.Tail.Overlay.CurrentStackLocation=ParentStacks+2;ParentIrp.CurrentLocation=3;ParentIrp.StackCount=2;
    ParentStacks[1].MajorFunction=(UCHAR)Major;ParentResult=Result;return &ParentIrp;
}
static void CompleteParent(void) {
    PIO_STACK_LOCATION S=IoGetNextIrpStackLocation(&ParentIrp);
    CHECK(S->MajorFunction==IRP_MJ_PNP && S->MinorFunction==IRP_MN_QUERY_INTERFACE && S->Parameters.QueryInterface.Size==sizeof(HostStore.Parent) &&
        S->Parameters.QueryInterface.Version==1 && RtlEqualMemory(S->Parameters.QueryInterface.InterfaceType,&BhParentGuid,sizeof(GUID)));
    const MALI_BOARD_PARENT_QUERY *Q=S->Parameters.QueryInterface.InterfaceSpecificData;
    CHECK(Q && Q->Size==sizeof(*Q) && Q->Version==1 && Q->Pdo==&PnpPdo);
    if(QueryResult==STATUS_SUCCESS) {
        MALI_BOARD_PARENT_INTERFACE *I=(void*)S->Parameters.QueryInterface.Interface;
        CHECK(I==&HostStore.Parent && !ParentRefs);++ParentRefs;
        I->Header.Size=sizeof(*I);I->Header.Version=1;I->Header.Context=&ParentLive;
        I->Header.InterfaceReference=ParentRef;I->Header.InterfaceDereference=ParentDrop;
        I->Open=ParentOpen;I->Close=ParentClose;
        if(Malformed==1)I->Header.Size--;
        if(Malformed==2)I->Header.Version++;
        if(Malformed==3)I->Header.InterfaceReference=0;
        if(Malformed==4)I->Header.InterfaceDereference=0;
        if(Malformed==5)I->Open=0;
        if(Malformed==6)I->Close=0;
    }
    ParentResult->Status=QueryResult;ParentEvent->Header.SignalState=1;QueryOutstanding=0;
}
static NTSTATUS HostCall(PDEVICE_OBJECT Target,PIRP Irp) {
    CHECK(Target==&PnpOther && !Deleted);
    if(Irp==&ParentIrp) {
        CHECK(TargetObjectRefs==1);++ParentQueries;
        if(QueryPending){QueryOutstanding=1;return STATUS_PENDING;}
        CompleteParent();return QueryResult;
    }
    CHECK(Irp==&PassIrp && Attached && !Detached && LockCount==2 && !PassOutstanding);
    IoSetNextIrpStackLocation(Irp);PassOutstanding=1;
    if(PassPending)return STATUS_PENDING;
    CompletePass();return PassStatus;
}
static NTSTATUS HostPower(PDEVICE_OBJECT Target,PIRP Irp) {++PowerCalls;return HostCall(Target,Irp);}
static void HostComplete(PIRP Irp,CCHAR Increment) {
    CHECK(Irp && !Increment);++HostCompleted;
    if(Irp==&PnpIrp && PnpStack.MajorFunction==IRP_MJ_PNP && PnpStack.MinorFunction==IRP_MN_REMOVE_DEVICE && LockRemoved)
        CHECK(Detached && LockDrained && !LockCount);
}
static void HostEvent(PRKEVENT Event,EVENT_TYPE Type,BOOLEAN State) {
    CHECK(Type==NotificationEvent && !State && !ParentEvent);ParentEvent=Event;Event->Header.SignalState=0;
}
static NTSTATUS HostWait(void *Event,KWAIT_REASON Reason,KPROCESSOR_MODE Mode,BOOLEAN Alert,PLARGE_INTEGER Timeout) {
    if(Event==&BhSocRegistry)return SocTestWait(Event,Reason,Mode,Alert,Timeout);
    CHECK(Event==ParentEvent && Reason==Executive && Mode==KernelMode && !Alert && !Timeout);++QueryWaits;
    if(QueryOutstanding)CompleteParent();
    if(PassOutstanding)CompletePass();
    CHECK(ParentEvent->Header.SignalState==1);return STATUS_SUCCESS;
}
static void ResetHost(void) {
    InitPnp();RtlZeroMemory(&HostStore,sizeof(HostStore));
    RtlZeroMemory(&HostDriver,sizeof(HostDriver));RtlZeroMemory(&HostExtension,sizeof(HostExtension));RtlZeroMemory(&HostDevice,sizeof(HostDevice));
    HostDriver.DriverExtension=&HostExtension;PnpPdo.DeviceType=FILE_DEVICE_VIDEO;
    PnpOther.Flags=DO_BUFFERED_IO|DO_POWER_PAGABLE;PnpOther.AlignmentRequirement=63;
    Created=Deleted=Attached=Detached=WorkAllocated=WorkFreed=WorkQueued=WorkRunning=0;
    ParentRefs=ParentLive=ParentOpens=ParentCloses=ParentQueries=QueryWaits=0;
    HostObjectRefs=PdoObjectRefs=TargetObjectRefs=WorkObjectRefs=0;
    LockCount=LockRemoved=LockDrained=HostCompleted=PowerCalls=0;
    FailCreate=FailWork=FailTarget=FailBuild=FailAttach=IdKind=Malformed=NoParentCookie=0;
    QueryPending=QueryOutstanding=InlineWork=NotifyDuringClose=0;
    QueryResult=OpenResult=CloseResult=STATUS_SUCCESS;
    ConnRegisterResult=ConnEnableResult=STATUS_SUCCESS;
    ConnRegistrations=ConnEnables=ConnDisables=ConnFrees=0;ConnLinkLive=SocConnMock=SocBtifGateStuck=FALSE;
    RtlZeroMemory(LockTags,sizeof(LockTags));QueuedRoutine=0;QueuedContext=0;ParentEvent=0;ParentResult=0;
    PassPending=PassOutstanding=0;PassStatus=STATUS_SUCCESS;HostIrql=BoardIrql=PASSIVE_LEVEL;
}
static NTSTATUS HostPnp(UCHAR Minor) {
    InitPnpDispatch(Minor);return MtkMaliBoardHostDispatch(&HostDevice,&PnpIrp);
}
static NTSTATUS HostPass(UCHAR Major,UINT Pending) {
    RtlZeroMemory(&PassIrp,sizeof(PassIrp));RtlZeroMemory(PassStacks,sizeof(PassStacks));
    PassIrp.Tail.Overlay.CurrentStackLocation=&PassStacks[1];PassIrp.CurrentLocation=2;PassIrp.StackCount=2;
    PassStacks[1].MajorFunction=Major;PassPending=Pending;
    return MtkMaliBoardHostDispatch(&HostDevice,&PassIrp);
}
static void HostCycles(void) {
    for(UINT Pending=0;Pending<2;Pending++) {
        ResetHost();QueryPending=Pending;IdKind=Pending?2:0;
        CHECK(MtkMaliBoardHostDriverEntry(&HostDriver,0)==STATUS_SUCCESS && HostExtension.AddDevice==MtkMaliBoardHostAddDevice);
        for(UINT I=0;I<=IRP_MJ_MAXIMUM_FUNCTION;I++)CHECK(HostDriver.MajorFunction[I]==MtkMaliBoardHostDispatch);
        CHECK(HostExtension.AddDevice(&HostDriver,&PnpPdo)==STATUS_SUCCESS && Attached && ParentRefs==1 && ParentLive);
        CHECK(QueryWaits==1 && !QueryOutstanding && !TargetObjectRefs && PdoObjectRefs==1);
        CHECK((HostDevice.Flags&(DO_BUFFERED_IO|DO_POWER_PAGABLE|DO_DEVICE_INITIALIZING))==(DO_BUFFERED_IO|DO_POWER_PAGABLE));
        CHECK(HostDevice.AlignmentRequirement==63);
        for(UINT Cycle=0;Cycle<3;Cycle++) {
            CHECK(HostPnp(IRP_MN_START_DEVICE)==STATUS_SUCCESS && Provider.Pdo==&PnpPdo && LockCount==1);
            for(UINT Power=0;Power<2;Power++) {
                InitPnpQuery(Power);PnpIrp.CurrentLocation=2;PnpIrp.StackCount=2;
                CHECK(MtkMaliBoardHostDispatch(&HostDevice,&PnpIrp)==STATUS_SUCCESS);
            }
            CHECK(HostObjectRefs==2 && HostStore.InterfacePins==2);
            BoardBase.Header.InterfaceReference(BoardBase.Header.Context);
            CHECK(HostObjectRefs==3 && HostStore.InterfacePins==3);
            BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);
            MALI_BOARD_LEASE L={0};CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==STATUS_SUCCESS && L.Cookie);
            CHECK(HostPnp(IRP_MN_QUERY_STOP_DEVICE)==STATUS_DEVICE_BUSY && LockCount==1);
            CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD3,PowerActionSleep)==STATUS_SUCCESS);
            CHECK(BoardPower.SetPower(BoardPower.Header.Context,L.Cookie,PowerDeviceD0,PowerActionNone)==STATUS_SUCCESS);
            CHECK(BoardBase.Release(BoardBase.Header.Context,L.Cookie)==STATUS_SUCCESS);
            BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);BoardPower.Header.InterfaceDereference(BoardPower.Header.Context);
            CHECK(!HostObjectRefs && !HostStore.InterfacePins && !WorkQueued);
            CHECK(HostPnp(IRP_MN_QUERY_STOP_DEVICE)==STATUS_SUCCESS);
            CHECK(HostPnp(IRP_MN_CANCEL_STOP_DEVICE)==STATUS_SUCCESS);
            CHECK(HostPnp(IRP_MN_STOP_DEVICE)==STATUS_SUCCESS && !Provider.Pdo && ParentLive && ParentRefs==1);
        }
        CHECK(HostPass(IRP_MJ_READ,0)==STATUS_SUCCESS && LockCount==1);
        CHECK(HostPass(IRP_MJ_POWER,1)==STATUS_PENDING && LockCount==2 && PowerCalls==1 && PassOutstanding);
        InlineWork=Pending;CHECK(HostPnp(IRP_MN_REMOVE_DEVICE)==STATUS_SUCCESS && Detached && LockDrained && !LockCount);
        if(!InlineWork){CHECK(WorkQueued && !Deleted);RunHostWork();}
        CHECK(Deleted && !WorkObjectRefs && !HostObjectRefs && !PdoObjectRefs && !ParentRefs && !ParentLive && ParentCloses==1);
        HostDriver.DriverUnload(&HostDriver);
    }
}
static void HostAddFailures(void) {
    for(UINT Kind=0;Kind<19;Kind++) {
        ResetHost();NTSTATUS Expected=STATUS_DRIVER_INTERNAL_ERROR;
        switch(Kind) {
        case 0:IdKind=1;Expected=STATUS_NO_SUCH_DEVICE;break;
        case 1:IdKind=3;Expected=STATUS_DEVICE_CONFIGURATION_ERROR;break;
        case 2:IdKind=4;Expected=STATUS_DEVICE_CONFIGURATION_ERROR;break;
        case 3:IdKind=5;Expected=STATUS_BUFFER_TOO_SMALL;break;
        case 4:FailCreate=1;Expected=STATUS_INSUFFICIENT_RESOURCES;break;
        case 5:FailWork=1;Expected=STATUS_INSUFFICIENT_RESOURCES;break;
        case 6:FailTarget=1;Expected=STATUS_NO_SUCH_DEVICE;break;
        case 7:FailBuild=1;Expected=STATUS_INSUFFICIENT_RESOURCES;break;
        case 8:QueryResult=STATUS_NOT_SUPPORTED;Expected=STATUS_SUCCESS;QueryPending=1;break;
        case 9:Malformed=1;break;case 10:Malformed=2;break;case 11:Malformed=3;break;
        case 12:Malformed=5;break;case 13:Malformed=6;break;
        case 14:OpenResult=STATUS_DEVICE_NOT_READY;Expected=OpenResult;break;
        case 15:Malformed=8;Expected=STATUS_INVALID_PARAMETER;break;
        case 16:FailAttach=1;Expected=STATUS_NO_SUCH_DEVICE;break;
        case 17:QueryResult=STATUS_TIMEOUT;Expected=STATUS_UNSUCCESSFUL;break;
        case 18:OpenResult=STATUS_TIMEOUT;Expected=STATUS_UNSUCCESSFUL;break;
        }
        NTSTATUS Added=MtkMaliBoardHostAddDevice(&HostDriver,&PnpPdo);
        if(Kind==8) {
            CHECK(Added==STATUS_SUCCESS && Attached && !HostStore.BoardInitialized);ParentEvent=0;
            CHECK(HostPnp(IRP_MN_START_DEVICE)==STATUS_NOT_SUPPORTED && !HostStore.BoardInitialized);
            CHECK(HostPnp(IRP_MN_REMOVE_DEVICE)==STATUS_SUCCESS);RunHostWork();
        } else CHECK(Added==Expected && !Attached);
        CHECK(!ParentRefs && !ParentLive && !HostObjectRefs && !PdoObjectRefs && !TargetObjectRefs && !WorkObjectRefs);
        CHECK(Created==Deleted && WorkAllocated==WorkFreed && !WorkQueued);
    }
    /* Partial factory ownership and malformed initial reference are retained. */
    for(UINT Kind=0;Kind<5;Kind++) {
        ResetHost();
        if(Kind==0){OpenResult=STATUS_DEVICE_NOT_READY;CloseResult=STATUS_DEVICE_BUSY;}
        if(Kind==1)Malformed=4;
        if(Kind==2)NoParentCookie=1;
        if(Kind==3){Malformed=9;CloseResult=STATUS_UNSUCCESSFUL;}
        if(Kind==4){FailAttach=1;CloseResult=STATUS_TIMEOUT;}
        CHECK(MtkMaliBoardHostAddDevice(&HostDriver,&PnpPdo)!=STATUS_SUCCESS);
        CHECK(HostStore.Quarantined && !Deleted && !Attached && ParentRefs==1 && PdoObjectRefs==1 && !WorkFreed);
        CHECK(HostStore.CleanupStatus!=STATUS_SUCCESS && !ResourceLive);
    }
}
static void HostRemoveReferences(void) {
    ResetHost();CHECK(MtkMaliBoardHostAddDevice(&HostDriver,&PnpPdo)==STATUS_SUCCESS);
    CHECK(HostPnp(IRP_MN_START_DEVICE)==STATUS_SUCCESS);GetBoth();
    CHECK(HostStore.InterfacePins==2 && HostObjectRefs==2);
    CHECK(HostPnp(IRP_MN_SURPRISE_REMOVAL)==STATUS_SUCCESS && !Detached);
    CHECK(HostPass(IRP_MJ_READ,0)==STATUS_DEVICE_REMOVED && LockCount==1);
    CHECK(HostPnp(IRP_MN_REMOVE_DEVICE)==STATUS_SUCCESS && Detached && LockDrained && WorkQueued && !Deleted);
    /* Notification while the worker is queued must coalesce, then go idle
       retaining the last reference. Its later drop queues one new cleanup. */
    BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);CHECK(HostStore.ReapState==2 && HostObjectRefs==1);
    RunHostWork();CHECK(!Deleted && HostStore.Quarantined && HostStore.ReapState==0 && ParentLive);
    BoardPower.Header.InterfaceDereference(BoardPower.Header.Context);CHECK(WorkQueued && !HostObjectRefs);
    NotifyDuringClose=1;RunHostWork();
    CHECK(Deleted && !ParentLive && !ParentRefs && !WorkObjectRefs && ParentCloses==2);
}
static void HostPoisonedRemoval(void) {
    for(UINT Poison=0;Poison<2;Poison++) {
        ResetHost();CHECK(MtkMaliBoardHostAddDevice(&HostDriver,&PnpPdo)==STATUS_SUCCESS);
        CHECK(HostPnp(IRP_MN_START_DEVICE)==STATUS_SUCCESS);GetBoth();MALI_BOARD_LEASE L={0};Stuck=Poison;
        CHECK(BoardBase.Acquire(BoardBase.Header.Context,&L)==(Poison?STATUS_IO_TIMEOUT:STATUS_SUCCESS) && L.Cookie);
        UINT R=ControllerReads(),W=ControllerWrites();
        CHECK(HostPnp(IRP_MN_SURPRISE_REMOVAL)==STATUS_SUCCESS);
        BoardBase.Header.InterfaceDereference(BoardBase.Header.Context);BoardPower.Header.InterfaceDereference(BoardPower.Header.Context);
        CHECK(HostPnp(IRP_MN_REMOVE_DEVICE)==STATUS_SUCCESS && Detached && LockDrained && !LockCount);
        RunHostWork();CHECK(!Deleted && HostStore.Quarantined && ResourceLive && ParentLive && ParentRefs==1);
        CHECK(Provider.ClientCookie==L.Cookie && !WorkQueued && !HostObjectRefs && PdoObjectRefs==1 && WorkAllocated && !WorkFreed);
        CHECK(ControllerReads()==R && ControllerWrites()==W && !ParentCloses && HostStore.CleanupStatus==STATUS_DEVICE_BUSY);
        InitPnpDispatch(IRP_MN_QUERY_INTERFACE);
        CHECK(MtkMaliBoardHostDispatch(&HostDevice,&PnpIrp)==STATUS_DELETE_PENDING && !WorkQueued);
    }
}
static void HostValidation(void) {
    ResetHost();CHECK(MtkMaliBoardHostDriverEntry(0,0)==STATUS_INVALID_PARAMETER);
    HostDriver.DeviceObject=&HostDevice;CHECK(MtkMaliBoardHostDriverEntry(&HostDriver,0)==STATUS_INVALID_PARAMETER);HostDriver.DeviceObject=0;
    CHECK(MtkMaliBoardHostAddDevice(&HostDriver,&PnpPdo)==STATUS_SUCCESS);
    CHECK(MtkMaliBoardHostDispatch(&HostDevice,0)==STATUS_INVALID_PARAMETER);
    for(UINT Kind=0;Kind<3;Kind++) {
        InitPnpDispatch(IRP_MN_REMOVE_DEVICE);
        if(Kind==0)PnpIrp.RequestorMode=UserMode;
        if(Kind==1)HostIrql=DISPATCH_LEVEL;
        if(Kind==2)PnpIrp.StackCount=1;
        CHECK(MtkMaliBoardHostDispatch(&HostDevice,&PnpIrp)==STATUS_INVALID_PARAMETER && !Detached && !LockRemoved && !HostStore.Removing && LockCount==1);
        HostIrql=PASSIVE_LEVEL;
    }
    InlineWork=1;CHECK(HostPnp(IRP_MN_REMOVE_DEVICE)==STATUS_SUCCESS && Deleted);
}
#ifndef MALI_HOST_TEST_ENTRY
#define MALI_HOST_TEST_ENTRY TestMain
#endif
int MALI_HOST_TEST_ENTRY(void) {
    HostCycles();if(Failure)return (int)Failure;
    HostAddFailures();if(Failure)return (int)Failure;
    HostRemoveReferences();if(Failure)return (int)Failure;
    HostPoisonedRemoval();if(Failure)return (int)Failure;
    HostValidation();return (int)Failure;
}

/* Actual SoC provider uses these virtual Windows/MMIO boundaries. */
static ULONG SocMapCalls,SocUnmapCalls,SocMapFail,SocMapMask,SocHardwareReads,SocHardwareWrites,SocStallUs;
static ULONG SocMdlBuilds,SocCacheFlushes;
static ULONG SocMockThread=1;
static ULONG SocMappedRegisters[MALI_SOC_REGIONS][1024];
static PKTHREAD SocTestThread(void) {return (PKTHREAD)(uintptr_t)(0x7000u+SocMockThread*0x100u);}
static void SocTestMutexInit(PRKMUTEX Mutex,ULONG Level) {
    CHECK(Mutex && !Level);RtlZeroMemory(Mutex,sizeof(*Mutex));Mutex->Header.SignalState=1;
}
static NTSTATUS SocTestWait(void *Object,KWAIT_REASON Reason,KPROCESSOR_MODE Mode,BOOLEAN Alert,PLARGE_INTEGER Timeout) {
    PRKMUTEX Mutex=Object;
    CHECK(Mutex && Reason==Executive && Mode==KernelMode && !Alert && !Timeout);
    CHECK(Mutex->Header.SignalState==1 || Mutex->OwnerThread==SocTestThread());
    --Mutex->Header.SignalState;Mutex->OwnerThread=SocTestThread();return STATUS_SUCCESS;
}
static LONG SocTestMutexRelease(PRKMUTEX Mutex,BOOLEAN Wait) {
    CHECK(Mutex && !Wait && Mutex->OwnerThread==SocTestThread() && Mutex->Header.SignalState<1);
    LONG Previous=Mutex->Header.SignalState++;
    if(Mutex->Header.SignalState==1)Mutex->OwnerThread=0;return Previous;
}
static void *SocTestMap(PHYSICAL_ADDRESS Address,SIZE_T Bytes,ULONG Protect) {
    CHECK(Bytes==4096 && Protect==(PAGE_READWRITE|PAGE_NOCACHE));
    ++SocMapCalls;if(SocMapCalls==SocMapFail)return 0;
    for(UINT I=0;I<MALI_SOC_REGIONS;I++)if((uint64_t)Address.QuadPart==SocBases[I]) {
        CHECK(!(SocMapMask&(1u<<I)));SocMapMask|=1u<<I;return SocMappedRegisters[I];
    }
    CHECK(0);return 0;
}
static void SocTestUnmap(void *Address,SIZE_T Bytes) {
    CHECK(Bytes==4096);
    for(UINT I=0;I<MALI_SOC_REGIONS;I++)if(Address==SocMappedRegisters[I]) {
        CHECK(SocMapMask&(1u<<I));SocMapMask&=~(1u<<I);++SocUnmapCalls;return;
    }
    CHECK(0);
}
static ULONG SocTestRead(volatile ULONG *Address) {
    uintptr_t A=(uintptr_t)Address;
    for(UINT I=0;I<MALI_SOC_REGIONS;I++)if(A>=(uintptr_t)SocMappedRegisters[I] && A<(uintptr_t)SocMappedRegisters[I]+4096) {
        ULONG Offset=(ULONG)(A-(uintptr_t)SocMappedRegisters[I]);CHECK(SocMapMask&(1u<<I));++SocHardwareReads;
        if(SocConnMock)return SocMappedRegisters[I][Offset/4];
        return ReadCombined(PDevice,I,Offset);
    }
    CHECK(0);return 0;
}
static void SocTestConnWrite(ULONG Space,ULONG Offset,ULONG Value) {
    ULONG *Regs=SocMappedRegisters[Space];
    if(Space==0) {
        if(Offset==0) {
            CHECK(Value==(0x0b160000|(Regs[0]&0xffff)|1u));
            Regs[0]=Value&0xffff;
            return;
        }
        CHECK(Offset==0x304 && !((Regs[0x304/4]^Value)&~0x1fu));
        Regs[0x304/4]=Value;
        for(ULONG Status=0x16c;Status<=0x170;Status+=4) {
            if((Value&0xc)==0xc)Regs[Status/4]|=2;
            if(!(Value&0xc))Regs[Status/4]&=~2u;
        }
        return;
    }
    switch(Offset) {
    case 0x80:case 0x84:
        CHECK(Value==1u<<31);
        if(SocBtifGateStuck)break;
        if(Offset==0x80)Regs[0x90/4]|=Value;
        else Regs[0x90/4]&=~Value;
        break;
    case 0x0c0:case 0x0c4:
        CHECK(Value==1u<<28);
        if(Offset==0x0c0)Regs[0x0c8/4]|=Value;
        else Regs[0x0c8/4]&=~Value;
        break;
    case 0x2a0:case 0x2a4:
        CHECK(Value==0x42000 || Value==0x04000);
        if(Offset==0x2a0){Regs[0x220/4]|=Value;Regs[0x228/4]|=Value;}
        else {Regs[0x220/4]&=~Value;Regs[0x228/4]&=~Value;}
        break;
    case 0x2a8:case 0x2ac:
        CHECK(Value==0x00400);
        if(Offset==0x2a8){Regs[0x250/4]|=Value;Regs[0x258/4]|=Value;}
        else {Regs[0x250/4]&=~Value;Regs[0x258/4]&=~Value;}
        break;
    case 0x380:case 0x388:
        CHECK(!((Regs[Offset/4]^Value)&~0xfffffu));Regs[Offset/4]=Value;break;
    case 0xc14:
        CHECK(!Value);Regs[Offset/4]=0;break;
    case 0x0f00:
        CHECK(!((Regs[Offset/4]^Value)&~0x00000800U));Regs[Offset/4]=Value;break;
    default:CHECK(0);break;
    }
}
static void SocTestWrite(volatile ULONG *Address,ULONG Value) {
    uintptr_t A=(uintptr_t)Address;
    for(UINT I=0;I<MALI_SOC_REGIONS;I++)if(A>=(uintptr_t)SocMappedRegisters[I] && A<(uintptr_t)SocMappedRegisters[I]+4096) {
        ULONG Offset=(ULONG)(A-(uintptr_t)SocMappedRegisters[I]);CHECK(SocMapMask&(1u<<I));++SocHardwareWrites;
        if(SocConnMock && I<2)SocTestConnWrite(I,Offset,Value);
        else WriteCombined(PDevice,I,Offset,Value);
        return;
    }
    CHECK(0);
}
static void SocTestStall(ULONG Us) {CHECK(Us && Us<=1000);SocStallUs+=Us;}
static void SocTestBuildMdl(PMDL Mdl) {
    CHECK(Mdl && MmGetMdlByteCount(Mdl) && MmGetMdlByteCount(Mdl)<=4096 &&
        MmGetMdlByteOffset(Mdl)+MmGetMdlByteCount(Mdl)<=4096);
    CHECK(Mdl->Size==sizeof(MDL)+sizeof(PFN_NUMBER));++SocMdlBuilds;
    Mdl->MdlFlags|=MDL_SOURCE_IS_NONPAGED_POOL;MmGetMdlPfnArray(Mdl)[0]=(uintptr_t)MmGetMdlVirtualAddress(Mdl)>>12;
}
static void SocTestFlush(PMDL Mdl,BOOLEAN Read,BOOLEAN Dma) {
    CHECK(Mdl && !Read && Dma && (Mdl->MdlFlags&MDL_SOURCE_IS_NONPAGED_POOL) &&
        MmGetMdlPfnArray(Mdl)[0]==((uintptr_t)MmGetMdlVirtualAddress(Mdl)>>12));
    ++SocCacheFlushes;
}

static LONG HostSetEvent(PRKEVENT Event,KPRIORITY Increment,BOOLEAN Wait) {
    CHECK(Event==ParentEvent && !Increment && !Wait);LONG Old=Event->Header.SignalState;
    Event->Header.SignalState=1;return Old;
}

static NTSTATUS HostDelay(KPROCESSOR_MODE Mode,BOOLEAN Alert,PLARGE_INTEGER Timeout) {
    CHECK(Mode==KernelMode && !Alert && Timeout && Timeout->QuadPart==-10000 && HostStore.PnpActive);
    InterlockedExchange(&HostStore.PnpActive,0);return STATUS_SUCCESS;
}

static POWER_STATE HostSetPower(PDEVICE_OBJECT Device,POWER_STATE_TYPE Type,POWER_STATE State) {
    CHECK(Device==&HostDevice && Type==DevicePowerState && HostStore.Role &&
        ((State.DeviceState==PowerDeviceD0 && !HostStore.Soc.Suspended) || (State.DeviceState==PowerDeviceD3 && HostStore.Soc.Suspended)));
    return State;
}
