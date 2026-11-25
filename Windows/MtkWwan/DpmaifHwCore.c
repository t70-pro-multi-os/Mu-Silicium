/* SPDX-License-Identifier: GPL-2.0-only */
/* Queue register programming derived from T70 DPMAIF v3 kernel helpers. */
#include "DpmaifHwCore.h"
#include "DpmaifDma.h"
#include <limits.h>
#include <string.h>

#define RemapAddressLimit (UINT64_C(1) << 34)

enum {
  RegAoUlChannelArb0 = 0x01cU,
  RegPdDlBase = 0x100U,
  RegPdDlBatInit = RegPdDlBase + 0x000U,
  RegPdDlBatInitCon0 = RegPdDlBase + 0x008U,
  RegPdDlBatInitCon1 = RegPdDlBase + 0x00cU,
  RegPdDlBatAdd = RegPdDlBase + 0x004U,
  RegPdDlPitInit = RegPdDlBase + 0x020U,
  RegPdDlPitInitCon0 = RegPdDlBase + 0x028U,
  RegPdDlPitInitCon1 = RegPdDlBase + 0x02cU,
  RegPdDlPitInitCon3 = RegPdDlBase + 0x034U,
  RegAoDlSram = 0xc00U,
  RegAoUlSram = 0xd00U,
  RegAoDlPktInfoCon0 = RegAoDlSram + 0x000U,
  RegAoDlPktInfoCon1 = RegAoDlSram + 0x004U,
  RegAoDlPktInfoCon2 = RegAoDlSram + 0x008U,
  RegAoDlReadyCheck = RegAoDlSram + 0x00cU,
  RegAoDlFragReadyCheck = RegAoDlSram + 0x010U,
  RegAoUlChannel0Con0 = RegAoUlSram + 0x010U,
  RegAoUlChannel0Con1 = RegAoUlSram + 0x014U,
  RegPdMiscBase = 0x400U,
  RegPdMiscApUlInterruptStatus = RegPdMiscBase + 0x000U,
  RegPdMiscApDlInterruptStatus = RegPdMiscBase + 0x050U,
  RegPdMiscApIpBusy = RegPdMiscBase + 0x060U,
  RegPdMiscClockEnable = RegPdMiscBase + 0x068U,
  RegPdMiscOverwrite = RegPdMiscBase + 0x090U,
  RegPdMiscMemoryClear = RegPdMiscBase + 0x094U,
  RegAoUlInterruptStatus = 0x080U,
  RegAoUlInterruptClear = 0x084U,
  RegAoUlInterruptSet = 0x088U,
  RegAoUlL1Timer = 0x08cU,
  RegAoDlInterruptStatus = 0x090U,
  RegAoDlInterruptClear = 0x094U,
  RegAoDlInterruptSet = 0x098U,
  RegAoUlIpBusyMask = 0x09cU,
  RegAoDlNoSramInitSet = 0x400U,
  RegPdSramMiscBase = 0xe00U,
  RegPdDlReserveRw = RegPdDlBase + 0x0a8U,
  RemapBankBytes = 0x02000000U,
  RemapMaximumBytes = 0x30000000U,
  RemapGroupBytes = 0x06000000U,
  MaskMemoryClear = 1U,
  MaskSramSync = 1U,
  MaskUlInitDone = 1U,
  MaskDlInitDone = 1U,
  MaskBatAddrHigh = 0xff000000U,
  MaskBatSize = 0x0000ffffU,
  MaskBatEnable = 1U << 16,
  MaskPitAddrHigh = 0xff000000U,
  MaskPitSize = 0x0003ffffU,
  MaskPitEnable = 0x01U,
  MaskBatRemainMin = 0x0000ff00U,
  MaskBatBidMaxCount = 0xffff0000U,
  MaskPitCheckCount = 0xff000000U,
  MaskBatBufferSize = 0x0001ff00U,
  MaskBatReserveLength = 0x000000ffU,
  MaskPacketAlignment = 0x00c00000U,
  MaskBatCheckThreshold = 0x003f0000U,
  MaskFragmentFeature = 1U << 28,
  MaskFragmentBufferSize = 0x0000ff00U,
  MaskFragmentCheckThreshold = 0x000000ffU,
  MaskBatCacheEnable = 1U << 22,
  MaskPitBurstEnable = 1U << 13,
  MaskUlReady = 0x0000000fU,
  MaskUlArbitration = 0x00000f00U,
  MaskInitNotReady = 1U << 31,
  MaskBatAddNotReady = 1U << 31,
  MaskBatFragmentAdd = 1U << 16,
  ValueInit = (1U << 31) | 1U,
  ValueInitFragment = (1U << 31) | (1U << 16) | 1U,
  ValueInitEnableOnly = 1U << 31,
  ValueInitFragmentEnableOnly = (1U << 31) | (1U << 16),
  ValueBatAddUpdate = 1U << 31,
  ValuePacketAlignmentEnable = 1U << 23,
  ValueMtu = 3U * 1024U + 8U,
  ValueBatRemain = 64U,
  ValueBatRemainUnit = 16U,
  ValueMaxPacketPerBat = 1U,
  ValueBatPacketBytes = 128U * 28U,
  ValueFragmentPacketBytes = 128U,
  ValueBatReserveBytes = 0U,
  ValueBatCheckThreshold = 62U,
  ValuePitCheckThreshold = 62U * 2U,
  ValueReceiveRingCheck = 64U,
  ValueFragmentCheckThreshold = 62U,
  ValueDmaAlign = 64U,
  ValueDmaAlignBytes = 64U,
  ValueBatBufferUnit = 128U,
  ValuePitCountWords = 4U,
  ValueBatCountWords = 2U,
  ValueTxCountWords = 2U
};

