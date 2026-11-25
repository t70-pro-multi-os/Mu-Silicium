/* SPDX-License-Identifier: GPL-2.0-only */
#include "DpmaifTxPacket.h"
#include "DpmaifDma.h"
#include <stdio.h>
#include <string.h>

static unsigned Checks, Failure;
#define CHECK(X) do { ++Checks; if (!(X) && !Failure) Failure = __LINE__; } while (0)

static void TestPlanAndCommit(void)
{
  T70_DPMAIF_TX_RING Ring;
  T70_DPMAIF_TX_SEGMENT Segments[3] = {
    { UINT64_C(0x023456789a), 512U },
    { UINT64_C(0x00300000), 256U },
    { UINT64_C(0x00310000), 232U }
  };
  T70_DPMAIF_TX_PACKET_PLAN Plan;
  T70_DPMAIF_TX_PACKET_PLAN BeforePlan;
  uint8_t Descriptors[32];
  T70_DPMAIF_TX_MESSAGE Message;
  T70_DPMAIF_TX_DATA Data;
  uint32_t NewIndex;

  CHECK(T70DpmaifTxRingInitialize(&Ring, 8U) == T70DpmaifRingSuccess);
  memset(Descriptors, 0xa5, sizeof(Descriptors));
  memset(&Plan, 0x5a, sizeof(Plan));
  BeforePlan = Plan;
  CHECK(T70DpmaifBuildTxPacketPlan(&Ring, 0x77U, 0x1000U, 1U, 1U,
      Segments, 3U, Descriptors, sizeof(Descriptors), &Plan) ==
      T70DpmaifRingSuccess);
  CHECK(Plan.StartIndex == 0U && Plan.NextIndex == 4U &&
        Plan.DescriptorCount == 4U && Plan.PacketBytes == 1000U);
  CHECK(Ring.WriteIndex == 0U);
  CHECK(T70DpmaifDecodeTxMessage(Descriptors, 8U, &Message) == T70DpmaifSuccess);
  CHECK(Message.ChannelId == 0x77U && Message.CountLow == 0x1000U &&
        Message.PacketLength == 1000U && Message.Ipv4Checksum == 1U &&
        Message.L4Checksum == 1U);
  CHECK(T70DpmaifDecodeTxData(Descriptors + 8U, 8U, &Data) == T70DpmaifSuccess);
  CHECK(Data.DataAddress == Segments[0].DmaAddress &&
        Data.DataLength == Segments[0].Length && Data.Continuation == 1U);
  CHECK(T70DpmaifDecodeTxData(Descriptors + 16U, 8U, &Data) == T70DpmaifSuccess);
  CHECK(Data.DataAddress == Segments[1].DmaAddress &&
        Data.DataLength == Segments[1].Length && Data.Continuation == 1U);
  CHECK(T70DpmaifDecodeTxData(Descriptors + 24U, 8U, &Data) ==
        T70DpmaifSuccess);
  CHECK(Data.DataAddress == Segments[2].DmaAddress &&
        Data.DataLength == Segments[2].Length && Data.Continuation == 0U);
  CHECK(T70DpmaifTxRingCommit(&Ring, Plan.StartIndex, Plan.DescriptorCount,
      &NewIndex) == T70DpmaifRingSuccess && NewIndex == Plan.NextIndex);

  Segments[0].DmaAddress = T70_DPMAIF_DMA_MASK_LIMIT;
  memset(Descriptors, 0x3c, sizeof(Descriptors));
  memset(&Plan, 0x69, sizeof(Plan));
  BeforePlan = Plan;
  CHECK(T70DpmaifBuildTxPacketPlan(&Ring, 1U, 0U, 0U, 0U,
      Segments, 1U, Descriptors, sizeof(Descriptors), &Plan) ==
      T70DpmaifRingInvalid);
  CHECK(Plan.StartIndex == BeforePlan.StartIndex &&
        Plan.NextIndex == BeforePlan.NextIndex &&
        Plan.DescriptorCount == BeforePlan.DescriptorCount &&
        Plan.PacketBytes == BeforePlan.PacketBytes);
  for (unsigned i = 0; i < sizeof(Descriptors); ++i)
    CHECK(Descriptors[i] == 0x3cU);
}

