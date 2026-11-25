/* SPDX-License-Identifier: GPL-2.0-only */
/* Normalize WDM DMA scatter/gather output before DPMAIF descriptor planning. */
#include "DpmaifSgCore.h"
#include "DpmaifDma.h"
#include <limits.h>

T70_DPMAIF_RING_STATUS
T70DpmaifNormalizeSgSegments (
  T70_DPMAIF_TX_SEGMENT *Segments,
  uint32_t InputCount,
  uint32_t ExpectedBytes,
  uint32_t Capacity,
  uint32_t *NormalizedCount
  )
{
  uint64_t TotalBytes = 0U;
  uint32_t OutputCount = 0U;
  uint32_t Index;
  uint64_t PreviousAddress = 0U;
  uint32_t PreviousLength = 0U;

  if (Segments == NULL || NormalizedCount == NULL || InputCount == 0U ||
      ExpectedBytes == 0U || ExpectedBytes > UINT16_MAX) {
    return T70DpmaifRingInvalid;
  }

  /* Preflight every element so malformed mappings never leave a partial list. */
  for (Index = 0U; Index < InputCount; ++Index) {
    const T70_DPMAIF_TX_SEGMENT *Segment = &Segments[Index];
    int Adjacent;

    if (Segment->Length == 0U || Segment->Length > UINT16_MAX ||
        !T70DpmaifDmaRangeValid(Segment->DmaAddress, Segment->Length)) {
      return T70DpmaifRingInvalid;
    }
    TotalBytes += Segment->Length;
    if (TotalBytes > ExpectedBytes) {
      return T70DpmaifRingInvalid;
    }

    Adjacent = OutputCount != 0U &&
        PreviousAddress + PreviousLength == Segment->DmaAddress &&
        (uint64_t)PreviousLength + Segment->Length <= UINT16_MAX;
    if (Adjacent) {
      PreviousLength += Segment->Length;
    } else {
      ++OutputCount;
      PreviousAddress = Segment->DmaAddress;
      PreviousLength = Segment->Length;
    }
  }
  if (TotalBytes != ExpectedBytes) {
    return T70DpmaifRingInvalid;
  }
  if (OutputCount > Capacity) {
    return T70DpmaifRingNoSpace;
  }

  /* The preflight proved these writes fit and cannot fail. */
  OutputCount = 0U;
  for (Index = 0U; Index < InputCount; ++Index) {
    const T70_DPMAIF_TX_SEGMENT Segment = Segments[Index];
    if (OutputCount != 0U &&
        Segments[OutputCount - 1U].DmaAddress +
            Segments[OutputCount - 1U].Length == Segment.DmaAddress &&
        (uint64_t)Segments[OutputCount - 1U].Length + Segment.Length <=
            UINT16_MAX) {
      Segments[OutputCount - 1U].Length += Segment.Length;
    } else {
      Segments[OutputCount++] = Segment;
    }
  }
  *NormalizedCount = OutputCount;
  return T70DpmaifRingSuccess;
}
