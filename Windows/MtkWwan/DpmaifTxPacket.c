/* SPDX-License-Identifier: GPL-2.0-only */
/* Message/payload DRB construction follows ccci_hif_dpmaif_v3.c. */
#include "DpmaifTxPacket.h"
#include "DpmaifDma.h"
#include <limits.h>

T70_DPMAIF_RING_STATUS
T70DpmaifBuildTxPacketPlan (
  const T70_DPMAIF_TX_RING *Ring,
  uint8_t ChannelId,
  uint16_t CountLow,
  uint8_t Ipv4Checksum,
  uint8_t L4Checksum,
  const T70_DPMAIF_TX_SEGMENT *Segments,
  uint32_t SegmentCount,
  uint8_t *DescriptorBytes,
  size_t DescriptorBufferBytes,
  T70_DPMAIF_TX_PACKET_PLAN *Plan
  )
{
  T70_DPMAIF_TX_PACKET_PLAN Result;
  T70_DPMAIF_TX_MESSAGE Message;
  uint64_t PacketLength = 0;
  size_t RequiredBytes;
  uint32_t StartIndex;
  uint32_t NextIndex;
  uint32_t DescriptorCount;
  uint32_t Index;
  T70_DPMAIF_RING_STATUS Status;
  T70_DPMAIF_STATUS DescriptorStatus;

  if (Ring == NULL || Segments == NULL || SegmentCount == 0U ||
      DescriptorBytes == NULL || Plan == NULL ||
      Ipv4Checksum > 1U || L4Checksum > 1U ||
      SegmentCount == UINT32_MAX) {
    return T70DpmaifRingInvalid;
  }
  DescriptorCount = SegmentCount + 1U;
  if (DescriptorCount > UINT32_MAX / T70_DPMAIF_TX_DATA_BYTES) {
    return T70DpmaifRingInvalid;
  }
  RequiredBytes = (size_t)DescriptorCount * T70_DPMAIF_TX_DATA_BYTES;
  if (DescriptorBufferBytes < RequiredBytes) {
    return T70DpmaifRingInvalid;
  }
  for (Index = 0; Index < SegmentCount; ++Index) {
    const T70_DPMAIF_TX_SEGMENT *Segment = &Segments[Index];
    if (Segment->Length == 0U || Segment->Length > UINT16_MAX ||
        !T70DpmaifDmaRangeValid(Segment->DmaAddress, Segment->Length)) {
      return T70DpmaifRingInvalid;
    }
    PacketLength += Segment->Length;
    if (PacketLength > UINT16_MAX) {
      return T70DpmaifRingInvalid;
    }
  }

  Status = T70DpmaifTxRingReserve(
    Ring, DescriptorCount, &StartIndex, &NextIndex);
  if (Status != T70DpmaifRingSuccess) {
    return Status;
  }

  Message = (T70_DPMAIF_TX_MESSAGE){0};
  Message.Type = 1U;
  Message.Continuation = 1U;
  Message.PacketLength = (uint16_t)PacketLength;
  Message.CountLow = CountLow;
  Message.ChannelId = ChannelId;
  Message.NetworkType = 0U;
  Message.Ipv4Checksum = Ipv4Checksum;
  Message.L4Checksum = L4Checksum;

  DescriptorStatus = T70DpmaifEncodeTxMessage(
    &Message, DescriptorBytes, T70_DPMAIF_TX_MESSAGE_BYTES);
  if (DescriptorStatus != T70DpmaifSuccess) {
    return T70DpmaifRingInvalid;
  }

  for (Index = 0; Index < SegmentCount; ++Index) {
    const T70_DPMAIF_TX_SEGMENT *Segment = &Segments[Index];
    T70_DPMAIF_TX_DATA Data = {0};
    Data.Type = 0U;
    Data.Continuation = (uint8_t)(Index + 1U < SegmentCount);
    Data.AddressExtension = (uint8_t)(Segment->DmaAddress >> 32);
    Data.DataLength = (uint16_t)Segment->Length;
    Data.AddressLow = (uint32_t)Segment->DmaAddress;
    Data.DataAddress = Segment->DmaAddress;
    DescriptorStatus = T70DpmaifEncodeTxData(&Data,
      DescriptorBytes + (size_t)(Index + 1U) * T70_DPMAIF_TX_DATA_BYTES,
      T70_DPMAIF_TX_DATA_BYTES);
    if (DescriptorStatus != T70DpmaifSuccess) {
      return T70DpmaifRingInvalid;
    }
  }

  Result.StartIndex = StartIndex;
  Result.NextIndex = NextIndex;
  Result.DescriptorCount = DescriptorCount;
  Result.PacketBytes = (uint32_t)PacketLength;
  *Plan = Result;
  return T70DpmaifRingSuccess;
}