static void TestDmaMask(void)
{
  T70_DPMAIF_TX_RING Ring;
  T70_DPMAIF_TX_SEGMENT Segment = {
    T70_DPMAIF_DMA_MASK_LIMIT - 8U, 8U
  };
  T70_DPMAIF_TX_PACKET_PLAN Plan = {9U, 8U, 7U, 6U};
  T70_DPMAIF_TX_PACKET_PLAN Before = Plan;
  uint8_t Descriptors[16], BeforeBytes[16];

  CHECK(T70DpmaifTxRingInitialize(&Ring, 4U) == T70DpmaifRingSuccess);
  memset(Descriptors, 0x4d, sizeof(Descriptors));
  memcpy(BeforeBytes, Descriptors, sizeof(Descriptors));
  CHECK(T70DpmaifBuildTxPacketPlan(&Ring, 1U, 0U, 0U, 0U,
      &Segment, 1U, Descriptors, sizeof(Descriptors), &Plan) ==
      T70DpmaifRingSuccess);
  CHECK(Plan.PacketBytes == 8U && Plan.DescriptorCount == 2U);
  memcpy(BeforeBytes, Descriptors, sizeof(Descriptors));

  Segment.DmaAddress = T70_DPMAIF_DMA_MASK_LIMIT - 7U;
  Plan = Before;
  CHECK(T70DpmaifBuildTxPacketPlan(&Ring, 1U, 0U, 0U, 0U,
      &Segment, 1U, Descriptors, sizeof(Descriptors), &Plan) ==
      T70DpmaifRingInvalid);
  CHECK(memcmp(&Plan, &Before, sizeof(Plan)) == 0);
  CHECK(memcmp(Descriptors, BeforeBytes, sizeof(Descriptors)) == 0);

  Segment.DmaAddress = T70_DPMAIF_DMA_MASK_LIMIT;
  CHECK(T70DpmaifBuildTxPacketPlan(&Ring, 1U, 0U, 0U, 0U,
      &Segment, 1U, Descriptors, sizeof(Descriptors), &Plan) ==
      T70DpmaifRingInvalid);
  CHECK(memcmp(&Plan, &Before, sizeof(Plan)) == 0);
  CHECK(memcmp(Descriptors, BeforeBytes, sizeof(Descriptors)) == 0);
}

static void TestFailures(void)
{
  T70_DPMAIF_TX_RING Ring;
  T70_DPMAIF_TX_SEGMENT Segments[2] = {
    { 0x1000U, UINT16_MAX }, { 0x20000U, 1U }
  };
  T70_DPMAIF_TX_PACKET_PLAN Plan = {9U, 8U, 7U, 6U};
  T70_DPMAIF_TX_PACKET_PLAN Before = Plan;
  uint8_t Descriptors[24], BeforeBytes[24];
  uint32_t NewIndex;

  CHECK(T70DpmaifTxRingInitialize(&Ring, 4U) == T70DpmaifRingSuccess);
  memset(Descriptors, 0x77, sizeof(Descriptors));
  memcpy(BeforeBytes, Descriptors, sizeof(Descriptors));
  CHECK(T70DpmaifBuildTxPacketPlan(&Ring, 1U, 0U, 0U, 0U,
      Segments, 2U, Descriptors, sizeof(Descriptors), &Plan) ==
      T70DpmaifRingInvalid);
  CHECK(memcmp(&Plan, &Before, sizeof(Plan)) == 0);
  CHECK(memcmp(Descriptors, BeforeBytes, sizeof(Descriptors)) == 0);

  CHECK(T70DpmaifBuildTxPacketPlan(&Ring, 1U, 0U, 0U, 0U,
      Segments, 0U, Descriptors, sizeof(Descriptors), &Plan) ==
      T70DpmaifRingInvalid);
  CHECK(T70DpmaifBuildTxPacketPlan(&Ring, 1U, 0U, 2U, 0U,
      Segments, 1U, Descriptors, sizeof(Descriptors), &Plan) ==
      T70DpmaifRingInvalid);
  CHECK(T70DpmaifBuildTxPacketPlan(&Ring, 1U, 0U, 0U, 0U,
      Segments, 1U, Descriptors, 15U, &Plan) == T70DpmaifRingInvalid);

  CHECK(T70DpmaifTxRingReserve(&Ring, 3U, &Plan.StartIndex,
      &Plan.NextIndex) == T70DpmaifRingSuccess);
  CHECK(T70DpmaifTxRingCommit(&Ring, Plan.StartIndex, 3U, &NewIndex) ==
      T70DpmaifRingSuccess);
  CHECK(T70DpmaifBuildTxPacketPlan(&Ring, 1U, 0U, 0U, 0U,
      Segments, 1U, Descriptors, sizeof(Descriptors), &Plan) ==
      T70DpmaifRingNoSpace);
}

int main(void)
{
  TestPlanAndCommit();
  TestDmaMask();
  TestFailures();
  if (Failure) {
    fprintf(stderr, "DpmaifTxPacketTest failed at line %u after %u checks\n",
            Failure, Checks);
    return (int)Failure;
  }
  printf("T70 DPMAIF TX packet plan: %u buffer and descriptor checks passed\n", Checks);
  return 0;
}
