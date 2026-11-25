// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "ManualExportCore.h"
T70_EXPORT_RESULT T70ManualExport(const T70_EXPORT_REQUEST *R,T70_EXPORT_KIND Kind,
 uint32_t Number,const void *Data,size_t Size,const T70_EXPORT_IO *Io) {
 T70_EXPORT_RESULT Out={T70ExportRejected,0,0,0,0,0};
 char Name[]="T70-00000000.txt"; static const char Hex[]="0123456789ABCDEF";
 void *File=NULL;int E;
 if(!R||!Io||!Data||!Size||Size>128U*1024U*1024U||!R->Selection||
    R->Manual!=1||R->SdFat!=1||R->Writable!=1||
    (Kind!=T70ExportLog&&Kind!=T70ExportScreenshot)||!Io->ValidateSelection||
    !Io->CreateNew||!Io->Write||!Io->Flush||!Io->Close)return Out;
 if(!Io->ValidateSelection(Io->Context,R->Selection))return Out;
 for(unsigned I=0;I<8;I++)Name[4+I]=Hex[(Number>>(28-4*I))&15];
 if(Kind==T70ExportScreenshot){Name[13]='b';Name[14]='m';Name[15]='p';}
 E=Io->CreateNew(Io->Context,R->Selection,Name,&File);
 if(E||!File){Out.Status=T70ExportCreateFailed;Out.OperationError=E;return Out;}
 Out.Created=1;
 E=Io->Write(Io->Context,File,Data,Size,&Out.Written);
 if(E){Out.Status=T70ExportWriteFailed;Out.OperationError=E;}
 else if(Out.Written!=Size)Out.Status=T70ExportShortWrite;
 else {
  E=Io->Flush(Io->Context,File);Out.OperationError=E;
  Out.Status=E?T70ExportFlushFailed:T70ExportOk;Out.FlushSucceeded=!E;
 }
 Out.CloseError=Io->Close(Io->Context,File);
 if(Out.Status==T70ExportOk&&Out.CloseError)Out.Status=T70ExportCloseFailed;
 return Out;
}
