// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "DmaAdapterOwner.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
static unsigned Checks,Irql,PdoRefs,PutCalls,GetCalls;static bool FailGet;
static DMA_OPERATIONS Ops;static DMA_ADAPTER Adapter;
static PDEVICE_OBJECT Pdo=(PDEVICE_OBJECT)(uintptr_t)0x1234;
#define C(X) do{++Checks;assert(X);}while(0)
unsigned char KeGetCurrentIrql(){return (unsigned char)Irql;}
void *ExAllocatePool2(uint64_t,size_t N,ULONG){return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG){free(P);}
LONG InterlockedIncrement(volatile LONG *P){return ++*P;}
LONG InterlockedDecrement(volatile LONG *P){return --*P;}
void ObReferenceObject(void *P){C(P==Pdo);++PdoRefs;}
void ObDereferenceObject(void *P){C(P==Pdo&&PdoRefs==1);--PdoRefs;}
static void Put(PDMA_ADAPTER A){C(A==&Adapter&&PdoRefs==1);++PutCalls;}
PDMA_ADAPTER IoGetDmaAdapter(PDEVICE_OBJECT P,DEVICE_DESCRIPTION *D,PULONG N){
 C(P==Pdo&&PdoRefs==1);++GetCalls;
 C(D->Version==DEVICE_DESCRIPTION_VERSION3&&D->DmaAddressWidth==34&&D->Master&&!D->ScatterGather&&D->InterfaceType==InterfaceTypeUndefined);
 *N=7;return FailGet?nullptr:&Adapter;
}
class Parent:public IUnknown{public:ULONG Refs=1;ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}};
static NTSTATUS Init(void *,PUNKNOWN,PRESOURCELIST,PPORTWAVERT){return STATUS_SUCCESS;}
static NTSTATUS Stream(void *,PPORTWAVERTSTREAM,ULONG,PKSDATAFORMAT,PMINIPORTWAVERTSTREAM *){return STATUS_DEVICE_NOT_READY;}
static NTSTATUS Confirm(void *P){return *(NTSTATUS *)P;}
static unsigned Maps,Unmaps;static NTSTATUS MapResult;static bool RaiseCreate,RaiseFree;
static T70_WAVERT_MDL_OWNER *Backing;static T70_DMA_ADAPTER_OWNER *AdapterOwner;
static NTSTATUS Map(PDMA_ADAPTER A,PMDL M,DMA_COMMON_BUFFER_EXTENDED_CONFIGURATION *L,ULONG N,PHYSICAL_ADDRESS *Out){
 C(A==&Adapter&&M==Backing->Mdl&&N==1&&L->LogicalAddressLimits.MaximumAddress.QuadPart==0x3ffffffffLL);
 C(AdapterOwner->Users==1&&Backing->Uses==1);++Maps;Out->QuadPart=0x100000000LL;
 if(RaiseCreate)Irql=2;return MapResult;
}
static void Unmap(PDMA_ADAPTER A,ULONG N,PHYSICAL_ADDRESS L,void *Cpu,BOOLEAN Cache){
 C(A==&Adapter&&N==4096&&L.QuadPart==0x100000000LL&&Cpu==Backing->Cpu&&Cache==TRUE);
 C(AdapterOwner->Users==1&&Backing->Uses==1);++Unmaps;if(RaiseFree)Irql=2;
}
int main(){
 Parent ParentOwner;T70_WAVERT_MINIPORT_PROVIDER Provider={nullptr,Init,Stream};T70WaveRtMiniport *M=nullptr;
 C(T70WaveRtMiniport::Create(&ParentOwner,Provider,&M)==0);
 T70_DMA_ADAPTER_OWNER O={};T70_DMA_ADAPTER_USE Use={};PDMA_ADAPTER Out=nullptr;
 C(T70DmaAdapterOpen(&O,M,Pdo)==STATUS_DEVICE_NOT_READY&&GetCalls==0&&!O.Adapter);
 unsigned char Fake[2]={};C(M->Init(nullptr,(PRESOURCELIST)&Fake[0],(PPORTWAVERT)&Fake[1])==0);
 Ops.Size=sizeof Ops;Ops.PutDmaAdapter=Put;Adapter={1,sizeof Adapter,&Ops};
 FailGet=true;C(T70DmaAdapterOpen(&O,M,Pdo)==STATUS_INSUFFICIENT_RESOURCES&&!PdoRefs&&!O.Adapter);FailGet=false;
 Irql=2;C(T70DmaAdapterOpen(&O,M,Pdo)==STATUS_INVALID_PARAMETER);Irql=0;
 C(T70DmaAdapterOpen(&O,M,nullptr)==STATUS_INVALID_PARAMETER);
 C(T70DmaAdapterOpen(&O,M,Pdo)==0&&O.MapRegisters==7&&PdoRefs==1);
 unsigned Gets=GetCalls;C(T70DmaAdapterOpen(&O,M,Pdo)==STATUS_INVALID_DEVICE_STATE&&GetCalls==Gets);
 // Miniport's last caller reference may go away; adapter retains it and owner.
 C(M->Release()==1&&ParentOwner.Refs==2);
 O.Users=UINT32_MAX;C(T70DmaAdapterBegin(&O,&Use,&Out)==STATUS_INVALID_DEVICE_STATE&&!Use.Owner&&!Out);O.Users=0;
 C(T70DmaAdapterBegin(&O,&Use,&Out)==0&&Out==&Adapter&&O.Users==1);
 T70_DMA_ADAPTER_USE Nested={};PDMA_ADAPTER NestedOut=nullptr;
 C(T70DmaAdapterBegin(&O,&Nested,&NestedOut)==0&&O.Users==2&&NestedOut==&Adapter);
 Irql=2;C(T70DmaAdapterClose(&O,nullptr,nullptr)==STATUS_INVALID_PARAMETER&&!O.Closing&&O.Users==2);
 C(T70DmaAdapterEnd(&O,&Nested)==STATUS_INVALID_PARAMETER&&Nested.Owner==&O&&O.Users==2);Irql=0;
 C(T70DmaAdapterEnd(&O,&Nested)==0&&O.Users==1);
 C(T70DmaAdapterBegin(&O,&Use,&Out)==STATUS_INVALID_DEVICE_STATE);
 NTSTATUS Proof=STATUS_SUCCESS;
 C(T70DmaAdapterClose(&O,Confirm,&Proof)==STATUS_INVALID_DEVICE_STATE&&!PutCalls&&O.Closing);
 T70_DMA_ADAPTER_USE Other={};Out=nullptr;
 C(T70DmaAdapterBegin(&O,&Other,&Out)==STATUS_INVALID_DEVICE_STATE&&!Out);
 C(T70DmaAdapterEnd(&O,&Other)==STATUS_INVALID_DEVICE_STATE);
 C(T70DmaAdapterEnd(&O,&Use)==0&&!Use.Owner);
 C(T70DmaAdapterEnd(&O,&Use)==STATUS_INVALID_DEVICE_STATE);
 C(T70DmaAdapterClose(&O,nullptr,nullptr)==STATUS_DEVICE_NOT_READY&&!PutCalls);
 Proof=0x103;C(T70DmaAdapterClose(&O,Confirm,&Proof)==STATUS_DEVICE_NOT_READY&&!PutCalls);
 Proof=STATUS_IO_TIMEOUT;C(T70DmaAdapterClose(&O,Confirm,&Proof)==Proof&&!PutCalls&&PdoRefs==1);
 Proof=0;C(T70DmaAdapterClose(&O,Confirm,&Proof)==0&&PutCalls==1&&!PdoRefs&&ParentOwner.Refs==1);
 C(T70DmaAdapterClose(&O,nullptr,nullptr)==0&&PutCalls==1);
 // Actual adapter lease -> existing MDL-to-DMA mapping -> reverse release.
 C(T70WaveRtMiniport::Create(&ParentOwner,Provider,&M)==0);C(M->Init(nullptr,(PRESOURCELIST)&Fake[0],(PPORTWAVERT)&Fake[1])==0);
 C(T70DmaAdapterOpen(&O,M,Pdo)==0);AdapterOwner=&O;
 alignas(PAGE_SIZE) static unsigned char Ram[PAGE_SIZE];MDL Mdl={};Mdl.ByteCount=PAGE_SIZE;
 T70_WAVERT_MDL_OWNER B={reinterpret_cast<IPortWaveRTStream *>(1),&Mdl,Ram,PAGE_SIZE,0};Backing=&B;
 T70_WAVERT_OWNED_DMA Bound={};Ops.FreeCommonBuffer=Unmap;Ops.CreateCommonBufferFromMdl=Map;
 MapResult=STATUS_INSUFFICIENT_RESOURCES;
 C(T70WaveRtOwnedDmaAcquire(&Bound,&B,&O)==MapResult&&!O.Users&&!B.Uses&&!Bound.AdapterUse.Owner&&!Unmaps);
 RaiseCreate=true;
 C(T70WaveRtOwnedDmaAcquire(&Bound,&B,&O)==STATUS_INVALID_PARAMETER&&O.Users==1&&B.Uses==1&&Bound.Mapping.LeaseHeld);
 Irql=0;RaiseCreate=false;C(T70DmaAdapterClose(&O,Confirm,&Proof)==STATUS_INVALID_DEVICE_STATE);
 C(T70WaveRtOwnedDmaRelease(&Bound)==0&&!O.Users&&!B.Uses);
 // Closing stays closed; no new mapping may slip in while recovery drains.
 unsigned BeforeMaps=Maps;C(T70WaveRtOwnedDmaAcquire(&Bound,&B,&O)==STATUS_INVALID_DEVICE_STATE&&Maps==BeforeMaps);
 C(T70DmaAdapterClose(&O,Confirm,&Proof)==0);C(T70DmaAdapterOpen(&O,M,Pdo)==0);
 MapResult=STATUS_SUCCESS;C(T70WaveRtOwnedDmaAcquire(&Bound,&B,&O)==0&&O.Users==1&&B.Uses==1&&Bound.Mapping.Mapped);
 C(T70WaveRtOwnedDmaAcquire(&Bound,&B,&O)==STATUS_INVALID_DEVICE_STATE);
 C(T70WaveRtMdlRelease(&B)==STATUS_INVALID_DEVICE_STATE);
 C(T70WaveRtDmaBeginUse(&Bound.Mapping)==0);
 unsigned BeforeUnmaps=Unmaps;
 C(T70WaveRtOwnedDmaRelease(&Bound)==STATUS_INVALID_DEVICE_STATE&&Unmaps==BeforeUnmaps&&O.Users==1);
 C(T70DmaAdapterClose(&O,Confirm,&Proof)==STATUS_INVALID_DEVICE_STATE);
 // Host-only recovery proof; no real hardware was configured in this test.
 C(T70WaveRtDmaEndUse(&Bound.Mapping)==0);RaiseFree=true;
 C(T70WaveRtOwnedDmaRelease(&Bound)==STATUS_INVALID_PARAMETER&&Unmaps==BeforeUnmaps+1&&O.Users==1&&B.Uses==1&&!Bound.Mapping.Mapped);
 Irql=0;RaiseFree=false;C(T70WaveRtOwnedDmaRelease(&Bound)==0&&Unmaps==BeforeUnmaps+1&&!O.Users&&!B.Uses);
 C(T70WaveRtOwnedDmaRelease(&Bound)==0);
 C(T70DmaAdapterClose(&O,Confirm,&Proof)==0);C(M->Release()==0&&ParentOwner.Refs==1);
 unsigned PriorPuts=PutCalls;
 // Malformed provider table quarantines ownership, never silently leaks it.
 C(T70WaveRtMiniport::Create(&ParentOwner,Provider,&M)==0);C(M->Init(nullptr,(PRESOURCELIST)&Fake[0],(PPORTWAVERT)&Fake[1])==0);
 Ops.PutDmaAdapter=nullptr;C(T70DmaAdapterOpen(&O,M,Pdo)==STATUS_DEVICE_DATA_ERROR&&O.Adapter==&Adapter&&O.Closing&&PdoRefs==1);
 C(T70DmaAdapterClose(&O,Confirm,&Proof)==STATUS_DEVICE_DATA_ERROR&&PutCalls==PriorPuts);
 Ops.PutDmaAdapter=Put;C(T70DmaAdapterClose(&O,Confirm,&Proof)==0&&PutCalls==PriorPuts+1&&!PdoRefs);
 C(M->Release()==0&&ParentOwner.Refs==1);
 for(unsigned Malformed=0;Malformed<3;Malformed++){
  C(T70WaveRtMiniport::Create(&ParentOwner,Provider,&M)==0);C(M->Init(nullptr,(PRESOURCELIST)&Fake[0],(PPORTWAVERT)&Fake[1])==0);
  if(Malformed==0)Adapter.Size=0;
  if(Malformed==1)Adapter.DmaOperations=nullptr;
  if(Malformed==2)Ops.Size=offsetof(DMA_OPERATIONS,PutDmaAdapter);
  C(T70DmaAdapterOpen(&O,M,Pdo)==STATUS_DEVICE_DATA_ERROR&&O.Closing&&PdoRefs==1);
  unsigned Puts=PutCalls;C(T70DmaAdapterClose(&O,Confirm,&Proof)==STATUS_DEVICE_DATA_ERROR&&PutCalls==Puts);
  // Fake provider repair only; no real damaged-table recovery promise.
  Adapter.Size=sizeof Adapter;Adapter.DmaOperations=&Ops;Ops.Size=sizeof Ops;
  C(T70DmaAdapterClose(&O,Confirm,&Proof)==0&&PutCalls==Puts+1&&!PdoRefs);
  C(M->Release()==0&&ParentOwner.Refs==1);
 }
 printf("DMA adapter owner: %u checks; fake OS only\n",Checks);
}
