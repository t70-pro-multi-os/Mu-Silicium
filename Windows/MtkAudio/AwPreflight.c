// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwPreflight.h"
static uint16_t U16(const uint8_t *P) {
  return (uint16_t)(P[0]|((uint16_t)P[1]<<8));
}
static int Special(uint16_t A) {
  return A==0x04 || A==0x05 || A==0x08 || A==0x14 || A==0x50;
}
int T70AwPreflightCount(const void *Bin,size_t Bytes,uint32_t *Count) {
  const uint8_t *B=Bin;uint32_t N,Found=0;
  if(!Count || !T70AwRegisterBinValidate(Bin,Bytes,&N))return 0;
  for(uint32_t I=0;I<N;I++)if(Special(U16(B+64+(size_t)I*4)))Found++;
  *Count=Found;return 1;
}
int T70AwPreflightEntry(const void *Bin,size_t Bytes,uint32_t Ordinal,
                        T70_AW_PREFLIGHT_ENTRY *Entry) {
  const uint8_t *B=Bin;uint32_t N,Found=0;
  if(!Entry || !T70AwRegisterBinValidate(Bin,Bytes,&N))return 0;
  for(uint32_t I=0;I<N;I++) {
    size_t Offset=64+(size_t)I*4;
    if(!Special(U16(B+Offset)))continue;
    if(Found++==Ordinal) {
      T70_AW_PREFLIGHT_ENTRY R={I,Offset,{U16(B+Offset),U16(B+Offset+2)}};
      *Entry=R;return 1;
    }
  }
  return 0;
}
