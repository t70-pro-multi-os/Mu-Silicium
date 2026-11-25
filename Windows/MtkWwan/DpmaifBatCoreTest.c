/* SPDX-License-Identifier: GPL-2.0-only */
#include "DpmaifBatCore.h"
#include "DpmaifDma.h"
#include <stdio.h>
#include <string.h>

static unsigned Checks, Failure;
#define CHECK(X) do { ++Checks; if (!(X) && !Failure) Failure = __LINE__; } while (0)

static T70_DPMAIF_DATA_PIT
PitFor (
  uint16_t Id,
  uint64_t Address,
  uint16_t Length
  )
{
  T70_DPMAIF_DATA_PIT Pit;
  memset(&Pit, 0, sizeof(Pit));
  Pit.PacketType = 0;
  Pit.BufferType = T70_DPMAIF_BUFFER_TYPE_REQUEST;
  Pit.CombinedBufferId = Id;
  Pit.DataAddress = Address;
  Pit.DataLength = Length;
  return Pit;
}

static void
TestDmaMask (
  void
  )
{
  T70_DPMAIF_BAT_CORE Core;
  T70_DPMAIF_BAT_SLOT Slots[4];
  T70_DPMAIF_BAT_PLAN Plan;
  T70_DPMAIF_BAT_PLAN Before;

  CHECK(T70DpmaifBatInitialize(&Core, Slots, 4U) == T70DpmaifRingSuccess);
  memset(&Plan, 0xa5, sizeof(Plan));
  Before = Plan;
  CHECK(T70DpmaifBatPreparePost(&Core, 1U,
        T70_DPMAIF_DMA_MASK_LIMIT - 4096U, 4096U, &Plan) ==
        T70DpmaifRingSuccess);
  CHECK(Plan.DmaAddress == T70_DPMAIF_DMA_MASK_LIMIT - 4096U &&
        Plan.BufferBytes == 4096U);

  Plan = Before;
  CHECK(T70DpmaifBatPreparePost(&Core, 1U,
        T70_DPMAIF_DMA_MASK_LIMIT - 4095U, 4096U, &Plan) ==
        T70DpmaifRingInvalid);
  CHECK(memcmp(&Plan, &Before, sizeof(Plan)) == 0);
  CHECK(T70DpmaifBatPreparePost(&Core, 1U,
        T70_DPMAIF_DMA_MASK_LIMIT, 1U, &Plan) == T70DpmaifRingInvalid);
  CHECK(memcmp(&Plan, &Before, sizeof(Plan)) == 0);
}

