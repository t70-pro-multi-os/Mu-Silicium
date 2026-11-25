// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwRegisters.h"
static uint32_t U32(const uint8_t *P) {
  return (uint32_t)P[0]|((uint32_t)P[1]<<8)|((uint32_t)P[2]<<16)|((uint32_t)P[3]<<24);
}
static uint16_t U16(const uint8_t *P) { return (uint16_t)(P[0]|((uint16_t)P[1]<<8)); }
int T70AwRegisterBinValidate(const void *Buffer,size_t Bytes,uint32_t *Count) {
  const uint8_t *B=Buffer;uint32_t Sum=0,N;
  if(!B || !Count || Bytes<64 || Bytes>UINT32_MAX ||
     U32(B+4)!=0x01000000 || U32(B+8)!=0 || U32(B+12)!=1 ||
     U32(B+16)!=Bytes-60 || U32(B+32)!=2 || U32(B+36)!=2) return 0;
  /* Stock profiles use both aw88261 and AW88261 in the inner header. */
  if(!((B[24]=='a'&&B[25]=='w')||(B[24]=='A'&&B[25]=='W')) ||
     B[26]!='8'||B[27]!='8'||B[28]!='2'||B[29]!='6'||B[30]!='1'||B[31]!=0) return 0;
  N=U32(B+60);
  if(!N || N!=(Bytes-64)/4 || (Bytes-64)%4) return 0;
  for(size_t I=4;I<Bytes;I++)Sum+=B[I];
  if(Sum!=U32(B))return 0;
  *Count=N;return 1;
}
int T70AwAcfRegister(const void *Buffer,size_t Bytes,uint16_t DeviceIndex,
                    uint16_t Profile,uint32_t Index,T70_AW_REGISTER *Register) {
  T70_AW_SECTION S;uint32_t N;
  if(!Register || !T70AwAcfFind(Buffer,Bytes,DeviceIndex,Profile,4,&S) ||
     !T70AwRegisterBinValidate(S.Data,S.Length,&N) || Index>=N)return 0;
  const uint8_t *P=S.Data+64+(size_t)Index*4;
  T70_AW_REGISTER R={U16(P),U16(P+2)};*Register=R;return 1;
}
