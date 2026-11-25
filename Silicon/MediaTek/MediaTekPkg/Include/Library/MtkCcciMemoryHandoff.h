/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MTK_CCCI_MEMORY_HANDOFF_H
#define T70_MTK_CCCI_MEMORY_HANDOFF_H

#include <stdint.h>

#define T70_CCCI_MEMORY_HANDOFF_ADDRESS UINT64_C(0x43f7f180)
#define T70_CCCI_MEMORY_HANDOFF_BYTES 0x40U
#define T70_CCCI_MEMORY_HANDOFF_LEGACY_VERSION 1U
#define T70_CCCI_MEMORY_HANDOFF_VERSION 2U
#define T70_CCCI_MEMORY_HANDOFF_SIGNATURE UINT64_C(0x3149434343303754)
#define T70_CCCI_MEMORY_MAP_STATE_ADDRESS UINT64_C(0x43f7f150)
#define T70_CCCI_MEMORY_MAP_STATE_VALID UINT64_C(0x43434d4150000001)
#define T70_CCCI_MEMORY_MAP_STATE_FAILED UINT64_C(0x43434d4150000002)
/* The LK header/error tags say nothing about the modem's runtime state. */
#define T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK       0x00000001U
#define T70_CCCI_MEMORY_FLAG_MD1_BOOT_STATUS_MATCHED 0x00000002U
#define T70_CCCI_MEMORY_FLAGS_MASK \
  (T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK | \
   T70_CCCI_MEMORY_FLAG_MD1_BOOT_STATUS_MATCHED)
#define T70_CCCI_MEMORY_DMA_LIMIT UINT64_C(0x1000000000)

/* Derived from the T70 kernel's MTK_SIP_SMC_CMD(0x505),
 * MD_POWER_CONFIG (6), and MD_BOOT_STATUS (4). */
#define T70_CCCI_SMC_BOOT_STATUS_FID UINT32_C(0xC2000505)
#define T70_CCCI_SMC_POWER_CONFIG 6U
#define T70_CCCI_SMC_MD_BOOT_STATUS 4U
#define T70_CCCI_SMC_SUCCESS 0U

typedef struct {
  uint64_t Signature;
  uint32_t Version;
  uint32_t Bytes;
  uint64_t ModemBase;
  uint64_t ModemBytes;
  uint32_t Flags;
  uint32_t MdGeneration;
  uint32_t MdSubVersion;
  uint32_t BootStatus0;
  uint32_t BootStatus1;
  uint32_t Reserved0;
  uint64_t Reserved[1];
} T70_CCCI_MEMORY_HANDOFF;

/* Same generation table used by md_sys1_platform.c in the kernel reference. */
static inline uint32_t
T70CcciExpectedMdBootStatus(uint32_t MdGeneration, uint32_t MdSubVersion)
{
  if ((MdGeneration <= 6295U) && (MdSubVersion == 0U)) return 0x54430007U;
  if (((MdGeneration == 6295U) && (MdSubVersion != 0U)) ||
      (MdGeneration == 6297U) ||
      ((MdGeneration == 6298U) && (MdSubVersion == 0U))) return 0x5443000CU;
  if ((MdGeneration == 6298U) && (MdSubVersion != 0U)) return 0x5443000FU;
  if (MdGeneration >= 6299U) return 0x54430012U;
  return 0U;
}

static inline int
T70CcciBootStatusMatches(uint32_t ExpectedStatus, uint32_t SmcStatus,
                         uint32_t BootStatus)
{
  return (ExpectedStatus != 0U) && (SmcStatus == T70_CCCI_SMC_SUCCESS) &&
      (BootStatus == ExpectedStatus);
}

static inline int
T70CcciMemoryHandoffIsValid(const T70_CCCI_MEMORY_HANDOFF *Handoff)
{
  uint32_t ExpectedBootStatus;
  if (Handoff == 0 ||
      Handoff->Signature != T70_CCCI_MEMORY_HANDOFF_SIGNATURE ||
      (Handoff->Version != T70_CCCI_MEMORY_HANDOFF_LEGACY_VERSION &&
       Handoff->Version != T70_CCCI_MEMORY_HANDOFF_VERSION) ||
      Handoff->Bytes != sizeof(*Handoff) ||
      Handoff->ModemBase == 0U ||
      Handoff->ModemBase >= T70_CCCI_MEMORY_DMA_LIMIT ||
      Handoff->ModemBytes == 0U ||
      Handoff->ModemBytes > T70_CCCI_MEMORY_DMA_LIMIT - Handoff->ModemBase ||
      Handoff->Reserved0 != 0U || Handoff->Reserved[0] != 0U) {
    return 0;
  }

  if (Handoff->Version == T70_CCCI_MEMORY_HANDOFF_LEGACY_VERSION) {
    /* In v1 these fields were reserved, including BootStatus0/1. */
    return ((Handoff->Flags & ~T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK) == 0U) &&
        (Handoff->MdGeneration == 0U) && (Handoff->MdSubVersion == 0U) &&
        (Handoff->BootStatus0 == 0U) && (Handoff->BootStatus1 == 0U);
  }

  if ((Handoff->Flags & ~T70_CCCI_MEMORY_FLAGS_MASK) != 0U) return 0;
  if ((Handoff->Flags & T70_CCCI_MEMORY_FLAG_MD1_BOOT_STATUS_MATCHED) == 0U) {
    return (Handoff->BootStatus0 == 0U) && (Handoff->BootStatus1 == 0U);
  }
  if ((Handoff->Flags & T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK) == 0U) return 0;
  ExpectedBootStatus = T70CcciExpectedMdBootStatus(Handoff->MdGeneration,
                                                   Handoff->MdSubVersion);
  return T70CcciBootStatusMatches(ExpectedBootStatus,
                                  T70_CCCI_SMC_SUCCESS,
                                  Handoff->BootStatus0);
}

#endif
