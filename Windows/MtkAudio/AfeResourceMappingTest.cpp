// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeResourceMapping.h"
#include "WaveRtMiniportBoundary.h"
#include <cassert>
#include <cstdio>
static unsigned Checks,Irql,Maps,Unmaps;static bool FailMap;
alignas(8) static unsigned char Ram[8192];
#define C(X) do{Checks++;assert(X);}while(0)
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
void *MmMapIoSpaceEx(PHYSICAL_ADDRESS P,size_t N,ULONG Flags){C(P.QuadPart==0x11210000&&N==8192&&Flags==(PAGE_READWRITE|PAGE_NOCACHE));Maps++;return FailMap?nullptr:Ram;}
void MmUnmapIoSpace(void *P,size_t N){C(P==Ram&&N==8192);Unmaps++;}
class Resources:public IResourceList{
public:ULONG Refs=1,Count=2;bool NullRaw=false,NullTranslated=false;unsigned Reads=0;
 CM_PARTIAL_RESOURCE_DESCRIPTOR Raw[2]={},Translated[2]={};
 Resources(){for(unsigned I=0;I<2;I++){Raw[I].Type=CmResourceTypeMemory;Raw[I].u.Memory.Start.QuadPart=0x11210000;Raw[I].u.Memory.Length=8192;Translated[I]=Raw[I];}}
 ULONG AddRef()override{return ++Refs;}ULONG Release()override{if(Maps&&!FailMap)C(Unmaps==Maps);return --Refs;}
 ULONG NumberOfEntriesOfType(CM_RESOURCE_TYPE T)override{C(T==CmResourceTypeMemory&&Refs==2);return Count;}
 PCM_PARTIAL_RESOURCE_DESCRIPTOR FindUntranslatedEntry(CM_RESOURCE_TYPE T,ULONG I)override{C(T==CmResourceTypeMemory&&I==1&&Refs==2);Reads++;return NullRaw?nullptr:&Raw[I];}
 PCM_PARTIAL_RESOURCE_DESCRIPTOR FindTranslatedEntry(CM_RESOURCE_TYPE T,ULONG I)override{C(T==CmResourceTypeMemory&&I==1&&Refs==2);Reads++;return NullTranslated?nullptr:&Translated[I];}
};
class Mini:public T70WaveRtMiniportBoundary{
public:Mini(const T70_WAVERT_MINIPORT_PROVIDER &P):T70WaveRtMiniportBoundary(0,P){}
 NTSTATUS QueryInterface(const GUID &,void **)override{return STATUS_NOT_SUPPORTED;}
 ULONG AddRef()override{return 1;}ULONG Release()override{return 1;}
 NTSTATUS GetDescription(PPCFILTER_DESCRIPTOR *)override{return STATUS_NOT_SUPPORTED;}
 NTSTATUS DataRangeIntersection(ULONG,PKSDATARANGE,PKSDATARANGE,ULONG,void *,ULONG *)override{return STATUS_NOT_SUPPORTED;}
 NTSTATUS GetDeviceDescription(PDEVICE_DESCRIPTION)override{return STATUS_DEVICE_NOT_READY;}
};
static NTSTATUS NoStream(void *,PPORTWAVERTSTREAM,ULONG,PKSDATAFORMAT,PMINIPORTWAVERTSTREAM *){return STATUS_DEVICE_NOT_READY;}
int main(){
 unsigned char PortObject=0;PPORTWAVERT Port=(PPORTWAVERT)&PortObject;
 for(unsigned Case=0;Case<15;Case++){
  Resources R;T70_AFE_RESOURCE_MAPPING O={};O.MemoryIndex=1;Maps=Unmaps=0;FailMap=Case==12;
  if(Case==0)R.Count=1;
  if(Case==1)R.NullRaw=true;if(Case==2)R.NullTranslated=true;
  if(Case==3)R.Raw[1].u.Memory.Start.QuadPart++;
  if(Case==4)R.Translated[1].u.Memory.Start.QuadPart++;
  if(Case==5)R.Raw[1].u.Memory.Length--;
  if(Case==6)R.Translated[1].u.Memory.Length++;
  if(Case==7)R.Raw[1].Type=0;
  if(Case==8)R.Translated[1].Type=0;
  if(Case==9)R.Raw[1].Flags=1;
  if(Case==10)R.Translated[1].Flags=1;
  if(Case==11)O.Afe.Mapping.Length=1;
  T70_WAVERT_MINIPORT_PROVIDER P={&O,T70AfeResourceMappingInitialize,NoStream};Mini M(P);
  NTSTATUS S=M.Init(nullptr,&R,Port);
  if(Case<=12){C(!NT_SUCCESS(S)&&!O.Resources&&R.Refs==1&&Maps==(Case==12?1u:0u)&&!Unmaps);}
  else{
   C(S==0&&O.Resources==&R&&R.Refs==2&&Maps==1);
   C(M.Init(nullptr,&R,Port)==STATUS_INVALID_DEVICE_STATE&&Maps==1);
   PMINIPORTWAVERTSTREAM Stream=(PMINIPORTWAVERTSTREAM)(uintptr_t)0x55;KSDATAFORMAT Format={};
   C(M.NewStream(&Stream,(PPORTWAVERTSTREAM)&PortObject,0,0,&Format)==STATUS_DEVICE_NOT_READY&&Stream==(PMINIPORTWAVERTSTREAM)(uintptr_t)0x55);
   Irql=2;C(T70AfeResourceMappingRelease(&O)==STATUS_INVALID_PARAMETER&&O.Resources==&R&&R.Refs==2&&!Unmaps);Irql=0;
   C(T70AfeMappingBegin(&O.Afe)==0);
   C(T70AfeResourceMappingRelease(&O)==STATUS_INVALID_DEVICE_STATE&&O.Resources==&R&&R.Refs==2&&!Unmaps);
   C(T70AfeMappingEnd(&O.Afe)==0);
   C(T70AfeResourceMappingRelease(&O)==0&&!O.Resources&&R.Refs==1&&Unmaps==1);
   C(T70AfeResourceMappingRelease(&O)==0&&Unmaps==1);
  }
  if(Case==12){
   Maps=Unmaps=0;FailMap=false;
   C(T70AfeResourceMappingInitialize(&O,nullptr,&R,Port)==0&&R.Refs==2&&Maps==1);
   C(T70AfeResourceMappingRelease(&O)==0&&R.Refs==1&&Unmaps==1);
  }
  if(Case==11)C(T70AfeResourceMappingRelease(&O)==STATUS_INVALID_DEVICE_STATE&&O.Afe.Mapping.Length==1);
 }
 Resources R;T70_AFE_RESOURCE_MAPPING O={};O.MemoryIndex=1;Maps=0;FailMap=false;
 Irql=2;C(T70AfeResourceMappingInitialize(&O,nullptr,&R,Port)==STATUS_INVALID_PARAMETER&&R.Refs==1&&!Maps);Irql=0;
 C(T70AfeResourceMappingInitialize(nullptr,nullptr,&R,Port)==STATUS_INVALID_PARAMETER);
 C(T70AfeResourceMappingInitialize(&O,nullptr,nullptr,Port)==STATUS_INVALID_PARAMETER);
 C(T70AfeResourceMappingInitialize(&O,nullptr,&R,nullptr)==STATUS_INVALID_PARAMETER);
 C(T70AfeResourceMappingRelease(nullptr)==STATUS_INVALID_PARAMETER);
 printf("AFE startup resource mapping: %u checks; fake map only\n",Checks);
}
