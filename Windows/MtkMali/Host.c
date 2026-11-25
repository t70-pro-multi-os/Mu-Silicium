/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Soc.h"
#define BH_SIGNATURE 0x48423730u
#define BH_TAG 0x48303754u
typedef struct {
    ULONG Signature,Role;
    PDEVICE_OBJECT Device,Pdo,Lower;
    IO_REMOVE_LOCK RemoveLock;
    PIO_WORKITEM Reap;
    volatile LONG InterfacePins,Removing,RemoveDone,ReapState,PnpActive;
    volatile LONG ConnPublished;
    UNICODE_STRING ConnLink;
    ULONG ParentHeld,Opened,BoardInitialized,SocInitialized,Quarantined;
    NTSTATUS CleanupStatus;
    MALI_BOARD_PARENT_INTERFACE Parent;
    void *ParentCookie;
    MALI_BOARD_CONFIG Config;
    MALI_BOARD_PROVIDER Board;
    MALI_SOC_PROVIDER Soc;
} MALI_BOARD_HOST;
static KMUTEX BhSocRegistry;
static MALI_BOARD_HOST *BhSocOwner;
static const GUID BhParentGuid=MALI_BOARD_PARENT_INTERFACE_GUID_INIT;
static void BhRequestReap(MALI_BOARD_HOST *H);
static NTSTATUS BhExact(NTSTATUS S) {return S!=STATUS_SUCCESS && NT_SUCCESS(S)?STATUS_UNSUCCESSFUL:S;}
static MALI_BOARD_HOST *BhHost(PDEVICE_OBJECT Device) {
    MALI_BOARD_HOST *H=Device?Device->DeviceExtension:0;
    return H && H->Signature==BH_SIGNATURE && H->Device==Device?H:0;
}
static void BhReference(void *Context) {
    MALI_BOARD_HOST *H=Context;ObReferenceObject(H->Device);
    InterlockedIncrement(&H->InterfacePins);
}
static void BhDereference(void *Context) {
    MALI_BOARD_HOST *H=Context;PDEVICE_OBJECT Device=H->Device;
    if(InterlockedDecrement(&H->InterfacePins)<0) {
        if(H->Role)InterlockedExchange((volatile LONG *)&H->Soc.ReferenceFault,1);
        else InterlockedExchange(&H->Board.ReferenceFault,1);return;
    }
    BhRequestReap(H);ObDereferenceObject(Device);
}
/* No interface lock participates in IRP remove rundown: a poisoned lease must
   retain its objects without hanging the system's REMOVE_DEVICE request. */
static NTSTATUS BhCleanup(MALI_BOARD_HOST *H) {
    if(InterlockedCompareExchange(&H->InterfacePins,0,0))return STATUS_DEVICE_BUSY;
    NTSTATUS S=STATUS_SUCCESS;
    if(H->Role)return H->SocInitialized?MtkMaliSocClose(&H->Soc):STATUS_SUCCESS;
    if(H->BoardInitialized) {
        if(H->Board.Pdo) {
            S=MtkMaliBoardUnbindResources(&H->Board,H->Pdo);
            if(S!=STATUS_SUCCESS)return S;
        }
        S=MtkMaliBoardProviderClose(&H->Board);if(S!=STATUS_SUCCESS)return S;
    }
    if(H->Opened && !H->ParentCookie)return STATUS_DRIVER_INTERNAL_ERROR;
    if(H->ParentCookie) {
        if(!H->Parent.Close)return STATUS_DRIVER_INTERNAL_ERROR;
        S=BhExact(H->Parent.Close(H->Parent.Header.Context,H->ParentCookie));
        if(S!=STATUS_SUCCESS)return S;
        H->ParentCookie=0;H->Opened=0;
    }
    if(H->ParentHeld) {
        if(!H->Parent.Header.InterfaceDereference)return STATUS_DRIVER_INTERNAL_ERROR;
        H->ParentHeld=0;H->Parent.Header.InterfaceDereference(H->Parent.Header.Context);
    }
    return STATUS_SUCCESS;
}
static void BhReap(PDEVICE_OBJECT Device,void *Context) {
    MALI_BOARD_HOST *H=Context;(void)Device;
    for(;;) {
        H->CleanupStatus=BhCleanup(H);
        if(H->CleanupStatus==STATUS_SUCCESS) {
            /* No valid reference can reappear after both admission and remove
               rundown closed. An I/O work item pins the device/code to return. */
            InterlockedExchange(&H->ReapState,3);
            PDEVICE_OBJECT Pdo=H->Pdo;PIO_WORKITEM Work=H->Reap;
            H->Reap=0;H->Signature=0;
            if(H->ConnLink.Buffer)RtlFreeUnicodeString(&H->ConnLink);
            IoFreeWorkItem(Work);ObDereferenceObject(Pdo);IoDeleteDevice(Device);return;
        }
        H->Quarantined=1;
        /* 0 idle, 1 queued/running, 2 notified while running, 3 destroyed.
           The last dereference cannot be lost while this worker goes idle. */
        LONG Previous=InterlockedCompareExchange(&H->ReapState,0,1);
        if(Previous==1)return;
        if(Previous!=2)return;
        InterlockedCompareExchange(&H->ReapState,1,2);
    }
}
static void BhRequestReap(MALI_BOARD_HOST *H) {
    if(!InterlockedCompareExchange(&H->RemoveDone,0,0))return;
    for(;;) {
        LONG State=InterlockedCompareExchange(&H->ReapState,0,0);
        if(State==3 || State==2)return;
        if(State==1) {
            if(InterlockedCompareExchange(&H->ReapState,2,1)==1)return;
        } else if(InterlockedCompareExchange(&H->ReapState,1,0)==0) {
            IoQueueWorkItem(H->Reap,BhReap,DelayedWorkQueue,H);return;
        }
    }
}

