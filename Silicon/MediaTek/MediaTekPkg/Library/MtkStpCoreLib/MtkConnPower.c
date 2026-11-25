// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkConnPower.h>

/*
 * T70 Pro wmt_drv.ko uses CCIF4_AP and runtime PM through
 * consys_platform_spm_conn_ctrl_mt6789. The CONN island sequence is from
 * mtk-scpsys.ko and the CONN domain entry in mtk-scpsys-mt6789.ko.
 */
#define CONN_CONTROL 0x304U
#define POWER_STATUS 0x16cU
#define POWER_STATUS_SECOND 0x170U
#define CONN_STATUS 0x02U
#define RESET_RELEASE 0x01U
#define ISOLATE 0x02U
#define POWER_ON 0x04U
#define POWER_ON_SECOND 0x08U
#define DISABLE_CLOCK 0x10U
#define CONTROL_MASK 0x1fU
#define CCIF_SET 0x0c0U
#define CCIF_CLEAR 0x0c4U
#define CCIF_STATUS 0x0c8U
#define CCIF_GATE (1U << 28)
#define REMAP_MASK 0xfffffU
#define PERIPHERAL_REMAP 0x1000U
#define POLL_INTERVAL_US 10U
#define POLL_RETRIES 100000U

typedef struct {
  unsigned Set, Clear, Enable, Ack, Mask;
} CONN_PROTECTION;

static const CONN_PROTECTION Protection[] = {
  {0x2a0, 0x2a4, 0x220, 0x228, 0x42000},
  {0x2a0, 0x2a4, 0x220, 0x228, 0x04000},
  {0x2a8, 0x2ac, 0x250, 0x258, 0x00400}
};
#define PROTECTION_COUNT (sizeof(Protection) / sizeof(Protection[0]))

// INFRACFG firmware EMI and peripherals, from consys_emi_set_remapping_reg.
static const unsigned RemapOffset[] = {0x380U, 0x388U};
#define REMAP_COUNT (sizeof(RemapOffset) / sizeof(RemapOffset[0]))

static int ValidIo(const MTK_CONN_POWER_IO *Io) {
  return Io && Io->Read && Io->Write && Io->WaitUs && Io->Barrier;
}

static int Read(const MTK_CONN_POWER_IO *Io, MTK_CONN_REGION Region,
                unsigned Offset, unsigned *Value) {
  return !Io->Read(Io->Context, Region, Offset, Value) && *Value != ~0U;
}

static int Write(const MTK_CONN_POWER_IO *Io, MTK_CONN_REGION Region,
                 unsigned Offset, unsigned Value) {
  int Error = Io->Write(Io->Context, Region, Offset, Value);
  Io->Barrier(Io->Context);
  return !Error;
}

static int UpdateControl(const MTK_CONN_POWER_IO *Io, unsigned Clear, unsigned Set) {
  unsigned Value;
  return Read(Io, MtkConnSpm, CONN_CONTROL, &Value) &&
         Write(Io, MtkConnSpm, CONN_CONTROL, (Value & ~Clear) | Set);
}

static MTK_CONN_POWER_STATUS CheckBits(const MTK_CONN_POWER_IO *Io, MTK_CONN_REGION Region,
                                     unsigned First, unsigned Second, unsigned Mask,
                                     unsigned Expected) {
  unsigned Value, Other = Expected;
  if (!Read(Io, Region, First, &Value) || (Second && !Read(Io, Region, Second, &Other)))
    return MtkConnPowerIoError;
  if ((Value & Mask) != Expected || (Other & Mask) != Expected) return MtkConnPowerNotReady;
  return MtkConnPowerOk;
}

