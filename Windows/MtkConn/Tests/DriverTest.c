// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../Driver.c"

typedef struct {
    ULONG Count;
    CM_PARTIAL_RESOURCE_DESCRIPTOR Descriptors[4];
} RESOURCE_LIST;

typedef struct {
    BOOLEAN Created, Open;
    CONN_PROVIDER Index;
    WDF_IO_TARGET_OPEN_PARAMS Callbacks;
} TARGET;

static CONN_DRIVER DriverState;
static CONN_DEVICE DeviceState[2];
static WDF_DRIVER_CONFIG DriverCallbacks;
static WDF_PNPPOWER_EVENT_CALLBACKS PowerCallbacks;
static EVT_WDFDEVICE_WDM_IRP_PREPROCESS *Preprocess;
static TARGET Targets[ConnProviderCount];
static unsigned Pdos[5], PdoRefs[5], InterfaceRefs[ConnProviderCount];
static unsigned DriverObject, Init, Lock;
static unsigned Invalidations, Failures, Starts, Stops, Releases, Loads, Frees;
static BOOLEAN NoGpio, DuplicateSoc, FailGpioQuery, FailFirmware, FailStart, FailStop, FailScoInterruptCreate;
static BOOLEAN FailRelationsAllocation;
static unsigned ScoInterruptLockDepth;
static ULONG Reset;
static T70_CONN_BTIF_INTERFACE BtifInterface;
static unsigned BtifRefs, BtifEnables, BtifRestores, BtifVisits;
static BOOLEAN BtifV1Registered, BtifPublished, FailBtifEnable, FailBtifRestore;
static UCHAR RaisedIrql;
static ULONG Aperture[0x100000U / sizeof(ULONG)];
static UCHAR Emi, Rom;
static WDF_INTERRUPT_CONFIG ScoInterruptConfig;
static WDFINTERRUPT ScoInterruptHandle;
static unsigned ScoInterruptCreates,ScoMmioReads,ScoMmioWrites;

static RESOURCE_LIST Raw = {4, {
    {.Type = CmResourceTypeMemory, .u.Memory = {{0x18000000}, 0x100000}},
    {.Type = CmResourceTypeMemory, .u.Memory = {{0x10007018}, 4}},
    {.Type = CmResourceTypeInterrupt, .ShareDisposition = CmResourceShareDeviceExclusive,
     .u.Interrupt = {439, 439, 1}},
    {.Type = CmResourceTypeInterrupt, .ShareDisposition = CmResourceShareDeviceExclusive,
     .u.Interrupt = {441, 441, 1}}
}};

PVOID TestWdfContext(WDFOBJECT Object) { return Object; }
UCHAR KeGetCurrentIrql(VOID) { return RaisedIrql; }
LONG InterlockedExchange(volatile LONG *Target,LONG Exchange)
{
    LONG Previous=*Target;*Target=Exchange;return Previous;
}

LONG InterlockedCompareExchange(volatile LONG *Target,LONG Exchange,LONG Comparand)
{
    LONG Previous=*Target;if(Previous==Comparand)*Target=Exchange;return Previous;
}

ULONG READ_REGISTER_ULONG(volatile ULONG *Address)
{
    assert(Address == (volatile ULONG *)(Aperture + CONN_SCO_CONTROL_OFFSET / sizeof(ULONG)));
    ScoMmioReads++;
    return *Address;
}

VOID WRITE_REGISTER_ULONG(volatile ULONG *Address, ULONG Value)
{
    assert(Address == (volatile ULONG *)(Aperture + CONN_SCO_CONTROL_OFFSET / sizeof(ULONG)));
    ScoMmioWrites++;
    *Address = Value;
}

VOID KeMemoryBarrier(VOID) { }

VOID WdfInterruptAcquireLock(WDFINTERRUPT Interrupt)
{
    assert(Interrupt == ScoInterruptHandle && Interrupt && !ScoInterruptLockDepth);
    ++ScoInterruptLockDepth;
}

VOID WdfInterruptReleaseLock(WDFINTERRUPT Interrupt)
{
    assert(Interrupt == ScoInterruptHandle && Interrupt && ScoInterruptLockDepth == 1);
    --ScoInterruptLockDepth;
}

VOID IoCompleteRequest(PIRP Irp, KPRIORITY PriorityBoost)
{
    (void)PriorityBoost;
    assert(Irp && !Irp->Completed);
    Irp->Completed = 1;
}

WDFDEVICE WdfInterruptGetDevice(WDFINTERRUPT Interrupt)
{
    assert(Interrupt == ScoInterruptHandle && Interrupt);
    return &DeviceState[0];
}