static int
ValidTable (
  uint64_t Address,
  uint32_t Entries,
  uint32_t EntryBytes,
  uint32_t MaxEntries
  )
{
  uint64_t Bytes;
  if (Entries < 2U || Entries > MaxEntries ||
      (Address & (ValueDmaAlignBytes - 1U)) != 0U) {
    return 0;
  }
  Bytes = (uint64_t)Entries * EntryBytes;
  return T70DpmaifDmaRangeValid(Address, Bytes);
}

static int
Append (
  T70_DPMAIF_HW_OP *Plan,
  size_t *Count,
  uint8_t Region,
  uint8_t Kind,
  uint32_t Offset,
  uint32_t Mask,
  uint32_t Value,
  uint32_t PollLimit
  )
{
  T70_DPMAIF_HW_OP *Op;
  if (*Count >= T70_DPMAIF_HW_MAX_PLAN_OPS) {
    return 0;
  }
  Op = &Plan[(*Count)++];
  Op->Region = Region;
  Op->Kind = Kind;
  Op->Reserved = 0U;
  Op->Offset = Offset;
  Op->Mask = Mask;
  Op->Value = Value;
  Op->PollLimit = PollLimit;
  return 1;
}

#define PUSH_WRITE(R,O,V)   do { if (!Append(Plan,&Count,(R),T70DpmaifHwWrite,(O),UINT32_MAX,(V),0U))     return T70DpmaifRingNoSpace; } while (0)
#define PUSH_UPDATE(R,O,M,V)   do { if (!Append(Plan,&Count,(R),T70DpmaifHwUpdate,(O),(M),(V),0U))     return T70DpmaifRingNoSpace; } while (0)
#define PUSH_POLL_CLEAR(R,O,M)   do { if (!Append(Plan,&Count,(R),T70DpmaifHwPollClear,(O),(M),0U,        T70_DPMAIF_HW_POLL_LIMIT)) return T70DpmaifRingNoSpace; } while (0)

