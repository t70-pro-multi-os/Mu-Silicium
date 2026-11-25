// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeFormatUpdate.h"
#include <assert.h>
#include <stdio.h>
static unsigned Checks;
#define C(X) do{assert(X);Checks++;}while(0)
typedef struct {unsigned Calls,Fail;int32_t Error;uint32_t Value,Expected[4];} MOCK;
static int32_t Read(void *Ctx,uint32_t Offset,uint32_t *Value){
  MOCK *M=Ctx;C(Offset==0x4c&&M->Calls<8&&M->Calls%2==0);
  unsigned I=M->Calls++;*Value=M->Value;return I==M->Fail?M->Error:0;
}
static int32_t Write(void *Ctx,uint32_t Offset,uint32_t Value){
  MOCK *M=Ctx;C(Offset==0x4c&&M->Calls<8&&M->Calls%2==1);
  unsigned I=M->Calls++;C(Value==M->Expected[I/2]);
  /* Even a failed write may have applied: production must not retry/rollback. */
  M->Value=Value;return I==M->Fail?M->Error:0;
}
static void Test(uint32_t Old,unsigned Format,unsigned Channels,unsigned Fail,int32_t Error){
  MOCK M={0};M.Value=Old;M.Fail=Fail;M.Error=Error;
  M.Expected[0]=(Old&~0x100U)|(Channels==1?0x100U:0);
  M.Expected[1]=(M.Expected[0]&0xf0ffffffU)|0x0a000000U;
  M.Expected[2]=(M.Expected[1]&~3U)|(Format==1?0:1);
  M.Expected[3]=M.Expected[2]&~0x10U;
  T70_AFE_FORMAT_IO Io={&M,Read,Write};
  T70_AFE_FIELDS_RESULT R=T70AfeDl1UpdateFormat(48000,Format,Channels,&Io);
  if(Fail==8){
    C(R.Code==T70AfeFieldsUpdated&&R.FailedField==4&&R.TransportStatus==0&&M.Calls==8);
    C(R.ReadsAttempted==4&&R.WritesAttempted==4&&R.WritesCompleted==4&&M.Value==M.Expected[3]);
    C((M.Value&~0x0f000113U)==(Old&~0x0f000113U));
  }else{
    C(R.Code==(Fail%2?T70AfeFieldsWriteFailed:T70AfeFieldsReadFailed)&&R.TransportStatus==Error&&R.FailedField==Fail/2);
    C(M.Calls==Fail+1&&R.ReadsAttempted==Fail/2+1&&R.WritesAttempted==(Fail+1)/2&&R.WritesCompleted==Fail/2);
  }
}
int main(void){
  for(uint32_t V=0;V<65536;V++)for(unsigned F=1;F<=2;F++)for(unsigned Ch=1;Ch<=2;Ch++)Test((V<<16)|(V^0xa5a5),F,Ch,8,0);
  const int32_t Errors[]={-1,1,INT32_MIN,INT32_MAX};
  for(unsigned Stage=0;Stage<8;Stage++)for(unsigned E=0;E<4;E++)Test(UINT32_MAX,2,1,Stage,Errors[E]);
  for(unsigned Bit=0;Bit<32;Bit++){Test(1U<<Bit,1,2,8,0);Test(~(1U<<Bit),2,1,8,0);}
  const uint32_t Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
  const uint32_t Codes[]={0,1,4,5,8,9,10,11,12,13,14};
  for(unsigned I=0;I<11;I++){
    MOCK N={0};N.Fail=8;N.Value=0xffffffff;
    N.Expected[0]=0xfffffeff;N.Expected[1]=(N.Expected[0]&0xf0ffffffU)|(Codes[I]<<24);
    N.Expected[2]=N.Expected[1]&~3U;N.Expected[3]=N.Expected[2]&~0x10U;
    T70_AFE_FORMAT_IO J={&N,Read,Write};
    C(T70AfeDl1UpdateFormat(Rates[I],1,2,&J).Code==T70AfeFieldsUpdated&&N.Value==N.Expected[3]);
  }
  MOCK M={0};T70_AFE_FORMAT_IO Io={&M,Read,Write};
  C(T70AfeDl1UpdateFormat(12000,1,2,&Io).Code==T70AfeFieldsInvalid);
  C(T70AfeDl1UpdateFormat(48000,3,2,&Io).Code==T70AfeFieldsInvalid);
  C(T70AfeDl1UpdateFormat(48000,1,0,&Io).Code==T70AfeFieldsInvalid);
  C(T70AfeDl1UpdateFormat(48000,1,2,NULL).Code==T70AfeFieldsInvalid);
  Io.Read32=NULL;C(T70AfeDl1UpdateFormat(48000,1,2,&Io).Code==T70AfeFieldsInvalid);
  Io.Read32=Read;Io.Write32=NULL;C(T70AfeDl1UpdateFormat(48000,1,2,&Io).Code==T70AfeFieldsInvalid&&M.Calls==0);
  printf("AFE format field callbacks: %u checks; mocks only\n",Checks);
}
