/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Board.h"
#include "InterruptResources.h"
#define BP_SIGNATURE 0x42503730u
static int BpRange(const void *P,size_t N) {return P && N && (uintptr_t)P<=UINTPTR_MAX-N;}
static int BpOverlap(const void *A,size_t N,const void *B,size_t M) {
    return (uintptr_t)A<(uintptr_t)B+M && (uintptr_t)B<(uintptr_t)A+N;
}
static int BpValid(MALI_BOARD_PROVIDER *D) {return D && D->Signature==BP_SIGNATURE;}
static NTSTATUS BpExact(NTSTATUS S) {return S!=STATUS_SUCCESS && NT_SUCCESS(S)?STATUS_UNSUCCESSFUL:S;}
static NTSTATUS BpStatus(MALI_RESULT R) {
    switch(R) {
    case MaliOk:return STATUS_SUCCESS;case MaliBusy:return STATUS_DEVICE_BUSY;
    case MaliTimeout:return STATUS_IO_TIMEOUT;case MaliUnsupported:return STATUS_NOT_SUPPORTED;
    case MaliInvalid:return STATUS_INVALID_PARAMETER;case MaliFault:return STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE;
    default:return STATUS_DEVICE_NOT_READY;
    }
}
static int BpPoisoned(const MALI_MFG_CONTROLLER *C) {
    return C->Poisoned || C->Power.Poisoned || C->Power.Domains.Poisoned || C->Power.Clocks.Poisoned ||
        C->Io.Rails->Poisoned || C->Io.Rails->Io.Pmic->Poisoned || C->Io.Clock->Poisoned;
}
static int BpClean(const MALI_MFG_CONTROLLER *C) {
    return C->Initialized && C->Power.Initialized && C->Power.Domains.Initialized && C->Power.Clocks.Initialized &&
        C->Io.Rails->Initialized && C->Io.Rails->Io.Pmic->Initialized && C->Io.Clock->Initialized &&
        (C->State==MaliMfgControllerUnknown || C->State==MaliMfgControllerOff) &&
        !BpPoisoned(C) && !C->Power.References && !C->Power.Stages && !C->Power.Clocks.Stages &&
        !C->Prepared && !C->Enabled && !C->ReferencePrepared && !C->ReferenceEnabled &&
        !C->Io.Rails->Acquired && !C->Io.Rails->Owned && !C->Io.Clock->Held;
}
static int BpOutput(MALI_BOARD_PROVIDER *D,const void *Out,size_t N) {
    MALI_MFG_CONTROLLER *C=D->Config.Controller;
    return BpRange(Out,N) && !((uintptr_t)Out&7u) && !BpOverlap(D,sizeof(*D),Out,N) &&
        !BpOverlap(C,sizeof(*C),Out,N) && !BpOverlap(C->Io.Rails,sizeof(*C->Io.Rails),Out,N) &&
        !BpOverlap(C->Io.Clock,sizeof(*C->Io.Clock),Out,N) &&
        !BpOverlap(C->Io.Rails->Io.Pmic,sizeof(*C->Io.Rails->Io.Pmic),Out,N);
}
static NTSTATUS BpEnter(MALI_BOARD_PROVIDER *D) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !BpValid(D))return STATUS_INVALID_PARAMETER;
    if(InterlockedCompareExchange(&D->Gate,1,0))return STATUS_DEVICE_BUSY;
    if(InterlockedCompareExchange(&D->ReferenceFault,0,0)) {
        InterlockedExchange(&D->Gate,0);return STATUS_DRIVER_INTERNAL_ERROR;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS BpExit(MALI_BOARD_PROVIDER *D,NTSTATUS S) {InterlockedExchange(&D->Gate,0);return BpExact(S);}
static MALI_BOARD_PROVIDER *BpOwner(void *X,uint32_t Power) {
    MALI_BOARD_REFERENCE *R=X;
    if(!R || Power>1 || R->Power!=Power || !BpValid(R->Owner) || R!=&R->Owner->Reference[Power])return 0;
    return R->Owner;
}
static void BpReference(void *X) {
    MALI_BOARD_REFERENCE *R=X;MALI_BOARD_PROVIDER *D=R?BpOwner(X,R->Power):0;
    if(!D)return;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL){InterlockedExchange(&D->ReferenceFault,1);return;}
    for(;;) {
        LONG N=InterlockedCompareExchange(&D->References[R->Power],0,0);
        if(N<0 || N==MAXLONG){InterlockedExchange(&D->ReferenceFault,1);return;}
        if(InterlockedCompareExchange(&D->References[R->Power],N+1,N)==N) {
            if(D->Config.Lifetime.Reference)D->Config.Lifetime.Reference(D->Config.Lifetime.Context);
            return;
        }
    }
}
static void BpDereference(void *X) {
    MALI_BOARD_REFERENCE *R=X;MALI_BOARD_PROVIDER *D=R?BpOwner(X,R->Power):0;
    if(!D)return;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL){InterlockedExchange(&D->ReferenceFault,1);return;}
    for(;;) {
        LONG N=InterlockedCompareExchange(&D->References[R->Power],0,0);
        if(N<=0){InterlockedExchange(&D->ReferenceFault,1);return;}
        if(InterlockedCompareExchange(&D->References[R->Power],N-1,N)==N) {
            if(D->Config.Lifetime.Dereference)D->Config.Lifetime.Dereference(D->Config.Lifetime.Context);
            return;
        }
    }
}
static int BpLeaseValid(const MALI_BOARD_LEASE *L) {
    return L->Cookie && L->AddressModel==MaliVmDirectSystemPhysical &&
        L->Cache.CleanForGpu && L->Cache.StoreBarrier && L->TimeoutTicks &&
        L->ApertureCommitLimit>=4096 && !(L->ApertureCommitLimit&4095u) &&
        L->ApertureCommitLimit<=MALI_ALLOCATION_MAX_BYTES;
}
static int BpClient(MALI_BOARD_PROVIDER *D,void *Cookie) {
    return Cookie && Cookie==D->ClientCookie && D->ResourceLease.Cookie &&
        InterlockedCompareExchange(&D->References[0],0,0)>0;
}
static NTSTATUS BpBeginAccess(MALI_BOARD_PROVIDER *D) {
    MALI_BOARD_RESOURCES *R=&D->Config.Resources;
    if(D->AccessHeld || !D->ResourceLease.Cookie)return STATUS_DRIVER_INTERNAL_ERROR;
    NTSTATUS S=BpExact(R->Enter(R->Context,D->ResourceLease.Cookie));if(S!=STATUS_SUCCESS)return S;
    D->AccessHeld=1;
    return BpExact(R->Validate(R->Context,&D->ResourceLease));
}
static void BpEndAccess(MALI_BOARD_PROVIDER *D) {
    if(D->AccessHeld) {
        MALI_BOARD_RESOURCES *R=&D->Config.Resources;
        R->Leave(R->Context,D->ResourceLease.Cookie,(BOOLEAN)BpPoisoned(D->Config.Controller));
        D->AccessHeld=0;
    }
}
static NTSTATUS BpPower(MALI_BOARD_PROVIDER *D,DEVICE_POWER_STATE State) {
    D->PowerState=PowerDeviceUnspecified;
    NTSTATUS S=BpBeginAccess(D);
    if(S==STATUS_SUCCESS)S=BpStatus(MaliMfgControllerSet(D->Config.Controller,State==PowerDeviceD0));
    if(S==STATUS_SUCCESS && State==PowerDeviceD0)
        S=BpExact(D->Config.Resources.Validate(D->Config.Resources.Context,&D->ResourceLease));
    BpEndAccess(D);
    if(S==STATUS_SUCCESS)D->PowerState=State;
    return S;
}
static NTSTATUS BpAcquire(void *X,MALI_BOARD_LEASE *L) {
    MALI_BOARD_PROVIDER *D=BpOwner(X,0);NTSTATUS S=BpEnter(D);if(S!=STATUS_SUCCESS)return S;
    if(!BpOutput(D,L,sizeof(*L)))
        return BpExit(D,STATUS_INVALID_PARAMETER);
    if(D->Removed)return BpExit(D,STATUS_DEVICE_REMOVED);
    if(D->Closing || D->Stopping || D->PnpActive || D->ClientCookie || D->ResourceLease.Cookie || D->AccessHeld ||
       InterlockedCompareExchange(&D->References[0],0,0)<=0)return BpExit(D,STATUS_DEVICE_BUSY);
    if(D->Generation==UINTPTR_MAX)return BpExit(D,STATUS_INSUFFICIENT_RESOURCES);
    RtlZeroMemory(L,sizeof(*L));
    MALI_BOARD_LEASE Resource={0};
    S=BpExact(D->Config.Resources.Acquire(D->Config.Resources.Context,&Resource));
    if(Resource.Cookie) {
        D->ResourceLease=Resource;D->ClientCookie=(void*)++D->Generation;
        L->Cookie=D->ClientCookie;D->PowerState=PowerDeviceUnspecified;
    }
    if(S!=STATUS_SUCCESS)return BpExit(D,S);
    if(!Resource.Cookie)return BpExit(D,STATUS_DRIVER_INTERNAL_ERROR);
    if(!BpLeaseValid(&Resource))return BpExit(D,STATUS_DEVICE_CONFIGURATION_ERROR);
    S=BpPower(D,PowerDeviceD0);
    if(S==STATUS_SUCCESS){*L=Resource;L->Cookie=D->ClientCookie;}
    return BpExit(D,S);
}
static NTSTATUS BpReady(void *X,void *Cookie) {
    MALI_BOARD_PROVIDER *D=BpOwner(X,0);NTSTATUS S=BpEnter(D);if(S!=STATUS_SUCCESS)return S;
    if(D->Removed)return BpExit(D,STATUS_DEVICE_REMOVED);
    if(!BpClient(D,Cookie))return BpExit(D,STATUS_INVALID_PARAMETER);
    if(D->PowerState!=PowerDeviceD0)return BpExit(D,STATUS_DEVICE_NOT_READY);
    S=BpBeginAccess(D);
    if(S==STATUS_SUCCESS)S=BpStatus(MaliMfgControllerReady(D->Config.Controller));
    BpEndAccess(D);if(S!=STATUS_SUCCESS)D->PowerState=PowerDeviceUnspecified;
    return BpExit(D,S);
}
static NTSTATUS BpSetPower(void *X,void *Cookie,DEVICE_POWER_STATE State,POWER_ACTION Action) {
    (void)Action; /* Only the requested device state controls this provider. */
    MALI_BOARD_PROVIDER *D=BpOwner(X,1);NTSTATUS S=BpEnter(D);if(S!=STATUS_SUCCESS)return S;
    if(D->Removed)return BpExit(D,STATUS_DEVICE_REMOVED);
    if(!BpClient(D,Cookie) || InterlockedCompareExchange(&D->References[1],0,0)<=0)
        return BpExit(D,STATUS_INVALID_PARAMETER);
    if(State!=PowerDeviceD0 && State!=PowerDeviceD3)return BpExit(D,STATUS_NOT_SUPPORTED);
    return BpExit(D,BpPower(D,State));
}
static NTSTATUS BpRelease(void *X,void *Cookie) {
    MALI_BOARD_PROVIDER *D=BpOwner(X,0);NTSTATUS S=BpEnter(D);if(S!=STATUS_SUCCESS)return S;
    if(D->Removed)return BpExit(D,STATUS_DEVICE_REMOVED);
    if(!BpClient(D,Cookie))return BpExit(D,STATUS_INVALID_PARAMETER);
    if(!BpClean(D->Config.Controller)) {
        S=BpBeginAccess(D);
        if(S==STATUS_SUCCESS)S=BpStatus(MaliMfgControllerSet(D->Config.Controller,0));
        if(S==STATUS_SUCCESS && !BpClean(D->Config.Controller))S=STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE;
        BpEndAccess(D);if(S!=STATUS_SUCCESS){D->PowerState=PowerDeviceUnspecified;return BpExit(D,S);}
    }
    D->PowerState=PowerDeviceD3;
    S=BpExact(D->Config.Resources.Release(D->Config.Resources.Context,D->ResourceLease.Cookie));
    if(S==STATUS_SUCCESS){RtlZeroMemory(&D->ResourceLease,sizeof(D->ResourceLease));D->ClientCookie=0;D->PowerState=PowerDeviceUnspecified;}
    return BpExit(D,S);
}
NTSTATUS MtkMaliBoardProviderInitialize(MALI_BOARD_PROVIDER *D,const MALI_BOARD_CONFIG *C) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !BpRange(D,sizeof(*D)) || !BpRange(C,sizeof(*C)) ||
       ((uintptr_t)D&7u) || ((uintptr_t)C&7u) || BpOverlap(D,sizeof(*D),C,sizeof(*C)))return STATUS_INVALID_PARAMETER;
    MALI_MFG_CONTROLLER *P=C->Controller;
    if(!BpRange(P,sizeof(*P)) || ((uintptr_t)P&7u) || BpOverlap(D,sizeof(*D),P,sizeof(*P)) ||
       BpOverlap(C,sizeof(*C),P,sizeof(*P)) || !P->Initialized ||
       !C->Resources.Acquire || !C->Resources.Validate || !C->Resources.Enter || !C->Resources.Leave || !C->Resources.Release ||
       (!!C->Lifetime.Reference != !!C->Lifetime.Dereference))
        return STATUS_INVALID_PARAMETER;
    if(!BpRange(P->Io.Rails,sizeof(*P->Io.Rails)) || ((uintptr_t)P->Io.Rails&7u) ||
       !BpRange(P->Io.Clock,sizeof(*P->Io.Clock)) || ((uintptr_t)P->Io.Clock&7u))return STATUS_INVALID_PARAMETER;
    const void *Objects[6]={D,C,P,P->Io.Rails,P->Io.Clock,P->Io.Rails->Io.Pmic};
    size_t Sizes[6]={sizeof(*D),sizeof(*C),sizeof(*P),sizeof(*P->Io.Rails),sizeof(*P->Io.Clock),sizeof(*P->Io.Rails->Io.Pmic)};
    for(UINT I=0;I<6;I++) {
        if(!BpRange(Objects[I],Sizes[I]) || ((uintptr_t)Objects[I]&7u))return STATUS_INVALID_PARAMETER;
        for(UINT J=0;J<I;J++)if(BpOverlap(Objects[I],Sizes[I],Objects[J],Sizes[J]))return STATUS_INVALID_PARAMETER;
    }
    if(D->Signature || D->Closing || D->Removed || D->Stopping || D->PnpActive || D->AccessHeld || D->Gate || D->References[0] || D->References[1] ||
       D->ReferenceFault || D->ClientCookie || D->ResourceLease.Cookie || D->Generation || D->Pdo || !BpClean(P))return STATUS_DEVICE_BUSY;
    D->Config=*C;D->Reference[0].Owner=D;D->Reference[0].Power=0;
    D->Reference[1].Owner=D;D->Reference[1].Power=1;D->PowerState=PowerDeviceUnspecified;
    D->Signature=BP_SIGNATURE;return STATUS_SUCCESS;
}
static NTSTATUS BpGetLocked(MALI_BOARD_PROVIDER *D,USHORT Version,USHORT Bytes,void *Out,uint32_t Power) {
    size_t Need=Power?sizeof(MALI_BOARD_POWER_INTERFACE):sizeof(MALI_BOARD_INTERFACE);
    if(!Version)return STATUS_NOT_SUPPORTED;
    if(Bytes<Need)return STATUS_BUFFER_TOO_SMALL;
    if(!BpOutput(D,Out,Need))return STATUS_INVALID_PARAMETER;
    if(D->Closing)return STATUS_DELETE_PENDING;
    if(D->Stopping || D->PnpActive)return STATUS_DEVICE_NOT_READY;
    if(InterlockedCompareExchange(&D->References[Power],0,0)==MAXLONG)return STATUS_INSUFFICIENT_RESOURCES;
    INTERFACE Header={0};Header.Size=(USHORT)Need;Header.Version=1;Header.Context=&D->Reference[Power];
    Header.InterfaceReference=BpReference;Header.InterfaceDereference=BpDereference;
    BpReference(Header.Context);
    if(D->ReferenceFault)return STATUS_DRIVER_INTERNAL_ERROR;
    if(Power) {
        MALI_BOARD_POWER_INTERFACE I={0};I.Header=Header;I.SetPower=BpSetPower;*(MALI_BOARD_POWER_INTERFACE*)Out=I;
    } else {
        MALI_BOARD_INTERFACE I={0};I.Header=Header;I.Acquire=BpAcquire;I.Ready=BpReady;I.Release=BpRelease;*(MALI_BOARD_INTERFACE*)Out=I;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS BpGet(MALI_BOARD_PROVIDER *D,USHORT Version,USHORT Bytes,void *Out,uint32_t Power) {
    NTSTATUS S=BpEnter(D);if(S!=STATUS_SUCCESS)return S;
    return BpExit(D,BpGetLocked(D,Version,Bytes,Out,Power));
}
NTSTATUS MtkMaliBoardGetInterface(MALI_BOARD_PROVIDER *D,USHORT Version,USHORT Bytes,MALI_BOARD_INTERFACE *Out) {
    return BpGet(D,Version,Bytes,Out,0);
}
NTSTATUS MtkMaliBoardGetPowerInterface(MALI_BOARD_PROVIDER *D,USHORT Version,USHORT Bytes,MALI_BOARD_POWER_INTERFACE *Out) {
    return BpGet(D,Version,Bytes,Out,1);
}
NTSTATUS MtkMaliBoardProviderClose(MALI_BOARD_PROVIDER *D) {
    NTSTATUS S=BpEnter(D);if(S!=STATUS_SUCCESS)return S;D->Closing=1;
    if(D->References[0] || D->References[1] || D->ClientCookie || D->ResourceLease.Cookie || D->AccessHeld || D->Pdo || D->PnpActive ||
       !BpClean(D->Config.Controller))return BpExit(D,STATUS_DEVICE_BUSY);
    return BpExit(D,STATUS_SUCCESS);
}

static int BpSameIrq(const CM_PARTIAL_RESOURCE_DESCRIPTOR *A,const CM_PARTIAL_RESOURCE_DESCRIPTOR *B) {
    return A->Type==B->Type && A->ShareDisposition==B->ShareDisposition && A->Flags==B->Flags &&
        A->u.Interrupt.Level==B->u.Interrupt.Level && A->u.Interrupt.Vector==B->u.Interrupt.Vector &&
#if defined(NT_PROCESSOR_GROUPS)
        A->u.Interrupt.Group==B->u.Interrupt.Group &&
#endif
        A->u.Interrupt.Affinity==B->u.Interrupt.Affinity;
}
static int BpList(const CM_RESOURCE_LIST *L) {
    size_t N=FIELD_OFFSET(CM_RESOURCE_LIST,List)+FIELD_OFFSET(CM_FULL_RESOURCE_DESCRIPTOR,PartialResourceList)+
        FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST,PartialDescriptors)+4u*sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
    return BpRange(L,N) && !((uintptr_t)L&3u) && L->Count==1 &&
        L->List[0].PartialResourceList.Version==1 && L->List[0].PartialResourceList.Revision==1 &&
        L->List[0].PartialResourceList.Count==4;
}
static NTSTATUS BpBindLocked(MALI_BOARD_PROVIDER *D,PDEVICE_OBJECT Pdo,
    const CM_RESOURCE_LIST *Raw,const CM_RESOURCE_LIST *Translated) {
    NTSTATUS S;
    if(!Pdo || !BpList(Raw) || !BpList(Translated))return STATUS_INVALID_PARAMETER;
    if(D->Closing)return STATUS_DELETE_PENDING;
    if(D->Pdo || D->References[0] || D->References[1] || D->ClientCookie || D->ResourceLease.Cookie ||
       D->AccessHeld || !BpClean(D->Config.Controller))return STATUS_DEVICE_BUSY;
    CM_PARTIAL_RESOURCE_DESCRIPTOR Irqs[3];UINT Memory=0,Count=0;
    for(UINT I=0;I<4;I++) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *A=&Raw->List[0].PartialResourceList.PartialDescriptors[I];
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *B=&Translated->List[0].PartialResourceList.PartialDescriptors[I];
        if(A->Type!=B->Type || A->ShareDisposition!=CmResourceShareDeviceExclusive ||
           B->ShareDisposition!=CmResourceShareDeviceExclusive)return STATUS_DEVICE_CONFIGURATION_ERROR;
        if(A->Type==CmResourceTypeMemory) {
            if(Memory++ || A->Flags!=CM_RESOURCE_MEMORY_READ_WRITE || B->Flags!=CM_RESOURCE_MEMORY_READ_WRITE ||
               A->u.Memory.Start.QuadPart!=0x13000000u || B->u.Memory.Start.QuadPart!=0x13000000u ||
               A->u.Memory.Length!=0x4000u || B->u.Memory.Length!=0x4000u)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
        } else if(A->Type==CmResourceTypeInterrupt) {
            if(Count==3 || (A->Flags&~(CM_RESOURCE_INTERRUPT_POLICY_INCLUDED|CM_RESOURCE_INTERRUPT_WAKE_HINT)) ||
               A->u.Interrupt.Vector!=428u+Count)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
#if defined(NT_PROCESSOR_GROUPS)
            if(A->u.Interrupt.Group)return STATUS_DEVICE_CONFIGURATION_ERROR;
#endif
            Irqs[Count++]=*B;
        } else return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if(Memory!=1 || Count!=3)return STATUS_DEVICE_CONFIGURATION_ERROR;
    ULONG Levels;KIRQL Maximum;S=MaliValidateInterruptResources(Irqs,3,&Levels,&Maximum);
    if(S==STATUS_SUCCESS){RtlCopyMemory(D->Interrupts,Irqs,sizeof(Irqs));D->Pdo=Pdo;D->Stopping=0;}
    return S;
}
NTSTATUS MtkMaliBoardBindResources(MALI_BOARD_PROVIDER *D,PDEVICE_OBJECT Pdo,
    const CM_RESOURCE_LIST *Raw,const CM_RESOURCE_LIST *Translated) {
    NTSTATUS S=BpEnter(D);if(S!=STATUS_SUCCESS)return S;
    if(D->PnpActive)return BpExit(D,STATUS_DEVICE_BUSY);
    return BpExit(D,BpBindLocked(D,Pdo,Raw,Translated));
}
NTSTATUS MtkMaliBoardUnbindResources(MALI_BOARD_PROVIDER *D,PDEVICE_OBJECT Pdo) {
    NTSTATUS S=BpEnter(D);if(S!=STATUS_SUCCESS)return S;
    if(!Pdo || D->Pdo!=Pdo)return BpExit(D,STATUS_NO_SUCH_DEVICE);
    if(D->PnpActive)return BpExit(D,STATUS_DEVICE_BUSY);
    if(D->References[0] || D->References[1] || D->ClientCookie || D->ResourceLease.Cookie || D->AccessHeld ||
       !BpClean(D->Config.Controller))return BpExit(D,STATUS_DEVICE_BUSY);
    D->Pdo=0;RtlZeroMemory(D->Interrupts,sizeof(D->Interrupts));return BpExit(D,STATUS_SUCCESS);
}
BOOLEAN MtkMaliBoardQueryInterfaceIrp(MALI_BOARD_PROVIDER *D,PDEVICE_OBJECT Pdo,PIRP Irp) {
    if(!Irp)return FALSE;
    PIO_STACK_LOCATION Stack=IoGetCurrentIrpStackLocation(Irp);
    if(!Stack || Stack->MajorFunction!=IRP_MJ_PNP || Stack->MinorFunction!=IRP_MN_QUERY_INTERFACE ||
       !Stack->Parameters.QueryInterface.InterfaceType)return FALSE;
    if(Irp->IoStatus.Status!=STATUS_NOT_SUPPORTED)return FALSE;
    const GUID *G=Stack->Parameters.QueryInterface.InterfaceType;uint32_t Power;
    if(RtlEqualMemory(G,&MtkMaliBoardInterfaceGuid,sizeof(*G)))Power=0;
    else if(RtlEqualMemory(G,&MtkMaliBoardPowerInterfaceGuid,sizeof(*G)))Power=1;
    else return FALSE;
    NTSTATUS S=BpEnter(D);
    if(S==STATUS_SUCCESS) {
        const MALI_BOARD_QUERY *Q=Stack->Parameters.QueryInterface.InterfaceSpecificData;
        if(D->Closing)S=STATUS_DELETE_PENDING;
        else if(D->Stopping || D->PnpActive)S=STATUS_DEVICE_NOT_READY;
        else if(!Pdo || D->Pdo!=Pdo)S=STATUS_NO_SUCH_DEVICE;
        else if(Irp->RequestorMode!=KernelMode || !BpRange(Q,sizeof(*Q)) || ((uintptr_t)Q&7u) ||
            Q->Size!=sizeof(*Q) || Q->Version!=1 || Q->Pdo!=Pdo)S=STATUS_INVALID_PARAMETER;
        else {
            for(UINT I=0;I<3;I++)if(!BpSameIrq(&Q->Interrupts[I],&D->Interrupts[I])) {
                S=STATUS_DEVICE_CONFIGURATION_ERROR;break;
            }
            if(S==STATUS_SUCCESS)S=BpGetLocked(D,Stack->Parameters.QueryInterface.Version,
                Stack->Parameters.QueryInterface.Size,Stack->Parameters.QueryInterface.Interface,Power);
        }
        S=BpExit(D,S);
    }
    Irp->IoStatus.Status=S;Irp->IoStatus.Information=0;return TRUE;
}

/* No pool allocation or device-control request: use the existing PnP IRP's
   next stack location and keep its resources alive until lower completion. */
static NTSTATUS BpPnpComplete(PDEVICE_OBJECT Device,PIRP Irp,void *Context) {
    (void)Device;(void)Irp;KeSetEvent((PKEVENT)Context,IO_NO_INCREMENT,FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}
NTSTATUS MtkMaliBoardForwardPnp(PDEVICE_OBJECT Lower,PIRP Irp) {
    KEVENT Event;KeInitializeEvent(&Event,NotificationEvent,FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp,BpPnpComplete,&Event,TRUE,TRUE,TRUE);
    (void)IoCallDriver(Lower,Irp);
    /* The completion is the lifetime boundary, including lower cancellation
       and failure. Never leave a stack event/resources behind on a timeout. */
    KeWaitForSingleObject(&Event,Executive,KernelMode,FALSE,0);
    return Irp->IoStatus.Status;
}
static int BpQuiescent(MALI_BOARD_PROVIDER *D) {
    return !InterlockedCompareExchange(&D->References[0],0,0) &&
        !InterlockedCompareExchange(&D->References[1],0,0) &&
        !D->ClientCookie && !D->ResourceLease.Cookie && !D->AccessHeld &&
        !D->ReferenceFault && BpClean(D->Config.Controller);
}
static void BpPnpWait(volatile LONG *Gate) {
    LARGE_INTEGER Delay;Delay.QuadPart=-10000; /* nonalertable 1 ms rundown */
    while(InterlockedCompareExchange(Gate,1,0))
        KeDelayExecutionThread(KernelMode,FALSE,&Delay);
}
NTSTATUS MtkMaliBoardProcessPnp(MALI_BOARD_PROVIDER *D,PDEVICE_OBJECT Pdo,
    PDEVICE_OBJECT Lower,PIRP Irp) {
    if(!Irp)return STATUS_INVALID_PARAMETER;
    PIO_STACK_LOCATION Stack=IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS S=STATUS_INVALID_PARAMETER;UCHAR Minor=Stack?Stack->MinorFunction:0;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !BpValid(D) || !Pdo || !Lower || !Stack ||
       Stack->MajorFunction!=IRP_MJ_PNP || Irp->RequestorMode!=KernelMode ||
       Irp->CurrentLocation<=1 || Irp->CurrentLocation>Irp->StackCount)goto Complete;
    if(Minor==IRP_MN_QUERY_INTERFACE) {
        if(!MtkMaliBoardQueryInterfaceIrp(D,Pdo,Irp))S=MtkMaliBoardForwardPnp(Lower,Irp);
        else S=Irp->IoStatus.Status;
        goto Complete;
    }
    int Forced=Minor==IRP_MN_SURPRISE_REMOVAL || Minor==IRP_MN_REMOVE_DEVICE;
    int Managed=Forced || Minor==IRP_MN_START_DEVICE || Minor==IRP_MN_STOP_DEVICE ||
        Minor==IRP_MN_QUERY_STOP_DEVICE || Minor==IRP_MN_QUERY_REMOVE_DEVICE ||
        Minor==IRP_MN_CANCEL_STOP_DEVICE || Minor==IRP_MN_CANCEL_REMOVE_DEVICE;
    if(!Managed){S=MtkMaliBoardForwardPnp(Lower,Irp);goto Complete;}
    if(Forced)BpPnpWait(&D->PnpActive);
    else if(InterlockedCompareExchange(&D->PnpActive,1,0)){S=STATUS_DEVICE_BUSY;goto Complete;}
    if(D->Pdo && D->Pdo!=Pdo) {
        InterlockedExchange(&D->PnpActive,0);S=STATUS_NO_SUCH_DEVICE;goto Complete;
    }
    if(Forced) {
        /* Withdraw before waiting for an in-flight hardware call; do not call
           the lower bus until that call has left all shared hardware locks. */
        InterlockedExchange(&D->Closing,1);InterlockedExchange(&D->Removed,1);
        BpPnpWait(&D->Gate);D->Stopping=Minor;
        if(Minor==IRP_MN_REMOVE_DEVICE && BpQuiescent(D)) {
            D->Pdo=0;RtlZeroMemory(D->Interrupts,sizeof(D->Interrupts));
        }
        BpExit(D,STATUS_SUCCESS);S=STATUS_SUCCESS;
    } else {
        if(Minor==IRP_MN_STOP_DEVICE || Minor==IRP_MN_CANCEL_STOP_DEVICE || Minor==IRP_MN_CANCEL_REMOVE_DEVICE) {
            BpPnpWait(&D->Gate);
            S=D->ReferenceFault?BpExit(D,STATUS_DRIVER_INTERNAL_ERROR):STATUS_SUCCESS;
        } else S=BpEnter(D);
        if(S==STATUS_SUCCESS) {
            if(D->Closing)S=STATUS_DELETE_PENDING;
            else if(D->Pdo && D->Pdo!=Pdo)S=STATUS_NO_SUCH_DEVICE;
            else switch(Minor) {
            case IRP_MN_START_DEVICE:
                if(D->Pdo || !BpQuiescent(D))S=STATUS_DEVICE_BUSY;
                else D->Stopping=IRP_MN_STOP_DEVICE;
                break;
            case IRP_MN_QUERY_STOP_DEVICE:case IRP_MN_QUERY_REMOVE_DEVICE:
            case IRP_MN_STOP_DEVICE:
                if(!D->Pdo && !(Minor==IRP_MN_STOP_DEVICE && D->Stopping==IRP_MN_STOP_DEVICE))S=STATUS_DEVICE_NOT_READY;
                else if((D->Stopping && D->Stopping!=Minor &&
                         !(Minor==IRP_MN_STOP_DEVICE && D->Stopping==IRP_MN_QUERY_STOP_DEVICE)) ||
                        !BpQuiescent(D))S=STATUS_DEVICE_BUSY;
                else D->Stopping=Minor;
                break;
            default:break; /* A cancel with no matching query is harmless. */
            }
            S=BpExit(D,S);
        }
    }
    if(S==STATUS_SUCCESS) {
        Irp->IoStatus.Status=STATUS_SUCCESS;Irp->IoStatus.Information=0;
        S=BpExact(MtkMaliBoardForwardPnp(Lower,Irp));
        if(S==STATUS_SUCCESS && Minor==IRP_MN_START_DEVICE) {
            BpPnpWait(&D->Gate);
            S=D->ReferenceFault?STATUS_DRIVER_INTERNAL_ERROR:
                BpBindLocked(D,Pdo,Stack->Parameters.StartDevice.AllocatedResources,
                    Stack->Parameters.StartDevice.AllocatedResourcesTranslated);
            S=BpExit(D,S);
        }
        else if(S==STATUS_SUCCESS && (Minor==IRP_MN_STOP_DEVICE ||
            Minor==IRP_MN_CANCEL_STOP_DEVICE || Minor==IRP_MN_CANCEL_REMOVE_DEVICE)) {
            BpPnpWait(&D->Gate);
            {
                if(Minor==IRP_MN_STOP_DEVICE) {
                    if(!BpQuiescent(D) || (D->Pdo && D->Pdo!=Pdo))S=STATUS_DEVICE_BUSY;
                    else {D->Pdo=0;RtlZeroMemory(D->Interrupts,sizeof(D->Interrupts));}
                } else if(!D->Closing && !D->ReferenceFault &&
                         ((Minor==IRP_MN_CANCEL_STOP_DEVICE && D->Stopping==IRP_MN_QUERY_STOP_DEVICE) ||
                          (Minor==IRP_MN_CANCEL_REMOVE_DEVICE && D->Stopping==IRP_MN_QUERY_REMOVE_DEVICE)))D->Stopping=0;
                S=BpExit(D,S);
            }
        }
    }
    InterlockedExchange(&D->PnpActive,0);
Complete:
    Irp->IoStatus.Status=S;return S;
}
NTSTATUS MtkMaliBoardDispatchPnp(MALI_BOARD_PROVIDER *D,PDEVICE_OBJECT Pdo,
    PDEVICE_OBJECT Lower,PIRP Irp) {
    NTSTATUS S=MtkMaliBoardProcessPnp(D,Pdo,Lower,Irp);
    if(Irp)IoCompleteRequest(Irp,IO_NO_INCREMENT);
    return S;
}