static void
TestPostAndClaim (
  void
  )
{
  T70_DPMAIF_BAT_CORE Core;
  T70_DPMAIF_BAT_SLOT Slots[8];
  T70_DPMAIF_BAT_PLAN Plan;
  T70_DPMAIF_BAT_PLAN BeforePlan;
  T70_DPMAIF_DATA_PIT Pit;
  uint8_t BatBytes[8];
  T70_DPMAIF_BAT DecodedBat;
  uint64_t Token = UINT64_C(0xdeadbeef);
  uint32_t Offset = 0xaaaaU, Length = 0xbbbbU, Index = 0xccccU;
  uint32_t Consumed = 0;

  memset(&Core, 0xa5, sizeof(Core));
  CHECK(T70DpmaifBatInitialize(&Core, Slots, 1U) == T70DpmaifRingInvalid);
  CHECK(Core.Capacity == UINT32_C(0xa5a5a5a5));
  CHECK(T70DpmaifBatInitialize(&Core, NULL, 8U) == T70DpmaifRingInvalid);
  CHECK(T70DpmaifBatInitialize(&Core, Slots, 8U) == T70DpmaifRingSuccess);
  CHECK(Core.Capacity == 8U && Core.WriteIndex == 0U && Core.HardwareReadIndex == 0U);

  memset(&Plan, 0xa5, sizeof(Plan));
  BeforePlan = Plan;
  CHECK(T70DpmaifBatPreparePost(&Core, 0, 0x1000U, 4096U, &Plan) ==
        T70DpmaifRingInvalid);
  CHECK(memcmp(&Plan, &BeforePlan, sizeof(Plan)) == 0);
  CHECK(T70DpmaifBatPreparePost(&Core, 1U, T70_DPMAIF_DMA_ADDRESS_LIMIT-4U,
        8U, &Plan) == T70DpmaifRingInvalid);
  CHECK(memcmp(&Plan, &BeforePlan, sizeof(Plan)) == 0);
  CHECK(T70DpmaifBatPreparePost(&Core, 1U, 0x1000U, 4096U, &Plan) ==
        T70DpmaifRingSuccess);
  CHECK(Plan.Index == 0U && Plan.NextIndex == 1U &&
        Plan.Descriptor.BufferAddress == 0x1000U &&
        Plan.Descriptor.Reserved == 0U);
  CHECK(T70DpmaifEncodeBat(&Plan.Descriptor, BatBytes, sizeof(BatBytes)) ==
        T70DpmaifSuccess);
  CHECK(T70DpmaifDecodeBat(BatBytes, sizeof(BatBytes), &DecodedBat) ==
        T70DpmaifSuccess);
  CHECK(DecodedBat.BufferAddress == Plan.DmaAddress &&
        DecodedBat.Reserved == 0U);
  CHECK(Core.WriteIndex == 0U && Slots[0].State == T70DpmaifBatFree);
  BeforePlan = Plan;
  Plan.Descriptor.Reserved = 1U;
  CHECK(T70DpmaifBatCommitPost(&Core, &Plan, &Index) == T70DpmaifRingInvalid);
  CHECK(Core.WriteIndex == 0U && Slots[0].State == T70DpmaifBatFree);
  Plan = BeforePlan;
  CHECK(T70DpmaifBatCommitPost(&Core, &Plan, &Index) == T70DpmaifRingSuccess);
  CHECK(Index == 1U && Core.WriteIndex == 1U &&
        Slots[0].State == T70DpmaifBatPosted && Slots[0].Token == 1U);

  Pit = PitFor(0U, 0x0fffU, 64U);
  CHECK(T70DpmaifBatClaim(&Core, &Pit, &Token, &Offset, &Length) ==
        T70DpmaifRingInvalid);
  CHECK(Token == UINT64_C(0xdeadbeef) && Offset == 0xaaaaU && Length == 0xbbbbU);
  Pit = PitFor(0U, 0x1ff0U, 32U);
  CHECK(T70DpmaifBatClaim(&Core, &Pit, &Token, &Offset, &Length) ==
        T70DpmaifRingInvalid);
  Pit = PitFor(0U, 0x1100U, 256U);
  Pit.BufferType = T70_DPMAIF_BUFFER_TYPE_FRAGMENT;
  CHECK(T70DpmaifBatClaim(&Core, &Pit, &Token, &Offset, &Length) ==
        T70DpmaifRingInvalid);
  Pit.BufferType = T70_DPMAIF_BUFFER_TYPE_REQUEST;
  CHECK(T70DpmaifBatClaim(&Core, &Pit, &Token, &Offset, &Length) ==
        T70DpmaifRingSuccess);
  CHECK(Token == 1U && Offset == 0x100U && Length == 256U &&
        Slots[0].State == T70DpmaifBatInFlight);
  CHECK(T70DpmaifBatClaim(&Core, &Pit, &Token, &Offset, &Length) ==
        T70DpmaifRingStale);
  CHECK(T70DpmaifBatReturn(&Core, 0U, 2U) == T70DpmaifRingStale);
  CHECK(T70DpmaifBatReturn(&Core, 0U, 1U) == T70DpmaifRingSuccess);
  CHECK(Slots[0].State == T70DpmaifBatFree && Slots[0].Token == 0U);
  CHECK(T70DpmaifBatReturn(&Core, 0U, 1U) == T70DpmaifRingStale);

  CHECK(T70DpmaifBatObserveHardwareRead(&Core, 1U, &Consumed) ==
        T70DpmaifRingSuccess);
  CHECK(Consumed == 1U && Core.HardwareReadIndex == 1U);
  CHECK(T70DpmaifBatObserveHardwareRead(&Core, 3U, &Consumed) ==
        T70DpmaifRingStale);
  CHECK(Core.HardwareReadIndex == 1U);
}

