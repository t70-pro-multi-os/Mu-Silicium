/* SPDX-License-Identifier: GPL-2.0-only */
#include "DpmaifRing.h"
#include <stdio.h>

static unsigned Checks, Failure;
#define CHECK(X) do { ++Checks; if (!(X) && !Failure) Failure = __LINE__; } while (0)

static void
TestArithmetic (
  void
  )
{
  uint32_t Count = 0xaaaaU;
  uint32_t Next = 0xbbbbU;

  CHECK(T70DpmaifRingReadable(8, 6, 2, &Count) == T70DpmaifRingSuccess);
  CHECK(Count == 4U);
  CHECK(T70DpmaifRingWritable(8, 6, 2, &Count) == T70DpmaifRingSuccess);
  CHECK(Count == 3U);
  CHECK(T70DpmaifRingAdvance(8, 7, 3, &Next) == T70DpmaifRingSuccess);
  CHECK(Next == 2U);
  Count = 0xaaaaU;
  CHECK(T70DpmaifRingReadable(1, 0, 0, &Count) == T70DpmaifRingInvalid);
  CHECK(Count == 0xaaaaU);
  CHECK(T70DpmaifRingWritable(8, 8, 0, &Count) == T70DpmaifRingInvalid);
  CHECK(T70DpmaifRingAdvance(8, 8, 1, &Next) == T70DpmaifRingInvalid);
  CHECK(Next == 2U);
  CHECK(T70DpmaifRingAdvance(8, 0, 9, &Next) == T70DpmaifRingInvalid);
  CHECK(Next == 2U);
  CHECK(T70DpmaifRingReadable(8, 0, 0, NULL) == T70DpmaifRingInvalid);
  CHECK(T70DpmaifRingWritable(8, 0, 0, NULL) == T70DpmaifRingInvalid);
  CHECK(T70DpmaifRingAdvance(8, 0, 0, NULL) == T70DpmaifRingInvalid);
}

static void
TestRxRing (
  void
  )
{
  T70_DPMAIF_RX_RING Ring;
  T70_DPMAIF_RX_RING Before;
  uint32_t Available = 0xaaaaU;
  uint32_t Index = 0xbbbbU;

  CHECK(T70DpmaifRxRingInitialize(&Ring, 1) == T70DpmaifRingInvalid);
  CHECK(T70DpmaifRxRingInitialize(NULL, 8) == T70DpmaifRingInvalid);
  CHECK(T70DpmaifRxRingInitialize(&Ring, 8) == T70DpmaifRingSuccess);
  CHECK(Ring.Capacity == 8U && Ring.HardwareWriteIndex == 0U &&
        Ring.ReadIndex == 0U && Ring.ReleaseIndex == 0U);
  CHECK(T70DpmaifRxRingAvailable(&Ring, &Available) == T70DpmaifRingSuccess);
  CHECK(Available == 0U);
  Before = Ring;
  CHECK(T70DpmaifRxRingObserveHardwareWrite(&Ring, 3, &Available) ==
        T70DpmaifRingSuccess);
  CHECK(Available == 3U && Ring.HardwareWriteIndex == 3U);
  CHECK(T70DpmaifRxRingObserveHardwareWrite(&Ring, 1, &Available) ==
        T70DpmaifRingStale);
  CHECK(Ring.HardwareWriteIndex == 3U);
  CHECK(T70DpmaifRxRingConsume(&Ring, 2, &Index) == T70DpmaifRingSuccess);
  CHECK(Index == 2U && Ring.ReadIndex == 2U);
  CHECK(T70DpmaifRxRingRelease(&Ring, 1, &Index) == T70DpmaifRingSuccess);
  CHECK(Index == 1U && Ring.ReleaseIndex == 1U);
  Before = Ring;
  CHECK(T70DpmaifRxRingRelease(&Ring, 2, &Index) == T70DpmaifRingStale);
  CHECK(Ring.ReleaseIndex == Before.ReleaseIndex && Index == 1U);
  CHECK(T70DpmaifRxRingRelease(&Ring, 1, &Index) == T70DpmaifRingSuccess);
  CHECK(Index == 2U && Ring.ReleaseIndex == 2U);
  CHECK(T70DpmaifRxRingObserveHardwareWrite(&Ring, 1, &Available) ==
        T70DpmaifRingSuccess);
  CHECK(Available == 7U);
  CHECK(T70DpmaifRxRingConsume(&Ring, 8, &Index) == T70DpmaifRingStale);
  CHECK(Ring.ReadIndex == 2U);
  CHECK(T70DpmaifRxRingConsume(&Ring, 7, &Index) == T70DpmaifRingSuccess);
  CHECK(Index == 1U && Ring.ReadIndex == 1U);
  CHECK(T70DpmaifRxRingRelease(&Ring, 7, &Index) == T70DpmaifRingSuccess);
  CHECK(Index == 1U && Ring.ReleaseIndex == 1U);
  Before = Ring;
  CHECK(T70DpmaifRxRingObserveHardwareWrite(&Ring, 8, &Available) ==
        T70DpmaifRingInvalid);
  CHECK(Ring.HardwareWriteIndex == Before.HardwareWriteIndex);
}