NTSTATUS WdfInterruptCreate(WDFDEVICE Device, WDF_INTERRUPT_CONFIG *Config,
                            WDF_OBJECT_ATTRIBUTES *Attributes, WDFINTERRUPT *Result)
{
    assert(Device == &DeviceState[0] && !Attributes && Config && Result);
    assert(Config->EvtInterruptIsr == ConnScoInterrupt && Config->InterruptRaw &&
           Config->InterruptTranslated);
    assert(Config->InterruptRaw->u.Interrupt.Vector == 439 &&
           Config->InterruptTranslated->u.Interrupt.Vector == 439);
    ++ScoInterruptCreates;
    if (FailScoInterruptCreate) return STATUS_INSUFFICIENT_RESOURCES;
    ScoInterruptConfig = *Config;
    ScoInterruptHandle = &ScoInterruptConfig;
    *Result = ScoInterruptHandle;
    return STATUS_SUCCESS;
}

VOID WdfObjectReference(WDFOBJECT Object)
{
    assert(Object == &DeviceState[0] && !Lock);
    ++BtifRefs;
}

VOID WdfObjectDereference(WDFOBJECT Object)
{
    assert(Object == &DeviceState[0] && BtifRefs && !Lock);
    --BtifRefs;
}

NTSTATUS WdfDeviceAddQueryInterface(WDFDEVICE Device, WDF_QUERY_INTERFACE_CONFIG *Query)
{
    assert(Device == &DeviceState[0] && !BtifPublished && !Lock);
    assert(Query->Interface->Context == Device);
    if (!memcmp(Query->InterfaceType, &GUID_T70_CONN_BTIF_INTERFACE_V1, sizeof(GUID))) {
        assert(!BtifV1Registered && Query->Interface->Version == 1 &&
               Query->Interface->Size == FIELD_OFFSET(T70_CONN_BTIF_INTERFACE, ClaimSession));
        memcpy(&BtifInterface, Query->Interface, Query->Interface->Size);
        assert(BtifInterface.Acquire && BtifInterface.Access && BtifInterface.Release &&
               !BtifInterface.ClaimSession);
        BtifV1Registered = TRUE;
        return STATUS_SUCCESS;
    }
    assert(BtifV1Registered && !BtifInterface.ClaimSession);
    assert(memcmp(Query->InterfaceType, &GUID_T70_CONN_BTIF_INTERFACE_V2, sizeof(GUID)) == 0);
    assert(Query->Interface->Size == sizeof(BtifInterface) &&
           Query->Interface->Version == T70_CONN_BTIF_INTERFACE_VERSION);
    memcpy(&BtifInterface, Query->Interface, sizeof(BtifInterface));
    return STATUS_SUCCESS;
}

NTSTATUS WdfDeviceCreateDeviceInterface(WDFDEVICE Device, const GUID *Guid, const UNICODE_STRING *Name)
{
    assert(Device == &DeviceState[0] && BtifInterface.ClaimSession && !BtifPublished && !Lock && !Name);
    assert(memcmp(Guid, &GUID_DEVINTERFACE_T70_CONN_BTIF, sizeof(GUID)) == 0);
    BtifPublished = TRUE;
    return STATUS_SUCCESS;
}

WDFDRIVER WdfDeviceGetDriver(WDFDEVICE Device)
{
    assert(Device == &DeviceState[0] || Device == &DeviceState[1]);
    return &DriverState;
}

WDFDEVICE WdfIoTargetGetDevice(WDFIOTARGET Target)
{
    assert(Target == &Targets[ConnSoc] || Target == &Targets[ConnGpio]);
    return &DeviceState[0];
}

PDEVICE_OBJECT WdfDeviceWdmGetPhysicalDevice(WDFDEVICE Device)
{
    assert(Device == &DeviceState[0] || Device == &DeviceState[1]);
    return &Pdos[Device == &DeviceState[0] ? 0 : 4];
}

PDRIVER_OBJECT WdfDriverWdmGetDriverObject(WDFDRIVER Driver)
{
    assert(Driver == &DriverState);
    return &DriverObject;
}

NTSTATUS WdfWaitLockCreate(WDF_OBJECT_ATTRIBUTES *Attributes, WDFWAITLOCK *Result)
{
    assert(Attributes->ParentObject == &DriverState);
    *Result = &Lock;
    return STATUS_SUCCESS;
}

VOID WdfWaitLockAcquire(WDFWAITLOCK Handle, const LARGE_INTEGER *Timeout)
{
    assert(Handle == &Lock && Lock == 0 && !Timeout);
    ++Lock;
}

VOID WdfWaitLockRelease(WDFWAITLOCK Handle)
{
    assert(Handle == &Lock && Lock == 1);
    --Lock;
}

NTSTATUS IoGetDeviceInterfaces(const GUID *Class, PDEVICE_OBJECT Pdo, ULONG Flags, PWSTR *Names)
{
    assert(!Pdo && !Flags && Lock == 0);
    const WCHAR Soc[] = L"Soc\0", Gpio[] = L"Gpio\0", Duplicate[] = L"Soc\0Soc2\0";
    BOOLEAN IsSoc = memcmp(Class, &GUID_DEVINTERFACE_MTK_CONN_POWER, sizeof(*Class)) == 0;
    assert(IsSoc || memcmp(Class, &GUID_DEVINTERFACE_MTK_CONN_PINS, sizeof(*Class)) == 0);
    size_t Bytes = IsSoc ? (DuplicateSoc ? sizeof(Duplicate) : sizeof(Soc)) :
                           (NoGpio ? sizeof(WCHAR) : sizeof(Gpio));
    *Names = malloc(Bytes);
    assert(*Names);
    memcpy(*Names, IsSoc ? (DuplicateSoc ? Duplicate : Soc) : (NoGpio ? L"" : Gpio), Bytes);
    return STATUS_SUCCESS;
}