static MTK_CONN_POWER_STATUS WaitBits(const MTK_CONN_POWER_IO *Io, MTK_CONN_REGION Region,
                                     unsigned First, unsigned Second, unsigned Mask,
                                     unsigned Expected) {
  unsigned Attempts;
  for (Attempts = 0; Attempts <= POLL_RETRIES; ++Attempts) {
    MTK_CONN_POWER_STATUS Status = CheckBits(Io, Region, First, Second, Mask, Expected);
    if (Status != MtkConnPowerNotReady) return Status;
    if (Attempts == POLL_RETRIES) break;
    if (Io->WaitUs(Io->Context, POLL_INTERVAL_US)) return MtkConnPowerIoError;
  }
  return MtkConnPowerTimeout;
}

static MTK_CONN_POWER_STATUS CheckIsland(const MTK_CONN_POWER_IO *Io, int On) {
  MTK_CONN_POWER_STATUS Status;
  unsigned Index;
  Status = CheckBits(Io, MtkConnSpm, POWER_STATUS, POWER_STATUS_SECOND, CONN_STATUS,
                     On ? CONN_STATUS : 0);
  if (Status != MtkConnPowerOk) return Status;
  Status = CheckBits(Io, MtkConnSpm, CONN_CONTROL, 0, CONTROL_MASK,
                     On ? RESET_RELEASE | POWER_ON | POWER_ON_SECOND : ISOLATE | DISABLE_CLOCK);
  if (Status != MtkConnPowerOk) return Status;
  for (Index = 0; Index < PROTECTION_COUNT; ++Index) {
    const CONN_PROTECTION *Entry = &Protection[Index];
    // Stock CONN ignores clear ACKs. Read enable bits for live admission.
    Status = CheckBits(Io, MtkConnInfra, Entry->Enable, On ? 0 : Entry->Ack,
                       Entry->Mask, On ? 0 : Entry->Mask);
    if (Status != MtkConnPowerOk) return Status;
  }
  return MtkConnPowerOk;
}

static MTK_CONN_POWER_STATUS SetProtection(const MTK_CONN_POWER_IO *Io, int Enable) {
  unsigned Index;
  for (Index = 0; Index < PROTECTION_COUNT; ++Index) {
    const CONN_PROTECTION *Entry = &Protection[Enable ? Index : PROTECTION_COUNT - Index - 1];
    MTK_CONN_POWER_STATUS Status;
    if (!Write(Io, MtkConnInfra, Enable ? Entry->Set : Entry->Clear, Entry->Mask))
      return MtkConnPowerIoError;
    Status = WaitBits(Io, MtkConnInfra, Entry->Enable, Enable ? Entry->Ack : 0,
                      Entry->Mask, Enable ? Entry->Mask : 0);
    if (Status != MtkConnPowerOk) return Status;
  }
  return MtkConnPowerOk;
}

static MTK_CONN_POWER_STATUS CheckRemap(const MTK_CONN_POWER *Power) {
  unsigned Index;
  for (Index = 0; Index < REMAP_COUNT; ++Index) {
    MTK_CONN_POWER_STATUS Status = CheckBits(&Power->Io, MtkConnInfra, RemapOffset[Index], 0,
                                           REMAP_MASK, Power->TargetRemap[Index]);
    if (Status != MtkConnPowerOk) return Status;
  }
  return MtkConnPowerOk;
}

static MTK_CONN_POWER_STATUS WriteRemap(const MTK_CONN_POWER_IO *Io, unsigned Index,
                                      unsigned Value) {
  unsigned Current, Offset = RemapOffset[Index];
  if (!Read(Io, MtkConnInfra, Offset, &Current))
    return MtkConnPowerIoError;
  if ((Current & REMAP_MASK) != Value &&
      !Write(Io, MtkConnInfra, Offset, (Current & ~REMAP_MASK) | Value))
    return MtkConnPowerIoError;
  return CheckBits(Io, MtkConnInfra, Offset, 0, REMAP_MASK, Value);
}

MTK_CONN_POWER_STATUS MtkConnPowerInit(MTK_CONN_POWER *Power, const MTK_CONN_POWER_IO *Io) {
  if (!Power || !ValidIo(Io)) return MtkConnPowerInvalid;
  Power->Io = *Io;
  Power->State = MtkConnOff;
  Power->IslandOwned = 0;
  Power->RestoreCcifGate = 0;
  Power->RemapOwned = 0;
  return MtkConnPowerOk;
}

