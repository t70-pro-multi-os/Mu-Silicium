/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Ddi.h"
#include "InterruptResources.h"
#include "Commands.h"
#define SIGNATURE 0x4d4a3730u
#define READY_BIT ((LONG64)(UINT64_C(1)<<62))
#define REQUEST_BIT ((LONG64)(UINT64_C(1)<<32))
static LONG64 State(MALI_DDI_ADAPTER *D) { return InterlockedCompareExchange64(&D->SchedulerState,0,0); }
static int CanSchedule(MALI_DDI_ADAPTER *D) { return (State(D)&READY_BIT)!=0; }
static NTSTATUS Status(MALI_RESULT R) {
    switch(R) {
        case MaliOk:return STATUS_SUCCESS;
        case MaliInvalid:return STATUS_INVALID_PARAMETER;
        case MaliUnsupported:return STATUS_NOT_SUPPORTED;
        case MaliBusy:return STATUS_DEVICE_BUSY;
        case MaliTimeout:return STATUS_IO_TIMEOUT;
        case MaliFault:return STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE;
        default:return STATUS_DEVICE_NOT_READY;
    }
}
static int Valid(const MALI_DDI_ADAPTER *D) {return D && D->Signature==SIGNATURE;}
static NTSTATUS Enter(MALI_DDI_ADAPTER *D) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !Valid(D)) return STATUS_INVALID_PARAMETER;
    return KeWaitForSingleObject(&D->Gate,Executive,KernelMode,FALSE,0);
}
static void Leave(MALI_DDI_ADAPTER *D) { KeReleaseMutex(&D->Gate,FALSE); }
enum {IrqConnect=0,IrqRearm=1,IrqPause=2,IrqDisconnect=3};
static const uint32_t IrqMaskOffsets[3]={0x28,0x1008,0x2008};
static const uint32_t IrqStatusOffsets[3]={0x2c,0x100c,0x200c};
typedef struct {MALI_DDI_ADAPTER *D;uint32_t Action,Called;NTSTATUS Status;} IRQ_CHANGE;
static LONG IrqConnected(MALI_DDI_ADAPTER *D) {return InterlockedCompareExchange(&D->InterruptConnected,0,0);}
NTSTATUS MtkMaliDdiValidateInterruptResources(const CM_PARTIAL_RESOURCE_DESCRIPTOR *Resources,
    UINT Count,ULONG *LevelMask,KIRQL *SyncIrql) {
    return MaliValidateInterruptResources(Resources,Count,LevelMask,SyncIrql);
}
NTSTATUS MtkMaliDdiConfigureInterruptDomain(MALI_DDI_ADAPTER *D,
    const CM_PARTIAL_RESOURCE_DESCRIPTOR *Resources,UINT Count) {
    NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    ULONG Levels=0;KIRQL Maximum=0;
    if(D->InterruptLevelMask || IrqConnected(D))S=STATUS_DEVICE_BUSY;
    else if(!D->Queue.Core->ManagedInterrupts || D->InterruptMessage)S=STATUS_NOT_SUPPORTED;
    else S=MtkMaliDdiValidateInterruptResources(Resources,Count,&Levels,&Maximum);
    /* Publish only after the entire input passes, before Connect enables IRQs.
       Failed configuration never touches MMIO or leaves a partial domain. */
    if(S==STATUS_SUCCESS){D->InterruptSyncIrql=Maximum;D->InterruptLevelMask=Levels;}
    Leave(D);return S;
}
static BOOLEAN IrqLock(MALI_DDI_ADAPTER *D,KIRQL *Previous) {
    KIRQL Current=KeGetCurrentIrql();
    if(Current<=DISPATCH_LEVEL || Current>D->InterruptSyncIrql ||
       !(D->InterruptLevelMask&(1u<<Current)))return FALSE;
    /* Raising first prevents a higher-priority source interrupting this CPU
       while the shared lock is held. The lock also excludes other CPUs. */
    KeRaiseIrql(D->InterruptSyncIrql,Previous);
    KeAcquireSpinLockAtDpcLevel(&D->InterruptGate);return TRUE;
}
static void IrqUnlock(MALI_DDI_ADAPTER *D,KIRQL Previous) {
    KeReleaseSpinLockFromDpcLevel(&D->InterruptGate);KeLowerIrql(Previous);
}
static BOOLEAN IrqAtInterrupt(PVOID Context) {
    IRQ_CHANGE *C=Context;MALI_DDI_ADAPTER *D=C->D;C->Called=1;KIRQL Previous;
    if(!IrqLock(D,&Previous)){C->Status=STATUS_DEVICE_CONFIGURATION_ERROR;return FALSE;}
    uint32_t Action=C->Action,Pending=(uint32_t)InterlockedCompareExchange(&D->InterruptPending,0,0);
    int Arm=Action<=IrqRearm;
    if(Action==IrqConnect)InterlockedExchange(&D->InterruptConnected,1);
    int All=Action!=IrqRearm || InterlockedCompareExchange(&D->InterruptPaused,0,0) ||
        InterlockedCompareExchange(&D->InterruptRetry,0,0);
    for(uint32_t I=0;I<3;I++)if(All || (Pending&(1u<<I)))
        D->InterruptIo.Write32(D->InterruptIo.Context,IrqMaskOffsets[I],Arm?D->InterruptMasks[I]:0);
    D->InterruptIo.Barrier(D->InterruptIo.Context);
    for(uint32_t I=0;I<3;I++)if((All || (Pending&(1u<<I))) &&
        D->InterruptIo.Read32(D->InterruptIo.Context,IrqMaskOffsets[I])!=(Arm?D->InterruptMasks[I]:0)) {
        C->Status=STATUS_IO_DEVICE_ERROR;break;
    }
    if(C->Status!=STATUS_SUCCESS) {
        /* Keep the owner and pending work on an unconfirmed hardware write. */
        for(uint32_t I=0;I<3;I++)D->InterruptIo.Write32(D->InterruptIo.Context,IrqMaskOffsets[I],0);
        D->InterruptIo.Barrier(D->InterruptIo.Context);
        InterlockedExchange(&D->InterruptPaused,1);InterlockedExchange(&D->InterruptRetry,1);
        IrqUnlock(D,Previous);return FALSE;
    }
    InterlockedExchange(&D->InterruptPending,0);InterlockedExchange(&D->InterruptPaused,!Arm);
    InterlockedExchange(&D->InterruptRetry,0);
    if(Action==IrqDisconnect)InterlockedExchange(&D->InterruptConnected,0);
    IrqUnlock(D,Previous);return TRUE;
}
/* Caller holds the PASSIVE adapter gate. The port enters a synchronization
   callback; IrqLock then excludes every GPU source. ISR never takes the gate. */
