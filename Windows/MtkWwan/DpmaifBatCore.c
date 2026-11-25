/* SPDX-License-Identifier: GPL-2.0-only */
/* T70 BAT allocation/reclaim state follows dpmaif_bat_v3.c. */
#include "DpmaifBatCore.h"
#include "DpmaifDma.h"
#include <stddef.h>
#include <string.h>

static int
ValidCore (
  const T70_DPMAIF_BAT_CORE *Core
  )
{
  return Core != NULL && Core->Slots != NULL && Core->Capacity > 1U &&
      Core->HardwareReadIndex < Core->Capacity &&
      Core->WriteIndex < Core->Capacity;
}

static int
ValidBuffer (
  uint64_t Token,
  uint64_t Address,
  uint32_t Bytes
  )
{
  return Token != 0 && T70DpmaifDmaRangeValid(Address, Bytes);
}

T70_DPMAIF_RING_STATUS
T70DpmaifBatInitialize (
  T70_DPMAIF_BAT_CORE *Core,
  T70_DPMAIF_BAT_SLOT *Slots,
  uint32_t Capacity
  )
{
  T70_DPMAIF_BAT_CORE Result;

  if (Core == NULL || Slots == NULL || Capacity <= 1U) {
    return T70DpmaifRingInvalid;
  }
  memset(Slots, 0, (size_t)Capacity * sizeof(*Slots));
  Result.Capacity = Capacity;
  Result.HardwareReadIndex = 0;
  Result.WriteIndex = 0;
  Result.BufferType = T70_DPMAIF_BUFFER_TYPE_REQUEST;
  Result.Slots = Slots;
  *Core = Result;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifBatPostInitialBuffers (
  T70_DPMAIF_BAT_CORE *Core,
  T70_DPMAIF_BAT_SLOT *Slots,
  uint32_t Capacity,
  const uint64_t *BufferAddresses,
  uint32_t AddressCount,
  uint32_t BufferBytes,
  uint32_t BufferAlignment,
  uint8_t *DescriptorTable,
  size_t DescriptorTableBytes,
  uint32_t *PostedCount
  )
{
  size_t RequiredBytes;
  uint32_t Index;
  T70_DPMAIF_RING_STATUS Status;

  if (Core == NULL || Slots == NULL || Capacity < 2U ||
      BufferAddresses == NULL || AddressCount == 0U ||
      AddressCount >= Capacity || BufferBytes == 0U ||
      BufferAlignment == 0U ||
      (BufferAlignment & (BufferAlignment - 1U)) != 0U ||
      DescriptorTable == NULL || PostedCount == NULL) {
    return T70DpmaifRingInvalid;
  }
  RequiredBytes = (size_t)Capacity * T70_DPMAIF_BAT_BYTES;
  if (RequiredBytes / T70_DPMAIF_BAT_BYTES != (size_t)Capacity) {
    return T70DpmaifRingInvalid;
  }
  if (DescriptorTableBytes < RequiredBytes) {
    return T70DpmaifRingNoSpace;
  }
  for (Index = 0U; Index < AddressCount; ++Index) {
    if (!ValidBuffer((uint64_t)Index + 1U,BufferAddresses[Index],BufferBytes) ||
        (BufferAddresses[Index] & (BufferAlignment - 1U)) != 0U) {
      return T70DpmaifRingInvalid;
    }
  }

  Status = T70DpmaifBatInitialize(Core,Slots,Capacity);
  if (Status != T70DpmaifRingSuccess) return Status;
  for (Index = 0U; Index < RequiredBytes; ++Index) {
    ((volatile uint8_t *)DescriptorTable)[Index] = 0U;
  }
  for (Index = 0U; Index < AddressCount; ++Index) {
    T70_DPMAIF_BAT_PLAN Plan;
    uint32_t NewWriteIndex;
    T70_DPMAIF_BAT Descriptor;

    Status = T70DpmaifBatPreparePost(Core,(uint64_t)Index+1U,
        BufferAddresses[Index],BufferBytes,&Plan);
    if (Status != T70DpmaifRingSuccess) return Status;
    Descriptor=Plan.Descriptor;
    if (T70DpmaifEncodeBat(&Descriptor,
          DescriptorTable+(size_t)Index*T70_DPMAIF_BAT_BYTES,
          T70_DPMAIF_BAT_BYTES)!=T70DpmaifSuccess) {
      return T70DpmaifRingInvalid;
    }
    Status = T70DpmaifBatCommitPost(Core,&Plan,&NewWriteIndex);
    if (Status != T70DpmaifRingSuccess) return Status;
  }
  *PostedCount=AddressCount;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifBatObserveHardwareRead (
  T70_DPMAIF_BAT_CORE *Core,
  uint32_t HardwareReadIndex,
  uint32_t *Consumed
  )
{
  uint32_t Posted;
  uint32_t Advanced;

  if (!ValidCore(Core) || Consumed == NULL ||
      HardwareReadIndex >= Core->Capacity) {
    return T70DpmaifRingInvalid;
  }
  if (T70DpmaifRingReadable(Core->Capacity, Core->HardwareReadIndex,
        HardwareReadIndex, &Advanced) != T70DpmaifRingSuccess ||
      T70DpmaifRingReadable(Core->Capacity, Core->HardwareReadIndex,
        Core->WriteIndex, &Posted) != T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  if (Advanced > Posted) {
    return T70DpmaifRingStale;
  }
  Core->HardwareReadIndex = HardwareReadIndex;
  *Consumed = Advanced;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifBatPreparePost (
  const T70_DPMAIF_BAT_CORE *Core,
  uint64_t Token,
  uint64_t DmaAddress,
  uint32_t BufferBytes,
  T70_DPMAIF_BAT_PLAN *Plan
  )
{
  T70_DPMAIF_BAT_PLAN Result;
  uint32_t FreeSlots;
  uint32_t Next;

  if (!ValidCore(Core) || Plan == NULL ||
      !ValidBuffer(Token, DmaAddress, BufferBytes)) {
    return T70DpmaifRingInvalid;
  }
  if (T70DpmaifRingWritable(Core->Capacity, Core->HardwareReadIndex,
        Core->WriteIndex, &FreeSlots) != T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  if (FreeSlots == 0U) {
    return T70DpmaifRingNoSpace;
  }
  if (T70DpmaifRingAdvance(Core->Capacity, Core->WriteIndex, 1U, &Next) !=
      T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  if (Core->Slots[Core->WriteIndex].State != T70DpmaifBatFree) {
    return T70DpmaifRingNoSpace;
  }

  memset(&Result, 0, sizeof(Result));
  Result.Index = Core->WriteIndex;
  Result.NextIndex = Next;
  Result.Token = Token;
  Result.DmaAddress = DmaAddress;
  Result.BufferBytes = BufferBytes;
  Result.Descriptor.BufferAddress = DmaAddress;
  *Plan = Result;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifBatCommitPost (
  T70_DPMAIF_BAT_CORE *Core,
  const T70_DPMAIF_BAT_PLAN *Plan,
  uint32_t *NewWriteIndex
  )
{
  T70_DPMAIF_BAT_SLOT *Slot;
  uint32_t FreeSlots;
  uint32_t Next;

  if (!ValidCore(Core) || Plan == NULL || NewWriteIndex == NULL ||
      Plan->Index >= Core->Capacity || Plan->NextIndex >= Core->Capacity ||
      !ValidBuffer(Plan->Token, Plan->DmaAddress, Plan->BufferBytes) ||
      Plan->Descriptor.BufferAddress != Plan->DmaAddress ||
      Plan->Descriptor.Reserved != 0U) {
    return T70DpmaifRingInvalid;
  }
  if (Plan->Index != Core->WriteIndex) {
    return T70DpmaifRingStale;
  }
  if (T70DpmaifRingAdvance(Core->Capacity, Core->WriteIndex, 1U, &Next) !=
      T70DpmaifRingSuccess || Plan->NextIndex != Next) {
    return T70DpmaifRingStale;
  }
  if (T70DpmaifRingWritable(Core->Capacity, Core->HardwareReadIndex,
        Core->WriteIndex, &FreeSlots) != T70DpmaifRingSuccess) {
    return T70DpmaifRingInvalid;
  }
  if (FreeSlots == 0U) {
    return T70DpmaifRingNoSpace;
  }
  if (Core->Slots[Plan->Index].State != T70DpmaifBatFree) {
    return T70DpmaifRingStale;
  }

  Slot = &Core->Slots[Plan->Index];
  Slot->Token = Plan->Token;
  Slot->DmaAddress = Plan->DmaAddress;
  Slot->BufferBytes = Plan->BufferBytes;
  Slot->State = T70DpmaifBatPosted;
  Core->WriteIndex = Plan->NextIndex;
  *NewWriteIndex = Core->WriteIndex;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifBatClaim (
  T70_DPMAIF_BAT_CORE *Core,
  const T70_DPMAIF_DATA_PIT *Pit,
  uint64_t *Token,
  uint32_t *Offset,
  uint32_t *Length
  )
{
  uint32_t BufferId;
  uint64_t Delta;
  T70_DPMAIF_BAT_SLOT *Slot;

  if (!ValidCore(Core) || Pit == NULL || Token == NULL ||
      Offset == NULL || Length == NULL || Pit->PacketType != 0U ||
      Pit->BufferType > T70_DPMAIF_BUFFER_TYPE_FRAGMENT ||
      Pit->BufferType != Core->BufferType ||
      !T70DpmaifDmaRangeValid(Pit->DataAddress, Pit->DataLength)) {
    return T70DpmaifRingInvalid;
  }
  BufferId = Pit->CombinedBufferId;
  if (BufferId >= Core->Capacity) {
    return T70DpmaifRingInvalid;
  }
  Slot = &Core->Slots[BufferId];
  if (Slot->State != T70DpmaifBatPosted) {
    return T70DpmaifRingStale;
  }
  if (Pit->DataAddress < Slot->DmaAddress) {
    return T70DpmaifRingInvalid;
  }
  Delta = Pit->DataAddress - Slot->DmaAddress;
  if (Delta >= Slot->BufferBytes ||
      Pit->DataLength > Slot->BufferBytes - (uint32_t)Delta) {
    return T70DpmaifRingInvalid;
  }

  Slot->State = T70DpmaifBatInFlight;
  *Token = Slot->Token;
  *Offset = (uint32_t)Delta;
  *Length = Pit->DataLength;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifBatReturn (
  T70_DPMAIF_BAT_CORE *Core,
  uint32_t BufferId,
  uint64_t Token
  )
{
  T70_DPMAIF_BAT_SLOT *Slot;

  if (!ValidCore(Core) || BufferId >= Core->Capacity || Token == 0) {
    return T70DpmaifRingInvalid;
  }
  Slot = &Core->Slots[BufferId];
  if (Slot->State != T70DpmaifBatInFlight || Slot->Token != Token) {
    return T70DpmaifRingStale;
  }
  memset(Slot, 0, sizeof(*Slot));
  return T70DpmaifRingSuccess;
}
