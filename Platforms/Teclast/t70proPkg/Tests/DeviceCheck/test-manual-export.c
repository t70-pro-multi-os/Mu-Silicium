// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../Applications/DeviceCheck/ManualExportCore.h"
static unsigned Creates,Writes,Flushes,Closes;static int Valid,Exists,WE,FE,CE,Short;
static char Name[32];static int File;
static int valid(void *C,uint64_t S){(void)C;return Valid&&S==7;}
static int create(void *C,uint64_t S,const char *N,void **F){(void)C;assert(S==7);Creates++;strcpy(Name,N);if(Exists)return -1;*F=&File;return 0;}
static int writebuf(void *C,void *F,const void *D,size_t N,size_t *W){(void)C;assert(F==&File&&D&&N==3);Writes++;*W=Short?N-1:N;return WE;}
static int flush(void *C,void *F){(void)C;assert(F==&File);Flushes++;return FE;}
static int closefile(void *C,void *F){(void)C;assert(F==&File);Closes++;return CE;}
static void reset(void){Creates=Writes=Flushes=Closes=0;Valid=1;Exists=WE=FE=CE=Short=0;}
int main(void){
 T70_EXPORT_IO I={NULL,valid,create,writebuf,flush,closefile};T70_EXPORT_REQUEST R={7,1,1,1};T70_EXPORT_RESULT O;
 for(unsigned K=1;K<=2;K++){reset();O=T70ManualExport(&R,K,0x1234,"abc",3,&I);assert(O.Status==T70ExportOk&&O.FlushSucceeded&&O.Written==3);assert(Creates==1&&Writes==1&&Flushes==1&&Closes==1);assert(!strcmp(Name,K==1?"T70-00001234.txt":"T70-00001234.bmp"));}
 for(unsigned K=0;K<5;K++){reset();T70_EXPORT_REQUEST X=R;if(K==0)X.Manual=0;if(K==1)X.SdFat=0;if(K==2)X.Writable=0;if(K==3)X.Selection=0;if(K==4)Valid=0;O=T70ManualExport(&X,1,0,"abc",3,&I);assert(O.Status==T70ExportRejected&&!Creates&&!Writes);}
 reset();Exists=1;O=T70ManualExport(&R,1,0,"abc",3,&I);assert(O.Status==T70ExportCreateFailed&&!Writes&&!Flushes&&!Closes);
 reset();WE=-2;CE=-4;O=T70ManualExport(&R,1,0,"abc",3,&I);assert(O.Status==T70ExportWriteFailed&&O.OperationError==-2&&O.CloseError==-4&&!Flushes&&Closes==1);
 reset();Short=1;O=T70ManualExport(&R,1,0,"abc",3,&I);assert(O.Status==T70ExportShortWrite&&!Flushes&&Closes==1);
 reset();FE=-3;CE=-4;O=T70ManualExport(&R,1,0,"abc",3,&I);assert(O.Status==T70ExportFlushFailed&&O.OperationError==-3&&O.CloseError==-4&&!O.FlushSucceeded);
 reset();CE=-4;O=T70ManualExport(&R,1,0,"abc",3,&I);assert(O.Status==T70ExportCloseFailed&&O.FlushSucceeded);
 reset();O=T70ManualExport(&R,3,0,"abc",3,&I);assert(O.Status==T70ExportRejected&&!Creates);
 reset();O=T70ManualExport(&R,1,0,NULL,3,&I);assert(O.Status==T70ExportRejected&&!Creates);
 reset();O=T70ManualExport(&R,1,0,"abc",0,&I);assert(O.Status==T70ExportRejected&&!Creates);
 reset();O=T70ManualExport(&R,1,0,"abc",128U*1024U*1024U+1,&I);assert(O.Status==T70ExportRejected&&!Creates);
 puts("Manual export: 16 mock scenarios passed; no firmware adapter/media proof");return 0;
}
