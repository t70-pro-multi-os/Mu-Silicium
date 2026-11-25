// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtMdlOwner.h"
#include <cassert>
#include <cstdio>
#include <cstring>
static unsigned Checks,Irql;
#define C(X) do{Checks++;assert(X);}while(0)
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
class Fake final:public IPortWaveRTStream{
public:
 unsigned Refs=1,Alloc=0,Map=0,Unmap=0,Free=0,Fail=0;MDL M={};
 alignas(PAGE_SIZE) unsigned char Ram[196608];
 ULONG AddRef()override{return ++Refs;} ULONG Release()override{C(Refs>1);return --Refs;}
 PMDL AllocateContiguousPagesForMdl(PHYSICAL_ADDRESS L,PHYSICAL_ADDRESS H,size_t N)override{
  C(L.QuadPart==0&&H.QuadPart==MAXLONGLONG);Alloc++;memset(Ram,0xa5,sizeof Ram);
  M={};M.ByteCount=(ULONG)N;if(Fail==1)return nullptr;
  if(Fail==2)M.Next=&M;if(Fail==3)M.ByteOffset=1;if(Fail==4)--M.ByteCount;return &M;
 }
 ULONG GetPhysicalPagesCount(PMDL X)override{C(X==&M);return M.ByteCount/PAGE_SIZE+(Fail==5);}
 void *MapAllocatedPages(PMDL X,MEMORY_CACHING_TYPE T)override{C(X==&M&&T==MmCached);Map++;if(Fail==6)return nullptr;return Ram+(Fail==7);}
 void UnmapAllocatedPages(void *P,PMDL X)override{C(X==&M&&P==Ram+(Fail==7));C(Free<Alloc);Unmap++;}
 void FreePagesFromMdl(PMDL X)override{C(X==&M);Free++;}
};
int main(){
 static Fake P;T70_WAVERT_MDL_OWNER O={};T70_WAVERT_MDL_VIEW V={};
 for(ULONG N=0;N<=196609;N++)if(!N||N%PAGE_SIZE||N>196608)C(T70WaveRtMdlAcquire(&O,&P,N)==STATUS_INVALID_PARAMETER);
 C(P.Alloc==0&&P.Refs==1);C(T70WaveRtMdlAcquire(nullptr,&P,4096)==STATUS_INVALID_PARAMETER);C(T70WaveRtMdlAcquire(&O,nullptr,4096)==STATUS_INVALID_PARAMETER);
 for(unsigned F=1;F<=7;F++){
  P.Fail=F;unsigned OldFree=P.Free,OldUnmap=P.Unmap;
  NTSTATUS S=T70WaveRtMdlAcquire(&O,&P,4096);C(S==(F==1||F==6?STATUS_INSUFFICIENT_RESOURCES:STATUS_DEVICE_DATA_ERROR));
  C(!O.Port&&!O.Mdl&&!O.Cpu&&!O.Bytes&&!O.Uses&&P.Refs==1);
  C(P.Free==OldFree+(F!=1));C(P.Unmap==OldUnmap+(F==7));
 }
 P.Fail=0;
 for(unsigned Pages=1;Pages<=48;Pages++){
  unsigned N=Pages*PAGE_SIZE;C(T70WaveRtMdlAcquire(&O,&P,N)==0);C(P.Refs==2);
  for(unsigned I=0;I<N;I++)C(P.Ram[I]==0);
  if(N<sizeof P.Ram)C(P.Ram[N]==0xa5);
  C(T70WaveRtMdlAcquire(&O,&P,N)==STATUS_INVALID_DEVICE_STATE);
  C(T70WaveRtMdlEnd(&O)==STATUS_INVALID_DEVICE_STATE);
  C(T70WaveRtMdlBegin(&O,&V)==0);C(V.Mdl==&P.M&&V.Cpu==P.Ram&&V.Bytes==N&&V.CpuCache==MmCached);
  C(T70WaveRtMdlRelease(&O)==STATUS_INVALID_DEVICE_STATE);C(P.Refs==2);
  C(T70WaveRtMdlBegin(&O,&V)==0);C(O.Uses==2);C(T70WaveRtMdlEnd(&O)==0);C(T70WaveRtMdlEnd(&O)==0);
  C(T70WaveRtMdlBegin(&O,reinterpret_cast<T70_WAVERT_MDL_VIEW *>(&O))==STATUS_INVALID_PARAMETER);
  C(T70WaveRtMdlBegin(&O,reinterpret_cast<T70_WAVERT_MDL_VIEW *>(P.Ram))==STATUS_INVALID_PARAMETER);C(O.Uses==0);
  O.Uses=UINT32_MAX;C(T70WaveRtMdlBegin(&O,&V)==STATUS_INVALID_DEVICE_STATE);O.Uses=0;
  Irql=2;C(T70WaveRtMdlRelease(&O)==STATUS_INVALID_PARAMETER);C(T70WaveRtMdlBegin(&O,&V)==STATUS_INVALID_PARAMETER);Irql=0;
  C(T70WaveRtMdlRelease(&O)==0);C(P.Refs==1);C(T70WaveRtMdlRelease(&O)==0);
 }
 O.Port=&P;C(T70WaveRtMdlRelease(&O)==STATUS_INVALID_DEVICE_STATE);C(O.Port==&P);
 printf("WaveRT MDL owner: %u checks; fake PortStream pages only\n",Checks);
}