static NTSTATUS IrqChange(MALI_DDI_ADAPTER *D,uint32_t Action) {
    if(Action!=IrqConnect && !IrqConnected(D))return STATUS_SUCCESS;
    IRQ_CHANGE C={D,Action,0,STATUS_SUCCESS};BOOLEAN Returned=FALSE;
    NTSTATUS S=D->Synchronize(D->DxgkHandle,IrqAtInterrupt,&C,D->InterruptMessage,&Returned);
    if(S==STATUS_SUCCESS)S=!C.Called?STATUS_UNSUCCESSFUL:C.Status!=STATUS_SUCCESS?C.Status:Returned?STATUS_SUCCESS:STATUS_UNSUCCESSFUL;
    if(S!=STATUS_SUCCESS && IrqConnected(D))InterlockedExchange(&D->InterruptRetry,1);
    D->LastInterruptStatus=S;return S;
}
NTSTATUS MtkMaliDdiConnectInterrupts(MALI_DDI_ADAPTER *D) {
    NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    MALI_JM *Core=D->Queue.Core;
    if(IrqConnected(D))S=STATUS_DEVICE_BUSY;
    else if((!CanSchedule(D) && !D->PowerPaused) || D->Queue.Faulted || !Core->Started || Core->Poisoned)S=STATUS_DEVICE_NOT_READY;
    else if(!Core->ManagedInterrupts || D->InterruptMessage)S=STATUS_NOT_SUPPORTED;
    else if(!D->InterruptLevelMask)S=STATUS_DEVICE_CONFIGURATION_ERROR;
    else if(!Core->Io.PowerReady(Core->Io.Context))S=STATUS_DEVICE_NOT_READY;
    else {
        D->InterruptIo=Core->Io;D->InterruptMasks[0]=0x81;
        D->InterruptMasks[1]=Core->Caps.JobSlots|(Core->Caps.JobSlots<<16);
        D->InterruptMasks[2]=Core->Caps.AddressSpaces|(Core->Caps.AddressSpaces<<16);
        S=IrqChange(D,IrqConnect);
    }
    Leave(D);return S;
}
NTSTATUS MtkMaliDdiDisconnectInterrupts(MALI_DDI_ADAPTER *D) {
    NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    S=IrqChange(D,IrqDisconnect);Leave(D);return S;
}
BOOLEAN MtkMaliDdiInterruptRoutine(PVOID Adapter,ULONG Message) {
    MALI_DDI_ADAPTER *D=Adapter;
    if(!Valid(D) || KeGetCurrentIrql()<=DISPATCH_LEVEL || Message!=D->InterruptMessage || !IrqConnected(D))return FALSE;
    KIRQL Previous;if(!IrqLock(D,&Previous))return FALSE;
    /* Disconnect may have won after the first lock-free ownership check. */
    if(!IrqConnected(D)){IrqUnlock(D,Previous);return FALSE;}
    uint32_t Pending=0;
    for(uint32_t I=0;I<3;I++)if(D->InterruptIo.Read32(D->InterruptIo.Context,IrqStatusOffsets[I])&D->InterruptMasks[I]) {
        Pending|=1u<<I;D->InterruptIo.Write32(D->InterruptIo.Context,IrqMaskOffsets[I],0);
    }
    if(!Pending){IrqUnlock(D,Previous);return FALSE;}
    /* Preserve RAW/status and fences for the serialized real Poll path. Masking
       deasserts level sources; no event/retained memory is retired at DIRQL. */
    D->InterruptIo.Barrier(D->InterruptIo.Context);
    InterlockedOr(&D->InterruptPending,(LONG)Pending);IrqUnlock(D,Previous);
    D->QueueDpc(D->DxgkHandle);return TRUE;
}
static int InPool(const void *P,const void *Base,size_t Count,size_t Size) {
    uintptr_t V=(uintptr_t)P, B=(uintptr_t)Base;
    return V>=B && V-B<Count*Size && !((V-B)%Size);
}
static MALI_DDI_PROCESS *Process(MALI_DDI_ADAPTER *D,HANDLE H) {
    MALI_DDI_PROCESS *P=H;
    return InPool(P,D->Processes,MALI_DDI_OBJECTS,sizeof(*P)) && P->Live ? P : 0;
}
static MALI_DDI_CONTEXT *Context(MALI_DDI_ADAPTER *D,HANDLE H) {
    MALI_DDI_CONTEXT *C=H;
    return InPool(C,D->Contexts,MALI_DDI_OBJECTS,sizeof(*C)) && C->Live ? C : 0;
}
static int DeviceValid(MALI_DDI_ADAPTER *D,MALI_DDI_DEVICE *V) {
    return InPool(V,D->Devices,MALI_DDI_OBJECTS,sizeof(*V)) && V->Live;
}
MALI_RESULT MtkMaliDdiInitialize(MALI_DDI_ADAPTER *D,MALI_JM *Core,uint64_t Timeout,
    const DXGKRNL_INTERFACE *K,ULONG Message,const MALI_DDI_HOOKS *Hooks,
    const MALI_WDDM_CACHE_OPS *Cache) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !D || !Core || !Core->Started || Core->Poisoned ||
       !K || K->Size<FIELD_OFFSET(DXGKRNL_INTERFACE,DxgkCbNotifyDpc)+sizeof(K->DxgkCbNotifyDpc) ||
       !K->DeviceHandle || !K->DxgkCbSynchronizeExecution || !K->DxgkCbNotifyInterrupt ||
       !K->DxgkCbQueueDpc || !K->DxgkCbNotifyDpc || !Hooks || !Hooks->Prepare ||
       !Hooks->Release || !Hooks->Wake || !Cache || !Cache->CleanForGpu || !Cache->StoreBarrier)
        return MaliInvalid;
    RtlZeroMemory(D,sizeof(*D));
    MALI_RESULT R=MaliExecInitialize(&D->Queue,Core,Timeout);if(R)return R;
    R=MtkMaliWddmInitialize(&D->Paging,Cache);if(R)return R;
    KeInitializeMutex(&D->Gate,0);KeInitializeSpinLock(&D->InterruptGate);
    D->Hooks=*Hooks;D->DxgkHandle=K->DeviceHandle;
    D->InterruptMessage=Message;D->Synchronize=K->DxgkCbSynchronizeExecution;
    D->NotifyInterrupt=K->DxgkCbNotifyInterrupt;D->QueueDpc=K->DxgkCbQueueDpc;
    D->NotifyDpc=K->DxgkCbNotifyDpc;D->Signature=SIGNATURE;D->PowerCaps=Core->Caps;
    /* Feature negotiation can synchronously query our feature interface. All
       callback-visible adapter state is ready before entering the port. */
    InterlockedExchange64(&D->SchedulerState,READY_BIT);
    MtkMaliAllocationInitialize(&D->Allocations,K);return MaliOk;
}
static int PowerIdle(MALI_DDI_ADAPTER *D) {
    if(D->Queue.ActiveValid || D->Queue.HardwareActive || D->Queue.Count || D->Queue.EventPending ||
       D->Queue.PreemptionPending || (State(D)&REQUEST_BIT) || D->Allocations.ReferenceCount ||
       (D->Vm && D->Vm->HoldCount))return 0;
    for(UINT I=0;I<MALI_DDI_RESIDENTS;I++)if(D->Resident[I].Live)return 0;
    for(UINT I=0;I<MALI_EXEC_CONTEXTS;I++)if(D->Queue.Contexts[I].References)return 0;
    for(UINT I=0;I<MALI_JM_MAX_SLOTS;I++)if(D->Queue.Core->Jobs[I].Active)return 0;
    return 1;
}
NTSTATUS MtkMaliDdiPowerPause(MALI_DDI_ADAPTER *D) {
    NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    if(!PowerIdle(D))S=STATUS_DEVICE_BUSY;
    else if(D->PowerPaused)S=IrqChange(D,IrqPause);
    else if(!CanSchedule(D) || D->Queue.Faulted || !D->Queue.Core->Started || D->Queue.Core->Poisoned)
        S=STATUS_DEVICE_NOT_READY;
    else {
        /* Consume idle GPU/MMU faults before masking or losing register state.
           The queue is empty, so the existing monotonic time is sufficient. */
        S=Status(MaliExecPump(&D->Queue,D->Queue.LastTime));
        if(S==STATUS_SUCCESS) {
            if(InterlockedCompareExchange64(&D->SchedulerState,0,READY_BIT)!=READY_BIT)S=STATUS_DEVICE_BUSY;
            else {D->PowerPaused=1;D->ProgressSeen=0;S=IrqChange(D,IrqPause);}
        }
    }
    Leave(D);return S;
}
NTSTATUS MtkMaliDdiPowerResume(MALI_DDI_ADAPTER *D) {
    NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    if(!D->PowerPaused || State(D) || !PowerIdle(D))S=STATUS_DEVICE_BUSY;
    else if(D->Queue.Faulted || !D->Queue.Core->Started || D->Queue.Core->Poisoned ||
            !D->Queue.Core->Io.PowerReady(D->Queue.Core->Io.Context) ||
            (D->Queue.Core->ManagedInterrupts && (!IrqConnected(D) ||
             InterlockedCompareExchange(&D->InterruptPaused,0,0) ||
             InterlockedCompareExchange(&D->InterruptRetry,0,0))))S=STATUS_DEVICE_NOT_READY;
    else {
        S=STATUS_SUCCESS;
        const MALI_CAPS *A=&D->PowerCaps,*B=&D->Queue.Core->Caps;
        if(A->GpuId!=B->GpuId || A->MmuFeatures!=B->MmuFeatures || A->AddressSpaces!=B->AddressSpaces ||
           A->JobSlots!=B->JobSlots || A->Shaders!=B->Shaders || A->Tilers!=B->Tilers || A->L2!=B->L2)
            S=STATUS_DEVICE_CONFIGURATION_ERROR;
        for(UINT I=0;I<MALI_JM_MAX_SLOTS;I++)if(A->JobFeatures[I]!=B->JobFeatures[I])S=STATUS_DEVICE_CONFIGURATION_ERROR;
        if(S==STATUS_SUCCESS) {
            D->ProgressSeen=0;D->PowerPaused=0;InterlockedExchange64(&D->SchedulerState,READY_BIT);
        }
    }
    Leave(D);return S;
}
static NTSTATUS Quiesce(MALI_DDI_ADAPTER *D) {
    NTSTATUS S=STATUS_SUCCESS;
    if(InterlockedCompareExchange64(&D->InterfaceReferences,0,0) ||
       (D->Vm && (D->Vm->TableCount || D->Vm->HoldCount)) || MtkMaliAllocationHasObjects(&D->Allocations) ||
       D->Queue.ActiveValid || D->Queue.Count || D->Queue.EventPending ||
       D->Queue.PreemptionPending || (State(D)&REQUEST_BIT))S=STATUS_DEVICE_BUSY;
    for(unsigned I=0;I<MALI_DDI_OBJECTS;I++)
        if(D->Processes[I].Live || D->Devices[I].Live || D->Contexts[I].Live)S=STATUS_DEVICE_BUSY;
    for(unsigned I=0;I<MALI_DDI_RESIDENTS;I++)if(D->Resident[I].Live)S=STATUS_DEVICE_BUSY;
    for(unsigned I=0;I<MALI_JM_MAX_SLOTS;I++)if(D->Queue.Core->Jobs[I].Active)S=STATUS_DEVICE_BUSY;
    if(S==STATUS_SUCCESS) {
        LONG64 Old=InterlockedCompareExchange64(&D->SchedulerState,0,READY_BIT);
        if(Old && Old!=READY_BIT)S=STATUS_DEVICE_BUSY;
        if(S==STATUS_SUCCESS)S=IrqChange(D,IrqPause);
    }
    return S;
}
NTSTATUS MtkMaliDdiQuiesce(MALI_DDI_ADAPTER *D) {
    NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    S=Quiesce(D);Leave(D);return S;
}
NTSTATUS MtkMaliDdiShutdown(MALI_DDI_ADAPTER *D) {
    NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    S=Quiesce(D);if(S==STATUS_SUCCESS)S=IrqChange(D,IrqDisconnect);if(S==STATUS_SUCCESS){D->Vm=0;D->PowerPaused=0;}
    Leave(D);return S;
}
NTSTATUS MtkMaliDdiAttachVm(MALI_DDI_ADAPTER *D,MALI_VM *Vm) {
    NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    if(!CanSchedule(D))S=STATUS_DEVICE_NOT_READY;
    else if(!MtkMaliVmUsesBook(Vm,&D->Allocations))S=STATUS_INVALID_PARAMETER;
    else if(D->Vm || Vm->TableCount || Vm->HoldCount || D->Queue.Count || D->Queue.ActiveValid)S=STATUS_DEVICE_BUSY;
    else {D->Vm=Vm;S=STATUS_SUCCESS;}
    Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiCreateProcess(HANDLE Adapter,DXGKARG_CREATEPROCESS *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(!NT_SUCCESS(S))return S;
    if(!A || !A->hDxgkProcess) S=STATUS_INVALID_PARAMETER;
    else if(A->Flags.Value&~3u || A->NumPasid || A->hKmdVmWorkerProcess) S=STATUS_NOT_SUPPORTED;
    else if(!CanSchedule(D)) S=STATUS_DEVICE_NOT_READY;
    else if(D->NextOwner==UINT64_MAX) S=STATUS_INSUFFICIENT_RESOURCES;
    else {
        S=STATUS_INSUFFICIENT_RESOURCES;
        for(unsigned I=0;I<MALI_DDI_OBJECTS;I++) if(!D->Processes[I].Live) {
            MALI_DDI_PROCESS P={D,A->hDxgkProcess,++D->NextOwner,1,0};
            D->Processes[I]=P;A->hKmdProcess=&D->Processes[I];S=STATUS_SUCCESS;break;
        }
    }
    Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiDestroyProcess(HANDLE Adapter,HANDLE Handle) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(!NT_SUCCESS(S))return S;
    MALI_DDI_PROCESS *P=Process(D,Handle);
    if(!P)S=STATUS_INVALID_PARAMETER;
    else if(P->Devices)S=STATUS_DEVICE_BUSY;
    else {
        S=D->Vm?MtkMaliVmForgetProcess(D->Vm,P->Owner):STATUS_SUCCESS;
        if(S==STATUS_SUCCESS)RtlZeroMemory(P,sizeof(*P));
    }
    Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiCreateDevice(HANDLE Adapter,DXGKARG_CREATEDEVICE *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(!NT_SUCCESS(S))return S;
    MALI_DDI_PROCESS *P=A ? Process(D,A->hKmdProcess) : 0;
    if(!A || !P)S=STATUS_INVALID_PARAMETER;
    else if(A->Flags.Value&~3u || A->Pasid)S=STATUS_NOT_SUPPORTED;
    else if(!CanSchedule(D))S=STATUS_DEVICE_NOT_READY;
    else {
        S=STATUS_INSUFFICIENT_RESOURCES;
        for(unsigned I=0;I<MALI_DDI_OBJECTS;I++) if(!D->Devices[I].Live) {
            MALI_DDI_DEVICE V={D,P,1,0};D->Devices[I]=V;
            ++P->Devices;A->hDevice=&D->Devices[I];S=STATUS_SUCCESS;break;
        }
    }
    Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiDestroyDevice(HANDLE Handle) {
    MALI_DDI_DEVICE *V=Handle;
    if(!V) return STATUS_INVALID_PARAMETER;
    MALI_DDI_ADAPTER *D=V->Adapter;NTSTATUS S=Enter(D);if(!NT_SUCCESS(S))return S;
    if(!DeviceValid(D,V))S=STATUS_INVALID_PARAMETER;
    else if(V->Contexts || MtkMaliAllocationHasDevice(&D->Allocations,V))S=STATUS_DEVICE_BUSY;
    else {--V->Process->Devices;RtlZeroMemory(V,sizeof(*V));S=STATUS_SUCCESS;}
    Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiCreateContext(HANDLE Handle,DXGKARG_CREATECONTEXT *A) {
    MALI_DDI_DEVICE *V=Handle;if(!V)return STATUS_INVALID_PARAMETER;
    MALI_DDI_ADAPTER *D=V->Adapter;NTSTATUS S=Enter(D);if(!NT_SUCCESS(S))return S;
    if(!DeviceValid(D,V) || !A) S=STATUS_INVALID_PARAMETER;
    else if(A->NodeOrdinal || A->EngineAffinity!=1 || !A->Flags.VirtualAddressing ||
        (A->Flags.Value&~5u) || A->PrivateDriverDataSize) S=STATUS_NOT_SUPPORTED;
    else if(!CanSchedule(D)) S=STATUS_DEVICE_NOT_READY;
    else {
        S=STATUS_INSUFFICIENT_RESOURCES;
        for(unsigned I=0;I<MALI_DDI_OBJECTS;I++) if(!D->Contexts[I].Live) {
            uint64_t Id=0;S=Status(MaliExecCreateContext(&D->Queue,V->Process->Owner,&Id));
            if(!NT_SUCCESS(S))break;
            MALI_DDI_CONTEXT C={D,V,Id,0,1,STATUS_DEVICE_NOT_READY};D->Contexts[I]=C;
            RtlZeroMemory(&A->ContextInfo,sizeof(A->ContextInfo));
            A->ContextInfo.DmaBufferSize=MALI_DDI_DMA_BYTES;
            A->ContextInfo.DmaBufferPrivateDataSize=MALI_COMMAND_MAX_PRIVATE;
            A->ContextInfo.Caps.NoPatchingRequired=1;
            A->hContext=&D->Contexts[I];++V->Contexts;break;
        }
    }
    Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiDestroyContext(HANDLE Handle) {
    MALI_DDI_CONTEXT *C=Handle;if(!C)return STATUS_INVALID_PARAMETER;
    MALI_DDI_ADAPTER *D=C->Adapter;NTSTATUS S=Enter(D);if(!NT_SUCCESS(S))return S;
    if(!Context(D,Handle))S=STATUS_INVALID_PARAMETER;
    else {
        S=Status(MaliExecDestroyContext(&D->Queue,C->Id));
        if(NT_SUCCESS(S)){--C->Device->Contexts;RtlZeroMemory(C,sizeof(*C));}
    }
    Leave(D);return S;
}
VOID APIENTRY MtkMaliDdiSetRootPageTable(HANDLE Adapter,const DXGKARG_SETROOTPAGETABLE *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(!NT_SUCCESS(S))return;
    MALI_DDI_CONTEXT *C=A ? Context(D,A->hContext) : 0;
    if(!C)S=STATUS_INVALID_PARAMETER;
    else {
        if(A->Address.SegmentId || A->Address.Padding || A->NumEntries!=512)S=STATUS_NOT_SUPPORTED;
        else S=Status(MaliExecSetRoot(&D->Queue,C->Id,A->Address.SegmentOffset));
        /* A void DDI cannot return failure. Block subsequent submits on this
           context rather than silently running with an old/wrong root. */
        C->RootStatus=S;if(NT_SUCCESS(S))C->Root=A->Address.SegmentOffset;
    }
    D->LastRootStatus=S;Leave(D);
}
SIZE_T APIENTRY MtkMaliDdiGetRootPageTableSize(HANDLE Adapter,DXGKARG_GETROOTPAGETABLESIZE *A) {
    MALI_DDI_ADAPTER *D=Adapter;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !Valid(D) || !A || A->PhysicalAdapterIndex ||
       A->NumberOfPte>512)return 0;
    /* WDDM only calls this for two levels; our four-level caps fix the root. */
    A->NumberOfPte=512;return 4096;
}
NTSTATUS APIENTRY MtkMaliDdiSubmitCommandVirtual(HANDLE Adapter,const DXGKARG_SUBMITCOMMANDVIRTUAL *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(!NT_SUCCESS(S))return S;
    MALI_DDI_CONTEXT *C=A ? Context(D,A->hContext) : 0;
    if(!C)S=STATUS_INVALID_PARAMETER;
    else if(A->NodeOrdinal || A->EngineOrdinal || (A->Flags.Value&~(1u<<7)))S=STATUS_NOT_SUPPORTED;
    else if(!A->DmaBufferVirtualAddress || (A->DmaBufferVirtualAddress&63u) ||
            A->DmaBufferVirtualAddress>=MALI_JM_VA_LIMIT || A->DmaBufferSize<64 ||
            A->DmaBufferSize>MALI_JM_VA_LIMIT-A->DmaBufferVirtualAddress ||
            A->DmaBufferPrivateDataSize>MALI_COMMAND_MAX_PRIVATE ||
            (A->DmaBufferPrivateDataSize && !A->pDmaBufferPrivateData) ||
            A->DmaBufferUmdPrivateDataSize>A->DmaBufferPrivateDataSize)S=STATUS_INVALID_PARAMETER;
    else if(!NT_SUCCESS(C->RootStatus))S=C->RootStatus;
    else if(!CanSchedule(D))S=STATUS_DEVICE_NOT_READY;
    else if(D->Queue.Faulted)S=STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE;
    else if((State(D)&REQUEST_BIT) || D->Queue.PreemptionPending ||
            D->Queue.Count==MALI_EXEC_QUEUE_DEPTH)S=STATUS_DEVICE_BUSY;
    else {
        MALI_DDI_RESIDENT *Resident=0;
        for(unsigned I=0;I<MALI_DDI_RESIDENTS;I++)if(!D->Resident[I].Live){Resident=&D->Resident[I];break;}
        if(!Resident)S=STATUS_INSUFFICIENT_RESOURCES;
        else {
            MALI_DDI_PREPARED Prepared={0};
            S=D->Hooks.Prepare(D->Hooks.Context,C->Device->Process->DxgkProcess,C->Id,C->Root,A,&Prepared);
            if(NT_SUCCESS(S)) {
                if(!Prepared.Cookie)S=STATUS_INVALID_PARAMETER;
                else {
                    S=Status(MaliExecSubmitRoot(&D->Queue,C->Id,Prepared.ExecutionRoot,A->SubmissionFenceId,
                        Prepared.DmaVa,Prepared.DmaBytes,&Prepared.Packet,Prepared.Packet.Bytes));
                    if(!NT_SUCCESS(S))D->Hooks.Release(D->Hooks.Context,Prepared.Cookie,MaliDdiRejected);
                    else {Resident->Cookie=Prepared.Cookie;Resident->Fence=A->SubmissionFenceId;Resident->Live=1;}
                }
            }
        }
    }
    Leave(D);if(NT_SUCCESS(S))D->Hooks.Wake(D->Hooks.Context);return S;
}
NTSTATUS APIENTRY MtkMaliDdiPreemptCommand(HANDLE Adapter,const DXGKARG_PREEMPTCOMMAND *A) {
    MALI_DDI_ADAPTER *D=Adapter;
    if(KeGetCurrentIrql()!=DISPATCH_LEVEL || !Valid(D) || !A)return STATUS_INVALID_PARAMETER;
    if(A->NodeOrdinal || A->EngineOrdinal || A->Flags.Value)return STATUS_NOT_SUPPORTED;
    LONG64 Old=State(D);
    for(;;) {
        if(!(Old&READY_BIT))return STATUS_DEVICE_NOT_READY;
        if(Old&REQUEST_BIT)return STATUS_DEVICE_BUSY;
        LONG64 New=Old|REQUEST_BIT|A->PreemptionFenceId;
        LONG64 Seen=InterlockedCompareExchange64(&D->SchedulerState,New,Old);
        if(Seen==Old)break;
        Old=Seen;
    }
    D->Hooks.Wake(D->Hooks.Context);return STATUS_SUCCESS;
}
NTSTATUS APIENTRY MtkMaliDdiQueryCurrentFence(HANDLE Adapter,DXGKARG_QUERYCURRENTFENCE *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(!NT_SUCCESS(S))return S;
    if(!A)S=STATUS_INVALID_PARAMETER;
    else if(A->NodeOrdinal || A->EngineOrdinal)S=STATUS_NOT_SUPPORTED;
    else {A->CurrentFence=D->Queue.LastCompletedFence;S=STATUS_SUCCESS;}
    Leave(D);return S;
}
static void ReleaseResident(MALI_DDI_ADAPTER *D,uint32_t Fence,int All,MALI_DDI_RELEASE_REASON Why) {
    for(unsigned I=0;I<MALI_DDI_RESIDENTS;I++) {
        MALI_DDI_RESIDENT *R=&D->Resident[I];
        if(R->Live && (All || R->Fence==Fence)) {
            D->Hooks.Release(D->Hooks.Context,R->Cookie,Why);RtlZeroMemory(R,sizeof(*R));
        }
    }
}
typedef struct { MALI_DDI_ADAPTER *D; DXGKARGCB_NOTIFY_INTERRUPT_DATA Data; int Sent; } NOTIFICATION;
static BOOLEAN NotifyAtInterrupt(PVOID Context) {
    NOTIFICATION *N=Context;
    N->D->NotifyInterrupt(N->D->DxgkHandle,&N->Data);N->Sent=1;
    /* FALSE can mean an existing DPC is already queued; the notification has
       still been delivered and must never be sent twice. */
    N->D->QueueDpc(N->D->DxgkHandle);return TRUE;
}
static NTSTATUS Notify(MALI_DDI_ADAPTER *D,const MALI_EXEC_EVENT *E) {
    NOTIFICATION N={0};N.D=D;
    if(E->Type==MaliExecCompleted) {
        N.Data.InterruptType=DXGK_INTERRUPT_DMA_COMPLETED;N.Data.DmaCompleted.SubmissionFenceId=E->Fence;
    } else if(E->Type==MaliExecPreempted) {
        N.Data.InterruptType=DXGK_INTERRUPT_DMA_PREEMPTED;N.Data.DmaPreempted.PreemptionFenceId=E->Fence;
        N.Data.DmaPreempted.LastCompletedFenceId=E->LastCompletedFence;
    } else {
        N.Data.InterruptType=DXGK_INTERRUPT_DMA_FAULTED;N.Data.DmaFaulted.FaultedFenceId=E->Fence;
        N.Data.DmaFaulted.Status=STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE;
    }
    BOOLEAN Return=FALSE;
    NTSTATUS S=D->Synchronize(D->DxgkHandle,NotifyAtInterrupt,&N,D->InterruptMessage,&Return);
    D->LastNotifyStatus=S;
    return N.Sent ? STATUS_SUCCESS : NT_SUCCESS(S) ? STATUS_UNSUCCESSFUL : S;
}
NTSTATUS MtkMaliDdiPump(MALI_DDI_ADAPTER *D,uint64_t Now) {
    NTSTATUS S=Enter(D);if(!NT_SUCCESS(S))return S;
    if(!CanSchedule(D)){Leave(D);return STATUS_DEVICE_NOT_READY;}
    for(unsigned I=0;I<2;I++) {
        /* NotifyInterrupt can allow a new dispatch-level preemption before
           the next iteration. Recheck before starting another DMA packet. */
        LONG64 Request=State(D);
        if((Request&REQUEST_BIT) && !D->Queue.PreemptionPending) {
            S=Status(MaliExecRequestPreempt(&D->Queue,(uint32_t)Request));
            if(!NT_SUCCESS(S)){Leave(D);return S;}
            /* Keep the request occupied until its actual notification. */
        }
        MALI_RESULT R=MaliExecPump(&D->Queue,Now);S=Status(R);
        MALI_EXEC_EVENT E;
        if(MaliExecPeek(&D->Queue,&E)!=MaliOk)break;
        NTSTATUS N=Notify(D,&E);if(!NT_SUCCESS(N)){S=N;break;}
        MALI_RESULT A=MaliExecAcknowledge(&D->Queue,&E);
        if(A){S=Status(A);break;}
        if(E.Type==MaliExecCompleted)ReleaseResident(D,E.Fence,0,MaliDdiCompleted);
        else if(E.Type==MaliExecPreempted) {
            ReleaseResident(D,0,1,MaliDdiPreempted);
            InterlockedAnd64(&D->SchedulerState,READY_BIT);
        } else break;
    }
    NTSTATUS IrqStatus=IrqChange(D,D->Queue.Faulted || D->Queue.Core->Poisoned || !D->Queue.Core->Started?IrqPause:IrqRearm);
    if(S==STATUS_SUCCESS && IrqStatus!=STATUS_SUCCESS)S=IrqStatus;
    Leave(D);return S;
}
NTSTATUS MtkMaliDdiCheckRunning(MALI_DDI_ADAPTER *D) {
    NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    S=CanSchedule(D) && D->Queue.Core->Started && !D->Queue.Faulted && !D->Queue.Core->Poisoned?
        STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
    Leave(D);return S;
}
NTSTATUS MtkMaliDdiHasWork(MALI_DDI_ADAPTER *D,BOOLEAN *Pending) {
    NTSTATUS S=Enter(D);if(!NT_SUCCESS(S))return S;
    if(!Pending)S=STATUS_INVALID_PARAMETER;
    else *Pending=(BOOLEAN)(CanSchedule(D) && (D->Queue.EventPending ||
        (IrqConnected(D) && (InterlockedCompareExchange(&D->InterruptPending,0,0) ||
         InterlockedCompareExchange(&D->InterruptRetry,0,0))) ||
        (!D->Queue.Faulted && (D->Queue.ActiveValid || D->Queue.Count ||
         D->Queue.PreemptionPending || (State(D)&REQUEST_BIT)))));
    Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiResetEngine(HANDLE Adapter,DXGKARG_RESETENGINE *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(!NT_SUCCESS(S))return S;
    if(!A)S=STATUS_INVALID_PARAMETER;
    else if(A->NodeOrdinal || A->EngineOrdinal)S=STATUS_NOT_SUPPORTED;
    else if(D->PowerPaused)S=STATUS_DEVICE_NOT_READY;
    else {
        S=IrqChange(D,IrqPause);if(S!=STATUS_SUCCESS){Leave(D);return S;}
        InterlockedAnd64(&D->SchedulerState,~READY_BIT);
        D->ProgressSeen=0;
        uint64_t Epoch=D->Queue.ResetEpoch;uint32_t Aborted=0;
        S=Status(MaliExecReset(&D->Queue,&Aborted));
        if(D->Queue.ResetEpoch!=Epoch) {
            ReleaseResident(D,0,1,MaliDdiReset);InterlockedAnd64(&D->SchedulerState,READY_BIT);
        }
        if(NT_SUCCESS(S)) {
            A->LastAbortedFenceId=Aborted;InterlockedExchange64(&D->SchedulerState,READY_BIT);
            S=IrqChange(D,IrqRearm);if(S!=STATUS_SUCCESS)InterlockedAnd64(&D->SchedulerState,~READY_BIT);
        }
    }
    Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiBuildPagingBuffer(HANDLE Adapter,DXGKARG_BUILDPAGINGBUFFER *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(!NT_SUCCESS(S))return S;
    if(!A)S=STATUS_INVALID_PARAMETER;
    else if(A->Operation==DXGK_OPERATION_UPDATE_PAGE_TABLE) {
        if(D->Queue.ActiveValid || D->Queue.Count)S=STATUS_DEVICE_BUSY;
        else if(D->Vm) {
            MALI_DDI_PROCESS *P=Process(D,A->UpdatePageTable.hProcess);
            S=P?MtkMaliVmUpdate(D->Vm,&D->Paging,P->Owner,A):STATUS_INVALID_PARAMETER;
        } else S=Status(MtkMaliWddmUpdatePageTable(&D->Paging,A));
    } else if(A->Operation==DXGK_OPERATION_FLUSH_TLB) {
        const DXGK_BUILDPAGINGBUFFER_FLUSHTLB *F=&A->FlushTlb;
        if(!Process(D,F->hProcess) || F->RootPageTableAddress.SegmentId || F->RootPageTableAddress.Padding ||
           A->pDmaBuffer || F->StartVirtualAddress>=MALI_JM_VA_LIMIT ||
           F->EndVirtualAddress>=MALI_JM_VA_LIMIT || F->StartVirtualAddress>F->EndVirtualAddress)
            S=STATUS_INVALID_PARAMETER;
        else {
            /* Full-root invalidation also covers the requested range. It avoids
               treating the API's end address as an invented length/alignment. */
            S=Status(MaliExecFlush(&D->Queue,F->RootPageTableAddress.SegmentOffset,0,MALI_JM_VA_LIMIT));
        }
    } else S=STATUS_NOT_SUPPORTED;
    Leave(D);return S;
}
VOID MtkMaliDdiDpcRoutine(PVOID Adapter) {
    MALI_DDI_ADAPTER *D=Adapter;
    if(Valid(D) && KeGetCurrentIrql()==DISPATCH_LEVEL) {
        if(IrqConnected(D) && (InterlockedCompareExchange(&D->InterruptPending,0,0) ||
           InterlockedCompareExchange(&D->InterruptRetry,0,0)))D->Hooks.Wake(D->Hooks.Context);
        D->NotifyDpc(D->DxgkHandle);
    }
}

NTSTATUS APIENTRY MtkMaliDdiCreateAllocation(HANDLE Adapter,DXGKARG_CREATEALLOCATION *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    S=CanSchedule(D)?MtkMaliAllocationCreate(&D->Allocations,A):STATUS_DEVICE_NOT_READY;Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiDestroyAllocation(HANDLE Adapter,const DXGKARG_DESTROYALLOCATION *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    S=MtkMaliAllocationDestroy(&D->Allocations,A);Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiOpenAllocation(HANDLE Handle,const DXGKARG_OPENALLOCATION *A) {
    MALI_DDI_DEVICE *V=Handle;if(!V)return STATUS_INVALID_PARAMETER;
    MALI_DDI_ADAPTER *D=V->Adapter;NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    if(!DeviceValid(D,V))S=STATUS_INVALID_PARAMETER;
    else if(!CanSchedule(D))S=STATUS_DEVICE_NOT_READY;
    else S=MtkMaliAllocationOpen(&D->Allocations,V,V->Process->DxgkProcess,A);
    Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiCloseAllocation(HANDLE Handle,const DXGKARG_CLOSEALLOCATION *A) {
    MALI_DDI_DEVICE *V=Handle;if(!V)return STATUS_INVALID_PARAMETER;
    MALI_DDI_ADAPTER *D=V->Adapter;NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    S=DeviceValid(D,V)?MtkMaliAllocationClose(&D->Allocations,V,A):STATUS_INVALID_PARAMETER;Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiDescribeAllocation(HANDLE Adapter,DXGKARG_DESCRIBEALLOCATION *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    S=MtkMaliAllocationDescribe(&D->Allocations,A);Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiSetAllocationBackingStore(HANDLE Adapter,const DXGKARG_SETALLOCATIONBACKINGSTORE *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    S=MtkMaliAllocationSetBacking(&D->Allocations,A);Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiQueryFeatureSupport(HANDLE Adapter,DXGKARG_QUERYFEATURESUPPORT *A) {
    /* OS feature negotiation may reenter during Initialize's query. Read only
       immutable capability state; do not take Gate around a port callback. */
    MALI_DDI_ADAPTER *D=Adapter;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !Valid(D) || !A)return STATUS_INVALID_PARAMETER;
    A->MinSupportedVersion=A->MaxSupportedVersion=0;A->SupportedByDriver=A->SupportedOnCurrentConfig=FALSE;
    if(A->FeatureId==DXGK_FEATURE_SHARE_BACKING_STORE_WITH_KMD) {
        A->MinSupportedVersion=A->MaxSupportedVersion=1;A->SupportedByDriver=TRUE;
        A->SupportedOnCurrentConfig=D->Allocations.GetHandleData!=0;
    }
    return STATUS_SUCCESS;
}

C_ASSERT(sizeof(MALI_ADAPTER_QUERY)==16);
C_ASSERT(sizeof(MALI_ADAPTER_CAPS)==64);
NTSTATUS MtkMaliDdiConfigureAdapter(MALI_DDI_ADAPTER *D,SIZE_T Limit) {
    NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    if(!CanSchedule(D))S=STATUS_DEVICE_NOT_READY;
    else if(!Limit || (Limit&4095u) || Limit>MALI_ALLOCATION_MAX_BYTES)S=STATUS_INVALID_PARAMETER;
    else if(!D->Vm || !MtkMaliVmUsesBook(D->Vm,&D->Allocations) || !D->Allocations.ShareBackingEnabled)S=STATUS_NOT_SUPPORTED;
    else if(D->Queue.Count || D->Queue.ActiveValid || D->Vm->HoldCount)S=STATUS_DEVICE_BUSY;
    else if(D->AdapterConfigured && D->ApertureCommitLimit!=Limit)S=STATUS_DEVICE_BUSY;
    else {D->ApertureCommitLimit=Limit;D->AdapterConfigured=1;}
    Leave(D);return S;
}
static int AdapterBuffer(const void *P,size_t Bytes) {return P && Bytes && (uintptr_t)P<=UINTPTR_MAX-Bytes;}
static NTSTATUS AdapterInput(const DXGKARG_QUERYADAPTERINFO *A,void *Out,size_t Bytes) {
    if(A->InputDataSize<Bytes || !AdapterBuffer(A->pInputData,Bytes))return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(Out,A->pInputData,Bytes);return STATUS_SUCCESS;
}
static NTSTATUS AdapterQuery(MALI_DDI_ADAPTER *D,const DXGKARG_QUERYADAPTERINFO *A) {
    if(!A || A->Flags.Value)return STATUS_INVALID_PARAMETER;
    if(!D->AdapterConfigured || (!CanSchedule(D) && !D->PowerPaused))return STATUS_DEVICE_NOT_READY;
    if(A->hKmdProcessHandle && !Process(D,A->hKmdProcessHandle))return STATUS_INVALID_HANDLE;
    size_t Need=0;
    switch(A->Type) {
    case DXGKQAITYPE_DRIVERCAPS:Need=sizeof(DXGK_DRIVERCAPS);break;
    case DXGKQAITYPE_GPUMMUCAPS:Need=sizeof(DXGK_GPUMMUCAPS);break;
    case DXGKQAITYPE_PAGETABLELEVELDESC:Need=sizeof(DXGK_PAGE_TABLE_LEVEL_DESC);break;
    case DXGKQAITYPE_QUERYSEGMENT4:Need=sizeof(DXGK_QUERYSEGMENTOUT4);break;
    case DXGKQAITYPE_NUMPOWERCOMPONENTS:Need=sizeof(UINT);break;
    case DXGKQAITYPE_HISTORYBUFFERPRECISION:Need=sizeof(DXGKARG_HISTORYBUFFERPRECISION);break;
    case DXGKQAITYPE_UMDRIVERPRIVATE:Need=sizeof(MALI_ADAPTER_CAPS);break;
    default:return STATUS_NOT_SUPPORTED;
    }
    if(A->OutputDataSize<Need)return STATUS_BUFFER_TOO_SMALL;
    if(!AdapterBuffer(A->pOutputData,Need))return STATUS_INVALID_PARAMETER;
    if(A->Type==DXGKQAITYPE_DRIVERCAPS) {
        if(A->InputDataSize)return STATUS_INVALID_PARAMETER;
        DXGK_DRIVERCAPS C={0};C.HighestAcceptableAddress.QuadPart=MALI_JM_PA_LIMIT-1;
        C.ApertureSegmentCommitLimit=D->ApertureCommitLimit;C.InterruptMessageNumber=D->InterruptMessage;
        C.SchedulingCaps.MultiEngineAware=1;C.SchedulingCaps.PreemptionAware=1;C.SchedulingCaps.NoDmaPatching=1;
        C.SchedulingCaps.No64BitAtomics=1;
        C.MemoryManagementCaps.VirtualAddressingSupported=1;C.MemoryManagementCaps.GpuMmuSupported=1;
        C.GpuEngineTopology.NbAsymetricProcessingNodes=1;C.SupportPerEngineTDR=TRUE;
        C.PresentationCaps.NoCacheCoherentApertureMemory=1;
        C.PreemptionCaps.GraphicsPreemptionGranularity=D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
        C.PreemptionCaps.ComputePreemptionGranularity=D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
        C.InternalGpuVirtualAddressRangeStart=MALI_JM_VA_LIMIT-MALI_COMMAND_SHADOW_BYTES;
        C.InternalGpuVirtualAddressRangeEnd=MALI_JM_VA_LIMIT-1;
        /* WDDMVersion is reserved for modern DDIs. No display, native fences,
           hardware queues, IoMmu, protected memory or runtime-power claims. */
        RtlCopyMemory(A->pOutputData,&C,sizeof(C));return STATUS_SUCCESS;
    }
    if(A->Type==DXGKQAITYPE_GPUMMUCAPS || A->Type==DXGKQAITYPE_PAGETABLELEVELDESC) {
        UINT Index=0;NTSTATUS S;
        if(A->Type==DXGKQAITYPE_GPUMMUCAPS) {
            DXGK_QUERYGPUMMUCAPSIN I;S=AdapterInput(A,&I,sizeof(I));if(S!=STATUS_SUCCESS)return S;
            if(I.PhysicalAdapterIndex)return STATUS_NOT_SUPPORTED;
        } else {
            DXGK_QUERYPAGETABLELEVELDESCIN I;S=AdapterInput(A,&I,sizeof(I));if(S!=STATUS_SUCCESS)return S;
            if(I.PhysicalAdapterIndex)return STATUS_NOT_SUPPORTED;if(I.LevelIndex>=4)return STATUS_INVALID_PARAMETER;Index=I.LevelIndex;
        }
        DXGK_GPUMMUCAPS C;DXGK_PAGE_TABLE_LEVEL_DESC L[4];
        if(MtkMaliWddmGetMmuCaps(&C,L,4)!=MaliOk)return STATUS_UNSUCCESSFUL;
        RtlCopyMemory(A->pOutputData,A->Type==DXGKQAITYPE_GPUMMUCAPS?(const void*)&C:(const void*)&L[Index],Need);
        return STATUS_SUCCESS;
    }
    if(A->Type==DXGKQAITYPE_QUERYSEGMENT4) {
        DXGK_QUERYSEGMENTIN4 I;NTSTATUS S=AdapterInput(A,&I,sizeof(I));if(S!=STATUS_SUCCESS)return S;
        if(I.PhysicalAdapterIndex)return STATUS_NOT_SUPPORTED;
        DXGK_QUERYSEGMENTOUT4 *O=A->pOutputData;
        /* The count-only call must touch no other field. */
        if(!O->NbSegment){O->NbSegment=1;return STATUS_SUCCESS;}
        if(O->NbSegment!=1 || O->SegmentDescriptorStride<sizeof(DXGK_SEGMENTDESCRIPTOR4) ||
           !AdapterBuffer(O->pSegmentDescriptor,sizeof(DXGK_SEGMENTDESCRIPTOR4)) ||
           ((uintptr_t)O->pSegmentDescriptor&(__alignof(DXGK_SEGMENTDESCRIPTOR4)-1u)))return STATUS_INVALID_PARAMETER;
        uintptr_t B=(uintptr_t)O->pSegmentDescriptor,H=(uintptr_t)O;
        if(B<H+sizeof(*O) && H<B+sizeof(DXGK_SEGMENTDESCRIPTOR4))return STATUS_INVALID_PARAMETER;
        DXGK_SEGMENTDESCRIPTOR4 Segment={0};Segment.Flags.Aperture=1;
        Segment.Size=D->ApertureCommitLimit;Segment.CommitLimit=D->ApertureCommitLimit;
        RtlCopyMemory(O->pSegmentDescriptor,&Segment,sizeof(Segment));
        O->PagingBufferSegmentId=0;O->PagingBufferSize=0;O->PagingBufferPrivateDataSize=0;
        return STATUS_SUCCESS;
    }
    if(A->Type==DXGKQAITYPE_NUMPOWERCOMPONENTS) {
        if(A->InputDataSize)return STATUS_INVALID_PARAMETER;
        UINT Zero=0;RtlCopyMemory(A->pOutputData,&Zero,sizeof(Zero));return STATUS_SUCCESS;
    }
    if(A->Type==DXGKQAITYPE_HISTORYBUFFERPRECISION) {
        DXGK_QUERYHISTORYBUFFERPRECISIONIN I;NTSTATUS S=AdapterInput(A,&I,sizeof(I));if(S!=STATUS_SUCCESS)return S;
        if(I.PhysicalAdapterIndex)return STATUS_NOT_SUPPORTED;
        DXGKARG_HISTORYBUFFERPRECISION H={0};RtlCopyMemory(A->pOutputData,&H,sizeof(H));return STATUS_SUCCESS;
    }
    MALI_ADAPTER_QUERY I;NTSTATUS S=AdapterInput(A,&I,sizeof(I));if(S!=STATUS_SUCCESS)return S;
    if(A->InputDataSize!=sizeof(I) || I.Version!=MALI_ADAPTER_ABI_VERSION || I.Bytes!=sizeof(I) || I.Flags || I.Reserved)
        return STATUS_GRAPHICS_DRIVER_MISMATCH;
    MALI_ADAPTER_CAPS C={0};C.Version=1;C.Bytes=sizeof(C);C.GpuId=D->PowerCaps.GpuId;
    for(UINT J=0;J<MALI_JM_MAX_SLOTS;J++)if(D->PowerCaps.JobSlots&(1u<<J)) {
        UINT F=D->PowerCaps.JobFeatures[J];
        if(F&(1u<<2))C.Features|=MALI_ADAPTER_IMMEDIATE_WRITE|MALI_ADAPTER_TIMESTAMP;
        if(F&(1u<<3))C.Features|=MALI_ADAPTER_CACHE_JOB;
        if(F&(1u<<4))C.Features|=MALI_ADAPTER_RESOURCE_COMPUTE;
        if(F&(1u<<9))C.Features|=MALI_ADAPTER_RESOURCE_FRAGMENT;
        if(F&(1u<<7))C.Features|=MALI_ADAPTER_RESOURCE_TILER;
        if(F&(1u<<11))C.Features|=MALI_ADAPTER_RESOURCE_MALLOC_VERTEX;
        if(F&(1u<<12))C.Features|=MALI_ADAPTER_RESOURCE_FULLSCREEN;
    }
    C.VaBits=48;C.PaBits=40;C.PageBytes=4096;C.AllocationVersion=MALI_ALLOCATION_VERSION;
    C.CommandVersion=MALI_COMMAND_RESOURCE_VERSION;C.MaxDmaBytes=MALI_COMMAND_MAX_DMA;
    C.MaxJobs=MALI_COMMAND_MAX_JOBS;C.MaxStages=MALI_EXEC_MAX_STEPS;C.MaxAllocationBytes=MALI_ALLOCATION_MAX_BYTES;
    C.RequiredAllocationFlags=MALI_ALLOCATION_CPU_VISIBLE|MALI_ALLOCATION_SHARE_BACKING;
    RtlCopyMemory(A->pOutputData,&C,sizeof(C));return STATUS_SUCCESS;
}
NTSTATUS APIENTRY MtkMaliDdiQueryAdapterInfo(HANDLE Adapter,const DXGKARG_QUERYADAPTERINFO *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    S=AdapterQuery(D,A);Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiGetNodeMetadata(HANDLE Adapter,UINT Node,DXGKARG_GETNODEMETADATA *Out) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    if(!D->AdapterConfigured || (!CanSchedule(D) && !D->PowerPaused))S=STATUS_DEVICE_NOT_READY;
    else if(!Out)S=STATUS_INVALID_PARAMETER;
    else if(Node)S=STATUS_NOT_SUPPORTED;
    else {
        DXGKARG_GETNODEMETADATA N={0};const WCHAR Name[]=L"Teclast Mali-G57 JM";
        N.EngineType=DXGK_ENGINE_TYPE_OTHER;N.GpuMmuSupported=TRUE;
        RtlCopyMemory(N.FriendlyName,Name,sizeof(Name));RtlCopyMemory(Out,&N,sizeof(N));
    }
    Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiQueryDependentEngineGroup(HANDLE Adapter,DXGKARG_QUERYDEPENDENTENGINEGROUP *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    if(!A)S=STATUS_INVALID_PARAMETER;else if(A->NodeOrdinal || A->EngineOrdinal)S=STATUS_NOT_SUPPORTED;
    else A->DependentNodeOrdinalMask=1;Leave(D);return S;
}
NTSTATUS APIENTRY MtkMaliDdiQueryEngineStatus(HANDLE Adapter,DXGKARG_QUERYENGINESTATUS *A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    if(!A)S=STATUS_INVALID_PARAMETER;else if(A->NodeOrdinal || A->EngineOrdinal)S=STATUS_NOT_SUPPORTED;
    else {
        MALI_EXECUTION *Q=&D->Queue;DXGK_ENGINESTATUS E={0};
        /* Active progress is based on previously observed real GPU completions,
           never merely an accepted submission or a running worker. */
        if(CanSchedule(D) && Q->Core->Started && !Q->Faulted && !Q->Core->Poisoned) {
            E.Responsive=!Q->HardwareActive || (D->ProgressSeen &&
                (D->ProgressFence!=Q->LastCompletedFence ||
                 (D->ProgressContext==Q->Active.Context && D->ProgressStep!=Q->Step)));
        }
        D->ProgressSeen=1;D->ProgressFence=Q->LastCompletedFence;
        D->ProgressContext=Q->ActiveValid?Q->Active.Context:0;D->ProgressStep=Q->Step;
        A->EngineStatus=E;
    }
    Leave(D);return S;
}
static const GUID MaliFeatureGuid={0x94bb3993,0xc6c3,0x4da7,{0x89,0x49,0xa1,0x13,0x82,0x32,0xe7,0x59}};
static int AdapterGuid(const GUID *A,const GUID *B) {
    const uint8_t *P=(const uint8_t*)A,*Q=(const uint8_t*)B;
    for(UINT I=0;I<sizeof(*A);I++)if(P[I]!=Q[I])return 0;return 1;
}
static int AdapterInterfaceRetain(MALI_DDI_ADAPTER *D,int Existing) {
    if(!Valid(D) || KeGetCurrentIrql()>DISPATCH_LEVEL)return 0;
    LONG64 Old=InterlockedCompareExchange64(&D->InterfaceReferences,0,0);
    while(Old>=Existing && Old<INT64_MAX) {
        LONG64 Seen=InterlockedCompareExchange64(&D->InterfaceReferences,Old+1,Old);
        if(Seen==Old)return 1;Old=Seen;
    }
    return 0;
}
static VOID AdapterInterfaceReference(PVOID X) { (void)AdapterInterfaceRetain(X,1); }
static VOID AdapterInterfaceDereference(PVOID X) {
    MALI_DDI_ADAPTER *D=X;if(!Valid(D) || KeGetCurrentIrql()>DISPATCH_LEVEL)return;
    LONG64 Old=InterlockedCompareExchange64(&D->InterfaceReferences,0,0);
    while(Old>0) {
        LONG64 Seen=InterlockedCompareExchange64(&D->InterfaceReferences,Old-1,Old);if(Seen==Old)return;Old=Seen;
    }
}
NTSTATUS MtkMaliDdiQueryInterface(PVOID Adapter,PQUERY_INTERFACE A) {
    MALI_DDI_ADAPTER *D=Adapter;NTSTATUS S=Enter(D);if(S!=STATUS_SUCCESS)return S;
    if(!A || !AdapterBuffer(A->InterfaceType,sizeof(GUID)) ||
       !AdapterBuffer(A->Interface,sizeof(DXGKDDI_FEATURE_INTERFACE)))S=STATUS_INVALID_PARAMETER;
    else if(!AdapterGuid(A->InterfaceType,&MaliFeatureGuid) || A->Version!=DXGK_FEATURE_INTERFACE_VERSION_1 ||
            A->InterfaceSpecificData || A->DeviceUid!=DISPLAY_ADAPTER_HW_ID)S=STATUS_NOT_SUPPORTED;
    else if(A->Size<sizeof(DXGKDDI_FEATURE_INTERFACE))S=STATUS_BUFFER_TOO_SMALL;
    else if(!CanSchedule(D))S=STATUS_DEVICE_NOT_READY;
    else if(!AdapterInterfaceRetain(D,0))S=STATUS_INSUFFICIENT_RESOURCES;
    else {
        DXGKDDI_FEATURE_INTERFACE I={0};I.Size=sizeof(I);I.Version=DXGK_FEATURE_INTERFACE_VERSION_1;
        I.Context=D;I.InterfaceReference=AdapterInterfaceReference;I.InterfaceDereference=AdapterInterfaceDereference;
        I.QueryFeatureSupport=MtkMaliDdiQueryFeatureSupport;
        RtlCopyMemory(A->Interface,&I,sizeof(I));
    }
    Leave(D);return S;
}
