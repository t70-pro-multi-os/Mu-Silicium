// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioDeviceStart.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
static unsigned Checks,Irql,Fail,Allocations,Frees,Maps,Unmaps,PdoRefs,Puts;
static NTSTATUS PdoStatus;static PDEVICE_OBJECT AssociatedPdo,ExpectedDevice;
static unsigned char Ram[8192];static DMA_OPERATIONS Ops;static DMA_ADAPTER Adapter;
#define C(X) do{Checks++;assert(X);}while(0)
unsigned char KeGetCurrentIrql(){return (unsigned char)Irql;}
void *ExAllocatePool2(uint64_t,size_t N,ULONG){++Allocations;if(Fail==Allocations)return nullptr;return calloc(1,N);}
void ExFreePoolWithTag(void *P,ULONG){++Frees;free(P);}
LONG InterlockedIncrement(volatile LONG *P){return ++*P;}
LONG InterlockedDecrement(volatile LONG *P){return --*P;}
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS P,size_t N,ULONG){C(P.QuadPart==0x11210000&&N==8192);if(Fail==4)return nullptr;Maps++;return Ram;}
void MmUnmapIoSpace(void *P,size_t N){C(P==Ram&&N==8192);Unmaps++;}
void ObReferenceObject(void *){PdoRefs++;}void ObDereferenceObject(void *){C(PdoRefs==1);PdoRefs--;}
PDMA_ADAPTER IoGetDmaAdapter(PDEVICE_OBJECT,DEVICE_DESCRIPTION *D,PULONG N){C(PdoRefs==1&&D->DmaAddressWidth==34);*N=4;return Fail==5?nullptr:&Adapter;}
NTSTATUS PcGetPhysicalDeviceObject(PDEVICE_OBJECT D,PDEVICE_OBJECT *Out){C(D==ExpectedDevice);*Out=AssociatedPdo;return PdoStatus;}
static void Put(PDMA_ADAPTER P){C(P==&Adapter&&PdoRefs==1);Puts++;}
class Resources:public IResourceList{
public:ULONG Refs=1;CM_PARTIAL_RESOURCE_DESCRIPTOR D={};
 Resources(){D.Type=CmResourceTypeMemory;D.u.Memory.Start.QuadPart=0x11210000;D.u.Memory.Length=8192;}
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{return --Refs;}
 ULONG NumberOfEntriesOfType(CM_RESOURCE_TYPE)override{return 1;}
 PCM_PARTIAL_RESOURCE_DESCRIPTOR FindTranslatedEntry(CM_RESOURCE_TYPE,ULONG)override{return &D;}
 PCM_PARTIAL_RESOURCE_DESCRIPTOR FindUntranslatedEntry(CM_RESOURCE_TYPE,ULONG)override{return &D;}
};
class Port:public IPortWaveRT{
public:ULONG Refs=0;PUNKNOWN Held=nullptr;
 ULONG AddRef()override{return ++Refs;}
 ULONG Release()override{C(Refs==1);Refs--;if(Held){Held->Release();Held=nullptr;}return 0;}
 NTSTATUS Init(void *,void *,PUNKNOWN M,PUNKNOWN A,PRESOURCELIST R)override{
  NTSTATUS S=static_cast<IMiniportWaveRT *>(M)->Init(A,R,this);
  if(S==0){M->AddRef();Held=M;}return S;
 }
};
static Port SystemPort;
NTSTATUS PcNewPort(PPORT *Out,const GUID &){if(Fail==3)return STATUS_INSUFFICIENT_RESOURCES;C(!SystemPort.Refs&&!SystemPort.Held);SystemPort.Refs=1;*Out=&SystemPort;return 0;}
static NTSTATUS Confirm(void *P){return *(NTSTATUS *)P;}
int main(){
 unsigned char Args[3]={};Resources R;Ops.Size=sizeof Ops;Ops.PutDmaAdapter=Put;Adapter={1,sizeof Adapter,&Ops};
 ExpectedDevice=(PDEVICE_OBJECT)&Args[0];AssociatedPdo=(PDEVICE_OBJECT)&Args[2];
 for(Fail=1;Fail<=5;Fail++){
  Allocations=Frees=Maps=Unmaps=Puts=0;T70_AUDIO_DEVICE_START O={};
  C(T70AudioDevicePrepare(&O,(PDEVICE_OBJECT)&Args[0],(PIRP)&Args[1],(PDEVICE_OBJECT)&Args[2],&R,0)==STATUS_INSUFFICIENT_RESOURCES);
  C(!O.Resources&&!O.Miniport&&!O.Port&&!O.Prepared&&!PdoRefs&&!SystemPort.Refs&&R.Refs==1&&Maps==Unmaps);
  C(Frees==(Fail==1?0u:Fail==2?1u:2u));
  C(T70AudioDeviceRelease(&O,nullptr,nullptr)==0);
 }
 Fail=0;Allocations=Frees=Maps=Unmaps=Puts=0;T70_AUDIO_DEVICE_START O={};
 C(T70AudioDevicePrepare(&O,(PDEVICE_OBJECT)&Args[0],(PIRP)&Args[1],(PDEVICE_OBJECT)&Args[2],&R,0)==0);
 C(O.Prepared&&PdoRefs==1&&R.Refs==2&&SystemPort.Refs==1);
 C(T70AudioDevicePrepare(&O,(PDEVICE_OBJECT)&Args[0],(PIRP)&Args[1],(PDEVICE_OBJECT)&Args[2],&R,0)==STATUS_INVALID_DEVICE_STATE);
 KSDATAFORMAT F={};PMINIPORTWAVERTSTREAM Stream=(PMINIPORTWAVERTSTREAM)(uintptr_t)0x55;
 C(O.Miniport->NewStream(&Stream,(PPORTWAVERTSTREAM)&Args[0],0,0,&F)==STATUS_DEVICE_NOT_READY&&Stream==(PMINIPORTWAVERTSTREAM)(uintptr_t)0x55);
 T70_DMA_ADAPTER_USE Use={};PDMA_ADAPTER A=nullptr;C(T70DmaAdapterBegin(&O.Dma,&Use,&A)==0);
 NTSTATUS Proof=0;C(T70AudioDeviceRelease(&O,Confirm,&Proof)==STATUS_INVALID_DEVICE_STATE&&!Puts&&!Unmaps);
 C(T70DmaAdapterEnd(&O.Dma,&Use)==0);
 Proof=0x103;C(T70AudioDeviceRelease(&O,Confirm,&Proof)==STATUS_DEVICE_NOT_READY&&!Puts&&!Unmaps&&O.Prepared);
 Proof=0;C(T70AudioDeviceRelease(&O,Confirm,&Proof)==0&&Puts==1&&Maps==Unmaps&&!PdoRefs&&R.Refs==1&&Frees==2&&!SystemPort.Refs);
 C(!O.Prepared&&!O.Resources&&!O.Miniport&&!O.Port);C(T70AudioDeviceRelease(&O,nullptr,nullptr)==0&&Puts==1);
 C(T70AudioDevicePrepare(&O,(PDEVICE_OBJECT)&Args[0],(PIRP)&Args[1],(PDEVICE_OBJECT)&Args[2],&R,0)==0);
 T70_AUDIO_RESOURCE_USE ResourceUse={};T70_AFE_MAPPING_OWNER *Mapping=nullptr;
 C(O.Resources->BeginUse(&ResourceUse,&Mapping)==0);
 unsigned BeforePuts=Puts,BeforeUnmaps=Unmaps;
 Irql=2;C(T70AudioDeviceRelease(&O,Confirm,&Proof)==STATUS_INVALID_PARAMETER&&Puts==BeforePuts);Irql=0;
 // Adapter is closed first, but a resource lease prevents AFE/port destruction.
 C(T70AudioDeviceRelease(&O,Confirm,&Proof)==STATUS_INVALID_DEVICE_STATE&&Puts==BeforePuts+1&&Unmaps==BeforeUnmaps&&O.Port&&O.Miniport&&O.Prepared);
 C(O.Resources->EndUse(&ResourceUse)==0);
 C(T70AudioDeviceRelease(&O,Confirm,&Proof)==0&&Puts==BeforePuts+1&&Unmaps==BeforeUnmaps+1&&!SystemPort.Refs&&R.Refs==1);
 T70_AUDIO_DEVICE_START Empty={};unsigned BeforeAlloc=Allocations,BeforeMaps=Maps;
 for(unsigned Bad=0;Bad<4;Bad++){
  PdoStatus=Bad==0?STATUS_NOT_SUPPORTED:Bad==1?(NTSTATUS)0x103:STATUS_SUCCESS;
  AssociatedPdo=Bad==2?nullptr:Bad==3?(PDEVICE_OBJECT)&Args[0]:(PDEVICE_OBJECT)&Args[2];
  NTSTATUS Expected=Bad==0?STATUS_NOT_SUPPORTED:Bad==1?STATUS_DEVICE_NOT_READY:STATUS_NO_SUCH_DEVICE;
  C(T70AudioDevicePrepare(&Empty,(PDEVICE_OBJECT)&Args[0],(PIRP)&Args[1],(PDEVICE_OBJECT)&Args[2],&R,0)==Expected);
  C(Allocations==BeforeAlloc&&Maps==BeforeMaps&&!Empty.Resources&&!Empty.Miniport&&!Empty.Port&&!PdoRefs);
 }
 printf("Audio device prepare: %u checks; fake OS, no endpoint/start\n",Checks);
}