T70_DPMAIF_RING_STATUS
T70DpmaifBuildQueueInitPlan (
  const T70_DPMAIF_QUEUE_CONFIG *Config,
  T70_DPMAIF_HW_OP *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  )
{
  T70_DPMAIF_HW_OP Plan[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t Count = 0U;
  size_t Index;
  uint32_t EncodedSize;
  uint32_t AddrHigh;

  if (Config == NULL || Operations == NULL || OperationCount == NULL ||
      Config->FragmentEnabled > 1U ||
      !ValidTable(Config->PitAddress, Config->PitEntries,
          16U, MaskPitSize) ||
      !ValidTable(Config->RequestBatAddress, Config->RequestBatEntries,
          8U, MaskBatSize)) {
    return T70DpmaifRingInvalid;
  }
  if (Config->FragmentEnabled) {
    if (!ValidTable(Config->FragmentBatAddress, Config->FragmentBatEntries,
            8U, MaskBatSize)) {
      return T70DpmaifRingInvalid;
    }
  } else if (Config->FragmentBatAddress != 0U ||
      Config->FragmentBatEntries != 0U) {
    return T70DpmaifRingInvalid;
  }
  for (Index = 0U; Index < T70_DPMAIF_HW_TX_QUEUES; ++Index) {
    if (!ValidTable(Config->TxDrbAddress[Index],
            Config->TxDrbEntries[Index], 8U, UINT16_MAX / ValueTxCountWords)) {
      return T70DpmaifRingInvalid;
    }
  }

  /* dpmaif_rx_hw_init: RX thresholds, performance bits, then PIT setup. */
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegAoDlPktInfoCon0,
      MaskBatRemainMin,
      ((ValueBatRemain / ValueBatRemainUnit) << 8) & MaskBatRemainMin);
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegAoDlPktInfoCon0,
      MaskBatBidMaxCount,
      (ValueMaxPacketPerBat << 16) & MaskBatBidMaxCount);
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegAoDlReadyCheck,
      MaskPacketAlignment, ValuePacketAlignmentEnable);
  PUSH_WRITE(T70DpmaifHwRegionPdUl, RegAoDlPktInfoCon1, ValueMtu);
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegAoDlPktInfoCon2,
      MaskPitCheckCount,
      (ValuePitCheckThreshold << 24) & MaskPitCheckCount);
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegAoDlPktInfoCon0,
      MaskBatReserveLength, ValueReceiveRingCheck & MaskBatReserveLength);
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegPdDlBatInitCon1,
      MaskBatCacheEnable, MaskBatCacheEnable);
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegAoDlReadyCheck,
      MaskPitBurstEnable, MaskPitBurstEnable);

  PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl, RegPdDlPitInit,
      MaskInitNotReady);
  PUSH_WRITE(T70DpmaifHwRegionPdUl, RegPdDlPitInitCon0,
      (uint32_t)Config->PitAddress);
  AddrHigh = (uint32_t)((Config->PitAddress >> 8) & MaskPitAddrHigh);
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegPdDlPitInitCon1,
      MaskPitAddrHigh | MaskPitSize,
      AddrHigh | (Config->PitEntries & MaskPitSize));
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegPdDlPitInitCon3,
      MaskPitEnable, MaskPitEnable);
  PUSH_WRITE(T70DpmaifHwRegionPdUl, RegPdDlPitInit, ValueInit);
  PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl, RegPdDlPitInit,
      MaskInitNotReady);

  /* ccci_dpmaif_bat_hw_init_v3: packet BAT table. */
  EncodedSize = ValueBatPacketBytes / ValueBatBufferUnit;
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegAoDlPktInfoCon2,
      MaskBatBufferSize, (EncodedSize << 8) & MaskBatBufferSize);
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegAoDlPktInfoCon2,
      MaskBatReserveLength, ValueBatReserveBytes & MaskBatReserveLength);
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegAoDlReadyCheck,
      MaskBatCheckThreshold,
      (ValueBatCheckThreshold << 16) & MaskBatCheckThreshold);
  PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl, RegPdDlBatInit,
      MaskInitNotReady);
  PUSH_WRITE(T70DpmaifHwRegionPdUl, RegPdDlBatInitCon0,
      (uint32_t)Config->RequestBatAddress);
  AddrHigh = (uint32_t)((Config->RequestBatAddress >> 8) & MaskBatAddrHigh);
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegPdDlBatInitCon1,
      MaskBatAddrHigh, AddrHigh);
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegPdDlBatInitCon1,
      MaskBatSize, Config->RequestBatEntries & MaskBatSize);
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegPdDlBatInitCon1,
      MaskBatEnable, 0U);
  PUSH_WRITE(T70DpmaifHwRegionPdUl, RegPdDlBatInit, ValueInit);
  PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl, RegPdDlBatInit,
      MaskInitNotReady);

  if (Config->FragmentEnabled) {
    EncodedSize = ValueFragmentPacketBytes / ValueBatBufferUnit;
    PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegAoDlFragReadyCheck,
        MaskFragmentFeature, MaskFragmentFeature);
    PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegAoDlFragReadyCheck,
        MaskFragmentBufferSize, (EncodedSize << 8) & MaskFragmentBufferSize);
    PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegAoDlFragReadyCheck,
        MaskFragmentCheckThreshold,
        ValueFragmentCheckThreshold & MaskFragmentCheckThreshold);
    PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl, RegPdDlBatInit,
        MaskInitNotReady);
    PUSH_WRITE(T70DpmaifHwRegionPdUl, RegPdDlBatInitCon0,
        (uint32_t)Config->FragmentBatAddress);
    AddrHigh = (uint32_t)((Config->FragmentBatAddress >> 8) & MaskBatAddrHigh);
    PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegPdDlBatInitCon1,
        MaskBatAddrHigh, AddrHigh);
    PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegPdDlBatInitCon1,
        MaskBatSize, Config->FragmentBatEntries & MaskBatSize);
    PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegPdDlBatInitCon1,
        MaskBatEnable, 0U);
    PUSH_WRITE(T70DpmaifHwRegionPdUl, RegPdDlBatInit, ValueInitFragment);
    PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl, RegPdDlBatInit,
        MaskInitNotReady);
  }

  /* dpmaif_tx_hw_init: size/base then ready/arb for four stock UL queues. */
  for (Index = 0U; Index < T70_DPMAIF_HW_TX_QUEUES; ++Index) {
    uint32_t QueueOffset = (uint32_t)Index * 0x10U;
    EncodedSize = Config->TxDrbEntries[Index] * ValueTxCountWords;
    PUSH_UPDATE(T70DpmaifHwRegionPdUl,
        RegAoUlChannel0Con1 + QueueOffset, MaskBatSize, EncodedSize);
    PUSH_WRITE(T70DpmaifHwRegionPdUl,
        RegAoUlChannel0Con0 + QueueOffset,
        (uint32_t)Config->TxDrbAddress[Index]);
    AddrHigh = (uint32_t)((Config->TxDrbAddress[Index] >> 8) & MaskBatAddrHigh);
    PUSH_UPDATE(T70DpmaifHwRegionPdUl,
        RegAoUlChannel0Con1 + QueueOffset, MaskBatAddrHigh, AddrHigh);
    PUSH_UPDATE(T70DpmaifHwRegionAoUl, RegAoUlChannelArb0,
        1U << Index, 1U << Index);
    PUSH_UPDATE(T70DpmaifHwRegionAoUl, RegAoUlChannelArb0,
        1U << (Index + 8U), 1U << (Index + 8U));
  }

  if (Count > OperationCapacity) {
    return T70DpmaifRingNoSpace;
  }
  memcpy(Operations, Plan, Count * sizeof(Plan[0]));
  *OperationCount = Count;
  return T70DpmaifRingSuccess;
}


