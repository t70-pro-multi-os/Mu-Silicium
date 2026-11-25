// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "WavePrepare.h"
#include <string.h>
static uint16_t U16(const uint8_t *P){return (uint16_t)((uint16_t)P[0]|((uint16_t)P[1]<<8));}
static uint32_t U32(const uint8_t *P){return (uint32_t)U16(P)|((uint32_t)U16(P+2)<<16);}
T70_WAVE_PREPARE_RESULT T70WaveExtensiblePrepare(const void *Descriptor,size_t Bytes,
 uint64_t DmaBase,uint64_t BufferBytes,uint64_t PeriodBytes,T70_AFE_PREPARED *Out){
 static const uint8_t PcmGuid[16]={1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
 if(!Descriptor || !Out || Bytes!=40)return T70WaveInvalid;
 const uint8_t *B=Descriptor;
 if(U16(B)!=0xfffe || U16(B+16)!=22 || memcmp(B+24,PcmGuid,16))return T70WaveInvalid;
 uint32_t Channels=U16(B+2),Rate=U32(B+4),Mask=U32(B+20);
 uint32_t Container=U16(B+14),Valid=U16(B+18),Format;
 if((Channels!=1 || Mask!=4) && (Channels!=2 || Mask!=3))return T70WaveInvalid;
 if(Container==16 && Valid==16)Format=T70AfeS16Le;
 else if(Container==32 && Valid==24)Format=T70AfeS24Le32;
 else return T70WaveInvalid;
 uint32_t Align=Channels*(Container/8);
 uint64_t ByteRate=(uint64_t)Rate*Align;
 if(U16(B+12)!=Align || ByteRate>UINT32_MAX || U32(B+8)!=ByteRate)return T70WaveInvalid;
 T70_AFE_PREPARED Prepared;
 if(!T70AfeDl1Prepare(Rate,Format,Channels,DmaBase,BufferBytes,PeriodBytes,&Prepared))return T70WaveInvalid;
 if(Format==T70AfeS24Le32)return T70WaveNeeds24BitNormalization;
 *Out=Prepared;return T70WavePrepared;
}
