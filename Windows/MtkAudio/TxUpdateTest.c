// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwTxUpdate.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned Checks,Reads,Writes,Sequence;static uint16_t Old,Expected,*Output;
static int32_t ReadStatus,WriteStatus;
#define CHECK(X) do {Checks++;if(!(X)){fprintf(stderr,"%d: %s\n",__LINE__,#X);abort();}}while(0)
static int32_t Read(void *C,uint16_t R,uint16_t *Value){CHECK(C==&Old && R==8 && Sequence++==0);Reads++;CHECK(*Output==0xbeef);*Value=Old;return ReadStatus;}
static int32_t Write(void *C,uint16_t R,uint16_t Value){CHECK(C==&Old && R==8 && Sequence++==1);Writes++;CHECK(Value==Expected && *Output==0xbeef);return WriteStatus;}
static void Reset(uint16_t *Out){Reads=Writes=Sequence=0;ReadStatus=WriteStatus=0;Output=Out;*Out=0xbeef;}
int main(void){
 T70_AW_TX_IO Io={&Old,Read,Write};uint16_t Applied;
 for(unsigned V=0;V<65536;V++)for(unsigned E=0;E<2;E++){
  Reset(&Applied);Old=(uint16_t)V;Expected=(uint16_t)((V&~64U)|(E*64));
  T70_AW_TX_UPDATE_RESULT R=T70Aw88261UpdateTx(&Io,E,&Applied);
  CHECK(R.Code==T70AwTxUpdated && R.TransportStatus==0 && Reads==1 && Writes==1);
  CHECK(Applied==Expected);
 }
 const int32_t Errors[]={-1,-7,19,INT32_MIN,INT32_MAX};
 for(unsigned E=0;E<5;E++)for(unsigned P=0;P<2;P++){
  Reset(&Applied);Old=Expected=0x1234;
  if(P)WriteStatus=Errors[E];else ReadStatus=Errors[E];
  T70_AW_TX_UPDATE_RESULT R=T70Aw88261UpdateTx(&Io,0,&Applied);
  CHECK(R.Code==(P?T70AwTxWriteFailed:T70AwTxReadFailed));
  CHECK(R.TransportStatus==Errors[E] && Applied==0xbeef && Reads==1 && Writes==P);
 }
 Reset(&Applied);
 for(unsigned E=2;E<256;E++)CHECK(T70Aw88261UpdateTx(&Io,E,&Applied).Code==T70AwTxInvalid);
 CHECK(T70Aw88261UpdateTx(&Io,UINT_MAX,&Applied).Code==T70AwTxInvalid);
 CHECK(T70Aw88261UpdateTx(NULL,0,&Applied).Code==T70AwTxInvalid);
 CHECK(T70Aw88261UpdateTx(&Io,0,NULL).Code==T70AwTxInvalid);
 Io.Read=NULL;CHECK(T70Aw88261UpdateTx(&Io,0,&Applied).Code==T70AwTxInvalid);
 Io.Read=Read;Io.Write=NULL;CHECK(T70Aw88261UpdateTx(&Io,0,&Applied).Code==T70AwTxInvalid);
 CHECK(Reads==0 && Writes==0 && Applied==0xbeef);
 printf("AW TX update: %u checks passed\n",Checks);return 0;
}
