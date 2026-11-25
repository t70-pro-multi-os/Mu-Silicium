/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T70_DPMAIF_SG_CORE_H
#define T70_DPMAIF_SG_CORE_H

#include <stdint.h>
#include "DpmaifTxPacket.h"

/* Validate a WDM SCATTER_GATHER_LIST copied into Segments and coalesce
 * physically adjacent pieces in place. Any failure leaves Segments and
 * NormalizedCount unchanged. */
T70_DPMAIF_RING_STATUS T70DpmaifNormalizeSgSegments(
  T70_DPMAIF_TX_SEGMENT *Segments,
  uint32_t InputCount,
  uint32_t ExpectedBytes,
  uint32_t Capacity,
  uint32_t *NormalizedCount
  );

#endif
