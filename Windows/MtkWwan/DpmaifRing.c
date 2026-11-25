/* SPDX-License-Identifier: GPL-2.0-only */
/* Ring arithmetic follows ccci_hif_dpmaif_v3.c and dpmaif_bat_v3.c. */
#include "DpmaifRing.h"
#include <stddef.h>

static int
ValidIndex (
  uint32_t Capacity,
  uint32_t Index
  )
{
  return Capacity > 1U && Index < Capacity;
}

T70_DPMAIF_RING_STATUS
T70DpmaifRingReadable (
  uint32_t Capacity,
  uint32_t ReadIndex,
  uint32_t WriteIndex,
  uint32_t *Count
  )
{
  uint64_t Result;

  if (Count == NULL || !ValidIndex(Capacity, ReadIndex) ||
      !ValidIndex(Capacity, WriteIndex)) {
    return T70DpmaifRingInvalid;
  }
  Result = WriteIndex >= ReadIndex ? (uint64_t)WriteIndex - ReadIndex :
      (uint64_t)Capacity + WriteIndex - ReadIndex;
  *Count = (uint32_t)Result;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifRingWritable (
  uint32_t Capacity,
  uint32_t ReleaseIndex,
  uint32_t WriteIndex,
  uint32_t *Count
  )
{
  uint64_t Result;

  if (Count == NULL || !ValidIndex(Capacity, ReleaseIndex) ||
      !ValidIndex(Capacity, WriteIndex)) {
    return T70DpmaifRingInvalid;
  }
  Result = WriteIndex < ReleaseIndex ?
      (uint64_t)ReleaseIndex - WriteIndex - 1U :
      (uint64_t)Capacity + ReleaseIndex - WriteIndex - 1U;
  *Count = (uint32_t)Result;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifRingAdvance (
  uint32_t Capacity,
  uint32_t Index,
  uint32_t Count,
  uint32_t *NextIndex
  )
{
  uint64_t Result;

  if (NextIndex == NULL || !ValidIndex(Capacity, Index) || Count > Capacity) {
    return T70DpmaifRingInvalid;
  }
  Result = (uint64_t)Index + Count;
  if (Result >= Capacity) {
    Result -= Capacity;
  }
  *NextIndex = (uint32_t)Result;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifRxRingInitialize (
  T70_DPMAIF_RX_RING *Ring,
  uint32_t Capacity
  )
{
  T70_DPMAIF_RX_RING Result = {0};

  if (Ring == NULL || Capacity <= 1U) {
    return T70DpmaifRingInvalid;
  }
  Result.Capacity = Capacity;
  *Ring = Result;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifRxRingObserveHardwareWrite (
  T70_DPMAIF_RX_RING *Ring,
  uint32_t HardwareWriteIndex,
  uint32_t *Available
  )
{
  uint32_t Count;
  uint32_t FreeSlots;
  uint32_t Produced;

  if (Ring == NULL || Available == NULL ||
      !ValidIndex(Ring->Capacity, HardwareWriteIndex) ||
      !ValidIndex(Ring->Capacity, Ring->ReadIndex) ||
      !ValidIndex(Ring->Capacity, Ring->HardwareWriteIndex)) {
    return T70DpmaifRingInvalid;
  }
  if (T70DpmaifRingReadable(
        Ring->Capacity, Ring->ReadIndex, HardwareWriteIndex, &Count) !=
      T70DpmaifRingSuccess ||
      T70DpmaifRingWritable(
        Ring->Capacity, Ring->ReleaseIndex, Ring->HardwareWriteIndex,
        &FreeSlots) != T70DpmaifRingSuccess ||
      T70DpmaifRingReadable(
        Ring->Capacity, Ring->HardwareWriteIndex, HardwareWriteIndex,
        &Produced) != T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  if (Produced > FreeSlots) {
    return T70DpmaifRingStale;
  }
  Ring->HardwareWriteIndex = HardwareWriteIndex;
  *Available = Count;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifRxRingAvailable (
  const T70_DPMAIF_RX_RING *Ring,
  uint32_t *Available
  )
{
  if (Ring == NULL || Available == NULL) {
    return T70DpmaifRingInvalid;
  }
  return T70DpmaifRingReadable(
    Ring->Capacity, Ring->ReadIndex, Ring->HardwareWriteIndex, Available);
}

T70_DPMAIF_RING_STATUS
T70DpmaifRxRingConsume (
  T70_DPMAIF_RX_RING *Ring,
  uint32_t Count,
  uint32_t *NewReadIndex
  )
{
  uint32_t Available;
  uint32_t Next;

  if (Ring == NULL || NewReadIndex == NULL) {
    return T70DpmaifRingInvalid;
  }
  if (T70DpmaifRxRingAvailable(Ring, &Available) != T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  if (Count > Available) {
    return T70DpmaifRingStale;
  }
  if (T70DpmaifRingAdvance(Ring->Capacity, Ring->ReadIndex, Count, &Next) !=
      T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  Ring->ReadIndex = Next;
  *NewReadIndex = Next;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifRxRingRelease (
  T70_DPMAIF_RX_RING *Ring,
  uint32_t Count,
  uint32_t *NewReleaseIndex
  )
{
  uint32_t Releasable;
  uint32_t Next;

  if (Ring == NULL || NewReleaseIndex == NULL) {
    return T70DpmaifRingInvalid;
  }
  if (T70DpmaifRingReadable(
        Ring->Capacity, Ring->ReleaseIndex, Ring->ReadIndex, &Releasable) !=
      T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  if (Count > Releasable) {
    return T70DpmaifRingStale;
  }
  if (T70DpmaifRingAdvance(
        Ring->Capacity, Ring->ReleaseIndex, Count, &Next) !=
      T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  Ring->ReleaseIndex = Next;
  *NewReleaseIndex = Next;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifTxRingInitialize (
  T70_DPMAIF_TX_RING *Ring,
  uint32_t Capacity
  )
{
  T70_DPMAIF_TX_RING Result = {0};

  if (Ring == NULL || Capacity <= 1U) {
    return T70DpmaifRingInvalid;
  }
  Result.Capacity = Capacity;
  *Ring = Result;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifTxRingFreeSlots (
  const T70_DPMAIF_TX_RING *Ring,
  uint32_t *FreeSlots
  )
{
  if (Ring == NULL || FreeSlots == NULL) {
    return T70DpmaifRingInvalid;
  }
  return T70DpmaifRingWritable(
    Ring->Capacity, Ring->ReleaseIndex, Ring->WriteIndex, FreeSlots);
}

T70_DPMAIF_RING_STATUS
T70DpmaifTxRingReserve (
  const T70_DPMAIF_TX_RING *Ring,
  uint32_t Count,
  uint32_t *StartIndex,
  uint32_t *NextIndex
  )
{
  uint32_t FreeSlots;
  uint32_t Next;

  if (Ring == NULL || StartIndex == NULL || NextIndex == NULL) {
    return T70DpmaifRingInvalid;
  }
  if (T70DpmaifTxRingFreeSlots(Ring, &FreeSlots) != T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  if (Count > FreeSlots) {
    return T70DpmaifRingNoSpace;
  }
  if (T70DpmaifRingAdvance(Ring->Capacity, Ring->WriteIndex, Count, &Next) !=
      T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  *StartIndex = Ring->WriteIndex;
  *NextIndex = Next;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifTxRingCommit (
  T70_DPMAIF_TX_RING *Ring,
  uint32_t ExpectedStartIndex,
  uint32_t Count,
  uint32_t *NewWriteIndex
  )
{
  uint32_t Start;
  uint32_t Next;

  if (Ring == NULL || NewWriteIndex == NULL) {
    return T70DpmaifRingInvalid;
  }
  if (ExpectedStartIndex != Ring->WriteIndex) {
    return T70DpmaifRingStale;
  }
  if (T70DpmaifTxRingReserve(Ring, Count, &Start, &Next) !=
      T70DpmaifRingSuccess) {
    uint32_t FreeSlots = 0;
    (void)T70DpmaifTxRingFreeSlots(Ring, &FreeSlots);
    return Count > FreeSlots ? T70DpmaifRingNoSpace : T70DpmaifRingInvalid;
  }
  if (Start != ExpectedStartIndex) {
    return T70DpmaifRingStale;
  }
  Ring->WriteIndex = Next;
  *NewWriteIndex = Next;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifTxRingObserveHardwareRead (
  T70_DPMAIF_TX_RING *Ring,
  uint32_t HardwareReadIndex,
  uint32_t *Completed
  )
{
  uint32_t Count;
  uint32_t Posted;

  if (Ring == NULL || Completed == NULL ||
      !ValidIndex(Ring->Capacity, HardwareReadIndex) ||
      !ValidIndex(Ring->Capacity, Ring->HardwareReadIndex) ||
      !ValidIndex(Ring->Capacity, Ring->WriteIndex)) {
    return T70DpmaifRingInvalid;
  }
  if (T70DpmaifRingReadable(
        Ring->Capacity, Ring->HardwareReadIndex, HardwareReadIndex, &Count) !=
      T70DpmaifRingSuccess ||
      T70DpmaifRingReadable(
        Ring->Capacity, Ring->HardwareReadIndex, Ring->WriteIndex, &Posted) !=
      T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  if (Count > Posted) {
    return T70DpmaifRingStale;
  }
  Ring->HardwareReadIndex = HardwareReadIndex;
  *Completed = Count;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifTxRingRelease (
  T70_DPMAIF_TX_RING *Ring,
  uint32_t Count,
  uint32_t *NewReleaseIndex
  )
{
  uint32_t Releasable;
  uint32_t Next;

  if (Ring == NULL || NewReleaseIndex == NULL) {
    return T70DpmaifRingInvalid;
  }
  if (T70DpmaifRingReadable(
        Ring->Capacity, Ring->ReleaseIndex, Ring->HardwareReadIndex,
        &Releasable) != T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  if (Count > Releasable) {
    return T70DpmaifRingStale;
  }
  if (T70DpmaifRingAdvance(
        Ring->Capacity, Ring->ReleaseIndex, Count, &Next) !=
      T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  Ring->ReleaseIndex = Next;
  *NewReleaseIndex = Next;
  return T70DpmaifRingSuccess;
}
