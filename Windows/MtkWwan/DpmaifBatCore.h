/* SPDX-License-Identifier: GPL-2.0-only */
/* DPMAIF BAT buffer ownership, independent of the OS DMA allocator. */
#ifndef T70_DPMAIF_BAT_CORE_H
#define T70_DPMAIF_BAT_CORE_H

#include <stddef.h>
#include <stdint.h>
#include "DpmaifDescriptor.h"
#include "DpmaifRing.h"

#define T70_DPMAIF_BUFFER_TYPE_REQUEST 0U
#define T70_DPMAIF_BUFFER_TYPE_FRAGMENT 1U

typedef enum {
  T70DpmaifBatFree = 0,
  T70DpmaifBatPosted,
  T70DpmaifBatInFlight
} T70_DPMAIF_BAT_SLOT_STATE;

typedef struct {
  uint64_t Token;
  uint64_t DmaAddress;
  uint32_t BufferBytes;
  uint8_t State;
} T70_DPMAIF_BAT_SLOT;

typedef struct {
  uint32_t Index;
  uint32_t NextIndex;
  uint64_t Token;
  uint64_t DmaAddress;
  uint32_t BufferBytes;
  T70_DPMAIF_BAT Descriptor;
} T70_DPMAIF_BAT_PLAN;

typedef struct {
  uint32_t Capacity;
  uint32_t HardwareReadIndex;
  uint32_t WriteIndex;
  uint8_t BufferType;
  T70_DPMAIF_BAT_SLOT *Slots;
} T70_DPMAIF_BAT_CORE;

T70_DPMAIF_RING_STATUS T70DpmaifBatInitialize(
  T70_DPMAIF_BAT_CORE *Core, T70_DPMAIF_BAT_SLOT *Slots, uint32_t Capacity);
T70_DPMAIF_RING_STATUS T70DpmaifBatPostInitialBuffers(
  T70_DPMAIF_BAT_CORE *Core,
  T70_DPMAIF_BAT_SLOT *Slots,
  uint32_t Capacity,
  const uint64_t *BufferAddresses,
  uint32_t AddressCount,
  uint32_t BufferBytes,
  uint32_t BufferAlignment,
  uint8_t *DescriptorTable,
  size_t DescriptorTableBytes,
  uint32_t *PostedCount);
T70_DPMAIF_RING_STATUS T70DpmaifBatObserveHardwareRead(
  T70_DPMAIF_BAT_CORE *Core, uint32_t HardwareReadIndex, uint32_t *Consumed);
T70_DPMAIF_RING_STATUS T70DpmaifBatPreparePost(
  const T70_DPMAIF_BAT_CORE *Core, uint64_t Token, uint64_t DmaAddress,
  uint32_t BufferBytes, T70_DPMAIF_BAT_PLAN *Plan);
T70_DPMAIF_RING_STATUS T70DpmaifBatCommitPost(
  T70_DPMAIF_BAT_CORE *Core, const T70_DPMAIF_BAT_PLAN *Plan,
  uint32_t *NewWriteIndex);
T70_DPMAIF_RING_STATUS T70DpmaifBatClaim(
  T70_DPMAIF_BAT_CORE *Core, const T70_DPMAIF_DATA_PIT *Pit,
  uint64_t *Token, uint32_t *Offset, uint32_t *Length);
T70_DPMAIF_RING_STATUS T70DpmaifBatReturn(
  T70_DPMAIF_BAT_CORE *Core, uint32_t BufferId, uint64_t Token);

#endif
