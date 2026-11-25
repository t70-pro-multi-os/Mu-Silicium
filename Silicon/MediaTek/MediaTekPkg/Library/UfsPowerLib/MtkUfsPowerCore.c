/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stddef.h>
#include <Library/MtkUfsPowerCore.h>

#define CORE_OPERATION_NONE  UINT32_MAX
#define POWER_HCI_LEN        0x2300U
#define POWER_INFRA_LEN      0x1000U
#define POWER_TOP_LEN        0x1000U
#define POWER_PWRAP_LEN      0x1000U

#define REG_CAP              0x000U
#define REG_HCS              0x030U
#define REG_HCE              0x034U
#define REG_IE               0x024U
#define REG_IS               0x020U
#define REG_UTRLRSR          0x060U
#define REG_UTMRRSR          0x080U
#define REG_UTRLDBR          0x058U
#define REG_UTMRDBR          0x078U
#define REG_UTRLBA           0x050U
#define REG_UTRLBAU          0x054U
#define REG_UTMRLBA          0x070U
#define REG_UTMRLBAU         0x074U
#define REG_AHIT             0x018U
#define REG_XOUFS            0x140U
#define REG_REFCLK           0x144U
#define REG_OPT              0x160U
#define REG_IP_VERSION       0x2240U
#define REG_DEBUG_SEL        0x22C0U
#define REG_UFS_PROBE        0x22C8U
#define REG_DEBUG_SEL_B0     0x22D0U
#define REG_DEBUG_SEL_B1     0x22D4U
#define REG_DEBUG_SEL_B2     0x22D8U
#define REG_DEBUG_SEL_B3     0x22DCU
#define REG_UIC_ARG1         0x094U
#define REG_UIC_ARG2         0x098U
#define REG_UIC_ARG3         0x09CU
#define REG_UIC_CMD          0x090U

#define REG_INFRA_SET        0x0A4U
#define REG_INFRA_CLR        0x0A8U
#define REG_INFRA_STATUS     0x0ACU
#define REG_RESET_HCI_SET    0x130U
#define REG_RESET_HCI_CLR    0x134U
#define REG_RESET_UNIPRO_SET 0x140U
#define REG_RESET_UNIPRO_CLR 0x144U
#define REG_RESET_CRYPTO_SET 0x150U
#define REG_RESET_CRYPTO_CLR 0x154U

#define REG_PWRAP_WACS2_CMD       0x880U
#define REG_PWRAP_INIT_DONE2      0x000U
#define REG_PWRAP_WACS2_VLDCLR    0x8A4U
#define REG_PWRAP_WACS2_RDATA     0x8A8U
#define REG_PWRAP_SWINF2_RDATA31  0x894U
#define PMIC_VEMC_CON0            0x1B1CU
#define PMIC_VEMC_ANA             0x1E38U

#define UFS_HCS_UCRDY         (1U << 3)
#define UFS_HCS_LINK_READY    0xFU
#define UFS_HCE_ENABLE         1U
#define UFS_IS_UCCS           (1U << 10)
#define UFS_ERROR_INTERRUPTS  0x00031886U
#define UIC_DME_GET           0x01U
#define UIC_DME_SET           0x02U
#define UIC_LINK_STARTUP      0x16U
#define UIC_HIBERNATE_ENTER   0x17U
#define DME_SAVEPOWER         0xD0A6U
#define DME_DEBUGCLOCK        0xD0A1U
#define DME_POWERDOWN         0xD0A8U
#define DME_LINK_TUNING       0xD09EU
#define DME_PCS_CPORT         0x155EU

#define SMC_RESET             2U
#define SMC_REFCLK            8U
#define SMC_FID               UINT64_C(0xC2000276)
#define UFS_CONTROLLER_ID     0U
#define PMIC_TIMEOUT_US       10000U
#define UIC_TIMEOUT_US        1000000U
#define REFCLK_TIMEOUT_US     3000U
#define H8_TIMEOUT_US         15000U
#define HCE_TIMEOUT_US        1000000U
/* Stock board policy selects 1/30/100/32 us for the gating wait by
 * manufacturer (default 32 us); use the observed conservative 100 us maximum
 * when the runtime ABI has no trusted manufacturer descriptor. T70 ungating
 * uses the configured 32 us delay. */
#define REFCLK_GATE_WAIT_US   100U
#define REFCLK_UNGATE_WAIT_US 32U
#define H8_POLL_STEP_US       100U
#define HCE_SETTLE_US         600U
#define RESET_SETTLE_US       100U
#define DEVICE_RESET_US       10000U
#define MAX_POLL_COUNT        4096U
#define UFS_LINK_HIBERN8      3U
#define TOP_UFS_SEL           0x0D0U
#define TOP_UFS_SEL_GATE       (1U << 31)
#define TOP_UFS_SEL_SHIFT      24U
#define TOP_UFS_SEL_MASK       (7U << TOP_UFS_SEL_SHIFT)

#define GATE_MASK ( (1U << 28) | (1U << 13) | (1U << 12) | (1U << 11) | (1U << 29) )

static const uint32_t mRootOffsets[MTK_UFS_POWER_ROOT_COUNT] = {
  REG_HCE, REG_UTRLRSR, REG_UTMRRSR, REG_UTRLDBR, REG_UTMRDBR,
  REG_UTRLBA, REG_UTRLBAU, REG_UTMRLBA, REG_UTMRLBAU, REG_IE
};

static const uint32_t mGateBits[MTK_UFS_POWER_GATE_COUNT] = {
  1U << 28, 1U << 13, 1U << 12, 1U << 11, 1U << 29
};

static const uint32_t mResetSet[3] = {
  REG_RESET_HCI_SET, REG_RESET_UNIPRO_SET, REG_RESET_CRYPTO_SET
};

static const uint32_t mResetClear[3] = {
  REG_RESET_HCI_CLR, REG_RESET_UNIPRO_CLR, REG_RESET_CRYPTO_CLR
};

static const uint32_t mResetBits[3] = {
  1U << 15, 1U << 7, 1U << 21
};

static int ValidCore(const MTK_UFS_POWER_CORE *Core)
{
  return Core != NULL && Core->Signature == MTK_UFS_POWER_CORE_SIGNATURE;
}

static MTK_UFS_POWER_RESULT Finish(
  MTK_UFS_POWER_CORE *Core,
  MTK_UFS_POWER_REQUEST *Request,
  MTK_UFS_POWER_RESULT Result
  )
{
  Request->State = Core != NULL && ValidCore(Core) ? Core->State : MtkUfsPowerFault;
  Request->Generation = Core != NULL && ValidCore(Core) ? Core->Generation : 0;
  Request->Result = (uint32_t)Result;
  Request->Capabilities = MTK_UFS_POWER_CAP_RETAINED_VEMC |
                          MTK_UFS_POWER_CAP_LEAF_GATES |
                          MTK_UFS_POWER_CAP_PROGRESS;
  return Result;
}

static MTK_UFS_POWER_RESULT Read32(
  const MTK_UFS_POWER_CORE_IO *Io,
  MTK_UFS_POWER_BANK Bank,
  uint32_t Offset,
  uint32_t *Value
  )
{
  MTK_UFS_POWER_IO_STATUS Status;
  uint32_t Length;
  if (Value == NULL || (Offset & 3U) != 0) return MtkUfsPowerInvalid;
  switch (Bank) {
    case MtkUfsPowerBankHci:    Length = POWER_HCI_LEN; break;
    case MtkUfsPowerBankInfra:  Length = POWER_INFRA_LEN; break;
    case MtkUfsPowerBankTop:    Length = POWER_TOP_LEN; break;
    case MtkUfsPowerBankPwrap:  Length = POWER_PWRAP_LEN; break;
    default: return MtkUfsPowerInvalid;
  }
  if (Offset > Length - sizeof(uint32_t)) return MtkUfsPowerInvalid;
  Status = Io->Read32(Io->Context, Bank, Offset, Value);
  if (Status == MtkUfsPowerIoBusy) return MtkUfsPowerBusy;
  return Status == MtkUfsPowerIoOk ? MtkUfsPowerSuccess : MtkUfsPowerIoError;
}

static MTK_UFS_POWER_RESULT Write32(
  const MTK_UFS_POWER_CORE_IO *Io,
  MTK_UFS_POWER_BANK Bank,
  uint32_t Offset,
  uint32_t Value
  )
{
  MTK_UFS_POWER_IO_STATUS Status;
  uint32_t Length;
  if ((Offset & 3U) != 0) return MtkUfsPowerInvalid;
  switch (Bank) {
    case MtkUfsPowerBankHci:    Length = POWER_HCI_LEN; break;
    case MtkUfsPowerBankInfra:  Length = POWER_INFRA_LEN; break;
    case MtkUfsPowerBankTop:    Length = POWER_TOP_LEN; break;
    case MtkUfsPowerBankPwrap:  Length = POWER_PWRAP_LEN; break;
    default: return MtkUfsPowerInvalid;
  }
  if (Offset > Length - sizeof(uint32_t)) return MtkUfsPowerInvalid;
  Status = Io->Write32(Io->Context, Bank, Offset, Value);
  if (Status == MtkUfsPowerIoBusy) return MtkUfsPowerBusy;
  return Status == MtkUfsPowerIoOk ? MtkUfsPowerSuccess : MtkUfsPowerIoError;
}

static uint64_t ElapsedUs(uint64_t Start, uint64_t Now)
{
  return Now >= Start ? Now - Start : UINT64_MAX;
}

/* Every wait is advanced by bounded calls. A stopped or broken counter must
 * not turn a runtime PRM request into an unbounded poll. */
static MTK_UFS_POWER_RESULT WaitBudget(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io,
  uint64_t TimeoutUs
  )
{
  if (Core->PhasePollCount <= MAX_POLL_COUNT) Core->PhasePollCount++;
  if (Core->PhasePollCount > MAX_POLL_COUNT ||
      ElapsedUs(Core->PhaseStartedUs, Io->NowUs(Io->Context)) > TimeoutUs) {
    return MtkUfsPowerTimeout;
  }
  return MtkUfsPowerInProgress;
}

