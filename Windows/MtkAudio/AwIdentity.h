// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_IDENTITY_H
#define T70_AW_IDENTITY_H
#include "AwMask.h"
#define T70_AW88261_ID_REGISTER UINT16_C(0x00)
#define T70_AW88261_CHIP_ID UINT16_C(0x2113)
/* Read-only identification using the stock AW88261 ID contract. One read, no
 * retry/reset/writes. Output is unchanged on invalid input, read error or wrong
 * identity. A matching ID is not proof of board position, power readiness,
 * calibration or permission to apply ACF records. */
int T70Aw88261Identify(const T70_AW_REGISTER_IO *Io,uint16_t *ChipId);
#endif