NTSTATUS WdfIoTargetCreate(WDFDEVICE Device, WDF_OBJECT_ATTRIBUTES *Attributes, WDFIOTARGET *Result)
{
    assert(Device == &DeviceState[0] && !Attributes && Lock == 0);
    CONN_PROVIDER Index = Targets[ConnSoc].Created ? ConnGpio : ConnSoc;
    assert(!Targets[Index].Created);
    Targets[Index] = (TARGET){.Created = TRUE, .Index = Index};
    *Result = &Targets[Index];
    return STATUS_SUCCESS;
}

NTSTATUS WdfIoTargetOpen(WDFIOTARGET Handle, WDF_IO_TARGET_OPEN_PARAMS *Parameters)
{
    TARGET *Target = Handle;
    assert(Target->Created && !Target->Open && Lock == 0);
    assert(DeviceState[0].Targets[Target->Index] == Target);
    assert(wcscmp(Parameters->Name.Buffer, Target->Index == ConnSoc ? L"Soc" : L"Gpio") == 0);
    assert(Parameters->Access == FILE_READ_ATTRIBUTES &&
           Parameters->ShareAccess == (FILE_SHARE_READ | FILE_SHARE_WRITE));
    Target->Callbacks = *Parameters;
    Target->Open = TRUE;
    assert(Target->Callbacks.EvtIoTargetQueryRemove(Target) == STATUS_DEVICE_BUSY);
    return STATUS_SUCCESS;
}

PDEVICE_OBJECT WdfIoTargetWdmGetTargetPhysicalDevice(WDFIOTARGET Handle)
{
    TARGET *Target = Handle;
    assert(Target->Created && Target->Open && Lock == 1);
    return &Pdos[Target->Index + 1];
}

static VOID ReferenceInterface(PVOID Context) { ++*(unsigned *)Context; }

static VOID DereferenceInterface(PVOID Context)
{
    assert(*(unsigned *)Context);
    --*(unsigned *)Context;
}

NTSTATUS WdfIoTargetQueryForInterface(WDFIOTARGET Handle, const GUID *Id, INTERFACE *Interface,
                                     USHORT Size, USHORT Version, void *Specific)
{
    TARGET *Target = Handle;
    assert(Target->Created && Target->Open && Lock == 0 && !Specific);
    assert(Version == 1);
    if (Target->Index == ConnSoc) {
        assert(Size == sizeof(MTK_CONN_POWER_INTERFACE));
        assert(memcmp(Id, &GUID_MTK_CONN_POWER_INTERFACE_V1, sizeof(*Id)) == 0);
    } else {
        assert(Size == sizeof(MTK_CONN_PINS_INTERFACE));
        assert(memcmp(Id, &GUID_MTK_CONN_PINS_INTERFACE_V1, sizeof(*Id)) == 0);
        if (FailGpioQuery) {
            return STATUS_NOT_SUPPORTED;
        }
    }
    *Interface = (INTERFACE){Size, Version, &InterfaceRefs[Target->Index],
                             ReferenceInterface, DereferenceInterface};
    ReferenceInterface(Interface->Context);
    return STATUS_SUCCESS;
}

VOID WdfIoTargetClose(WDFIOTARGET Handle)
{
    TARGET *Target = Handle;
    assert(Target->Created && Lock == 0);
    Target->Open = FALSE;
}

VOID WdfObjectDelete(WDFOBJECT Handle)
{
    TARGET *Target = Handle;
    assert(Target->Created && !Target->Open && Lock == 0);
    Target->Created = FALSE;
}

VOID WdfDeviceSetFailed(WDFDEVICE Device, ULONG Action)
{
    assert(Device == &DeviceState[0] && Action == WdfDeviceFailedNoRestart && Lock == 0);
    ++Failures;
}

ULONG WdfCmResourceListGetCount(WDFCMRESLIST List) { return ((RESOURCE_LIST *)List)->Count; }

PCM_PARTIAL_RESOURCE_DESCRIPTOR WdfCmResourceListGetDescriptor(WDFCMRESLIST List, ULONG Index)
{
    RESOURCE_LIST *Resources = List;
    assert(Index < Resources->Count);
    return &Resources->Descriptors[Index];
}

VOID WdfDeviceInitSetPnpPowerEventCallbacks(PWDFDEVICE_INIT DeviceInit, WDF_PNPPOWER_EVENT_CALLBACKS *Callbacks)
{
    assert(DeviceInit == &Init);
    PowerCallbacks = *Callbacks;
}

