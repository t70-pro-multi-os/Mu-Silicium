/* SPDX-License-Identifier: GPL-2.0-only */
/* Stock DPMAIF v3 table sizes and 64-byte DMA-pool alignment. */
#include "DpmaifLayout.h"
#include "DpmaifDma.h"
#include "DpmaifDescriptor.h"
#include <limits.h>
#include <string.h>

#define T70_DPMAIF_TABLE_ALIGNMENT 64U
#define T70_DPMAIF_MAX_PIT_ENTRIES 0x3ffffU
#define T70_DPMAIF_MAX_BAT_ENTRIES 0xffffU
#define T70_DPMAIF_MAX_TX_DRB_ENTRIES (UINT16_MAX / 2U)

static int
ValidCounts (
  const T70_DPMAIF_QUEUE_COUNTS *Counts
  )
{
  size_t Index;

  if (Counts == NULL || Counts->FragmentEnabled > 1U ||
      Counts->PitEntries < 2U ||
      Counts->PitEntries > T70_DPMAIF_MAX_PIT_ENTRIES ||
      Counts->RequestBatEntries < 2U ||
      Counts->RequestBatEntries > T70_DPMAIF_MAX_BAT_ENTRIES) {
    return 0;
  }
  if (Counts->FragmentEnabled != 0U) {
    if (Counts->FragmentBatEntries < 2U ||
        Counts->FragmentBatEntries > T70_DPMAIF_MAX_BAT_ENTRIES) {
      return 0;
    }
  } else if (Counts->FragmentBatEntries != 0U) {
    return 0;
  }
  for (Index = 0U; Index < T70_DPMAIF_HW_TX_QUEUES; ++Index) {
    if (Counts->TxDrbEntries[Index] < 2U ||
        Counts->TxDrbEntries[Index] > T70_DPMAIF_MAX_TX_DRB_ENTRIES) {
      return 0;
    }
  }
  return 1;
}

static uint32_t
TableBytes (
  uint32_t Entries,
  uint32_t EntryBytes
  )
{
  uint64_t Bytes = (uint64_t)Entries * EntryBytes;
  return Bytes <= UINT32_MAX ? (uint32_t)Bytes : 0U;
}

static int
RangeValid (
  uint64_t Address,
  uint32_t Bytes
  )
{
  return Address != 0U &&
      (Address & (T70_DPMAIF_TABLE_ALIGNMENT - 1U)) == 0U &&
      T70DpmaifDmaRangeValid(Address, Bytes);
}

