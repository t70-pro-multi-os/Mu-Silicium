// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../../Applications/DeviceCheck/SdFatExport.c"
#include "../../Applications/DeviceCheck/ManualExportCore.c"
#define main identity_tests
#include "test-sd-fat-target.c"
#undef main
static EFI_FILE_PROTOCOL Root,File;
static EFI_FAT_CREATE_NEW_PROTOCOL New;
static int Opens,Creates,Writes,Flushes,Closes,RootCloses,Fault;
static EFI_STATUS EFIAPI OpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Self,EFI_FILE_PROTOCOL **R){
 assert(Self==&Fs);Opens++;*R=&Root;if(Fault==1)Media.MediaId++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CreateFile(EFI_FAT_CREATE_NEW_PROTOCOL *Self,EFI_HANDLE Handle,UINT32 Mid,EFI_FILE_PROTOCOL *R,CONST CHAR16 *Name,EFI_FILE_PROTOCOL **F){
 assert(Self==&New&&Handle==P&&Mid==7&&R==&Root);assert(Name[0]==84&&Name[16]==0);Creates++;
 *F=NULL;if(Fault==2)return EFI_ACCESS_DENIED;if(Fault==9)return EFI_MEDIA_CHANGED;*F=&File;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI WriteFile(EFI_FILE_PROTOCOL *Self,UINTN *N,VOID *Data){assert(Self==&File&&*N==4&&!memcmp(Data,"test",4));Writes++;
 if(Fault==3)return EFI_DEVICE_ERROR;if(Fault==10){*N=0;return EFI_NO_MEDIA;}if(Fault==4)(*N)--;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI FlushFile(EFI_FILE_PROTOCOL *Self){assert(Self==&File);Flushes++;return Fault==5?EFI_DEVICE_ERROR:Fault==11?EFI_MEDIA_CHANGED:EFI_SUCCESS;}
static EFI_STATUS EFIAPI CloseFile(EFI_FILE_PROTOCOL *Self){if(Self==&Root){RootCloses++;return Fault==7||Fault==11?EFI_DEVICE_ERROR:EFI_SUCCESS;}
 assert(Self==&File);Closes++;return Fault==6?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI GetExport(EFI_HANDLE Handle,EFI_GUID *Guid,VOID **Out){
 if(Handle==P&&!memcmp(Guid,&NewGuid,sizeof(*Guid))){*Out=&New;return Fault==8?EFI_NOT_FOUND:EFI_SUCCESS;}return Get(Handle,Guid,Out);}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Guid,VOID *Key,UINTN *Count,EFI_HANDLE **List){
 assert(Type==ByProtocol&&!Key);*Count=1;*List=malloc(sizeof(**List));assert(*List);Allocations++;
 **List=!memcmp(Guid,&PassGuid,sizeof(*Guid))?H:!memcmp(Guid,&BlockGuid,sizeof(*Guid))?C:P;return EFI_SUCCESS;}
int main(void){
 identity_tests();EFI_BOOT_SERVICES B={0};B.HandleProtocol=GetExport;B.OpenProtocolInformation=Info;B.FreePool=Free;B.LocateHandleBuffer=Locate;
 New.Revision=1;New.CreateNew=CreateFile;Root.Close=CloseFile;File.Close=CloseFile;File.Write=WriteFile;File.Flush=FlushFile;Fs.OpenVolume=OpenVolume;
 T70_SD_FAT_IDENTITY Id;unsigned Cases=0;UINTN N;EFI_STATUS Detail;
 for(Fault=0;Fault<=8;Fault++){
  fresh();Opens=Creates=Writes=Flushes=Closes=RootCloses=0;
  assert(T70ValidateSelectedSdFatTarget(&B,H,C,P,NULL,&Id)==EFI_SUCCESS);
  T70_EXPORT_RESULT R=T70ExportSelectedSdFat(&B,&Id,T70ExportLog,123,"test",4,&Detail);
  assert(R.Status==(Fault==0?T70ExportOk:Fault==1?T70ExportCreateFailed:Fault==2||Fault==8?T70ExportCreateFailed:Fault==3?T70ExportWriteFailed:Fault==4?T70ExportShortWrite:Fault==5?T70ExportFlushFailed:T70ExportCloseFailed));
  assert(Opens==(Fault==8?0:1));assert(RootCloses==Opens);assert(Creates==(Fault==1||Fault==8?0:1));
  assert(Writes==(Fault==1||Fault==2||Fault==8?0:1));assert(Closes==Writes);
  if(Fault==3)assert(R.Written==0); /* FAT can fail without updating BufferSize. */
  assert(Flushes==(Writes&&Fault!=3&&Fault!=4));assert(!Allocations);Cases++;
 }
 /* On mid-operation media loss, never retry on a replacement or report success. */
 for(Fault=9;Fault<=11;Fault++){
  fresh();Opens=Creates=Writes=Flushes=Closes=RootCloses=0;
  assert(T70ValidateSelectedSdFatTarget(&B,H,C,P,NULL,&Id)==EFI_SUCCESS);
  T70_EXPORT_RESULT R=T70ExportSelectedSdFat(&B,&Id,T70ExportLog,123,"test",4,&Detail);
  assert(R.Status==(Fault==9?T70ExportCreateFailed:Fault==10?T70ExportWriteFailed:T70ExportFlushFailed));
  assert(Detail==(Fault==10?EFI_NO_MEDIA:EFI_MEDIA_CHANGED));
  assert(Opens==1&&Creates==1&&RootCloses==1&&Writes==(Fault!=9)&&Closes==Writes);
  assert(Flushes==(Fault==11)&&R.Created==(Fault!=9)&&!R.FlushSucceeded);
  assert(R.Written==(Fault==11?4:0)&&R.CloseError==(Fault==11)&&!Allocations);Cases++;
 }
 Fault=0;fresh();assert(T70DiscoverSdFatTargets(&B,&Id,1,&N)==EFI_SUCCESS&&N==1);Cases++;
 Media.ReadOnly=TRUE;assert(T70DiscoverSdFatTargets(&B,&Id,1,&N)==EFI_NOT_FOUND&&N==0);Cases++;
 fresh();Hp[19]=Cp[19]=Pp[19]=0;assert(T70DiscoverSdFatTargets(&B,&Id,1,&N)==EFI_NOT_FOUND&&N==0);Cases++;
 fresh();assert(T70ValidateSelectedSdFatTarget(&B,H,C,P,NULL,&Id)==EFI_SUCCESS);Opens=0;Media.MediaId++;
 assert(T70ExportSelectedSdFat(&B,&Id,T70ExportScreenshot,0,"test",4,&Detail).Status==T70ExportRejected&&Opens==0);Cases++;
 fresh();Opens=0;assert(T70ExportSelectedSdFat(&B,&Id,T70ExportLog,0,NULL,0,&Detail).Status==T70ExportRejected&&Opens==0);Cases++;
 assert(!Allocations);printf("SD FAT export: %u adapter scenarios passed; mocked protocols only\n",Cases);return 0;
}
