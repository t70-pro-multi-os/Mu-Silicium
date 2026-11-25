// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef CONN_TEST_WDF_H
#define CONN_TEST_WDF_H

#include <ntifs.h>

typedef void *WDFOBJECT, *WDFDRIVER, *WDFDEVICE, *WDFIOTARGET, *WDFWAITLOCK, *WDFINTERRUPT;
typedef void *PWDFDEVICE_INIT, *WDFCMRESLIST;
typedef wchar_t WCHAR, *PWSTR;
typedef void *PDEVICE_OBJECT;
typedef uintptr_t ULONG_PTR;

typedef enum { TargetDeviceRelation, PowerRelations } DEVICE_RELATION_TYPE;
typedef struct {
    ULONG Count;
    PDEVICE_OBJECT Objects[1];
} DEVICE_RELATIONS, *PDEVICE_RELATIONS;
typedef struct {
    struct { struct { DEVICE_RELATION_TYPE Type; } QueryDeviceRelations; } Parameters;
} IO_STACK_LOCATION;
typedef struct {
    struct { NTSTATUS Status; ULONG_PTR Information; } IoStatus;
    IO_STACK_LOCATION Current;
    int Skipped;
    int Completed;
} IRP, *PIRP;

typedef enum { WdfPowerDeviceD0, WdfPowerDeviceD3, WdfPowerDeviceD3Final,
               WdfPowerDevicePrepareForHibernation } WDF_POWER_DEVICE_STATE;
typedef NTSTATUS EVT_WDF_DRIVER_DEVICE_ADD(WDFDRIVER, PWDFDEVICE_INIT);
typedef VOID EVT_WDF_DRIVER_UNLOAD(WDFDRIVER);
typedef NTSTATUS EVT_WDF_DEVICE_PREPARE_HARDWARE(WDFDEVICE, WDFCMRESLIST, WDFCMRESLIST);
typedef NTSTATUS EVT_WDF_DEVICE_RELEASE_HARDWARE(WDFDEVICE, WDFCMRESLIST);
typedef NTSTATUS EVT_WDF_DEVICE_D0_ENTRY(WDFDEVICE, WDF_POWER_DEVICE_STATE);
typedef NTSTATUS EVT_WDF_DEVICE_D0_EXIT(WDFDEVICE, WDF_POWER_DEVICE_STATE);
typedef VOID EVT_WDF_DEVICE_SURPRISE_REMOVAL(WDFDEVICE);
typedef NTSTATUS EVT_WDFDEVICE_WDM_IRP_PREPROCESS(WDFDEVICE, PIRP);
typedef BOOLEAN EVT_WDF_INTERRUPT_ISR(WDFINTERRUPT, ULONG);
typedef NTSTATUS EVT_WDF_IO_TARGET_QUERY_REMOVE(WDFIOTARGET);
typedef VOID EVT_WDF_IO_TARGET_REMOVE_COMPLETE(WDFIOTARGET);

typedef struct {
    EVT_WDF_DEVICE_PREPARE_HARDWARE *EvtDevicePrepareHardware;
    EVT_WDF_DEVICE_RELEASE_HARDWARE *EvtDeviceReleaseHardware;
    EVT_WDF_DEVICE_D0_ENTRY *EvtDeviceD0Entry;
    EVT_WDF_DEVICE_D0_EXIT *EvtDeviceD0Exit;
    EVT_WDF_DEVICE_SURPRISE_REMOVAL *EvtDeviceSurpriseRemoval;
} WDF_PNPPOWER_EVENT_CALLBACKS;
typedef struct {
    EVT_WDF_DRIVER_DEVICE_ADD *EvtDriverDeviceAdd;
    EVT_WDF_DRIVER_UNLOAD *EvtDriverUnload;
} WDF_DRIVER_CONFIG;
typedef struct {
    unsigned ExecutionLevel, SynchronizationScope;
    size_t ContextSize;
    WDFOBJECT ParentObject;
} WDF_OBJECT_ATTRIBUTES;
typedef struct {
    INTERFACE *Interface;
    const GUID *InterfaceType;
} WDF_QUERY_INTERFACE_CONFIG;
typedef struct {
    UNICODE_STRING Name;
    ULONG Access, ShareAccess;
    EVT_WDF_IO_TARGET_QUERY_REMOVE *EvtIoTargetQueryRemove;
    EVT_WDF_IO_TARGET_REMOVE_COMPLETE *EvtIoTargetRemoveComplete;
} WDF_IO_TARGET_OPEN_PARAMS;
typedef VOID EVT_WDF_INTERRUPT_WORKITEM(WDFINTERRUPT, WDFOBJECT);
typedef struct {
    EVT_WDF_INTERRUPT_ISR *EvtInterruptIsr;
    EVT_WDF_INTERRUPT_WORKITEM *EvtInterruptWorkItem;
    WDFWAITLOCK WaitLock;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR InterruptRaw, InterruptTranslated;
    BOOLEAN PassiveHandling;
} WDF_INTERRUPT_CONFIG;

#define WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Type, Name) \
    static Type *Name(WDFOBJECT Object) { return (Type *)TestWdfContext(Object); }
#define WDF_OBJECT_ATTRIBUTES_INIT(Attrs) memset((Attrs), 0, sizeof(*(Attrs)))
#define WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(Attrs, Type) \
    do { WDF_OBJECT_ATTRIBUTES_INIT(Attrs); (Attrs)->ContextSize = sizeof(Type); } while (0)
#define WDF_DRIVER_CONFIG_INIT(Config, Add) (*(Config) = (WDF_DRIVER_CONFIG){.EvtDriverDeviceAdd = (Add)})
#define WDF_PNPPOWER_EVENT_CALLBACKS_INIT(Callbacks) memset((Callbacks), 0, sizeof(*(Callbacks)))
#define WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(Params, Path, Flags) \
    (*(Params) = (WDF_IO_TARGET_OPEN_PARAMS){.Name = *(Path), .Access = (Flags)})
