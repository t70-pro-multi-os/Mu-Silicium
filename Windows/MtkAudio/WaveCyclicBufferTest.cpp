// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveCyclicBuffer.h"
#include <cassert>
#include <initializer_list>

#include <cstring>
#include <cstdio>
static unsigned char Irql;
unsigned char KeGetCurrentIrql(){return Irql;}
struct Channel:IDmaChannel{
 ULONG Refs=1,Free=0,Copies=0,Size=0;NTSTATUS Status=0;bool Bad=false;ULONG Maximum=512;bool WrongSize=false;ULONG Actual=512;unsigned char Ram[512]={};
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 NTSTATUS AllocateBuffer(ULONG N,PHYSICAL_ADDRESS *P)override{assert(!P);Size=N;return Status;}
 void FreeBuffer()override{++Free;}void *SystemAddress()override{return Bad?nullptr:Ram;}
 ULONG AllocatedBufferSize()override{return Actual;}void SetBufferSize(ULONG N)override{Size=N;}
 ULONG MaximumBufferSize()override{return Maximum;}
 ULONG BufferSize()override{return WrongSize?Size-1:Size;}
 void CopyTo(void *D,void *S,ULONG N)override{++Copies;memcpy(D,S,N);}
};
struct Port:IPortWaveCyclic{
 ULONG Refs=1;NTSTATUS Status=0;bool Null=false;Channel C;
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 NTSTATUS Init(void *,void *,PUNKNOWN,PUNKNOWN,PRESOURCELIST)override{return STATUS_NOT_SUPPORTED;}
 NTSTATUS NewMasterDmaChannel(IDmaChannel **Out,PUNKNOWN U,PRESOURCELIST R,ULONG N,BOOLEAN A,BOOLEAN B,DMA_WIDTH,DMA_SPEED)override{
 assert(!U&&!R&&N==196608&&A&&!B);if(Status==0)*Out=Null?nullptr:&C;return Status;
 }
};
int main(){
 for(NTSTATUS S:{STATUS_DEVICE_DATA_ERROR,(NTSTATUS)0x103}){
  Port P;T70_WAVECYCLIC_BUFFER O={};P.Status=S;assert(T70WaveCyclicBufferAcquire(&O,&P,192,4)!=0);assert(P.Refs==1&&!O.Port);
  P.Status=0;P.C.Status=S;assert(T70WaveCyclicBufferAcquire(&O,&P,192,4)!=0);assert(P.C.Refs==0&&P.Refs==1&&!P.C.Free);
 }
 {Port P;T70_WAVECYCLIC_BUFFER O={};P.C.Bad=true;assert(T70WaveCyclicBufferAcquire(&O,&P,192,4)!=0);assert(P.C.Free==1&&P.Refs==1);}
 {Port P;T70_WAVECYCLIC_BUFFER O={};P.Null=true;
 assert(T70WaveCyclicBufferAcquire(&O,&P,192,4)!=0&&P.Refs==1);
 P.Null=false;assert(T70WaveCyclicBufferAcquire(&O,&P,192,4)==0);
 assert(T70WaveCyclicBufferRelease(&O)==0);}
 {Port P;T70_WAVECYCLIC_BUFFER O={};P.C.Actual=191;
 assert(T70WaveCyclicBufferAcquire(&O,&P,192,4)!=0&&P.C.Free==1&&P.Refs==1);}
 for(unsigned Test=0;Test<2;Test++){Port Q;T70_WAVECYCLIC_BUFFER V={};
 if(Test)Q.C.WrongSize=true;else Q.C.Maximum=191;
 assert(T70WaveCyclicBufferAcquire(&V,&Q,192,4)==STATUS_DEVICE_DATA_ERROR&&Q.C.Free==1&&Q.Refs==1&&!V.Channel);}
 Port P;T70_WAVECYCLIC_BUFFER O={};unsigned char S[192];memset(S,0x6a,sizeof(S));
 assert(T70WaveCyclicBufferAcquire(&O,&P,191,4)!=0);
 assert(T70WaveCyclicBufferAcquire(&O,&P,192,4)==0&&P.Refs==2);
 assert(T70WaveCyclicBufferAcquire(&O,&P,192,4)!=0);
 assert(T70WaveCyclicBufferCopy(&O,0,S,192)==0&&!memcmp(S,P.C.Ram,192));
 assert(T70WaveCyclicBufferCopy(&O,188,S,8)!=0);
 assert(T70WaveCyclicBufferCopy(&O,1,S,4)!=0);
 assert(T70WaveCyclicBufferCopy(&O,0,P.C.Ram,4)!=0);
 assert(T70WaveCyclicBufferCopy(&O,0,&O,4)!=0);
 assert(T70WaveCyclicBufferCopy(&O,192,nullptr,0)==0&&P.C.Copies==1);
 assert(P.C.Copies==1&&!memcmp(S,P.C.Ram,192));
 assert(T70WaveCyclicBufferEnd(&O)!=0);
 O.Uses=UINT32_MAX;assert(T70WaveCyclicBufferBegin(&O)!=0&&O.Uses==UINT32_MAX);O.Uses=0;
 assert(T70WaveCyclicBufferBegin(&O)==0);
 assert(T70WaveCyclicBufferRelease(&O)!=0&&!P.C.Free);
 assert(T70WaveCyclicBufferEnd(&O)==0);
 Irql=DISPATCH_LEVEL;assert(T70WaveCyclicBufferRelease(&O)!=0);Irql=0;
 assert(T70WaveCyclicBufferRelease(&O)==0&&P.C.Free==1&&P.C.Refs==0&&P.Refs==1);
 assert(T70WaveCyclicBufferRelease(&O)==0&&P.C.Free==1);
 puts("WaveCyclic OS channel allocation/copy/lifetime tests passed");
}