static void StartWait(MTK_UFS_POWER_CORE *Core, const MTK_UFS_POWER_CORE_IO *Io)
{
  Core->PhaseStartedUs = Io->NowUs(Io->Context);
  Core->PhasePollCount = 0;
}

static void LatchFault(MTK_UFS_POWER_CORE *Core, MTK_UFS_POWER_RESULT Result)
{
  Core->State = MtkUfsPowerFault;
  Core->Phase = MtkUfsPowerPhaseFault;
  Core->Operation = CORE_OPERATION_NONE;
  Core->LastError = (uint32_t)Result;
}

/* WACS2 is shared. Start only from idle, never clear an unowned response, and
 * retain every accepted read until its response has been consumed. */
static MTK_UFS_POWER_RESULT PmicReadStep(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io,
  uint16_t Address,
  uint32_t *Value
  )
{
  MTK_UFS_POWER_RESULT Result;
  uint32_t Raw, Fsm;
  uint64_t Now;
  if (Value == NULL || (Address & 1U) != 0) return MtkUfsPowerInvalid;

  if (Core->WacsStage != 0U && Core->WacsAddress != Address) {
    uint32_t Discard;
    Result = PmicReadStep(Core, Io, (uint16_t)Core->WacsAddress, &Discard);
    return Result == MtkUfsPowerSuccess ? MtkUfsPowerInProgress : Result;
  }

  if (Core->WacsStage == 0) {
    Result = Read32(Io, MtkUfsPowerBankPwrap, REG_PWRAP_INIT_DONE2, &Raw);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Raw != 1U) return MtkUfsPowerIoError;
    Result = Read32(Io, MtkUfsPowerBankPwrap, REG_PWRAP_WACS2_RDATA, &Raw);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Raw == UINT32_MAX) return MtkUfsPowerIoError;
    Fsm = (Raw >> 1) & 7U;
    if (Fsm != 0U) return MtkUfsPowerBusy;
    Core->WacsAddress = Address;
    Core->WacsPollCount = 0;
    Core->WacsStartedUs = Io->NowUs(Io->Context);
    Core->WacsStage = 1;
    Result = Write32(Io, MtkUfsPowerBankPwrap, REG_PWRAP_WACS2_CMD, Address);
    if (Result != MtkUfsPowerSuccess) return Result;
    return MtkUfsPowerInProgress;
  }

  if (Core->WacsAddress != Address) return MtkUfsPowerBusy;
  Result = Read32(Io, MtkUfsPowerBankPwrap, REG_PWRAP_WACS2_RDATA, &Raw);
  if (Result != MtkUfsPowerSuccess) return Result;
  if (Raw == UINT32_MAX) return MtkUfsPowerIoError;
  Fsm = (Raw >> 1) & 7U;
  Now = Io->NowUs(Io->Context);
  if (Core->WacsPollCount <= MAX_POLL_COUNT) Core->WacsPollCount++;
  if (Core->WacsPollCount > MAX_POLL_COUNT || ElapsedUs(Core->WacsStartedUs, Now) > PMIC_TIMEOUT_US) {
    return MtkUfsPowerTimeout;
  }

  if (Core->WacsStage == 1) {
    if (Fsm == 0U) return MtkUfsPowerIoError;
    if (Fsm != 6U) return MtkUfsPowerInProgress;
    Result = Read32(Io, MtkUfsPowerBankPwrap, REG_PWRAP_SWINF2_RDATA31, &Core->WacsValue);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->WacsValue &= 0xFFFFU;
    Core->WacsStage = 2;
    return MtkUfsPowerInProgress;
  }

  if (Core->WacsStage == 2) {
    /* A failed clear is uncertain: do not issue it again or reissue the PMIC read. */
    Core->WacsStage = 4;
    Result = Write32(Io, MtkUfsPowerBankPwrap, REG_PWRAP_WACS2_VLDCLR, 1U);
    if (Result != MtkUfsPowerSuccess) return MtkUfsPowerIoError;
    Core->WacsStage = 3;
    return MtkUfsPowerInProgress;
  }

  if (Core->WacsStage == 3 || Core->WacsStage == 4) {
    if (Fsm != 0U) {
      if (Core->WacsStage == 4 &&
          (Core->WacsPollCount > MAX_POLL_COUNT || ElapsedUs(Core->WacsStartedUs, Now) > PMIC_TIMEOUT_US)) {
        return MtkUfsPowerTimeout;
      }
      return MtkUfsPowerInProgress;
    }
    *Value = Core->WacsValue;
    Core->WacsStage = 0;
    Core->WacsAddress = 0;
    Core->WacsPollCount = 0;
    return MtkUfsPowerSuccess;
  }
  return MtkUfsPowerIoError;
}

static MTK_UFS_POWER_RESULT ReadVemcEnable(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io,
  int *Enabled
  )
{
  uint32_t Value;
  MTK_UFS_POWER_RESULT Result = PmicReadStep(Core, Io, PMIC_VEMC_CON0, &Value);
  if (Result == MtkUfsPowerSuccess) *Enabled = (Value & 1U) != 0;
  return Result;
}

static MTK_UFS_POWER_RESULT ReadVemcVoltage(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io,
  int *Valid
  )
{
  uint32_t Value, Selector;
  MTK_UFS_POWER_RESULT Result = PmicReadStep(Core, Io, PMIC_VEMC_ANA, &Value);
  if (Result != MtkUfsPowerSuccess) return Result;
  Selector = (Value >> 8) & 7U;
  *Valid = Selector == 2U || Selector == 3U || Selector == 5U;
  return MtkUfsPowerSuccess;
}

static MTK_UFS_POWER_RESULT FreshVemcStep(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io,
  uint32_t *Step
  )
{
  MTK_UFS_POWER_RESULT Result;
  int Good;
  if (*Step == 0U) {
    Result = ReadVemcEnable(Core, Io, &Good);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (!Good) return MtkUfsPowerWrongState;
    *Step = 1U;
    return MtkUfsPowerInProgress;
  }
  if (*Step == 1U) {
    Result = ReadVemcVoltage(Core, Io, &Good);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (!Good) return MtkUfsPowerWrongState;
    *Step = 2U;
    return MtkUfsPowerSuccess;
  }
  return MtkUfsPowerSuccess;
}

static MTK_UFS_POWER_RESULT GateStatus(
  const MTK_UFS_POWER_CORE_IO *Io,
  uint32_t *EnabledMask
  )
{
  uint32_t Value;
  MTK_UFS_POWER_RESULT Result = Read32(Io, MtkUfsPowerBankInfra, REG_INFRA_STATUS, &Value);
  if (Result != MtkUfsPowerSuccess) return Result;
  if (Value == UINT32_MAX) return MtkUfsPowerIoError;
  *EnabledMask = (~Value) & GATE_MASK;
  return MtkUfsPowerSuccess;
}

/* TOP_UFS is a shared parent. Verify its selected route and gate without
 * changing either; this does not claim the selected PLL itself is measured. */
static MTK_UFS_POWER_RESULT CheckTopUfsParent(const MTK_UFS_POWER_CORE_IO *Io)
{
  uint32_t Value;
  MTK_UFS_POWER_RESULT Result = Read32(Io, MtkUfsPowerBankTop, TOP_UFS_SEL, &Value);
  if (Result != MtkUfsPowerSuccess) return Result;
  if (Value == UINT32_MAX) return MtkUfsPowerIoError;
  if ((Value & TOP_UFS_SEL_GATE) != 0U ||
      ((Value & TOP_UFS_SEL_MASK) >> TOP_UFS_SEL_SHIFT) >= 7U) {
    return MtkUfsPowerWrongState;
  }
  return MtkUfsPowerSuccess;
}

static MTK_UFS_POWER_RESULT NormalizeIpVersion(uint32_t Raw, uint32_t *Normalized)
{
  if (Normalized == NULL || Raw == 0U || Raw == UINT32_MAX) return MtkUfsPowerIoError;
  *Normalized = ((Raw | 0x01000000U) >> 24) == 1U
                  ? (Raw & 0x00FFFFFFU) | 0x10000000U : Raw;
  return *Normalized == 0x10480000U ? MtkUfsPowerUnsupported : MtkUfsPowerSuccess;
}

static MTK_UFS_POWER_RESULT CheckActiveHardware(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io,
  uint32_t Index,
  int *Complete
  )
{
  static const uint32_t Offsets[] = {
    REG_IP_VERSION, REG_HCE, REG_HCS, REG_UTRLDBR, REG_UTMRDBR, REG_REFCLK
  };
  uint32_t Value, Gates;
  MTK_UFS_POWER_RESULT Result;
  *Complete = 0;
  if (Index == 0U) {
    Result = CheckTopUfsParent(Io);
    if (Result != MtkUfsPowerSuccess) return Result;
    return MtkUfsPowerInProgress;
  }
  if (Index == 1U) {
    Result = GateStatus(Io, &Gates);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->GateObservedMask = Gates;
    return (Gates & GATE_MASK) == GATE_MASK ? MtkUfsPowerInProgress : MtkUfsPowerWrongState;
  }
  if (Index < 2U + (sizeof(Offsets) / sizeof(Offsets[0]))) {
    Result = Read32(Io, MtkUfsPowerBankHci, Offsets[Index - 2U], &Value);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Index == 2U) {
      Result = NormalizeIpVersion(Value, &Core->IpVersion);
      if (Result != MtkUfsPowerSuccess) return Result;
      return MtkUfsPowerInProgress;
    }
    if (Value == UINT32_MAX) return MtkUfsPowerIoError;
    switch (Index - 2U) {
      case 1: if (Value != UFS_HCE_ENABLE) return MtkUfsPowerWrongState; break;
      case 2: if ((Value & UFS_HCS_LINK_READY) != UFS_HCS_LINK_READY) return MtkUfsPowerWrongState; break;
      case 3: case 4: if (Value != 0U) return MtkUfsPowerBusy; break;
      case 5: if ((Value & 3U) != 3U) return MtkUfsPowerWrongState; break;
    }
    return MtkUfsPowerInProgress;
  }
  *Complete = 1;
  return MtkUfsPowerSuccess;
}

