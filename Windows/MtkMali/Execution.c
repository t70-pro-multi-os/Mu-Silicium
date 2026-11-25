/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Execution.h"

static MALI_EXEC_CONTEXT *Find(MALI_EXECUTION *Q, uint64_t Id) {
    uint32_t Index=(uint32_t)(Id & 255u);
    if(!Q || !Q->Initialized || !Index || Index>MALI_EXEC_CONTEXTS) return 0;
    MALI_EXEC_CONTEXT *C=&Q->Contexts[Index-1];
    return C->Live && C->Generation==(Id>>8) ? C : 0;
}
static int PhysicalPage(uint64_t Pa) {
    return Pa && Pa<MALI_JM_PA_LIMIT && !(Pa&4095u);
}
static int AnyCoreJob(const MALI_JM *D) {
    for(uint32_t I=0; I<MALI_JM_MAX_SLOTS; I++) if(D->Jobs[I].Active) return 1;
    return 0;
}
static void ReleaseActive(MALI_EXECUTION *Q) {
    if(Q->ActiveValid) {
        MALI_EXEC_CONTEXT *C=Find(Q,Q->Active.Context);
        if(C && C->References) --C->References;
    }
    Q->ActiveValid=0; Q->HardwareActive=0; Q->ActiveToken=0;
}
static void DropPending(MALI_EXECUTION *Q) {
    while(Q->Count) {
        MALI_EXEC_CONTEXT *C=Find(Q,Q->Pending[Q->First].Context);
        if(C && C->References) --C->References;
        Q->First=(Q->First+1u)%MALI_EXEC_QUEUE_DEPTH; --Q->Count;
    }
}
static void Fault(MALI_EXECUTION *Q, uint32_t Status, uint64_t Address) {
    Q->Faulted=1;
    if(Q->ActiveValid && !Q->EventPending && !Q->EventAcknowledged) {
        MALI_EXEC_EVENT E={MaliExecFaulted,Q->Active.Fence,Q->LastCompletedFence,
                          Status,Q->Active.Context,Address};
        Q->Event=E; Q->EventPending=1;
    }
}
static int SameRootElsewhere(MALI_EXECUTION *Q, MALI_EXEC_CONTEXT *Current) {
    for(uint32_t I=0; I<MALI_EXEC_CONTEXTS; I++) {
        MALI_EXEC_CONTEXT *C=&Q->Contexts[I];
        if(C!=Current && C->Live && C->Root==Current->Root) return 1;
    }
    return 0;
}
static MALI_RESULT DetachOldRoot(MALI_EXECUTION *Q, MALI_EXEC_CONTEXT *C) {
    if(C->Root && !SameRootElsewhere(Q,C) &&
       (Q->Core->BoundSpaces & (1u<<Q->AddressSpace)) &&
       Q->Core->Roots[Q->AddressSpace]==C->Root)
        return MaliJmUnbind(Q->Core,Q->AddressSpace);
    return MaliOk;
}
MALI_RESULT MaliExecInitialize(MALI_EXECUTION *Q, MALI_JM *D, uint64_t Timeout) {
    if(!Q || !D || !Timeout || Timeout>INT64_MAX || AnyCoreJob(D)) return MaliInvalid;
    /* The queue is nonpaged caller storage, larger than a kernel stack frame. */
    unsigned char *Bytes=(unsigned char*)Q;
    for(size_t I=0; I<sizeof(*Q); I++) Bytes[I]=0;
    Q->Core=D; Q->TimeoutTicks=Timeout; Q->Initialized=1;
    /* Probe/start owns discovery. The first pump chooses an implemented AS. */
    Q->AddressSpace=MALI_JM_MAX_AS;
    return MaliOk;
}
MALI_RESULT MaliExecCreateContext(MALI_EXECUTION *Q, uint64_t Owner, uint64_t *Id) {
    if(!Q || !Q->Initialized || !Owner || !Id) return MaliInvalid;
    for(uint32_t I=0; I<MALI_EXEC_CONTEXTS; I++) {
        MALI_EXEC_CONTEXT *C=&Q->Contexts[I];
        if(!C->Live && C->Generation<(UINT64_MAX>>8)) {
            uint64_t Generation=C->Generation+1;
            MALI_EXEC_CONTEXT New={Generation,Owner,0,1,0,0}; *C=New;
            *Id=(Generation<<8)|(I+1u); return MaliOk;
        }
    }
    return MaliBusy;
}
int MaliExecContextBusy(const MALI_EXECUTION *Q, uint64_t Id) {
    MALI_EXEC_CONTEXT *C=Find((MALI_EXECUTION*)Q,Id);
    return C ? C->References!=0 : -1;
}
MALI_RESULT MaliExecDestroyContext(MALI_EXECUTION *Q, uint64_t Id) {
    MALI_EXEC_CONTEXT *C=Find(Q,Id); if(!C) return MaliInvalid;
    if(C->References) return MaliBusy;
    if(Q->AddressSpace<MALI_JM_MAX_AS) {
        MALI_RESULT R=DetachOldRoot(Q,C); if(R) return R;
    }
    uint64_t Generation=C->Generation;
    MALI_EXEC_CONTEXT Empty={0}; *C=Empty; C->Generation=Generation;
    return MaliOk;
}
MALI_RESULT MaliExecSetRoot(MALI_EXECUTION *Q, uint64_t Id, uint64_t Root) {
    MALI_EXEC_CONTEXT *C=Find(Q,Id); if(!C || !PhysicalPage(Root)) return MaliInvalid;
    if(C->References) return MaliBusy;
    if(C->Root!=Root && Q->AddressSpace<MALI_JM_MAX_AS) {
        MALI_RESULT R=DetachOldRoot(Q,C); if(R) return R;
    }
    C->Root=Root; C->RootReady=1; return MaliOk;
}
MALI_RESULT MaliExecSubmitRoot(MALI_EXECUTION *Q, uint64_t Id, uint64_t Root, uint32_t Fence,
                          uint64_t Va, uint32_t Bytes, const MALI_EXEC_PACKET *Input,
                          uint32_t PrivateBytes) {
    MALI_EXEC_CONTEXT *C=Find(Q,Id);
    if(!C || !Input || !C->RootReady || !PhysicalPage(Root) || !Va || (Va&63u) || Bytes<64 ||
       Va>=MALI_JM_VA_LIMIT || Bytes>MALI_JM_VA_LIMIT-Va || PrivateBytes<16 ||
       PrivateBytes>sizeof(MALI_EXEC_PACKET)) return MaliInvalid;
    if(Q->Faulted || Q->Core->Poisoned) return MaliFault;
    if(!Q->Core->Started) return MaliOffline;
    if(Q->PreemptionPending || Q->Count==MALI_EXEC_QUEUE_DEPTH) return MaliBusy;
    /* Read only the validated byte count, then use the immutable snapshot. */
    MALI_EXEC_PACKET Packet={0};
    unsigned char *To=(unsigned char*)&Packet; const unsigned char *From=(const unsigned char*)Input;
    for(uint32_t I=0; I<PrivateBytes; I++) To[I]=From[I];
    if(Packet.Version!=MALI_EXEC_PACKET_VERSION || Packet.Flags || !Packet.StepCount ||
       Packet.StepCount>MALI_EXEC_MAX_STEPS || Packet.Bytes!=PrivateBytes ||
       PrivateBytes!=MALI_EXEC_PACKET_BYTES(Packet.StepCount)) return MaliInvalid;
    for(uint32_t I=0; I<Packet.StepCount; I++) {
        const MALI_EXEC_STEP *S=&Packet.Steps[I];
        if(S->Slot>=MALI_JM_MAX_SLOTS || !(Q->Core->Caps.JobSlots&(1u<<S->Slot)) ||
           S->Reserved || S->Chain<Va ||
           (S->Chain&63u) || S->Chain-Va>Bytes-64u) return MaliInvalid;
    }
    /* Fence values belong to the scheduler. Zero and 32-bit wrap are valid;
       reject duplicates only while a fence is still outstanding. */
    if(Q->ActiveValid && Q->Active.Fence==Fence) return MaliInvalid;
    for(uint32_t I=0; I<Q->Count; I++)
        if(Q->Pending[(Q->First+I)%MALI_EXEC_QUEUE_DEPTH].Fence==Fence) return MaliInvalid;
    MALI_EXEC_SUBMISSION S={Id,Root,Va,Fence,Bytes,Packet,Root!=C->Root};
    Q->Pending[(Q->First+Q->Count)%MALI_EXEC_QUEUE_DEPTH]=S;
    ++Q->Count; ++C->References; Q->LastSubmittedFence=Fence;
    return MaliOk;
}
MALI_RESULT MaliExecSubmit(MALI_EXECUTION *Q,uint64_t Id,uint32_t Fence,
    uint64_t Va,uint32_t Bytes,const MALI_EXEC_PACKET *Input,uint32_t PrivateBytes) {
    MALI_EXEC_CONTEXT *C=Find(Q,Id);
    return C?MaliExecSubmitRoot(Q,Id,C->Root,Fence,Va,Bytes,Input,PrivateBytes):MaliInvalid;
}
static MALI_RESULT StartStep(MALI_EXECUTION *Q) {
    const MALI_EXEC_STEP *S=&Q->Active.Packet.Steps[Q->Step];
    if(!(Q->Core->Caps.JobSlots & (1u<<S->Slot))) return MaliUnsupported;
    if(Q->Token==UINT64_MAX) return MaliUnsupported;
    uint64_t Token=Q->Token+1;
    MALI_RESULT R=MaliJmSubmit(Q->Core,S->Slot,Q->AddressSpace,S->Chain,
                              Q->Core->Caps.Shaders,Token);
    if(!R) { Q->Token=Token; Q->ActiveToken=Token; Q->HardwareActive=1; }
    return R;
}
static void PreemptEvent(MALI_EXECUTION *Q) {
    MALI_EXEC_EVENT E={MaliExecPreempted,Q->PreemptionFence,Q->LastCompletedFence,0,0,0};
    Q->Event=E; Q->EventPending=1;
}
MALI_RESULT MaliExecPump(MALI_EXECUTION *Q, uint64_t Now) {
    if(!Q || !Q->Initialized) return MaliInvalid;
    if(Now<Q->LastTime) { Fault(Q,0,0); return MaliInvalid; }
    Q->LastTime=Now;
    if(Q->Faulted) return MaliFault;
    if(Q->EventPending) return MaliBusy;
    if(Q->Core->Poisoned) { Fault(Q,0,0); return MaliFault; }
    MALI_EVENT Events[MALI_JM_MAX_EVENTS]; uint32_t Count=0;
    MALI_RESULT R=MaliOk;
    /* GPU/MMU faults can arrive without an active submission. Consume them
       before rearming interrupts or starting queued DMA; never invent a fence. */
    if(Q->Core->Started || Q->HardwareActive) {
        R=MaliJmPoll(Q->Core,Events,MALI_JM_MAX_EVENTS,&Count);
        if(R || (Count && !Q->HardwareActive)) {
            Fault(Q,Count?Events[0].Status:0,Count?Events[0].Address:0);
            return R?R:MaliFault;
        }
    }
    if(Q->HardwareActive) {
        if(!Count) {
            if(Now-Q->StartTime>=Q->TimeoutTicks) { Fault(Q,0,0); return MaliTimeout; }
            return MaliOk;
        }
        /* One hardware job is owned by this node; extra/stale events are fatal. */
        if(Count!=1 || Events[0].Type!=MaliJobDone ||
           Events[0].Fence!=Q->ActiveToken || Events[0].Slot!=Q->Active.Packet.Steps[Q->Step].Slot) {
            Fault(Q,Count?Events[0].Status:0,Count?Events[0].Address:0); return MaliFault;
        }
        Q->HardwareActive=0;
        if(++Q->Step<Q->Active.Packet.StepCount) {
            R=StartStep(Q); if(R) Fault(Q,0,0); return R;
        }
        if(Q->Active.PrivateRoot) {
            R=MaliJmUnbind(Q->Core,Q->AddressSpace);
            if(R){Fault(Q,0,0);return R;}
        }
        Q->LastCompletedFence=Q->Active.Fence;
        MALI_EXEC_EVENT E={MaliExecCompleted,Q->Active.Fence,Q->LastCompletedFence,1,Q->Active.Context,0};
        Q->Event=E; Q->EventPending=1; return MaliOk;
    }
    if(Q->PreemptionPending) { PreemptEvent(Q); return MaliOk; }
    if(!Q->Count) return MaliOk;
    if(Q->Core->Poisoned) { Fault(Q,0,0); return MaliFault; }
    if(!Q->Core->Started) return MaliOffline;
    if(AnyCoreJob(Q->Core)) { Fault(Q,0,0); return MaliBusy; }
    if(Q->AddressSpace==MALI_JM_MAX_AS) {
        for(uint32_t A=0; A<MALI_JM_MAX_AS; A++)
            if(Q->Core->Caps.AddressSpaces & (1u<<A)) { Q->AddressSpace=A; break; }
        if(Q->AddressSpace==MALI_JM_MAX_AS) return MaliUnsupported;
    }
    Q->Active=Q->Pending[Q->First]; Q->First=(Q->First+1u)%MALI_EXEC_QUEUE_DEPTH;
    --Q->Count; Q->ActiveValid=1; Q->Step=0; Q->StartTime=Now; Q->EventAcknowledged=0;
    R=MaliJmBind(Q->Core,Q->AddressSpace,Q->Active.Root);
    if(!R) R=StartStep(Q);
    if(R) Fault(Q,0,0);
    return R;
}
MALI_RESULT MaliExecRequestPreempt(MALI_EXECUTION *Q, uint32_t Fence) {
    if(!Q || !Q->Initialized) return MaliInvalid;
    if(Q->Faulted) return MaliFault;
    if(Q->PreemptionPending) return MaliBusy;
    Q->PreemptionPending=1; Q->PreemptionFence=Fence;
    /* No MMIO or waiting here: the WDDM preempt DDI can run at DISPATCH. */
    return MaliOk;
}
MALI_RESULT MaliExecPeek(MALI_EXECUTION *Q, MALI_EXEC_EVENT *Event) {
    if(!Q || !Q->Initialized || !Event) return MaliInvalid;
    if(!Q->EventPending) return MaliOffline;
    *Event=Q->Event; return MaliOk;
}
MALI_RESULT MaliExecAcknowledge(MALI_EXECUTION *Q, const MALI_EXEC_EVENT *E) {
    if(!Q || !Q->Initialized || !E || !Q->EventPending || E->Type!=Q->Event.Type ||
       E->Fence!=Q->Event.Fence || E->LastCompletedFence!=Q->Event.LastCompletedFence ||
       E->Context!=Q->Event.Context || E->HardwareStatus!=Q->Event.HardwareStatus ||
       E->FaultAddress!=Q->Event.FaultAddress) return MaliInvalid;
    Q->EventPending=0;
    if(E->Type==MaliExecFaulted) {
        /* Scheduler notification is not proof that DMA stopped. */
        Q->EventAcknowledged=1; return MaliOk;
    }
    if(E->Type==MaliExecPreempted) {
        DropPending(Q); Q->PreemptionPending=0; Q->PreemptionFence=0; return MaliOk;
    }
    ReleaseActive(Q); Q->EventAcknowledged=0; return MaliOk;
}
MALI_RESULT MaliExecReset(MALI_EXECUTION *Q, uint32_t *LastAborted) {
    if(!Q || !Q->Initialized || !LastAborted) return MaliInvalid;
    if(Q->ResetEpoch==UINT64_MAX) return MaliUnsupported;
    MALI_EVENT Aborted[MALI_JM_MAX_SLOTS]; uint32_t Count=0;
    MALI_RESULT R=MaliJmReset(Q->Core,Aborted,MALI_JM_MAX_SLOTS,&Count);
    if(R) { Q->Faulted=1; return R; }
    /* RESET_DONE is observed. Every owned job's DMA has stopped. The caller
       reports LastAborted via the real reset DDI; do not fabricate completions. */
    ++Q->ResetEpoch;
    *LastAborted=Q->ActiveValid &&
                 (Q->HardwareActive || Q->Step) ? Q->Active.Fence : Q->LastCompletedFence;
    ReleaseActive(Q); DropPending(Q);
    Q->EventPending=0; Q->EventAcknowledged=0; Q->PreemptionPending=0; Q->PreemptionFence=0;
    Q->AddressSpace=MALI_JM_MAX_AS; Q->Faulted=0;
    R=MaliJmStart(Q->Core); if(R) Q->Faulted=1;
    else Q->LastCompletedFence=*LastAborted;
    return R;
}
MALI_RESULT MaliExecFlush(MALI_EXECUTION *Q, uint64_t Root, uint64_t Start, uint64_t Size) {
    uint64_t Encoded;
    if(!Q || !Q->Initialized || !PhysicalPage(Root)) return MaliInvalid;
    MALI_RESULT R=MaliJmLockAddress(Start,Size,&Encoded); if(R) return R;
    if(Q->Faulted) return MaliFault;
    if(Q->ActiveValid && Q->Active.Root==Root) return MaliBusy;
    if(Q->AddressSpace<MALI_JM_MAX_AS &&
       (Q->Core->BoundSpaces & (1u<<Q->AddressSpace)) && Q->Core->Roots[Q->AddressSpace]==Root)
        return MaliJmFlush(Q->Core,Q->AddressSpace,Start,Size);
    /* An unbound root has no TLB state; every future bind flushes/reloads. */
    return MaliOk;
}
