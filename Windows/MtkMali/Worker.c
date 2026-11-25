/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Worker.h"
static void Run(PVOID Context) {
    MALI_DDI_WORKER *W=Context;
    NTSTATUS S=STATUS_SUCCESS;
    for(;;) {
        if(InterlockedCompareExchange(&W->StopRequested,0,0))break;
        BOOLEAN Pending=FALSE;
        S=MtkMaliDdiHasWork(W->Adapter,&Pending);
        if(!NT_SUCCESS(S))break;
        /* Poll every 50 ms only with outstanding work or an undelivered event.
           Idle has no periodic wakeups. Real IRQ DPCs can wake this sooner. */
        LARGE_INTEGER Delay;Delay.QuadPart=-500000;
        S=KeWaitForSingleObject(&W->Signal,Executive,KernelMode,FALSE,Pending?&Delay:0);
        if(S!=STATUS_SUCCESS && S!=STATUS_TIMEOUT)break;
        if(InterlockedCompareExchange(&W->StopRequested,0,0))break;
        LARGE_INTEGER Now=KeQueryPerformanceCounter(0);
        if(Now.QuadPart<0){S=STATUS_INVALID_PARAMETER;break;}
        S=MtkMaliDdiPump(W->Adapter,(uint64_t)Now.QuadPart);
        InterlockedExchange(&W->LastStatus,S);
        /* Hardware faults park in HasWork after their real notification;
           a failed notification remains pending and is retried. */
    }
    if(!NT_SUCCESS(S))InterlockedExchange(&W->LastStatus,S);
    PsTerminateSystemThread(STATUS_SUCCESS);
}
NTSTATUS MtkMaliWorkerStart(MALI_DDI_WORKER *W,MALI_DDI_ADAPTER *D) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !W || !D)return STATUS_INVALID_PARAMETER;
    if(W->Thread || InterlockedCompareExchange(&W->Initialized,0,0))return STATUS_DEVICE_BUSY;
    BOOLEAN Pending=FALSE;
    NTSTATUS S=MtkMaliDdiHasWork(D,&Pending);if(!NT_SUCCESS(S))return S;
    LARGE_INTEGER Frequency;
    KeQueryPerformanceCounter(&Frequency);
    if(Frequency.QuadPart<=0)return STATUS_INVALID_PARAMETER;
    W->Adapter=D;W->Frequency=Frequency;W->LastStatus=STATUS_SUCCESS;W->StopRequested=0;
    KeInitializeEvent(&W->Signal,SynchronizationEvent,FALSE);
    ExInitializeRundownProtection(&W->Calls);
    OBJECT_ATTRIBUTES Attributes;
    InitializeObjectAttributes(&Attributes,0,OBJ_KERNEL_HANDLE,0,0);
    InterlockedExchange(&W->Initialized,1);
    S=PsCreateSystemThread(&W->Thread,SYNCHRONIZE,&Attributes,0,0,Run,W);
    if(!NT_SUCCESS(S)) {
        InterlockedExchange(&W->Initialized,0);
        ExWaitForRundownProtectionRelease(&W->Calls);W->Thread=0;
    }
    return S;
}
void MtkMaliWorkerWake(MALI_DDI_WORKER *W) {
    if(!W || KeGetCurrentIrql()>DISPATCH_LEVEL || !InterlockedCompareExchange(&W->Initialized,0,0))return;
    if(!ExAcquireRundownProtection(&W->Calls))return;
    if(!InterlockedCompareExchange(&W->StopRequested,0,0))KeSetEvent(&W->Signal,IO_NO_INCREMENT,FALSE);
    ExReleaseRundownProtection(&W->Calls);
}
NTSTATUS MtkMaliWorkerStop(MALI_DDI_WORKER *W) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !W)return STATUS_INVALID_PARAMETER;
    if(!W->Thread)return InterlockedCompareExchange(&W->Initialized,0,0)?STATUS_DEVICE_BUSY:STATUS_SUCCESS;
    InterlockedExchange(&W->StopRequested,1);
    KeSetEvent(&W->Signal,IO_NO_INCREMENT,FALSE);
    NTSTATUS S=ZwWaitForSingleObject(W->Thread,FALSE,0);
    if(S!=STATUS_SUCCESS)return NT_SUCCESS(S)?STATUS_UNSUCCESSFUL:S;
    ExWaitForRundownProtectionRelease(&W->Calls);
    S=ZwClose(W->Thread);
    if(NT_SUCCESS(S)){W->Thread=0;InterlockedExchange(&W->Initialized,0);}
    return S;
}