MTK_CONN_POWER_STATUS MtkConnPowerCheck(const MTK_CONN_POWER *Power) {
  MTK_CONN_POWER_STATUS Status;
  if (!Power || !ValidIo(&Power->Io)) return MtkConnPowerInvalid;
  if (Power->State != MtkConnOn) return MtkConnPowerNotReady;
  Status = CheckIsland(&Power->Io, 1);
  if (Status != MtkConnPowerOk) return Status;
  Status = CheckBits(&Power->Io, MtkConnInfra, CCIF_STATUS, 0, CCIF_GATE, 0);
  if (Status != MtkConnPowerOk) return Status;
  return CheckRemap(Power);
}

MTK_CONN_POWER_STATUS MtkConnPowerCanStart(const MTK_CONN_POWER *Power,
                                         unsigned long long EmiBase, unsigned EmiSize) {
  if (!Power || !ValidIo(&Power->Io) || EmiSize != MTK_CONN_EMI_SIZE ||
      !EmiBase || (EmiBase & (MTK_CONN_EMI_ALIGN - 1U)) ||
      EmiBase > MTK_CONN_EMI_LIMIT - MTK_CONN_EMI_SIZE)
    return MtkConnPowerInvalid;
  if (Power->State != MtkConnOff) return MtkConnPowerBusy;
  return CheckIsland(&Power->Io, 0);
}

MTK_CONN_POWER_STATUS MtkConnPowerStart(MTK_CONN_POWER *Power,
                                      unsigned long long EmiBase, unsigned EmiSize) {
  const MTK_CONN_POWER_IO *Io;
  MTK_CONN_POWER_STATUS Status;
  unsigned Clock, Index, Value;
  Status = MtkConnPowerCanStart(Power, EmiBase, EmiSize);
  if (Status != MtkConnPowerOk) return Status;
  Io = &Power->Io;
  if (!Read(Io, MtkConnInfra, CCIF_STATUS, &Clock)) return MtkConnPowerIoError;
  for (Index = 0; Index < REMAP_COUNT; ++Index) {
    if (!Read(Io, MtkConnInfra, RemapOffset[Index], &Value)) return MtkConnPowerIoError;
    Power->SavedRemap[Index] = Value & REMAP_MASK;
  }
  Power->TargetRemap[0] = (unsigned)(EmiBase >> 16);
  Power->TargetRemap[1] = PERIPHERAL_REMAP;

  Power->State = MtkConnFault;
  for (Index = 0; Index < REMAP_COUNT; ++Index) {
    if (Power->SavedRemap[Index] == Power->TargetRemap[Index]) continue;
    Power->RemapOwned |= 1U << Index;
    Status = WriteRemap(Io, Index, Power->TargetRemap[Index]);
    if (Status != MtkConnPowerOk) return Status;
  }
  Status = CheckRemap(Power);
  if (Status != MtkConnPowerOk) return Status;
  Power->IslandOwned = 1;
  Power->RestoreCcifGate = (Clock & CCIF_GATE) != 0;
  if (Power->RestoreCcifGate) {
    if (!Write(Io, MtkConnInfra, CCIF_CLEAR, CCIF_GATE)) return MtkConnPowerIoError;
    Status = WaitBits(Io, MtkConnInfra, CCIF_STATUS, 0, CCIF_GATE, 0);
    if (Status != MtkConnPowerOk) return Status;
  }
  if (!UpdateControl(Io, 0, POWER_ON) || !UpdateControl(Io, 0, POWER_ON_SECOND))
    return MtkConnPowerIoError;
  Status = WaitBits(Io, MtkConnSpm, POWER_STATUS, POWER_STATUS_SECOND, CONN_STATUS, CONN_STATUS);
  if (Status != MtkConnPowerOk) return Status;
  if (Io->WaitUs(Io->Context, 100)) return MtkConnPowerIoError;
  Status = CheckRemap(Power);
  if (Status != MtkConnPowerOk) return Status;
  if (!UpdateControl(Io, DISABLE_CLOCK, 0) ||
      !UpdateControl(Io, ISOLATE, 0) || !UpdateControl(Io, 0, RESET_RELEASE))
    return MtkConnPowerIoError;
  Status = CheckBits(Io, MtkConnSpm, CONN_CONTROL, 0, CONTROL_MASK,
                     POWER_ON | POWER_ON_SECOND | RESET_RELEASE);
  if (Status != MtkConnPowerOk) return Status;
  Status = CheckBits(Io, MtkConnInfra, CCIF_STATUS, 0, CCIF_GATE, 0);
  if (Status != MtkConnPowerOk) return Status;
  Status = CheckRemap(Power);
  if (Status != MtkConnPowerOk) return Status;
  Status = SetProtection(Io, 0);
  if (Status != MtkConnPowerOk) return Status;
  Power->State = MtkConnOn;
  Status = MtkConnPowerCheck(Power);
  if (Status != MtkConnPowerOk) Power->State = MtkConnFault;
  return Status;
}

