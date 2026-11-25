// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_TX_FIELD_H
#define T70_AW_TX_FIELD_H
#include <stdint.h>
#define T70_AW88261_TX_REGISTER UINT16_C(0x08)
#define T70_AW88261_TX_PRESERVE_MASK UINT16_C(0xffbf)
/* Pure value composition for the stock 2113 TX descriptor. Enabled must be
 * exactly 0 or 1. Set bit 6 means enabled; all other bits are retained.
 * Invalid input leaves output unchanged. No hardware I/O or state change;
 * this does not determine when TX should be enabled or identify register 0x13. */
int T70Aw88261ComposeTxField(uint16_t Existing,unsigned Enabled,uint16_t *Result);
#endif