static uint32_t
RemapValue (
  uint64_t Base,
  uint32_t EnableMask
  )
{
  uint64_t Bank0 = Base;
  uint64_t Bank1 = Base + RemapBankBytes;
  uint64_t Bank2 = Base + (2U * RemapBankBytes);

  return (uint32_t)(((Bank0 >> 24) & 0x3feU) +
      ((Bank1 >> 14) & (0x3feU << 10)) +
      ((Bank2 >> 4) & (0x3feU << 20)) +
      ((EnableMask & 1U) << 0) +
      ((EnableMask & 2U) << 9) +
      ((EnableMask & 4U) << 18));
}

T70_DPMAIF_RING_STATUS
T70DpmaifBuildCommonInitPlan (
  const T70_DPMAIF_MODEM_MEMORY *Memory,
  T70_DPMAIF_HW_OP *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  )
{
  static const uint32_t RemapOffsets[8] = {
    0x0cU, 0x10U, 0x14U, 0x18U, 0x1cU, 0x20U, 0x24U, 0x28U
  };
  T70_DPMAIF_HW_OP Plan[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t Count = 0U;
  size_t Index;
  uint64_t Base;

  if (Memory == NULL || Operations == NULL || OperationCount == NULL ||
      Memory->ModemMemoryBytes < RemapBankBytes ||
      Memory->ModemMemoryBytes > RemapMaximumBytes ||
      (Memory->ModemMemoryBase & (RemapBankBytes - 1U)) != 0U ||
      (Memory->ModemMemoryBytes & (RemapBankBytes - 1U)) != 0U ||
      Memory->ModemMemoryBase >= RemapAddressLimit ||
      Memory->ModemMemoryBytes > RemapAddressLimit - Memory->ModemMemoryBase ||
      !T70DpmaifDmaRangeValid(Memory->ModemMemoryBase,
                              Memory->ModemMemoryBytes)) {
    return T70DpmaifRingInvalid;
  }
  Base = Memory->ModemMemoryBase;

  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegPdMiscMemoryClear,
      MaskMemoryClear, MaskMemoryClear);
  PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl, RegPdMiscMemoryClear,
      MaskMemoryClear);
  PUSH_WRITE(T70DpmaifHwRegionPdUl, RegPdMiscClockEnable, 0x7fU);
  for (Index = 0U; Index < 8U; ++Index) {
    uint64_t GroupOffset = (uint64_t)Index * RemapGroupBytes;
    uint64_t Remaining = Memory->ModemMemoryBytes > GroupOffset ?
        Memory->ModemMemoryBytes - GroupOffset : 0U;
    uint32_t BankCount = (uint32_t)((Remaining + RemapBankBytes - 1U) /
                                    RemapBankBytes);
    uint32_t EnableMask;
    uint64_t BankBase;
    uint32_t Value;
    if (BankCount > 3U) BankCount = 3U;
    EnableMask = BankCount == 0U ? 0U : (1U << BankCount) - 1U;
    BankBase = Base + GroupOffset;
    Value = EnableMask == 0U ? 0U : RemapValue(BankBase, EnableMask);
    PUSH_WRITE(T70DpmaifHwRegionPdUl,
        RegPdSramMiscBase + RemapOffsets[Index], Value);
  }
  PUSH_WRITE(T70DpmaifHwRegionAoUl, RegAoUlL1Timer,
      (1U << 9) | (1U << 10) | (1U << 15) | (1U << 16));
  PUSH_WRITE(T70DpmaifHwRegionPdUl, RegPdDlReserveRw, 0xffU);

  if (Count > OperationCapacity) {
    return T70DpmaifRingNoSpace;
  }
  memcpy(Operations, Plan, Count * sizeof(Plan[0]));
  *OperationCount = Count;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifBuildInterruptInitPlan (
  T70_DPMAIF_HW_OP *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  )
{
  T70_DPMAIF_HW_OP Plan[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t Count = 0U;
  const uint32_t UlMask = 0x01fffc1fU;
  const uint32_t DlMask = 0x00000043U;

  if (Operations == NULL || OperationCount == NULL) {
    return T70DpmaifRingInvalid;
  }

  PUSH_WRITE(T70DpmaifHwRegionPdUl,
      RegPdMiscApUlInterruptStatus, UINT32_MAX);
  PUSH_WRITE(T70DpmaifHwRegionAoUl,
      RegAoUlInterruptClear, UlMask);
  PUSH_WRITE(T70DpmaifHwRegionAoUl,
      RegAoUlInterruptSet, ~UlMask);
  if (!Append(Plan, &Count, T70DpmaifHwRegionAoUl,
          T70DpmaifHwPollMaskedNotEqual, RegAoUlInterruptStatus,
          UlMask, UlMask, T70_DPMAIF_HW_POLL_LIMIT)) {
    return T70DpmaifRingNoSpace;
  }

  PUSH_WRITE(T70DpmaifHwRegionPdUl,
      RegPdMiscApDlInterruptStatus, UINT32_MAX);
  PUSH_WRITE(T70DpmaifHwRegionAoUl,
      RegAoDlInterruptClear, DlMask);
  PUSH_WRITE(T70DpmaifHwRegionAoUl,
      RegAoDlInterruptSet, ~DlMask);
  if (!Append(Plan, &Count, T70DpmaifHwRegionAoUl,
          T70DpmaifHwPollMaskedNotEqual, RegAoDlInterruptStatus,
          DlMask, DlMask, T70_DPMAIF_HW_POLL_LIMIT)) {
    return T70DpmaifRingNoSpace;
  }
  PUSH_WRITE(T70DpmaifHwRegionPdUl, RegPdMiscApIpBusy, UINT32_MAX);
  PUSH_WRITE(T70DpmaifHwRegionAoUl, RegAoUlIpBusyMask, 1U);

  if (Count > OperationCapacity) {
    return T70DpmaifRingNoSpace;
  }
  memcpy(Operations, Plan, Count * sizeof(Plan[0]));
  *OperationCount = Count;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifBuildBatStartPlan (
  const T70_DPMAIF_QUEUE_CONFIG *Config,
  T70_DPMAIF_HW_OP *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  )
{
  T70_DPMAIF_HW_OP Plan[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t Count=0U;
  if (Config==NULL || Operations==NULL || OperationCount==NULL ||
      Config->RequestBatEntries<2U ||
      Config->RequestBatPostedEntries!=Config->RequestBatEntries-1U ||
      Config->FragmentEnabled>1U ||
      (Config->FragmentEnabled &&
       (Config->FragmentBatEntries<2U ||
        Config->FragmentBatPostedEntries!=Config->FragmentBatEntries-1U)) ||
      (!Config->FragmentEnabled &&
       (Config->FragmentBatEntries!=0U || Config->FragmentBatPostedEntries!=0U)))
    return T70DpmaifRingInvalid;

  if (Config->FragmentEnabled) {
    PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl,RegPdDlBatAdd,MaskBatAddNotReady);
    PUSH_WRITE(T70DpmaifHwRegionPdUl,RegPdDlBatAdd,
        ValueBatAddUpdate | MaskBatFragmentAdd | Config->FragmentBatPostedEntries);
    PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl,RegPdDlBatAdd,MaskBatAddNotReady);
  }
  PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl,RegPdDlBatAdd,MaskBatAddNotReady);
  PUSH_WRITE(T70DpmaifHwRegionPdUl,RegPdDlBatAdd,
      ValueBatAddUpdate | Config->RequestBatPostedEntries);
  PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl,RegPdDlBatAdd,MaskBatAddNotReady);

  if (Config->FragmentEnabled) {
    PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl,RegPdDlBatInit,MaskInitNotReady);
    PUSH_UPDATE(T70DpmaifHwRegionPdUl,RegPdDlBatInitCon1,
        MaskBatEnable,MaskBatEnable);
    PUSH_WRITE(T70DpmaifHwRegionPdUl,RegPdDlBatInit,
        ValueInitFragmentEnableOnly);
    PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl,RegPdDlBatInit,MaskInitNotReady);
  }
  PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl,RegPdDlBatInit,MaskInitNotReady);
  PUSH_UPDATE(T70DpmaifHwRegionPdUl,RegPdDlBatInitCon1,
      MaskBatEnable,MaskBatEnable);
  PUSH_WRITE(T70DpmaifHwRegionPdUl,RegPdDlBatInit,ValueInitEnableOnly);
  PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl,RegPdDlBatInit,MaskInitNotReady);

  if (Count>OperationCapacity) return T70DpmaifRingNoSpace;
  memcpy(Operations,Plan,Count*sizeof(Plan[0]));
  *OperationCount=Count;
  return T70DpmaifRingSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifBuildInitDonePlan (
  T70_DPMAIF_HW_OP *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  )
{
  T70_DPMAIF_HW_OP Plan[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t Count = 0U;

  if (Operations == NULL || OperationCount == NULL) {
    return T70DpmaifRingInvalid;
  }
  PUSH_UPDATE(T70DpmaifHwRegionPdUl, RegPdMiscOverwrite,
      MaskSramSync, MaskSramSync);
  PUSH_POLL_CLEAR(T70DpmaifHwRegionPdUl, RegPdMiscOverwrite,
      MaskSramSync);
  PUSH_WRITE(T70DpmaifHwRegionAoUl, 0x000U, MaskUlInitDone);
  PUSH_WRITE(T70DpmaifHwRegionAoUl, RegAoDlNoSramInitSet,
      MaskDlInitDone);

  if (Count > OperationCapacity) {
    return T70DpmaifRingNoSpace;
  }
  memcpy(Operations, Plan, Count * sizeof(Plan[0]));
  *OperationCount = Count;
  return T70DpmaifRingSuccess;
}

static int
ValidOperation (
  const T70_DPMAIF_HW_OP *Op,
  int *HasPoll
  )
{
  if (Op == NULL || Op->Reserved != 0U ||
      Op->Region > T70DpmaifHwRegionPdSram ||
      Op->Offset > 0xffcU || (Op->Offset & 3U) != 0U) {
    return 0;
  }

  switch (Op->Kind) {
  case T70DpmaifHwWrite:
    return Op->Mask == UINT32_MAX && Op->PollLimit == 0U;
  case T70DpmaifHwUpdate:
    return Op->Mask != 0U && (Op->Value & ~Op->Mask) == 0U &&
        Op->PollLimit == 0U;
  case T70DpmaifHwPollClear:
    if (Op->Mask == 0U || Op->Value != 0U || Op->PollLimit == 0U ||
        Op->PollLimit > T70_DPMAIF_HW_POLL_LIMIT) {
      return 0;
    }
    *HasPoll = 1;
    return 1;
  case T70DpmaifHwPollMaskedNotEqual:
    if (Op->Mask == 0U || (Op->Value & ~Op->Mask) != 0U ||
        Op->PollLimit == 0U ||
        Op->PollLimit > T70_DPMAIF_HW_POLL_LIMIT) {
      return 0;
    }
    *HasPoll = 1;
    return 1;
  default:
    return 0;
  }
}

T70_DPMAIF_HW_EXEC_STATUS
T70DpmaifApplyQueueInitPlan (
  const T70_DPMAIF_HW_OP *Operations,
  size_t OperationCount,
  const T70_DPMAIF_HW_IO *Io
  )
{
  size_t Index;
  int HasPoll = 0;

  if (Operations == NULL || OperationCount == 0U ||
      OperationCount > T70_DPMAIF_HW_MAX_PLAN_OPS ||
      Io == NULL || Io->Read32 == NULL || Io->Write32 == NULL) {
    return T70DpmaifHwExecInvalid;
  }
  for (Index = 0U; Index < OperationCount; ++Index) {
    if (!ValidOperation(&Operations[Index], &HasPoll)) {
      return T70DpmaifHwExecInvalid;
    }
  }
  if (HasPoll && Io->PollStep == NULL) {
    return T70DpmaifHwExecInvalid;
  }

  for (Index = 0U; Index < OperationCount; ++Index) {
    const T70_DPMAIF_HW_OP *Op = &Operations[Index];
    uint32_t Value;

    if (Op->Kind == T70DpmaifHwWrite) {
      if (!Io->Write32(Io->Context, Op->Region, Op->Offset, Op->Value)) {
        return T70DpmaifHwExecIoError;
      }
    } else if (Op->Kind == T70DpmaifHwUpdate) {
      if (!Io->Read32(Io->Context, Op->Region, Op->Offset, &Value) ||
          !Io->Write32(Io->Context, Op->Region, Op->Offset,
              (Value & ~Op->Mask) | (Op->Value & Op->Mask))) {
        return T70DpmaifHwExecIoError;
      }
    } else {
      uint32_t Attempt;
      for (Attempt = 0U; Attempt < Op->PollLimit; ++Attempt) {
        int Satisfied;
        if (!Io->Read32(Io->Context, Op->Region, Op->Offset, &Value)) {
          return T70DpmaifHwExecIoError;
        }
        if (Op->Kind == T70DpmaifHwPollClear) {
          Satisfied = ((Value & Op->Mask) == 0U);
        } else {
          Satisfied = ((Value & Op->Mask) != (Op->Value & Op->Mask));
        }
        if (Satisfied) {
          break;
        }
        if (Attempt + 1U == Op->PollLimit) {
          return T70DpmaifHwExecTimeout;
        }
        if (!Io->PollStep(Io->Context)) {
          return T70DpmaifHwExecCancelled;
        }
      }
    }
  }
  return T70DpmaifHwExecSuccess;
}

T70_DPMAIF_RING_STATUS
T70DpmaifBuildStartupPlan (
  const T70_DPMAIF_MODEM_MEMORY *Memory,
  const T70_DPMAIF_QUEUE_CONFIG *QueueConfig,
  T70_DPMAIF_HW_OP *Operations,
  size_t OperationCapacity,
  size_t *OperationCount
  )
{
  T70_DPMAIF_HW_OP Plan[T70_DPMAIF_HW_MAX_PLAN_OPS];
  size_t Count = 0U;
  size_t StageCount = 0U;
  T70_DPMAIF_RING_STATUS Status;

  if (Memory == NULL || QueueConfig == NULL || Operations == NULL ||
      OperationCount == NULL) {
    return T70DpmaifRingInvalid;
  }

  Status = T70DpmaifBuildCommonInitPlan(Memory, Plan + Count,
      T70_DPMAIF_HW_MAX_PLAN_OPS - Count, &StageCount);
  if (Status != T70DpmaifRingSuccess) return Status;
  Count += StageCount;

  Status = T70DpmaifBuildInterruptInitPlan(Plan + Count,
      T70_DPMAIF_HW_MAX_PLAN_OPS - Count, &StageCount);
  if (Status != T70DpmaifRingSuccess) return Status;
  Count += StageCount;

  Status = T70DpmaifBuildQueueInitPlan(QueueConfig, Plan + Count,
      T70_DPMAIF_HW_MAX_PLAN_OPS - Count, &StageCount);
  if (Status != T70DpmaifRingSuccess) return Status;
  Count += StageCount;

  Status = T70DpmaifBuildBatStartPlan(QueueConfig,Plan+Count,
      T70_DPMAIF_HW_MAX_PLAN_OPS-Count,&StageCount);
  if (Status != T70DpmaifRingSuccess) return Status;
  Count += StageCount;

  Status = T70DpmaifBuildInitDonePlan(Plan + Count,
      T70_DPMAIF_HW_MAX_PLAN_OPS - Count, &StageCount);
  if (Status != T70DpmaifRingSuccess) return Status;
  Count += StageCount;

  if (Count > OperationCapacity) {
    return T70DpmaifRingNoSpace;
  }
  memcpy(Operations, Plan, Count * sizeof(Plan[0]));
  *OperationCount = Count;
  return T70DpmaifRingSuccess;
}
