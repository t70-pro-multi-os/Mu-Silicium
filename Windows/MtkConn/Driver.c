// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <ntddk.h>
#include <wdf.h>

#include "Bootstrap.h"
#include "BtifInterface.h"
#include "Firmware.h"

#define CONN_RELATIONS_TAG 'rCnC'
#define CONN_SCO_CONTROL_OFFSET 0x50148U
#define CONN_SCO_CONTROL_INTERRUPT 0x80000000U
#define CONN_SCO_CONTROL_CLEAR_MASK 0x81e00000U

typedef enum { ConnSoc, ConnGpio, ConnProviderCount } CONN_PROVIDER;

typedef struct {
    WDFWAITLOCK Lock;
    WDFDEVICE Owner;
    ULONGLONG BtifToken, LastBtifToken;
    T70_CONN_BOOTSTRAP Hardware;
} CONN_DRIVER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CONN_DRIVER, ConnDriver)

typedef struct {
    WDFIOTARGET Targets[ConnProviderCount];
    PDEVICE_OBJECT Providers[ConnProviderCount];
    T70_CONN_FIRMWARE Firmware;
    ULONGLONG BtifSession;
    BOOLEAN OwnsHardware;
    BOOLEAN Ready;
    BOOLEAN ProviderLost;
    BOOLEAN Closing;
    BOOLEAN SurpriseRemoved;
    WDFINTERRUPT ScoInterrupt;
    volatile LONG ScoInterruptReady;
} CONN_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CONN_DEVICE, ConnDevice)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD ConnAddDevice;
EVT_WDF_DRIVER_UNLOAD ConnUnload;
EVT_WDF_DEVICE_PREPARE_HARDWARE ConnPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE ConnReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY ConnD0Entry;
EVT_WDF_DEVICE_D0_EXIT ConnD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL ConnSurpriseRemoval;
EVT_WDF_INTERRUPT_ISR ConnScoInterrupt;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS ConnQueryRelations;
EVT_WDF_IO_TARGET_QUERY_REMOVE ConnProviderQueryRemove;
EVT_WDF_IO_TARGET_REMOVE_COMPLETE ConnProviderRemoved;

static VOID ConnSetScoInterruptReady(CONN_DEVICE *Context,BOOLEAN Ready)
{
    if (Context->ScoInterrupt) {
        WdfInterruptAcquireLock(Context->ScoInterrupt);
    }
    KeMemoryBarrier();
    InterlockedExchange(&Context->ScoInterruptReady, Ready ? 1 : 0);
    KeMemoryBarrier();
    if (Context->ScoInterrupt) {
        WdfInterruptReleaseLock(Context->ScoInterrupt);
    }
}

static NTSTATUS ExactStatus(NTSTATUS Status)
{
    return Status != STATUS_SUCCESS && NT_SUCCESS(Status) ? STATUS_IO_DEVICE_ERROR : Status;
}

static CONN_DRIVER *DriverFor(WDFDEVICE Device)
{
    return ConnDriver(WdfDeviceGetDriver(Device));
}

/* Called with the driver lock held. A resource or provider removal revokes
 * future access before unmapping, without waiting for the client to detach. */
static BOOLEAN ProvidersAvailable(const CONN_DEVICE *Context)
{
    return Context->OwnsHardware && !Context->SurpriseRemoved &&
           !Context->ProviderLost && !Context->Closing &&
           Context->Providers[ConnSoc] && Context->Providers[ConnGpio];
}

