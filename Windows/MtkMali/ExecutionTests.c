/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Execution.h"
static unsigned FirstFailure;
unsigned MtkMaliExecutionAssertionCount;
#define CHECK(X) do { ++MtkMaliExecutionAssertionCount; if(!(X) && !FirstFailure) FirstFailure=__LINE__; } while(0)
#include "TestGpu.h"
static GPU G;
static MALI_JM D;
static MALI_EXECUTION Q;
static void Begin(void) {
    Init(&G,&D); CHECK(MaliJmStart(&D)==MaliOk);
    CHECK(MaliExecInitialize(&Q,&D,100)==MaliOk);
}
static uint64_t Context(uint64_t Owner,uint64_t Root) {
    uint64_t C=0; CHECK(MaliExecCreateContext(&Q,Owner,&C)==MaliOk);
    CHECK(MaliExecSetRoot(&Q,C,Root)==MaliOk); return C;
}
static MALI_EXEC_PACKET Packet(unsigned Steps) {
    MALI_EXEC_PACKET P={0}; P.Version=1; P.Bytes=MALI_EXEC_PACKET_BYTES(Steps); P.StepCount=Steps;
    for(unsigned I=0; I<Steps; I++) {P.Steps[I].Chain=0x80000000+I*64;P.Steps[I].Slot=I%3;}
    return P;
}
static MALI_RESULT Submit(uint64_t C,unsigned Fence,unsigned Steps) {
    MALI_EXEC_PACKET P=Packet(Steps);
    return MaliExecSubmit(&Q,C,Fence,0x80000000,4096,&P,P.Bytes);
}
static void Ack(MALI_EXEC_EVENT_TYPE Type,unsigned Fence) {
    MALI_EXEC_EVENT E={0}; CHECK(MaliExecPeek(&Q,&E)==MaliOk);
    CHECK(E.Type==Type && E.Fence==Fence); CHECK(MaliExecAcknowledge(&Q,&E)==MaliOk);
}
static void LifetimeAndOrder(void) {
    Begin(); uint64_t A=Context(1,0x40001000), B=Context(2,0x40002000);
    CHECK(Submit(A,UINT32_MAX,3)==MaliOk); CHECK(Submit(B,0,1)==MaliOk);
    CHECK(Submit(A,UINT32_MAX,1)==MaliInvalid);
    CHECK(MaliExecContextBusy(&Q,A)==1 && MaliExecContextBusy(&Q,B)==1);
    CHECK(MaliExecDestroyContext(&Q,A)==MaliBusy);
    CHECK(MaliExecSetRoot(&Q,A,0x40003000)==MaliBusy);
    CHECK(MaliExecPump(&Q,1)==MaliOk && D.Jobs[0].Active && Q.Count==1);
    CHECK(D.Roots[0]==0x40001000 && Q.Active.Fence==UINT32_MAX);
    for(unsigned S=0; S<3; S++) {
        CHECK(D.Jobs[S].Active && D.Jobs[S].Fence==S+1);
        Completion(&G,S,1,0); CHECK(MaliExecPump(&Q,2+S)==MaliOk);
        CHECK(!D.Jobs[S].Active);
        if(S<2) {CHECK(!Q.EventPending);CHECK(Q.LastCompletedFence==0);}
    }
    CHECK(Q.EventPending && Q.LastCompletedFence==UINT32_MAX);
    unsigned W=G.Writes; CHECK(MaliExecPump(&Q,5)==MaliBusy && G.Writes==W);
    MALI_EXEC_EVENT E; CHECK(MaliExecPeek(&Q,&E)==MaliOk);
    E.Context=B; CHECK(MaliExecAcknowledge(&Q,&E)==MaliInvalid);
    CHECK(MaliExecContextBusy(&Q,A)==1); Ack(MaliExecCompleted,UINT32_MAX);
    CHECK(MaliExecContextBusy(&Q,A)==0 && Q.Count==1);
    CHECK(MaliExecPump(&Q,6)==MaliOk && D.Roots[0]==0x40002000);
    Completion(&G,0,1,0); CHECK(MaliExecPump(&Q,7)==MaliOk); Ack(MaliExecCompleted,0);
    CHECK(Q.LastCompletedFence==0 && !Q.ActiveValid && !Q.Count);
    CHECK(MaliExecDestroyContext(&Q,A)==MaliOk); CHECK(MaliExecDestroyContext(&Q,B)==MaliOk);
    CHECK(!D.BoundSpaces);
    CHECK(MaliExecContextBusy(&Q,A)==-1 && MaliExecSetRoot(&Q,A,0x40001000)==MaliInvalid);
    uint64_t New=Context(3,0x40001000); CHECK(New!=A && MaliExecDestroyContext(&Q,A)==MaliInvalid);
    CHECK(MaliExecDestroyContext(&Q,New)==MaliOk);
}
static void Validation(void) {
    Begin(); uint64_t C=0;
    CHECK(MaliExecCreateContext(&Q,0,&C)==MaliInvalid);
    CHECK(MaliExecCreateContext(&Q,1,&C)==MaliOk);
    MALI_EXEC_PACKET P=Packet(1); unsigned W=G.Writes;
    CHECK(MaliExecSubmit(&Q,C,1,0x80000000,4096,&P,P.Bytes)==MaliInvalid);
    CHECK(MaliExecSetRoot(&Q,C,0)==MaliInvalid);
    CHECK(MaliExecSetRoot(&Q,C,0x40001001)==MaliInvalid);
    CHECK(MaliExecSetRoot(&Q,C,MALI_JM_PA_LIMIT)==MaliInvalid);
    CHECK(MaliExecSetRoot(&Q,C,0x40001000)==MaliOk);
    for(unsigned Kind=0; Kind<13; Kind++) {
        P=Packet(1); uint64_t Va=0x80000000; unsigned Bytes=4096, N=P.Bytes;
        switch(Kind) {
            case 0:P.Version++;break; case 1:P.Flags=1;break; case 2:P.StepCount=0;break;
            case 3:P.StepCount=17;break; case 4:P.Bytes++;break; case 5:N--;break;
            case 6:P.Steps[0].Reserved=1;break; case 7:P.Steps[0].Slot=3;break;
            case 8:P.Steps[0].Chain++;break; case 9:P.Steps[0].Chain=Va-64;break;
            case 10:P.Steps[0].Chain=Va+4096;break; case 11:Va++;break; case 12:Bytes=63;break;
        }
        CHECK(MaliExecSubmit(&Q,C,Kind,Va,Bytes,&P,N)==MaliInvalid);
        CHECK(!Q.Count && !Q.Contexts[0].References && G.Writes==W);
    }
    P=Packet(1); P.Steps[0].Chain=MALI_JM_VA_LIMIT-64;
    CHECK(MaliExecSubmit(&Q,C,1,MALI_JM_VA_LIMIT-64,65,&P,P.Bytes)==MaliInvalid);
    D.Caps.JobSlots=1; P=Packet(2);
    CHECK(MaliExecSubmit(&Q,C,1,0x80000000,4096,&P,P.Bytes)==MaliInvalid);
    P=Packet(1);D.Started=0;
    CHECK(MaliExecSubmit(&Q,C,1,0x80000000,4096,&P,P.Bytes)==MaliOffline);
    D.Started=1;
    CHECK(MaliExecSubmit(&Q,C,1,0x80000000,4096,&P,15)==MaliInvalid);
    CHECK(MaliExecSubmit(&Q,C,1,0x80000000,4096,&P,sizeof(P)+1)==MaliInvalid);
    CHECK(MaliExecSubmit(&Q,C,1,0x80000000,4096,0,32)==MaliInvalid);
    CHECK(MaliExecInitialize(0,&D,1)==MaliInvalid);
    CHECK(MaliExecInitialize(&Q,&D,0)==MaliInvalid);
    CHECK(!Q.Count && G.Writes==W);
}
static void LimitsAndRoots(void) {
    Begin(); uint64_t C[64];
    for(unsigned I=0;I<64;I++) CHECK(MaliExecCreateContext(&Q,I+1,&C[I])==MaliOk);
    uint64_t Extra=0;CHECK(MaliExecCreateContext(&Q,1,&Extra)==MaliBusy && !Extra);
    for(unsigned I=0;I<64;I++) CHECK(MaliExecDestroyContext(&Q,C[I])==MaliOk);
    uint64_t A=Context(1,0x40001000),B=Context(2,0x40001000);
    CHECK(Submit(A,1,1)==MaliOk); CHECK(MaliExecPump(&Q,1)==MaliOk);
    CHECK(MaliExecFlush(&Q,0x40001000,0,4096)==MaliBusy);
    Completion(&G,0,1,0);CHECK(MaliExecPump(&Q,2)==MaliOk);Ack(MaliExecCompleted,1);
    CHECK(MaliExecFlush(&Q,0x40001000,0,4096)==MaliOk);
    CHECK(MaliExecDestroyContext(&Q,A)==MaliOk && D.BoundSpaces==1);
    CHECK(MaliExecSetRoot(&Q,B,0x40002000)==MaliOk && !D.BoundSpaces);
    unsigned W=G.Writes;
    CHECK(MaliExecFlush(&Q,0x40001000,0,MALI_JM_VA_LIMIT)==MaliOk && G.Writes==W);
    CHECK(MaliExecFlush(&Q,0x40001000,0,0)==MaliInvalid);
    for(unsigned I=0;I<32;I++)CHECK(Submit(B,I,1)==MaliOk);
    CHECK(Submit(B,33,1)==MaliBusy && Q.Count==32);
    CHECK(MaliExecDestroyContext(&Q,B)==MaliBusy);
    unsigned Aborted=99; CHECK(MaliExecReset(&Q,&Aborted)==MaliOk && Aborted==1);
    CHECK(MaliExecDestroyContext(&Q,B)==MaliOk);
}
static void Preemption(void) {
    Begin(); uint64_t A=Context(1,0x40001000), B=Context(2,0x40002000);
    CHECK(Submit(A,10,2)==MaliOk && Submit(B,11,1)==MaliOk);
    CHECK(MaliExecPump(&Q,1)==MaliOk); unsigned W=G.Writes;
    CHECK(MaliExecRequestPreempt(&Q,100)==MaliOk && G.Writes==W);
    CHECK(MaliExecRequestPreempt(&Q,101)==MaliBusy && Submit(B,12,1)==MaliBusy);
    CHECK(MaliExecPump(&Q,2)==MaliOk && !Q.EventPending);
    Completion(&G,0,1,0); CHECK(MaliExecPump(&Q,3)==MaliOk && D.Jobs[1].Active);
    Completion(&G,1,1,0); CHECK(MaliExecPump(&Q,4)==MaliOk); Ack(MaliExecCompleted,10);
    CHECK(MaliExecPump(&Q,5)==MaliOk && !D.Jobs[0].Active);
    MALI_EXEC_EVENT E; CHECK(MaliExecPeek(&Q,&E)==MaliOk);
    CHECK(E.Type==MaliExecPreempted && E.Fence==100 && E.LastCompletedFence==10);
    CHECK(MaliExecContextBusy(&Q,B)==1); CHECK(MaliExecAcknowledge(&Q,&E)==MaliOk);
    CHECK(!Q.Count && MaliExecContextBusy(&Q,B)==0 && !Q.PreemptionPending);
    CHECK(Submit(B,11,1)==MaliOk);CHECK(MaliExecPump(&Q,6)==MaliOk);
    Completion(&G,0,1,0);CHECK(MaliExecPump(&Q,7)==MaliOk);Ack(MaliExecCompleted,11);
    CHECK(MaliExecDestroyContext(&Q,A)==MaliOk && MaliExecDestroyContext(&Q,B)==MaliOk);
}
static void FailureAndReset(void) {
    Begin(); uint64_t C=Context(1,0x40001000);
    CHECK(Submit(C,7,1)==MaliOk && MaliExecPump(&Q,1)==MaliOk);
    CHECK(MaliExecPump(&Q,100)==MaliOk); CHECK(MaliExecPump(&Q,101)==MaliTimeout);
    CHECK(Q.Faulted && D.Jobs[0].Active && MaliExecContextBusy(&Q,C)==1);
    Ack(MaliExecFaulted,7); CHECK(D.Jobs[0].Active && MaliExecDestroyContext(&Q,C)==MaliBusy);
    unsigned Aborted=99;G.ResetStuck=1;
    CHECK(MaliExecReset(&Q,&Aborted)==MaliTimeout && Aborted==99);
    CHECK(D.Jobs[0].Active && MaliExecContextBusy(&Q,C)==1);
    G.ResetStuck=0;CHECK(MaliExecReset(&Q,&Aborted)==MaliOk && Aborted==7);
    CHECK(!Q.Faulted && !D.Jobs[0].Active && MaliExecContextBusy(&Q,C)==0);
    CHECK(Submit(C,8,1)==MaliOk && MaliExecPump(&Q,102)==MaliOk);
    Completion(&G,0,0x42,1);CHECK(MaliExecPump(&Q,103)==MaliFault);
    MALI_EXEC_EVENT E;CHECK(MaliExecPeek(&Q,&E)==MaliOk && E.HardwareStatus==0x42);
    CHECK(MaliExecReset(&Q,&Aborted)==MaliOk && Aborted==8);
    CHECK(Submit(C,9,1)==MaliOk && MaliExecPump(&Q,104)==MaliOk);
    G.Reg[0x2000/4]=1;G.Reg[0x241c/4]=0xc1;G.Reg[0x2420/4]=0xdead0000;
    CHECK(MaliExecPump(&Q,105)==MaliFault);
    CHECK(MaliExecPeek(&Q,&E)==MaliOk && E.FaultAddress==0xdead0000 && E.HardwareStatus==0xc1);
    CHECK(MaliExecReset(&Q,&Aborted)==MaliOk && Aborted==9);
    /* Completed IRQ wins over a late worker timeout; no invented GPU timeout. */
    CHECK(Submit(C,10,1)==MaliOk && MaliExecPump(&Q,106)==MaliOk);
    Completion(&G,0,1,0);CHECK(MaliExecPump(&Q,206)==MaliOk);Ack(MaliExecCompleted,10);
    CHECK(MaliExecPump(&Q,205)==MaliInvalid && Q.Faulted);
    CHECK(MaliExecReset(&Q,&Aborted)==MaliOk && Aborted==10);
    CHECK(MaliExecDestroyContext(&Q,C)==MaliOk);
}
static void ImmutableAndOwnership(void) {
    Begin();uint64_t C=Context(1,0x40001000);MALI_EXEC_PACKET P=Packet(1);
    CHECK(MaliExecSubmit(&Q,C,1,0x80000000,4096,&P,P.Bytes)==MaliOk);
    P.Steps[0].Chain=0;P.Steps[0].Slot=2;
    CHECK(MaliExecPump(&Q,1)==MaliOk && D.Jobs[0].Chain==0x80000000 && !D.Jobs[2].Active);
    Completion(&G,0,1,0);CHECK(MaliExecPump(&Q,2)==MaliOk);Ack(MaliExecCompleted,1);
    CHECK(Submit(C,2,1)==MaliOk);
    D.Jobs[2].Active=1;CHECK(MaliExecPump(&Q,3)==MaliBusy && Q.Faulted);
    CHECK(Q.Count==1 && MaliExecDestroyContext(&Q,C)==MaliBusy);
    D.Jobs[2].Fence=55; unsigned Aborted=0;
    CHECK(MaliExecReset(&Q,&Aborted)==MaliOk && Aborted==1);
    CHECK(MaliExecDestroyContext(&Q,C)==MaliOk);
}
static void PrivateExecutionRoots(void) {
    Begin();uint64_t C=Context(1,0x40001000);MALI_EXEC_PACKET P=Packet(1);unsigned Writes=G.Writes;
    CHECK(MaliExecSubmitRoot(&Q,C,0,1,0x80000000,4096,&P,P.Bytes)==MaliInvalid);
    CHECK(MaliExecSubmitRoot(&Q,C,MALI_JM_PA_LIMIT,1,0x80000000,4096,&P,P.Bytes)==MaliInvalid);
    CHECK(MaliExecSubmitRoot(&Q,C,0x50000001,1,0x80000000,4096,&P,P.Bytes)==MaliInvalid && G.Writes==Writes);
    CHECK(MaliExecSubmitRoot(&Q,C,0x50000000,1,0x80000000,4096,&P,P.Bytes)==MaliOk);
    CHECK(MaliExecPump(&Q,1)==MaliOk && D.Roots[0]==0x50000000 && Q.Active.PrivateRoot);
    Completion(&G,0,1,0);CHECK(MaliExecPump(&Q,2)==MaliOk && !D.BoundSpaces && Q.EventPending);
    CHECK(Q.LastCompletedFence==1);Ack(MaliExecCompleted,1);
    CHECK(MaliExecSubmitRoot(&Q,C,0x50001000,2,0x80000000,4096,&P,P.Bytes)==MaliOk);
    CHECK(MaliExecPump(&Q,3)==MaliOk);Completion(&G,0,1,0);G.AsStuck=1;
    CHECK(MaliExecPump(&Q,4)==MaliTimeout && Q.Faulted && D.BoundSpaces && Q.LastCompletedFence==1);
    CHECK(Q.ActiveValid && MaliExecContextBusy(&Q,C)==1);Ack(MaliExecFaulted,2);
    unsigned Aborted=0;G.AsStuck=0;CHECK(MaliExecReset(&Q,&Aborted)==MaliOk && Aborted==2);
    CHECK(!D.BoundSpaces && !Q.ActiveValid && MaliExecDestroyContext(&Q,C)==MaliOk);
}
static void IdleFaults(void) {
    static const uint32_t Raw[3]={0x20,0x1000,0x2000};
    for(unsigned I=0;I<3;I++) {
        Begin();uint64_t C=Context(1,0x40001000);G.Reg[Raw[I]/4]=1;
        CHECK(MaliExecPump(&Q,1)==MaliFault && Q.Faulted && D.Poisoned);
        CHECK(!Q.EventPending && !Q.ActiveValid && !Q.Count && !G.Reg[Raw[I]/4]);
        CHECK(!G.Reg[0x28/4] && !G.Reg[0x1008/4] && !G.Reg[0x2008/4]);
        unsigned W=G.Writes;CHECK(MaliExecPump(&Q,2)==MaliFault && G.Writes==W);
        uint32_t Aborted=99;CHECK(MaliExecReset(&Q,&Aborted)==MaliOk && !Aborted);
        CHECK(MaliExecDestroyContext(&Q,C)==MaliOk);
    }
    Begin();uint64_t C=Context(1,0x40001000);CHECK(Submit(C,7,1)==MaliOk);
    G.Reg[0x20/4]=1;CHECK(MaliExecPump(&Q,1)==MaliFault);
    CHECK(Q.Count==1 && !Q.ActiveValid && !Q.EventPending && !D.Jobs[0].Active);
    CHECK(MaliExecContextBusy(&Q,C)==1);
    uint32_t Aborted=99;CHECK(MaliExecReset(&Q,&Aborted)==MaliOk && !Aborted);
    CHECK(!Q.Count && MaliExecDestroyContext(&Q,C)==MaliOk);
}
static void ResetFenceProgress(void) {
    Begin(); uint64_t C=Context(1,0x40001000); uint32_t Aborted=99;
    CHECK(Submit(C,UINT32_MAX,1)==MaliOk && MaliExecPump(&Q,1)==MaliOk);
    CHECK(MaliExecPump(&Q,102)==MaliTimeout);
    G.ResetStuck=1;
    CHECK(MaliExecReset(&Q,&Aborted)==MaliTimeout && Aborted==99 && Q.LastCompletedFence==0);
    G.ResetStuck=0;
    CHECK(MaliExecReset(&Q,&Aborted)==MaliOk && Aborted==UINT32_MAX);
    CHECK(Q.LastCompletedFence==UINT32_MAX);
    CHECK(MaliExecRequestPreempt(&Q,100)==MaliOk && MaliExecPump(&Q,103)==MaliOk);
    MALI_EXEC_EVENT E;
    CHECK(MaliExecPeek(&Q,&E)==MaliOk && E.Type==MaliExecPreempted && E.LastCompletedFence==UINT32_MAX);
    CHECK(MaliExecAcknowledge(&Q,&E)==MaliOk);
    Aborted=99;
    CHECK(MaliExecReset(&Q,&Aborted)==MaliOk && Aborted==UINT32_MAX && Q.LastCompletedFence==UINT32_MAX);
    CHECK(Submit(C,0,1)==MaliOk && MaliExecPump(&Q,104)==MaliOk);
    CHECK(MaliExecPump(&Q,205)==MaliTimeout);
    CHECK(MaliExecReset(&Q,&Aborted)==MaliOk && Aborted==0 && Q.LastCompletedFence==0);
    CHECK(MaliExecDestroyContext(&Q,C)==MaliOk);

    Begin(); C=Context(1,0x40001000);
    CHECK(Submit(C,7,1)==MaliOk && MaliExecPump(&Q,1)==MaliOk);
    CHECK(MaliExecPump(&Q,102)==MaliTimeout);
    G.PowerStuck=1; G.Reg[0x160/4]=0;
    CHECK(MaliExecReset(&Q,&Aborted)==MaliTimeout && Q.LastCompletedFence==0 && Q.Faulted);
    CHECK(!Q.ActiveValid && !D.Jobs[0].Active);

    Begin(); Q.ResetEpoch=UINT64_MAX; Aborted=99;
    CHECK(MaliExecReset(&Q,&Aborted)==MaliUnsupported && Aborted==99 && Q.LastCompletedFence==0);
}
int TestMain(void) {
    LifetimeAndOrder();Validation();LimitsAndRoots();Preemption();FailureAndReset();ImmutableAndOwnership();PrivateExecutionRoots();IdleFaults();ResetFenceProgress();
    return (int)FirstFailure;
}
#ifndef _WIN32
#include <stdio.h>
int main(void) {
    int R=TestMain();
    printf("Mali execution: %u checks, first failure line %d (synthetic GPU)\n",MtkMaliExecutionAssertionCount,R);
    return R ? 1 : 0;
}
#endif