T70_DPMAIF_RING_STATUS
T70DpmaifQueueMemorySizes (
  const T70_DPMAIF_QUEUE_COUNTS *Counts,
  T70_DPMAIF_QUEUE_MEMORY_SIZES *Sizes
  )
{
  T70_DPMAIF_QUEUE_MEMORY_SIZES Result;
  uint64_t Total;
  size_t Index;

  if (Sizes == NULL || !ValidCounts(Counts)) {
    return T70DpmaifRingInvalid;
  }
  memset(&Result, 0, sizeof(Result));
  Result.PitBytes = TableBytes(Counts->PitEntries,
      T70_DPMAIF_DATA_PIT_BYTES);
  Result.RequestBatBytes = TableBytes(Counts->RequestBatEntries,
      T70_DPMAIF_BAT_BYTES);
  Result.FragmentBatBytes = Counts->FragmentEnabled != 0U ?
      TableBytes(Counts->FragmentBatEntries, T70_DPMAIF_BAT_BYTES) : 0U;
  Total = (uint64_t)Result.PitBytes + Result.RequestBatBytes +
      Result.FragmentBatBytes;
  for (Index = 0U; Index < T70_DPMAIF_HW_TX_QUEUES; ++Index) {
    Result.TxDrbBytes[Index] = TableBytes(Counts->TxDrbEntries[Index],
        T70_DPMAIF_TX_DATA_BYTES);
    Total += Result.TxDrbBytes[Index];
  }
  if (Result.PitBytes == 0U || Result.RequestBatBytes == 0U ||
      (Counts->FragmentEnabled != 0U && Result.FragmentBatBytes == 0U) ||
      Total > UINT32_MAX) {
    return T70DpmaifRingNoSpace;
  }
  Result.TotalBytes = (uint32_t)Total;
  *Sizes = Result;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifBuildQueueLayout (
  const T70_DPMAIF_QUEUE_COUNTS *Counts,
  const T70_DPMAIF_QUEUE_ADDRESSES *Addresses,
  T70_DPMAIF_QUEUE_LAYOUT *Layout
  )
{
  T70_DPMAIF_QUEUE_LAYOUT Result;
  uint64_t Starts[3U + T70_DPMAIF_HW_TX_QUEUES];
  uint32_t Lengths[3U + T70_DPMAIF_HW_TX_QUEUES];
  size_t Active = 0U;
  size_t Index;
  size_t Other;
  T70_DPMAIF_RING_STATUS Status;

  if (Counts == NULL || Addresses == NULL || Layout == NULL) {
    return T70DpmaifRingInvalid;
  }
  Status = T70DpmaifQueueMemorySizes(Counts, &Result.Sizes);
  if (Status != T70DpmaifRingSuccess) {
    return Status;
  }
  memset(&Result.Queues, 0, sizeof(Result.Queues));
  Result.Queues.PitEntries = Counts->PitEntries;
  Result.Queues.RequestBatEntries = Counts->RequestBatEntries;
  Result.Queues.FragmentBatEntries = Counts->FragmentBatEntries;
  Result.Queues.FragmentEnabled = Counts->FragmentEnabled;
  memcpy(Result.Queues.TxDrbEntries, Counts->TxDrbEntries,
      sizeof(Result.Queues.TxDrbEntries));

  if (!RangeValid(Addresses->PitAddress, Result.Sizes.PitBytes) ||
      !RangeValid(Addresses->RequestBatAddress, Result.Sizes.RequestBatBytes)) {
    return T70DpmaifRingInvalid;
  }
  Result.Queues.PitAddress = Addresses->PitAddress;
  Result.Queues.RequestBatAddress = Addresses->RequestBatAddress;
  Starts[Active] = Addresses->PitAddress;
  Lengths[Active++] = Result.Sizes.PitBytes;
  Starts[Active] = Addresses->RequestBatAddress;
  Lengths[Active++] = Result.Sizes.RequestBatBytes;

  if (Counts->FragmentEnabled != 0U) {
    if (!RangeValid(Addresses->FragmentBatAddress,
            Result.Sizes.FragmentBatBytes)) {
      return T70DpmaifRingInvalid;
    }
    Result.Queues.FragmentBatAddress = Addresses->FragmentBatAddress;
    Starts[Active] = Addresses->FragmentBatAddress;
    Lengths[Active++] = Result.Sizes.FragmentBatBytes;
  } else if (Addresses->FragmentBatAddress != 0U) {
    return T70DpmaifRingInvalid;
  }

  for (Index = 0U; Index < T70_DPMAIF_HW_TX_QUEUES; ++Index) {
    if (!RangeValid(Addresses->TxDrbAddress[Index],
            Result.Sizes.TxDrbBytes[Index])) {
      return T70DpmaifRingInvalid;
    }
    Result.Queues.TxDrbAddress[Index] = Addresses->TxDrbAddress[Index];
    Starts[Active] = Addresses->TxDrbAddress[Index];
    Lengths[Active++] = Result.Sizes.TxDrbBytes[Index];
  }
  for (Index = 0U; Index < Active; ++Index) {
    uint64_t End = Starts[Index] + Lengths[Index];
    for (Other = Index + 1U; Other < Active; ++Other) {
      uint64_t OtherEnd = Starts[Other] + Lengths[Other];
      if (Starts[Index] < OtherEnd && Starts[Other] < End) {
        return T70DpmaifRingInvalid;
      }
    }
  }
  Result.BytesUsed = Result.Sizes.TotalBytes;
  *Layout = Result;
  return T70DpmaifRingSuccess;
}