static NTSTATUS BtifAvailable(WDFDEVICE Device, const CONN_DRIVER *Driver, const CONN_DEVICE *Context)
{
    if (Context->SurpriseRemoved || Context->ProviderLost || Context->Closing) {
        return STATUS_DEVICE_REMOVED;
    }
    return Driver->Owner == Device && Context->Ready ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

static VOID NTAPI BtifInterfaceReference(PVOID Context)
{
    WdfObjectReference((WDFDEVICE)Context);
}

static VOID NTAPI BtifInterfaceDereference(PVOID Context)
{
    WdfObjectDereference((WDFDEVICE)Context);
}

static NTSTATUS NTAPI BtifAcquire(PVOID InterfaceContext, PULONGLONG Token)
{
    if (!Token || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Token = 0;
    WDFDEVICE Device = InterfaceContext;
    CONN_DRIVER *Driver = DriverFor(Device);
    WdfWaitLockAcquire(Driver->Lock, NULL);
    NTSTATUS Status = BtifAvailable(Device, Driver, ConnDevice(Device));
    if (Status == STATUS_SUCCESS) {
        if (Driver->BtifToken) {
            Status = STATUS_DEVICE_BUSY;
        } else if (Driver->LastBtifToken == (ULONGLONG)-1) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            Status = T70ConnEnableBtifClock(&Driver->Hardware);
            if (Status == STATUS_SUCCESS) {
                *Token = Driver->BtifToken = ++Driver->LastBtifToken;
            }
        }
    }
    WdfWaitLockRelease(Driver->Lock);
    return Status;
}

static NTSTATUS NTAPI BtifAccess(PVOID InterfaceContext, ULONGLONG Token,
                                T70_CONN_BTIF_ACCESS *Operation, PVOID OperationContext)
{
    if (!Token || !Operation || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    WDFDEVICE Device = InterfaceContext;
    CONN_DRIVER *Driver = DriverFor(Device);
    WdfWaitLockAcquire(Driver->Lock, NULL);
    NTSTATUS Status = BtifAvailable(Device, Driver, ConnDevice(Device));
    if (Status == STATUS_SUCCESS) {
        Status = Token == Driver->BtifToken ? Operation(OperationContext) : STATUS_INVALID_PARAMETER;
    }
    WdfWaitLockRelease(Driver->Lock);
    return Status;
}

static NTSTATUS NTAPI BtifRelease(PVOID InterfaceContext, ULONGLONG Token, BOOLEAN Quiesced)
{
    if (!Token || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    WDFDEVICE Device = InterfaceContext;
    CONN_DRIVER *Driver = DriverFor(Device);
    CONN_DEVICE *Context = ConnDevice(Device);
    WdfWaitLockAcquire(Driver->Lock, NULL);
    NTSTATUS Status = STATUS_INVALID_PARAMETER;
    if (Driver->Owner == Device && Token == Driver->BtifToken) {
        Driver->BtifToken = 0;
        Status = BtifAvailable(Device, Driver, Context);
        if (!Quiesced) {
            Context->Ready = FALSE;
        } else if (Status == STATUS_SUCCESS) {
            Status = T70ConnRestoreBtifClock(&Driver->Hardware);
        }
    } else if (Context->SurpriseRemoved || Context->ProviderLost || Context->Closing) {
        Status = STATUS_DEVICE_REMOVED;
    }
    WdfWaitLockRelease(Driver->Lock);
    return Status;
}

static NTSTATUS NTAPI BtifClaimSession(PVOID InterfaceContext, ULONGLONG Token,
                                      PULONGLONG Session, BOOLEAN *Fresh)
{
    if (!Session || !Fresh || !Token || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Session = 0;
    *Fresh = FALSE;
    WDFDEVICE Device = InterfaceContext;
    CONN_DRIVER *Driver = DriverFor(Device);
    CONN_DEVICE *Context = ConnDevice(Device);
    WdfWaitLockAcquire(Driver->Lock, NULL);
    NTSTATUS Status = BtifAvailable(Device, Driver, Context);
    if (Status == STATUS_SUCCESS) {
        if (Driver->BtifToken == Token) {
            if (!Context->BtifSession) {
                Context->BtifSession = Token;
                *Fresh = TRUE;
            }
            *Session = Context->BtifSession;
        } else {
            Status = STATUS_INVALID_PARAMETER;
        }
    }
    WdfWaitLockRelease(Driver->Lock);
    return Status;
}

NTSTATUS ConnProviderQueryRemove(WDFIOTARGET Target)
{
    // Orderly provider removal requires stopping its dependent WCN0 first.
    UNREFERENCED_PARAMETER(Target);
    return STATUS_DEVICE_BUSY;
}

VOID ConnProviderRemoved(WDFIOTARGET Target)
{
    WDFDEVICE Device = WdfIoTargetGetDevice(Target);
    CONN_DRIVER *Driver = DriverFor(Device);
    CONN_DEVICE *Context = ConnDevice(Device);

    WdfWaitLockAcquire(Driver->Lock, NULL);
    BOOLEAN Active = !Context->Closing &&
                     (Target == Context->Targets[ConnSoc] || Target == Context->Targets[ConnGpio]);
    if (Active) {
        Context->Ready = FALSE;
        ConnSetScoInterruptReady(Context, FALSE);
        Context->ProviderLost = TRUE;
    }
    WdfWaitLockRelease(Driver->Lock);

    WdfIoTargetClose(Target);
    if (Active) {
        IoInvalidateDeviceRelations(WdfDeviceWdmGetPhysicalDevice(Device), PowerRelations);
        WdfDeviceSetFailed(Device, WdfDeviceFailedNoRestart);
    }
}

static NTSTATUS OpenProvider(WDFDEVICE Device, const GUID *Class, CONN_PROVIDER Index)
{
    CONN_DRIVER *Driver = DriverFor(Device);
    CONN_DEVICE *Context = ConnDevice(Device);
    WDFIOTARGET Target = NULL;
    PWSTR Paths = NULL;
    NTSTATUS Status = IoGetDeviceInterfaces(Class, NULL, 0, &Paths);
    if (Status != STATUS_SUCCESS) {
        return ExactStatus(Status);
    }
    if (!Paths || !*Paths) {
        Status = STATUS_DEVICE_NOT_READY;
    } else {
        UNICODE_STRING Name;
        RtlInitUnicodeString(&Name, Paths);
        if (Paths[Name.Length / sizeof(WCHAR) + 1]) {
            Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        } else {
            Status = WdfIoTargetCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, &Target);
            if (Status == STATUS_SUCCESS) {
                WdfWaitLockAcquire(Driver->Lock, NULL);
                Context->Targets[Index] = Target;
                WdfWaitLockRelease(Driver->Lock);

                WDF_IO_TARGET_OPEN_PARAMS Params;
                WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Params, &Name, FILE_READ_ATTRIBUTES);
                Params.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
                Params.EvtIoTargetQueryRemove = ConnProviderQueryRemove;
                Params.EvtIoTargetRemoveComplete = ConnProviderRemoved;
                Status = WdfIoTargetOpen(Target, &Params);
            }
        }
    }
    if (Paths) {
        ExFreePool(Paths);
    }
    if (Status != STATUS_SUCCESS) {
        return ExactStatus(Status);
    }

    WdfWaitLockAcquire(Driver->Lock, NULL);
    if (Context->ProviderLost) {
        Status = STATUS_DEVICE_REMOVED;
    } else {
        PDEVICE_OBJECT Pdo = WdfIoTargetWdmGetTargetPhysicalDevice(Target);
        if (!Pdo || Pdo == WdfDeviceWdmGetPhysicalDevice(Device) ||
            Pdo == Context->Providers[1U - Index]) {
            Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        } else {
            ObReferenceObject(Pdo);
            Context->Providers[Index] = Pdo;
        }
    }
    WdfWaitLockRelease(Driver->Lock);
    return Status;
}

static BOOLEAN ConnLevelInterrupt(const CM_PARTIAL_RESOURCE_DESCRIPTOR *Resource)
{
    return Resource && Resource->Type == CmResourceTypeInterrupt &&
           Resource->ShareDisposition == CmResourceShareDeviceExclusive &&
           !(Resource->Flags & (CM_RESOURCE_INTERRUPT_LATCHED | CM_RESOURCE_INTERRUPT_MESSAGE |
                                CM_RESOURCE_INTERRUPT_SECONDARY_INTERRUPT));
}

static NTSTATUS MapHardware(T70_CONN_BOOTSTRAP *Hardware, WDFCMRESLIST Raw, WDFCMRESLIST Translated,
                            const CM_PARTIAL_RESOURCE_DESCRIPTOR **ScoRaw,
                            const CM_PARTIAL_RESOURCE_DESCRIPTOR **ScoTranslated)
{
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *RawWindow = NULL, *RawReset = NULL;
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *Window = NULL, *Reset = NULL;
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *WlanRaw = NULL, *WlanTranslated = NULL;
    ULONG Count = WdfCmResourceListGetCount(Raw);
    if (ScoRaw) *ScoRaw = NULL;
    if (ScoTranslated) *ScoTranslated = NULL;
    if (!ScoRaw || !ScoTranslated || Count != 4 || Count != WdfCmResourceListGetCount(Translated)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    for (ULONG Index = 0; Index < Count; ++Index) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *R = WdfCmResourceListGetDescriptor(Raw, Index);
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *T = WdfCmResourceListGetDescriptor(Translated, Index);
        if (!R || !T || R->Type != T->Type) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (R->Type == CmResourceTypeMemory) {
            if (R->u.Memory.Length == sizeof(ULONG) && !RawReset) {
                RawReset = R;
                Reset = T;
            } else if (R->u.Memory.Length == 0x100000U && !RawWindow) {
                RawWindow = R;
                Window = T;
            } else {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            continue;
        }
        if (!ConnLevelInterrupt(R) || !ConnLevelInterrupt(T)) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (R->u.Interrupt.Vector == 439 && !*ScoRaw) {
            *ScoRaw = R;
            *ScoTranslated = T;
        } else if (R->u.Interrupt.Vector == 441 && !WlanRaw) {
            WlanRaw = R;
            WlanTranslated = T;
        } else {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    if (!WlanRaw || !WlanTranslated) return STATUS_DEVICE_CONFIGURATION_ERROR;
    return T70ConnPrepareHardware(Hardware, RawWindow, Window, RawReset, Reset);
}

BOOLEAN ConnScoInterrupt(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    UNREFERENCED_PARAMETER(MessageId);
    WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
    CONN_DEVICE *Context = ConnDevice(Device);
    T70_CONN_BOOTSTRAP *Hardware = &DriverFor(Device)->Hardware;
    if (InterlockedCompareExchange(&Context->ScoInterruptReady, 0, 0) == 0 || !Hardware->Window) return FALSE;

    volatile ULONG *Control = (volatile ULONG *)(Hardware->Window + CONN_SCO_CONTROL_OFFSET);
    ULONG Sample = READ_REGISTER_ULONG(Control);
    if (Sample == MAXULONG || !(Sample & CONN_SCO_CONTROL_INTERRUPT)) return FALSE;

    // Until the audio endpoint owns the stream, ACK only status bits from the
    // referenced mtk-btcvsd handler; exact T70 mask parity is still unverified.
    WRITE_REGISTER_ULONG(Control, Sample & ~CONN_SCO_CONTROL_CLEAR_MASK);
    KeMemoryBarrier();
    return TRUE;
}

NTSTATUS ConnPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    CONN_DRIVER *Driver = DriverFor(Device);
    CONN_DEVICE *Context = ConnDevice(Device);
    WdfWaitLockAcquire(Driver->Lock, NULL);
    if (Driver->Owner) {
        WdfWaitLockRelease(Driver->Lock);
        return STATUS_DEVICE_BUSY;
    }
    Driver->Owner = Device;
    Context->OwnsHardware = TRUE;
    Context->Ready = FALSE;
    Context->ProviderLost = Context->Closing = Context->SurpriseRemoved = FALSE;
    Context->ScoInterrupt = NULL;
    Context->ScoInterruptReady = 0;
    WdfWaitLockRelease(Driver->Lock);

    const CM_PARTIAL_RESOURCE_DESCRIPTOR *ScoRaw = NULL, *ScoTranslated = NULL;
    NTSTATUS Status = MapHardware(&Driver->Hardware, Raw, Translated, &ScoRaw, &ScoTranslated);
    if (Status == STATUS_SUCCESS) {
        WDF_INTERRUPT_CONFIG Interrupt;
        WDF_INTERRUPT_CONFIG_INIT(&Interrupt, ConnScoInterrupt, NULL);
        Interrupt.InterruptRaw = (PCM_PARTIAL_RESOURCE_DESCRIPTOR)ScoRaw;
        Interrupt.InterruptTranslated = (PCM_PARTIAL_RESOURCE_DESCRIPTOR)ScoTranslated;
        WDFINTERRUPT Handle = NULL;
        Status = WdfInterruptCreate(Device, &Interrupt, WDF_NO_OBJECT_ATTRIBUTES, &Handle);
        if (Status == STATUS_SUCCESS) Context->ScoInterrupt = Handle;
    }
    if (Status == STATUS_SUCCESS) {
        Status = OpenProvider(Device, &GUID_DEVINTERFACE_MTK_CONN_POWER, ConnSoc);
    }
    if (Status == STATUS_SUCCESS) {
        Status = OpenProvider(Device, &GUID_DEVINTERFACE_MTK_CONN_PINS, ConnGpio);
    }
    if (Status == STATUS_SUCCESS) {
        // START is ordered by _DEP. PowerRelations keeps those providers in D0
        // until our global system power transition has quiesced the hardware.
        IoInvalidateDeviceRelations(WdfDeviceWdmGetPhysicalDevice(Device), PowerRelations);
        Status = T70ConnLoadFirmware(WdfDriverWdmGetDriverObject(WdfDeviceGetDriver(Device)),
                                     &Context->Firmware);
    }
    return Status;
}

static VOID ReleaseInterface(INTERFACE *Interface)
{
    if (Interface->InterfaceDereference) {
        Interface->InterfaceDereference(Interface->Context);
    }
}

NTSTATUS ConnD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Prior)
{
    UNREFERENCED_PARAMETER(Prior);
    CONN_DRIVER *Driver = DriverFor(Device);
    CONN_DEVICE *Context = ConnDevice(Device);
    T70_CONN_BOOTSTRAP *Hardware = &Driver->Hardware;
    WdfWaitLockAcquire(Driver->Lock, NULL);
    Context->Ready = FALSE;
    // A prior teardown could have left an unresolved hardware journal.
    NTSTATUS Status = STATUS_DEVICE_NOT_READY;
    if (ProvidersAvailable(Context)) {
        Status = Driver->BtifToken ? STATUS_DEVICE_BUSY : T70ConnStop(Hardware);
    }
    WdfWaitLockRelease(Driver->Lock);
    if (Status != STATUS_SUCCESS) {
        return Status;
    }

    MTK_CONN_POWER_INTERFACE Power = {0};
    MTK_CONN_PINS_INTERFACE Pins = {0};
    Status = WdfIoTargetQueryForInterface(Context->Targets[ConnSoc], &GUID_MTK_CONN_POWER_INTERFACE_V1,
                                         &Power.Header, sizeof(Power), MTK_CONN_POWER_INTERFACE_VERSION, NULL);
    if (Status != STATUS_SUCCESS) {
        return ExactStatus(Status);
    }
    Status = WdfIoTargetQueryForInterface(Context->Targets[ConnGpio], &GUID_MTK_CONN_PINS_INTERFACE_V1,
                                         &Pins.Header, sizeof(Pins), MTK_CONN_PINS_INTERFACE_VERSION, NULL);
    NTSTATUS Cleanup = STATUS_SUCCESS;
    if (Status == STATUS_SUCCESS) {
        WdfWaitLockAcquire(Driver->Lock, NULL);
        if (ProvidersAvailable(Context)) {
            Status = T70ConnStart(Hardware, &Power, &Pins, Context->Firmware.Images);
            Context->Ready = Status == STATUS_SUCCESS;
            ConnSetScoInterruptReady(Context, Context->Ready);
            if (Context->Ready) {
                Context->BtifSession = 0;
            }
            if (!Context->Ready && Hardware->Emi) {
                // Failed D0Entry is not followed by D0Exit.
                Cleanup = T70ConnStop(Hardware);
            }
        } else {
            Status = STATUS_DEVICE_REMOVED;
        }
        WdfWaitLockRelease(Driver->Lock);
        ReleaseInterface(&Pins.Header);
    }
    ReleaseInterface(&Power.Header);
    if (Cleanup != STATUS_SUCCESS) {
        WdfDeviceSetFailed(Device, WdfDeviceFailedNoRestart);
        return Cleanup;
    }
    return ExactStatus(Status);
}

NTSTATUS ConnD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE Target)
{
    CONN_DRIVER *Driver = DriverFor(Device);
    CONN_DEVICE *Context = ConnDevice(Device);
    ConnSetScoInterruptReady(Context, FALSE);
    WdfWaitLockAcquire(Driver->Lock, NULL);
    Context->Ready = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;
    if (Target != WdfPowerDevicePrepareForHibernation && ProvidersAvailable(Context)) {
        Status = Driver->BtifToken ? STATUS_DEVICE_BUSY : T70ConnStop(&Driver->Hardware);
    }
    WdfWaitLockRelease(Driver->Lock);
    if (Status != STATUS_SUCCESS) {
        WdfDeviceSetFailed(Device, WdfDeviceFailedNoRestart);
    }
    return Status;
}

VOID ConnSurpriseRemoval(WDFDEVICE Device)
{
    CONN_DRIVER *Driver = DriverFor(Device);
    CONN_DEVICE *Context = ConnDevice(Device);
    ConnSetScoInterruptReady(Context, FALSE);
    WdfWaitLockAcquire(Driver->Lock, NULL);
    Context->Ready = FALSE;
    Context->SurpriseRemoved = TRUE;
    WdfWaitLockRelease(Driver->Lock);
}

NTSTATUS ConnReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Translated);
    CONN_DEVICE *Context = ConnDevice(Device);
    if (!Context->OwnsHardware) {
        return STATUS_SUCCESS;
    }

    CONN_DRIVER *Driver = DriverFor(Device);
    ConnSetScoInterruptReady(Context, FALSE);
    WDFIOTARGET Targets[ConnProviderCount];
    PDEVICE_OBJECT Providers[ConnProviderCount];
    WdfWaitLockAcquire(Driver->Lock, NULL);
    Context->Closing = TRUE;
    Context->Ready = FALSE;
    Driver->BtifToken = 0;
    RtlCopyMemory(Targets, Context->Targets, sizeof(Targets));
    RtlCopyMemory(Providers, Context->Providers, sizeof(Providers));
    RtlZeroMemory(Context->Targets, sizeof(Context->Targets));
    RtlZeroMemory(Context->Providers, sizeof(Context->Providers));
    NTSTATUS Status = T70ConnReleaseHardware(&Driver->Hardware);
    WdfWaitLockRelease(Driver->Lock);

    IoInvalidateDeviceRelations(WdfDeviceWdmGetPhysicalDevice(Device), PowerRelations);
    for (unsigned Index = 0; Index < ConnProviderCount; ++Index) {
        if (Targets[Index]) {
            WdfIoTargetClose(Targets[Index]);
            WdfObjectDelete(Targets[Index]);
        }
        if (Providers[Index]) {
            ObDereferenceObject(Providers[Index]);
        }
    }
    T70ConnFreeFirmware(&Context->Firmware);

    WdfWaitLockAcquire(Driver->Lock, NULL);
    Context->OwnsHardware = FALSE;
    Context->ScoInterrupt = NULL;
    Driver->Owner = NULL;
    WdfWaitLockRelease(Driver->Lock);
    return Status;
}

