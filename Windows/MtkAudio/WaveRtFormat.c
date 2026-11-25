// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WaveRtFormat.h"
#include <ntddk.h>
#include <portcls.h>
#include <ks.h>
#include <ksmedia.h>
#include <string.h>
_Static_assert(sizeof(KSDATAFORMAT)==64,"KS format header");
_Static_assert(offsetof(KSDATAFORMAT,MajorFormat)==16,"KS major offset");
_Static_assert(offsetof(KSDATAFORMAT,SubFormat)==32,"KS subtype offset");
_Static_assert(offsetof(KSDATAFORMAT,Specifier)==48,"KS specifier offset");
_Static_assert(offsetof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE,WaveFormatExt)==64,"wave offset");
_Static_assert(sizeof(WAVEFORMATEX)==18,"plain wave descriptor size");
_Static_assert(sizeof(KSDATAFORMAT_WAVEFORMATEX)==82,"packed KS PCM descriptor size");
_Static_assert(offsetof(KSDATAFORMAT_WAVEFORMATEX,WaveFormatEx)==64,"plain wave offset");
_Static_assert(sizeof(WAVEFORMATEXTENSIBLE)==40,"wave descriptor size");
static uint32_t U32(const uint8_t *B){return (uint32_t)B[0]|((uint32_t)B[1]<<8)|((uint32_t)B[2]<<16)|((uint32_t)B[3]<<24);}
T70_WAVE_PREPARE_RESULT T70WaveRtPrepareBufferFormat(const void *Data,size_t N,
 uint64_t Base,uint64_t Bytes,uint64_t Period,T70_AFE_PREPARED *Out){
 static const GUID Audio={STATIC_KSDATAFORMAT_TYPE_AUDIO};
 static const GUID Pcm={STATIC_KSDATAFORMAT_SUBTYPE_PCM};
 static const GUID Wave={STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX};
 uintptr_t A=(uintptr_t)Data,O=(uintptr_t)Out;
 if(!Data||!Out||(N!=82&&N!=104)||N>UINTPTR_MAX-A||sizeof(*Out)>UINTPTR_MAX-O||
    O%_Alignof(T70_AFE_PREPARED)||(A<O+sizeof(*Out)&&O<A+N))return T70WaveInvalid;
 const uint8_t *B=Data;
 if(U32(B)!=N||U32(B+4)||U32(B+12)||memcmp(B+16,&Audio,16)||
    memcmp(B+32,&Pcm,16)||memcmp(B+48,&Wave,16))return T70WaveInvalid;
 /* Fixed PCM sample is one complete interleaved frame. */
 uint32_t Align=(uint32_t)B[76]|((uint32_t)B[77]<<8);
 if(U32(B+8)!=0&&U32(B+8)!=Align)return T70WaveInvalid;
 uint8_t Canonical[40];
 const uint8_t *Format=B+64;
 if(N==82){
  // WDM PCM uses packed WAVEFORMATEX, not obsolete PCMWAVEFORMAT.
  // PCM cbSize is implicitly zero (Microsoft mmreg contract): never use its
  // supplied value to read beyond these 18 bytes. Only S16 mono/stereo is added.
  uint32_t Channels=(uint32_t)B[66]|((uint32_t)B[67]<<8);
  if(B[64]!=1||B[65]||Channels<1||Channels>2||B[78]!=16||B[79])return T70WaveInvalid;
  memset(Canonical,0,sizeof Canonical);memcpy(Canonical,Format,18);
  Canonical[0]=0xfe;Canonical[1]=0xff;Canonical[16]=22;Canonical[17]=0;
  Canonical[18]=16;Canonical[20]=Channels==1?4:3;memcpy(Canonical+24,&Pcm,16);
  Format=Canonical;
 }
 T70_AFE_PREPARED P;
 T70_WAVE_PREPARE_RESULT R=T70WaveExtensiblePrepare(Format,40,Base,Bytes,Period,&P);
 if(R==T70WavePrepared)*Out=P;
 return R;
}
