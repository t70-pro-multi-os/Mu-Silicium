/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Miniport.h"
#define MP_SIGNATURE 0x4d503730u
#define MP_INTERFACE 1u
#define MP_MAPPING 2u
#define MP_BOARD 4u
#define MP_CORE 8u
#define MP_POWER_INTERFACE 16u
#define MP_GPU_BASE 0x13000000u
#define MP_GPU_BYTES 0x4000u
C_ASSERT(FIELD_OFFSET(MALI_MINIPORT,Runtime)==0);
static int MpValid(MALI_MINIPORT *P) {return P && P->Signature==MP_SIGNATURE;}
static NTSTATUS MpEnter(MALI_MINIPORT *P) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !MpValid(P))return STATUS_INVALID_PARAMETER;
    return InterlockedCompareExchange(&P->PnpActive,1,0)?STATUS_DEVICE_BUSY:STATUS_SUCCESS;
}
static NTSTATUS MpStatus(MALI_RESULT R) {
    switch(R) {
    case MaliOk:return STATUS_SUCCESS;case MaliBusy:return STATUS_DEVICE_BUSY;
    case MaliTimeout:return STATUS_IO_TIMEOUT;case MaliUnsupported:return STATUS_NOT_SUPPORTED;
    case MaliInvalid:return STATUS_INVALID_PARAMETER;case MaliFault:return STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE;
    default:return STATUS_DEVICE_NOT_READY;
    }
}
static int MpReady(void *X) {
    MALI_MINIPORT *P=X;
    return KeGetCurrentIrql()==PASSIVE_LEVEL && P->PowerState==PowerDeviceD0 && (P->Stages&MP_BOARD) && P->Lease.Cookie &&
        !InterlockedCompareExchange(&P->IoFault,0,0) && P->Board.Ready &&
        P->Board.Ready(P->Board.Header.Context,P->Lease.Cookie)==STATUS_SUCCESS;
}
static uint32_t MpRead(void *X,uint32_t Offset) {
    MALI_MINIPORT *P=X;
    if(P->PowerState!=PowerDeviceD0 || !P->Gpu || (Offset&3u) || Offset>MP_GPU_BYTES-4) {InterlockedExchange(&P->IoFault,1);return UINT32_MAX;}
    return READ_REGISTER_ULONG((volatile ULONG*)((uint8_t*)P->Gpu+Offset));
}
static void MpWrite(void *X,uint32_t Offset,uint32_t Value) {
    MALI_MINIPORT *P=X;
    if(P->PowerState!=PowerDeviceD0 || !P->Gpu || (Offset&3u) || Offset>MP_GPU_BYTES-4) {InterlockedExchange(&P->IoFault,1);return;}
    P->CoreTouched=1;WRITE_REGISTER_ULONG((volatile ULONG*)((uint8_t*)P->Gpu+Offset),Value);
}
static void MpBarrier(void *X) {(void)X;KeMemoryBarrier();}
static void MpDelay(void *X,uint32_t Us) {(void)X;KeStallExecutionProcessor(Us);}
static NTSTATUS MpQueryInterface(MALI_MINIPORT *P,const GUID *Guid,PINTERFACE Interface,USHORT Bytes) {
    KEVENT Event;IO_STATUS_BLOCK Result;RtlZeroMemory(&Result,sizeof(Result));
    RtlZeroMemory(Interface,Bytes);
    KeInitializeEvent(&Event,NotificationEvent,FALSE);
    PDEVICE_OBJECT Target=IoGetAttachedDeviceReference(P->Pdo);
    if(!Target)return STATUS_NO_SUCH_DEVICE;
    PIRP Irp=IoBuildSynchronousFsdRequest(IRP_MJ_PNP,Target,0,0,0,&Event,&Result);
    if(!Irp){ObDereferenceObject(Target);return STATUS_INSUFFICIENT_RESOURCES;}
    PIO_STACK_LOCATION Stack=IoGetNextIrpStackLocation(Irp);
    Stack->MinorFunction=IRP_MN_QUERY_INTERFACE;
    Stack->Parameters.QueryInterface.InterfaceType=(LPGUID)Guid;
    Stack->Parameters.QueryInterface.Size=Bytes;
    Stack->Parameters.QueryInterface.Version=1;
    Stack->Parameters.QueryInterface.Interface=Interface;
    MALI_BOARD_QUERY Proof={0};Proof.Size=sizeof(Proof);Proof.Version=1;Proof.Pdo=P->Pdo;
    RtlCopyMemory(Proof.Interrupts,P->Interrupts,sizeof(Proof.Interrupts));
    Stack->Parameters.QueryInterface.InterfaceSpecificData=&Proof;
    Irp->IoStatus.Status=STATUS_NOT_SUPPORTED;
    NTSTATUS S=IoCallDriver(Target,Irp);
    if(S==STATUS_PENDING) {
        /* Kernel, nonalertable, no timeout: the stack event/IOSB must remain
           alive until the synchronous IRP has actually completed. */
        KeWaitForSingleObject(&Event,Executive,KernelMode,FALSE,0);S=Result.Status;
    }
    ObDereferenceObject(Target);return S!=STATUS_SUCCESS && NT_SUCCESS(S)?STATUS_UNSUCCESSFUL:S;
}
static NTSTATUS MpQueryBoard(MALI_MINIPORT *P) {
    NTSTATUS S=MpQueryInterface(P,&MtkMaliBoardInterfaceGuid,(PINTERFACE)&P->Board,sizeof(P->Board));
    if(S!=STATUS_SUCCESS)return S;
    P->Stages|=MP_INTERFACE;
    if(P->Board.Header.Size<sizeof(P->Board) || P->Board.Header.Version!=1 ||
       !P->Board.Header.InterfaceReference || !P->Board.Header.InterfaceDereference ||
       !P->Board.Acquire || !P->Board.Ready || !P->Board.Release)return STATUS_DRIVER_INTERNAL_ERROR;
    S=MpQueryInterface(P,&MtkMaliBoardPowerInterfaceGuid,(PINTERFACE)&P->Power,sizeof(P->Power));
    if(S==STATUS_NOT_SUPPORTED)return STATUS_SUCCESS;
    if(S!=STATUS_SUCCESS)return S;
    P->Stages|=MP_POWER_INTERFACE;
    if(P->Power.Header.Size!=sizeof(P->Power) || P->Power.Header.Version!=1 ||
       !P->Power.Header.InterfaceReference || !P->Power.Header.InterfaceDereference ||
       !P->Power.SetPower)return STATUS_DRIVER_INTERNAL_ERROR;
    return STATUS_SUCCESS;
}
static NTSTATUS MpResources(MALI_MINIPORT *P,const DXGK_DEVICE_INFO *Info) {
    PCM_RESOURCE_LIST List=Info->TranslatedResourceList;
    if(Info->PhysicalDeviceObject!=P->Pdo || !List || List->Count!=1)return STATUS_DEVICE_CONFIGURATION_ERROR;
    PCM_PARTIAL_RESOURCE_LIST Part=&List->List[0].PartialResourceList;
    if(Part->Count!=4 || Part->Version!=1 || Part->Revision!=1)return STATUS_DEVICE_CONFIGURATION_ERROR;
    UINT Memory=0,Irqs=0;
    for(UINT I=0;I<4;I++) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *R=&Part->PartialDescriptors[I];
        if(R->Type==CmResourceTypeMemory) {
            if(Memory++ || R->Flags!=CM_RESOURCE_MEMORY_READ_WRITE ||
               R->ShareDisposition!=CmResourceShareDeviceExclusive ||
               R->u.Memory.Start.QuadPart!=MP_GPU_BASE || R->u.Memory.Length!=MP_GPU_BYTES)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
        } else if(R->Type==CmResourceTypeInterrupt) {
            if(Irqs==3)return STATUS_DEVICE_CONFIGURATION_ERROR;
            P->Interrupts[Irqs++]=*R;
        } else return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if(Memory!=1 || Irqs!=3)return STATUS_DEVICE_CONFIGURATION_ERROR;
    ULONG Levels;KIRQL Maximum;
    return MtkMaliDdiValidateInterruptResources(P->Interrupts,3,&Levels,&Maximum);
}
static BOOLEAN MpCallbackEnter(MALI_MINIPORT *P) {
    if(!MpValid(P) || InterlockedCompareExchange(&P->Closing,0,0))return FALSE;
    InterlockedIncrement(&P->ActiveCallbacks);
    if(InterlockedCompareExchange(&P->Closing,0,0)) {InterlockedDecrement(&P->ActiveCallbacks);return FALSE;}
    return TRUE;
}
BOOLEAN MtkMaliMiniportInterrupt(PVOID X,ULONG Message) {
    MALI_MINIPORT *P=X;
    if(KeGetCurrentIrql()<=DISPATCH_LEVEL || !MpCallbackEnter(P))return FALSE;
    BOOLEAN Claimed=MtkMaliDdiInterruptRoutine(&P->Runtime.Adapter,Message);
    InterlockedDecrement(&P->ActiveCallbacks);return Claimed;
}
VOID MtkMaliMiniportDpc(PVOID X) {
    MALI_MINIPORT *P=X;
    if(KeGetCurrentIrql()!=DISPATCH_LEVEL || !MpValid(P))return;
    InterlockedIncrement(&P->ActiveCallbacks);
    if(!InterlockedCompareExchange(&P->Closing,0,0))MtkMaliDdiDpcRoutine(&P->Runtime.Adapter);
    else if(InterlockedCompareExchange(&P->Started,0,0) && P->Dxgk.DxgkCbNotifyDpc)
        /* A queued DPC still acknowledges the scheduler in D3, without MMIO
           or worker access. The port owns final callback lifetime at Remove. */
        P->Dxgk.DxgkCbNotifyDpc(P->Dxgk.DeviceHandle);
    InterlockedDecrement(&P->ActiveCallbacks);
}
static NTSTATUS MpDrainCallbacks(MALI_MINIPORT *P) {
    InterlockedExchange(&P->Closing,1);
    for(UINT I=0;InterlockedCompareExchange(&P->ActiveCallbacks,0,0);I++) {
        if(I==1000)return STATUS_IO_TIMEOUT;
        LARGE_INTEGER Delay;Delay.QuadPart=-10000;
        NTSTATUS S=KeDelayExecutionThread(KernelMode,FALSE,&Delay);if(S!=STATUS_SUCCESS)return S;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS MpSuspendSoftware(MALI_MINIPORT *P) {
    NTSTATUS S=MtkMaliRuntimeSuspend(&P->Runtime);if(S!=STATUS_SUCCESS)return S;
    P->CoreTouched=0;return MpDrainCallbacks(P);
}
static NTSTATUS MpCleanup(MALI_MINIPORT *P) {
    NTSTATUS S=STATUS_SUCCESS;
    if(P->Runtime.Stages) {S=MtkMaliRuntimeStop(&P->Runtime);if(S!=STATUS_SUCCESS)goto Done;}
    /* RuntimeStop has masked/disconnected the hardware sources first. Keep
       the AddDevice object until Remove; the port owns final entry rundown. */
    InterlockedExchange(&P->Started,0);
    S=MpDrainCallbacks(P);if(S!=STATUS_SUCCESS)goto Done;
    if(P->Stages&MP_CORE) {
        if(P->CoreTouched) {
            if(P->Core.Started && !P->Core.Poisoned)S=MpStatus(MaliJmStop(&P->Core));
            else {MALI_EVENT Aborted[MALI_JM_MAX_SLOTS];uint32_t Count;
                S=MpStatus(MaliJmReset(&P->Core,Aborted,MALI_JM_MAX_SLOTS,&Count));}
            if(S!=STATUS_SUCCESS)goto Done;
        }
        P->Stages&=~MP_CORE;P->CoreTouched=0;
    }
    if(P->Stages&MP_BOARD) {
        S=P->Board.Release(P->Board.Header.Context,P->Lease.Cookie);if(S!=STATUS_SUCCESS)goto Done;
        P->Stages&=~MP_BOARD;P->PowerState=PowerDeviceUnspecified;RtlZeroMemory(&P->Lease,sizeof(P->Lease));
    }
    if(P->Stages&MP_MAPPING) {
        S=P->Dxgk.DxgkCbUnmapMemory(P->Dxgk.DeviceHandle,P->Gpu);if(S!=STATUS_SUCCESS)goto Done;
        P->Gpu=0;P->Stages&=~MP_MAPPING;
    }
    if(P->Stages&MP_POWER_INTERFACE) {
        if(!P->Power.Header.InterfaceDereference){S=STATUS_DRIVER_INTERNAL_ERROR;goto Done;}
        P->Power.Header.InterfaceDereference(P->Power.Header.Context);
        P->Stages&=~MP_POWER_INTERFACE;RtlZeroMemory(&P->Power,sizeof(P->Power));
    }
    if(P->Stages&MP_INTERFACE) {
        if(!P->Board.Header.InterfaceDereference){S=STATUS_DRIVER_INTERNAL_ERROR;goto Done;}
        P->Board.Header.InterfaceDereference(P->Board.Header.Context);
        P->Stages&=~MP_INTERFACE;RtlZeroMemory(&P->Board,sizeof(P->Board));
    }
Done:if(S!=STATUS_SUCCESS && NT_SUCCESS(S))S=STATUS_UNSUCCESSFUL;
    P->CleanupStatus=S;return S;
}
NTSTATUS MtkMaliMiniportAddDevice(PDEVICE_OBJECT Pdo,PVOID *Context) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !Pdo || !Context)return STATUS_INVALID_PARAMETER;
    MALI_MINIPORT *P=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*P),MALI_MINIPORT_POOL_TAG);
    if(!P)return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(P,sizeof(*P));P->Signature=MP_SIGNATURE;P->Pdo=Pdo;P->Closing=1;
    *Context=P;return STATUS_SUCCESS;
}
NTSTATUS MtkMaliMiniportStartDevice(PVOID X,DXGK_START_INFO *Start,DXGKRNL_INTERFACE *K,ULONG *Views,ULONG *Children) {
    MALI_MINIPORT *P=X;NTSTATUS S=MpEnter(P);if(S!=STATUS_SUCCESS)return S;
    P->StartStatus=STATUS_INVALID_PARAMETER;
    if(!Start || !K || !Views || !Children || K->Version<DXGKDDI_INTERFACE_VERSION_WDDM3_1 ||
       K->Size<FIELD_OFFSET(DXGKRNL_INTERFACE,DxgkCbQueryFeatureSupport)+sizeof(K->DxgkCbQueryFeatureSupport) ||
       !K->DeviceHandle || !K->DxgkCbGetDeviceInformation || !K->DxgkCbMapMemory || !K->DxgkCbUnmapMemory ||
       !K->DxgkCbSynchronizeExecution || !K->DxgkCbNotifyInterrupt || !K->DxgkCbQueueDpc ||
       !K->DxgkCbNotifyDpc || !K->DxgkCbGetHandleData || !K->DxgkCbQueryFeatureSupport)goto Exit;
    if(P->Stages || P->Runtime.Stages || P->Started){P->StartStatus=STATUS_DEVICE_BUSY;goto Exit;}
    if(K!=&P->Dxgk) {
        RtlZeroMemory(&P->Dxgk,sizeof(P->Dxgk));
        SIZE_T Bytes=K->Size<sizeof(P->Dxgk)?K->Size:sizeof(P->Dxgk);RtlCopyMemory(&P->Dxgk,K,Bytes);
    }
    P->StartInfo=*Start;P->IoFault=0;P->CleanupStatus=STATUS_SUCCESS;
    DXGK_DEVICE_INFO Info;RtlZeroMemory(&Info,sizeof(Info));
    S=P->Dxgk.DxgkCbGetDeviceInformation(P->Dxgk.DeviceHandle,&Info);if(S!=STATUS_SUCCESS)goto Failed;
    S=MpResources(P,&Info);if(S!=STATUS_SUCCESS)goto Failed;
    S=MpQueryBoard(P);if(S!=STATUS_SUCCESS)goto Failed;
    PHYSICAL_ADDRESS Address;Address.QuadPart=MP_GPU_BASE;
    S=P->Dxgk.DxgkCbMapMemory(P->Dxgk.DeviceHandle,Address,MP_GPU_BYTES,FALSE,FALSE,MmNonCached,&P->Gpu);
    if(S!=STATUS_SUCCESS)goto Failed;
    if(!P->Gpu){S=STATUS_DEVICE_CONFIGURATION_ERROR;goto Failed;}P->Stages|=MP_MAPPING;
    S=P->Board.Acquire(P->Board.Header.Context,&P->Lease);
    if(P->Lease.Cookie)P->Stages|=MP_BOARD;
    if(S!=STATUS_SUCCESS)goto Failed;
    if(!P->Lease.Cookie || P->Lease.AddressModel!=MaliVmDirectSystemPhysical ||
       !P->Lease.Cache.CleanForGpu || !P->Lease.Cache.StoreBarrier || !P->Lease.TimeoutTicks ||
       !P->Lease.ApertureCommitLimit || (P->Lease.ApertureCommitLimit&4095u) ||
       P->Lease.ApertureCommitLimit>MALI_ALLOCATION_MAX_BYTES) {S=STATUS_DEVICE_CONFIGURATION_ERROR;goto Failed;}
    P->PowerState=PowerDeviceD0;
    if(!MpReady(P)){S=STATUS_DEVICE_NOT_READY;goto Failed;}
    MALI_IO Io={P,MpRead,MpWrite,MpBarrier,MpDelay,MpReady};
    S=MpStatus(MaliJmInitialize(&P->Core,&Io));if(S!=STATUS_SUCCESS)goto Failed;
    S=MpStatus(MaliJmSetManagedInterrupts(&P->Core,1));if(S!=STATUS_SUCCESS)goto Failed;
    P->Stages|=MP_CORE;S=MpStatus(MaliJmStart(&P->Core));if(S!=STATUS_SUCCESS)goto Failed;
    MALI_RUNTIME_CONFIG Config={0};Config.AddressModel=P->Lease.AddressModel;
    Config.Interrupts=P->Interrupts;Config.InterruptCount=3;Config.TimeoutTicks=P->Lease.TimeoutTicks;
    Config.ApertureCommitLimit=P->Lease.ApertureCommitLimit;
    InterlockedExchange(&P->Closing,0);
    S=MtkMaliRuntimeStart(&P->Runtime,&P->Core,&P->Dxgk,&P->Lease.Cache,&Config);
    if(S!=STATUS_SUCCESS)goto Failed;
    InterlockedExchange(&P->Started,1);P->PowerStatus=STATUS_SUCCESS;*Views=0;*Children=0;P->StartStatus=STATUS_SUCCESS;goto Exit;
Failed:P->StartStatus=S;(void)MpCleanup(P);
Exit:if(P->StartStatus!=STATUS_SUCCESS && NT_SUCCESS(P->StartStatus))P->StartStatus=STATUS_UNSUCCESSFUL;
    InterlockedExchange(&P->PnpActive,0);return P->StartStatus;
}
NTSTATUS MtkMaliMiniportStopDevice(PVOID X) {
    MALI_MINIPORT *P=X;NTSTATUS S=MpEnter(P);if(S!=STATUS_SUCCESS)return S;
    S=MpCleanup(P);InterlockedExchange(&P->PnpActive,0);return S;
}
NTSTATUS MtkMaliMiniportRemoveDevice(PVOID X) {
    MALI_MINIPORT *P=X;NTSTATUS S=MpEnter(P);if(S!=STATUS_SUCCESS)return S;
    if(P->Stages || P->Runtime.Stages || P->Started || !P->Closing || P->ActiveCallbacks) {
        InterlockedExchange(&P->PnpActive,0);return STATUS_DEVICE_BUSY;
    }
    P->Signature=0;ExFreePoolWithTag(P,MALI_MINIPORT_POOL_TAG);return STATUS_SUCCESS;
}
static NTSTATUS MpChildRelations(PVOID X,DXGK_CHILD_DESCRIPTOR *Children,ULONG Bytes) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !MpValid(X) || !Children || Bytes<sizeof(*Children))return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Children,sizeof(*Children));return STATUS_SUCCESS;
}
static NTSTATUS MpChildStatus(PVOID X,DXGK_CHILD_STATUS *Status,BOOLEAN NonDestructive) {
    (void)NonDestructive;
    return KeGetCurrentIrql()==PASSIVE_LEVEL && MpValid(X) && Status?STATUS_NO_SUCH_DEVICE:STATUS_INVALID_PARAMETER;
}
static NTSTATUS MpDescriptor(PVOID X,ULONG Uid,DXGK_DEVICE_DESCRIPTOR *Descriptor) {
    (void)Uid;
    return KeGetCurrentIrql()==PASSIVE_LEVEL && MpValid(X) && Descriptor?
        STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA:STATUS_INVALID_PARAMETER;
}
static NTSTATUS MpIoRequest(PVOID X,ULONG Source,VIDEO_REQUEST_PACKET *Request) {
    (void)Source;
    return KeGetCurrentIrql()==PASSIVE_LEVEL && MpValid(X) && Request?STATUS_NOT_SUPPORTED:STATUS_INVALID_PARAMETER;
}
/* WDDM requires valid power callbacks not to fail. This offline bridge retains
   and reports an unproved/partial transition instead of claiming power success.
   A production board provider and OS hardware-failure policy must qualify that
   requirement before registration; this is not an installable power driver. */