static void
TestTxRing (
  void
  )
{
  T70_DPMAIF_TX_RING Ring;
  T70_DPMAIF_TX_RING Before;
  uint32_t FreeSlots = 0;
  uint32_t Start = 0xaaaaU;
  uint32_t Next = 0xbbbbU;
  uint32_t Completed = 0;
  uint32_t Release = 0;

  CHECK(T70DpmaifTxRingInitialize(&Ring, 0) == T70DpmaifRingInvalid);
  CHECK(T70DpmaifTxRingInitialize(&Ring, 8) == T70DpmaifRingSuccess);
  CHECK(T70DpmaifTxRingObserveHardwareRead(&Ring, 1, &Completed) ==
        T70DpmaifRingStale);
  CHECK(Ring.HardwareReadIndex == 0U);
  CHECK(T70DpmaifTxRingFreeSlots(&Ring, &FreeSlots) == T70DpmaifRingSuccess);
  CHECK(FreeSlots == 7U);
  CHECK(T70DpmaifTxRingReserve(&Ring, 7, &Start, &Next) == T70DpmaifRingSuccess);
  CHECK(Start == 0U && Next == 7U && Ring.WriteIndex == 0U);
  CHECK(T70DpmaifTxRingCommit(&Ring, Start, 7, &Next) == T70DpmaifRingSuccess);
  CHECK(Next == 7U && Ring.WriteIndex == 7U);
  CHECK(T70DpmaifTxRingFreeSlots(&Ring, &FreeSlots) == T70DpmaifRingSuccess);
  CHECK(FreeSlots == 0U);
  Before = Ring;
  CHECK(T70DpmaifTxRingReserve(&Ring, 1, &Start, &Next) == T70DpmaifRingNoSpace);
  CHECK(Start == 0U && Next == 7U && Ring.WriteIndex == Before.WriteIndex);
  CHECK(T70DpmaifTxRingObserveHardwareRead(&Ring, 3, &Completed) ==
        T70DpmaifRingSuccess);
  CHECK(Completed == 3U && Ring.HardwareReadIndex == 3U);
  CHECK(T70DpmaifTxRingRelease(&Ring, 3, &Release) == T70DpmaifRingSuccess);
  CHECK(Release == 3U && Ring.ReleaseIndex == 3U);
  CHECK(T70DpmaifTxRingFreeSlots(&Ring, &FreeSlots) == T70DpmaifRingSuccess);
  CHECK(FreeSlots == 3U);
  CHECK(T70DpmaifTxRingReserve(&Ring, 3, &Start, &Next) == T70DpmaifRingSuccess);
  CHECK(Start == 7U && Next == 2U);
  CHECK(T70DpmaifTxRingCommit(&Ring, Start + 1U, 3, &Release) ==
        T70DpmaifRingStale);
  CHECK(Ring.WriteIndex == 7U);
  CHECK(T70DpmaifTxRingCommit(&Ring, Start, 3, &Release) ==
        T70DpmaifRingSuccess);
  CHECK(Release == 2U && Ring.WriteIndex == 2U);
  CHECK(T70DpmaifTxRingObserveHardwareRead(&Ring, 7, &Completed) ==
        T70DpmaifRingSuccess);
  CHECK(Completed == 4U);
  CHECK(T70DpmaifTxRingRelease(&Ring, 4, &Release) == T70DpmaifRingSuccess);
  CHECK(Release == 7U);
  Before = Ring;
  CHECK(T70DpmaifTxRingObserveHardwareRead(&Ring, 3, &Completed) ==
        T70DpmaifRingStale);
  CHECK(Ring.HardwareReadIndex == Before.HardwareReadIndex);
  CHECK(T70DpmaifTxRingObserveHardwareRead(&Ring, 2, &Completed) ==
        T70DpmaifRingSuccess);
  CHECK(Completed == 3U);
  CHECK(T70DpmaifTxRingRelease(&Ring, 3, &Release) == T70DpmaifRingSuccess);
  CHECK(Release == 2U);
  Before = Ring;
  CHECK(T70DpmaifTxRingObserveHardwareRead(&Ring, 8, &Completed) ==
        T70DpmaifRingInvalid);
  CHECK(Ring.HardwareReadIndex == Before.HardwareReadIndex);
  CHECK(T70DpmaifTxRingRelease(&Ring, 1, &Release) == T70DpmaifRingStale);
  CHECK(Ring.ReleaseIndex == Before.ReleaseIndex);
}

int
main (
  void
  )
{
  TestArithmetic();
  TestRxRing();
  TestTxRing();
  if (Failure != 0) {
    fprintf(stderr, "DpmaifRingTest failed at line %u after %u checks\n",
            Failure, Checks);
    return (int)Failure;
  }
  printf("T70 DPMAIF ring lifecycle: %u source-semantics checks passed\n", Checks);
  return 0;
}
