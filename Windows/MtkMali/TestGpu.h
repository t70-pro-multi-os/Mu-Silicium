/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Shared synthetic MMIO, never a board-power implementation. CHECK is supplied by each test. */
#ifndef T70_MALI_TEST_GPU_H
#define T70_MALI_TEST_GPU_H
#include <string.h>
typedef struct {
    uint32_t Reg[4096],Offsets[1024],Values[1024],Writes,Reads,Delays,Barriers;
    int Powered,ResetStuck,PowerStuck,AsStuck,CacheStuck;
} GPU;
static uint32_t Read(void *C,uint32_t O) {
    GPU *G=C;CHECK(!(O&3)&&O<0x4000);G->Reads++;
    if(G->AsStuck && O>=0x2400 && (O&63)==0x28)return 1;
    if(O==0x2c || O==0x100c || O==0x200c)return G->Reg[(O-12)/4]&G->Reg[(O-4)/4];
    return G->Reg[O/4];
}
static void Write(void *C,uint32_t O,uint32_t V) {
    GPU *G=C;CHECK(!(O&3)&&O<0x4000);CHECK(G->Writes<1024);
    G->Offsets[G->Writes]=O;G->Values[G->Writes++]=V;G->Reg[O/4]=V;
    if(O==0x24)G->Reg[0x20/4]&=~V;
    if(O==0x1004)G->Reg[0x1000/4]&=~V;
    if(O==0x2004)G->Reg[0x2000/4]&=~V;
    if(O==0x30&&V==1&&!G->ResetStuck) {
        G->Reg[0x20/4]|=256;
        for(unsigned S=0;S<3;S++)G->Reg[(0x1800+S*0x80+0x60)/4]=0;
    }
    if(O==0x30&&V==8&&!G->CacheStuck)G->Reg[0x20/4]|=1u<<17;
    if(!G->PowerStuck) {
        if(O==0x180||O==0x184)G->Reg[(O-0x40)/4]=V;
        if(O==0x190||O==0x194)G->Reg[(O-0x40)/4]=V;
        if(O==0x1a0||O==0x1a4)G->Reg[(O-0x40)/4]=V;
    }
    for(unsigned S=0;S<3;S++)if(O==0x1860+S*0x80 && V==1) {
        G->Reg[O/4]=0;G->Reg[(0x1824+S*0x80)/4]=8;
    }
}
static void Barrier(void *C) { ((GPU*)C)->Barriers++; }
static void Delay(void *C,uint32_t Us) { CHECK(Us==10);((GPU*)C)->Delays++; }
static int Ready(void *C) { return ((GPU*)C)->Powered; }
static void Init(GPU *G,MALI_JM *D) {
    memset(G,0,sizeof(*G));G->Powered=1;G->Reg[0]=0x90930002;
    G->Reg[0x14/4]=0x2830;G->Reg[0x18/4]=0xff;G->Reg[0x1c/4]=7;
    G->Reg[0x100/4]=5;G->Reg[0x110/4]=1;G->Reg[0x120/4]=1;
    MALI_IO Io={G,Read,Write,Barrier,Delay,Ready};CHECK(MaliJmInitialize(D,&Io)==MaliOk);
}
static void Completion(GPU *G,unsigned S,unsigned Status,int Error) {
    G->Reg[(0x1824+S*0x80)/4]=Status;
    G->Reg[0x1000/4]|=1u<<(S+(Error?16:0));
}
#endif
