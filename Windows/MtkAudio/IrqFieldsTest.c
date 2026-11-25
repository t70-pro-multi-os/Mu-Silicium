// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeIrqFields.h"
#include <assert.h>
#include <stdio.h>
#include <limits.h>
static unsigned Checks;
#define C(X) do{assert(X);Checks++;}while(0)
typedef struct {unsigned Calls,Fail;int32_t Error;uint32_t Reg[2],Value[2];} MOCK;
static int32_t Read(void *Ctx,uint32_t O,uint32_t *V){MOCK *M=Ctx;unsigned N=++M->Calls;C(N==1||N==3);C(O==M->Reg[(N-1)/2]);*V=M->Value[(N-1)/2];return N==M->Fail?M->Error:0;}
static int32_t Write(void *Ctx,uint32_t O,uint32_t V){MOCK *M=Ctx;unsigned N=++M->Calls;C(N==2||N==4);C(O==M->Reg[(N-1)/2]);M->Value[(N-1)/2]=V;return N==M->Fail?M->Error:0;}
int main(void){
 const uint32_t Off[]={0x300,0x3ac,0x3b0,0x3e4,0x3e8,0x3bc,0x304,0x3dc,0x308,0x600,0x604,0x3ec,0x40c,0x608,0x60c,0x610,0x614,0x618,0x61c,0x620,0x624,0x628,0x62c,0x630,0x634,0x638,0x63c};
 const uint32_t Fs[]={0x2e4,0x2e8,0x2f0,0x2f4},Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
 const int32_t Errors[]={-1,1,INT32_MIN,INT32_MAX};
 T70_AFE_PREPARED P;
 for(unsigned Id=0;Id<27;Id++)for(unsigned Rate=0;Rate<11;Rate++){
 C(T70AfeDl1Prepare(Rates[Rate],T70AfeS16Le,2,0x100000000ULL,512,256,&P));
 T70_AFE_IRQ_DESCRIPTOR D={Id,Off[Id],0,0x3ffff,Fs[Id/8],(Id%8)*4,15};
 for(unsigned Pattern=0;Pattern<32;Pattern++)for(unsigned Fail=0;Fail<=4;Fail++)for(unsigned E=0;E<4;E++){
 uint32_t Initial=(UINT32_C(1)<<Pattern)^0xa5a5a5a5;
 MOCK M={0,Fail,Errors[E],{D.CountOffset,D.RateOffset},{Initial,~Initial}};T70_AFE_FORMAT_IO Io={&M,Read,Write};
 T70_AFE_IRQ_FIELDS_RESULT R=T70AfeDl1UpdateIrqFields(&P,&D,&Io);
 unsigned Steps=Fail?Fail:4;C(M.Calls==Steps);C(R.ReadsAttempted==(Steps+1)/2);C(R.WritesAttempted==Steps/2);C(R.WritesCompleted==Steps/2-((Fail&&!(Fail%2))?1:0));
 C(R.Code==(Fail?(Fail%2?T70AfeIrqFieldsReadFailed:T70AfeIrqFieldsWriteFailed):T70AfeIrqFieldsCompleted));C(R.TransportStatus==(Fail?Errors[E]:0));C(R.FailedField==(Fail?(Fail-1)/2:2));
 C(M.Value[0]==(Steps>=2?((Initial&~0x3ffffU)|P.Geometry.PeriodFrames):Initial));
 uint32_t Mask=15U<<D.RateShift;C(M.Value[1]==(Steps>=4?((~Initial&~Mask)|(P.Format.RateCode<<D.RateShift)):~Initial));
 }
 MOCK M={0};T70_AFE_FORMAT_IO Io={&M,Read,Write};
#define BAD(F) do{T70_AFE_IRQ_DESCRIPTOR B=D;B.F^=1;C(T70AfeDl1UpdateIrqFields(&P,&B,&Io).Code==T70AfeIrqFieldsInvalid);C(!M.Calls);}while(0)
 BAD(Id);BAD(CountOffset);BAD(CountShift);BAD(CountMask);BAD(RateOffset);BAD(RateShift);BAD(RateMask);
 Io.Read32=NULL;C(T70AfeDl1UpdateIrqFields(&P,&D,&Io).Code==T70AfeIrqFieldsInvalid);C(!M.Calls);Io.Read32=Read;
 Io.Write32=NULL;C(T70AfeDl1UpdateIrqFields(&P,&D,&Io).Code==T70AfeIrqFieldsInvalid);C(!M.Calls);Io.Write32=Write;
 T70_AFE_IRQ_DESCRIPTOR Out=D;Out.Id=UINT32_MAX;C(T70AfeDl1UpdateIrqFields(&P,&Out,&Io).Code==T70AfeIrqFieldsInvalid);C(!M.Calls);
 P.Geometry.PeriodFrames++;C(T70AfeDl1UpdateIrqFields(&P,&D,&Io).Code==T70AfeIrqFieldsInvalid);C(!M.Calls);
 C(T70AfeDl1UpdateIrqFields(NULL,&D,&Io).Code==T70AfeIrqFieldsInvalid);C(T70AfeDl1UpdateIrqFields(&P,NULL,&Io).Code==T70AfeIrqFieldsInvalid);C(T70AfeDl1UpdateIrqFields(&P,&D,NULL).Code==T70AfeIrqFieldsInvalid);
 }
 printf("IrqFieldsTest: %u checks; mocks only\n",Checks);
}
