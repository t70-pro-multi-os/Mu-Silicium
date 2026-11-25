// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwPairInspect.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
static unsigned Checks,Calls,Fail,Wrong;
static int32_t Error;
#define C(X) do{assert(X);Checks++;}while(0)
typedef struct {unsigned Channel;} MOCK;
static int32_t Read(void *Ctx,uint16_t Reg,uint16_t *Value){
 MOCK *M=Ctx;unsigned N=++Calls;C(N<=4);C(M->Channel==(N-1)/2);C(Reg==(N%2?0:1));
 *Value=Reg?0x11:(M->Channel==Wrong?0:0x2113);
 return N==Fail?Error:0;
}
static void Run(const uint8_t *B,size_t N,unsigned F,int32_t Err,unsigned W,uint16_t Profile){
 MOCK M[2]={{0},{1}};T70_AW_PROBE_ENDPOINT E[2]={{0x11e01000,0x34,0,{&M[0],Read}},{0x11e01000,0x35,1,{&M[1],Read}}};
 Calls=0;Fail=F;Error=Err;Wrong=W;
 T70_AW_PAIR_INSPECTION S;memset(&S,0x5a,sizeof S);unsigned char Old[sizeof S];memcpy(Old,&S,sizeof S);
 T70_AW_INSPECT_RESULT R=T70Aw88261InspectPair(B,N,&E[0],&E[1],Profile,&S);
 if(!B||N!=3856||Profile==1){C(R.Code==T70AwInspectProfileFailed);C(!R.ProbeStarted);C(!Calls);C(!memcmp(Old,&S,sizeof S));}
 else if(F||W<2){unsigned Stop=F?F:W*2+1;C(R.Code==T70AwInspectProbeFailed);C(R.ProbeStarted);C(Calls==Stop);C(R.Probe.FailedChannel==(Stop-1)/2);C(!memcmp(Old,&S,sizeof S));C(R.Probe.Endpoint.TransportStatus==(F?Err:0));}
 else{C(R.Code==T70AwInspectObserved);C(Calls==4);C(S.Profiles.RegisterProfileCount==2);
 for(unsigned I=0;I<2;I++){C(S.Profiles.Channel[I].Section.Profile==Profile);C(S.Profiles.Channel[I].Section.DeviceIndex==I);C(S.Observations.Channels[I].ChipId==0x2113);}}
 /* Invalid second target must reject before even the first read. */
 Calls=0;E[1].Io.Context=E[0].Io.Context;memcpy(Old,&S,sizeof S);
 R=T70Aw88261InspectPair(B,N,&E[0],&E[1],Profile,&S);C(R.Code==T70AwInspectInvalid);C(!Calls);C(!memcmp(Old,&S,sizeof S));
}
int main(int argc,char **argv){C(argc==2);FILE *F=fopen(argv[1],"rb");C(F);uint8_t B[3857];size_t N=fread(B,1,sizeof B,F);C(!ferror(F));fclose(F);C(N==3856);
 int32_t Errors[]={-1,1,INT32_MIN,INT32_MAX};
 for(unsigned P=0;P<=10;P+=10){Run(B,N,0,0,2,(uint16_t)P);for(unsigned I=1;I<=4;I++)for(unsigned E=0;E<4;E++)Run(B,N,I,Errors[E],2,(uint16_t)P);for(unsigned W=0;W<2;W++)Run(B,N,0,0,W,(uint16_t)P);}
 for(size_t I=0;I<N;I++)Run(B,I,0,0,2,0);Run(NULL,N,0,0,2,0);Run(B,N,0,0,2,1);
 MOCK M[2]={{0},{1}};T70_AW_PROBE_ENDPOINT E[2]={{0x11e01000,0x34,0,{&M[0],Read}},{0x11e01000,0x35,1,{&M[1],Read}}};
 T70_AW_PAIR_INSPECTION S;memset(&S,0x5a,sizeof S);unsigned char Old[sizeof S];memcpy(Old,&S,sizeof S);
 Calls=0;B[N-1]^=1;T70_AW_INSPECT_RESULT R=T70Aw88261InspectPair(B,N,&E[0],&E[1],0,&S);
 C(R.Code==T70AwInspectProfileFailed);C(!Calls&&!R.ProbeStarted);C(!memcmp(Old,&S,sizeof S));B[N-1]^=1;
 for(unsigned I=0;I<6;I++){
 T70_AW_PROBE_ENDPOINT Bad=E[1];
 if(I==0)Bad.Base++;if(I==1)Bad.Address=0x34;if(I==2)Bad.Channel=0;
 if(I==3)Bad.Io.Context=NULL;if(I==4)Bad.Io.Read=NULL;
 R=T70Aw88261InspectPair(B,N,&E[0],I==5?NULL:&Bad,0,&S);
 C(R.Code==T70AwInspectInvalid);C(!Calls&&!R.ProbeStarted);C(!memcmp(Old,&S,sizeof S));
 }
 R=T70Aw88261InspectPair(B,N,NULL,&E[1],0,&S);C(R.Code==T70AwInspectInvalid);C(!Calls);
 R=T70Aw88261InspectPair(B,N,&E[0],&E[1],0,NULL);C(R.Code==T70AwInspectInvalid);C(!Calls);
 printf("PairInspectTest: %u checks; injected read targets only\n",Checks);
}