static MTK_UFS_POWER_RESULT CheckStoppedRoots(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io,
  uint32_t *Index,
  int *Complete
  )
{
  uint32_t Value;
  MTK_UFS_POWER_RESULT Result;
  *Complete = 0;
  if (*Index == 0U) {
    Result = CheckTopUfsParent(Io);
    if (Result != MtkUfsPowerSuccess) return Result;
    *Index = 1U;
    return MtkUfsPowerInProgress;
  }
  if (*Index == 1U) {
    uint32_t Gates;
    Result = GateStatus(Io, &Gates);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->GateObservedMask = Gates;
    /* HCI is inaccessible unless every exclusive leaf gate is present. */
    if ((Gates & GATE_MASK) != GATE_MASK) return MtkUfsPowerBusy;
    *Index = 2U;
    return MtkUfsPowerInProgress;
  }
  if (*Index >= MTK_UFS_POWER_ROOT_COUNT + 2U) {
    *Complete = 1;
    return MtkUfsPowerSuccess;
  }
  Result = Read32(Io, MtkUfsPowerBankHci, mRootOffsets[*Index - 2U], &Value);
  if (Result != MtkUfsPowerSuccess) return Result;
  if (Value == UINT32_MAX) return MtkUfsPowerIoError;
  if (Value != 0U) return MtkUfsPowerBusy;
  (*Index)++;
  if (*Index >= MTK_UFS_POWER_ROOT_COUNT + 2U) *Complete = 1;
  return MtkUfsPowerInProgress;
}

static MTK_UFS_POWER_RESULT CheckReleasedHardware(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io,
  uint32_t *Index,
  int *Complete
  )
{
  uint32_t Gates;
  MTK_UFS_POWER_RESULT Result;
  *Complete = 0;
  if (*Index == 0U) {
    Result = CheckTopUfsParent(Io);
    if (Result != MtkUfsPowerSuccess) return Result;
    (*Index)++;
    return MtkUfsPowerInProgress;
  }
  Result = GateStatus(Io, &Gates);
  if (Result != MtkUfsPowerSuccess) return Result;
  Core->GateObservedMask = Gates;
  if ((Gates & Core->GateOwnedMask) != 0U) return MtkUfsPowerBusy;
  *Complete = 1;
  return MtkUfsPowerSuccess;
}

/* StopPrepared has two valid read-only observations: the consumer may not
 * have stopped HCE yet, or it may already have proved all ten roots zero. */
static MTK_UFS_POWER_RESULT CheckPreparedHardware(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io,
  int *Complete
  )
{
  static const uint32_t HostLiveOffsets[] = {
    REG_UTRLDBR, REG_UTMRDBR, REG_REFCLK, REG_UFS_PROBE
  };
  uint32_t Value, Gates;
  MTK_UFS_POWER_RESULT Result;
  *Complete = 0;

  if (Core->QueryStopProof == 1U) {
    return CheckStoppedRoots(Core, Io, &Core->RootIndex, Complete);
  }
  if (Core->RootIndex == 0U) {
    Result = CheckTopUfsParent(Io);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->RootIndex++;
    return MtkUfsPowerInProgress;
  }
  if (Core->RootIndex == 1U) {
    Result = GateStatus(Io, &Gates);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->GateObservedMask = Gates;
    if ((Gates & GATE_MASK) != GATE_MASK) return MtkUfsPowerWrongState;
    Core->RootIndex++;
    return MtkUfsPowerInProgress;
  }
  if (Core->RootIndex == 2U) {
    Result = Read32(Io, MtkUfsPowerBankHci, REG_HCE, &Value);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Value == UINT32_MAX) return MtkUfsPowerIoError;
    if (Value == 0U) {
      Core->QueryStopProof = 1U;
      Core->RootIndex = 2U;
      return MtkUfsPowerInProgress;
    }
    if (Value != UFS_HCE_ENABLE) return MtkUfsPowerWrongState;
    Core->QueryStopProof = 2U;
    Core->RootIndex = 3U;
    return MtkUfsPowerInProgress;
  }
  if (Core->QueryStopProof != 2U || Core->RootIndex < 3U || Core->RootIndex > 6U) {
    return MtkUfsPowerIoError;
  }
  Result = Read32(Io, MtkUfsPowerBankHci, HostLiveOffsets[Core->RootIndex - 3U], &Value);
  if (Result != MtkUfsPowerSuccess) return Result;
  if (Value == UINT32_MAX) return MtkUfsPowerIoError;
  switch (Core->RootIndex) {
    case 3U: case 4U:
      if (Value != 0U) return MtkUfsPowerBusy;
      break;
    case 5U:
      if ((Value & 3U) != 3U) return MtkUfsPowerWrongState;
      break;
    case 6U:
      if ((Value >> 28) != UFS_LINK_HIBERN8) return MtkUfsPowerWrongState;
      *Complete = 1;
      return MtkUfsPowerSuccess;
  }
  Core->RootIndex++;
  return MtkUfsPowerInProgress;
}

static void BeginOperation(MTK_UFS_POWER_CORE *Core, uint32_t Operation, uint32_t Phase)
{
  Core->Operation = Operation;
  Core->TransactionGeneration = Core->Generation;
  Core->Phase = Phase;
  Core->RootIndex = 0;
  Core->GateIndex = 0;
  Core->WacsPollCount = 0;
  Core->UicPollCount = 0;
  Core->PhasePollCount = 0;
  Core->LastError = MtkUfsPowerSuccess;
  Core->PhaseStartedUs = 0;
  Core->NextPollUs = 0U;
}

static MTK_UFS_POWER_RESULT CompleteOperation(
  MTK_UFS_POWER_CORE *Core,
  uint32_t NewState
  )
{
  if (Core->Generation == UINT64_MAX) {
    Core->State = MtkUfsPowerFault;
    Core->Phase = MtkUfsPowerPhaseFault;
    Core->Operation = CORE_OPERATION_NONE;
    return MtkUfsPowerIoError;
  }
  Core->State = NewState;
  Core->Generation++;
  Core->Operation = CORE_OPERATION_NONE;
  Core->Phase = MtkUfsPowerPhaseIdle;
  Core->RootIndex = 0;
  Core->GateIndex = 0;
  Core->UicPending = 0;
  Core->UicPollCount = 0;
  Core->PhasePollCount = 0;
  Core->NextPollUs = 0U;
  Core->WacsStage = 0;
  Core->SmcPending = 0;
  return MtkUfsPowerSuccess;
}

static MTK_UFS_POWER_RESULT SmcStep(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io,
  uint32_t Command,
  uint32_t Value,
  uint32_t Stage
  )
{
  MTK_UFS_POWER_IO_STATUS Status;
  int64_t Raw = 0;
  if (Core->SmcPending != 0U) {
    if (Core->SmcCommand != Command || Core->SmcValue != Value || Core->SmcStage != Stage) {
      return MtkUfsPowerBusy;
    }
    return MtkUfsPowerTimeout;
  }
  Core->SmcCommand = Command;
  Core->SmcValue = Value;
  Core->SmcStage = Stage;
  Core->SmcPending = 1U;
  Status = Io->Smc(Io->Context, Command, Value, Stage, &Raw);
  if (Status != MtkUfsPowerIoOk) return MtkUfsPowerIoError;
  Core->SmcPending = 0U;
  if (Raw == 0 || (Command == SMC_REFCLK && Raw == -1)) return MtkUfsPowerSuccess;
  Core->SmcPending = 2U;
  Core->LastError = (uint32_t)(Raw & 0xFFFFFFFFU);
  return MtkUfsPowerIoError;
}

/* Retain one UIC intent from its bounded readiness wait through UCCS
 * completion. Completion polling never submits an accepted command twice. */
