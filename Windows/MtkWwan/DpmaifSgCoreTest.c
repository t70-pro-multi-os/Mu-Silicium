/* SPDX-License-Identifier: GPL-2.0-only */
#include "DpmaifDma.h"
#include "DpmaifSgCore.h"
#include <stdio.h>
#include <string.h>

static unsigned Checks, Failure;
#define CHECK(X) do { ++Checks; if (!(X) && !Failure) Failure = __LINE__; } while (0)

static void TestCoalesceAndCapacity(void)
{
  T70_DPMAIF_TX_SEGMENT Segments[4] = {
    {0x1000U, 128U}, {0x1080U, 128U}, {0x2000U, 256U}, {0x3000U, 0U}
  };
  T70_DPMAIF_TX_SEGMENT Before[4];
  uint32_t Count = 0xaaaaaaaaU;

  Segments[3].Length = 0U;
  memcpy(Before, Segments, sizeof(Segments));
  CHECK(T70DpmaifNormalizeSgSegments(Segments, 3U, 512U, 3U, &Count) ==
        T70DpmaifRingSuccess);
  CHECK(Count == 2U && Segments[0].DmaAddress == 0x1000U &&
        Segments[0].Length == 256U && Segments[1].DmaAddress == 0x2000U &&
        Segments[1].Length == 256U);

  memcpy(Segments, Before, sizeof(Segments));
  Count = 0xaaaaaaaaU;
  CHECK(T70DpmaifNormalizeSgSegments(Segments, 3U, 512U, 1U, &Count) ==
        T70DpmaifRingNoSpace);
  CHECK(memcmp(Segments, Before, sizeof(Segments)) == 0 &&
        Count == 0xaaaaaaaaU);
}

static void TestRangesAndAtomicFailure(void)
{
  T70_DPMAIF_TX_SEGMENT Segments[2] = {
    {T70_DPMAIF_DMA_MASK_LIMIT - 8U, 8U}, {0U, 0U}
  };
  T70_DPMAIF_TX_SEGMENT Before[2];
  uint32_t Count = 0x55555555U;

  CHECK(T70DpmaifNormalizeSgSegments(Segments, 1U, 8U, 1U, &Count) ==
        T70DpmaifRingSuccess);
  CHECK(Count == 1U && Segments[0].DmaAddress ==
        T70_DPMAIF_DMA_MASK_LIMIT - 8U);

  Segments[0].DmaAddress = T70_DPMAIF_DMA_MASK_LIMIT - 7U;
  memcpy(Before, Segments, sizeof(Segments));
  Count = 0x55555555U;
  CHECK(T70DpmaifNormalizeSgSegments(Segments, 1U, 8U, 1U, &Count) ==
        T70DpmaifRingInvalid);
  CHECK(memcmp(Segments, Before, sizeof(Segments)) == 0 &&
        Count == 0x55555555U);

  Segments[0].DmaAddress = T70_DPMAIF_DMA_MASK_LIMIT;
  Segments[0].Length = 1U;
  memcpy(Before, Segments, sizeof(Segments));
  CHECK(T70DpmaifNormalizeSgSegments(Segments, 1U, 1U, 1U, &Count) ==
        T70DpmaifRingInvalid);
  CHECK(memcmp(Segments, Before, sizeof(Segments)) == 0 &&
        Count == 0x55555555U);
}

static void TestMalformedLists(void)
{
  T70_DPMAIF_TX_SEGMENT Segments[2] = {{0x1000U, 64U}, {0x2000U, 32U}};
  T70_DPMAIF_TX_SEGMENT Before[2];
  uint32_t Count = 0x12345678U;

  memcpy(Before, Segments, sizeof(Segments));
  CHECK(T70DpmaifNormalizeSgSegments(Segments, 2U, 95U, 2U, &Count) ==
        T70DpmaifRingInvalid);
  CHECK(memcmp(Segments, Before, sizeof(Segments)) == 0 &&
        Count == 0x12345678U);
  CHECK(T70DpmaifNormalizeSgSegments(Segments, 0U, 96U, 2U, &Count) ==
        T70DpmaifRingInvalid);
  Segments[1].Length = 0U;
  memcpy(Before, Segments, sizeof(Segments));
  CHECK(T70DpmaifNormalizeSgSegments(Segments, 2U, 64U, 2U, &Count) ==
        T70DpmaifRingInvalid);
  CHECK(memcmp(Segments, Before, sizeof(Segments)) == 0 &&
        Count == 0x12345678U);
}

int main(void)
{
  TestCoalesceAndCapacity();
  TestRangesAndAtomicFailure();
  TestMalformedLists();
  if (Failure) {
    fprintf(stderr, "DpmaifSgCoreTest failed at line %u after %u checks\n",
            Failure, Checks);
    return (int)Failure;
  }
  printf("T70 DPMAIF WDM scatter/gather normalization: %u checks passed\n",
         Checks);
  return 0;
}
