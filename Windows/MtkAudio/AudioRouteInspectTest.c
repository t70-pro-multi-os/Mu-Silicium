// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioRouteInspect.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
static unsigned Checks,Calls,Fail,Wrong;
static int32_t Error;
#define C(X) do{assert(X);++Checks;}while(0)
typedef struct{unsigned Channel,Alive,Calls;} AMP;
typedef struct{unsigned Alive,Calls;uint32_t A,B;} ROUTE;
static int32_t AmpRead(void *Ctx,uint16_t Reg,uint16_t *V){
 AMP *M=Ctx;C(M->Alive);unsigned N=++Calls;++M->Calls;
 C(N<=4 && M->Channel==(N-1)/2 && Reg==(N%2?0:1));
 *V=Reg?0x11:(Wrong==M->Channel?0:0x2113);return N==Fail?Error:0;
}
static int32_t RouteRead(void *Ctx,uint32_t Reg,uint32_t *V){
 ROUTE *M=Ctx;C(M->Alive);C(Calls==4+M->Calls);C(Reg==0x20+M->Calls*4);
 *V=M->Calls?M->B:M->A;++M->Calls;++Calls;return Calls==Fail?Error:0;
}
int main(int argc,char **argv){
 C(argc==2);FILE *F=fopen(argv[1],"rb");C(F);uint8_t B[3857];size_t N=fread(B,1,sizeof B,F);C(!ferror(F));fclose(F);C(N==3856);
 AMP A[2]={{0,1,0},{1,1,0}};ROUTE Q={1,0,0,0};
 T70_AW_PROBE_ENDPOINT E[2]={{0x11e01000,0x34,0,{&A[0],AmpRead}},{0x11e01000,0x35,1,{&A[1],AmpRead}}};
 T70_AFE_ROUTE_READER Io={&Q,RouteRead};T70_AFE_PREPARED P;
 C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0x100000000ULL,512,256,&P));
 T70_AUDIO_ROUTE_INSPECTION Out;unsigned char Old[sizeof Out];T70_AUDIO_ROUTE_RESULT R;
#define RESET() do{memset(&Out,0x5a,sizeof Out);memcpy(Old,&Out,sizeof Out);Calls=Fail=0;Wrong=2;A[0].Calls=A[1].Calls=Q.Calls=0;}while(0)
#define RUN() T70AudioInspectWithRoute(&P,B,N,&E[0],&E[1],Profile,&Io,&Out)
#define KEEP() C(!memcmp(Old,&Out,sizeof Out))
 for(uint16_t Profile=0;Profile<=10;Profile+=10){
 for(unsigned Bits=0;Bits<4;Bits++){
 RESET();Q.A=Bits&1?32:0;Q.B=Bits&2?64:0;R=RUN();
 C(R.Code==T70AudioRouteCollected && R.AudioStarted && R.RouteStarted);C(Calls==6);
 C(Out.Route.Conn0==Q.A && Out.Route.Conn1==Q.B);
 C(Out.Route.Dl1Channel1Selected==(Bits&1));C(Out.Route.Dl1Channel2Selected==((Bits>>1)&1));
 C(T70AfeDl1PreparedValid(&Out.Audio.Dl1Values));
 for(unsigned J=0;J<2;J++){C(Out.Audio.Amplifiers.Profiles.Channel[J].Section.Profile==Profile);C(A[J].Calls==2);}
 /* Caller contexts may die after synchronous return; result retains no context. */
 A[0].Alive=A[1].Alive=Q.Alive=0;
 C(Out.Audio.Amplifiers.Observations.Channels[0].ChipId==0x2113);
 C(Out.Audio.Amplifiers.Profiles.Channel[0].RecordCount==57);
 A[0].Alive=A[1].Alive=Q.Alive=1;
 }
 const int32_t Errors[]={-73,1,INT32_MIN,INT32_MAX};
 for(unsigned Step=1;Step<=6;Step++)for(unsigned K=0;K<4;K++){
 RESET();Fail=Step;Error=Errors[K];R=RUN();KEEP();C(Calls==Step);
 C(R.Code==(Step<=4?T70AudioRouteAudioFailed:T70AudioRouteReadFailed));
 C(R.RouteStarted==(Step>4));C(Q.Calls==(Step>4?Step-4:0));
 if(Step>4)C(R.Route.TransportStatus==Error);else C(R.Audio.Amplifiers.Probe.Endpoint.TransportStatus==Error);
 }
 for(unsigned J=0;J<2;J++){RESET();Wrong=J;R=RUN();C(R.Code==T70AudioRouteAudioFailed && !R.RouteStarted && Calls==J*2+1);KEEP();}
 }
 uint16_t Profile=0;
 RESET();Io.Context=&A[0];R=RUN();C(R.Code==T70AudioRouteInvalid && !Calls);KEEP();Io.Context=&Q;
 RESET();Io.Read32=0;R=RUN();C(R.Code==T70AudioRouteInvalid && !Calls);KEEP();Io.Read32=RouteRead;
 RESET();Io.Context=0;R=RUN();C(R.Code==T70AudioRouteInvalid && !Calls);KEEP();Io.Context=&Q;
 RESET();P.Format.RateCode^=1;R=RUN();C(R.Code==T70AudioRouteAudioFailed && !R.RouteStarted && !Calls);KEEP();P.Format.RateCode^=1;
 RESET();E[1].Address=0x34;R=RUN();C(R.Code==T70AudioRouteAudioFailed && !Calls);KEEP();E[1].Address=0x35;
 RESET();R=T70AudioInspectWithRoute(&P,B,N,&E[0],&E[1],0,&Io,0);C(R.Code==T70AudioRouteInvalid && !Calls);
 for(size_t I=0;I<N;I++){RESET();R=T70AudioInspectWithRoute(&P,B,I,&E[0],&E[1],0,&Io,&Out);C(R.Code==T70AudioRouteAudioFailed && !Calls && !R.RouteStarted);KEEP();}
 printf("AudioRouteInspectTest: %u checks; mock reads only\n",Checks);
}
