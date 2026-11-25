// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WavePrepare.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks;
#define C(X) do{assert(X);++Checks;}while(0)
static void P16(uint8_t *B,unsigned V){B[0]=(uint8_t)V;B[1]=(uint8_t)(V>>8);}
static void P32(uint8_t *B,uint32_t V){P16(B,V);P16(B+2,V>>16);}
static void Build(uint8_t *B,uint32_t Rate,unsigned Ch,unsigned Bits){
 const uint8_t Guid[]={1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
 memset(B,0,40);P16(B,0xfffe);P16(B+2,Ch);P32(B+4,Rate);
 unsigned Width=Bits==16?16:32;P32(B+8,Rate*Ch*(Width/8));P16(B+12,Ch*(Width/8));P16(B+14,Width);P16(B+16,22);P16(B+18,Bits);P32(B+20,Ch==1?4:3);memcpy(B+24,Guid,16);
}
int main(void){
 uint8_t Storage[42],Good[40];uint8_t *B=Storage+1;T70_AFE_PREPARED O;unsigned char Before[sizeof O];
 const uint32_t Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
#define RESET() do{memset(&O,0xa5,sizeof O);memcpy(Before,&O,sizeof O);}while(0)
#define RUN() T70WaveExtensiblePrepare(B,40,0x100000000ULL,512,256,&O)
#define KEEP() C(!memcmp(Before,&O,sizeof O))
 for(unsigned R=0;R<11;R++)for(unsigned Ch=1;Ch<=2;Ch++)for(unsigned Bits=16;Bits<=24;Bits+=8){
 Build(B,Rates[R],Ch,Bits);RESET();T70_WAVE_PREPARE_RESULT Result=RUN();
 if(Bits==24){C(Result==T70WaveNeeds24BitNormalization);KEEP();}
 else{C(Result==T70WavePrepared);C(T70AfeDl1PreparedValid(&O));C(O.Rate==Rates[R] && O.Channels==Ch && O.SampleFormat==T70AfeS16Le);}
 }
 Build(Good,48000,2,16);
 for(size_t N=0;N<=42;N++)if(N!=40){memcpy(B,Good,40);RESET();C(T70WaveExtensiblePrepare(B,N,0,512,256,&O)==T70WaveInvalid);KEEP();}
 for(unsigned V=0;V<=65535;V++){
 memcpy(B,Good,40);P16(B+16,V);RESET();C((RUN()==T70WavePrepared)==(V==22));if(V!=22)KEEP();
 memcpy(B,Good,40);P16(B+2,V);RESET();C((RUN()==T70WavePrepared)==(V==2));if(V!=2)KEEP();
 memcpy(B,Good,40);P32(B+20,V);RESET();C((RUN()==T70WavePrepared)==(V==3));if(V!=3)KEEP();
 }
 for(unsigned Bit=0;Bit<128;Bit++){memcpy(B,Good,40);B[24+Bit/8]^=1u<<(Bit%8);RESET();C(RUN()==T70WaveInvalid);KEEP();}
 const unsigned Fields[]={0,8,12,14,18};
 for(unsigned I=0;I<5;I++){memcpy(B,Good,40);B[Fields[I]]^=1;RESET();C(RUN()==T70WaveInvalid);KEEP();}
 const uint32_t BadRates[]={0,1,48001,384000,UINT32_MAX};
 for(unsigned I=0;I<5;I++){Build(B,BadRates[I],2,24);RESET();C(RUN()==T70WaveInvalid);KEEP();}
 Build(B,48000,2,24);P16(B+18,32);RESET();C(RUN()==T70WaveInvalid);KEEP();
 Build(B,48000,2,24);P16(B+14,24);P16(B+12,6);P32(B+8,288000);RESET();C(RUN()==T70WaveInvalid);KEEP();
 Build(B,48000,2,24);RESET();C(T70WaveExtensiblePrepare(B,40,UINT64_MAX,512,256,&O)==T70WaveInvalid);KEEP();
 memcpy(B,Good,40);RESET();C(T70WaveExtensiblePrepare(B,40,0,512,0,&O)==T70WaveInvalid);KEEP();
 C(T70WaveExtensiblePrepare(NULL,40,0,512,256,&O)==T70WaveInvalid);KEEP();
 C(T70WaveExtensiblePrepare(B,40,0,512,256,NULL)==T70WaveInvalid);
 C(T70WaveExtensiblePrepare(B,SIZE_MAX,0,512,256,&O)==T70WaveInvalid);KEEP();
 printf("WavePrepareTest: %u checks; byte parsing only\n",Checks);
}
