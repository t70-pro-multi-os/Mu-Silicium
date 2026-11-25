/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_UFS_MEMORY_H
#define T70_UFS_MEMORY_H
#include <stdint.h>

/* Firmware-owned ranges must remain wholly outside the allocator's real RAM window. */
static inline int T70UfsRangeOutsideOwnedRam(uint64_t Address, uint64_t Bytes,
                                            uint64_t RamBase, uint64_t RamBytes)
{
  if (!Bytes || !RamBytes || Address > UINT64_MAX - Bytes ||
      RamBase > UINT64_MAX - RamBytes) return 0;
  return Address + Bytes <= RamBase || RamBase + RamBytes <= Address;
}
#endif