static void
TestInitialTable (
  void
  )
{
  T70_DPMAIF_BAT_CORE Core, BeforeCore;
  T70_DPMAIF_BAT_SLOT Slots[4], BeforeSlots[4];
  uint64_t Addresses[3] = {
    UINT64_C(0x10000), UINT64_C(0x20000), UINT64_C(0x30000)
  };
  uint8_t Table[4U * T70_DPMAIF_BAT_BYTES];
  uint8_t BeforeTable[sizeof(Table)];
  T70_DPMAIF_BAT Descriptor;
  uint32_t Posted = UINT32_C(0xdeadbeef);

  memset(&Core, 0xa5, sizeof(Core));
  memset(Slots, 0x5a, sizeof(Slots));
  memset(Table, 0x3c, sizeof(Table));
  BeforeCore = Core;
  memcpy(BeforeSlots, Slots, sizeof(Slots));
  memcpy(BeforeTable, Table, sizeof(Table));

  CHECK(T70DpmaifBatPostInitialBuffers(&Core, Slots, 4U, Addresses, 3U,
        128U, 128U, Table, sizeof(Table) - 1U, &Posted) ==
        T70DpmaifRingNoSpace);
  CHECK(memcmp(&Core, &BeforeCore, sizeof(Core)) == 0 &&
        memcmp(Slots, BeforeSlots, sizeof(Slots)) == 0 &&
        memcmp(Table, BeforeTable, sizeof(Table)) == 0 &&
        Posted == UINT32_C(0xdeadbeef));

  Addresses[1] += 1U;
  CHECK(T70DpmaifBatPostInitialBuffers(&Core, Slots, 4U, Addresses, 3U,
        128U, 128U, Table, sizeof(Table), &Posted) ==
        T70DpmaifRingInvalid);
  CHECK(memcmp(&Core, &BeforeCore, sizeof(Core)) == 0 &&
        memcmp(Slots, BeforeSlots, sizeof(Slots)) == 0 &&
        memcmp(Table, BeforeTable, sizeof(Table)) == 0 &&
        Posted == UINT32_C(0xdeadbeef));

  Addresses[1] -= 1U;
  CHECK(T70DpmaifBatPostInitialBuffers(&Core, Slots, 4U, Addresses, 3U,
        128U, 128U, Table, sizeof(Table), &Posted) ==
        T70DpmaifRingSuccess);
  CHECK(Posted == 3U && Core.Capacity == 4U &&
        Core.HardwareReadIndex == 0U && Core.WriteIndex == 3U);
  CHECK(Slots[0].State == T70DpmaifBatPosted &&
        Slots[0].Token == 1U && Slots[0].DmaAddress == Addresses[0] &&
        Slots[1].Token == 2U && Slots[1].DmaAddress == Addresses[1] &&
        Slots[2].Token == 3U && Slots[2].DmaAddress == Addresses[2] &&
        Slots[3].State == T70DpmaifBatFree);
  CHECK(T70DpmaifDecodeBat(Table, T70_DPMAIF_BAT_BYTES, &Descriptor) ==
        T70DpmaifSuccess && Descriptor.BufferAddress == Addresses[0]);
  CHECK(T70DpmaifDecodeBat(Table + 2U * T70_DPMAIF_BAT_BYTES,
        T70_DPMAIF_BAT_BYTES, &Descriptor) == T70DpmaifSuccess &&
        Descriptor.BufferAddress == Addresses[2]);
  CHECK(memcmp(Table + 3U * T70_DPMAIF_BAT_BYTES,
        (uint8_t[T70_DPMAIF_BAT_BYTES]){0}, T70_DPMAIF_BAT_BYTES) == 0);
}

static void
TestPostPipelineAndBounds (
  void
  )
{
  T70_DPMAIF_BAT_CORE Core;
  T70_DPMAIF_BAT_SLOT Slots[4];
  T70_DPMAIF_BAT_PLAN Plan;
  T70_DPMAIF_DATA_PIT Pit;
  uint64_t Token = 0;
  uint32_t Offset = 0, Length = 0;
  uint32_t WriteIndex = 0;
  uint32_t Consumed = 0;

  CHECK(T70DpmaifBatInitialize(&Core, Slots, 4U) == T70DpmaifRingSuccess);
  for (uint64_t Id = 1; Id <= 3; ++Id) {
    CHECK(T70DpmaifBatPreparePost(&Core, Id, 0x100000U + (Id * 0x1000U),
        2048U, &Plan) == T70DpmaifRingSuccess);
    CHECK(T70DpmaifBatCommitPost(&Core, &Plan, &WriteIndex) ==
          T70DpmaifRingSuccess);
  }
  CHECK(WriteIndex == 3U);
  CHECK(T70DpmaifBatPreparePost(&Core, 4U, 0x104000U, 2048U, &Plan) ==
        T70DpmaifRingNoSpace);
  CHECK(T70DpmaifBatObserveHardwareRead(&Core, 2U, &Consumed) ==
        T70DpmaifRingSuccess);
  CHECK(Consumed == 2U);

  Pit = PitFor(0U, 0x101080U, 1024U);
  CHECK(T70DpmaifBatClaim(&Core, &Pit, &Token, &Offset, &Length) ==
        T70DpmaifRingSuccess);
  CHECK(Token == 1U && Offset == 128U && Length == 1024U);
  Pit = PitFor(1U, 0x102100U, 512U);
  CHECK(T70DpmaifBatClaim(&Core, &Pit, &Token, &Offset, &Length) ==
        T70DpmaifRingSuccess);
  CHECK(Token == 2U && Offset == 256U && Length == 512U);
  CHECK(T70DpmaifBatReturn(&Core, 0U, 1U) == T70DpmaifRingSuccess);
  CHECK(T70DpmaifBatReturn(&Core, 1U, 2U) == T70DpmaifRingSuccess);

  CHECK(T70DpmaifBatPreparePost(&Core, 4U, 0x104000U, 2048U, &Plan) ==
        T70DpmaifRingSuccess);
  CHECK(Plan.Index == 3U && Plan.NextIndex == 0U);
  CHECK(T70DpmaifBatCommitPost(&Core, &Plan, &WriteIndex) ==
        T70DpmaifRingSuccess);
  CHECK(WriteIndex == 0U && Slots[3].State == T70DpmaifBatPosted);
}

int
main (
  void
  )
{
  TestDmaMask();
  TestPostAndClaim();
  TestInitialTable();
  TestPostPipelineAndBounds();
  if (Failure != 0) {
    fprintf(stderr, "DpmaifBatCoreTest failed at line %u after %u checks\n",
            Failure, Checks);
    return (int)Failure;
  }
  printf("T70 DPMAIF BAT ownership core: %u source/lifetime checks passed\n", Checks);
  return 0;
}
