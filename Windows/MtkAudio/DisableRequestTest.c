// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeDisableRequest.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned Checks,Calls;static uint32_t Value;static int32_t ReadError,WriteError;
#define C(X) do{Checks++;if(!(X)){fprintf(stderr,"line %d\n",__LINE__);exit(1);}}while(0)
static int32_t Read(void *Ctx,uint32_t O,uint32_t *V){C(Ctx==&Calls&&O==0x1b08&&Calls++==0);*V=Value;return ReadError;}
static int32_t Write(void *Ctx,uint32_t O,uint32_t V){C(Ctx==&Calls&&O==0x1b08&&V==0x10&&Calls++==1);return WriteError;}
int main(void){T70_AFE_FORMAT_IO Io={&Calls,Read,Write};T70_AFE_DISABLE_RESULT R;
 for(unsigned I=0;I<65536;I++)for(unsigned Upper=0;Upper<2;Upper++){
  Value=I|(Upper?0xffff0000u:0);Calls=0;R=T70AfeDl1RequestDisable(&Io);
  int Allowed=Value==0;
  C(R.Code==(Allowed?T70DisableWriteReturned:Value==0x10?T70DisableAlreadySetObserved:T70DisableAliasUnresolved));C(Calls==(Allowed?2u:1u));
  C(R.ReadAttempted&&R.WriteAttempted==(unsigned)Allowed&&R.AliasValue==Value&&!R.TransportStatus);
 }
 int32_t Errors[]={-1,1,INT32_MIN,INT32_MAX};
 for(unsigned I=0;I<4;I++)for(unsigned Bit=0;Bit<2;Bit++){
  Value=Bit?0x10:0;Calls=0;ReadError=Errors[I];R=T70AfeDl1RequestDisable(&Io);
  C(R.Code==T70DisableReadFailed&&R.TransportStatus==Errors[I]&&Calls==1&&!R.WriteAttempted&&!R.AliasValue);
  ReadError=0;WriteError=Errors[I];Calls=0;R=T70AfeDl1RequestDisable(&Io);
  if(!Bit)C(R.Code==T70DisableWriteFailed&&R.TransportStatus==Errors[I]&&Calls==2&&R.WriteAttempted);
  else C(R.Code==T70DisableAlreadySetObserved&&!R.TransportStatus&&Calls==1&&!R.WriteAttempted);WriteError=0;
 }
 Calls=0;C(T70AfeDl1RequestDisable(NULL).Code==T70DisableInvalid);
 for(unsigned I=0;I<3;I++){T70_AFE_FORMAT_IO Bad=Io;if(I==0)Bad.Context=NULL;if(I==1)Bad.Read32=NULL;if(I==2)Bad.Write32=NULL;C(T70AfeDl1RequestDisable(&Bad).Code==T70DisableInvalid);}
 C(!Calls);printf("DisableRequest: %u checks; no idle/drain claim\n",Checks);return 0;
}
