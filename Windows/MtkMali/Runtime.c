/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Runtime.h"
#define RUNTIME_PREPARE 1u
#define RUNTIME_DDI 2u
#define RUNTIME_VM 4u
#define RUNTIME_RESIDENCY 8u
#define RUNTIME_WORKER 16u
#define RUNTIME_INTERRUPTS 32u
C_ASSERT(FIELD_OFFSET(MALI_RUNTIME,Adapter)==0);
static void RuntimeWake(void *X) {MtkMaliWorkerWake(X);}
static NTSTATUS RuntimeGate(MALI_RUNTIME *R) {
    return KeWaitForSingleObject(&R->Adapter.Gate,Executive,KernelMode,FALSE,0);
}
static void RuntimeUngate(MALI_RUNTIME *R) {KeReleaseMutex(&R->Adapter.Gate,FALSE);}
NTSTATUS MtkMaliRuntimeStop(MALI_RUNTIME *R) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !R)return STATUS_INVALID_PARAMETER;
    NTSTATUS S=STATUS_SUCCESS;
    /* Quiesce before destroying the backend: shutdown alone detaches the VM
       that Residency still needs to validate its final release. */
    if(R->Stages&RUNTIME_DDI) {
        S=MtkMaliDdiQuiesce(&R->Adapter);if(S!=STATUS_SUCCESS)goto Done;
    }
    if(R->Stages&RUNTIME_INTERRUPTS) {
        S=MtkMaliDdiDisconnectInterrupts(&R->Adapter);if(S!=STATUS_SUCCESS)goto Done;
        R->Stages&=~RUNTIME_INTERRUPTS;
    }
    if(R->Stages&RUNTIME_WORKER) {
        S=MtkMaliWorkerStop(&R->Worker);if(S!=STATUS_SUCCESS)goto Done;
        R->Stages&=~RUNTIME_WORKER;
    }
    if(R->Stages&(RUNTIME_RESIDENCY|RUNTIME_VM)) {
        S=RuntimeGate(R);if(S!=STATUS_SUCCESS)goto Done;
        if(R->Stages&RUNTIME_RESIDENCY) {
            S=MtkMaliResidencyShutdown(&R->Residency);
            if(S==STATUS_SUCCESS)R->Stages&=~RUNTIME_RESIDENCY;
        }
        if(S==STATUS_SUCCESS && (R->Stages&RUNTIME_VM)) {
            S=MtkMaliVmShutdown(&R->Vm);if(S==STATUS_SUCCESS)R->Stages&=~RUNTIME_VM;
        }
        RuntimeUngate(R);if(S!=STATUS_SUCCESS)goto Done;
    }
    if(R->Stages&RUNTIME_DDI) {
        S=MtkMaliDdiShutdown(&R->Adapter);if(S!=STATUS_SUCCESS)goto Done;
        R->Stages&=~RUNTIME_DDI;
    }
    if(R->Stages&RUNTIME_PREPARE) {
        S=MtkMaliPrepareShutdown(&R->Prepare);if(S!=STATUS_SUCCESS)goto Done;
        R->Stages&=~RUNTIME_PREPARE;
    }
