// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included only by the real FAT regular-image host test.
// Synthetic ancestry/driver metadata; SFS/CreateNew are actual FAT publications.
#include "SdFatExport.h"
#ifdef FAT_REGISTRY_EXPORT
#include "ManualExportChoice.h"
#endif
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SdMmcPassThru.h>
static EFI_GUID RegPath=EFI_DEVICE_PATH_PROTOCOL_GUID,RegBlock=EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID RegDisk=EFI_DISK_IO_PROTOCOL_GUID,RegPass=EFI_SD_MMC_PASS_THRU_PROTOCOL_GUID;
static EFI_GUID RegBinding=EFI_DRIVER_BINDING_PROTOCOL_GUID,RegImage=EFI_LOADED_IMAGE_PROTOCOL_GUID;
static EFI_GUID RegHost={0xb615f1f5,0x5088,0x43cd,{0x80,0x9c,0xa1,0x6e,0x52,0x48,0x7d,1}};
static EFI_GUID RegFat={0x961578fe,0xb6b7,0x44c3,{0xaf,0x35,0x6b,0xc7,5,0xcd,0x2b,0x1f}};
#define REG_PART ((EFI_HANDLE)1)
#define REG_HOST ((EFI_HANDLE)2)
#define REG_CARD ((EFI_HANDLE)3)
#define REG_DRIVER ((EFI_HANDLE)4)
#define REG_IMAGE ((EFI_HANDLE)5)
static UINT8 RegHp[24],RegCp[29],RegPp[71],RegFp[24];
static EFI_BLOCK_IO_PROTOCOL *RegBio;
static EFI_DRIVER_BINDING_PROTOCOL RegDriver;
static EFI_LOADED_IMAGE_PROTOCOL RegLoaded;
static T70_SD_FAT_IDENTITY RegSnapshot;
static unsigned RegQueries;
static EFI_STATUS EFIAPI RegGet(EFI_HANDLE H,EFI_GUID *G,VOID **Out){
 RegQueries++;*Out=NULL;
 if(CompareGuid(G,&RegPath))*Out=H==REG_HOST?RegHp:H==REG_CARD?RegCp:H==REG_PART?RegPp:NULL;
 else if(H==REG_PART&&CompareGuid(G,&RegBlock))*Out=RegBio;
 else if(Installed&&H==REG_PART&&CompareGuid(G,&gEfiSimpleFileSystemProtocolGuid))*Out=Fs;
 else if(Installed&&H==REG_PART&&CompareGuid(G,&gEfiFatCreateNewProtocolGuid))*Out=New;
 else if(H==REG_DRIVER&&CompareGuid(G,&RegBinding))*Out=&RegDriver;
 else if(H==REG_IMAGE&&CompareGuid(G,&RegImage))*Out=&RegLoaded;
 return *Out?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI RegInfo(EFI_HANDLE H,EFI_GUID *G,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **Out,UINTN *Count){
 *Out=NULL;*Count=0;RegQueries++;
 if(!((H==REG_HOST&&CompareGuid(G,&RegPass))||
      ((H==REG_CARD||H==REG_PART)&&CompareGuid(G,&RegDisk))))return EFI_UNSUPPORTED;
 if(H==REG_PART&&!Installed)return EFI_NOT_FOUND;
 *Out=AllocateZeroPool(sizeof(**Out));*Count=1;
 (*Out)->AgentHandle=REG_DRIVER;(*Out)->OpenCount=1;
 (*Out)->ControllerHandle=H==REG_HOST?REG_CARD:REG_PART;
 (*Out)->Attributes=H==REG_PART?EFI_OPEN_PROTOCOL_BY_DRIVER:EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER;
 return 0;
}
static EFI_STATUS EFIAPI RegLocate(EFI_LOCATE_SEARCH_TYPE T,EFI_GUID *G,VOID *Key,UINTN *Count,EFI_HANDLE **Out){
 assert(T==ByProtocol&&!Key);*Count=0;*Out=NULL;RegQueries++;
 EFI_HANDLE H=CompareGuid(G,&RegPass)?REG_HOST:CompareGuid(G,&RegBlock)?REG_CARD:
  CompareGuid(G,&gEfiSimpleFileSystemProtocolGuid)&&Installed?REG_PART:NULL;
 if(!H)return EFI_NOT_FOUND;
 *Out=AllocateZeroPool(sizeof(**Out));**Out=H;*Count=1;return 0;
}
static EFI_STATUS EFIAPI RegFree(VOID *P){FreePool(P);return 0;}
static void RegNode(UINT8 *P,UINT8 T,UINT8 S,UINT8 Size){P[0]=T;P[1]=S;P[2]=Size;P[3]=0;}
static void RegistryBefore(EFI_BOOT_SERVICES *B,EFI_BLOCK_IO_PROTOCOL *Bio){
 assert(Installed&&Fs&&New);RegBio=Bio;Bio->Media->LogicalPartition=TRUE;
 RegNode(RegHp,HARDWARE_DEVICE_PATH,HW_VENDOR_DP,20);memcpy(RegHp+4,&RegHost,16);RegNode(RegHp+20,127,255,4);
 memcpy(RegCp,RegHp,20);RegNode(RegCp+20,MESSAGING_DEVICE_PATH,MSG_SD_DP,5);RegNode(RegCp+25,127,255,4);
 memcpy(RegPp,RegCp,25);RegNode(RegPp+25,MEDIA_DEVICE_PATH,MEDIA_HARDDRIVE_DP,42);RegNode(RegPp+67,127,255,4);
 HARDDRIVE_DEVICE_PATH *Hd=(VOID *)(RegPp+25);Hd->PartitionNumber=1;Hd->PartitionSize=Bio->Media->LastBlock+1;
 RegNode(RegFp,MEDIA_DEVICE_PATH,MEDIA_PIWG_FW_FILE_DP,20);memcpy(RegFp+4,&RegFat,16);RegNode(RegFp+20,127,255,4);
 RegDriver.DriverBindingHandle=REG_DRIVER;RegDriver.ImageHandle=REG_IMAGE;RegLoaded.FilePath=(VOID *)RegFp;
 B->HandleProtocol=RegGet;B->OpenProtocolInformation=RegInfo;B->LocateHandleBuffer=RegLocate;B->FreePool=RegFree;
 int OldWrites=Writes,OldReads=Reads,OldFlushes=Flushes,OldAllocations=Allocations;UINTN Count=0;
 assert(T70DiscoverSdFatTargets(B,&RegSnapshot,1,&Count)==0&&Count==1);
 assert(RegSnapshot.FileSystem==Fs&&RegSnapshot.BlockIo==Bio&&RegSnapshot.Partition==REG_PART);
 VOID *Actual=NULL;assert(RegGet(REG_PART,&gEfiFatCreateNewProtocolGuid,&Actual)==0&&Actual==New);
 assert(Writes==OldWrites&&Reads==OldReads&&Flushes==OldFlushes&&Allocations==OldAllocations);
}
#ifdef FAT_REGISTRY_EXPORT
// Exercise the production choice state machine and adapter, not the dirty UI.
static unsigned RegExportCalls;
static T70_EXPORT_RESULT RegistryChosenExport(EFI_BOOT_SERVICES *B,T70_EXPORT_CHOICE *Choice,UINT32 Number,CONST VOID *Data,UINTN Size,EFI_STATUS *Detail){
 T70_EXPORT_RESULT Rejected={T70ExportRejected,0,0,0,0,0};
 if(Choice->State!=T70ChoiceApproved)return Rejected;
 RegExportCalls++;return T70ExportSelectedSdFat(B,&RegSnapshot,T70ExportLog,Number,Data,Size,Detail);
}
static void RegistryExport(EFI_BOOT_SERVICES *B){
 T70_EXPORT_CHOICE Choice;EFI_STATUS Detail=0;char Payload[]="manual export fixture\n";
 int OldWrites=Writes,OldReads=Reads,OldFlushes=Flushes;
 T70ChoiceStart(&Choice,1);T70ChoiceKey(&Choice,'1');T70ChoiceKey(&Choice,'n');
 assert(Choice.State==T70ChoiceCancelled);
 assert(RegistryChosenExport(B,&Choice,0xabcdef00,Payload,sizeof(Payload)-1,&Detail).Status==T70ExportRejected);
 assert(!RegExportCalls&&Writes==OldWrites&&Reads==OldReads&&Flushes==OldFlushes);
 T70ChoiceStart(&Choice,1);T70ChoiceKey(&Choice,'1');T70ChoiceKey(&Choice,'Y');
 assert(Choice.State==T70ChoiceApproved);
 T70_EXPORT_RESULT R=RegistryChosenExport(B,&Choice,0xabcdef01,Payload,sizeof(Payload)-1,&Detail);
 assert(R.Status==T70ExportOk&&Detail==EFI_SUCCESS&&R.Created&&R.FlushSucceeded&&R.Written==sizeof(Payload)-1);
 assert(!memcmp(Payload,"manual export fixture\n",sizeof(Payload)));
 // Fresh explicit approval for a colliding name must not overwrite or truncate.
 T70ChoiceKey(&Choice,'Y');assert(Choice.State==T70ChoiceCancelled);
 assert(RegistryChosenExport(B,&Choice,0xabcdef00,Payload,sizeof(Payload)-1,&Detail).Status==T70ExportRejected);
 T70ChoiceStart(&Choice,1);T70ChoiceKey(&Choice,'1');T70ChoiceKey(&Choice,'Y');
 R=RegistryChosenExport(B,&Choice,0xabcdef01,"replacement",11,&Detail);
 assert(R.Status==T70ExportCreateFailed&&Detail==EFI_ACCESS_DENIED&&!R.Created&&RegExportCalls==2);
 printf("Production choice/core/SD adapter -> real FAT: cancel/replay no call, confirmed file flushed, collision rejected\n");
}
#endif
static void RegistryAfter(EFI_BOOT_SERVICES *B){
 assert(!Installed);UINTN Count=99;T70_SD_FAT_IDENTITY Now;
 int OldWrites=Writes,OldReads=Reads,OldFlushes=Flushes,OldAllocations=Allocations;
 assert(T70DiscoverSdFatTargets(B,&Now,1,&Count)==EFI_NOT_FOUND&&Count==0);
 assert(EFI_ERROR(T70ValidateSelectedSdFatTarget(B,REG_HOST,REG_CARD,REG_PART,&RegSnapshot,&Now)));
 VOID *Actual=(VOID *)99;assert(EFI_ERROR(RegGet(REG_PART,&gEfiFatCreateNewProtocolGuid,&Actual))&&!Actual);
#ifdef FAT_REGISTRY_EXPORT
 EFI_STATUS Detail=0;
 T70_EXPORT_RESULT R=T70ExportSelectedSdFat(B,&RegSnapshot,T70ExportLog,0xabcdef02,"stale",5,&Detail);
 assert(R.Status==T70ExportRejected&&EFI_ERROR(Detail)&&!R.Created);
#endif
 assert(Writes==OldWrites&&Reads==OldReads&&Flushes==OldFlushes&&Allocations==OldAllocations);
 printf("Actual FAT registry: published candidate found; uninstall removes candidate and rejects old snapshot (%u metadata calls, no discovery IO)\n",RegQueries);
}
