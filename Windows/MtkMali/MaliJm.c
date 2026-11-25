// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MaliJm.h"

#define GPU_IRQ_RAW 0x20u
#define GPU_IRQ_CLEAR 0x24u
#define GPU_IRQ_MASK 0x28u
#define GPU_COMMAND 0x30u
#define JOB_IRQ_RAW 0x1000u
#define JOB_IRQ_CLEAR 0x1004u
#define JOB_IRQ_MASK 0x1008u
#define MMU_IRQ_RAW 0x2000u
#define MMU_IRQ_CLEAR 0x2004u
#define MMU_IRQ_MASK 0x2008u
#define JS(S,O) (0x1800u + (S)*0x80u + (O))
#define AS(A,O) (0x2400u + (A)*0x40u + (O))
#define RESET_DONE (1u<<8)
#define CACHE_DONE (1u<<17)
#define GPU_FAULTS ((1u<<0)|(1u<<7))
#define PA_MASK UINT64_C(0x000000fffffff000)
#define XN_MASK (UINT64_C(1)<<54)
#define LEAF_ALLOWED (PA_MASK|XN_MASK|UINT64_C(0x4cb))
#define POLLS 10000u

static int Page(uint64_t A) { return A && A<MALI_JM_PA_LIMIT && !(A&4095); }
static uint32_t Read(MALI_JM *D,uint32_t O) { return D->Io.Read32(D->Io.Context,O); }
static void Write(MALI_JM *D,uint32_t O,uint32_t V) { D->Io.Write32(D->Io.Context,O,V); }
static void Barrier(MALI_JM *D) { D->Io.Barrier(D->Io.Context); }
static uint64_t Read64(MALI_JM *D,uint32_t O) {
    uint32_t L=Read(D,O),H=Read(D,O+4);return ((uint64_t)H<<32)|L;
}
static void Write64(MALI_JM *D,uint32_t O,uint64_t V) {
    Write(D,O,(uint32_t)V);Write(D,O+4,(uint32_t)(V>>32));
}
static MALI_RESULT Power(MALI_JM *D) {
    if(!D || !D->Io.PowerReady)return MaliOffline;
    if(!D->Io.PowerReady(D->Io.Context)) {
        if(D->Started)D->Poisoned=1;
        return MaliOffline;
    }
    return MaliOk;
}
static MALI_RESULT Usable(MALI_JM *D) {
    MALI_RESULT R=Power(D);if(R)return R;
    if(D->Poisoned)return MaliFault;
    return D->Started?MaliOk:MaliOffline;
}
static MALI_RESULT Wait(MALI_JM *D,uint32_t O,uint64_t Mask,uint64_t Value,int Wide) {
    for(uint32_t I=0;I<POLLS;I++) {
        uint64_t V=Wide?Read64(D,O):Read(D,O);
        if((V&Mask)==Value)return MaliOk;
        D->Io.DelayUs(D->Io.Context,10);
    }
    D->Poisoned=1;return MaliTimeout;
}
static MALI_RESULT AsCommand(MALI_JM *D,uint32_t A,uint32_t Cmd) {
    MALI_RESULT R=Wait(D,AS(A,0x28),1,0,0);if(R)return R;
    Write(D,AS(A,0x18),Cmd);Barrier(D);
    return Wait(D,AS(A,0x28),1,0,0);
}
static int AsInUse(MALI_JM *D,uint32_t A) {
    for(uint32_t I=0;I<MALI_JM_MAX_SLOTS;I++)
        if(D->Jobs[I].Active && D->Jobs[I].AddressSpace==A)return 1;
    return 0;
}
static int AnyJob(MALI_JM *D) {
    for(uint32_t I=0;I<MALI_JM_MAX_SLOTS;I++)if(D->Jobs[I].Active)return 1;
    return 0;
}
static void MaskAll(MALI_JM *D) {
    Write(D,GPU_IRQ_MASK,0);Write(D,JOB_IRQ_MASK,0);Write(D,MMU_IRQ_MASK,0);Barrier(D);
}
MALI_RESULT MaliJmEncodePte(uint32_t Level,const MALI_PTE *E,uint64_t *Out) {
    if(!E||!Out||Level>3)return MaliInvalid;
    uint32_t F=E->Flags;
    if(!F) {if(E->Address)return MaliInvalid;*Out=0;return MaliOk;}
    if(!(F&MaliPteValid)||!Page(E->Address))return MaliInvalid;
    if(Level<3) {
        if(F!=(MaliPteValid|MaliPteTable))return MaliInvalid;
        *Out=E->Address|0x403;return MaliOk;
    }
    if((F&~(MaliPteValid|MaliPteRead|MaliPteWrite|MaliPteExecute|MaliPteCached)) ||
       !(F&MaliPteRead))return MaliInvalid;
    /* Stock G57 uses TRANSCFG AArch64-4K, not the old ARM_MALI_LPAE mode:
       L3 type 3, AF=1, stage-2 R/W, one NX bit at 54. */
    *Out=E->Address|0x403|((F&MaliPteCached)?8u:0u)|(1u<<6)|
        ((F&MaliPteWrite)?(2u<<6):0u)|((F&MaliPteExecute)?0:XN_MASK);
    return MaliOk;
}
static int RangesOverlap(uintptr_t A,size_t ABytes,uintptr_t B,size_t BBytes) {
    return ABytes && BBytes && A<B+BBytes && B<A+ABytes;
}
MALI_RESULT MaliJmWritePtes(uint64_t *T,uint32_t L,uint32_t Start,uint32_t N,
                          const MALI_PTE *E,int Repeat,uint64_t *Encoded,uint32_t Capacity) {
    if(!T||((uintptr_t)T&7)||L>3||Start>512||N>512-Start||(!E&&N)||
       (Repeat!=0&&Repeat!=1)||Capacity<N||(!Encoded&&N)||((uintptr_t)Encoded&7))return MaliInvalid;
    if(!N)return MaliOk;
    uintptr_t Table=(uintptr_t)T,Input=(uintptr_t)E,Scratch=(uintptr_t)Encoded;
    size_t InputBytes=(Repeat?1u:N)*sizeof(*E),ScratchBytes=N*sizeof(*Encoded);
    if((Input&7u)||Table>UINTPTR_MAX-4096 || Input>UINTPTR_MAX-InputBytes ||
       Scratch>UINTPTR_MAX-ScratchBytes ||
       RangesOverlap(Scratch,ScratchBytes,Table,4096) ||
       RangesOverlap(Scratch,ScratchBytes,Input,InputBytes))return MaliInvalid;
    for(uint32_t I=0;I<N;I++) {
        MALI_RESULT R=MaliJmEncodePte(L,&E[Repeat?0:I],&Encoded[I]);if(R)return R;
    }
    for(uint32_t I=0;I<N;I++)T[Start+I]=Encoded[I];
    return MaliOk;
}
MALI_RESULT MaliJmTranslate(void *C,MALI_RESOLVE_TABLE Resolve,uint64_t Root,uint64_t Va,uint64_t *Pa,uint32_t *Flags) {
    if(!Resolve||!Pa||!Flags||!Page(Root)||Va>=MALI_JM_VA_LIMIT)return MaliInvalid;
    uint64_t Seen[4],Table=Root;
    for(uint32_t L=0;L<4;L++) {
        for(uint32_t J=0;J<L;J++)if(Seen[J]==Table)return MaliInvalid;
        Seen[L]=Table;
        const uint64_t *P=Resolve(C,Table);if(!P||((uintptr_t)P&7))return MaliInvalid;
        uint64_t E=P[(Va>>(39-L*9))&511],Address=E&PA_MASK;
        if(!E)return MaliOffline;
        if(!Page(Address))return MaliInvalid;
        if(L<3) {
            if((E&~PA_MASK)!=0x403)return MaliInvalid;
            Table=Address;continue;
        }
        if((E&0x403)!=0x403||(E&~LEAF_ALLOWED)||(((E>>2)&7)!=0&&((E>>2)&7)!=2)||
           !(E&(1u<<6)))return MaliInvalid;
        *Pa=Address|(Va&4095);
        *Flags=MaliPteValid|MaliPteRead|((E&(2u<<6))?MaliPteWrite:0)|
            ((E&XN_MASK)?0:MaliPteExecute)|((E&8)?MaliPteCached:0);
        return MaliOk;
    }
    return MaliInvalid;
}
MALI_RESULT MaliJmLockAddress(uint64_t Start,uint64_t Size,uint64_t *Out) {
    if(!Out||!Size||Start>=MALI_JM_VA_LIMIT||Size>MALI_JM_VA_LIMIT-Start)return MaliInvalid;
    uint64_t Diff=Start^(Start+Size-1);uint32_t Bits=0;
    while(Diff) {++Bits;Diff>>=1;}
    if(Bits<15)Bits=15;
    *Out=(Start&~((UINT64_C(1)<<Bits)-1))|(Bits-1);return MaliOk;
}
MALI_RESULT MaliJmInitialize(MALI_JM *D,const MALI_IO *Io) {
    if(!D||!Io||!Io->Read32||!Io->Write32||!Io->Barrier||!Io->DelayUs||!Io->PowerReady)return MaliInvalid;
    /* Initialize must only be used on an unowned/new object, never on a live GPU. */
    MALI_JM Empty={0};*D=Empty;D->Io=*Io;return MaliOk;
}
MALI_RESULT MaliJmProbe(MALI_JM *D,MALI_CAPS *Caps) {
    if(!Caps)return MaliInvalid;
    MALI_RESULT R=Power(D);if(R)return R;
    MALI_CAPS C={0};C.GpuId=Read(D,0);
    if((C.GpuId>>16)!=0x9093)return MaliUnsupported;
    C.MmuFeatures=Read(D,0x14);C.AddressSpaces=Read(D,0x18);C.JobSlots=Read(D,0x1c);
    C.Shaders=Read64(D,0x100);C.Tilers=Read64(D,0x110);C.L2=Read64(D,0x120);
    if(!C.AddressSpaces||(C.AddressSpaces&~0xffffu)||!C.JobSlots||(C.JobSlots&~7u)||
       (C.MmuFeatures&255)<48||((C.MmuFeatures>>8)&255)<40||
       !C.Shaders||!C.Tilers||!C.L2||C.Shaders==UINT64_MAX||C.Tilers==UINT64_MAX||C.L2==UINT64_MAX)return MaliUnsupported;
    for(uint32_t I=0;I<MALI_JM_MAX_SLOTS;I++)C.JobFeatures[I]=Read(D,0xc0+4*I);
    *Caps=C;return MaliOk;
}
MALI_RESULT MaliJmReset(MALI_JM *D,MALI_EVENT *Aborted,uint32_t Capacity,uint32_t *Count) {
    if(!D||!Aborted||!Count||Capacity<MALI_JM_MAX_SLOTS)return MaliInvalid;
    *Count=0;MALI_RESULT R=Power(D);if(R)return R;
    MALI_CAPS C;R=MaliJmProbe(D,&C);if(R)return R;
    MaskAll(D);D->Started=0;D->Poisoned=1;
    Write(D,GPU_IRQ_CLEAR,RESET_DONE);Write(D,GPU_COMMAND,1);Barrier(D);
    R=Wait(D,GPU_IRQ_RAW,RESET_DONE,RESET_DONE,0);if(R)return R;
    Write(D,GPU_IRQ_CLEAR,RESET_DONE);Barrier(D);
    /* RESET_DONE establishes that old DMA stopped; only now retire old jobs. */
    for(uint32_t I=0;I<MALI_JM_MAX_SLOTS;I++) {
        if(D->Jobs[I].Active) {
            MALI_EVENT E={MaliJobAborted,I,D->Jobs[I].AddressSpace,0,D->Jobs[I].Fence,D->Jobs[I].Chain};
            Aborted[(*Count)++]=E;
        }
        MALI_JOB Empty={0};D->Jobs[I]=Empty;
    }
    for(uint32_t I=0;I<MALI_JM_MAX_AS;I++)D->Roots[I]=0;
    D->Caps=C;D->BoundSpaces=0;D->Poisoned=0;return MaliOk;
}
MALI_RESULT MaliJmSetManagedInterrupts(MALI_JM *D,uint32_t Managed) {
    if(!D || !D->Io.Read32 || Managed>1)return MaliInvalid;
    if(D->Started || D->Poisoned || D->BoundSpaces || AnyJob(D))return MaliBusy;
    D->ManagedInterrupts=Managed;return MaliOk;
}
MALI_RESULT MaliJmStart(MALI_JM *D) {
    if(!D)return MaliInvalid;
    MALI_RESULT R=Power(D);if(R)return R;
    if(D->Started||AnyJob(D))return MaliBusy;
    MALI_EVENT E[MALI_JM_MAX_SLOTS];uint32_t N=0;
    R=MaliJmReset(D,E,MALI_JM_MAX_SLOTS,&N);if(R)return R;
    Write64(D,0x1a0,D->Caps.L2);Barrier(D);
    R=Wait(D,0x160,D->Caps.L2,D->Caps.L2,1);if(R)return R;
    Write64(D,0x190,D->Caps.Tilers);Barrier(D);
    R=Wait(D,0x150,D->Caps.Tilers,D->Caps.Tilers,1);if(R)return R;
    Write64(D,0x180,D->Caps.Shaders);Barrier(D);
    R=Wait(D,0x140,D->Caps.Shaders,D->Caps.Shaders,1);if(R)return R;
    Write(D,JOB_IRQ_CLEAR,D->Caps.JobSlots|(D->Caps.JobSlots<<16));
    Write(D,MMU_IRQ_CLEAR,D->Caps.AddressSpaces|(D->Caps.AddressSpaces<<16));
    Write(D,GPU_IRQ_CLEAR,GPU_FAULTS|CACHE_DONE);
    Write(D,GPU_IRQ_MASK,D->ManagedInterrupts?0:GPU_FAULTS);
    Write(D,JOB_IRQ_MASK,D->ManagedInterrupts?0:D->Caps.JobSlots|(D->Caps.JobSlots<<16));
    Write(D,MMU_IRQ_MASK,D->ManagedInterrupts?0:D->Caps.AddressSpaces|(D->Caps.AddressSpaces<<16));
    Barrier(D);D->Started=1;return MaliOk;
}
MALI_RESULT MaliJmFlush(MALI_JM *D,uint32_t A,uint64_t Start,uint64_t Size) {
    uint64_t Lock;MALI_RESULT R=MaliJmLockAddress(Start,Size,&Lock);if(R)return R;
    R=Usable(D);if(R)return R;
    if(A>=MALI_JM_MAX_AS||!(D->BoundSpaces&(1u<<A)))return MaliInvalid;
    /* The caller must quiesce the AS before replacing any live PTEs. */
    if(AsInUse(D,A))return MaliBusy;
    R=Wait(D,AS(A,0x28),1,0,0);if(R)return R;
    Barrier(D);Write64(D,AS(A,0x10),Lock);
    R=AsCommand(D,A,2);if(R)return R;
    return AsCommand(D,A,5);
}
MALI_RESULT MaliJmBind(MALI_JM *D,uint32_t A,uint64_t Root) {
    if(!Page(Root)||A>=MALI_JM_MAX_AS)return MaliInvalid;
    MALI_RESULT R=Usable(D);if(R)return R;
    if(!(D->Caps.AddressSpaces&(1u<<A)))return MaliInvalid;
    if(AsInUse(D,A))return MaliBusy;
    if(D->BoundSpaces&(1u<<A)) {
        R=MaliJmFlush(D,A,0,MALI_JM_VA_LIMIT);if(R)return R;
    }
    R=Wait(D,AS(A,0x28),1,0,0);if(R)return R;
    Barrier(D);Write64(D,AS(A,0),Root);Write64(D,AS(A,8),MALI_JM_MEMATTR);
    Write64(D,AS(A,0x30),UINT64_C(0x42000006));
    R=AsCommand(D,A,1);if(R)return R;
    D->Roots[A]=Root;D->BoundSpaces|=1u<<A;return MaliOk;
}
MALI_RESULT MaliJmUnbind(MALI_JM *D,uint32_t A) {
    MALI_RESULT R=Usable(D);if(R)return R;
    if(A>=MALI_JM_MAX_AS||!(D->BoundSpaces&(1u<<A)))return MaliInvalid;
    if(AsInUse(D,A))return MaliBusy;
    R=MaliJmFlush(D,A,0,MALI_JM_VA_LIMIT);if(R)return R;
    Write64(D,AS(A,0),0);Write64(D,AS(A,8),0);Write64(D,AS(A,0x30),1);R=AsCommand(D,A,1);if(R)return R;
    D->Roots[A]=0;D->BoundSpaces&=~(1u<<A);return MaliOk;
}
MALI_RESULT MaliJmSubmit(MALI_JM *D,uint32_t S,uint32_t A,uint64_t Chain,uint64_t Affinity,uint64_t Fence) {
    if(S>=MALI_JM_MAX_SLOTS||A>=MALI_JM_MAX_AS||!Chain||Chain>=MALI_JM_VA_LIMIT||
       (Chain&63)||!Affinity||!Fence)return MaliInvalid;
    MALI_RESULT R=Usable(D);if(R)return R;
    if(!(D->Caps.JobSlots&(1u<<S))||!(D->BoundSpaces&(1u<<A))||(Affinity&~D->Caps.Shaders))return MaliInvalid;
    if(D->Jobs[S].Active||Read(D,JS(S,0x60)))return MaliBusy;
    for(uint32_t I=0;I<MALI_JM_MAX_SLOTS;I++)
        if(D->Jobs[I].Active&&D->Jobs[I].Fence==Fence)return MaliInvalid;
    if((Read64(D,0x140)&Affinity)!=Affinity)return MaliOffline;
    /* One in-flight job per slot avoids ambiguous CURRENT/NEXT IRQ accounting.
       MMU root and every descriptor/BO must already be owned, resident and clean. */
    MALI_JOB J={Chain,Affinity,Fence,A,1};D->Jobs[S]=J;
    Barrier(D);Write64(D,JS(S,0x40),Chain);Write64(D,JS(S,0x50),Affinity);
    Write(D,JS(S,0x58),A|(3u<<8)|(3u<<12)|(8u<<16));Barrier(D);
    Write(D,JS(S,0x60),1);Barrier(D);return MaliOk;
}
MALI_RESULT MaliJmPoll(MALI_JM *D,MALI_EVENT *Events,uint32_t Capacity,uint32_t *Count) {
    if(!D||!Events||!Count||Capacity<MALI_JM_MAX_EVENTS)return MaliInvalid;
    *Count=0;MALI_RESULT R=Power(D);if(R)return R;
    if(!D->Started)return MaliOffline;
    if(D->Poisoned)return MaliFault;
    uint32_t G=Read(D,GPU_IRQ_RAW)&GPU_FAULTS;
    uint32_t M=Read(D,MMU_IRQ_RAW)&(D->Caps.AddressSpaces|(D->Caps.AddressSpaces<<16));
    uint32_t J=Read(D,JOB_IRQ_RAW)&(D->Caps.JobSlots|(D->Caps.JobSlots<<16));
    if(G) {
        MALI_EVENT E={MaliGpuFault,UINT32_MAX,UINT32_MAX,Read(D,0x3c),0,Read64(D,0x40)};
        Events[(*Count)++]=E;D->Poisoned=1;
    }
    for(uint32_t A=0;A<MALI_JM_MAX_AS;A++)if(M&((1u<<A)|(1u<<(A+16)))) {
        MALI_EVENT E={MaliMmuFault,UINT32_MAX,A,Read(D,AS(A,0x1c)),0,Read64(D,AS(A,0x20))};
        Events[(*Count)++]=E;D->Poisoned=1;
    }
    for(uint32_t S=0;S<MALI_JM_MAX_SLOTS;S++)if(J&((1u<<S)|(1u<<(S+16)))) {
        uint32_t Status=Read(D,JS(S,0x24));
        if(!D->Jobs[S].Active) {D->Poisoned=1;continue;}
        int Failed=D->Poisoned||(J&(1u<<(S+16)))||((Status&255)!=1);
        MALI_EVENT E={Failed?MaliJobFailed:MaliJobDone,S,D->Jobs[S].AddressSpace,Status,
                      D->Jobs[S].Fence,D->Jobs[S].Chain};
        Events[(*Count)++]=E;
        if(Failed)D->Poisoned=1;
        else {Barrier(D);D->Jobs[S].Active=0;}
    }
    if(D->Poisoned)MaskAll(D);
    if(G)Write(D,GPU_IRQ_CLEAR,G);
    if(M)Write(D,MMU_IRQ_CLEAR,M);
    if(J)Write(D,JOB_IRQ_CLEAR,J);
    Barrier(D);return D->Poisoned?MaliFault:MaliOk;
}
MALI_RESULT MaliJmStop(MALI_JM *D) {
    MALI_RESULT R=Usable(D);if(R)return R;
    if(AnyJob(D))return MaliBusy;
    Write(D,GPU_IRQ_CLEAR,CACHE_DONE);Write(D,GPU_COMMAND,8);Barrier(D);
    R=Wait(D,GPU_IRQ_RAW,CACHE_DONE,CACHE_DONE,0);if(R)return R;
    Write(D,GPU_IRQ_CLEAR,CACHE_DONE);
    for(uint32_t A=0;A<MALI_JM_MAX_AS;A++)if(D->BoundSpaces&(1u<<A)) {
        R=MaliJmUnbind(D,A);if(R)return R;
    }
    MaskAll(D);D->Started=0;return MaliOk;
}
