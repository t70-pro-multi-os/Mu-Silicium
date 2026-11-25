// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../Applications/DeviceCheck/SdFatTarget.c"
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static UINT8 Hp[24],Cp[29],Pp[71],Fp[24];
static EFI_BLOCK_IO_MEDIA Media;static EFI_BLOCK_IO_PROTOCOL Block;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;static EFI_DRIVER_BINDING_PROTOCOL Binding;
static EFI_LOADED_IMAGE_PROTOCOL Image;static int Missing,Allocations,RelationBad,OwnerBad;
#define H ((EFI_HANDLE)1)
#define C ((EFI_HANDLE)2)
#define P ((EFI_HANDLE)3)
#define A ((EFI_HANDLE)4)
#define I ((EFI_HANDLE)5)
static EFI_STATUS EFIAPI Get(EFI_HANDLE Handle,EFI_GUID *G,VOID **Out){
 *Out=NULL;if(Missing)return EFI_NOT_FOUND;
 if(!CompareMem(G,&PathId,sizeof(*G)))*Out=Handle==H?Hp:Handle==C?Cp:Handle==P?Pp:NULL;
 else if(Handle==P&&!CompareMem(G,&BlockId,sizeof(*G)))*Out=&Block;
 else if(Handle==P&&!CompareMem(G,&FsId,sizeof(*G)))*Out=&Fs;
 else if(Handle==A&&!CompareMem(G,&BindingId,sizeof(*G)))*Out=&Binding;
 else if(Handle==I&&!CompareMem(G,&ImageId,sizeof(*G)))*Out=&Image;
 return *Out?EFI_SUCCESS:EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Info(EFI_HANDLE Handle,EFI_GUID *G,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **Out,UINTN *N){
 // Match the actual producer: SD Bus uses PassThru, PartitionDxe uses DiskIo.
 *N=0;*Out=NULL;
 if(!((Handle==H&&!CompareMem(G,&PassId,sizeof(*G)))||
      ((Handle==C||Handle==P)&&!CompareMem(G,&DiskId,sizeof(*G)))))
  return EFI_UNSUPPORTED;
 *N=1;*Out=calloc(1,sizeof(**Out));assert(*Out);Allocations++;
 (*Out)->OpenCount=1;(*Out)->AgentHandle=A;
 (*Out)->ControllerHandle=Handle==H?C:Handle==C?P:P;
 (*Out)->Attributes=Handle==P?EFI_OPEN_PROTOCOL_BY_DRIVER:EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER;
 if(RelationBad&&Handle!=P)(*Out)->ControllerHandle=NULL;
 if(OwnerBad&&Handle==P)(*Out)->AgentHandle=NULL;
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Free(VOID *P0){assert(Allocations>0);Allocations--;free(P0);return EFI_SUCCESS;}
static void node(UINT8 *P0,unsigned T,unsigned S,unsigned N){P0[0]=T;P0[1]=S;P0[2]=N;P0[3]=N>>8;}
static void fresh(void){
 memset(Hp,0,sizeof Hp);memset(Cp,0,sizeof Cp);memset(Pp,0,sizeof Pp);memset(Fp,0,sizeof Fp);
 node(Hp,HARDWARE_DEVICE_PATH,HW_VENDOR_DP,20);memcpy(Hp+4,&HostId,16);Hp[19]=1; /* Exact MSDC1 producer index, independent of validator constant. */node(Hp+20,127,255,4);
 memcpy(Cp,Hp,20);node(Cp+20,MESSAGING_DEVICE_PATH,MSG_SD_DP,5);node(Cp+25,127,255,4);
 memcpy(Pp,Cp,25);node(Pp+25,MEDIA_DEVICE_PATH,MEDIA_HARDDRIVE_DP,42);node(Pp+67,127,255,4);
 HARDDRIVE_DEVICE_PATH *D=(void *)(Pp+25);D->PartitionNumber=1;D->PartitionSize=100;
 node(Fp,MEDIA_DEVICE_PATH,MEDIA_PIWG_FW_FILE_DP,20);memcpy(Fp+4,&FatId,16);node(Fp+20,127,255,4);
 memset(&Media,0,sizeof Media);Media.MediaPresent=TRUE;Media.LogicalPartition=TRUE;Media.MediaId=7;Media.BlockSize=512;Media.LastBlock=99;Block.Media=&Media;
 Binding.DriverBindingHandle=A;Binding.ImageHandle=I;Image.FilePath=(void *)Fp;
 Missing=RelationBad=OwnerBad=0;assert(!Allocations);
}
int main(void){
 EFI_BOOT_SERVICES B={0};B.HandleProtocol=Get;B.OpenProtocolInformation=Info;B.FreePool=Free;
 T70_SD_FAT_IDENTITY S,O,Before;unsigned Cases=0;
 EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *Entries=NULL;UINTN Count=0;
 assert(Info(H,&DiskId,&Entries,&Count)==EFI_UNSUPPORTED&&!Entries&&!Count);Cases++;
 assert(Info(C,&BlockId,&Entries,&Count)==EFI_UNSUPPORTED&&!Entries&&!Count);Cases++;
 assert(Info(A,&DiskId,&Entries,&Count)==EFI_UNSUPPORTED&&!Entries&&!Count);Cases++;
 fresh();assert(T70ValidateSelectedSdFatTarget(&B,H,C,P,NULL,&S)==EFI_SUCCESS);Cases++;
 assert(T70ValidateSelectedSdFatTarget(&B,H,C,P,&S,&O)==EFI_SUCCESS);Cases++;
 for(unsigned K=0;K<13;K++){
  fresh();memset(&O,0xa5,sizeof O);Before=O;
  switch(K){case 0:Missing=1;break;case 1:RelationBad=1;break;case 2:OwnerBad=1;break;
   case 3:Fp[4]^=1;break;case 4:Media.ReadOnly=TRUE;break;case 5:Media.MediaPresent=FALSE;break;
   case 6:Media.MediaId++;break;case 7:Media.LastBlock--;break;case 8:Cp[24]=1;break;
   case 9:Pp[26]=MEDIA_FILEPATH_DP;break;case 10:Hp[4]^=1;break;case 11:Media.LogicalPartition=FALSE;break;case 12:Hp[19]=0;Cp[19]=0;Pp[19]=0;break;}
  assert(EFI_ERROR(T70ValidateSelectedSdFatTarget(&B,H,C,P,&S,&O)));
  assert(!memcmp(&O,&Before,sizeof O));assert(!Allocations);Cases++;
 }
 printf("SD FAT target: %u live-backend mock cases passed; zero media IO\n",Cases);return 0;
}
