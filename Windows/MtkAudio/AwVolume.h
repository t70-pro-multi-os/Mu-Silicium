// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_VOLUME_H
#define T70_AW_VOLUME_H
#include <stdint.h>
#define T70_AW88261_VOLUME_REGISTER UINT16_C(0x05)
#define T70_AW88261_VOLUME_PRESERVE_MASK UINT16_C(0xfc00)
#define T70_AW88261_ATTENUATION_LIMIT UINT16_C(720)
/* Pure arithmetic only; no I/O, mute/power transition or ownership claim.
 * Units are 1/8 dB attenuation, NOT a percentage or signed dB gain.
 * Decode matches profile init-volume extraction. It is not the stock live
 * getter, which has a separate raw-field clamp before decoding. */
uint16_t T70Aw88261DecodeInitialVolume(uint16_t RegisterValue);
/* Canonical encoding for 0..720 only; invalid input leaves output unchanged. */
int T70Aw88261EncodeAttenuation(uint16_t Attenuation,uint16_t *Field);
/* Match setter arithmetic: clamp Initial+Requested to 720 without overflow,
 * encode and retain existing bits 15:10. Result is data, never applied here. */
uint16_t T70Aw88261ComposeVolume(uint16_t ExistingRegister,uint16_t Initial,
                                uint32_t Requested);
#endif