Done:
    if(!R->Stages)R->Suspended=0;
    R->CleanupStatus=S;return S;
}
NTSTATUS MtkMaliRuntimeStart(MALI_RUNTIME *R,MALI_JM *Core,const DXGKRNL_INTERFACE *K,
    const MALI_WDDM_CACHE_OPS *Cache,const MALI_RUNTIME_CONFIG *Config) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !R || !Core || !K || !Cache || !Config)return STATUS_INVALID_PARAMETER;
    if(R->Stages)return STATUS_DEVICE_BUSY;
    R->StartStatus=STATUS_INVALID_PARAMETER;R->CleanupStatus=STATUS_SUCCESS;
    if(!Config->TimeoutTicks || !Config->ApertureCommitLimit || (Config->ApertureCommitLimit&4095u) ||
       Config->ApertureCommitLimit>MALI_ALLOCATION_MAX_BYTES)return R->StartStatus;
    if(Config->AddressModel!=MaliVmDirectSystemPhysical) {
        R->StartStatus=STATUS_NOT_SUPPORTED;return R->StartStatus;
    }
    MALI_PREPARE_OPS Ops;MALI_DDI_HOOKS Hooks;
    MtkMaliResidencyGetOps(&R->Residency,&Ops);
    NTSTATUS S=MtkMaliPrepareInitialize(&R->Prepare,Core,&Ops,&Hooks);
    if(S!=STATUS_SUCCESS)goto Failed;R->Stages|=RUNTIME_PREPARE;
    MALI_RESULT Result=MtkMaliDdiInitialize(&R->Adapter,Core,Config->TimeoutTicks,K,Config->InterruptMessage,&Hooks,Cache);
    if(Result!=MaliOk){S=STATUS_INVALID_PARAMETER;goto Failed;}R->Stages|=RUNTIME_DDI;
    if(Core->ManagedInterrupts) {
        S=MtkMaliDdiConfigureInterruptDomain(&R->Adapter,Config->Interrupts,Config->InterruptCount);
        if(S!=STATUS_SUCCESS)goto Failed;
    }
    S=RuntimeGate(R);if(S!=STATUS_SUCCESS)goto Failed;
    S=MtkMaliVmInitialize(&R->Vm,&R->Adapter.Allocations,Config->AddressModel);
    if(S==STATUS_SUCCESS)R->Stages|=RUNTIME_VM;
    RuntimeUngate(R);if(S!=STATUS_SUCCESS)goto Failed;
    S=MtkMaliDdiAttachVm(&R->Adapter,&R->Vm);if(S!=STATUS_SUCCESS)goto Failed;
    S=RuntimeGate(R);if(S!=STATUS_SUCCESS)goto Failed;
    S=MtkMaliResidencyInitialize(&R->Residency,&R->Adapter,&R->Vm,&R->Worker,RuntimeWake);
    if(S==STATUS_SUCCESS)R->Stages|=RUNTIME_RESIDENCY;
    RuntimeUngate(R);if(S!=STATUS_SUCCESS)goto Failed;
    S=MtkMaliDdiConfigureAdapter(&R->Adapter,Config->ApertureCommitLimit);if(S!=STATUS_SUCCESS)goto Failed;
    S=MtkMaliWorkerStart(&R->Worker,&R->Adapter);if(S!=STATUS_SUCCESS)goto Failed;
    R->Stages|=RUNTIME_WORKER;
    if(Core->ManagedInterrupts) {
        /* Mark before the call: a failed connect can still own live IRQ state. */
        R->Stages|=RUNTIME_INTERRUPTS;S=MtkMaliDdiConnectInterrupts(&R->Adapter);if(S!=STATUS_SUCCESS)goto Failed;
    }
    R->StartStatus=STATUS_SUCCESS;return STATUS_SUCCESS;
Failed:
    R->StartStatus=S;(void)MtkMaliRuntimeStop(R);return S;
}