static NTSTATUS BhSocLookup(MALI_BOARD_HOST *H) {
    NTSTATUS S=KeWaitForSingleObject(&BhSocRegistry,Executive,KernelMode,FALSE,0);
    if(S!=STATUS_SUCCESS)return BhExact(S);
    S=BhSocOwner?MtkMaliSocGetFactory(&BhSocOwner->Soc,H->Pdo,&H->Parent):STATUS_NOT_SUPPORTED;
    KeReleaseMutex(&BhSocRegistry,FALSE);return BhExact(S);
}
static NTSTATUS BhSocPublish(MALI_BOARD_HOST *H,BOOLEAN Publish) {
    NTSTATUS S=KeWaitForSingleObject(&BhSocRegistry,Executive,KernelMode,FALSE,0);
    if(S!=STATUS_SUCCESS)return BhExact(S);
    if(Publish) {
        if(BhSocOwner && BhSocOwner!=H)S=STATUS_DEVICE_BUSY;
        else BhSocOwner=H;
    } else if(BhSocOwner==H)BhSocOwner=0;
    KeReleaseMutex(&BhSocRegistry,FALSE);return BhExact(S);
}
static NTSTATUS BhSocPublishStart(MALI_BOARD_HOST *H) {
    NTSTATUS S=IoSetDeviceInterfaceState(&H->ConnLink,TRUE);
    if(!NT_SUCCESS(S))return S;
    S=BhSocPublish(H,TRUE);
    if(S==STATUS_SUCCESS)InterlockedExchange(&H->ConnPublished,1);
    else (void)IoSetDeviceInterfaceState(&H->ConnLink,FALSE);
    return S;
}
static void BhConnUnpublish(MALI_BOARD_HOST *H) {
    if(InterlockedExchange(&H->ConnPublished,0))
        (void)IoSetDeviceInterfaceState(&H->ConnLink,FALSE);
}
static NTSTATUS BhQueryConnPower(MALI_BOARD_HOST *H,PIO_STACK_LOCATION Stack) {
    if(InterlockedCompareExchange(&H->Removing,0,0))return STATUS_DEVICE_REMOVED;
    if(!InterlockedCompareExchange(&H->ConnPublished,0,0))return STATUS_DEVICE_NOT_READY;
    if(Stack->Parameters.QueryInterface.Version!=MTK_CONN_POWER_INTERFACE_VERSION)return STATUS_NOT_SUPPORTED;
    if(Stack->Parameters.QueryInterface.Size<sizeof(MTK_CONN_POWER_INTERFACE))return STATUS_BUFFER_TOO_SMALL;
    return MtkMaliSocGetConnPower(&H->Soc,(MTK_CONN_POWER_INTERFACE *)Stack->Parameters.QueryInterface.Interface);
}
static void BhPnpWait(MALI_BOARD_HOST *H) {
    LARGE_INTEGER Delay;Delay.QuadPart=-10000;
    while(InterlockedCompareExchange(&H->PnpActive,1,0))
        KeDelayExecutionThread(KernelMode,FALSE,&Delay);
}
static NTSTATUS BhSocPnp(MALI_BOARD_HOST *H,PIRP Irp) {
    PIO_STACK_LOCATION Stack=IoGetCurrentIrpStackLocation(Irp);UCHAR Minor=Stack->MinorFunction;
    int Forced=Minor==IRP_MN_REMOVE_DEVICE || Minor==IRP_MN_SURPRISE_REMOVAL;
    if(Forced)BhPnpWait(H);
    else if(InterlockedCompareExchange(&H->PnpActive,1,0)){Irp->IoStatus.Status=STATUS_DEVICE_BUSY;return STATUS_DEVICE_BUSY;}
    NTSTATUS S=STATUS_SUCCESS;
    if(Forced) {
        S=BhSocPublish(H,FALSE);
        NTSTATUS Withdraw=MtkMaliSocWithdraw(&H->Soc);
        if(S==STATUS_SUCCESS)S=Withdraw;
        BhConnUnpublish(H);
        if(S!=STATUS_SUCCESS)H->Soc.Quarantined=1;
    } else switch(Minor) {
    case IRP_MN_START_DEVICE:
        if(H->Soc.Started)S=STATUS_DEVICE_BUSY;
        break;
    case IRP_MN_QUERY_STOP_DEVICE:case IRP_MN_QUERY_REMOVE_DEVICE:case IRP_MN_STOP_DEVICE:
        S=MtkMaliSocSetStopping(&H->Soc,Minor);break;
    default:break;
    }
    if(S==STATUS_SUCCESS || Forced) {
        NTSTATUS Local=S;
        /* Preserve unknown PnP requests; claim the requests owned here. */
        if(Forced || Minor==IRP_MN_START_DEVICE || Minor==IRP_MN_QUERY_STOP_DEVICE ||
           Minor==IRP_MN_QUERY_REMOVE_DEVICE || Minor==IRP_MN_STOP_DEVICE ||
           Minor==IRP_MN_CANCEL_STOP_DEVICE || Minor==IRP_MN_CANCEL_REMOVE_DEVICE) {
            Irp->IoStatus.Status=STATUS_SUCCESS;Irp->IoStatus.Information=0;
        }
        S=BhExact(MtkMaliBoardForwardPnp(H->Lower,Irp));
        if(S==STATUS_SUCCESS && Local!=STATUS_SUCCESS)S=Local;
        if(S==STATUS_SUCCESS)switch(Minor) {
        case IRP_MN_START_DEVICE:
            S=MtkMaliSocStart(&H->Soc,Stack->Parameters.StartDevice.AllocatedResources,
                Stack->Parameters.StartDevice.AllocatedResourcesTranslated);
            if(S==STATUS_SUCCESS) {
                S=BhSocPublishStart(H);
                if(S!=STATUS_SUCCESS)(void)MtkMaliSocStop(&H->Soc);
            }
            break;
        case IRP_MN_STOP_DEVICE:
            BhConnUnpublish(H);
            S=BhSocPublish(H,FALSE);if(S==STATUS_SUCCESS)S=MtkMaliSocStop(&H->Soc);break;
        case IRP_MN_CANCEL_STOP_DEVICE:case IRP_MN_CANCEL_REMOVE_DEVICE:
            S=MtkMaliSocCancelStopping(&H->Soc,Minor);break;
        default:break;
        }
    }
    InterlockedExchange(&H->PnpActive,0);Irp->IoStatus.Status=S;return S;
}
static NTSTATUS BhPowerComplete(PDEVICE_OBJECT Device,PIRP Irp,void *Context) {
    (void)Device;(void)Irp;KeSetEvent((PKEVENT)Context,IO_NO_INCREMENT,FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}
static NTSTATUS BhSocPower(MALI_BOARD_HOST *H,PIRP Irp) {
    PIO_STACK_LOCATION Stack=IoGetCurrentIrpStackLocation(Irp);
    DEVICE_POWER_STATE State=Stack->Parameters.Power.State.DeviceState;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
    if(State!=PowerDeviceD0 && State!=PowerDeviceD3)return STATUS_NOT_SUPPORTED;
    NTSTATUS S=State==PowerDeviceD3?MtkMaliSocSuspend(&H->Soc):MtkMaliSocFencePower(&H->Soc);
    if(S!=STATUS_SUCCESS)return S;
    KEVENT Event;KeInitializeEvent(&Event,NotificationEvent,FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,BhPowerComplete,&Event,TRUE,TRUE,TRUE);
    (void)PoCallDriver(H->Lower,Irp);
    KeWaitForSingleObject(&Event,Executive,KernelMode,FALSE,0);
    S=BhExact(Irp->IoStatus.Status);
    if(S==STATUS_SUCCESS && State==PowerDeviceD0)S=MtkMaliSocResume(&H->Soc);
    else if(S!=STATUS_SUCCESS && State==PowerDeviceD3)(void)MtkMaliSocResume(&H->Soc);
    if(S==STATUS_SUCCESS)PoSetPowerState(H->Device,DevicePowerState,Stack->Parameters.Power.State);
    return S;
}

static NTSTATUS BhQueryParent(MALI_BOARD_HOST *H) {
    PDEVICE_OBJECT Target=IoGetAttachedDeviceReference(H->Pdo);
    if(!Target)return STATUS_NO_SUCH_DEVICE;
    KEVENT Event;IO_STATUS_BLOCK Result={0};KeInitializeEvent(&Event,NotificationEvent,FALSE);
    PIRP Irp=IoBuildSynchronousFsdRequest(IRP_MJ_PNP,Target,0,0,0,&Event,&Result);
    if(!Irp){ObDereferenceObject(Target);return STATUS_INSUFFICIENT_RESOURCES;}
    PIO_STACK_LOCATION Stack=IoGetNextIrpStackLocation(Irp);
    Stack->MinorFunction=IRP_MN_QUERY_INTERFACE;
    Stack->Parameters.QueryInterface.InterfaceType=(LPGUID)&BhParentGuid;
    Stack->Parameters.QueryInterface.Size=sizeof(H->Parent);
    Stack->Parameters.QueryInterface.Version=1;
    Stack->Parameters.QueryInterface.Interface=(PINTERFACE)&H->Parent;
    MALI_BOARD_PARENT_QUERY Query={sizeof(Query),1,H->Pdo};
    Stack->Parameters.QueryInterface.InterfaceSpecificData=&Query;
    Irp->IoStatus.Status=STATUS_NOT_SUPPORTED;
    (void)IoCallDriver(Target,Irp);
    KeWaitForSingleObject(&Event,Executive,KernelMode,FALSE,0);
    ObDereferenceObject(Target);
    NTSTATUS S=BhExact(Result.Status);
    if(S==STATUS_NOT_SUPPORTED) {
        MALI_BOARD_PARENT_INTERFACE Empty={0};
        if(RtlEqualMemory(&H->Parent,&Empty,sizeof(Empty)))S=BhSocLookup(H);
    }
    if(S!=STATUS_SUCCESS)return S;
    H->ParentHeld=1;
    if(H->Parent.Header.Size!=sizeof(H->Parent) || H->Parent.Header.Version!=1 ||
       !H->Parent.Header.InterfaceReference || !H->Parent.Header.InterfaceDereference ||
       !H->Parent.Open || !H->Parent.Close)return STATUS_DRIVER_INTERNAL_ERROR;
    H->Opened=1;
    S=BhExact(H->Parent.Open(H->Parent.Header.Context,H->Pdo,&H->Config,&H->ParentCookie));
    if(S!=STATUS_SUCCESS){if(!H->ParentCookie)H->Opened=0;return S;}
    if(!H->ParentCookie || H->Config.Lifetime.Reference || H->Config.Lifetime.Dereference || H->Config.Lifetime.Context)
        return STATUS_DRIVER_INTERNAL_ERROR;
    H->Opened=1;H->Config.Lifetime.Context=H;
    H->Config.Lifetime.Reference=BhReference;H->Config.Lifetime.Dereference=BhDereference;
    S=MtkMaliBoardProviderInitialize(&H->Board,&H->Config);
    if(S==STATUS_SUCCESS)H->BoardInitialized=1;
    return S;
}
/* _DEP orders START, not AddDevice. Keep an unacquired filter attached when
   the resource owner is not published yet; query only after lower START. */
static NTSTATUS BhDeferredPnp(MALI_BOARD_HOST *H,PIRP Irp) {
    PIO_STACK_LOCATION Stack=IoGetCurrentIrpStackLocation(Irp);UCHAR Minor=Stack->MinorFunction;
    BOOLEAN Forced=Minor==IRP_MN_REMOVE_DEVICE || Minor==IRP_MN_SURPRISE_REMOVAL;
    if(Forced)BhPnpWait(H);
    else if(InterlockedCompareExchange(&H->PnpActive,1,0)){Irp->IoStatus.Status=STATUS_DEVICE_BUSY;return STATUS_DEVICE_BUSY;}
    /* A START admitted before this forced request may have created Board. */
    if(Forced && H->BoardInitialized) {
        InterlockedExchange(&H->PnpActive,0);
        return MtkMaliBoardProcessPnp(&H->Board,H->Pdo,H->Lower,Irp);
    }
    if(Forced || Minor==IRP_MN_START_DEVICE || Minor==IRP_MN_STOP_DEVICE ||
       Minor==IRP_MN_QUERY_STOP_DEVICE || Minor==IRP_MN_QUERY_REMOVE_DEVICE ||
       Minor==IRP_MN_CANCEL_STOP_DEVICE || Minor==IRP_MN_CANCEL_REMOVE_DEVICE) {
        Irp->IoStatus.Status=STATUS_SUCCESS;Irp->IoStatus.Information=0;
    }
    NTSTATUS S=BhExact(MtkMaliBoardForwardPnp(H->Lower,Irp));
    if(S==STATUS_SUCCESS && Minor==IRP_MN_START_DEVICE) {
        if(H->Removing)S=STATUS_DELETE_PENDING;
        else if(H->ParentHeld || H->Opened || H->ParentCookie)S=STATUS_DEVICE_BUSY;
        else {
            S=BhQueryParent(H);
            if(S==STATUS_SUCCESS) {
                H->Board.Stopping=IRP_MN_STOP_DEVICE;
                S=MtkMaliBoardBindResources(&H->Board,H->Pdo,
                    Stack->Parameters.StartDevice.AllocatedResources,Stack->Parameters.StartDevice.AllocatedResourcesTranslated);
            }
        }
    }
    InterlockedExchange(&H->PnpActive,0);Irp->IoStatus.Status=S;return S;
}
static NTSTATUS BhHardwareId(PDEVICE_OBJECT Pdo,BOOLEAN Root) {
    WCHAR Buffer[128],Text[]=L"ACPI\\HANA678E";ULONG Bytes=0;
    if(Root)Text[12]=L'F';
    UNICODE_STRING Expected={sizeof(Text)-sizeof(WCHAR),sizeof(Text),Text};
    NTSTATUS S=IoGetDeviceProperty(Pdo,DevicePropertyHardwareID,sizeof(Buffer),Buffer,&Bytes);
    if(S!=STATUS_SUCCESS)return BhExact(S);
    if(Bytes<2*sizeof(WCHAR) || Bytes>sizeof(Buffer) || (Bytes&1) ||
       Buffer[Bytes/sizeof(WCHAR)-1] || Buffer[Bytes/sizeof(WCHAR)-2])return STATUS_DEVICE_CONFIGURATION_ERROR;
    ULONG Offset=0,Count=Bytes/sizeof(WCHAR);int Found=0;
    while(Offset<Count && Buffer[Offset]) {
        ULONG End=Offset;while(End<Count && Buffer[End])++End;
        UNICODE_STRING Id={(USHORT)((End-Offset)*sizeof(WCHAR)),(USHORT)((End-Offset)*sizeof(WCHAR)),Buffer+Offset};
        if(RtlEqualUnicodeString(&Id,&Expected,TRUE))Found=1;
        Offset=End+1;
    }
    return Found?STATUS_SUCCESS:STATUS_NO_SUCH_DEVICE;
}
static NTSTATUS BhPassComplete(PDEVICE_OBJECT Device,PIRP Irp,void *Context) {
    MALI_BOARD_HOST *H=Context;(void)Device;
    if(Irp->PendingReturned)IoMarkIrpPending(Irp);
    IoReleaseRemoveLock(&H->RemoveLock,Irp);return STATUS_CONTINUE_COMPLETION;
}
NTSTATUS MtkMaliBoardHostDispatch(PDEVICE_OBJECT Device,PIRP Irp) {
    MALI_BOARD_HOST *H=BhHost(Device);NTSTATUS S=STATUS_INVALID_PARAMETER;
    if(!Irp)return S;
    if(!H){Irp->IoStatus.Status=S;IoCompleteRequest(Irp,IO_NO_INCREMENT);return S;}
    PIO_STACK_LOCATION Stack=IoGetCurrentIrpStackLocation(Irp);
    if(!Stack || Irp->CurrentLocation<=1 || Irp->CurrentLocation>Irp->StackCount ||
       (Stack->MajorFunction==IRP_MJ_PNP && (KeGetCurrentIrql()!=PASSIVE_LEVEL || Irp->RequestorMode!=KernelMode))) {
        Irp->IoStatus.Status=S;IoCompleteRequest(Irp,IO_NO_INCREMENT);return S;
    }
    S=IoAcquireRemoveLock(&H->RemoveLock,Irp);
    if(S!=STATUS_SUCCESS){Irp->IoStatus.Status=S;IoCompleteRequest(Irp,IO_NO_INCREMENT);return S;}
    if(Stack->MajorFunction==IRP_MJ_PNP) {
        UCHAR Minor=Stack->MinorFunction;
        if(H->Role && Minor==IRP_MN_QUERY_INTERFACE) {
            const GUID *Guid=Stack->Parameters.QueryInterface.InterfaceType;
            if(!Guid || !RtlEqualMemory(Guid,&GUID_MTK_CONN_POWER_INTERFACE_V1,sizeof(*Guid)))goto Forward;
            S=BhQueryConnPower(H,Stack);
            Irp->IoStatus.Status=S;Irp->IoStatus.Information=0;
        } else {
            if(Minor==IRP_MN_SURPRISE_REMOVAL || Minor==IRP_MN_REMOVE_DEVICE)InterlockedExchange(&H->Removing,1);
            S=H->Role?BhSocPnp(H,Irp):!H->BoardInitialized?BhDeferredPnp(H,Irp):
                MtkMaliBoardProcessPnp(&H->Board,H->Pdo,H->Lower,Irp);
        }
        if(Minor==IRP_MN_REMOVE_DEVICE) {
            IoReleaseRemoveLockAndWait(&H->RemoveLock,Irp);
            IoDetachDevice(H->Lower);H->Lower=0;
            InterlockedExchange(&H->RemoveDone,1);BhRequestReap(H);
            /* The reaper can delete Device now; no host field is read below. */
        } else IoReleaseRemoveLock(&H->RemoveLock,Irp);
        IoCompleteRequest(Irp,IO_NO_INCREMENT);return S;
    }
    if(InterlockedCompareExchange(&H->Removing,0,0)) {
        S=STATUS_DEVICE_REMOVED;IoReleaseRemoveLock(&H->RemoveLock,Irp);
        Irp->IoStatus.Status=S;IoCompleteRequest(Irp,IO_NO_INCREMENT);return S;
    }
    if(H->Role && Stack->MajorFunction==IRP_MJ_POWER && Stack->MinorFunction==IRP_MN_SET_POWER &&
       Stack->Parameters.Power.Type==DevicePowerState) {
        S=BhSocPower(H,Irp);Irp->IoStatus.Status=S;
        IoReleaseRemoveLock(&H->RemoveLock,Irp);IoCompleteRequest(Irp,IO_NO_INCREMENT);return S;
    }
Forward:
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,BhPassComplete,H,TRUE,TRUE,TRUE);
    if(Stack->MajorFunction==IRP_MJ_POWER)return PoCallDriver(H->Lower,Irp);
    return IoCallDriver(H->Lower,Irp);
}
NTSTATUS MtkMaliBoardHostAddDevice(PDRIVER_OBJECT Object,PDEVICE_OBJECT Pdo) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !Object || !Pdo)return STATUS_INVALID_PARAMETER;
    BOOLEAN Root=FALSE;
    NTSTATUS S=BhHardwareId(Pdo,TRUE);
    if(S==STATUS_SUCCESS)Root=TRUE;
    else if(S==STATUS_NO_SUCH_DEVICE)S=BhHardwareId(Pdo,FALSE);
    if(S!=STATUS_SUCCESS)return S;
    PDEVICE_OBJECT Device=0;
    S=IoCreateDevice(Object,sizeof(MALI_BOARD_HOST),0,Pdo->DeviceType,FILE_DEVICE_SECURE_OPEN,FALSE,&Device);
    if(S!=STATUS_SUCCESS)return BhExact(S);
    MALI_BOARD_HOST *H=Device->DeviceExtension;RtlZeroMemory(H,sizeof(*H));
    H->Signature=BH_SIGNATURE;H->Role=Root;H->Device=Device;H->Pdo=Pdo;ObReferenceObject(Pdo);
    IoInitializeRemoveLock(&H->RemoveLock,BH_TAG,0,0);
    H->Reap=IoAllocateWorkItem(Device);
    if(!H->Reap){S=STATUS_INSUFFICIENT_RESOURCES;goto Fail;}
    if(Root) {
        MALI_BOARD_LIFETIME Life={H,BhReference,BhDereference};
        S=MtkMaliSocInitialize(&H->Soc,&Life);
        if(S==STATUS_SUCCESS) {
            H->SocInitialized=1;
            S=IoRegisterDeviceInterface(Pdo,&GUID_DEVINTERFACE_MTK_CONN_POWER,0,&H->ConnLink);
            if(NT_SUCCESS(S))S=STATUS_SUCCESS;
        }
    } else {
        S=BhQueryParent(H);
        if(S==STATUS_NOT_SUPPORTED || S==STATUS_DEVICE_NOT_READY) {
            MALI_BOARD_PARENT_INTERFACE Empty={0};
            if(!H->ParentHeld && !H->Opened && !H->ParentCookie && RtlEqualMemory(&H->Parent,&Empty,sizeof(Empty)))S=STATUS_SUCCESS;
        }
    }
    if(S!=STATUS_SUCCESS)goto Fail;
    S=BhExact(IoAttachDeviceToDeviceStackSafe(Device,Pdo,&H->Lower));if(S!=STATUS_SUCCESS)goto Fail;
    Device->Flags|=H->Lower->Flags&(DO_BUFFERED_IO|DO_DIRECT_IO|DO_POWER_PAGABLE|DO_POWER_INRUSH);
    if(Device->AlignmentRequirement<H->Lower->AlignmentRequirement)Device->AlignmentRequirement=H->Lower->AlignmentRequirement;
    if(Root){Device->Flags&=~DO_POWER_INRUSH;Device->Flags|=DO_POWER_PAGABLE;}
    Device->Flags&=~DO_DEVICE_INITIALIZING;return STATUS_SUCCESS;