static NTSTATUS MpPower(PVOID X,ULONG Uid,DEVICE_POWER_STATE State,POWER_ACTION Action) {
    MALI_MINIPORT *P=X;
    if(Uid!=DISPLAY_ADAPTER_HW_ID || State<=PowerDeviceUnspecified || State>=PowerDeviceMaximum)
        return STATUS_INVALID_PARAMETER;
    NTSTATUS S=MpEnter(P);if(S!=STATUS_SUCCESS)return S;
    S=STATUS_DEVICE_NOT_READY;
    if(!P->Started || !(P->Stages&MP_BOARD) || !(P->Stages&MP_CORE))goto Done;
    // This board has a single quiesced hardware state for the WDDM low-power requests.
    if(State!=PowerDeviceD0)State=PowerDeviceD3;
    if(State==PowerDeviceD0 && P->PowerState==PowerDeviceD0 && !P->Runtime.Suspended && !P->Closing) {
        S=MpReady(P)?MtkMaliRuntimeResume(&P->Runtime):STATUS_DEVICE_NOT_READY;goto Done;
    }
    if(!(P->Stages&MP_POWER_INTERFACE)){S=STATUS_NOT_SUPPORTED;goto Done;}
    if(State==PowerDeviceD3 && P->PowerState==PowerDeviceD3) {
        S=STATUS_SUCCESS;goto Done;
    }
    S=MpSuspendSoftware(P);if(S!=STATUS_SUCCESS)goto Done;
    if(State==PowerDeviceD3 || P->PowerState!=PowerDeviceD0) {
        /* A failed provider call may have changed shared rails/domains. Block
           Core MMIO until an explicit D0 succeeds and Ready proves the lease. */
        P->PowerState=PowerDeviceUnspecified;
        S=P->Power.SetPower(P->Power.Header.Context,P->Lease.Cookie,State,
            State==PowerDeviceD0?PowerActionNone:Action);
        if(S!=STATUS_SUCCESS)goto Done;
        P->PowerState=State;
    }
    if(State==PowerDeviceD0) {
        if(!MpReady(P)){S=STATUS_DEVICE_NOT_READY;goto Done;}
        InterlockedExchange(&P->Closing,0);
        S=MtkMaliRuntimeResume(&P->Runtime);
        if(S!=STATUS_SUCCESS && P->Runtime.PowerCleanupStatus==STATUS_SUCCESS &&
           !P->Core.Started && !P->Core.Poisoned)P->CoreTouched=0;
    }
Done:if(S!=STATUS_SUCCESS && NT_SUCCESS(S))S=STATUS_UNSUCCESSFUL;
    P->PowerStatus=S;InterlockedExchange(&P->PnpActive,0);return S;
}
static void MpUnload(void) { /* No global allocations; Remove owns each adapter. */ }
NTSTATUS DriverEntry(PDRIVER_OBJECT Driver,PUNICODE_STRING Path) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !Driver || !Path)return STATUS_INVALID_PARAMETER;
    DRIVER_INITIALIZATION_DATA Init={0};Init.Version=DXGKDDI_INTERFACE_VERSION_WDDM3_1;
    Init.DxgkDdiAddDevice=MtkMaliMiniportAddDevice;Init.DxgkDdiStartDevice=MtkMaliMiniportStartDevice;
    Init.DxgkDdiStopDevice=MtkMaliMiniportStopDevice;Init.DxgkDdiRemoveDevice=MtkMaliMiniportRemoveDevice;
    Init.DxgkDdiInterruptRoutine=MtkMaliMiniportInterrupt;Init.DxgkDdiDpcRoutine=MtkMaliMiniportDpc;
    Init.DxgkDdiQueryChildRelations=MpChildRelations;Init.DxgkDdiQueryChildStatus=MpChildStatus;
    Init.DxgkDdiQueryDeviceDescriptor=MpDescriptor;Init.DxgkDdiDispatchIoRequest=MpIoRequest;
    Init.DxgkDdiSetPowerState=MpPower;Init.DxgkDdiUnload=MpUnload;
    Init.DxgkDdiQueryAdapterInfo=MtkMaliDdiQueryAdapterInfo;Init.DxgkDdiQueryInterface=MtkMaliDdiQueryInterface;
    Init.DxgkDdiCreateProcess=MtkMaliDdiCreateProcess;Init.DxgkDdiDestroyProcess=MtkMaliDdiDestroyProcess;
    Init.DxgkDdiCreateDevice=MtkMaliDdiCreateDevice;Init.DxgkDdiDestroyDevice=MtkMaliDdiDestroyDevice;
    Init.DxgkDdiCreateContext=MtkMaliDdiCreateContext;Init.DxgkDdiDestroyContext=MtkMaliDdiDestroyContext;
    Init.DxgkDdiSetRootPageTable=MtkMaliDdiSetRootPageTable;Init.DxgkDdiGetRootPageTableSize=MtkMaliDdiGetRootPageTableSize;
    Init.DxgkDdiSubmitCommandVirtual=MtkMaliDdiSubmitCommandVirtual;Init.DxgkDdiPreemptCommand=MtkMaliDdiPreemptCommand;
    Init.DxgkDdiQueryCurrentFence=MtkMaliDdiQueryCurrentFence;Init.DxgkDdiResetEngine=MtkMaliDdiResetEngine;
    Init.DxgkDdiBuildPagingBuffer=MtkMaliDdiBuildPagingBuffer;
    Init.DxgkDdiCreateAllocation=MtkMaliDdiCreateAllocation;Init.DxgkDdiDestroyAllocation=MtkMaliDdiDestroyAllocation;
    Init.DxgkDdiOpenAllocation=MtkMaliDdiOpenAllocation;Init.DxgkDdiCloseAllocation=MtkMaliDdiCloseAllocation;
    Init.DxgkDdiDescribeAllocation=MtkMaliDdiDescribeAllocation;
    Init.DxgkDdiSetAllocationBackingStore=MtkMaliDdiSetAllocationBackingStore;
    Init.DxgkDdiGetNodeMetadata=MtkMaliDdiGetNodeMetadata;Init.DxgkDdiQueryDependentEngineGroup=MtkMaliDdiQueryDependentEngineGroup;
    Init.DxgkDdiQueryEngineStatus=MtkMaliDdiQueryEngineStatus;
    return DxgkInitialize(Driver,Path,&Init);
}