static MTK_UFS_POWER_RESULT UicStep(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io,
  uint32_t Opcode,
  uint32_t Arg1,
  uint32_t Arg3,
  uint32_t *ReturnedArg3
  )
{
  MTK_UFS_POWER_RESULT Result;
  uint32_t Value, Is, Arg2;
  uint64_t Now;
  if (ReturnedArg3 == NULL) return MtkUfsPowerInvalid;

  if (Core->UicPending == 0U) {
    Core->UicCommand = Opcode;
    Core->UicArg1 = Arg1;
    Core->UicArg2 = 0;
    Core->UicInputArg3 = Arg3;
    Core->UicStartedUs = Io->NowUs(Io->Context);
    Core->UicPollCount = 0;
    /* Keep the submitted intent while waiting for HCS.UCRDY. This also
     * prevents a retry from silently replacing the command being prepared. */
    Core->UicPending = 4U;
  }

  if (Core->UicPending == 4U) {
    if (Core->UicCommand != Opcode || Core->UicArg1 != Arg1 || Core->UicInputArg3 != Arg3) {
      return MtkUfsPowerBusy;
    }
    Result = Read32(Io, MtkUfsPowerBankHci, REG_HCS, &Value);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Value == UINT32_MAX) return MtkUfsPowerIoError;
    Result = Read32(Io, MtkUfsPowerBankHci, REG_IS, &Is);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Is == UINT32_MAX || (Is & UFS_ERROR_INTERRUPTS) != 0U) return MtkUfsPowerIoError;
    Now = Io->NowUs(Io->Context);
    if (Core->UicPollCount <= MAX_POLL_COUNT) Core->UicPollCount++;
    if (Core->UicPollCount > MAX_POLL_COUNT || ElapsedUs(Core->UicStartedUs, Now) > UIC_TIMEOUT_US) {
      return MtkUfsPowerTimeout;
    }
    /* UCCS predates this request, so it is not ours to acknowledge. Keep the
     * preparation journal and wait for the owning path to clear the status. */
    if ((Is & UFS_IS_UCCS) != 0U) return MtkUfsPowerBusy;
    if ((Value & UFS_HCS_UCRDY) == 0U) return MtkUfsPowerInProgress;

    Result = Write32(Io, MtkUfsPowerBankHci, REG_UIC_ARG1, Arg1);
    if (Result == MtkUfsPowerSuccess) Result = Write32(Io, MtkUfsPowerBankHci, REG_UIC_ARG2, 0U);
    if (Result == MtkUfsPowerSuccess) Result = Write32(Io, MtkUfsPowerBankHci, REG_UIC_ARG3, Arg3);
    if (Result != MtkUfsPowerSuccess) {
      Core->UicPending = 3U;
      return MtkUfsPowerIoError;
    }
    /* Set before the command write: an error can mean the command reached HCI. */
    Core->UicPending = 1U;
    Result = Write32(Io, MtkUfsPowerBankHci, REG_UIC_CMD, Opcode);
    if (Result != MtkUfsPowerSuccess) return MtkUfsPowerIoError;
    return MtkUfsPowerInProgress;
  }

  if (Core->UicCommand != Opcode || Core->UicArg1 != Arg1 || Core->UicInputArg3 != Arg3) {
    return MtkUfsPowerBusy;
  }
  if (Core->UicPending == 3U) return MtkUfsPowerIoError;
  Result = Read32(Io, MtkUfsPowerBankHci, REG_IS, &Is);
  if (Result != MtkUfsPowerSuccess) return Result;
  if (Is == UINT32_MAX || (Is & UFS_ERROR_INTERRUPTS) != 0U) return MtkUfsPowerIoError;
  Now = Io->NowUs(Io->Context);
  if (ElapsedUs(Core->UicStartedUs, Now) > UIC_TIMEOUT_US) return MtkUfsPowerTimeout;
  if (Core->UicPollCount <= MAX_POLL_COUNT) Core->UicPollCount++;
  if (Core->UicPollCount > MAX_POLL_COUNT) return MtkUfsPowerTimeout;
  if (Core->UicPending == 1U) {
    if ((Is & UFS_IS_UCCS) == 0U) return MtkUfsPowerInProgress;
    Result = Read32(Io, MtkUfsPowerBankHci, REG_UIC_ARG2, &Arg2);
    if (Result != MtkUfsPowerSuccess) return Result;
    Result = Read32(Io, MtkUfsPowerBankHci, REG_UIC_ARG3, &Value);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->UicArg2 = Arg2;
    Core->UicResultArg3 = Value;
    Core->UicPending = 2U;
    if ((Arg2 & 0xFFU) != 0U) Core->LastError = Arg2;
    Result = Write32(Io, MtkUfsPowerBankHci, REG_IS, UFS_IS_UCCS);
    if (Result != MtkUfsPowerSuccess) return MtkUfsPowerIoError;
  } else if (Core->UicPending == 2U) {
    if ((Is & UFS_IS_UCCS) != 0U) return MtkUfsPowerInProgress;
  } else {
    return MtkUfsPowerIoError;
  }
  if ((Core->UicArg2 & 0xFFU) != 0U) {
    Core->UicPending = 3U;
    return MtkUfsPowerIoError;
  }
  *ReturnedArg3 = Core->UicResultArg3;
  Core->UicPending = 0U;
  return MtkUfsPowerSuccess;
}

static MTK_UFS_POWER_RESULT CheckAndModify(
  const MTK_UFS_POWER_CORE_IO *Io,
  uint32_t Offset,
  uint32_t ClearMask,
  uint32_t SetMask
  )
{
  uint32_t Value;
  MTK_UFS_POWER_RESULT Result = Read32(Io, MtkUfsPowerBankHci, Offset, &Value);
  if (Result != MtkUfsPowerSuccess) return Result;
  if (Value == UINT32_MAX) return MtkUfsPowerIoError;
  return Write32(Io, MtkUfsPowerBankHci, Offset, (Value & ~ClearMask) | SetMask);
}

void MtkUfsPowerCoreInitialize(MTK_UFS_POWER_CORE *Core)
{
  uint8_t *Bytes;
  size_t Index;
  if (Core == NULL) return;
  Bytes = (uint8_t *)Core;
  for (Index = 0; Index < sizeof(*Core); Index++) Bytes[Index] = 0;
  Core->Signature = MTK_UFS_POWER_CORE_SIGNATURE;
  Core->State = MtkUfsPowerDormant;
  Core->Phase = MtkUfsPowerPhaseIdle;
  Core->Operation = CORE_OPERATION_NONE;
  Core->Generation = 1U;
}

static MTK_UFS_POWER_RESULT StartQuery(MTK_UFS_POWER_CORE *Core)
{
  if (Core->Operation != CORE_OPERATION_NONE) return MtkUfsPowerBusy;
  if (Core->State == MtkUfsPowerFault || Core->State == MtkUfsPowerRestorePreparing ||
      Core->State == MtkUfsPowerStopPreparing) return MtkUfsPowerBusy;
  BeginOperation(Core, MtkUfsPowerQuery, MtkUfsPowerPhaseQueryVemcEnable);
  Core->RootIndex = 0;
  Core->QueryVemcGood = 0;
  Core->QueryStopProof = 0U;
  Core->NextPollUs = 0U;
  Core->WacsStage = 0;
  return MtkUfsPowerInProgress;
}

static MTK_UFS_POWER_RESULT ContinueQuery(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io
  )
{
  MTK_UFS_POWER_RESULT Result;
  int Good, Complete;
  if (Core->Phase == MtkUfsPowerPhaseQueryVemcEnable) {
    Result = ReadVemcEnable(Core, Io, &Good);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (!Good) goto WrongState;
    Core->Phase = MtkUfsPowerPhaseQueryVemcVoltage;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseQueryVemcVoltage) {
    Result = ReadVemcVoltage(Core, Io, &Good);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (!Good) goto WrongState;
    Core->QueryVemcGood = 1U;
    Core->Phase = MtkUfsPowerPhaseQueryHardware;
    Core->RootIndex = 0;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase != MtkUfsPowerPhaseQueryHardware) return MtkUfsPowerIoError;

  if (Core->State == MtkUfsPowerReleased) {
    Result = CheckReleasedHardware(Core, Io, &Core->RootIndex, &Complete);
    if (Result == MtkUfsPowerBusy) {
      Core->RootIndex = 0;
      return Result;
    }
    if (Result != MtkUfsPowerSuccess && Result != MtkUfsPowerInProgress) return Result;
    if (!Complete) return MtkUfsPowerInProgress;
    Core->Operation = CORE_OPERATION_NONE;
    Core->Phase = MtkUfsPowerPhaseIdle;
    return MtkUfsPowerSuccess;
  }

  if (Core->State == MtkUfsPowerStopPrepared) {
    Result = CheckPreparedHardware(Core, Io, &Complete);
    if (Result == MtkUfsPowerBusy || Result == MtkUfsPowerWrongState) {
      Core->RootIndex = 0U;
      Core->QueryStopProof = 0U;
      return Result;
    }
    if (Result != MtkUfsPowerSuccess && Result != MtkUfsPowerInProgress) return Result;
    if (!Complete) return MtkUfsPowerInProgress;
    Core->Operation = CORE_OPERATION_NONE;
    Core->Phase = MtkUfsPowerPhaseIdle;
    Core->RootIndex = 0U;
    Core->QueryStopProof = 0U;
    return MtkUfsPowerSuccess;
  }

  Result = CheckActiveHardware(Core, Io, Core->RootIndex, &Complete);
  if (Result == MtkUfsPowerBusy || Result == MtkUfsPowerWrongState) {
    Core->RootIndex = 0;
    Core->QueryStopProof = 0U;
    return Result;
  }
  if (Result != MtkUfsPowerSuccess && Result != MtkUfsPowerInProgress) return Result;
  if (!Complete) {
    Core->RootIndex++;
    return MtkUfsPowerInProgress;
  }
  if (Core->State == MtkUfsPowerDormant) {
    Core->State = MtkUfsPowerActive;
    /* Query observes boot-enabled gates; ownership transfers only after the
     * PrepareStop proof and accepted device-down acknowledgement. */
    Core->GateOwnedMask = 0U;
  }
  Core->Operation = CORE_OPERATION_NONE;
  Core->Phase = MtkUfsPowerPhaseIdle;
  return MtkUfsPowerSuccess;

WrongState:
  Core->Operation = CORE_OPERATION_NONE;
  Core->Phase = MtkUfsPowerPhaseIdle;
  Core->WacsStage = 0;
  return MtkUfsPowerWrongState;
}

static MTK_UFS_POWER_RESULT BeginMutation(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_REQUEST *Request
  )
{
  if (Core->Operation != CORE_OPERATION_NONE) {
    if (Core->Operation == Request->Operation && Core->TransactionGeneration == Request->Generation) {
      return MtkUfsPowerInProgress;
    }
    /* Restore may safely unwind an interrupted Release while the confirmed
     * device state is still StopPrepared. Reuse an owned WACS response, but
     * never preempt an uncertain secure/UIC request. */
    if (Request->Operation == MtkUfsPowerRestore && Request->Flags == 0U &&
        Core->Operation == MtkUfsPowerRelease && Core->State == MtkUfsPowerStopPrepared &&
        Core->TransactionGeneration == Request->Generation && Request->Generation == Core->Generation &&
        Core->SmcPending == 0U && Core->UicPending == 0U &&
        (Core->Phase == MtkUfsPowerPhaseReleaseRoots ||
         Core->Phase == MtkUfsPowerPhaseReleaseVemc ||
         Core->Phase == MtkUfsPowerPhaseReleaseGateRead ||
         Core->Phase == MtkUfsPowerPhaseReleaseGateWrite ||
         Core->Phase == MtkUfsPowerPhaseReleaseGateVerify)) {
      Core->RestoreOriginState = MtkUfsPowerStopPrepared;
      Core->QueryVemcGood = 0U;
      Core->State = MtkUfsPowerRestorePreparing;
      BeginOperation(Core, Request->Operation, MtkUfsPowerPhaseRestoreVemc);
      return MtkUfsPowerInProgress;
    }
    return MtkUfsPowerBusy;
  }
  if (Request->Generation != Core->Generation) return MtkUfsPowerStaleGeneration;
  switch (Request->Operation) {
    case MtkUfsPowerPrepareStop:
      if (Request->Flags != MTK_UFS_POWER_DEVICE_DOWN_ACKED || Core->State != MtkUfsPowerActive) return MtkUfsPowerWrongState;
      Core->State = MtkUfsPowerStopPreparing;
      BeginOperation(Core, Request->Operation, MtkUfsPowerPhasePrepareCheck);
      return MtkUfsPowerInProgress;
    case MtkUfsPowerRelease:
      if (Request->Flags != 0U || Core->State != MtkUfsPowerStopPrepared) return MtkUfsPowerWrongState;
      BeginOperation(Core, Request->Operation, MtkUfsPowerPhaseReleaseRoots);
      return MtkUfsPowerInProgress;
    case MtkUfsPowerRestore:
      if (Request->Flags != 0U || (Core->State != MtkUfsPowerReleased && Core->State != MtkUfsPowerStopPrepared)) return MtkUfsPowerWrongState;
      Core->RestoreOriginState = Core->State;
      Core->QueryVemcGood = 0U;
      Core->State = MtkUfsPowerRestorePreparing;
      if (Core->RestoreOriginState == MtkUfsPowerReleased) {
        BeginOperation(Core, Request->Operation, MtkUfsPowerPhaseRestoreVemc);
      } else {
        /* Prove the retained device supply before even reading HCI state. */
        BeginOperation(Core, Request->Operation, MtkUfsPowerPhaseRestoreVemc);
      }
      return MtkUfsPowerInProgress;
    default:
      return MtkUfsPowerInvalid;
  }
}

