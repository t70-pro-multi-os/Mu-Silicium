// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AudioInspect.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks,Calls,Fail,Wrong;
#define C(X) do{assert(X);Checks++;}while(0)
typedef struct{unsigned Channel;}MOCK;
static int32_t Read(void *Ctx,uint16_t Reg,uint16_t *Value){
 MOCK *M=Ctx;unsigned N=++Calls;C(N<=4);C(M->Channel==(N-1)/2);C(Reg==(N%2?0:1));
 *Value=Reg?0x11:(Wrong==M->Channel?0:0x2113);return N==Fail?-73:0;
}
int main(int argc,char **argv){
 C(argc==2);FILE *F=fopen(argv[1],"rb");C(F);uint8_t B[3857];size_t N=fread(B,1,sizeof B,F);C(!ferror(F));fclose(F);C(N==3856);
 MOCK M[2]={{0},{1}};T70_AW_PROBE_ENDPOINT E[2]={{0x11e01000,0x34,0,{&M[0],Read}},{0x11e01000,0x35,1,{&M[1],Read}}};
 T70_AFE_PREPARED P;C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0x100000000ULL,512,256,&P));
 T70_AUDIO_INSPECTION Out;unsigned char Old[sizeof Out];T70_AUDIO_INSPECT_RESULT R;
#define RESET() do{memset(&Out,0x5a,sizeof Out);memcpy(Old,&Out,sizeof Out);Calls=0;Fail=0;Wrong=2;}while(0)
#define UNCHANGED() C(!memcmp(Old,&Out,sizeof Out))
 for(unsigned Profile=0;Profile<=10;Profile+=10){
 RESET();R=T70AudioInspect(&P,B,N,&E[0],&E[1],(uint16_t)Profile,&Out);
 C(R.Code==T70AudioInspectCollected);C(Calls==4);C(Out.Dl1Values.DmaBase==P.DmaBase);C(T70AfeDl1PreparedValid(&Out.Dl1Values));
 for(unsigned I=0;I<2;I++){C(Out.Amplifiers.Profiles.Channel[I].Section.Profile==Profile);C(Out.Amplifiers.Observations.Channels[I].ChipId==0x2113);}
 for(unsigned Step=1;Step<=4;Step++){
 RESET();Fail=Step;R=T70AudioInspect(&P,B,N,&E[0],&E[1],(uint16_t)Profile,&Out);
 C(R.Code==T70AudioInspectAmplifiersFailed);C(R.AmplifiersStarted);C(Calls==Step);C(R.Amplifiers.Probe.Endpoint.TransportStatus==-73);UNCHANGED();
 }
 for(unsigned I=0;I<2;I++){RESET();Wrong=I;R=T70AudioInspect(&P,B,N,&E[0],&E[1],(uint16_t)Profile,&Out);C(R.Code==T70AudioInspectAmplifiersFailed);C(Calls==I*2+1);UNCHANGED();}
 }
#define BAD(Field) do{RESET();T70_AFE_PREPARED Q=P;Q.Field^=1;R=T70AudioInspect(&Q,B,N,&E[0],&E[1],0,&Out);C(R.Code==T70AudioInspectInvalid);C(!R.AmplifiersStarted&&!Calls);UNCHANGED();}while(0)
 BAD(Format.RateCode);BAD(Format.HdMode);BAD(Format.AlignmentCode);BAD(Format.Mono);BAD(Format.SampleBytes);BAD(Format.FrameBytes);
 BAD(Geometry.EndInclusive);BAD(Geometry.BaseLow);BAD(Geometry.BaseHigh);BAD(Geometry.EndLow);BAD(Geometry.EndHigh);BAD(Geometry.BufferFrames);BAD(Geometry.PeriodFrames);BAD(Geometry.Periods);
 for(size_t I=0;I<N;I++){RESET();R=T70AudioInspect(&P,B,I,&E[0],&E[1],0,&Out);C(R.Code==T70AudioInspectAmplifiersFailed);C(!Calls);UNCHANGED();}
 RESET();R=T70AudioInspect(NULL,B,N,&E[0],&E[1],0,&Out);C(R.Code==T70AudioInspectInvalid);C(!Calls);UNCHANGED();
 RESET();R=T70AudioInspect(&P,B,N,&E[0],&E[1],0,NULL);C(R.Code==T70AudioInspectInvalid);C(!Calls);
 RESET();R=T70AudioInspect(&P,B,N,&E[0],NULL,0,&Out);C(R.Code==T70AudioInspectAmplifiersFailed);C(!Calls);UNCHANGED();
 RESET();E[1].Io.Context=E[0].Io.Context;R=T70AudioInspect(&P,B,N,&E[0],&E[1],0,&Out);C(R.Code==T70AudioInspectAmplifiersFailed);C(!Calls);UNCHANGED();
 printf("AudioInspectTest: %u checks; mock amp reads, no AFE I/O\n",Checks);
}
