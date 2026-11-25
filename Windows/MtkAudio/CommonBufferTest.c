// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeCommonBuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
static unsigned Checks,Irql,Allocations,Frees;static uint64_t Address;static void *Returned;
static DMA_ADAPTER Adapter;static unsigned char Storage[196608];static ULONG ExpectedLength=4096;
#define C(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
unsigned char KeGetCurrentIrql(void){return Irql;}
static void Free(PDMA_ADAPTER A,ULONG L,PHYSICAL_ADDRESS P,void *V,BOOLEAN Cache){C(A==&Adapter&&L==ExpectedLength&&P.QuadPart==(int64_t)Address&&V==Returned&&!Cache);Frees++;}
static void *Allocate(PDMA_ADAPTER A,PHYSICAL_ADDRESS *Max,ULONG L,PHYSICAL_ADDRESS *P,BOOLEAN Cache,ULONG Node){C(A==&Adapter&&Max->QuadPart==0x3ffffffff&&L==ExpectedLength&&!Cache&&Node==0);Allocations++;P->QuadPart=(int64_t)Address;return Returned;}
static NTSTATUS Acquire(T70_AFE_COMMON_BUFFER *O){return T70AfeCommonBufferAcquire(O,&Adapter,48000,1,2,4096,1024);}
int main(void){DMA_OPERATIONS Ops={sizeof(Ops),Free,Allocate,NULL,NULL};Adapter=(DMA_ADAPTER){1,sizeof(Adapter),&Ops};
 uint64_t Bases[]={0,0x100000000,0x3fffff000,0x3fffff001,0x400000000,UINT64_MAX};
 for(unsigned I=0;I<6;I++){T70_AFE_COMMON_BUFFER O={0},Old=O;Address=Bases[I];Returned=Storage;Allocations=Frees=0;
  NTSTATUS S=Acquire(&O);C(Allocations==1);if(I>=3){C(S==STATUS_INVALID_DEVICE_STATE&&Frees==1);C(!memcmp(&O,&Old,sizeof O));continue;}
  C(S==0&&Frees==0&&O.Prepared.DmaBase==Address&&O.VirtualAddress==Storage);
  C(T70AfeCommonBufferBegin(&O)==0);C(T70AfeCommonBufferBegin(&O)==0);C(T70AfeCommonBufferRelease(&O)==STATUS_INVALID_DEVICE_STATE);
  C(T70AfeCommonBufferEnd(&O)==0);C(T70AfeCommonBufferRelease(&O)==STATUS_INVALID_DEVICE_STATE);C(T70AfeCommonBufferEnd(&O)==0);
  C(T70AfeCommonBufferEnd(&O)==STATUS_INVALID_DEVICE_STATE);C(T70AfeCommonBufferRelease(&O)==0&&Frees==1);C(T70AfeCommonBufferRelease(&O)==0&&Frees==1);
 }
 T70_AFE_COMMON_BUFFER O={0},Old=O;Address=0;Returned=NULL;Frees=0;C(Acquire(&O)==STATUS_INSUFFICIENT_RESOURCES);C(!Frees&&!memcmp(&O,&Old,sizeof O));
 Returned=&O;C(Acquire(&O)==STATUS_INVALID_DEVICE_STATE&&Frees==1);C(!memcmp(&O,&Old,sizeof O));
 Returned=(void *)(UINTPTR_MAX-1);C(Acquire(&O)==STATUS_INVALID_DEVICE_STATE&&Frees==2);C(!memcmp(&O,&Old,sizeof O));Returned=Storage;
 for(unsigned Bad=0;Bad<5;Bad++){unsigned Before=Allocations;Ops.Size=sizeof Ops;Ops.FreeCommonBuffer=Free;Ops.AllocateCommonBufferEx=Allocate;Adapter.Size=sizeof Adapter;Adapter.DmaOperations=&Ops;
  if(Bad==0)Ops.Size=offsetof(DMA_OPERATIONS,AllocateCommonBufferEx);if(Bad==1)Ops.FreeCommonBuffer=NULL;if(Bad==2)Ops.AllocateCommonBufferEx=NULL;if(Bad==3)Adapter.Size=0;if(Bad==4)Adapter.DmaOperations=NULL;
  C(Acquire(&O)==STATUS_INVALID_PARAMETER);C(Allocations==Before&&!memcmp(&O,&Old,sizeof O));
 }
 Adapter.Size=sizeof Adapter;Adapter.DmaOperations=&Ops;Ops=(DMA_OPERATIONS){sizeof Ops,Free,Allocate,NULL,NULL};
 unsigned Before=Allocations;C(T70AfeCommonBufferAcquire(&O,&Adapter,48000,3,2,4096,1024)==STATUS_INVALID_PARAMETER);C(T70AfeCommonBufferAcquire(&O,&Adapter,48000,1,2,UINT64_MAX,1024)==STATUS_INVALID_PARAMETER);C(Allocations==Before);
 C(Acquire(&O)==0);Old=O;C(Acquire(&O)==STATUS_INVALID_DEVICE_STATE);C(!memcmp(&O,&Old,sizeof O));
 O.ActiveUses=UINT32_MAX;C(T70AfeCommonBufferBegin(&O)==STATUS_INVALID_DEVICE_STATE);O.ActiveUses=0;
 Irql=1;Old=O;C(T70AfeCommonBufferRelease(&O)==STATUS_INVALID_DEVICE_STATE);C(T70AfeCommonBufferBegin(&O)==STATUS_INVALID_DEVICE_STATE);C(T70AfeCommonBufferEnd(&O)==STATUS_INVALID_DEVICE_STATE);C(!memcmp(&O,&Old,sizeof O));Irql=0;C(T70AfeCommonBufferRelease(&O)==0);
 unsigned BeforeInvalid=Allocations;Irql=1;
 C(Acquire(&O)==STATUS_INVALID_DEVICE_STATE);Irql=0;
 C(Acquire(NULL)==STATUS_INVALID_PARAMETER);
 C(T70AfeCommonBufferAcquire(&O,NULL,48000,1,2,4096,1024)==STATUS_INVALID_PARAMETER);
 C(Allocations==BeforeInvalid);
 Returned=NULL;C(Acquire(&O)==STATUS_INSUFFICIENT_RESOURCES);Returned=Storage;
 C(Acquire(&O)==0);C(T70AfeCommonBufferRelease(&O)==0);
 for(unsigned I=0;I<2;I++){
  ExpectedLength=I?196608:192;unsigned F=Frees;
  C(T70AfeCommonBufferAcquire(&O,&Adapter,48000,1,2,ExpectedLength,ExpectedLength/2)==0);
  C(O.Length==ExpectedLength&&O.Prepared.BufferBytes==ExpectedLength);
  C(T70AfeCommonBufferRelease(&O)==0&&Frees==F+1);
 }
 C(T70AfeCommonBufferRelease(NULL)==STATUS_INVALID_PARAMETER);printf("CommonBuffer: %u checks; fake DMA adapter only\n",Checks);return 0;
}