static BOOLEAN ContainsPdo(const DEVICE_RELATIONS *Relations, PDEVICE_OBJECT Pdo)
{
    if (Relations) {
        for (ULONG Index = 0; Index < Relations->Count; ++Index) {
            if (Relations->Objects[Index] == Pdo) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static NTSTATUS ReportPowerRelations(WDFDEVICE Device, PIRP Irp)
{
    CONN_DRIVER *Driver = DriverFor(Device);
    CONN_DEVICE *Context = ConnDevice(Device);
    WdfWaitLockAcquire(Driver->Lock, NULL);

    PDEVICE_RELATIONS Old = (PDEVICE_RELATIONS)Irp->IoStatus.Information;
    ULONG OldCount = Old ? Old->Count : 0;
    PDEVICE_OBJECT Add[ConnProviderCount];
    ULONG Count = 0;
    if (!Context->ProviderLost && !Context->Closing) {
        for (unsigned Index = 0; Index < ConnProviderCount; ++Index) {
            PDEVICE_OBJECT Pdo = Context->Providers[Index];
            if (Pdo && !ContainsPdo(Old, Pdo)) {
                Add[Count++] = Pdo;
            }
        }
    }
    NTSTATUS Status = STATUS_SUCCESS;
    if (OldCount > MAXULONG - Count) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    } else if (Count || !Old) {
        SIZE_T Size = FIELD_OFFSET(DEVICE_RELATIONS, Objects) +
                      (SIZE_T)(OldCount + Count) * sizeof(Add[0]);
        PDEVICE_RELATIONS Next = ExAllocatePool2(POOL_FLAG_PAGED, Size, CONN_RELATIONS_TAG);
        if (!Next) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            Next->Count = OldCount + Count;
            if (OldCount) {
                RtlCopyMemory(Next->Objects, Old->Objects, OldCount * sizeof(Add[0]));
            }
            for (ULONG Index = 0; Index < Count; ++Index) {
                ObReferenceObject(Add[Index]);
                Next->Objects[OldCount + Index] = Add[Index];
            }
            if (Old) {
                ExFreePool(Old);
            }
            Irp->IoStatus.Information = (ULONG_PTR)Next;
            Irp->IoStatus.Status = STATUS_SUCCESS;
        }
    }
    WdfWaitLockRelease(Driver->Lock);
    return Status;
}

NTSTATUS ConnQueryRelations(WDFDEVICE Device, PIRP Irp)
{
    if (IoGetCurrentIrpStackLocation(Irp)->Parameters.QueryDeviceRelations.Type == PowerRelations) {
        NTSTATUS Status = ReportPowerRelations(Device, Irp);
        if (!NT_SUCCESS(Status)) {
            Irp->IoStatus.Status = Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;
        }
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
}

NTSTATUS ConnAddDevice(WDFDRIVER Driver, PWDFDEVICE_INIT Init)
{
    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS Callbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Callbacks);
    Callbacks.EvtDevicePrepareHardware = ConnPrepareHardware;
    Callbacks.EvtDeviceReleaseHardware = ConnReleaseHardware;
    Callbacks.EvtDeviceD0Entry = ConnD0Entry;
    Callbacks.EvtDeviceD0Exit = ConnD0Exit;
    Callbacks.EvtDeviceSurpriseRemoval = ConnSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init, &Callbacks);

    UCHAR Minor = IRP_MN_QUERY_DEVICE_RELATIONS;
    NTSTATUS Status = WdfDeviceInitAssignWdmIrpPreprocessCallback(Init, ConnQueryRelations, IRP_MJ_PNP, &Minor, 1);
    if (Status != STATUS_SUCCESS) {
        return ExactStatus(Status);
    }
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, CONN_DEVICE);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    WDFDEVICE Device;
    Status = WdfDeviceCreate(&Init, &Attributes, &Device);
    if (Status != STATUS_SUCCESS) {
        return ExactStatus(Status);
    }
    T70_CONN_BTIF_INTERFACE Btif = {
        {FIELD_OFFSET(T70_CONN_BTIF_INTERFACE, ClaimSession), 1, Device,
         BtifInterfaceReference, BtifInterfaceDereference},
        BtifAcquire, BtifAccess, BtifRelease, BtifClaimSession
    };
    WDF_QUERY_INTERFACE_CONFIG Query;
    WDF_QUERY_INTERFACE_CONFIG_INIT(&Query, &Btif.Header, &GUID_T70_CONN_BTIF_INTERFACE_V1, NULL);
    Status = WdfDeviceAddQueryInterface(Device, &Query);
    if (Status == STATUS_SUCCESS) {
        Btif.Header.Size = sizeof(Btif);
        Btif.Header.Version = T70_CONN_BTIF_INTERFACE_VERSION;
        WDF_QUERY_INTERFACE_CONFIG_INIT(&Query, &Btif.Header, &GUID_T70_CONN_BTIF_INTERFACE_V2, NULL);
        Status = WdfDeviceAddQueryInterface(Device, &Query);
    }
    return Status == STATUS_SUCCESS ?
        WdfDeviceCreateDeviceInterface(Device, &GUID_DEVINTERFACE_T70_CONN_BTIF, NULL) :
        ExactStatus(Status);
}

VOID ConnUnload(WDFDRIVER Driver)
{
    T70_CONN_BOOTSTRAP *Hardware = &ConnDriver(Driver)->Hardware;
    if (Hardware->Emi) {
        // Resources are gone. Providers keep any outstanding lease alive.
        // The CONN remap may still reference EMI, so keep those pages pinned.
        ReleaseInterface(&Hardware->Pins.Header);
        ReleaseInterface(&Hardware->Power.Header);
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "MtkConn: DMA reservation still in use on unload\n");
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT Object, PUNICODE_STRING Path)
{
    WDF_DRIVER_CONFIG Config;
    WDF_DRIVER_CONFIG_INIT(&Config, ConnAddDevice);
    Config.EvtDriverUnload = ConnUnload;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, CONN_DRIVER);
    WDFDRIVER Driver;
    NTSTATUS Status = WdfDriverCreate(Object, Path, &Attributes, &Config, &Driver);
    if (Status != STATUS_SUCCESS) {
        return ExactStatus(Status);
    }
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Driver;
    return WdfWaitLockCreate(&Attributes, &ConnDriver(Driver)->Lock);
}
