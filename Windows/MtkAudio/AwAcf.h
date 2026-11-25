// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_ACF_H
#define T70_AW_ACF_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
  uint16_t DeviceIndex, Profile;
  uint32_t SectionType;
  const uint8_t *Data;
  size_t Length;
} T70_AW_SECTION;
/* Caller owns an immutable buffer for the duration of validation and use.
 * Returns 1 only for the observed T70 AW88261 ACF v1 layout. No I/O or allocation.
 * Count and Section are unchanged on failure. CRC8 is corruption detection,
 * not authentication. Trusted provenance remains the caller's responsibility. */
int T70AwAcfValidate(const void *Buffer, size_t Bytes, uint32_t *Count);
int T70AwAcfFind(const void *Buffer, size_t Bytes, uint16_t DeviceIndex,
                uint16_t Profile, uint32_t SectionType, T70_AW_SECTION *Section);
#endif