static MTK_UFS_POWER_RESULT PrepareCheckStep(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io
  )
{
  MTK_UFS_POWER_RESULT Result;
  uint32_t Value, Gates;
  int Good;
  switch (Core->RootIndex) {
    case 0:
      Result = ReadVemcEnable(Core, Io, &Good);
      if (Result != MtkUfsPowerSuccess) return Result;
      if (!Good) return MtkUfsPowerWrongState;
      break;
    case 1:
      Result = ReadVemcVoltage(Core, Io, &Good);
      if (Result != MtkUfsPowerSuccess) return Result;
      if (!Good) return MtkUfsPowerWrongState;
      break;
    case 2:
      Result = CheckTopUfsParent(Io);
      if (Result != MtkUfsPowerSuccess) return Result;
      break;
    case 3:
      Result = GateStatus(Io, &Gates);
      if (Result != MtkUfsPowerSuccess) return Result;
      if ((Gates & GATE_MASK) != GATE_MASK) return MtkUfsPowerWrongState;
      Core->GateObservedMask = Gates;
      break;
    case 4: case 5: case 6: case 7: case 8:
    {
      static const uint32_t Offsets[] = { REG_HCE, REG_HCS, REG_UTRLDBR, REG_UTMRDBR, REG_REFCLK };
      Result = Read32(Io, MtkUfsPowerBankHci, Offsets[Core->RootIndex - 4U], &Value);
      if (Result != MtkUfsPowerSuccess) return Result;
      if (Value == UINT32_MAX) return MtkUfsPowerIoError;
      if (Core->RootIndex == 4U && Value != 1U) return MtkUfsPowerWrongState;
      if (Core->RootIndex == 5U && (Value & 0xFU) != 0xFU) return MtkUfsPowerWrongState;
      if ((Core->RootIndex == 6U || Core->RootIndex == 7U) && Value != 0U) return MtkUfsPowerBusy;
      if (Core->RootIndex == 8U && (Value & 3U) != 3U) return MtkUfsPowerWrongState;
      break;
    }
    case 9:
      Result = Read32(Io, MtkUfsPowerBankHci, REG_IP_VERSION, &Value);
      if (Result != MtkUfsPowerSuccess) return Result;
      Result = NormalizeIpVersion(Value, &Core->IpVersion);
      if (Result != MtkUfsPowerSuccess) return Result;
      break;
    default:
      Core->GateOwnedMask = GATE_MASK;
      Core->Phase = MtkUfsPowerPhasePrepareHibernate;
      Core->RootIndex = 0;
      return MtkUfsPowerInProgress;
  }
  Core->RootIndex++;
  return MtkUfsPowerInProgress;
}

static MTK_UFS_POWER_RESULT ContinuePrepare(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io
  )
{
  MTK_UFS_POWER_RESULT Result;
  uint32_t Returned;
  if (Core->Phase == MtkUfsPowerPhasePrepareCheck) {
    return PrepareCheckStep(Core, Io);
  }
  if (Core->Phase == MtkUfsPowerPhasePrepareHibernate) {
    Result = UicStep(Core, Io, UIC_HIBERNATE_ENTER, 0U, 0U, &Returned);
    if (Result == MtkUfsPowerSuccess) {
      Core->Phase = MtkUfsPowerPhasePrepareHibernateProbe;
      Core->RootIndex = 0U;
      Core->UicPending = 0;
      Core->NextPollUs = 0U;
      StartWait(Core, Io);
      return MtkUfsPowerInProgress;
    }
    return Result;
  }
  if (Core->Phase == MtkUfsPowerPhasePrepareHibernateProbe) {
    if (Core->RootIndex == 0U && Core->NextPollUs != 0U) {
      uint64_t Now = Io->NowUs(Io->Context);
      Result = WaitBudget(Core, Io, H8_TIMEOUT_US);
      if (Result == MtkUfsPowerTimeout) return Result;
      if (Now < Core->NextPollUs) return MtkUfsPowerInProgress;
      Core->NextPollUs = 0U;
    }
    switch (Core->RootIndex) {
      case 0U:
        Result = Write32(Io, MtkUfsPowerBankHci, REG_DEBUG_SEL,
                         (((Core->IpVersion >> 16) & 0xFFU) >= 0x36U) ? 0x00820820U : 0x20U);
        if (Result != MtkUfsPowerSuccess) return Result;
        Core->RootIndex = (((Core->IpVersion >> 16) & 0xFFU) >= 0x36U) ? 1U : 5U;
        return MtkUfsPowerInProgress;
      case 1U:
        Result = Write32(Io, MtkUfsPowerBankHci, REG_DEBUG_SEL_B0, 0U);
        if (Result != MtkUfsPowerSuccess) return Result;
        Core->RootIndex++;
        return MtkUfsPowerInProgress;
      case 2U:
        Result = Write32(Io, MtkUfsPowerBankHci, REG_DEBUG_SEL_B1, 0x55555555U);
        if (Result != MtkUfsPowerSuccess) return Result;
        Core->RootIndex++;
        return MtkUfsPowerInProgress;
      case 3U:
        Result = Write32(Io, MtkUfsPowerBankHci, REG_DEBUG_SEL_B2, 0xAAAAAAAAU);
        if (Result != MtkUfsPowerSuccess) return Result;
        Core->RootIndex++;
        return MtkUfsPowerInProgress;
      case 4U:
        Result = Write32(Io, MtkUfsPowerBankHci, REG_DEBUG_SEL_B3, UINT32_MAX);
        if (Result != MtkUfsPowerSuccess) return Result;
        Core->RootIndex = 5U;
        return MtkUfsPowerInProgress;
      default:
        Core->Phase = MtkUfsPowerPhasePrepareHibernateWait;
        Core->RootIndex = 0U;
        return MtkUfsPowerInProgress;
    }
  }
  if (Core->Phase == MtkUfsPowerPhasePrepareHibernateWait) {
    uint32_t Probe;
    uint64_t Now = Io->NowUs(Io->Context);
    Result = WaitBudget(Core, Io, H8_TIMEOUT_US);
    if (Result == MtkUfsPowerTimeout) return Result;
    if (Core->NextPollUs != 0U && Now < Core->NextPollUs) return MtkUfsPowerInProgress;
    Core->NextPollUs = 0U;
    Result = Read32(Io, MtkUfsPowerBankHci, REG_UFS_PROBE, &Probe);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Probe == UINT32_MAX) return MtkUfsPowerIoError;
    if ((Probe >> 28) == UFS_LINK_HIBERN8) {
      Core->Phase = MtkUfsPowerPhasePreparePowerdown;
      Core->RootIndex = 0U;
      return MtkUfsPowerInProgress;
    }
    Now = Io->NowUs(Io->Context);
    Core->NextPollUs = Now > UINT64_MAX - H8_POLL_STEP_US ? UINT64_MAX : Now + H8_POLL_STEP_US;
    Core->Phase = MtkUfsPowerPhasePrepareHibernateProbe;
    Core->RootIndex = 0U;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhasePreparePowerdown) {
    Result = UicStep(Core, Io, UIC_DME_SET, (uint32_t)DME_POWERDOWN << 16, 1U, &Returned);
    if (Result != MtkUfsPowerSuccess) return Result;
    return CompleteOperation(Core, MtkUfsPowerStopPrepared);
  }
  return MtkUfsPowerIoError;
}

