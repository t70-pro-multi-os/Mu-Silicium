// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtDmaOwner.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
static unsigned Checks,Irql,Creates,Frees;static NTSTATUS Result;
static uint64_t Address;static bool RaiseOnCreate,RaiseOnFree;
static T70_WAVERT_MDL_OWNER *Backing;static PDMA_ADAPTER Expected;
#define C(X) do{Checks++;assert(X);}while(0)
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
static NTSTATUS Create(PDMA_ADAPTER A,PMDL M,DMA_COMMON_BUFFER_EXTENDED_CONFIGURATION *L,ULONG Count,PHYSICAL_ADDRESS *Out){
 C(A==Expected&&M==Backing->Mdl&&Backing->Uses>0);C(Count==1&&L&&L->ConfigType==CommonBufferConfigTypeLogicalAddressLimits);
 C(L->LogicalAddressLimits.MinimumAddress.QuadPart==0&&L->LogicalAddressLimits.MaximumAddress.QuadPart==0x3ffffffffLL);
 Creates++;Out->QuadPart=(int64_t)Address;if(RaiseOnCreate)Irql=2;return Result;
}
static void Free(PDMA_ADAPTER A,ULONG N,PHYSICAL_ADDRESS L,void *Cpu,BOOLEAN Cache){
 C(A==Expected&&N==Backing->Bytes&&(uint64_t)L.QuadPart==Address&&Cpu==Backing->Cpu&&Cache==TRUE);C(Backing->Uses>0);Frees++;if(RaiseOnFree)Irql=2;
}
int main(){
 alignas(PAGE_SIZE) static uint8_t Ram[196608];MDL M={};M.ByteCount=4096;
 T70_WAVERT_MDL_OWNER B={reinterpret_cast<IPortWaveRTStream *>(1),&M,Ram,4096,0};Backing=&B;
 DMA_OPERATIONS Ops={};Ops.Size=sizeof Ops;Ops.FreeCommonBuffer=Free;Ops.CreateCommonBufferFromMdl=Create;
 DMA_ADAPTER A={1,sizeof A,&Ops};Expected=&A;T70_WAVERT_DMA_OWNER O={};
 for(unsigned N=PAGE_SIZE;N<=196608;N+=PAGE_SIZE){
  B.Bytes=N;M.ByteCount=N;
  const uint64_t Values[]={0,16,0x100000000ULL,0x400000000ULL-N,0x3fffffff0ULL,0x400000000ULL,UINT64_MAX,1};
  for(auto V:Values){
   Address=V;Result=STATUS_SUCCESS;unsigned Old=Frees;
   bool Good=V<=0x3ffffffffULL&&V%16==0&&N-1<=0x3ffffffffULL-V;
   NTSTATUS S=T70WaveRtDmaAcquire(&O,&B,&A);
   C(S==(Good?STATUS_SUCCESS:STATUS_DEVICE_DATA_ERROR));
   if(Good){C(O.Mapped&&O.LeaseHeld&&B.Uses==1);C(T70WaveRtMdlRelease(&B)==STATUS_INVALID_DEVICE_STATE);C(T70WaveRtDmaAcquire(&O,&B,&A)==STATUS_INVALID_DEVICE_STATE);C(T70WaveRtDmaRelease(&O)==0);}
   C(Frees==Old+1&&B.Uses==0&&!O.LeaseHeld&&!O.Mapped);C(T70WaveRtDmaRelease(&O)==0);
  }
 }
 B.Bytes=4096;M.ByteCount=4096;
 for(auto S:{STATUS_NOT_SUPPORTED,STATUS_INSUFFICIENT_RESOURCES,STATUS_INVALID_PARAMETER}){
  Result=S;unsigned Old=Frees;C(T70WaveRtDmaAcquire(&O,&B,&A)==S);C(Frees==Old&&B.Uses==0&&!O.LeaseHeld);
 }
 Result=0;Address=0x100000000ULL;
 unsigned Old=Creates;
 B.Uses=1;C(T70WaveRtDmaAcquire(&O,&B,&A)==STATUS_INVALID_DEVICE_STATE);C(Creates==Old);B.Uses=0;
 Ops.Size=offsetof(DMA_OPERATIONS,CreateCommonBufferFromMdl);C(T70WaveRtDmaAcquire(&O,&B,&A)==STATUS_NOT_SUPPORTED);Ops.Size=sizeof Ops;
 Ops.CreateCommonBufferFromMdl=nullptr;C(T70WaveRtDmaAcquire(&O,&B,&A)==STATUS_NOT_SUPPORTED);Ops.CreateCommonBufferFromMdl=Create;
 Ops.FreeCommonBuffer=nullptr;C(T70WaveRtDmaAcquire(&O,&B,&A)==STATUS_NOT_SUPPORTED);Ops.FreeCommonBuffer=Free;
 C(Creates==Old&&B.Uses==0);
 M.ByteOffset=1;C(T70WaveRtDmaAcquire(&O,&B,&A)==STATUS_INVALID_PARAMETER);C(Creates==Old&&B.Uses==0);M.ByteOffset=0;
 M.Next=&M;C(T70WaveRtDmaAcquire(&O,&B,&A)==STATUS_INVALID_PARAMETER);C(Creates==Old&&B.Uses==0);M.Next=nullptr;
 M.ByteCount=4095;C(T70WaveRtDmaAcquire(&O,&B,&A)==STATUS_INVALID_PARAMETER);C(Creates==Old&&B.Uses==0);M.ByteCount=4096;
 A.Size=0;C(T70WaveRtDmaAcquire(&O,&B,&A)==STATUS_NOT_SUPPORTED);A.Size=sizeof A;
 C(Creates==Old&&B.Uses==0);
 // A failing provider followed by temporary cleanup unavailability retains lease.
 RaiseOnCreate=true;Result=STATUS_INSUFFICIENT_RESOURCES;
 C(T70WaveRtDmaAcquire(&O,&B,&A)==STATUS_INVALID_PARAMETER);C(O.LeaseHeld&&!O.Mapped&&B.Uses==1);
 Irql=0;RaiseOnCreate=false;C(T70WaveRtDmaRelease(&O)==0);C(B.Uses==0);
 // Logical mapping removal succeeded, but End cannot run until PASSIVE again.
 Result=0;C(T70WaveRtDmaAcquire(&O,&B,&A)==0);RaiseOnFree=true;Old=Frees;
 C(T70WaveRtDmaRelease(&O)==STATUS_INVALID_PARAMETER);C(O.LeaseHeld&&!O.Mapped&&B.Uses==1&&Frees==Old+1);
 Irql=0;RaiseOnFree=false;C(T70WaveRtDmaRelease(&O)==0);C(Frees==Old+1&&B.Uses==0);
 C(T70WaveRtDmaAcquire(nullptr,&B,&A)==STATUS_INVALID_PARAMETER);C(T70WaveRtDmaRelease(nullptr)==STATUS_INVALID_PARAMETER);
 printf("WaveRT DMA MDL: %u checks; fake mapping only\n",Checks);
}
