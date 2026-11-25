// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AfeIrqCount.h"
int T70AfeIrqCountField(uint64_t Frames,uint32_t *Count) {
  if(!Count || !Frames || Frames>T70_AFE_IRQ_COUNT_MAX)return 0;
  *Count=(uint32_t)Frames;return 1;
}
int T70AfeDl1IrqCount(const T70_AFE_PREPARED *Prepared,uint32_t *Count) {
  if(!Count || !T70AfeDl1PreparedValid(Prepared))return 0;
  return T70AfeIrqCountField(Prepared->Geometry.PeriodFrames,Count);
}
