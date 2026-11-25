// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdFatTarget.h"
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/DiskIo.h>
#include <Protocol/SdMmcPassThru.h>
#include <Library/BaseMemoryLib.h>
static EFI_GUID PathId=EFI_DEVICE_PATH_PROTOCOL_GUID,BlockId=EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID FsId=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,DiskId=EFI_DISK_IO_PROTOCOL_GUID;
static EFI_GUID PassId=EFI_SD_MMC_PASS_THRU_PROTOCOL_GUID,BindingId=EFI_DRIVER_BINDING_PROTOCOL_GUID;
static EFI_GUID ImageId=EFI_LOADED_IMAGE_PROTOCOL_GUID;
/* MsdcDxe stamps Data4[7]=Index: 1 is the external SD controller. */
static EFI_GUID HostId={0xb615f1f5,0x5088,0x43cd,{0x80,0x9c,0xa1,0x6e,0x52,0x48,0x7d,1}};
static EFI_GUID FatId={0x961578fe,0xb6b7,0x44c3,{0xaf,0x35,0x6b,0xc7,5,0xcd,0x2b,0x1f}};
static BOOLEAN Node(CONST EFI_DEVICE_PATH_PROTOCOL *P,UINT8 T,UINT8 S,UINTN N){
 return P&&P->Type==T&&P->SubType==S&&((UINTN)P->Length[0]+((UINTN)P->Length[1]<<8))==N;
}
static BOOLEAN End(CONST VOID *P){return Node(P,END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,4);}
static EFI_STATUS Relation(EFI_BOOT_SERVICES *B,EFI_HANDLE Parent,EFI_GUID *G,EFI_HANDLE Child){
 EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *E=NULL;UINTN N=0,Found=0;EFI_STATUS S;
 S=B->OpenProtocolInformation(Parent,G,&E,&N);if(EFI_ERROR(S))return S;
 if(N&&!E)return EFI_COMPROMISED_DATA;
 for(UINTN I=0;I<N;I++)if((E[I].Attributes&EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER)&&E[I].ControllerHandle==Child&&E[I].OpenCount)Found++;
 if(E)B->FreePool(E);return Found==1?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static BOOLEAN FatOwner(EFI_BOOT_SERVICES *B,EFI_HANDLE Partition){
 EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *E=NULL;UINTN N=0,Owners=0;BOOLEAN Known=FALSE;
 if(EFI_ERROR(B->OpenProtocolInformation(Partition,&DiskId,&E,&N))||(N&&!E))return FALSE;
 for(UINTN I=0;I<N;I++)if(E[I].Attributes&EFI_OPEN_PROTOCOL_BY_DRIVER){
  EFI_DRIVER_BINDING_PROTOCOL *D=NULL;EFI_LOADED_IMAGE_PROTOCOL *L=NULL;Owners++;
  if(!E[I].OpenCount||E[I].ControllerHandle!=Partition||
   EFI_ERROR(B->HandleProtocol(E[I].AgentHandle,&BindingId,(VOID **)&D))||!D||
   D->DriverBindingHandle!=E[I].AgentHandle||!D->ImageHandle||
   EFI_ERROR(B->HandleProtocol(D->ImageHandle,&ImageId,(VOID **)&L))||!L)continue;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *P=(VOID *)L->FilePath;
  if(Node((VOID *)P,MEDIA_DEVICE_PATH,MEDIA_PIWG_FW_FILE_DP,sizeof(*P))&&
     !CompareMem(&P->FvFileName,&FatId,sizeof(FatId))&&End((UINT8 *)P+sizeof(*P)))Known=TRUE;
 }
 if(E)B->FreePool(E);return Owners==1&&Known;
}
EFI_STATUS T70ValidateSelectedSdFatTarget(EFI_BOOT_SERVICES *B,EFI_HANDLE Host,
 EFI_HANDLE Card,EFI_HANDLE Partition,CONST T70_SD_FAT_IDENTITY *Expected,T70_SD_FAT_IDENTITY *Out){
 EFI_DEVICE_PATH_PROTOCOL *H=NULL,*C=NULL,*P=NULL;T70_SD_FAT_IDENTITY R={0};EFI_STATUS S;
 if(!B||!B->HandleProtocol||!B->OpenProtocolInformation||!B->FreePool||!Host||!Card||!Partition||
    Host==Card||Card==Partition||Host==Partition||!Out)return EFI_INVALID_PARAMETER;
 if(EFI_ERROR(B->HandleProtocol(Host,&PathId,(VOID **)&H))||
    EFI_ERROR(B->HandleProtocol(Card,&PathId,(VOID **)&C))||
    EFI_ERROR(B->HandleProtocol(Partition,&PathId,(VOID **)&P)))return EFI_UNSUPPORTED;
 UINTN V=sizeof(VENDOR_DEVICE_PATH),D=sizeof(SD_DEVICE_PATH);
 if(!Node(H,HARDWARE_DEVICE_PATH,HW_VENDOR_DP,V)||CompareMem(&((VENDOR_DEVICE_PATH *)H)->Guid,&HostId,sizeof(HostId))||!End((UINT8 *)H+V)||
    !C||!P||CompareMem(H,C,V)||CompareMem(H,P,V))return EFI_UNSUPPORTED;
 SD_DEVICE_PATH *Sd=(VOID *)((UINT8 *)C+V);
 if(!Node((VOID *)Sd,MESSAGING_DEVICE_PATH,MSG_SD_DP,D)||Sd->SlotNumber!=0||!End((UINT8 *)Sd+D)||CompareMem(C,P,V+D))return EFI_UNSUPPORTED;
 HARDDRIVE_DEVICE_PATH *Hd=(VOID *)((UINT8 *)P+V+D);
 if(!Node((VOID *)Hd,MEDIA_DEVICE_PATH,MEDIA_HARDDRIVE_DP,sizeof(*Hd))||!Hd->PartitionNumber||!Hd->PartitionSize||!End((UINT8 *)Hd+sizeof(*Hd)))return EFI_UNSUPPORTED;
 S=Relation(B,Host,&PassId,Card);if(EFI_ERROR(S))return S;
 S=Relation(B,Card,&DiskId,Partition);if(EFI_ERROR(S))return S;
 if(EFI_ERROR(B->HandleProtocol(Partition,&BlockId,(VOID **)&R.BlockIo))||!R.BlockIo||!R.BlockIo->Media||
    EFI_ERROR(B->HandleProtocol(Partition,&FsId,(VOID **)&R.FileSystem))||!R.FileSystem||!FatOwner(B,Partition))return EFI_UNSUPPORTED;
 EFI_BLOCK_IO_MEDIA *M=R.BlockIo->Media;
 if(!M->LogicalPartition||!M->MediaPresent)return EFI_NO_MEDIA;
 if(M->ReadOnly)return EFI_WRITE_PROTECTED;
 if(!M->BlockSize||M->LastBlock==MAX_UINT64||Hd->PartitionSize!=M->LastBlock+1)return EFI_COMPROMISED_DATA;
 R.Controller=Host;R.Card=Card;R.Partition=Partition;R.MediaId=M->MediaId;R.BlockSize=M->BlockSize;R.LastBlock=M->LastBlock;CopyMem(&R.PartitionPath,Hd,sizeof(*Hd));
 if(Expected&&(Expected->Controller!=Host||Expected->Card!=Card||Expected->Partition!=Partition||
 Expected->BlockIo!=R.BlockIo||Expected->FileSystem!=R.FileSystem||Expected->MediaId!=R.MediaId||
 Expected->BlockSize!=R.BlockSize||Expected->LastBlock!=R.LastBlock||CompareMem(&Expected->PartitionPath,Hd,sizeof(*Hd))))return EFI_MEDIA_CHANGED;
 *Out=R;return EFI_SUCCESS;
}
