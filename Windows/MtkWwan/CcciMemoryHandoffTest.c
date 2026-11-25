/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/MtkCcciMemoryHandoff.h>
#include <stdio.h>
#include <string.h>

static unsigned Checks;
static unsigned Failure;
#define CHECK(Expression) do { \
  ++Checks; \
  if (!(Expression) && Failure == 0U) Failure = __LINE__; \
} while (0)

int main(void)
{
  T70_CCCI_MEMORY_HANDOFF Handoff;
  memset(&Handoff, 0, sizeof(Handoff));
  Handoff.Signature = T70_CCCI_MEMORY_HANDOFF_SIGNATURE;
  Handoff.Version = T70_CCCI_MEMORY_HANDOFF_VERSION;
  Handoff.Bytes = sizeof(Handoff);
  Handoff.ModemBase = UINT64_C(0x82000000);
  Handoff.ModemBytes = UINT64_C(0x04000000);
  Handoff.Flags = T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK;
  CHECK(sizeof(Handoff) == T70_CCCI_MEMORY_HANDOFF_BYTES);
  CHECK(T70_CCCI_SMC_BOOT_STATUS_FID == UINT32_C(0xC2000505));
  CHECK(T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.Flags = 0U;
  CHECK(T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.Version = T70_CCCI_MEMORY_HANDOFF_LEGACY_VERSION;
  Handoff.Flags = T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK;
  CHECK(T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.Version = T70_CCCI_MEMORY_HANDOFF_VERSION;
  Handoff.Flags = T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK;
  Handoff.MdGeneration = 6295U;
  Handoff.MdSubVersion = 1U;
  Handoff.BootStatus0 = 0x5443000CU;
  Handoff.BootStatus1 = 0x12345678U;
  Handoff.Flags |= T70_CCCI_MEMORY_FLAG_MD1_BOOT_STATUS_MATCHED;
  CHECK(T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.BootStatus0 = 0U;
  CHECK(!T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.BootStatus0 = 0x5443000CU;
  Handoff.Flags = T70_CCCI_MEMORY_FLAG_MD1_BOOT_STATUS_MATCHED;
  CHECK(!T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.Flags = T70_CCCI_MEMORY_FLAG_MD1_IMAGE_LOAD_OK;
  Handoff.MdGeneration = 0U;
  Handoff.MdSubVersion = 0U;
  Handoff.BootStatus0 = 0U;
  Handoff.BootStatus1 = 0U;

  Handoff.Signature = 0U;
  CHECK(!T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.Signature = T70_CCCI_MEMORY_HANDOFF_SIGNATURE;
  Handoff.Version++;
  CHECK(!T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.Version--;
  Handoff.Bytes--;
  CHECK(!T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.Bytes++;
  Handoff.Flags |= 2U;
  CHECK(!T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.Flags = 0U;
  Handoff.ModemBase = 0U;
  CHECK(!T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.ModemBase = T70_CCCI_MEMORY_DMA_LIMIT;
  CHECK(!T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.ModemBase = T70_CCCI_MEMORY_DMA_LIMIT - 0x100U;
  Handoff.ModemBytes = 0x101U;
  CHECK(!T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.ModemBytes = 0x100U;
  CHECK(T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.ModemBytes = 0U;
  CHECK(!T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.ModemBytes = 0x100U;
  Handoff.Reserved0 = 1U;
  CHECK(!T70CcciMemoryHandoffIsValid(&Handoff));
  Handoff.Reserved0 = 0U;
  Handoff.Reserved[0] = 1U;
  CHECK(!T70CcciMemoryHandoffIsValid(&Handoff));

  if (Failure != 0U) {
    fprintf(stderr, "CcciMemoryHandoffTest failed at line %u after %u checks\n",
        Failure, Checks);
    return (int)Failure;
  }
  printf("T70 CCCI memory handoff bounds and malformed-record checks passed: %u\n",
      Checks);
  return 0;
}
