// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtFormat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned Checks;
#define C(X) do{Checks++;assert(X);}while(0)
static void W16(uint8_t *B,unsigned V){B[0]=(uint8_t)V;B[1]=(uint8_t)(V>>8);}
static void W32(uint8_t *B,uint32_t V){W16(B,V);W16(B+2,V>>16);}
static void Build(uint8_t *B,unsigned Ch,uint32_t Rate,unsigned Valid){
 const uint8_t Audio[]={0x61,0x75,0x64,0x73,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
 const uint8_t Pcm[]={1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
 const uint8_t Wave[]={0x81,0x9f,0x58,5,0x56,0xc3,0xce,0x11,0xbf,1,0,0xaa,0,0x55,0x59,0x5a};
 unsigned Width=Valid==16?16:32,Align=Ch*(Width/8);
 memset(B,0,104);W32(B,104);W32(B+8,Align);memcpy(B+16,Audio,16);memcpy(B+32,Pcm,16);memcpy(B+48,Wave,16);
 W16(B+64,0xfffe);W16(B+66,Ch);W32(B+68,Rate);W32(B+72,Rate*Align);W16(B+76,Align);W16(B+78,Width);W16(B+80,22);W16(B+82,Valid);W32(B+84,Ch==1?4:3);memcpy(B+88,Pcm,16);
}
int main(void){
 uint8_t Storage[106],Good[104];uint8_t *B=Storage+1;T70_AFE_PREPARED O,Expected;unsigned char Before[sizeof O];
#define RESET() do{memset(&O,0xa5,sizeof O);memcpy(Before,&O,sizeof O);}while(0)
#define RUN() T70WaveRtPrepareBufferFormat(B,104,0x100000000ULL,512,256,&O)
#define KEEP() C(!memcmp(Before,&O,sizeof O))
 const unsigned Rates[]={8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000};
 for(unsigned I=0;I<11;I++)for(unsigned Ch=1;Ch<=2;Ch++){
 Build(B,Ch,Rates[I],16);RESET();C(RUN()==T70WavePrepared);C(T70WaveExtensiblePrepare(B+64,40,0x100000000ULL,512,256,&Expected)==T70WavePrepared);C(O.Rate==Expected.Rate&&O.Channels==Expected.Channels&&T70AfeDl1PreparedValid(&O));
 Build(B,Ch,Rates[I],24);RESET();C(RUN()==T70WaveNeeds24BitNormalization);KEEP();
 Build(B,Ch,Rates[I],32);RESET();C(RUN()==T70WaveInvalid);KEEP();
 }
 Build(Good,2,48000,16);
 for(unsigned Bit=0;Bit<512;Bit++){memcpy(B,Good,104);B[Bit/8]^=(uint8_t)(1u<<(Bit%8));RESET();C(RUN()==(Bit==66?T70WavePrepared:T70WaveInvalid));if(Bit!=66)KEEP();}
 for(unsigned V=0;V<65536;V++){
 memcpy(B,Good,104);W32(B+4,V);RESET();C((RUN()==T70WavePrepared)==(V==0));if(V)KEEP();
 memcpy(B,Good,104);W32(B+8,V);RESET();C((RUN()==T70WavePrepared)==(V==0||V==4));if(V!=0&&V!=4)KEEP();
 }
 memcpy(B,Good,104);
 for(unsigned N=0;N<=106;N++)if(N!=104){RESET();C(T70WaveRtPrepareBufferFormat(B,N,0,512,256,&O)==T70WaveInvalid);KEEP();}
 RESET();C(T70WaveRtPrepareBufferFormat(NULL,104,0,512,256,&O)==T70WaveInvalid);KEEP();
 C(T70WaveRtPrepareBufferFormat(B,104,0,512,256,NULL)==T70WaveInvalid);
 RESET();C(T70WaveRtPrepareBufferFormat(B,104,UINT64_MAX,512,256,&O)==T70WaveInvalid);KEEP();
 RESET();C(T70WaveRtPrepareBufferFormat(B,104,0,511,256,&O)==T70WaveInvalid);KEEP();
 RESET();C(T70WaveRtPrepareBufferFormat(B,104,0,512,255,&O)==T70WaveInvalid);KEEP();
 _Alignas(T70_AFE_PREPARED) uint8_t Alias[512];memset(Alias,0xa5,sizeof Alias);memcpy(Alias,Good,104);uint8_t Copy[512];memcpy(Copy,Alias,512);
 C(T70WaveRtPrepareBufferFormat(Alias,104,0,512,256,(T70_AFE_PREPARED *)(void *)Alias)==T70WaveInvalid);C(!memcmp(Alias,Copy,512));
 C(T70WaveRtPrepareBufferFormat(B,104,0,512,256,(T70_AFE_PREPARED *)(void *)(Alias+1))==T70WaveInvalid);C(!memcmp(Alias,Copy,512));
 // Official WAVEFORMATEX descriptor is 82 bytes, not an 88-byte padded wrapper.
 uint8_t Plain[82];
 for(unsigned I=0;I<11;I++)for(unsigned Ch=1;Ch<=2;Ch++)for(unsigned Zero=0;Zero<2;Zero++){
  Build(B,Ch,Rates[I],16);C(RUN()==T70WavePrepared);Expected=O;
  memcpy(Plain,B,82);W32(Plain,82);W16(Plain+64,1);W16(Plain+80,0);if(Zero)W32(Plain+8,0);
  RESET();C(T70WaveRtPrepareBufferFormat(Plain,82,0x100000000ULL,512,256,&O)==T70WavePrepared);
  C(!memcmp(&O,&Expected,sizeof O));
 }
 for(unsigned Extra=0;Extra<65536;Extra++){
  W16(Plain+80,Extra);RESET();C(T70WaveRtPrepareBufferFormat(Plain,82,0,512,256,&O)==T70WavePrepared);
 }
 for(unsigned Case=0;Case<14;Case++){
  Build(B,2,48000,16);memcpy(Plain,B,82);W32(Plain,82);W16(Plain+64,1);W16(Plain+80,0);
  if(Case==0)W16(Plain+64,3); // float
  if(Case==1)W16(Plain+64,0xfffe); // truncated extensible
  if(Case==2)W16(Plain+78,24);
  if(Case==3)W16(Plain+78,32);
  if(Case==4)W16(Plain+66,0);
  if(Case==5)W16(Plain+66,3);
  if(Case==6)W16(Plain+76,2);
  if(Case==7)W32(Plain+72,1);
  if(Case==8)W32(Plain+68,12345);
  if(Case==9)W32(Plain+8,1);
  if(Case==10)W32(Plain,88);
  if(Case==11)Plain[16]^=1;
  if(Case==12)W32(Plain+4,1);
  if(Case==13)W32(Plain+12,1);
  RESET();C(T70WaveRtPrepareBufferFormat(Plain,82,0,512,256,&O)==T70WaveInvalid);KEEP();
 }
 Build(B,2,48000,16);W32(B,82);W16(B+64,1);W16(B+80,0);
 for(unsigned N=0;N<=106;N++)if(N!=82){RESET();C(T70WaveRtPrepareBufferFormat(B,N,0,512,256,&O)==T70WaveInvalid);KEEP();}
 memcpy(Alias,B,82);memcpy(Copy,Alias,512);
 C(T70WaveRtPrepareBufferFormat(Alias,82,0,512,256,(T70_AFE_PREPARED *)(void *)Alias)==T70WaveInvalid);C(!memcmp(Alias,Copy,512));
 printf("WaveRT KS format: %u checks; metadata only\n",Checks);return 0;
}