static MTK_CONN_POWER_STATUS StopIsland(MTK_CONN_POWER *Power) {
  const MTK_CONN_POWER_IO *Io = &Power->Io;
  MTK_CONN_POWER_STATUS Status;
  Status = SetProtection(Io, 1);
  if (Status != MtkConnPowerOk) return Status;
  if (!UpdateControl(Io, 0, ISOLATE) || !UpdateControl(Io, RESET_RELEASE, 0) ||
      !UpdateControl(Io, 0, DISABLE_CLOCK))
    return MtkConnPowerIoError;
  Status = CheckBits(Io, MtkConnSpm, CONN_CONTROL, 0, ISOLATE | RESET_RELEASE | DISABLE_CLOCK,
                     ISOLATE | DISABLE_CLOCK);
  if (Status != MtkConnPowerOk) return Status;
  if (!UpdateControl(Io, POWER_ON, 0) || !UpdateControl(Io, POWER_ON_SECOND, 0))
    return MtkConnPowerIoError;
  Status = WaitBits(Io, MtkConnSpm, POWER_STATUS, POWER_STATUS_SECOND, CONN_STATUS, 0);
  if (Status != MtkConnPowerOk) return Status;
  Status = CheckIsland(Io, 0);
  if (Status != MtkConnPowerOk) return Status;
  if (Power->RestoreCcifGate) {
    if (!Write(Io, MtkConnInfra, CCIF_SET, CCIF_GATE)) return MtkConnPowerIoError;
    Status = WaitBits(Io, MtkConnInfra, CCIF_STATUS, 0, CCIF_GATE, CCIF_GATE);
    if (Status != MtkConnPowerOk) return Status;
  }
  Power->RestoreCcifGate = 0;
  Power->IslandOwned = 0;
  return MtkConnPowerOk;
}

MTK_CONN_POWER_STATUS MtkConnPowerStop(MTK_CONN_POWER *Power) {
  MTK_CONN_POWER_STATUS Status;
  unsigned Remaining;
  if (!Power || !ValidIo(&Power->Io)) return MtkConnPowerInvalid;
  if (Power->State == MtkConnOff) return MtkConnPowerOk;
  Power->State = MtkConnFault;
  Status = Power->IslandOwned ? StopIsland(Power) : CheckIsland(&Power->Io, 0);
  if (Status != MtkConnPowerOk) return Status;

  for (Remaining = REMAP_COUNT; Remaining != 0; --Remaining) {
    unsigned Index = Remaining - 1;
    unsigned Bit = 1U << Index;
    if (!(Power->RemapOwned & Bit)) continue;
    Status = WriteRemap(&Power->Io, Index, Power->SavedRemap[Index]);
    if (Status != MtkConnPowerOk) return Status;
    Power->RemapOwned &= ~Bit;
  }
  Power->State = MtkConnOff;
  return MtkConnPowerOk;
}