#define WDF_INTERRUPT_CONFIG_INIT(Config, Isr, Dpc) \
    do { (void)(Dpc); memset((Config), 0, sizeof(*(Config))); (Config)->EvtInterruptIsr = (Isr); } while (0)
#define WDF_QUERY_INTERFACE_CONFIG_INIT(Config, Fn, Guid, Callback) \
    do { (void)(Callback); *(Config) = (WDF_QUERY_INTERFACE_CONFIG){(Fn), (Guid)}; } while (0)
#define WDF_NO_OBJECT_ATTRIBUTES NULL
typedef LONG KPRIORITY;
#define IO_NO_INCREMENT 0
#define WdfExecutionLevelPassive 1
#define WdfSynchronizationScopeNone 1
#define WdfDeviceFailedNoRestart 1
#define FILE_SHARE_WRITE 2U
#define STATUS_DEVICE_REMOVED ((NTSTATUS)0xc00002b6)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xc00000bb)
#define IRP_MJ_PNP 0x1b
#define IRP_MN_QUERY_DEVICE_RELATIONS 7
#define DPFLTR_IHVDRIVER_ID 77
#define DPFLTR_ERROR_LEVEL 0
#define IoGetCurrentIrpStackLocation(Irp) (&(Irp)->Current)
#define IoSkipCurrentIrpStackLocation(Irp) (++(Irp)->Skipped)
#define FIELD_OFFSET(Type, Field) offsetof(Type, Field)

PVOID TestWdfContext(WDFOBJECT);
WDFDRIVER WdfDeviceGetDriver(WDFDEVICE);
WDFDEVICE WdfIoTargetGetDevice(WDFIOTARGET);
PDEVICE_OBJECT WdfDeviceWdmGetPhysicalDevice(WDFDEVICE);
PDRIVER_OBJECT WdfDriverWdmGetDriverObject(WDFDRIVER);
VOID WdfWaitLockAcquire(WDFWAITLOCK, const LARGE_INTEGER *);
VOID WdfWaitLockRelease(WDFWAITLOCK);
VOID WdfInterruptAcquireLock(WDFINTERRUPT);
VOID WdfInterruptReleaseLock(WDFINTERRUPT);
VOID IoCompleteRequest(PIRP, KPRIORITY);
NTSTATUS WdfWaitLockCreate(WDF_OBJECT_ATTRIBUTES *, WDFWAITLOCK *);
NTSTATUS WdfIoTargetCreate(WDFDEVICE, WDF_OBJECT_ATTRIBUTES *, WDFIOTARGET *);
NTSTATUS WdfIoTargetOpen(WDFIOTARGET, WDF_IO_TARGET_OPEN_PARAMS *);
NTSTATUS WdfIoTargetQueryForInterface(WDFIOTARGET, const GUID *, INTERFACE *, USHORT, USHORT, void *);
NTSTATUS WdfInterruptCreate(WDFDEVICE, WDF_INTERRUPT_CONFIG *, WDF_OBJECT_ATTRIBUTES *, WDFINTERRUPT *);
WDFDEVICE WdfInterruptGetDevice(WDFINTERRUPT);
PDEVICE_OBJECT WdfIoTargetWdmGetTargetPhysicalDevice(WDFIOTARGET);
VOID WdfIoTargetClose(WDFIOTARGET);
VOID WdfDeviceSetFailed(WDFDEVICE, ULONG);
VOID WdfObjectDelete(WDFOBJECT);
VOID WdfObjectReference(WDFOBJECT);
VOID WdfObjectDereference(WDFOBJECT);
NTSTATUS WdfDeviceAddQueryInterface(WDFDEVICE, WDF_QUERY_INTERFACE_CONFIG *);
NTSTATUS WdfDeviceCreateDeviceInterface(WDFDEVICE, const GUID *, const UNICODE_STRING *);
NTSTATUS WdfDeviceWdmDispatchPreprocessedIrp(WDFDEVICE, PIRP);
ULONG WdfCmResourceListGetCount(WDFCMRESLIST);
PCM_PARTIAL_RESOURCE_DESCRIPTOR WdfCmResourceListGetDescriptor(WDFCMRESLIST, ULONG);
VOID WdfDeviceInitSetPnpPowerEventCallbacks(PWDFDEVICE_INIT, WDF_PNPPOWER_EVENT_CALLBACKS *);
NTSTATUS WdfDeviceInitAssignWdmIrpPreprocessCallback(PWDFDEVICE_INIT, EVT_WDFDEVICE_WDM_IRP_PREPROCESS *,
                                                      UCHAR, UCHAR *, ULONG);
NTSTATUS WdfDeviceCreate(PWDFDEVICE_INIT *, WDF_OBJECT_ATTRIBUTES *, WDFDEVICE *);
NTSTATUS WdfDriverCreate(PDRIVER_OBJECT, PUNICODE_STRING, WDF_OBJECT_ATTRIBUTES *,
                         WDF_DRIVER_CONFIG *, WDFDRIVER *);
NTSTATUS IoGetDeviceInterfaces(const GUID *, PDEVICE_OBJECT, ULONG, PWSTR *);
VOID IoInvalidateDeviceRelations(PDEVICE_OBJECT, DEVICE_RELATION_TYPE);
VOID ObReferenceObject(PDEVICE_OBJECT);
VOID ObDereferenceObject(PDEVICE_OBJECT);
VOID ExFreePool(PVOID);
ULONG DbgPrintEx(ULONG, ULONG, const char *, ...);

#endif
