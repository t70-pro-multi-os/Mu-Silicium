// SPDX-License-Identifier: BSD-2-Clause-Patent
/* Execute production WDM DMA lifetime callbacks against a host-only DMA adapter. */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include "DpmaifWdmDma.c"
static DMA_ADAPTER AdapterObject;
static DMA_OPERATIONS AdapterOps;
static KIRQL CurrentIrql;
static unsigned AdapterGets,AdapterPuts,MapCalls,PutCalls,CompletionCalls,NullCompletions,PutIrql;
static ULONG DmaWidth=36U;
static volatile LONG ReleaseReady,ReleaseGo;
static NTSTATUS ReleaseStatus[2];
static T70_DPMAIF_WDM_DMA *DmaForRelease;
static PDRIVER_LIST_CONTROL SavedCompletion;
static PVOID SavedContext,ExpectedUserContext;
static SCATTER_GATHER_LIST List;
static volatile unsigned Checks;
typedef struct _DMA_COMMON_BUFFER_VECTOR { ULONG Elements, Bytes; } DMA_COMMON_BUFFER_VECTOR;
static DMA_COMMON_BUFFER_VECTOR NativeVector;
static _Alignas(128) uint8_t VectorMemory[32][4096];
static unsigned VectorAllocCalls,VectorGetCalls,VectorFreeCalls,VectorMode;
static ULONG VectorElements,VectorElementBytes;
static uint64_t VectorBase;
static NTSTATUS VectorAllocationStatus;
static BOOLEAN VectorReturnNull;
#define CHECK(X) do { __atomic_add_fetch(&Checks,1U,__ATOMIC_SEQ_CST); if (!(X)) { fprintf(stderr,"line %d: %s\n",__LINE__,#X); abort(); } } while(0)
LONG InterlockedCompareExchange(volatile LONG *Target,LONG Exchange,LONG Comparand){LONG Expected=Comparand;__atomic_compare_exchange_n(Target,&Expected,Exchange,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return Expected;}
LONG InterlockedExchange(volatile LONG *Target,LONG Exchange){return __atomic_exchange_n(Target,Exchange,__ATOMIC_SEQ_CST);}
LONG InterlockedIncrement(volatile LONG *Target){return __atomic_add_fetch(Target,1,__ATOMIC_SEQ_CST);}
LONG InterlockedDecrement(volatile LONG *Target){return __atomic_sub_fetch(Target,1,__ATOMIC_SEQ_CST);}
KIRQL KeGetCurrentIrql(void){return CurrentIrql;}
VOID KeRaiseIrql(KIRQL NewIrql,PKIRQL OldIrql){CHECK(NewIrql==DISPATCH_LEVEL&&OldIrql);*OldIrql=CurrentIrql;CurrentIrql=NewIrql;}
VOID KeLowerIrql(KIRQL Irql){CHECK(CurrentIrql==DISPATCH_LEVEL);CurrentIrql=Irql;}
PDMA_ADAPTER IoGetDmaAdapter(PDEVICE_OBJECT Pdo,PDEVICE_DESCRIPTION Description,PULONG MapRegisters){
 CHECK(Pdo&&Description&&Description->Version==DEVICE_DESCRIPTION_VERSION3&&Description->Master&&Description->ScatterGather&&Description->Dma64BitAddresses&&Description->DmaAddressWidth==36&&Description->MaximumLength==65536);
 AdapterGets++;*MapRegisters=7;return &AdapterObject;
}
static VOID PutAdapter(PDMA_ADAPTER A){CHECK(A==&AdapterObject);__atomic_add_fetch(&AdapterPuts,1,__ATOMIC_SEQ_CST);for(unsigned I=0;I<10000;I++)sched_yield();}
static NTSTATUS GetInfo(PDMA_ADAPTER A,PDMA_ADAPTER_INFO Info){CHECK(A==&AdapterObject&&Info->Version==DMA_ADAPTER_INFO_VERSION1);Info->V1.DmaAddressWidth=DmaWidth;return STATUS_SUCCESS;}
static PVOID AllocateCommon(PDMA_ADAPTER A,ULONG Bytes,PHYSICAL_ADDRESS *Logical,BOOLEAN Cache){CHECK(A==&AdapterObject&&Bytes&&Logical);(void)Cache;return NULL;}
static VOID FreeCommon(PDMA_ADAPTER A,ULONG Bytes,PHYSICAL_ADDRESS Logical,PVOID Virtual,BOOLEAN Cache){CHECK(A==&AdapterObject&&Bytes&&Virtual);(void)Logical;(void)Cache;}
static NTSTATUS GetList(PDMA_ADAPTER A,PDEVICE_OBJECT Pdo,PMDL Mdl,PVOID Va,ULONG Bytes,PDRIVER_LIST_CONTROL Complete,PVOID Context,BOOLEAN Write){
 CHECK(A==&AdapterObject&&Pdo&&Mdl&&Va==Mdl->Address&&Bytes==Mdl->Bytes&&Complete&&Context&&Write);
 MapCalls++;SavedCompletion=Complete;SavedContext=Context;return STATUS_SUCCESS;
}
static VOID PutList(PDMA_ADAPTER A,PSCATTER_GATHER_LIST Sg,BOOLEAN Write){CHECK(A==&AdapterObject&&Sg==&List&&Write);PutCalls++;PutIrql=CurrentIrql;}
static void *ReleaseWorker(void *Argument){
  uintptr_t Index=(uintptr_t)Argument;
  __atomic_add_fetch(&ReleaseReady,1,__ATOMIC_SEQ_CST);
  while(!__atomic_load_n(&ReleaseGo,__ATOMIC_SEQ_CST))sched_yield();
  ReleaseStatus[Index]=T70DpmaifWdmDmaRelease(DmaForRelease);
  return NULL;
}
static VOID Completion(PDEVICE_OBJECT Pdo,PIRP Irp,PSCATTER_GATHER_LIST Sg,PVOID Context){
 CHECK(Pdo&&Irp&&Context==ExpectedUserContext);CompletionCalls++;
 if(!Sg){NullCompletions++;CHECK(((T70_DPMAIF_WDM_MAPPING *)SavedContext)->State==T70DpmaifMappingFailed);}
 else CHECK(Sg==&List&&((T70_DPMAIF_WDM_MAPPING *)Context)->State==T70DpmaifMappingReady);
}
static NTSTATUS AllocateVector(PDMA_ADAPTER A,PHYSICAL_ADDRESS Low,
    PHYSICAL_ADDRESS High,ULONG Cache,ULONG Node,ULONG Flags,ULONG Elements,
    ULONG Bytes,PDMA_COMMON_BUFFER_VECTOR *Out){
 CHECK(A==&AdapterObject&&Low.QuadPart==0&&
       (uint64_t)High.QuadPart==((uint64_t)1<<36)-1U&&Cache==MmWriteCombined&&
       Node==MM_ANY_NODE_OK&&Flags==0&&Out);
 VectorAllocCalls++;VectorElements=Elements;VectorElementBytes=Bytes;
 if(VectorAllocationStatus!=STATUS_SUCCESS)return VectorAllocationStatus;
 if(VectorReturnNull){*Out=NULL;return STATUS_SUCCESS;}
 NativeVector.Elements=Elements;NativeVector.Bytes=Bytes;*Out=&NativeVector;
 return STATUS_SUCCESS;
}
static VOID GetVectorElement(PDMA_ADAPTER A,PDMA_COMMON_BUFFER_VECTOR V,
    ULONG Index,PVOID *Virtual,PHYSICAL_ADDRESS *Logical){
 uint64_t Address;
 CHECK(A==&AdapterObject&&V==&NativeVector&&Index<NativeVector.Elements&&Virtual&&Logical);
 VectorGetCalls++;
 *Virtual=VectorMemory[Index];
 Address=VectorBase+(uint64_t)Index*((VectorElementBytes+127U)&~127U);
 if(VectorMode==1U)Address++;
 if(VectorMode==2U)Address=((uint64_t)1<<36)-64U;
 if(VectorMode==3U)*Virtual=NULL;
 Logical->QuadPart=(LONGLONG)Address;
}
static VOID FreeVector(PDMA_ADAPTER A,PDMA_COMMON_BUFFER_VECTOR V){
 CHECK(A==&AdapterObject&&V==&NativeVector&&V->Elements==VectorElements&&V->Bytes==VectorElementBytes);
 VectorFreeCalls++;RtlZeroMemory(V,sizeof(*V));
}
static VOID Reset(void){
 RtlZeroMemory(&AdapterOps,sizeof(AdapterOps));AdapterOps.Size=sizeof(AdapterOps);
 AdapterOps.PutDmaAdapter=PutAdapter;AdapterOps.GetDmaAdapterInfo=GetInfo;
 AdapterOps.AllocateCommonBuffer=AllocateCommon;AdapterOps.FreeCommonBuffer=FreeCommon;
 AdapterOps.GetScatterGatherList=GetList;AdapterOps.PutScatterGatherList=PutList;
 AdapterOps.AllocateCommonBufferVector=AllocateVector;
 AdapterOps.GetCommonBufferFromVectorByIndex=GetVectorElement;
 AdapterOps.FreeCommonBufferVector=FreeVector;
 AdapterObject.DmaOperations=&AdapterOps;AdapterGets=AdapterPuts=MapCalls=PutCalls=CompletionCalls=NullCompletions=PutIrql=0;DmaWidth=36U;
 VectorAllocCalls=VectorGetCalls=VectorFreeCalls=VectorMode=0;
 VectorElements=VectorElementBytes=0;VectorBase=0x20000U;
 VectorAllocationStatus=STATUS_SUCCESS;VectorReturnNull=FALSE;RtlZeroMemory(&NativeVector,sizeof(NativeVector));
 ReleaseReady=ReleaseGo=0;ReleaseStatus[0]=ReleaseStatus[1]=STATUS_SUCCESS;
 SavedCompletion=NULL;SavedContext=NULL;CurrentIrql=0;List.NumberOfElements=1;List.Elements[0].Address.QuadPart=0x10000;List.Elements[0].Length=128;
}
static void TestCommonBufferVectors(void){
 T70_DPMAIF_WDM_DMA Dma=T70_DPMAIF_WDM_DMA_INITIALIZER;
 T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR Vector=T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR_INITIALIZER;
 T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR Other=T70_DPMAIF_WDM_COMMON_BUFFER_VECTOR_INITIALIZER;
 DEVICE_OBJECT Pdo={0};PVOID Virtual=(PVOID)(uintptr_t)0x55;PHYSICAL_ADDRESS Logical={0x66};
 Reset();
 CHECK(T70DpmaifWdmDmaInitialize(&Dma,&Pdo,65536)==STATUS_SUCCESS);
 CHECK(T70DpmaifWdmAllocateCommonBufferVector(&Dma,4,128,128,&Vector)==STATUS_SUCCESS);
 CHECK(Vector.Ready&&Vector.Owner==&Dma&&Vector.Vector==&NativeVector&&
       Vector.NumberOfElements==4&&Vector.ElementBytes==128&&Vector.ElementAlignment==128&&
       VectorAllocCalls==1&&VectorElements==4&&VectorElementBytes==128&&Dma.Closing==1);
 CHECK(T70DpmaifWdmDmaRelease(&Dma)==STATUS_DEVICE_BUSY&&AdapterPuts==0);
 CHECK(T70DpmaifWdmGetCommonBufferVectorElement(&Vector,2,&Virtual,&Logical)==STATUS_SUCCESS);
 CHECK(Virtual==VectorMemory[2]&&Logical.QuadPart==0x20100&&VectorGetCalls==1);
 Virtual=(PVOID)(uintptr_t)0x55;Logical.QuadPart=0x66;
 CHECK(T70DpmaifWdmGetCommonBufferVectorElement(&Vector,4,&Virtual,&Logical)==STATUS_INVALID_PARAMETER&&
       Virtual==(PVOID)(uintptr_t)0x55&&Logical.QuadPart==0x66&&VectorGetCalls==1);
 CHECK(T70DpmaifWdmFreeCommonBufferVector(&Dma,&Other)==STATUS_INVALID_PARAMETER);
 VectorMode=1;
 CHECK(T70DpmaifWdmGetCommonBufferVectorElement(&Vector,0,&Virtual,&Logical)==STATUS_DEVICE_CONFIGURATION_ERROR&&
       Virtual==(PVOID)(uintptr_t)0x55&&Logical.QuadPart==0x66);
 VectorMode=2;
 CHECK(T70DpmaifWdmGetCommonBufferVectorElement(&Vector,0,&Virtual,&Logical)==STATUS_DEVICE_CONFIGURATION_ERROR&&
       Virtual==(PVOID)(uintptr_t)0x55&&Logical.QuadPart==0x66);
 VectorMode=3;
 CHECK(T70DpmaifWdmGetCommonBufferVectorElement(&Vector,0,&Virtual,&Logical)==STATUS_DEVICE_CONFIGURATION_ERROR&&
       Virtual==(PVOID)(uintptr_t)0x55&&Logical.QuadPart==0x66);
 VectorMode=0;
 {
  VOID (*SavedFree)(PDMA_ADAPTER,PDMA_COMMON_BUFFER_VECTOR)=AdapterOps.FreeCommonBufferVector;
  AdapterOps.FreeCommonBufferVector=NULL;
  NTSTATUS MissingFree=T70DpmaifWdmFreeCommonBufferVector(&Dma,&Vector);
  CHECK(MissingFree==STATUS_NOT_SUPPORTED&&Vector.Ready&&Dma.Closing==0x40000001L&&VectorFreeCalls==0);
  AdapterOps.FreeCommonBufferVector=SavedFree;
 }
 CHECK(T70DpmaifWdmFreeCommonBufferVector(&Dma,&Vector)==STATUS_SUCCESS&&
       !Vector.Ready&&!Vector.Vector&&!Vector.Owner&&Dma.Closing==0x40000000L&&VectorFreeCalls==1);
 CHECK(T70DpmaifWdmFreeCommonBufferVector(&Dma,&Vector)==STATUS_INVALID_PARAMETER&&VectorFreeCalls==1);
 CHECK(T70DpmaifWdmDmaRelease(&Dma)==STATUS_SUCCESS&&AdapterPuts==1);

 Reset();Dma=(T70_DPMAIF_WDM_DMA)T70_DPMAIF_WDM_DMA_INITIALIZER;
 CHECK(T70DpmaifWdmDmaInitialize(&Dma,&Pdo,65536)==STATUS_SUCCESS);
 VectorAllocationStatus=STATUS_INSUFFICIENT_RESOURCES;
 CHECK(T70DpmaifWdmAllocateCommonBufferVector(&Dma,4,128,128,&Vector)==STATUS_INSUFFICIENT_RESOURCES&&
       !Vector.Ready&&!Vector.Vector&&Dma.Closing==0);
 VectorAllocationStatus=STATUS_SUCCESS;VectorReturnNull=TRUE;
 CHECK(T70DpmaifWdmAllocateCommonBufferVector(&Dma,4,128,128,&Vector)==STATUS_INSUFFICIENT_RESOURCES&&
       !Vector.Ready&&!Vector.Vector&&Dma.Closing==0);
 VectorReturnNull=FALSE;
 CHECK(T70DpmaifWdmAllocateCommonBufferVector(&Dma,0,128,128,&Vector)==STATUS_INVALID_PARAMETER&&VectorAllocCalls==2);
 CHECK(T70DpmaifWdmAllocateCommonBufferVector(&Dma,4,128,3,&Vector)==STATUS_INVALID_PARAMETER&&VectorAllocCalls==2);
 {
  NTSTATUS (*SavedAllocate)(PDMA_ADAPTER,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,ULONG,ULONG,ULONG,ULONG,ULONG,PDMA_COMMON_BUFFER_VECTOR*)=AdapterOps.AllocateCommonBufferVector;
  AdapterOps.AllocateCommonBufferVector=NULL;
  CHECK(T70DpmaifWdmAllocateCommonBufferVector(&Dma,4,128,128,&Vector)==STATUS_NOT_SUPPORTED&&Dma.Closing==0);
  AdapterOps.AllocateCommonBufferVector=SavedAllocate;
 }
 CHECK(T70DpmaifWdmDmaRelease(&Dma)==STATUS_SUCCESS&&AdapterPuts==1);
}

int main(void){
 T70_DPMAIF_WDM_DMA Dma=T70_DPMAIF_WDM_DMA_INITIALIZER;
 T70_DPMAIF_WDM_MAPPING Mapping=T70_DPMAIF_WDM_MAPPING_INITIALIZER;
 DEVICE_OBJECT Pdo={0},Target={0};MDL Mdl={(PVOID)0x20000,128};IRP Irp={0};
 Reset();ExpectedUserContext=&Mapping;CHECK(T70DpmaifWdmDmaInitialize(&Dma,&Pdo,65536)==STATUS_SUCCESS&&Dma.Ready&&Dma.MapRegisterCount==7);
 CHECK(T70DpmaifWdmMappingInitialize(&Mapping)==STATUS_SUCCESS);
 CHECK(T70DpmaifWdmGetScatterGatherList(&Dma,&Target,&Mdl,Mdl.Address,Mdl.Bytes,Completion,ExpectedUserContext,TRUE,&Mapping)==STATUS_SUCCESS);
 CHECK(MapCalls==1&&Dma.Closing==1&&CompletionCalls==0);
 CHECK(T70DpmaifWdmDmaRelease(&Dma)==STATUS_DEVICE_BUSY&&AdapterPuts==0);
 CHECK(T70DpmaifWdmDmaRelease(&Dma)==STATUS_DEVICE_BUSY&&AdapterPuts==0);
 SavedCompletion(&Pdo,&Irp,&List,SavedContext);
 CHECK(CompletionCalls==1&&Mapping.State==T70DpmaifMappingReady);
 CHECK(T70DpmaifWdmPutScatterGatherList(&Mapping)==STATUS_SUCCESS&&PutCalls==1&&PutIrql==DISPATCH_LEVEL&&CurrentIrql==0);
 CHECK(T70DpmaifWdmDmaRelease(&Dma)==STATUS_SUCCESS&&AdapterPuts==1&&!Dma.Ready);
 CHECK(T70DpmaifWdmDmaRelease(&Dma)==STATUS_SUCCESS&&AdapterPuts==1);

 Reset();Dma=(T70_DPMAIF_WDM_DMA)T70_DPMAIF_WDM_DMA_INITIALIZER;Mapping=(T70_DPMAIF_WDM_MAPPING)T70_DPMAIF_WDM_MAPPING_INITIALIZER;
 ExpectedUserContext=&Mapping;CHECK(T70DpmaifWdmDmaInitialize(&Dma,&Pdo,65536)==STATUS_SUCCESS);
 CHECK(T70DpmaifWdmMappingInitialize(&Mapping)==STATUS_SUCCESS);
 CHECK(T70DpmaifWdmGetScatterGatherList(&Dma,&Target,&Mdl,Mdl.Address,Mdl.Bytes,Completion,ExpectedUserContext,TRUE,&Mapping)==STATUS_SUCCESS);
 SavedCompletion(&Pdo,&Irp,NULL,SavedContext);
 CHECK(NullCompletions==1&&Mapping.State==T70DpmaifMappingFailed&&Dma.Closing==0);
 CHECK(T70DpmaifWdmDmaRelease(&Dma)==STATUS_SUCCESS&&AdapterPuts==1);

 Reset();Dma=(T70_DPMAIF_WDM_DMA)T70_DPMAIF_WDM_DMA_INITIALIZER;Mapping=(T70_DPMAIF_WDM_MAPPING)T70_DPMAIF_WDM_MAPPING_INITIALIZER;
 ExpectedUserContext=&Mapping;CHECK(T70DpmaifWdmDmaInitialize(&Dma,&Pdo,65536)==STATUS_SUCCESS);
 CHECK(T70DpmaifWdmMappingInitialize(&Mapping)==STATUS_SUCCESS);
 CHECK(T70DpmaifWdmGetScatterGatherList(&Dma,&Target,&Mdl,Mdl.Address,Mdl.Bytes,NULL,ExpectedUserContext,TRUE,&Mapping)==STATUS_INVALID_PARAMETER);
 CHECK(T70DpmaifWdmDmaRelease(&Dma)==STATUS_SUCCESS&&AdapterPuts==1);

 Reset();Dma=(T70_DPMAIF_WDM_DMA)T70_DPMAIF_WDM_DMA_INITIALIZER;DmaWidth=35U;
 CHECK(T70DpmaifWdmDmaInitialize(&Dma,&Pdo,65536)==STATUS_NOT_SUPPORTED&&AdapterPuts==1&&!Dma.Ready);
 TestCommonBufferVectors();
 Reset();Dma=(T70_DPMAIF_WDM_DMA)T70_DPMAIF_WDM_DMA_INITIALIZER;
 CHECK(T70DpmaifWdmDmaInitialize(&Dma,&Pdo,65536)==STATUS_SUCCESS);DmaForRelease=&Dma;
 pthread_t Threads[2];
 for(uintptr_t I=0;I<2;I++)CHECK(pthread_create(&Threads[I],NULL,ReleaseWorker,(void *)I)==0);
 while(__atomic_load_n(&ReleaseReady,__ATOMIC_SEQ_CST)<2)sched_yield();
 __atomic_store_n(&ReleaseGo,1,__ATOMIC_SEQ_CST);
 for(unsigned I=0;I<2;I++)CHECK(pthread_join(Threads[I],NULL)==0);
 CHECK((ReleaseStatus[0]==STATUS_SUCCESS||ReleaseStatus[0]==STATUS_DEVICE_BUSY)&&
       (ReleaseStatus[1]==STATUS_SUCCESS||ReleaseStatus[1]==STATUS_DEVICE_BUSY)&&
       (ReleaseStatus[0]==STATUS_SUCCESS||ReleaseStatus[1]==STATUS_SUCCESS)&&AdapterPuts==1);
 printf("DPMAIF WDM DMA callbacks: vectors, concurrent release, width gate, NULL completion and DISPATCH-level unmap; %u checks passed\n",__atomic_load_n(&Checks,__ATOMIC_SEQ_CST));
 return 0;
}
