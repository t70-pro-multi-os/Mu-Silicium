// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_PROFILE_H
#define T70_AW_PROFILE_H
#include "AwPreflight.h"
typedef struct {
  T70_AW_SECTION Section;
  uint32_t RecordCount, SpecialCount;
} T70_AW_PROFILE_VIEW;
/* Select an explicit board tuple, validate ACF and selected bin, inventory
 * known special records. No I/O, allocation or applicability verdict.
 * Section.Data is BORROWED: Acf must remain alive and immutable for the entire
 * view lifetime, including later T70AwPreflightEntry calls. Output must not
 * overlap input; unchanged on failure. No internal state retains the view.
 * Only stock board channels 0/1 are accepted; ACF indices 2/3 are NOT devices
 * discovered on this board. Identity is supplied by a trusted caller. */
int T70AwProfileView(const void *Acf,size_t Bytes,uint64_t ControllerBase,
                     uint16_t Address,uint16_t SoundChannel,uint16_t Profile,
                     T70_AW_PROFILE_VIEW *View);
#endif
