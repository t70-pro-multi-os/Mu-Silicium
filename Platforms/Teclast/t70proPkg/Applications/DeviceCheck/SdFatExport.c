// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SdFatExport.h"
#include <Protocol/SdMmcPassThru.h>
#include <Protocol/FatCreateNew.h>
static EFI_GUID PassGuid=EFI_SD_MMC_PASS_THRU_PROTOCOL_GUID;
static EFI_GUID BlockGuid=EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID FsGuid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static EFI_GUID NewGuid=EFI_FAT_CREATE_NEW_PROTOCOL_GUID;
EFI_STATUS T70DiscoverSdFatTargets(EFI_BOOT_SERVICES *B,T70_SD_FAT_IDENTITY *Out,UINTN Cap,UINTN *Count){
 EFI_HANDLE *Hosts=NULL,*Cards=NULL,*Parts=NULL;UINTN Nh=0,Nc=0,Np=0,N=0;EFI_STATUS S;
 if(!B||!B->LocateHandleBuffer||!B->FreePool||!Out||!Cap||!Count)return EFI_INVALID_PARAMETER;
 *Count=0;
 S=B->LocateHandleBuffer(ByProtocol,&PassGuid,NULL,&Nh,&Hosts);if(EFI_ERROR(S))goto Done;
 S=B->LocateHandleBuffer(ByProtocol,&BlockGuid,NULL,&Nc,&Cards);if(EFI_ERROR(S))goto Done;
 S=B->LocateHandleBuffer(ByProtocol,&FsGuid,NULL,&Np,&Parts);if(EFI_ERROR(S))goto Done;
 if((Nh&&!Hosts)||(Nc&&!Cards)||(Np&&!Parts)){S=EFI_COMPROMISED_DATA;goto Done;}
 /* Bound hostile/broken protocol databases, not the number of UI choices. */
 if(Nh>16||Nc>256||Np>256){S=EFI_OUT_OF_RESOURCES;goto Done;}
 for(UINTN P=0;P<Np;P++)for(UINTN H=0;H<Nh;H++)for(UINTN C=0;C<Nc;C++){
  T70_SD_FAT_IDENTITY Id;EFI_FAT_CREATE_NEW_PROTOCOL *New=NULL;
  if(EFI_ERROR(T70ValidateSelectedSdFatTarget(B,Hosts[H],Cards[C],Parts[P],NULL,&Id)))continue;
  if(EFI_ERROR(B->HandleProtocol(Parts[P],&NewGuid,(VOID **)&New))||!New||New->Revision!=1||!New->CreateNew)continue;
  for(UINTN I=0;I<N;I++)if(Out[I].Partition==Parts[P]){S=EFI_COMPROMISED_DATA;goto Done;}
  if(N==Cap){S=EFI_BUFFER_TOO_SMALL;goto Done;}Out[N++]=Id;
 }
 S=N?EFI_SUCCESS:EFI_NOT_FOUND;if(!EFI_ERROR(S))*Count=N;
Done:
 if(Parts)B->FreePool(Parts);if(Cards)B->FreePool(Cards);if(Hosts)B->FreePool(Hosts);return S;
}
typedef struct {EFI_BOOT_SERVICES *B;CONST T70_SD_FAT_IDENTITY *Id;EFI_STATUS Error;EFI_FILE_PROTOCOL *Root;} CONTEXT;
static int Record(CONTEXT *C,EFI_STATUS S){if(EFI_ERROR(S)&&!EFI_ERROR(C->Error))C->Error=S;return EFI_ERROR(S)?1:0;}
static int Validate(void *V,uint64_t Selection){CONTEXT *C=V;T70_SD_FAT_IDENTITY Now;
 if(Selection!=(uint64_t)(UINTN)C->Id->Partition)return 0;
 return !Record(C,T70ValidateSelectedSdFatTarget(C->B,C->Id->Controller,C->Id->Card,C->Id->Partition,C->Id,&Now));
}
static int Create(void *V,uint64_t Selection,const char *Name,void **File){
 CONTEXT *C=V;EFI_FAT_CREATE_NEW_PROTOCOL *New=NULL;EFI_STATUS S;CHAR16 Wide[17];*File=NULL;
 if(!Validate(C,Selection))return 1;
 S=C->B->HandleProtocol(C->Id->Partition,&NewGuid,(VOID **)&New);if(Record(C,S))return 1;
 if(!New||New->Revision!=1||!New->CreateNew)return Record(C,EFI_UNSUPPORTED);
 if(!C->Id->FileSystem->OpenVolume)return Record(C,EFI_UNSUPPORTED);
 S=C->Id->FileSystem->OpenVolume(C->Id->FileSystem,&C->Root);if(Record(C,S))return 1;
 if(!C->Root)return Record(C,EFI_DEVICE_ERROR);
 /* OpenVolume can yield to callbacks; check snapshot again before CreateNew. */
 if(!Validate(C,Selection))return 1;
 for(UINTN I=0;I<17;I++)Wide[I]=(CHAR16)(UINT8)Name[I];
 S=New->CreateNew(New,C->Id->Partition,C->Id->MediaId,C->Root,Wide,(EFI_FILE_PROTOCOL **)File);
 return Record(C,S);
}
static int Write(void *V,void *F,const void *D,size_t N,size_t *Written){CONTEXT *C=V;UINTN Size=N;
 EFI_STATUS S=((EFI_FILE_PROTOCOL *)F)->Write(F,&Size,(VOID *)D);
 /* A FAT error before data access may leave BufferSize equal to the request.
    Do not present that unchanged count as confirmed progress. */
 *Written=EFI_ERROR(S)&&Size>=N?0:Size;return Record(C,S);}
static int Flush(void *V,void *F){return Record(V,((EFI_FILE_PROTOCOL *)F)->Flush(F));}
static int Close(void *V,void *F){return Record(V,((EFI_FILE_PROTOCOL *)F)->Close(F));}
T70_EXPORT_RESULT T70ExportSelectedSdFat(EFI_BOOT_SERVICES *B,CONST T70_SD_FAT_IDENTITY *Id,
 T70_EXPORT_KIND Kind,UINT32 Number,CONST VOID *Data,UINTN Size,EFI_STATUS *Detail){
 T70_EXPORT_RESULT R={T70ExportRejected,0,0,0,0,0};CONTEXT C={B,Id,EFI_SUCCESS,NULL};
 if(Detail)*Detail=EFI_INVALID_PARAMETER;
 if(!B||!Id||!Id->Partition)return R;
 T70_EXPORT_REQUEST Request={(uint64_t)(UINTN)Id->Partition,1,1,1};
 T70_EXPORT_IO Io={&C,Validate,Create,Write,Flush,Close};
 R=T70ManualExport(&Request,Kind,Number,Data,Size,&Io);
 if(C.Root){EFI_STATUS S=C.Root->Close(C.Root);if(Record(&C,S)){R.CloseError=1;if(R.Status==T70ExportOk)R.Status=T70ExportCloseFailed;}}
 if(!EFI_ERROR(C.Error)&&R.Status==T70ExportRejected)C.Error=EFI_INVALID_PARAMETER;
 if(!EFI_ERROR(C.Error)&&R.Status==T70ExportShortWrite)C.Error=EFI_DEVICE_ERROR;
 if(Detail)*Detail=C.Error;return R;
}