static MTK_UFS_POWER_RESULT ContinueRelease(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io
  )
{
  MTK_UFS_POWER_RESULT Result;
  uint32_t Value, Enabled, Bit;
  int Complete;
  if (Core->Phase == MtkUfsPowerPhaseReleaseRoots) {
    Result = CheckStoppedRoots(Core, Io, &Core->RootIndex, &Complete);
    if (Result == MtkUfsPowerBusy) {
      Core->RootIndex = 0;
      return Result;
    }
    if (Result != MtkUfsPowerSuccess && Result != MtkUfsPowerInProgress) return Result;
    if (!Complete) return MtkUfsPowerInProgress;
    Core->Phase = MtkUfsPowerPhaseReleaseVemc;
    Core->RootIndex = 0;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseReleaseVemc) {
    Result = FreshVemcStep(Core, Io, &Core->RootIndex);
    if (Result != MtkUfsPowerSuccess) return Result;
    Result = CheckTopUfsParent(Io);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->Phase = MtkUfsPowerPhaseReleaseRefSmcPre;
    Core->PhaseStartedUs = 0;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseReleaseRefSmcPre) {
    Result = SmcStep(Core, Io, SMC_REFCLK, 0U, 0U);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->Phase = MtkUfsPowerPhaseReleaseRefDelayPre;
    StartWait(Core, Io);
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseReleaseRefDelayPre) {
    Result = WaitBudget(Core, Io, HCE_TIMEOUT_US);
    if (Result == MtkUfsPowerTimeout) return Result;
    if (ElapsedUs(Core->PhaseStartedUs, Io->NowUs(Io->Context)) < REFCLK_GATE_WAIT_US) return MtkUfsPowerInProgress;
    Core->Phase = MtkUfsPowerPhaseReleaseRefWrite;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseReleaseRefWrite) {
    Core->Phase = MtkUfsPowerPhaseReleaseRefAck;
    StartWait(Core, Io);
    Result = Write32(Io, MtkUfsPowerBankHci, REG_REFCLK, 0U);
    if (Result != MtkUfsPowerSuccess) return Result;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseReleaseRefAck) {
    Result = Read32(Io, MtkUfsPowerBankHci, REG_REFCLK, &Value);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Value == UINT32_MAX) return MtkUfsPowerIoError;
    if ((Value & 3U) != 0U) {
      return WaitBudget(Core, Io, REFCLK_TIMEOUT_US);
    }
    Core->Phase = MtkUfsPowerPhaseReleaseRefSmcPost;
  }
  if (Core->Phase == MtkUfsPowerPhaseReleaseRefSmcPost) {
    Result = SmcStep(Core, Io, SMC_REFCLK, 0U, 1U);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->GateIndex = MTK_UFS_POWER_GATE_COUNT - 1U;
    Core->Phase = MtkUfsPowerPhaseReleaseGateRead;
    Core->PhasePollCount = 0U;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseReleaseGateRead) {
    if (Core->GateIndex >= MTK_UFS_POWER_GATE_COUNT) return MtkUfsPowerIoError;
    Bit = mGateBits[Core->GateIndex];
    if ((Core->GateOwnedMask & Bit) == 0U) {
      if (Core->GateIndex == 0U) return CompleteOperation(Core, MtkUfsPowerReleased);
      Core->GateIndex--;
      return MtkUfsPowerInProgress;
    }
    Result = GateStatus(Io, &Enabled);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->GateObservedMask = Enabled;
    if ((Enabled & Bit) == 0U) {
      if (Core->GateIndex == 0U) return CompleteOperation(Core, MtkUfsPowerReleased);
      Core->GateIndex--;
      return MtkUfsPowerInProgress;
    }
    Core->PhasePollCount = 0U;
    Core->PhaseStartedUs = Io->NowUs(Io->Context);
    Core->Phase = MtkUfsPowerPhaseReleaseGateWrite;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseReleaseGateWrite) {
    Bit = mGateBits[Core->GateIndex];
    /* Verify after every write attempt, including an uncertain transport
     * error, before deciding whether an idempotent retry is needed. */
    Core->Phase = MtkUfsPowerPhaseReleaseGateVerify;
    Result = Write32(Io, MtkUfsPowerBankInfra, REG_INFRA_SET, Bit);
    if (Result != MtkUfsPowerSuccess) return Result;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseReleaseGateVerify) {
    Result = GateStatus(Io, &Enabled);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->GateObservedMask = Enabled;
    Bit = mGateBits[Core->GateIndex];
    if ((Enabled & Bit) != 0U) {
      Result = WaitBudget(Core, Io, REFCLK_TIMEOUT_US);
      if (Result == MtkUfsPowerTimeout) return Result;
      Core->Phase = MtkUfsPowerPhaseReleaseGateWrite;
      return MtkUfsPowerInProgress;
    }
    if (Core->GateIndex == 0U) return CompleteOperation(Core, MtkUfsPowerReleased);
    Core->GateIndex--;
    Core->PhasePollCount = 0U;
    Core->Phase = MtkUfsPowerPhaseReleaseGateRead;
    return MtkUfsPowerInProgress;
  }
  (void)Value;
  return MtkUfsPowerIoError;
}

