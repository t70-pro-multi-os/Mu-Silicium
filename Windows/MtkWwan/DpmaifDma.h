/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T70_DPMAIF_DMA_H
#define T70_DPMAIF_DMA_H

#include <stdint.h>

/* ccci_hif_dpmaif_v3.c sets dpmaif_dmamask = DMA_BIT_MASK(36). The
 * descriptor format has 40 address bits, but this platform may only submit
 * addresses returned within the 36-bit DMA aperture. */
#define T70_DPMAIF_DMA_MASK_BITS 36U
#define T70_DPMAIF_DMA_MASK_LIMIT (UINT64_C(1) << T70_DPMAIF_DMA_MASK_BITS)

static inline int
T70DpmaifDmaRangeValid(uint64_t Address, uint64_t Bytes)
{
  return Bytes != 0U && Address < T70_DPMAIF_DMA_MASK_LIMIT &&
      Bytes <= T70_DPMAIF_DMA_MASK_LIMIT - Address;
}

#endif
