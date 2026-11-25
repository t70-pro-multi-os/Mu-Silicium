// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwAcf.h"
static uint32_t U32(const uint8_t *P) {
  return (uint32_t)P[0]|((uint32_t)P[1]<<8)|((uint32_t)P[2]<<16)|((uint32_t)P[3]<<24);
}
static uint16_t U16(const uint8_t *P) { return (uint16_t)(P[0]|((uint16_t)P[1]<<8)); }
static int Equal(const uint8_t *A, const char *B, size_t N) {
  for(size_t I=0;I<N;I++) if(A[I]!=(uint8_t)B[I]) return 0;
  return 1;
}
static uint8_t Crc(const uint8_t *P,size_t N) {
  uint8_t C=0;
  for(size_t I=0;I<N;I++) {
    C^=P[I];
    for(unsigned J=0;J<8;J++) C=(uint8_t)((C>>1)^((C&1)?0x8c:0));
  }
  return C;
}
int T70AwAcfValidate(const void *Buffer,size_t Bytes,uint32_t *Count) {
  const uint8_t *B=Buffer;
  size_t Cursor; uint32_t N;
  if(!B || !Count || Bytes<80 || U32(B)!=0x0a15f908 ||
     !Equal(B+4,"T70\0",4) || U32(B+64)!=1 || U32(B+60)!=80) return 0;
  N=U32(B+56);
  /* The observed container has 12 descriptors; bounded extensions are allowed
   * only when they retain the same unambiguous device/profile/type contract. */
  if(!N || N>64 || U32(B+52)!=N*64U || (size_t)N*64U>Bytes-80) return 0;
  Cursor=80+(size_t)N*64;
  for(uint32_t I=0;I<N;I++) {
    const uint8_t *D=B+80+(size_t)I*64;
    uint32_t Length=U32(D+32),Offset=U32(D+36),Type=U32(D+28);
    if(U32(D)!=2 || !Equal(D+4,"aw88261\0",8) || U16(D+20)>3 ||
       U16(D+22)!=0xffff || U16(D+24)!=0xffff ||
       (U16(D+26)!=0 && U16(D+26)!=10) || (Type!=4 && Type!=10) ||
       !Length || Offset!=Cursor || Length>Bytes-Cursor || U32(D+40)>255 ||
       Crc(B+Cursor,Length)!=U32(D+40)) return 0;
    for(uint32_t J=0;J<I;J++) {
      const uint8_t *P=B+80+(size_t)J*64;
      if(U16(P+20)==U16(D+20) && U16(P+26)==U16(D+26) && U32(P+28)==Type) return 0;
    }
    Cursor+=Length;
  }
  if(Cursor!=Bytes) return 0;
  *Count=N;return 1;
}
int T70AwAcfFind(const void *Buffer,size_t Bytes,uint16_t DeviceIndex,
                uint16_t Profile,uint32_t Type,T70_AW_SECTION *Section) {
  uint32_t N; const uint8_t *B=Buffer;
  if(!Section || !T70AwAcfValidate(Buffer,Bytes,&N)) return 0;
  for(uint32_t I=0;I<N;I++) {
    const uint8_t *D=B+80+(size_t)I*64;
    if(U16(D+20)==DeviceIndex && U16(D+26)==Profile && U32(D+28)==Type) {
      T70_AW_SECTION S={DeviceIndex,Profile,Type,B+U32(D+36),U32(D+32)};
      *Section=S;return 1;
    }
  }
  return 0;
}
