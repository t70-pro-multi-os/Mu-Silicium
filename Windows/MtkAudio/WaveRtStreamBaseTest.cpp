// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtStreamBase.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>
static unsigned Checks,Irql;
#define C(X) do{Checks++;assert(X);}while(0)
unsigned char KeGetCurrentIrql(void){return (unsigned char)Irql;}
static_assert(std::is_abstract<T70WaveRtStreamBase>::value,"provider must be implemented");
class Fake final:public T70WaveRtStreamBase{
public:explicit Fake(const T70_AFE_PREPARED &P):T70WaveRtStreamBase(P){}
 NTSTATUS QueryInterface(const GUID &,void **) override{return STATUS_NOT_SUPPORTED;}
 ULONG AddRef() override{return 1;} ULONG Release() override{return 1;}
 NTSTATUS AllocateAudioBuffer(ULONG,PMDL *,ULONG *,ULONG *,MEMORY_CACHING_TYPE *) override{return STATUS_DEVICE_NOT_READY;}
 void FreeAudioBuffer(PMDL,ULONG) override{}
 void GetHWLatency(KSRTAUDIO_HWLATENCY *) override{assert(false);}
};
static void W16(uint8_t *B,unsigned V){B[0]=(uint8_t)V;B[1]=(uint8_t)(V>>8);}
static void W32(uint8_t *B,uint32_t V){W16(B,V);W16(B+2,V>>16);}
static void Format(uint8_t *B,unsigned Rate,unsigned Valid){
 const uint8_t Audio[]={0x61,0x75,0x64,0x73,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
 const uint8_t Pcm[]={1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
 const uint8_t Wave[]={0x81,0x9f,0x58,5,0x56,0xc3,0xce,0x11,0xbf,1,0,0xaa,0,0x55,0x59,0x5a};
 unsigned A=Valid==16?4:8;memset(B,0,104);W32(B,104);W32(B+8,A);
 memcpy(B+16,Audio,16);memcpy(B+32,Pcm,16);memcpy(B+48,Wave,16);
 W16(B+64,0xfffe);W16(B+66,2);W32(B+68,Rate);W32(B+72,Rate*A);W16(B+76,A);W16(B+78,Valid==16?16:32);W16(B+80,22);W16(B+82,Valid);W32(B+84,3);memcpy(B+88,Pcm,16);
}
int main(){
 T70_AFE_PREPARED P,O;C(T70AfeDl1Prepare(48000,T70AfeS16Le,2,0,512,256,&P));Fake S(P);IMiniportWaveRTStream *I=&S;
 alignas(KSDATAFORMAT) uint8_t B[104];Format(B,44100,16);
 C(I->SetFormat((PKSDATAFORMAT)B)==STATUS_SUCCESS);C(S.CopyPlan(&O));C(O.Rate==44100&&T70AfeDl1PreparedValid(&O));
 for(unsigned V=0;V<=65535;V++){
 Format(B,48000,16);W16(B+80,V);NTSTATUS R=I->SetFormat((PKSDATAFORMAT)B);
 C((R==STATUS_SUCCESS)==(V==22));C(S.CopyPlan(&O));C(O.Rate==(V<22?44100u:48000u));
 }
 Format(B,48000,24);C(I->SetFormat((PKSDATAFORMAT)B)==STATUS_NOT_SUPPORTED);C(S.CopyPlan(&O)&&O.SampleFormat==T70AfeS16Le);
 C(I->SetFormat(nullptr)==STATUS_INVALID_PARAMETER);
 for(int State=0;State<=3;State++)C(I->SetState((KSSTATE)State)==(State==0?STATUS_SUCCESS:STATUS_NOT_SUPPORTED));
 KSAUDIO_POSITION Pos={0x123,0x456};C(I->GetPosition(&Pos)==STATUS_DEVICE_NOT_READY);C(Pos.PlayOffset==0x123&&Pos.WriteOffset==0x456);
 KSRTAUDIO_HWREGISTER R,Before;memset(&R,0xa5,sizeof R);Before=R;
 C(I->GetPositionRegister(&R)==STATUS_NOT_SUPPORTED);C(!memcmp(&R,&Before,sizeof R));C(I->GetClockRegister(&R)==STATUS_NOT_SUPPORTED);C(!memcmp(&R,&Before,sizeof R));
 C(I->GetPosition(nullptr)==STATUS_INVALID_PARAMETER);C(I->GetClockRegister(nullptr)==STATUS_INVALID_PARAMETER);C(I->GetPositionRegister(nullptr)==STATUS_INVALID_PARAMETER);
 Irql=2;C(I->SetState(KSSTATE_STOP)==STATUS_INVALID_PARAMETER);C(I->SetFormat((PKSDATAFORMAT)B)==STATUS_INVALID_PARAMETER);C(I->GetPosition(&Pos)==STATUS_DEVICE_NOT_READY);C(!S.CopyPlan(&O));Irql=0;
 // Exhaustive mock KIRQL byte values: no unsupported-level hardware work,
 // no invented zero/play=write output even at a legitimate dispatch query.
 for(unsigned Level=0;Level<=255;Level++){
  Irql=Level;Pos={0x123,0x456};
  C(I->GetPosition(&Pos)==((Level==PASSIVE_LEVEL||Level==DISPATCH_LEVEL)?STATUS_DEVICE_NOT_READY:STATUS_INVALID_PARAMETER));
  C(Pos.PlayOffset==0x123&&Pos.WriteOffset==0x456);
  C(I->GetPosition(nullptr)==STATUS_INVALID_PARAMETER);
 }
 Irql=0;
 T70_AFE_PREPARED Bad={};Fake Invalid(Bad);C(Invalid.SetState(KSSTATE_STOP)==STATUS_INVALID_DEVICE_STATE);C(Invalid.SetFormat((PKSDATAFORMAT)B)==STATUS_INVALID_DEVICE_STATE);C(!Invalid.CopyPlan(&O));
 C(T70AfeDl1Prepare(48000,T70AfeS24Le32,2,0,512,256,&Bad));Fake Unsupported(Bad);C(Unsupported.SetState(KSSTATE_STOP)==STATUS_INVALID_DEVICE_STATE);
 printf("WaveRT interface: %u checks; fake provider, no stream registration\n",Checks);
}