Fail:
    H->CleanupStatus=BhCleanup(H);
    if(H->CleanupStatus!=STATUS_SUCCESS){H->Quarantined=1;InterlockedExchange(&H->Removing,1);return S;}
    if(H->Reap)IoFreeWorkItem(H->Reap);
    if(H->ConnLink.Buffer)RtlFreeUnicodeString(&H->ConnLink);
    H->Signature=0;ObDereferenceObject(Pdo);IoDeleteDevice(Device);return S;
}
static void BhUnload(PDRIVER_OBJECT Object) {(void)Object; /* Device removal owns all storage. */}
NTSTATUS MtkMaliBoardHostDriverEntry(PDRIVER_OBJECT Object,PUNICODE_STRING Path) {
    (void)Path;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !Object || !Object->DriverExtension || Object->DeviceObject)
        return STATUS_INVALID_PARAMETER;
    KeInitializeMutex(&BhSocRegistry,0);BhSocOwner=0;
    for(ULONG I=0;I<=IRP_MJ_MAXIMUM_FUNCTION;I++)Object->MajorFunction[I]=MtkMaliBoardHostDispatch;
    Object->DriverExtension->AddDevice=MtkMaliBoardHostAddDevice;Object->DriverUnload=BhUnload;
    return STATUS_SUCCESS;
}
