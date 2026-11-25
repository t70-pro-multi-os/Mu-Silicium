// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AwTxField.h"
int T70Aw88261ComposeTxField(uint16_t Existing,unsigned Enabled,uint16_t *Result) {
  if(!Result || Enabled>1)return 0;
  *Result=(uint16_t)((Existing&T70_AW88261_TX_PRESERVE_MASK)|(Enabled?0x0040:0));
  return 1;
}
