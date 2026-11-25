// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MaliJm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks;
#define CHECK(X) do {++Checks;if(!(X)){fprintf(stderr,"line %d: %s\n",__LINE__,#X);assert(X);}} while(0)
#include "TestGpu.h"
static uint64_t Tables[4][512];
static const uint64_t *Resolve(void *C,uint64_t P) {
    (void)C;return P>=0x40001000&&P<=0x40004000?Tables[(P-0x40001000)/4096]:NULL;
}
static void PteTests(void) {
    uint64_t V=UINT64_MAX;MALI_PTE E={0x40000000,MaliPteValid|MaliPteRead};
    CHECK(MaliJmEncodePte(3,&E,&V)==MaliOk);CHECK(V==UINT64_C(0x0040000040000443));
    E.Flags|=MaliPteWrite|MaliPteExecute|MaliPteCached;
    CHECK(MaliJmEncodePte(3,&E,&V)==MaliOk);CHECK(V==UINT64_C(0x400004cb));
    E.Flags=MaliPteValid|MaliPteTable;
    for(unsigned L=0;L<3;L++){CHECK(MaliJmEncodePte(L,&E,&V)==MaliOk);CHECK(V==0x40000403);}
    CHECK(MaliJmEncodePte(3,&E,&V)==MaliInvalid);
    E.Flags=MaliPteValid|MaliPteWrite;CHECK(MaliJmEncodePte(3,&E,&V)==MaliInvalid);
    E.Flags=MaliPteValid|MaliPteRead;
    for(unsigned B=0;B<64;B++) {
        E.Address=UINT64_C(1)<<B;
        CHECK(MaliJmEncodePte(3,&E,&V)==((B>=12&&B<40)?MaliOk:MaliInvalid));
    }
    E.Address=0x40000000;
    for(unsigned F=0;F<256;F++) {
        E.Flags=F;MALI_RESULT R=MaliJmEncodePte(3,&E,&V);
        int Valid=(F&MaliPteValid)&&(F&MaliPteRead)&&!(F&~31u);
        CHECK(R==(Valid?MaliOk:MaliInvalid));
    }
    uint64_t T[512],Before[512],Scratch[512];MALI_PTE Entries[512];
    for(unsigned I=0;I<512;I++) {T[I]=UINT64_C(0xabcdef0123456789);Entries[I]=(MALI_PTE){0x40000000+(uint64_t)I*4096,7};}
    memcpy(Before,T,sizeof(T));Entries[511].Address++;
    CHECK(MaliJmWritePtes(T,3,0,512,Entries,0,Scratch,512)==MaliInvalid);CHECK(!memcmp(T,Before,sizeof(T)));
    CHECK(MaliJmWritePtes(T,3,0,512,Entries,0,Scratch,511)==MaliInvalid);
    CHECK(MaliJmWritePtes(T,3,0,1,Entries,0,T,512)==MaliInvalid);
    CHECK(MaliJmWritePtes(T,3,0,1,Entries,0,(uint64_t*)Entries,512)==MaliInvalid);
    CHECK(MaliJmWritePtes(T,3,0,1,Entries,0,0,512)==MaliInvalid);
    CHECK(!memcmp(T,Before,sizeof(T)));
    Entries[511].Address--;CHECK(MaliJmWritePtes(T,3,0,512,Entries,0,Scratch,512)==MaliOk);
    for(unsigned I=0;I<512;I++)CHECK(T[I]==UINT64_C(0x00400000400004c3)+(uint64_t)I*4096);
    CHECK(MaliJmWritePtes(T,3,511,2,Entries,0,Scratch,512)==MaliInvalid);
    CHECK(MaliJmWritePtes(T,3,0,512,Entries,1,Scratch,512)==MaliOk);
    for(unsigned I=0;I<512;I++)CHECK(T[I]==UINT64_C(0x00400000400004c3));
    CHECK(MaliJmWritePtes(T,3,512,0,NULL,0,Scratch,512)==MaliOk);
    MALI_PTE Empty={0,0};CHECK(MaliJmWritePtes(T,3,0,512,&Empty,1,Scratch,512)==MaliOk);
    for(unsigned I=0;I<512;I++)CHECK(T[I]==0);
    for(unsigned I=0;I<200;I++) {
        memset(Tables,0,sizeof(Tables));uint64_t Va=((UINT64_C(0x123456789abc)*I)&(MALI_JM_VA_LIMIT-1));
        for(unsigned L=0;L<3;L++)Tables[L][(Va>>(39-L*9))&511]=(0x40002000+(uint64_t)L*4096)|0x403;
        Tables[3][(Va>>12)&511]=UINT64_C(0x00400000567894cb);
        uint64_t Pa=0;uint32_t Flags=0;
        CHECK(MaliJmTranslate(NULL,Resolve,0x40001000,Va,&Pa,&Flags)==MaliOk);
        CHECK(Pa==(UINT64_C(0x56789000)|(Va&4095)));CHECK(Flags==(MaliPteValid|MaliPteRead|MaliPteWrite|MaliPteCached));
        Tables[3][(Va>>12)&511]|=UINT64_C(1)<<40;
        CHECK(MaliJmTranslate(NULL,Resolve,0x40001000,Va,&Pa,&Flags)==MaliInvalid);
        Tables[0][(Va>>39)&511]=0x40001403;
        CHECK(MaliJmTranslate(NULL,Resolve,0x40001000,Va,&Pa,&Flags)==MaliInvalid);
    }
    CHECK(MaliJmLockAddress(0x1000,4096,&V)==MaliOk);CHECK(V==14);
    CHECK(MaliJmLockAddress(0x7fff,2,&V)==MaliOk);CHECK(V==15);
    CHECK(MaliJmLockAddress(0,MALI_JM_VA_LIMIT,&V)==MaliOk);CHECK(V==47);
    CHECK(MaliJmLockAddress(MALI_JM_VA_LIMIT-1,2,&V)==MaliInvalid);
    CHECK(MaliJmLockAddress(0,0,&V)==MaliInvalid);
}
static void DeviceTests(void) {
    GPU G;MALI_JM D;MALI_CAPS C;MALI_EVENT E[MALI_JM_MAX_EVENTS];uint32_t N=99,W;
    Init(&G,&D);G.Powered=0;
    CHECK(MaliJmProbe(&D,&C)==MaliOffline);CHECK(MaliJmStart(&D)==MaliOffline);CHECK(!G.Reads&&!G.Writes);
    G.Powered=1;G.Reg[0]=0x92000000;CHECK(MaliJmStart(&D)==MaliUnsupported);CHECK(!G.Writes);
    G.Reg[0]=0x90930002;CHECK(MaliJmStart(&D)==MaliOk);CHECK(D.Started&&!D.Poisoned);
    CHECK(G.Reg[0x140/4]==5&&G.Reg[0x150/4]==1&&G.Reg[0x160/4]==1);
    CHECK(MaliJmStart(&D)==MaliBusy);
    CHECK(MaliJmBind(&D,0,0x40001000)==MaliOk);
    CHECK(G.Reg[0x2400/4]==0x40001000&&G.Reg[0x2408/4]==0x888d8f88&&G.Reg[0x240c/4]==0x4c8d&&G.Reg[0x2430/4]==0x42000006);
    CHECK(MaliJmFlush(&D,0,0x7fff,2)==MaliOk);CHECK(G.Reg[0x2410/4]==15&&G.Reg[0x2418/4]==5);
    W=G.Writes;
    CHECK(MaliJmSubmit(&D,3,0,0x80000000,5,1)==MaliInvalid);
    CHECK(MaliJmSubmit(&D,0,0,0x80000001,5,1)==MaliInvalid);
    CHECK(MaliJmSubmit(&D,0,0,0x80000000,2,1)==MaliInvalid);CHECK(G.Writes==W);
    CHECK(MaliJmSubmit(&D,1,0,0x80000000,5,1)==MaliOk);
    CHECK(G.Offsets[G.Writes-1]==0x18e0&&G.Values[G.Writes-1]==1);
    CHECK(G.Reg[0x18c0/4]==0x80000000&&G.Reg[0x18d0/4]==5&&G.Reg[0x18d8/4]==0x83300);
    CHECK(MaliJmSubmit(&D,1,0,0x80000040,5,2)==MaliBusy);
    CHECK(MaliJmSubmit(&D,0,0,0x80000040,5,1)==MaliInvalid);
    CHECK(MaliJmFlush(&D,0,0,4096)==MaliBusy);CHECK(MaliJmUnbind(&D,0)==MaliBusy);CHECK(MaliJmStop(&D)==MaliBusy);
    CHECK(MaliJmPoll(&D,E,20,&N)==MaliOk);CHECK(N==0&&D.Jobs[1].Active);
    Completion(&G,1,1,0);CHECK(MaliJmPoll(&D,E,20,&N)==MaliOk);
    CHECK(N==1&&E[0].Type==MaliJobDone&&E[0].Fence==1&&!D.Jobs[1].Active);
    CHECK(MaliJmPoll(&D,E,20,&N)==MaliOk);CHECK(!N);
    CHECK(MaliJmSubmit(&D,0,0,0x80000040,5,2)==MaliOk);
    Completion(&G,0,0x42,1);CHECK(MaliJmPoll(&D,E,20,&N)==MaliFault);
    CHECK(N==1&&E[0].Type==MaliJobFailed&&D.Jobs[0].Active&&D.Poisoned);
    CHECK(G.Reg[0x28/4]==0&&G.Reg[0x1008/4]==0&&G.Reg[0x2008/4]==0);
    G.ResetStuck=1;CHECK(MaliJmReset(&D,E,20,&N)==MaliTimeout);CHECK(!N&&D.Jobs[0].Active&&D.Poisoned);CHECK(G.Delays==10000);
    G.ResetStuck=0;CHECK(MaliJmReset(&D,E,20,&N)==MaliOk);
    CHECK(N==1&&E[0].Type==MaliJobAborted&&E[0].Fence==2&&!D.Jobs[0].Active&&!D.Started&&!D.BoundSpaces);
    CHECK(MaliJmStart(&D)==MaliOk);CHECK(MaliJmBind(&D,3,0x40002000)==MaliOk);
    CHECK(MaliJmSubmit(&D,2,3,0x80001000,4,3)==MaliOk);
    G.Reg[0x2000/4]=(1u<<3);G.Reg[(0x2400+3*64+0x1c)/4]=0xc1;
    G.Reg[(0x2400+3*64+0x20)/4]=0x76543210;G.Reg[(0x2400+3*64+0x24)/4]=0x1234;
    CHECK(MaliJmPoll(&D,E,20,&N)==MaliFault);CHECK(N==1&&E[0].Type==MaliMmuFault);
    CHECK(E[0].Address==UINT64_C(0x123476543210)&&E[0].AddressSpace==3&&E[0].Status==0xc1);
    CHECK(D.Jobs[2].Active);CHECK(MaliJmReset(&D,E,20,&N)==MaliOk);CHECK(N==1&&E[0].Fence==3);
    CHECK(MaliJmStart(&D)==MaliOk);CHECK(MaliJmBind(&D,0,0x40001000)==MaliOk);
    G.Powered=0;W=G.Writes;CHECK(MaliJmUnbind(&D,0)==MaliOffline);CHECK(G.Writes==W&&D.Poisoned);
    G.Powered=1;CHECK(MaliJmSubmit(&D,0,0,0x80000000,5,4)==MaliFault);
    CHECK(MaliJmReset(&D,E,20,&N)==MaliOk);CHECK(MaliJmStart(&D)==MaliOk);
    CHECK(MaliJmBind(&D,0,0x40001000)==MaliOk);CHECK(MaliJmStop(&D)==MaliOk);CHECK(!D.Started&&!D.BoundSpaces);
    Init(&G,&D);G.PowerStuck=1;CHECK(MaliJmStart(&D)==MaliTimeout);CHECK(D.Poisoned&&!D.Started);
    Init(&G,&D);CHECK(MaliJmStart(&D)==MaliOk);G.AsStuck=1;W=G.Writes;
    CHECK(MaliJmBind(&D,0,0x40001000)==MaliTimeout);CHECK(G.Writes==W&&D.Poisoned);
    Init(&G,&D);CHECK(MaliJmStart(&D)==MaliOk);G.CacheStuck=1;
    CHECK(MaliJmStop(&D)==MaliTimeout);CHECK(D.Poisoned&&D.Started);
}
static void ManagedInterruptTests(void) {
    GPU G;MALI_JM D,Empty={0};MALI_EVENT E[MALI_JM_MAX_EVENTS];uint32_t N=0;
    Init(&G,&D);CHECK(!D.ManagedInterrupts);
    CHECK(MaliJmSetManagedInterrupts(NULL,1)==MaliInvalid);
    CHECK(MaliJmSetManagedInterrupts(&Empty,1)==MaliInvalid);
    CHECK(MaliJmSetManagedInterrupts(&D,2)==MaliInvalid && !G.Reads && !G.Writes);
    CHECK(MaliJmSetManagedInterrupts(&D,1)==MaliOk);
    CHECK(MaliJmStart(&D)==MaliOk && D.ManagedInterrupts);
    CHECK(!G.Reg[0x28/4] && !G.Reg[0x1008/4] && !G.Reg[0x2008/4]);
    unsigned W=G.Writes;CHECK(MaliJmSetManagedInterrupts(&D,0)==MaliBusy && G.Writes==W);
    CHECK(MaliJmBind(&D,0,0x40001000)==MaliOk);
    CHECK(MaliJmSubmit(&D,0,0,0x80000000,5,7)==MaliOk);
    Completion(&G,0,1,0);CHECK(MaliJmPoll(&D,E,MALI_JM_MAX_EVENTS,&N)==MaliOk && N==1);
    CHECK(MaliJmReset(&D,E,MALI_JM_MAX_EVENTS,&N)==MaliOk && D.ManagedInterrupts);
    CHECK(MaliJmStart(&D)==MaliOk && MaliJmStop(&D)==MaliOk);
    for(unsigned I=0;I<G.Writes;I++)if(G.Offsets[I]==0x28 || G.Offsets[I]==0x1008 || G.Offsets[I]==0x2008)
        CHECK(!G.Values[I]);
    D.Poisoned=1;CHECK(MaliJmSetManagedInterrupts(&D,0)==MaliBusy);D.Poisoned=0;
    D.BoundSpaces=1;CHECK(MaliJmSetManagedInterrupts(&D,0)==MaliBusy);D.BoundSpaces=0;
    D.Jobs[0].Active=1;CHECK(MaliJmSetManagedInterrupts(&D,0)==MaliBusy);D.Jobs[0].Active=0;
    CHECK(MaliJmSetManagedInterrupts(&D,0)==MaliOk && MaliJmStart(&D)==MaliOk);
    CHECK(G.Reg[0x28/4]==0x81 && G.Reg[0x1008/4]==0x70007 && G.Reg[0x2008/4]==0xff00ff);
}
int main(void) {PteTests();DeviceTests();ManagedInterruptTests();printf("Mali JM core: %u checks passed (synthetic MMIO/page tables; no GPU)\n",Checks);return 0;}
