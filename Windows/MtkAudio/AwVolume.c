// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwVolume.h"
uint16_t T70Aw88261DecodeInitialVolume(uint16_t RegisterValue) {
  uint16_t Field=(uint16_t)(RegisterValue&0x03ff);
  return (uint16_t)((Field>>6)*48+(Field&63));
}
int T70Aw88261EncodeAttenuation(uint16_t Attenuation,uint16_t *Field) {
  if(!Field || Attenuation>T70_AW88261_ATTENUATION_LIMIT)return 0;
  *Field=(uint16_t)((Attenuation/48)*64+Attenuation%48);return 1;
}
uint16_t T70Aw88261ComposeVolume(uint16_t ExistingRegister,uint16_t Initial,
                                uint32_t Requested) {
  uint16_t Attenuation,Field;
  if(Initial>=T70_AW88261_ATTENUATION_LIMIT ||
     Requested>=(uint32_t)(T70_AW88261_ATTENUATION_LIMIT-Initial))
    Attenuation=T70_AW88261_ATTENUATION_LIMIT;
  else Attenuation=(uint16_t)(Initial+Requested);
  T70Aw88261EncodeAttenuation(Attenuation,&Field);
  return (uint16_t)((ExistingRegister&T70_AW88261_VOLUME_PRESERVE_MASK)|Field);
}
