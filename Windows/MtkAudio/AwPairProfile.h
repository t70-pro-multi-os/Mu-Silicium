// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_PAIR_PROFILE_H
#define T70_AW_PAIR_PROFILE_H
#include "AwProfile.h"
typedef struct {
  T70_AW_PROFILE_VIEW Channel[2];
  uint32_t RegisterProfileCount;
} T70_AW_PAIR_PROFILE_VIEW;
/* Read-only, single immutable ACF, explicit board addresses in channel 0/1
 * order (not an assertion of physical left/right). Require the observed two
 * register profiles 0/10 for BOTH channels, validate every selected inner bin,
 * then publish both requested views together. No equality requirement on
 * register values/counts across channels: channel tuning may differ.
 * This deliberately strict catalog policy is not a hardware applicability test.
 * Type 10 metadata is container-validated, not interpreted as register data.
 * Borrowed views require ACF lifetime/immutability. Output must not overlap ACF;
 * failure leaves it entirely unchanged. No I/O, allocation or retained state. */
int T70AwPairProfileView(const void *Acf,size_t Bytes,uint64_t Base,
                         uint16_t Address0,uint16_t Address1,uint16_t Profile,
                         T70_AW_PAIR_PROFILE_VIEW *View);
#endif
