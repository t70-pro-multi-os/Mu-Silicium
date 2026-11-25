/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T70_DPMAIF_TX_PACKET_H
#define T70_DPMAIF_TX_PACKET_H

#include <stddef.h>
#include <stdint.h>
#include "DpmaifDescriptor.h"
#include "DpmaifRing.h"

typedef struct {
  uint64_t DmaAddress;
  uint32_t Length;
} T70_DPMAIF_TX_SEGMENT;

typedef struct {
  uint32_t StartIndex;
  uint32_t NextIndex;
  uint32_t DescriptorCount;
  uint32_t PacketBytes;
} T70_DPMAIF_TX_PACKET_PLAN;

/* Builds the stock message descriptor followed by payload descriptors.
 * The caller serializes the queue, copies descriptors into the wrapped ring,
 * then commits the reserved producer index before ringing the hardware. */
T70_DPMAIF_RING_STATUS T70DpmaifBuildTxPacketPlan(
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
  );

#endif