static NTSTATUS RuntimePowerCore(MALI_RUNTIME *R,BOOLEAN On) {
    NTSTATUS S=RuntimeGate(R);if(S!=STATUS_SUCCESS)return S;
    MALI_DDI_ADAPTER *D=&R->Adapter;MALI_JM *C=D->Queue.Core;
    MALI_RESULT Result=MaliOk;
    if(!D->PowerPaused)S=STATUS_DEVICE_NOT_READY;
    else {
        if(On)Result=MaliJmStart(C);
        else if(C->Poisoned && !D->Queue.Faulted) {
            /* No submissions can enter. A failed earlier start/stop still
               requires RESET_DONE before the parent can remove power. */
            MALI_EVENT Aborted[MALI_JM_MAX_SLOTS];uint32_t Count=0;
            Result=MaliJmReset(C,Aborted,MALI_JM_MAX_SLOTS,&Count);
            if(!Result && Count)Result=MaliFault;
        } else if(C->Started) {
            Result=MaliExecPump(&D->Queue,D->Queue.LastTime);
            if(!Result)Result=MaliJmStop(C);
        }
        switch(Result) {
        case MaliOk:S=STATUS_SUCCESS;break;case MaliInvalid:S=STATUS_INVALID_PARAMETER;break;
        case MaliBusy:S=STATUS_DEVICE_BUSY;break;case MaliTimeout:S=STATUS_IO_TIMEOUT;break;
        case MaliUnsupported:S=STATUS_NOT_SUPPORTED;break;
        case MaliFault:S=STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE;break;
        default:S=STATUS_DEVICE_NOT_READY;break;
        }
        if(S==STATUS_SUCCESS && On) {
            const MALI_CAPS *A=&D->PowerCaps,*B=&C->Caps;
            if(A->GpuId!=B->GpuId || A->MmuFeatures!=B->MmuFeatures || A->AddressSpaces!=B->AddressSpaces ||
               A->JobSlots!=B->JobSlots || A->Shaders!=B->Shaders || A->Tilers!=B->Tilers || A->L2!=B->L2)
                S=STATUS_DEVICE_CONFIGURATION_ERROR;
            for(UINT I=0;I<MALI_JM_MAX_SLOTS;I++)if(A->JobFeatures[I]!=B->JobFeatures[I])S=STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    RuntimeUngate(R);return S;
}
NTSTATUS MtkMaliRuntimeSuspend(MALI_RUNTIME *R) {
    const uint32_t Required=RUNTIME_PREPARE|RUNTIME_DDI|RUNTIME_VM|RUNTIME_RESIDENCY;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !R)return STATUS_INVALID_PARAMETER;
    NTSTATUS S=STATUS_DEVICE_NOT_READY;
    if((R->Stages&Required)!=Required)goto Done;
    S=MtkMaliDdiPowerPause(&R->Adapter);R->Suspended=R->Adapter.PowerPaused;
    if(S!=STATUS_SUCCESS)goto Done;
    if(R->Stages&RUNTIME_INTERRUPTS) {
        S=MtkMaliDdiDisconnectInterrupts(&R->Adapter);if(S!=STATUS_SUCCESS)goto Done;
        R->Stages&=~RUNTIME_INTERRUPTS;
    }
    if(R->Stages&RUNTIME_WORKER) {
        S=MtkMaliWorkerStop(&R->Worker);if(S!=STATUS_SUCCESS)goto Done;
        R->Stages&=~RUNTIME_WORKER;
    }
    S=RuntimePowerCore(R,FALSE);
Done:R->PowerStatus=S;return S;
}
NTSTATUS MtkMaliRuntimeResume(MALI_RUNTIME *R) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !R)return STATUS_INVALID_PARAMETER;
    if(!R->Suspended) {
        const uint32_t Ready=RUNTIME_PREPARE|RUNTIME_DDI|RUNTIME_VM|RUNTIME_RESIDENCY|RUNTIME_WORKER;
        NTSTATUS S=STATUS_DEVICE_NOT_READY;
        if((R->Stages&Ready)==Ready && R->Worker.Thread &&
           (!R->Adapter.Queue.Core->ManagedInterrupts || (R->Stages&RUNTIME_INTERRUPTS)))
            S=MtkMaliDdiCheckRunning(&R->Adapter);
        R->PowerStatus=S;return S;
    }
    /* Finish retained partial software transitions before a fresh restart. */
    NTSTATUS S=MtkMaliRuntimeSuspend(R);if(S!=STATUS_SUCCESS)return S;
    R->PowerCleanupStatus=STATUS_SUCCESS;
    S=RuntimePowerCore(R,TRUE);if(S!=STATUS_SUCCESS)goto Failed;
    S=MtkMaliWorkerStart(&R->Worker,&R->Adapter);if(S!=STATUS_SUCCESS)goto Failed;
    R->Stages|=RUNTIME_WORKER;
    if(R->Adapter.Queue.Core->ManagedInterrupts) {
        R->Stages|=RUNTIME_INTERRUPTS;
        S=MtkMaliDdiConnectInterrupts(&R->Adapter);if(S!=STATUS_SUCCESS)goto Failed;
    }
    S=MtkMaliDdiPowerResume(&R->Adapter);if(S!=STATUS_SUCCESS)goto Failed;
    R->Suspended=0;R->PowerStatus=STATUS_SUCCESS;return STATUS_SUCCESS;
Failed:R->PowerCleanupStatus=MtkMaliRuntimeSuspend(R);R->PowerStatus=S;return S;
}