static MTK_UFS_POWER_RESULT ContinueRestore(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io
  )
{
  MTK_UFS_POWER_RESULT Result;
  uint32_t Value, Gates, Bit, Returned;
  int Complete;

  if (Core->Phase == MtkUfsPowerPhaseRestoreVemc) {
    Result = FreshVemcStep(Core, Io, &Core->RootIndex);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->QueryVemcGood = 1U;
    Result = CheckTopUfsParent(Io);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->GateIndex = 0;
    Core->Phase = MtkUfsPowerPhaseRestoreGateRead;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreGateRead) {
    if (Core->GateIndex >= MTK_UFS_POWER_GATE_COUNT) {
      Core->Phase = MtkUfsPowerPhaseRestoreRoots;
      Core->RootIndex = 0;
      return MtkUfsPowerInProgress;
    }
    Bit = mGateBits[Core->GateIndex];
    if ((Core->GateOwnedMask & Bit) == 0U) {
      Core->GateIndex++;
      return MtkUfsPowerInProgress;
    }
    Result = GateStatus(Io, &Gates);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->GateObservedMask = Gates;
    if ((Gates & Bit) != 0U) {
      Core->GateIndex++;
      return MtkUfsPowerInProgress;
    }
    Core->PhasePollCount = 0U;
    Core->PhaseStartedUs = Io->NowUs(Io->Context);
    Core->Phase = MtkUfsPowerPhaseRestoreGateWrite;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreGateWrite) {
    Bit = mGateBits[Core->GateIndex];
    Core->Phase = MtkUfsPowerPhaseRestoreGateVerify;
    Result = Write32(Io, MtkUfsPowerBankInfra, REG_INFRA_CLR, Bit);
    if (Result != MtkUfsPowerSuccess) return Result;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreGateVerify) {
    Result = GateStatus(Io, &Gates);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->GateObservedMask = Gates;
    Bit = mGateBits[Core->GateIndex];
    if ((Gates & Bit) == 0U) {
      Result = WaitBudget(Core, Io, REFCLK_TIMEOUT_US);
      if (Result == MtkUfsPowerTimeout) return Result;
      Core->Phase = MtkUfsPowerPhaseRestoreGateWrite;
      return MtkUfsPowerInProgress;
    }
    Core->GateOwnedMask |= Bit;
    Core->GateIndex++;
    Core->Phase = MtkUfsPowerPhaseRestoreGateRead;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreRoots) {
    Result = CheckStoppedRoots(Core, Io, &Core->RootIndex, &Complete);
    if (Result == MtkUfsPowerBusy) {
      if ((Core->GateObservedMask & Core->GateOwnedMask) != Core->GateOwnedMask) {
        Core->RootIndex = 0U;
        Core->QueryVemcGood = 0U;
        Core->Phase = MtkUfsPowerPhaseRestoreVemc;
        return MtkUfsPowerInProgress;
      }
      Core->RootIndex = 0;
      return Result;
    }
    if (Result != MtkUfsPowerSuccess && Result != MtkUfsPowerInProgress) return Result;
    if (!Complete) return MtkUfsPowerInProgress;
    Core->RootIndex = 0;
    Core->Phase = MtkUfsPowerPhaseRestoreRefOffCheck;
    StartWait(Core, Io);
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreRefOffCheck) {
    Result = Read32(Io, MtkUfsPowerBankHci, REG_REFCLK, &Value);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Value == UINT32_MAX) return MtkUfsPowerIoError;
    if ((Value & 3U) == 0U) {
      Core->Phase = MtkUfsPowerPhaseRestoreResetAssert;
      return MtkUfsPowerInProgress;
    }
    if ((Value & 3U) == 3U) {
      Core->Phase = MtkUfsPowerPhaseRestoreRefOffSmcPre;
      return MtkUfsPowerInProgress;
    }
    Result = WaitBudget(Core, Io, REFCLK_TIMEOUT_US);
    return Result == MtkUfsPowerTimeout ? Result : MtkUfsPowerBusy;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreRefOffSmcPre) {
    Result = SmcStep(Core, Io, SMC_REFCLK, 0U, 0U);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->Phase = MtkUfsPowerPhaseRestoreRefOffDelayPre;
    StartWait(Core, Io);
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreRefOffDelayPre) {
    Result = WaitBudget(Core, Io, HCE_TIMEOUT_US);
    if (Result == MtkUfsPowerTimeout) return Result;
    if (ElapsedUs(Core->PhaseStartedUs, Io->NowUs(Io->Context)) < REFCLK_GATE_WAIT_US) {
      return MtkUfsPowerInProgress;
    }
    Core->Phase = MtkUfsPowerPhaseRestoreRefOffWrite;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreRefOffWrite) {
    Core->Phase = MtkUfsPowerPhaseRestoreRefOffAck;
    StartWait(Core, Io);
    Result = Write32(Io, MtkUfsPowerBankHci, REG_REFCLK, 0U);
    if (Result != MtkUfsPowerSuccess) return Result;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreRefOffAck) {
    Result = Read32(Io, MtkUfsPowerBankHci, REG_REFCLK, &Value);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Value == UINT32_MAX) return MtkUfsPowerIoError;
    if ((Value & 3U) != 0U) {
      return WaitBudget(Core, Io, REFCLK_TIMEOUT_US);
    }
    Core->Phase = MtkUfsPowerPhaseRestoreRefOffSmcPost;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreRefOffSmcPost) {
    Result = SmcStep(Core, Io, SMC_REFCLK, 0U, 1U);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->Phase = MtkUfsPowerPhaseRestoreResetAssert;
    Core->RootIndex = 0U;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreRefOnCheck) {
    Result = Read32(Io, MtkUfsPowerBankHci, REG_REFCLK, &Value);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Value == UINT32_MAX) return MtkUfsPowerIoError;
    if ((Value & 3U) == 0U) {
      Core->Phase = MtkUfsPowerPhaseRestoreRefSmcPre;
      return MtkUfsPowerInProgress;
    }
    if ((Value & 3U) == 3U) return MtkUfsPowerWrongState;
    Result = WaitBudget(Core, Io, REFCLK_TIMEOUT_US);
    return Result == MtkUfsPowerTimeout ? Result : MtkUfsPowerBusy;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreRefWrite) {
    if (Core->RootIndex != 1U) return MtkUfsPowerIoError;
    Core->Phase = MtkUfsPowerPhaseRestoreRefAck;
    StartWait(Core, Io);
    Result = Write32(Io, MtkUfsPowerBankHci, REG_REFCLK, 1U);
    if (Result != MtkUfsPowerSuccess) return Result;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreRefSmcPre) {
    Result = SmcStep(Core, Io, SMC_REFCLK, 1U, 0U);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->Phase = MtkUfsPowerPhaseRestoreRefWrite;
    Core->RootIndex = 1U;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreRefAck) {
    Result = Read32(Io, MtkUfsPowerBankHci, REG_REFCLK, &Value);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Value == UINT32_MAX) return MtkUfsPowerIoError;
    if ((Value & 3U) != 3U) {
      return WaitBudget(Core, Io, REFCLK_TIMEOUT_US);
    }
    Core->Phase = MtkUfsPowerPhaseRestoreRefDelayPost;
    StartWait(Core, Io);
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreRefDelayPost) {
    Result = WaitBudget(Core, Io, HCE_TIMEOUT_US);
    if (Result == MtkUfsPowerTimeout) return Result;
    if (ElapsedUs(Core->PhaseStartedUs, Io->NowUs(Io->Context)) < REFCLK_UNGATE_WAIT_US) return MtkUfsPowerInProgress;
    Core->Phase = MtkUfsPowerPhaseRestoreRefSmcPost;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreRefSmcPost) {
    Result = SmcStep(Core, Io, SMC_REFCLK, 1U, 1U);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->Phase = MtkUfsPowerPhaseRestorePreHce;
    Core->RootIndex = 0U;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreResetAssert) {
    Result = SmcStep(Core, Io, SMC_RESET, 0U, 0U);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->Phase = MtkUfsPowerPhaseRestoreResetRelease;
    Core->ResetIndex = 0;
    Core->PhaseStartedUs = Io->NowUs(Io->Context);
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreResetRelease) {
    if (Core->ResetIndex == 0U) {
      Core->ResetIndex = 1U;
      StartWait(Core, Io);
      Result = Write32(Io, MtkUfsPowerBankHci, REG_HCE, 0U);
      if (Result != MtkUfsPowerSuccess) return Result;
      return MtkUfsPowerInProgress;
    }
    if (Core->ResetIndex == 1U) {
      Result = Read32(Io, MtkUfsPowerBankHci, REG_HCE, &Value);
      if (Result != MtkUfsPowerSuccess) return Result;
      if (Value == UINT32_MAX) return MtkUfsPowerIoError;
      if ((Value & 1U) != 0U) {
        Result = WaitBudget(Core, Io, HCE_TIMEOUT_US);
        return Result == MtkUfsPowerTimeout ? Result : MtkUfsPowerInProgress;
      }
      if (ElapsedUs(Core->PhaseStartedUs, Io->NowUs(Io->Context)) < 10U) {
        Result = WaitBudget(Core, Io, HCE_TIMEOUT_US);
        return Result == MtkUfsPowerTimeout ? Result : MtkUfsPowerInProgress;
      }
      Result = SmcStep(Core, Io, SMC_RESET, 1U, 0U);
      if (Result != MtkUfsPowerSuccess) return Result;
      Core->ResetIndex = 2U;
      StartWait(Core, Io);
      return MtkUfsPowerInProgress;
    }
    if (Core->ResetIndex == 2U) {
      if (ElapsedUs(Core->PhaseStartedUs, Io->NowUs(Io->Context)) < DEVICE_RESET_US) {
        Result = WaitBudget(Core, Io, HCE_TIMEOUT_US);
        return Result == MtkUfsPowerTimeout ? Result : MtkUfsPowerInProgress;
      }
      Core->Phase = MtkUfsPowerPhaseRestoreRefOnCheck;
      Core->RootIndex = 0;
      StartWait(Core, Io);
      return MtkUfsPowerInProgress;
    }
  }
  if (Core->Phase == MtkUfsPowerPhaseRestorePreHce) {
    /* Reset assertion/release order and reset bits are the MT6789 T70 entries. */
    if (Core->RootIndex == 0U) {
      Result = Read32(Io, MtkUfsPowerBankHci, REG_IP_VERSION, &Value);
      if (Result != MtkUfsPowerSuccess) return Result;
      if (Value == 0U || Value == UINT32_MAX) return MtkUfsPowerIoError;
      Core->IpVersion = (((Value | 0x01000000U) >> 24) == 1U) ? ((Value & 0x00FFFFFFU) | 0x10000000U) : Value;
      if (Core->IpVersion == 0x10480000U) return MtkUfsPowerUnsupported;
      Core->RootIndex++;
      return MtkUfsPowerInProgress;
    }
    if (Core->RootIndex >= 1U && Core->RootIndex <= 3U) {
      static const uint32_t Order[3] = { 0U, 2U, 1U };
      uint32_t I = Order[Core->RootIndex - 1U];
      Result = Write32(Io, MtkUfsPowerBankInfra, mResetSet[I], mResetBits[I]);
      if (Result != MtkUfsPowerSuccess) return Result;
      Core->RootIndex++;
      return MtkUfsPowerInProgress;
    }
    if (Core->RootIndex == 4U) {
      StartWait(Core, Io);
      Core->RootIndex++;
      return MtkUfsPowerInProgress;
    }
    if (Core->RootIndex == 5U) {
      if (ElapsedUs(Core->PhaseStartedUs, Io->NowUs(Io->Context)) < RESET_SETTLE_US) {
        Result = WaitBudget(Core, Io, HCE_TIMEOUT_US);
        return Result == MtkUfsPowerTimeout ? Result : MtkUfsPowerInProgress;
      }
      Core->RootIndex++;
      return MtkUfsPowerInProgress;
    }
    if (Core->RootIndex >= 6U && Core->RootIndex <= 8U) {
      static const uint32_t Order[3] = { 1U, 2U, 0U };
      uint32_t I = Order[Core->RootIndex - 6U];
      Result = Write32(Io, MtkUfsPowerBankInfra, mResetClear[I], mResetBits[I]);
      if (Result != MtkUfsPowerSuccess) return Result;
      Core->RootIndex++;
      return MtkUfsPowerInProgress;
    }
    if (Core->RootIndex == 9U) {
      Result = Write32(Io, MtkUfsPowerBankHci, REG_AHIT, 0U);
      if (Result != MtkUfsPowerSuccess) return Result;
      Core->RootIndex++;
      return MtkUfsPowerInProgress;
    }
    if (Core->RootIndex == 10U) {
      Result = CheckAndModify(Io, REG_XOUFS, 0U, 0x80U);
      if (Result != MtkUfsPowerSuccess) return Result;
      Core->RootIndex++;
      return MtkUfsPowerInProgress;
    }
    if (Core->RootIndex == 11U && Core->IpVersion > 0x1044FFFFU && Core->IpVersion != 0x20160706U) {
      Result = CheckAndModify(Io, REG_OPT, 0x007FFF00U, 0x00453000U);
      if (Result != MtkUfsPowerSuccess) return Result;
      Core->RootIndex++;
      return MtkUfsPowerInProgress;
    }
    if (Core->RootIndex == 12U && Core->IpVersion > 0x1045FFFFU) {
      Result = CheckAndModify(Io, REG_OPT, 0x10000000U, 0x02000000U);
      if (Result != MtkUfsPowerSuccess) return Result;
      Core->RootIndex++;
      return MtkUfsPowerInProgress;
    }
    Core->Phase = MtkUfsPowerPhaseRestoreHce;
    Core->RootIndex = 0U;
    Core->PhasePollCount = 0U;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreHce) {
    if (Core->RootIndex == 0U) {
      Core->RootIndex = 1U;
      StartWait(Core, Io);
      Result = Write32(Io, MtkUfsPowerBankHci, REG_HCE, 1U);
      if (Result != MtkUfsPowerSuccess) return Result;
      return MtkUfsPowerInProgress;
    }
    if (ElapsedUs(Core->PhaseStartedUs, Io->NowUs(Io->Context)) < HCE_SETTLE_US) {
      Result = WaitBudget(Core, Io, HCE_TIMEOUT_US);
      return Result == MtkUfsPowerTimeout ? Result : MtkUfsPowerInProgress;
    }
    Result = Read32(Io, MtkUfsPowerBankHci, REG_HCE, &Value);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Value == UINT32_MAX || (Value & 1U) == 0U) {
      Result = WaitBudget(Core, Io, HCE_TIMEOUT_US);
      return Result == MtkUfsPowerTimeout ? Result : MtkUfsPowerInProgress;
    }
    Result = Read32(Io, MtkUfsPowerBankHci, REG_HCS, &Value);
    if (Result != MtkUfsPowerSuccess) return Result;
    if (Value == UINT32_MAX || (Value & UFS_HCS_UCRDY) == 0U) {
      Result = WaitBudget(Core, Io, HCE_TIMEOUT_US);
      return Result == MtkUfsPowerTimeout ? Result : MtkUfsPowerInProgress;
    }
    Core->Phase = MtkUfsPowerPhaseRestorePostHce;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestorePostHce) {
    Core->Phase = MtkUfsPowerPhaseRestorePreLink;
    Core->RootIndex = 0;
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestorePreLink) {
    switch (Core->RootIndex) {
      case 0:
        Result = UicStep(Core, Io, UIC_DME_SET, (uint32_t)DME_POWERDOWN << 16, 0U, &Returned);
        break;
      case 1:
        Result = UicStep(Core, Io, UIC_DME_SET, (uint32_t)DME_PCS_CPORT << 16, 0U, &Returned);
        break;
      case 2:
        Result = UicStep(Core, Io, UIC_DME_GET, (uint32_t)DME_SAVEPOWER << 16, 0U, &Returned);
        if (Result == MtkUfsPowerSuccess) Core->UicArg3 = Returned & ~0x40U;
        break;
      case 3:
        Result = UicStep(Core, Io, UIC_DME_SET, (uint32_t)DME_SAVEPOWER << 16, Core->UicArg3, &Returned);
        break;
      case 4:
        if (Core->IpVersion != 0x10450000U) {
          Core->RootIndex++;
          return MtkUfsPowerInProgress;
        }
        Result = UicStep(Core, Io, UIC_DME_GET, (uint32_t)DME_LINK_TUNING << 16, 0U, &Returned);
        if (Result == MtkUfsPowerSuccess) Core->UicArg3 = Returned | 0x10U;
        break;
      case 5:
        if (Core->IpVersion != 0x10450000U) {
          Core->RootIndex++;
          return MtkUfsPowerInProgress;
        }
        Result = UicStep(Core, Io, UIC_DME_SET, (uint32_t)DME_LINK_TUNING << 16, Core->UicArg3, &Returned);
        break;
      default:
        Core->Phase = MtkUfsPowerPhaseRestoreLinkStartup;
        return MtkUfsPowerInProgress;
    }
    if (Result == MtkUfsPowerSuccess) {
      Core->RootIndex++;
      return MtkUfsPowerInProgress;
    }
    return Result;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestoreLinkStartup) {
    Result = UicStep(Core, Io, UIC_LINK_STARTUP, 0U, 0U, &Returned);
    if (Result != MtkUfsPowerSuccess) return Result;
    Core->Phase = MtkUfsPowerPhaseRestorePostLink;
    Core->RootIndex = 0;
    StartWait(Core, Io);
    return MtkUfsPowerInProgress;
  }
  if (Core->Phase == MtkUfsPowerPhaseRestorePostLink) {
    if (Core->RootIndex == 0U) {
      Result = Read32(Io, MtkUfsPowerBankHci, REG_HCS, &Value);
      if (Result != MtkUfsPowerSuccess) return Result;
      if (Value == UINT32_MAX) return MtkUfsPowerIoError;
      if ((Value & 9U) != 9U) {
        Result = WaitBudget(Core, Io, UIC_TIMEOUT_US);
        return Result == MtkUfsPowerTimeout ? Result : MtkUfsPowerInProgress;
      }
      Core->RootIndex++;
      return MtkUfsPowerInProgress;
    }
    if (Core->RootIndex == 1U && Core->IpVersion >= 0x10450100U) {
      Result = UicStep(Core, Io, UIC_DME_GET, (uint32_t)DME_LINK_TUNING << 16, 0U, &Returned);
      if (Result == MtkUfsPowerSuccess) Core->UicArg3 = Returned | 0x100U;
      if (Result == MtkUfsPowerSuccess) { Core->RootIndex++; return MtkUfsPowerInProgress; }
      return Result;
    }
    if ((Core->RootIndex == 2U) && Core->IpVersion >= 0x10450100U) {
      Result = UicStep(Core, Io, UIC_DME_SET, (uint32_t)DME_LINK_TUNING << 16, Core->UicArg3, &Returned);
      if (Result != MtkUfsPowerSuccess) return Result;
      Core->RootIndex++;
      return MtkUfsPowerInProgress;
    }
    if (Core->RootIndex == 1U || Core->RootIndex == 3U) {
      Result = UicStep(Core, Io, UIC_DME_GET, (uint32_t)DME_SAVEPOWER << 16, 0U, &Returned);
      if (Result == MtkUfsPowerSuccess) Core->UicArg3 = Returned | 0x0DU;
      if (Result == MtkUfsPowerSuccess) { Core->RootIndex = 4U; return MtkUfsPowerInProgress; }
      return Result;
    }
    if (Core->RootIndex == 4U) {
      Result = UicStep(Core, Io, UIC_DME_SET, (uint32_t)DME_SAVEPOWER << 16, Core->UicArg3, &Returned);
      if (Result != MtkUfsPowerSuccess) return Result;
      Core->RootIndex = 5U;
      return MtkUfsPowerInProgress;
    }
    if (Core->RootIndex == 5U) {
      Result = UicStep(Core, Io, UIC_DME_GET, (uint32_t)DME_DEBUGCLOCK << 16, 0U, &Returned);
      if (Result == MtkUfsPowerSuccess) Core->UicArg3 = Returned & ~0x20U;
      if (Result == MtkUfsPowerSuccess) { Core->RootIndex = 6U; return MtkUfsPowerInProgress; }
      return Result;
    }
    if (Core->RootIndex == 6U) {
      Result = UicStep(Core, Io, UIC_DME_SET, (uint32_t)DME_DEBUGCLOCK << 16, Core->UicArg3, &Returned);
      if (Result != MtkUfsPowerSuccess) return Result;
      return CompleteOperation(Core, MtkUfsPowerActive);
    }
  }
  return MtkUfsPowerIoError;
}

