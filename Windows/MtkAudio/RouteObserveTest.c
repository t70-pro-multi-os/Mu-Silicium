// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeRouteObserve.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
static unsigned Checks;
#define CHECK(X) do{assert(X);++Checks;}while(0)
typedef struct {uint32_t Value[2],Calls,Fail;int32_t Error;} MOCK;
static int32_t Read(void *Context,uint32_t Offset,uint32_t *Value) {
  MOCK *M=Context;CHECK(M->Calls<2);CHECK(Offset==0x20+4*M->Calls);
  *Value=M->Value[M->Calls++]; /* even failed reads may modify local storage */
  return M->Calls==M->Fail?M->Error:0;
}
static void Success(uint32_t A,uint32_t B) {
  MOCK M={{A,B},0,0,0};T70_AFE_ROUTE_READER Io={&M,Read};
  T70_AFE_ROUTE_OBSERVATION O;memset(&O,0xa5,sizeof(O));
  T70_AFE_ROUTE_RESULT R=T70AfeObserveDl1I2s3(&Io,&O);
  CHECK(R.Code==T70AfeRouteObserved);CHECK(R.TransportStatus==0);
  CHECK(R.ReadsAttempted==2 && R.FailedRead==2 && M.Calls==2);
  CHECK(O.Conn0==A && O.Conn1==B);
  CHECK(O.Dl1Channel1Selected==((A&32)!=0));
  CHECK(O.Dl1Channel2Selected==((B&64)!=0));
}
int main(void) {
  for(uint32_t V=0;V<65536;V++){Success(V,~V);Success(~V,V);}
  for(unsigned Bit=0;Bit<32;Bit++)for(unsigned Selected=0;Selected<4;Selected++)
    Success(((1u<<Bit)&~32u)|((Selected&1)?32:0),
            ((~(1u<<Bit))&~64u)|((Selected&2)?64:0));
  const int32_t Errors[]={-1,1,INT32_MIN,INT32_MAX};
  for(unsigned F=1;F<=2;F++)for(unsigned E=0;E<4;E++) {
    MOCK M={{UINT32_MAX,UINT32_MAX},0,F,Errors[E]};
    T70_AFE_ROUTE_READER Io={&M,Read};T70_AFE_ROUTE_OBSERVATION O,Before;
    memset(&O,0xa5,sizeof(O));Before=O;
    T70_AFE_ROUTE_RESULT R=T70AfeObserveDl1I2s3(&Io,&O);
    CHECK(R.Code==T70AfeRouteReadFailed && R.TransportStatus==Errors[E]);
    CHECK(R.ReadsAttempted==F && R.FailedRead==F-1 && M.Calls==F);
    CHECK(memcmp(&O,&Before,sizeof(O))==0);
  }
  MOCK M={{0,0},0,0,0};T70_AFE_ROUTE_READER Io={&M,Read},Bad={&M,0};
  T70_AFE_ROUTE_OBSERVATION O,Before;memset(&O,0xa5,sizeof(O));Before=O;
  T70_AFE_ROUTE_RESULT R=T70AfeObserveDl1I2s3(0,&O);
  CHECK(R.Code==T70AfeRouteInvalid && R.ReadsAttempted==0 && R.FailedRead==2);
  R=T70AfeObserveDl1I2s3(&Bad,&O);CHECK(R.Code==T70AfeRouteInvalid);
  R=T70AfeObserveDl1I2s3(&Io,0);CHECK(R.Code==T70AfeRouteInvalid);
  CHECK(M.Calls==0 && memcmp(&O,&Before,sizeof(O))==0);
  printf("RouteObserveTest: %u checks passed\n",Checks);return 0;
}