NTSTATUS WdfDeviceInitAssignWdmIrpPreprocessCallback(PWDFDEVICE_INIT DeviceInit,
    EVT_WDFDEVICE_WDM_IRP_PREPROCESS *Callback, UCHAR Major, UCHAR *Minor, ULONG Count)
{
    assert(DeviceInit == &Init && Major == IRP_MJ_PNP && Count == 1 && *Minor == IRP_MN_QUERY_DEVICE_RELATIONS);
    Preprocess = Callback;
    return STATUS_SUCCESS;
}

NTSTATUS WdfDeviceCreate(PWDFDEVICE_INIT *DeviceInit, WDF_OBJECT_ATTRIBUTES *Attributes, WDFDEVICE *Device)
{
    assert(*DeviceInit == &Init && Attributes->ContextSize == sizeof(CONN_DEVICE));
    assert(Attributes->ExecutionLevel == WdfExecutionLevelPassive);
    assert(Attributes->SynchronizationScope == WdfSynchronizationScopeNone);
    *Device = &DeviceState[0];
    *DeviceInit = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS WdfDriverCreate(PDRIVER_OBJECT Object, PUNICODE_STRING Path,
    WDF_OBJECT_ATTRIBUTES *Attributes, WDF_DRIVER_CONFIG *Config, WDFDRIVER *Driver)
{
    assert(Object == &DriverObject && Path && Attributes->ContextSize == sizeof(CONN_DRIVER));
    DriverCallbacks = *Config;
    *Driver = &DriverState;
    return STATUS_SUCCESS;
}

VOID IoInvalidateDeviceRelations(PDEVICE_OBJECT Pdo, DEVICE_RELATION_TYPE Relation)
{
    assert(Pdo == &Pdos[0] && Relation == PowerRelations && Lock == 0);
    ++Invalidations;
}

VOID ObReferenceObject(PDEVICE_OBJECT Pdo) { ++PdoRefs[(unsigned *)Pdo - Pdos]; }

VOID ObDereferenceObject(PDEVICE_OBJECT Pdo)
{
    unsigned Index = (unsigned *)Pdo - Pdos;
    assert(PdoRefs[Index]);
    --PdoRefs[Index];
}

PVOID ExAllocatePool2(ULONGLONG Flags, SIZE_T Bytes, ULONG Tag)
{
    assert(Flags == POOL_FLAG_PAGED && Tag == CONN_RELATIONS_TAG);
    return FailRelationsAllocation ? NULL : calloc(1, Bytes);
}

VOID ExFreePool(PVOID Memory) { free(Memory); }

VOID RtlCopyMemory(PVOID Destination, const void *Source, SIZE_T Size) { memcpy(Destination, Source, Size); }

ULONG DbgPrintEx(ULONG Component, ULONG Level, const char *Format, ...)
{
    (void)Component;
    (void)Level;
    (void)Format;
    return 0;
}

NTSTATUS WdfDeviceWdmDispatchPreprocessedIrp(WDFDEVICE Device, PIRP Irp)
{
    assert(Device == &DeviceState[0] && Irp->Skipped == 1 && Lock == 0);
    return Irp->IoStatus.Status;
}

NTSTATUS T70ConnPrepareHardware(T70_CONN_BOOTSTRAP *Hardware, const CM_PARTIAL_RESOURCE_DESCRIPTOR *RawWindow,
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *Window, const CM_PARTIAL_RESOURCE_DESCRIPTOR *RawReset,
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *ResetResource)
{
    assert(Hardware == &DriverState.Hardware && !Hardware->Window && !Hardware->Reset);
    if (!RawWindow || !Window || !RawReset || !ResetResource) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    assert(RawWindow->u.Memory.Start.QuadPart == 0x18000000 && Window->u.Memory.Length == 0x100000);
    assert(RawReset->u.Memory.Start.QuadPart == 0x10007018 && ResetResource->u.Memory.Length == 4);
    Hardware->Window = (PUCHAR)Aperture;
    Hardware->Reset = &Reset;
    return STATUS_SUCCESS;
}

NTSTATUS T70ConnStart(T70_CONN_BOOTSTRAP *Hardware, const MTK_CONN_POWER_INTERFACE *Power,
    const MTK_CONN_PINS_INTERFACE *Pins, const MTK_CONN_ROM_SET Images[2])
{
    assert(Lock == 1 && Hardware->Window == (PUCHAR)Aperture && Hardware->Reset == &Reset && !Hardware->Emi);
    assert(InterfaceRefs[ConnSoc] == 1 && InterfaceRefs[ConnGpio] == 1);
    assert(Images[0].Image[0].Data == &Rom && Images[1].Image[0].Data == &Rom);
    Hardware->Power = *Power;
    Hardware->Pins = *Pins;
    ReferenceInterface(Power->Header.Context);
    ReferenceInterface(Pins->Header.Context);
    Hardware->Emi = &Emi;
    ++Starts;
    return FailStart ? STATUS_IO_TIMEOUT : STATUS_SUCCESS;
}

NTSTATUS T70ConnEnableBtifClock(T70_CONN_BOOTSTRAP *Hardware)
{
    assert(Hardware == &DriverState.Hardware && Hardware->Window == (PUCHAR)Aperture && Hardware->Emi && Lock == 1);
    ++BtifEnables;
    if (FailBtifEnable) {
        return STATUS_DEVICE_NOT_READY;
    }
    Hardware->BtifRestore = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS T70ConnRestoreBtifClock(T70_CONN_BOOTSTRAP *Hardware)
{
    assert(Hardware == &DriverState.Hardware && Hardware->Window == (PUCHAR)Aperture && Hardware->Emi && Lock == 1);
    ++BtifRestores;
    if (FailBtifRestore) {
        return STATUS_IO_TIMEOUT;
    }
    Hardware->BtifRestore = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS T70ConnStop(T70_CONN_BOOTSTRAP *Hardware)
{
    assert(Lock == 1 && !DriverState.BtifToken);
    ++Stops;
    if (!Hardware->Emi) {
        return STATUS_SUCCESS;
    }
    assert(Hardware->Window == (PUCHAR)Aperture && Hardware->Reset == &Reset);
    if (FailStop) {
        return STATUS_IO_TIMEOUT;
    }
    if (Hardware->BtifRestore) {
        NTSTATUS Status = T70ConnRestoreBtifClock(Hardware);
        if (Status != STATUS_SUCCESS) {
            return Status;
        }
    }
    DereferenceInterface(Hardware->Power.Header.Context);
    DereferenceInterface(Hardware->Pins.Header.Context);
    Hardware->Emi = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS T70ConnReleaseHardware(T70_CONN_BOOTSTRAP *Hardware)
{
    assert(Hardware == &DriverState.Hardware && Lock == 1 && !DriverState.BtifToken);
    ++Releases;
    Hardware->Window = NULL;
    Hardware->Reset = NULL;
    return Hardware->Emi ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
}

NTSTATUS T70ConnLoadFirmware(PDRIVER_OBJECT Object, T70_CONN_FIRMWARE *Firmware)
{
    assert(Object == &DriverObject && !Firmware->Images[0].Image[0].Data);
    ++Loads;
    if (FailFirmware) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    Firmware->Images[0].Image[0].Data = Firmware->Images[1].Image[0].Data = &Rom;
    return STATUS_SUCCESS;
}

VOID T70ConnFreeFirmware(T70_CONN_FIRMWARE *Firmware)
{
    if (Firmware->Images[0].Image[0].Data) {
        ++Frees;
    }
    RtlZeroMemory(Firmware, sizeof(*Firmware));
}

static VOID CheckScoInterrupt(VOID)
{
    volatile ULONG *Control = (volatile ULONG *)(Aperture + CONN_SCO_CONTROL_OFFSET / sizeof(ULONG));
    ULONG Initial = 0x81e54321U;
    *Control = Initial;
    assert(DeviceState[0].ScoInterruptReady);
    assert(ScoInterruptConfig.EvtInterruptIsr(ScoInterruptHandle, 0));
    assert(*Control == (Initial & ~CONN_SCO_CONTROL_CLEAR_MASK));
    Initial = 0x00154321U;
    *Control = Initial;
    assert(!ScoInterruptConfig.EvtInterruptIsr(ScoInterruptHandle, 0));
    assert(*Control == Initial);
}

static NTSTATUS Prepare(VOID)
{
    return PowerCallbacks.EvtDevicePrepareHardware(&DeviceState[0], &Raw, &Raw);
}

static NTSTATUS Enter(VOID)
{
    return PowerCallbacks.EvtDeviceD0Entry(&DeviceState[0], WdfPowerDeviceD3);
}

static NTSTATUS Exit(VOID)
{
    return PowerCallbacks.EvtDeviceD0Exit(&DeviceState[0], WdfPowerDeviceD3);
}

static VOID Release(NTSTATUS Expected)
{
    assert(PowerCallbacks.EvtDeviceReleaseHardware(&DeviceState[0], &Raw) == Expected);
    assert(!Targets[ConnSoc].Created && !Targets[ConnGpio].Created);
    assert(!DriverState.Owner && !DriverState.Hardware.Window && !DriverState.Hardware.Reset);
    assert(!DeviceState[0].OwnsHardware && !DeviceState[0].ScoInterrupt &&
           !DeviceState[0].Firmware.Images[0].Image[0].Data);
    assert(!PdoRefs[1] && !PdoRefs[2]);
    assert(InterfaceRefs[ConnSoc] == (Expected == STATUS_DEVICE_BUSY));
    assert(InterfaceRefs[ConnGpio] == InterfaceRefs[ConnSoc]);
    ScoInterruptHandle = NULL;
}

static VOID Relations(BOOLEAN Ready)
{
    DEVICE_RELATIONS *Before = malloc(FIELD_OFFSET(DEVICE_RELATIONS, Objects) + 2 * sizeof(PDEVICE_OBJECT));
    assert(Before);
    Before->Count = Ready ? 2 : 1;
    Before->Objects[0] = &Pdos[3];
    ObReferenceObject(Before->Objects[0]);
    if (Ready) {
        Before->Objects[1] = &Pdos[1];
        ObReferenceObject(Before->Objects[1]);
    }
    IRP Irp = {.IoStatus = {STATUS_SUCCESS, (ULONG_PTR)Before}};
    Irp.Current.Parameters.QueryDeviceRelations.Type = PowerRelations;
    assert(Preprocess(&DeviceState[0], &Irp) == STATUS_SUCCESS);
    DEVICE_RELATIONS *After = (DEVICE_RELATIONS *)Irp.IoStatus.Information;
    assert(After->Count == (Ready ? 3U : 1U) && After->Objects[0] == &Pdos[3]);
    if (Ready) {
        assert(After->Objects[1] == &Pdos[1] && After->Objects[2] == &Pdos[2]);
    }
    for (ULONG Index = 0; Index < After->Count; ++Index) {
        ObDereferenceObject(After->Objects[Index]);
    }
    free(After);
    assert(!PdoRefs[3] && PdoRefs[1] == (DeviceState[0].Providers[ConnSoc] != NULL));
    assert(PdoRefs[2] == (DeviceState[0].Providers[ConnGpio] != NULL));
}

static NTSTATUS NTAPI VisitBtif(PVOID Argument)
{
    assert(Argument == &BtifVisits && Lock == 1);
    ++BtifVisits;
    return STATUS_IO_TIMEOUT;
}

static void TestBtifLifetime(void)
{
    assert(BtifPublished && !BtifRefs);
    INTERFACE *Header = &BtifInterface.Header;
    PVOID Context = Header->Context;
    Header->InterfaceReference(Context);
    assert(BtifRefs == 1);
    ULONGLONG Token = 99, Other = 99;

    assert(Prepare() == STATUS_SUCCESS);
    assert(BtifInterface.Acquire(Context, &Token) == STATUS_DEVICE_NOT_READY && !Token && !BtifEnables);
    assert(Enter() == STATUS_SUCCESS);
    FailBtifEnable = TRUE;
    assert(BtifInterface.Acquire(Context, &Token) == STATUS_DEVICE_NOT_READY && !Token);
    FailBtifEnable = FALSE;
    assert(BtifInterface.Acquire(Context, &Token) == STATUS_SUCCESS && Token && BtifEnables == 2);
    assert(BtifInterface.Acquire(Context, &Other) == STATUS_DEVICE_BUSY && !Other && BtifEnables == 2);
    assert(BtifInterface.Access(Context, Token + 1, VisitBtif, &BtifVisits) == STATUS_INVALID_PARAMETER);
    RaisedIrql = 2;
    assert(BtifInterface.Access(Context, Token, VisitBtif, &BtifVisits) == STATUS_INVALID_PARAMETER);
    RaisedIrql = 0;
    assert(!BtifVisits);
    assert(BtifInterface.Access(Context, Token, VisitBtif, &BtifVisits) == STATUS_IO_TIMEOUT && BtifVisits == 1);

    FailBtifRestore = TRUE;
    assert(BtifInterface.Release(Context, Token, TRUE) == STATUS_IO_TIMEOUT && BtifRestores == 1);
    assert(!DriverState.BtifToken && DriverState.Hardware.BtifRestore);
    assert(BtifInterface.Release(Context, Token, TRUE) == STATUS_INVALID_PARAMETER && BtifRestores == 1);
    FailBtifRestore = FALSE;
    assert(Exit() == STATUS_SUCCESS && BtifRestores == 2);
    assert(Enter() == STATUS_SUCCESS);
    assert(BtifInterface.Acquire(Context, &Other) == STATUS_SUCCESS && Other > Token);
    assert(BtifInterface.Access(Context, Token, VisitBtif, &BtifVisits) == STATUS_INVALID_PARAMETER);
    assert(BtifInterface.Release(Context, Other, FALSE) == STATUS_SUCCESS && BtifRestores == 2);
    assert(BtifInterface.Acquire(Context, &Token) == STATUS_DEVICE_NOT_READY && !Token);
    assert(BtifInterface.Access(Context, Other, VisitBtif, &BtifVisits) == STATUS_DEVICE_NOT_READY);
    assert(Exit() == STATUS_SUCCESS && BtifRestores == 3 && Enter() == STATUS_SUCCESS);
    assert(BtifInterface.Acquire(Context, &Token) == STATUS_SUCCESS && Token > Other);

    unsigned PriorStops = Stops;
    assert(Exit() == STATUS_DEVICE_BUSY && Stops == PriorStops && DriverState.BtifToken == Token);
    assert(BtifInterface.Access(Context, Token, VisitBtif, &BtifVisits) == STATUS_DEVICE_NOT_READY);
    assert(BtifInterface.Release(Context, Token, FALSE) == STATUS_DEVICE_NOT_READY && BtifRestores == 3);
    assert(Exit() == STATUS_SUCCESS && BtifRestores == 4);
    assert(Enter() == STATUS_SUCCESS && BtifInterface.Acquire(Context, &Other) == STATUS_SUCCESS);
    Targets[ConnSoc].Callbacks.EvtIoTargetRemoveComplete(&Targets[ConnSoc]);
    assert(BtifInterface.Access(Context, Other, VisitBtif, &BtifVisits) == STATUS_DEVICE_REMOVED);
    assert(BtifInterface.Release(Context, Other, TRUE) == STATUS_DEVICE_REMOVED && BtifRestores == 4);
    assert(BtifVisits == 1 && Exit() == STATUS_SUCCESS);
    Release(STATUS_DEVICE_BUSY);
    Header->InterfaceDereference(Context);
    assert(!BtifRefs);

    assert(Prepare() == STATUS_SUCCESS && Enter() == STATUS_SUCCESS && BtifRestores == 5);
    Header->InterfaceReference(Context);
    assert(BtifInterface.Acquire(Context, &Token) == STATUS_SUCCESS && Token > Other);
    PowerCallbacks.EvtDeviceSurpriseRemoval(&DeviceState[0]);
    Release(STATUS_DEVICE_BUSY);
    assert(BtifInterface.Access(Context, Token, VisitBtif, &BtifVisits) == STATUS_DEVICE_REMOVED);
    assert(BtifInterface.Release(Context, Token, TRUE) == STATUS_DEVICE_REMOVED && BtifRestores == 5);
    Header->InterfaceDereference(Context);
    assert(!BtifRefs);

    assert(Prepare() == STATUS_SUCCESS && Enter() == STATUS_SUCCESS && BtifRestores == 6);
    Header->InterfaceReference(Context);
    ULONGLONG Session = 99;
    BOOLEAN Fresh = TRUE;
    assert(BtifInterface.Acquire(Context, &Token) == STATUS_SUCCESS);
    assert(BtifInterface.ClaimSession(Context, Token + 1, &Session, &Fresh) == STATUS_INVALID_PARAMETER);
    assert(!Session && !Fresh && !DeviceState[0].BtifSession);
    assert(BtifInterface.ClaimSession(Context, Token, &Session, &Fresh) == STATUS_SUCCESS &&
           Fresh && Session == Token);
    assert(BtifInterface.ClaimSession(Context, Token, &Session, &Fresh) == STATUS_SUCCESS &&
           !Fresh && Session == Token);
    assert(BtifInterface.Release(Context, Token, TRUE) == STATUS_SUCCESS);
    assert(BtifInterface.Acquire(Context, &Other) == STATUS_SUCCESS && Other > Token);
    assert(BtifInterface.ClaimSession(Context, Other, &Session, &Fresh) == STATUS_SUCCESS &&
           !Fresh && Session == Token);
    assert(BtifInterface.Release(Context, Other, TRUE) == STATUS_SUCCESS);
    assert(Exit() == STATUS_SUCCESS && Enter() == STATUS_SUCCESS);
    assert(BtifInterface.Acquire(Context, &Token) == STATUS_SUCCESS && Token > Other);
    assert(BtifInterface.ClaimSession(Context, Token, &Session, &Fresh) == STATUS_SUCCESS &&
           Fresh && Session == Token);
    assert(BtifInterface.Release(Context, Token, TRUE) == STATUS_SUCCESS);
    Header->InterfaceDereference(Context);
    assert(Exit() == STATUS_SUCCESS);
    Release(STATUS_SUCCESS);
}

int main(void)
{
    UNICODE_STRING Path;
    RtlInitUnicodeString(&Path, L"Mock");
    assert(DriverEntry(&DriverObject, &Path) == STATUS_SUCCESS);
    assert(DriverCallbacks.EvtDriverDeviceAdd(&DriverState, &Init) == STATUS_SUCCESS);

    Raw.Descriptors[0].Type = 0;
    assert(Prepare() == STATUS_DEVICE_CONFIGURATION_ERROR);
    Release(STATUS_SUCCESS);
    Raw.Descriptors[0].Type = CmResourceTypeMemory;

    ULONG SavedScoVector = Raw.Descriptors[2].u.Interrupt.Vector;
    Raw.Descriptors[2].u.Interrupt.Vector = 440;
    assert(Prepare() == STATUS_DEVICE_CONFIGURATION_ERROR);
    Release(STATUS_SUCCESS);
    Raw.Descriptors[2].u.Interrupt.Vector = SavedScoVector;
    ULONG SavedWlanVector = Raw.Descriptors[3].u.Interrupt.Vector;
    Raw.Descriptors[3].u.Interrupt.Vector = 439;
    assert(Prepare() == STATUS_DEVICE_CONFIGURATION_ERROR);
    Release(STATUS_SUCCESS);
    Raw.Descriptors[3].u.Interrupt.Vector = SavedWlanVector;
    UCHAR SavedShare = Raw.Descriptors[2].ShareDisposition;
    Raw.Descriptors[2].ShareDisposition = CmResourceShareShared;
    assert(Prepare() == STATUS_DEVICE_CONFIGURATION_ERROR);
    Release(STATUS_SUCCESS);
    Raw.Descriptors[2].ShareDisposition = SavedShare;
    FailScoInterruptCreate = TRUE;
    assert(Prepare() == STATUS_INSUFFICIENT_RESOURCES);
    Release(STATUS_SUCCESS);
    FailScoInterruptCreate = FALSE;

    DuplicateSoc = TRUE;
    assert(Prepare() == STATUS_DEVICE_CONFIGURATION_ERROR);
    Release(STATUS_SUCCESS);
    DuplicateSoc = FALSE;

    NoGpio = TRUE;
    assert(Prepare() == STATUS_DEVICE_NOT_READY && PdoRefs[1] == 1 && !PdoRefs[2]);
    Release(STATUS_SUCCESS);
    NoGpio = FALSE;

    FailFirmware = TRUE;
    assert(Prepare() == STATUS_OBJECT_NAME_NOT_FOUND);
    Release(STATUS_SUCCESS);
    FailFirmware = FALSE;

    unsigned InterruptsBeforeReadyPrepare = ScoInterruptCreates;
    assert(Prepare() == STATUS_SUCCESS);
    assert(ScoInterruptCreates == InterruptsBeforeReadyPrepare + 1 && ScoInterruptHandle);
    assert(!ScoInterruptConfig.EvtInterruptIsr(ScoInterruptHandle, 0));
    assert(PowerCallbacks.EvtDevicePrepareHardware(&DeviceState[1], &Raw, &Raw) == STATUS_DEVICE_BUSY);
    unsigned PriorReleases = Releases;
    assert(PowerCallbacks.EvtDeviceReleaseHardware(&DeviceState[1], &Raw) == STATUS_SUCCESS);
    assert(Releases == PriorReleases && DriverState.Owner == &DeviceState[0]);
    Relations(TRUE);
    FailRelationsAllocation = TRUE;
    IRP FailedRelations = {.IoStatus = {STATUS_SUCCESS, 0}};
    FailedRelations.Current.Parameters.QueryDeviceRelations.Type = PowerRelations;
    assert(Preprocess(&DeviceState[0], &FailedRelations) == STATUS_INSUFFICIENT_RESOURCES);
    assert(FailedRelations.Completed && FailedRelations.Skipped == 0 &&
           FailedRelations.IoStatus.Status == STATUS_INSUFFICIENT_RESOURCES);
    FailRelationsAllocation = FALSE;

    FailGpioQuery = TRUE;
    assert(Enter() == STATUS_NOT_SUPPORTED && !Starts);
    assert(!InterfaceRefs[ConnSoc] && !InterfaceRefs[ConnGpio]);
    FailGpioQuery = FALSE;

    FailStart = TRUE;
    assert(Enter() == STATUS_IO_TIMEOUT && !DriverState.Hardware.Emi);
    assert(!InterfaceRefs[ConnSoc] && !InterfaceRefs[ConnGpio]);
    FailStart = FALSE;
    assert(Enter() == STATUS_SUCCESS && Starts == 2);
    CheckScoInterrupt();
    assert(Exit() == STATUS_SUCCESS && !DriverState.Hardware.Emi && !DeviceState[0].ScoInterruptReady);
    assert(Enter() == STATUS_SUCCESS && Starts == 3);

    unsigned PriorStops = Stops;
    PowerCallbacks.EvtDeviceSurpriseRemoval(&DeviceState[0]);
    assert(Exit() == STATUS_SUCCESS && Stops == PriorStops);
    Release(STATUS_DEVICE_BUSY);

    assert(Prepare() == STATUS_SUCCESS && Enter() == STATUS_SUCCESS && Starts == 4);
    assert(DriverState.Hardware.Emi == &Emi);
    PriorStops = Stops;
    Targets[ConnGpio].Callbacks.EvtIoTargetRemoveComplete(&Targets[ConnGpio]);
    assert(Failures == 1 && !Targets[ConnGpio].Open && !DeviceState[0].ScoInterruptReady);
    unsigned ReadsAfterProviderLoss = ScoMmioReads, WritesAfterProviderLoss = ScoMmioWrites;
    assert(!ScoInterruptConfig.EvtInterruptIsr(ScoInterruptHandle,0));
    assert(ScoMmioReads==ReadsAfterProviderLoss && ScoMmioWrites==WritesAfterProviderLoss);
    Relations(FALSE);
    assert(Exit() == STATUS_SUCCESS && Stops == PriorStops);
    assert(Enter() == STATUS_DEVICE_NOT_READY);
    Release(STATUS_DEVICE_BUSY);

    assert(Prepare() == STATUS_SUCCESS);
    FailStop = TRUE;
    assert(Enter() == STATUS_IO_TIMEOUT && Starts == 4);
    Release(STATUS_DEVICE_BUSY);
    FailStop = FALSE;

    assert(Prepare() == STATUS_SUCCESS && Enter() == STATUS_SUCCESS && Starts == 5);
    assert(Exit() == STATUS_SUCCESS);
    Release(STATUS_SUCCESS);

    assert(Failures == 1 && Loads == 5 && Frees == 4 && Invalidations == 18);
    TestBtifLifetime();
    DriverCallbacks.EvtDriverUnload(&DriverState);
    puts("Windows WCN0 KMDF: PnP, power, BTIF rundown and SCO GSIV 439 resource/ISR ACK checks passed");
    return 0;
}