MTK_UFS_POWER_RESULT MtkUfsPowerCoreHandle(
  MTK_UFS_POWER_CORE *Core,
  const MTK_UFS_POWER_CORE_IO *Io,
  MTK_UFS_POWER_REQUEST *Request
  )
{
  MTK_UFS_POWER_RESULT Result;
  uint32_t Index;
  int GateRetryable;
  if (Request == NULL) return MtkUfsPowerInvalid;
  if (Request->Size != MTK_UFS_POWER_REQUEST_BYTES || Request->Version != MTK_UFS_POWER_VERSION ||
      Request->State != 0U || Request->Result != 0U || Request->Capabilities != 0U ||
      Request->Reserved[0] != 0U || Request->Reserved[1] != 0U ||
      Request->Reserved[2] != 0U || Request->Reserved[3] != 0U) {
    Request->State = 0U; Request->Generation = 0U; Request->Result = MtkUfsPowerInvalid; Request->Capabilities = 0U;
    return MtkUfsPowerInvalid;
  }
  if (!ValidCore(Core) || Io == NULL || Io->TryLock == NULL || Io->Unlock == NULL ||
      Io->Read32 == NULL || Io->Write32 == NULL || Io->Smc == NULL || Io->NowUs == NULL) {
    Request->State = MtkUfsPowerFault; Request->Generation = 0U; Request->Result = MtkUfsPowerInvalid; Request->Capabilities = 0U;
    return MtkUfsPowerInvalid;
  }
  if (Io->TryLock(Io->Context) == 0) {
    Request->State = 0U; Request->Generation = 0U; Request->Result = MtkUfsPowerBusy; Request->Capabilities = 0U;
    return MtkUfsPowerBusy;
  }

  if (Request->Operation == MtkUfsPowerQuery) {
    if (Request->Flags != 0U || Request->Generation != 0U) Result = MtkUfsPowerInvalid;
    else if (Core->Operation == CORE_OPERATION_NONE) Result = StartQuery(Core);
    else if (Core->Operation != MtkUfsPowerQuery) Result = MtkUfsPowerBusy;
    else Result = MtkUfsPowerInProgress;
    if (Result == MtkUfsPowerInProgress) Result = ContinueQuery(Core, Io);
  } else if (Request->Operation <= MtkUfsPowerRestore) {
    if ((Request->Operation == MtkUfsPowerPrepareStop && Request->Flags != MTK_UFS_POWER_DEVICE_DOWN_ACKED) ||
        (Request->Operation != MtkUfsPowerPrepareStop && Request->Flags != 0U)) {
      Result = MtkUfsPowerInvalid;
    } else {
      Result = BeginMutation(Core, Request);
    }
    if (Result == MtkUfsPowerInProgress) {
      if (Request->Operation == MtkUfsPowerPrepareStop) Result = ContinuePrepare(Core, Io);
      else if (Request->Operation == MtkUfsPowerRelease) Result = ContinueRelease(Core, Io);
      else Result = ContinueRestore(Core, Io);
    }
  } else {
    Result = MtkUfsPowerInvalid;
  }

  /* A root read that discovers a live controller is retryable; do not mark it
   * stopped or change hardware. Restore of StopPrepared must prove all roots. */
  if ((Result == MtkUfsPowerWrongState || Result == MtkUfsPowerUnsupported) &&
      Core->Operation == MtkUfsPowerPrepareStop && Core->Phase == MtkUfsPowerPhasePrepareCheck) {
    Core->State = MtkUfsPowerActive;
    Core->Operation = CORE_OPERATION_NONE;
    Core->Phase = MtkUfsPowerPhaseIdle;
  }
  if ((Result == MtkUfsPowerWrongState || Result == MtkUfsPowerUnsupported) &&
      Core->Operation == MtkUfsPowerQuery) {
    Core->Operation = CORE_OPERATION_NONE;
    Core->Phase = MtkUfsPowerPhaseIdle;
  }
  GateRetryable =
    (Core->Operation == MtkUfsPowerRelease &&
     (Core->Phase == MtkUfsPowerPhaseReleaseGateRead ||
      Core->Phase == MtkUfsPowerPhaseReleaseGateWrite ||
      Core->Phase == MtkUfsPowerPhaseReleaseGateVerify)) ||
    (Core->Operation == MtkUfsPowerRestore &&
     (Core->Phase == MtkUfsPowerPhaseRestoreGateRead ||
      Core->Phase == MtkUfsPowerPhaseRestoreGateWrite ||
      Core->Phase == MtkUfsPowerPhaseRestoreGateVerify));
  if ((Result == MtkUfsPowerTimeout && !GateRetryable) ||
      (Result == MtkUfsPowerIoError && (Core->SmcPending != 0U || Core->UicPending == 3U)) ||
      ((Result == MtkUfsPowerWrongState || Result == MtkUfsPowerUnsupported) &&
       Core->Operation == MtkUfsPowerPrepareStop && Core->Phase != MtkUfsPowerPhasePrepareCheck) ||
      (Result == MtkUfsPowerWrongState && Core->Operation == MtkUfsPowerRestore &&
       Core->Phase == MtkUfsPowerPhaseRestoreRefOnCheck) ||
      (Result == MtkUfsPowerUnsupported && Core->Operation == MtkUfsPowerRestore)) {
    LatchFault(Core, Result);
  }

  Index = (uint32_t)Result;
  Result = Finish(Core, Request, (MTK_UFS_POWER_RESULT)Index);
  Io->Unlock(Io->Context);
  return Result;
}
