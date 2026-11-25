// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkConnRails.h>

#define VCN18_ENABLE    0x1c58U
#define VCN18_STATUS    0x1c66U
#define LDO_QI          0x8000U
#define LDO_ENABLE_US   270U
#define LDO_POLL_US     10U
#define LDO_POLL_COUNT  100U
#define LDO_OP_MASK     3U
#define LDO_OP_ENABLE   3U
#define LDO_OP_CONFIG   2U
#define VCN18_OFF_US    300U

#define VS2_STATUS       0x1720U
#define VS2_VOTER        0x1724U
#define VS2_VOTER_SET    0x1726U
#define VS2_VOTER_CLEAR  0x1728U
#define VS2_VCN13_VOTE   0x10U

enum { PinMode, PinDirection, PinOutput };
enum { Vs2VoteVoltage, Vs2Elr0, Vs2Con1 };

static const unsigned PinOffset[] = {0x380U, 0x20U, 0x120U};
static const unsigned PinMask[] = {0xf0000U, 0x10U, 0x10U};
static const unsigned Vs2Address[] = {0x172aU, 0x172eU, 0x170aU};
static const unsigned Vs2Value[] = {0x46U, 0x4dU, 0x46U};

typedef struct {
  unsigned Enable, OpEnable, OpConfig, Status, Voltage, VoltageMask, TargetVoltage, OtherEnable;
} CONN_LDO;

// Runtime fields from stock consys_hw_vcn18_ctrl and consys_hw_vcn33_ctrl.
// MT6366 voltage selectors include the four-bit fine adjustment.
static const CONN_LDO Vcn18 = {VCN18_ENABLE, 0x1c5aU, 0x1c60U, VCN18_STATUS,
                             0x1e8cU, 0xf0fU, 0xc00U, 0};
static const CONN_LDO Vcn33 = {0x1d1cU, 0x1d1eU, 0x1d24U, 0x1d2cU,
                             0x1e28U, 0x30fU, 0x100U, 0x1d2aU};

static int ReadPmic(const MTK_CONN_RAIL_IO *Io, unsigned Address, unsigned *Value)
{
  return !Io->ReadPmic(Io->Context, Address, Value) && *Value <= 0xffffU;
}

static int UpdatePmic(const MTK_CONN_RAIL_IO *Io, unsigned Address, unsigned Mask, unsigned Value)
{
  unsigned Live, Verify;
  if (!ReadPmic(Io, Address, &Live)) return 0;
  if ((Live & Mask) == Value) return 1;
  return !Io->WritePmic(Io->Context, Address, (Live & ~Mask) | Value) &&
         ReadPmic(Io, Address, &Verify) && (Verify & Mask) == Value;
}

static int UpdatePin(const MTK_CONN_RAIL_IO *Io, unsigned Field, unsigned Value)
{
  unsigned Live, Verify;
  unsigned Offset = PinOffset[Field], Mask = PinMask[Field];
  if (Io->ReadGpio(Io->Context, Offset, &Live)) return 0;
  if ((Live & Mask) == Value) return 1;
  return !Io->WriteGpio(Io->Context, Offset, (Live & ~Mask) | Value) &&
         !Io->ReadGpio(Io->Context, Offset, &Verify) && (Verify & Mask) == Value;
}

static MTK_CONN_POWER_STATUS WaitLdo(const MTK_CONN_RAIL_IO *Io, unsigned Address, int Ready)
{
  unsigned Status;
  for (unsigned Poll = 0; Poll <= LDO_POLL_COUNT; ++Poll) {
    if (!ReadPmic(Io, Address, &Status)) return MtkConnPowerIoError;
    if (!!(Status & LDO_QI) == Ready) return MtkConnPowerOk;
    if (Poll != LDO_POLL_COUNT && Io->WaitUs(Io->Context, LDO_POLL_US))
      return MtkConnPowerIoError;
  }
  return MtkConnPowerTimeout;
}

static int SetVs2Vote(const MTK_CONN_RAIL_IO *Io, int Enable)
{
  unsigned Live;
  unsigned Expected = Enable ? VS2_VCN13_VOTE : 0U;
  if (!ReadPmic(Io, VS2_VOTER, &Live)) return 0;
  if ((Live & VS2_VCN13_VOTE) == Expected) return 1;
  return !Io->WritePmic(Io->Context, Enable ? VS2_VOTER_SET : VS2_VOTER_CLEAR, VS2_VCN13_VOTE) &&
         ReadPmic(Io, VS2_VOTER, &Live) && (Live & VS2_VCN13_VOTE) == Expected;
}

static MTK_CONN_POWER_STATUS AcquireVcn13(MTK_CONN_RAILS *Rails)
{
  const MTK_CONN_RAIL_IO *Io = &Rails->Io;
  unsigned Live, Pin[3], Vs2[3];
  if (Rails->Vcn13Captured) return MtkConnPowerBusy;
  if (!Rails->Vcn18Ready) return MtkConnPowerNotReady;
  if (!ReadPmic(Io, VCN18_STATUS, &Live)) return MtkConnPowerIoError;
  if (!(Live & LDO_QI)) return MtkConnPowerNotReady;
  if (!ReadPmic(Io, VS2_STATUS, &Live)) return MtkConnPowerIoError;
  if (!(Live & 1U)) return MtkConnPowerNotReady;
  if (!ReadPmic(Io, VS2_VOTER, &Live)) return MtkConnPowerIoError;
  if (Live & VS2_VCN13_VOTE) return MtkConnPowerBusy;

  for (unsigned Field = 0; Field < 3U; ++Field) {
    if (Io->ReadGpio(Io->Context, PinOffset[Field], &Pin[Field]) ||
        !ReadPmic(Io, Vs2Address[Field], &Vs2[Field]))
      return MtkConnPowerIoError;
    Pin[Field] &= PinMask[Field];
  }
  if (!Pin[PinMode] && Pin[PinDirection] && Pin[PinOutput]) return MtkConnPowerBusy;
  for (unsigned Field = 0; Field < 3U; ++Field) {
    Rails->SavedPin[Field] = Pin[Field];
    Rails->SavedVs2[Field] = Vs2[Field];
  }
  Rails->Vcn13Captured = 1;

  // Make sure GPIO68 stays low when changing the mux or output direction.
  // The stock PMIC votes precede asserting the pin to enable VCN13.
  if (!UpdatePin(Io, PinOutput, 0) || !UpdatePin(Io, PinMode, 0) ||
      !UpdatePin(Io, PinDirection, PinMask[PinDirection]) ||
      !UpdatePmic(Io, Vs2Address[Vs2VoteVoltage], 0xffffU, Vs2Value[Vs2VoteVoltage]) ||
      !UpdatePmic(Io, Vs2Address[Vs2Elr0], 0xffffU, Vs2Value[Vs2Elr0]) ||
      !SetVs2Vote(Io, 1) ||
      !UpdatePmic(Io, Vs2Address[Vs2Con1], 0xffffU, Vs2Value[Vs2Con1]))
    return MtkConnPowerIoError;
  if (!ReadPmic(Io, VS2_STATUS, &Live)) return MtkConnPowerIoError;
  if (!(Live & 1U)) return MtkConnPowerNotReady;
  return UpdatePin(Io, PinOutput, PinMask[PinOutput]) ? MtkConnPowerOk : MtkConnPowerIoError;
}

static MTK_CONN_POWER_STATUS ReleaseVcn13(MTK_CONN_RAILS *Rails)
{
  const MTK_CONN_RAIL_IO *Io = &Rails->Io;
  if (!UpdatePin(Io, PinOutput, 0) || !SetVs2Vote(Io, 0) ||
      !UpdatePmic(Io, Vs2Address[Vs2Elr0], 0xffffU, Rails->SavedVs2[Vs2Elr0]) ||
      !UpdatePmic(Io, Vs2Address[Vs2Con1], 0xffffU, Rails->SavedVs2[Vs2Con1]) ||
      !UpdatePmic(Io, Vs2Address[Vs2VoteVoltage], 0xffffU, Rails->SavedVs2[Vs2VoteVoltage]) ||
      !UpdatePin(Io, PinMode, Rails->SavedPin[PinMode]) ||
      !UpdatePin(Io, PinDirection, Rails->SavedPin[PinDirection]) ||
      !UpdatePin(Io, PinOutput, Rails->SavedPin[PinOutput]))
    return MtkConnPowerIoError;
  Rails->Vcn13Captured = 0;
  return MtkConnPowerOk;
}

static MTK_CONN_POWER_STATUS AcquireLdo(const MTK_CONN_RAIL_IO *Io, const CONN_LDO *Ldo,
                                       MTK_CONN_LDO_STATE *Saved, int ConfigureOps)
{
  unsigned Enable, OpEnable = 0, OpConfig = 0, Voltage, Status, Other = 0;
  if (!ReadPmic(Io, Ldo->Enable, &Enable) ||
      (ConfigureOps && (!ReadPmic(Io, Ldo->OpEnable, &OpEnable) ||
                        !ReadPmic(Io, Ldo->OpConfig, &OpConfig))) ||
      !ReadPmic(Io, Ldo->Voltage, &Voltage) ||
      !ReadPmic(Io, Ldo->Status, &Status) ||
      (Ldo->OtherEnable && !ReadPmic(Io, Ldo->OtherEnable, &Other)))
    return MtkConnPowerIoError;

  Voltage &= Ldo->VoltageMask;
  if (Voltage != Ldo->TargetVoltage && (((Enable | Other) & 1U) || (Status & LDO_QI)))
    return MtkConnPowerBusy;

  *Saved = (MTK_CONN_LDO_STATE){1, Enable & 1U, OpEnable & LDO_OP_MASK,
                              OpConfig & LDO_OP_MASK, Voltage};
  // Select the required voltage while the output is off. An inherited
  // live voltage passed the check above, so this leaves its selector alone.
  if (!UpdatePmic(Io, Ldo->Voltage, Ldo->VoltageMask, Ldo->TargetVoltage) ||
      !UpdatePmic(Io, Ldo->Enable, 1U, 1U))
    return MtkConnPowerIoError;

  // Stock VCN33 calls regulator_enable before setting the operation mode.
  if (Ldo == &Vcn33 && !Saved->Enable && Io->WaitUs(Io->Context, LDO_ENABLE_US))
    return MtkConnPowerIoError;
  if (ConfigureOps && (!UpdatePmic(Io, Ldo->OpEnable, LDO_OP_MASK, LDO_OP_ENABLE) ||
                       !UpdatePmic(Io, Ldo->OpConfig, LDO_OP_MASK, LDO_OP_CONFIG)))
    return MtkConnPowerIoError;
  if (Ldo == &Vcn18 && !Saved->Enable && Io->WaitUs(Io->Context, LDO_ENABLE_US))
    return MtkConnPowerIoError;
  return WaitLdo(Io, Ldo->Status, 1);
}

static MTK_CONN_POWER_STATUS ReleaseLdo(const MTK_CONN_RAIL_IO *Io, const CONN_LDO *Ldo,
                                       MTK_CONN_LDO_STATE *Saved, int RestoreOps)
{
  unsigned Voltage, Other;
  if ((RestoreOps && (!UpdatePmic(Io, Ldo->OpConfig, LDO_OP_MASK, Saved->OpConfig) ||
                      !UpdatePmic(Io, Ldo->OpEnable, LDO_OP_MASK, Saved->OpEnable))) ||
      !UpdatePmic(Io, Ldo->Enable, 1U, Saved->Enable))
    return MtkConnPowerIoError;

  if (Saved->Voltage != Ldo->TargetVoltage) {
    if (!ReadPmic(Io, Ldo->Voltage, &Voltage)) return MtkConnPowerIoError;
    if ((Voltage & Ldo->VoltageMask) != Saved->Voltage) {
      if (Ldo->OtherEnable) {
        if (!ReadPmic(Io, Ldo->OtherEnable, &Other)) return MtkConnPowerIoError;
        if (Other & 1U) return MtkConnPowerBusy;
      }
      MTK_CONN_POWER_STATUS Status = WaitLdo(Io, Ldo->Status, 0);
      if (Status != MtkConnPowerOk) return Status;
      if (!UpdatePmic(Io, Ldo->Voltage, Ldo->VoltageMask, Saved->Voltage))
        return MtkConnPowerIoError;
    }
  }
  Saved->Captured = 0;
  return MtkConnPowerOk;
}

static MTK_CONN_POWER_STATUS CheckRuntimeLdo(const MTK_CONN_RAIL_IO *Io, const CONN_LDO *Ldo)
{
  unsigned Enable, Voltage, Status;
  if (!ReadPmic(Io, Ldo->Enable, &Enable) ||
      !ReadPmic(Io, Ldo->Voltage, &Voltage) ||
      !ReadPmic(Io, Ldo->Status, &Status))
    return MtkConnPowerIoError;
  return (Enable & 1U) && (Voltage & Ldo->VoltageMask) == Ldo->TargetVoltage && (Status & LDO_QI)
           ? MtkConnPowerOk : MtkConnPowerNotReady;
}

MTK_CONN_POWER_STATUS MtkConnRailsInit(MTK_CONN_RAILS *Rails, const MTK_CONN_RAIL_IO *Io)
{
  if (!Rails || !Io || !Io->ReadPmic || !Io->WritePmic || !Io->ReadGpio ||
      !Io->WriteGpio || !Io->WaitUs || !Io->Authorized)
    return MtkConnPowerInvalid;
  if (Rails->Io.Authorized) return MtkConnPowerBusy;
  Rails->Io = *Io;
  return MtkConnPowerOk;
}

int MtkConnRailsSetProbeRail(void *Context, MTK_CONN_PROBE_RAIL Rail, int Enable)
{
  MTK_CONN_RAILS *Rails = Context;
  if (!Rails || !Rails->Io.Authorized || (Enable != 0 && Enable != 1) ||
      (Rail != MtkConnProbeVcn18 && Rail != MtkConnProbeVcn13))
    return MtkConnPowerInvalid;
  if (Rails->RuntimeOwner) return Enable ? MtkConnPowerBusy : MtkConnPowerOk;
  if (!Enable && !(Rail == MtkConnProbeVcn18 ? Rails->Vcn18.Captured : Rails->Vcn13Captured))
    return MtkConnPowerOk;
  if (!Rails->Io.Authorized(Rails->Io.Context, Rail)) return MtkConnPowerNotReady;
  if (Rail == MtkConnProbeVcn13) return Enable ? AcquireVcn13(Rails) : ReleaseVcn13(Rails);
  if (Enable) {
    if (Rails->Vcn18.Captured) return MtkConnPowerBusy;
    MTK_CONN_POWER_STATUS Status = AcquireLdo(&Rails->Io, &Vcn18, &Rails->Vcn18, 0);
    Rails->Vcn18Ready = Status == MtkConnPowerOk;
    return Status;
  }
  if (Rails->Vcn13Captured) return MtkConnPowerBusy;
  Rails->Vcn18Ready = 0;
  return ReleaseLdo(&Rails->Io, &Vcn18, &Rails->Vcn18, 0);
}

MTK_CONN_POWER_STATUS MtkConnRailsAcquireRuntime(MTK_CONN_RAILS *Rails, unsigned Adie, const void *Owner)
{
  if (!Rails || !Rails->Io.Authorized || !Owner || (Adie != 0x6631U && Adie != 0x6635U))
    return MtkConnPowerInvalid;
  if (Rails->RuntimeOwner || Rails->Vcn18.Captured || Rails->Vcn13Captured) return MtkConnPowerBusy;

  const MTK_CONN_RAIL_IO *Io = &Rails->Io;
  MTK_CONN_RAIL Secondary = Adie == 0x6631U ? MtkConnRuntimeVcn33 : MtkConnProbeVcn13;
  if (!Io->Authorized(Io->Context, MtkConnRuntimeVcn18) || !Io->Authorized(Io->Context, Secondary))
    return MtkConnPowerNotReady;

  Rails->RuntimeOwner = Owner;
  Rails->RuntimeAdie = Adie;
  MTK_CONN_POWER_STATUS Status = AcquireLdo(Io, &Vcn18, &Rails->Vcn18, 1);
  if (Status != MtkConnPowerOk) return Status;
  Rails->Vcn18Ready = 1;
  Status = Adie == 0x6631U ? AcquireLdo(Io, &Vcn33, &Rails->Vcn33, 1) : AcquireVcn13(Rails);
  Rails->RuntimeReady = Status == MtkConnPowerOk;
  return Status;
}

MTK_CONN_POWER_STATUS MtkConnRailsCheckRuntime(const MTK_CONN_RAILS *Rails, const void *Owner)
{
  if (!Rails || !Rails->Io.Authorized || !Owner) return MtkConnPowerInvalid;
  if (!Rails->RuntimeReady) return MtkConnPowerNotReady;
  if (Rails->RuntimeOwner != Owner) return MtkConnPowerBusy;

  const MTK_CONN_RAIL_IO *Io = &Rails->Io;
  MTK_CONN_RAIL Secondary = Rails->RuntimeAdie == 0x6631U ? MtkConnRuntimeVcn33 : MtkConnProbeVcn13;
  if (!Io->Authorized(Io->Context, MtkConnRuntimeVcn18) || !Io->Authorized(Io->Context, Secondary))
    return MtkConnPowerNotReady;

  MTK_CONN_POWER_STATUS Status = CheckRuntimeLdo(Io, &Vcn18);
  if (Status != MtkConnPowerOk) return Status;
  if (Secondary == MtkConnRuntimeVcn33) return CheckRuntimeLdo(Io, &Vcn33);

  unsigned Live, Vote;
  if (!ReadPmic(Io, VS2_STATUS, &Live) || !ReadPmic(Io, VS2_VOTER, &Vote))
    return MtkConnPowerIoError;
  if (!(Live & 1U) || !(Vote & VS2_VCN13_VOTE)) return MtkConnPowerNotReady;
  for (unsigned Field = 0; Field < 3U; ++Field) {
    if (Io->ReadGpio(Io->Context, PinOffset[Field], &Live)) return MtkConnPowerIoError;
    unsigned Expected = Field == PinMode ? 0 : PinMask[Field];
    if ((Live & PinMask[Field]) != Expected) return MtkConnPowerNotReady;
  }
  return MtkConnPowerOk;
}

MTK_CONN_POWER_STATUS MtkConnRailsReleaseRuntime(MTK_CONN_RAILS *Rails, const void *Owner)
{
  if (!Rails || !Rails->Io.Authorized || !Owner) return MtkConnPowerInvalid;
  if (!Rails->RuntimeOwner) return MtkConnPowerOk;
  if (Rails->RuntimeOwner != Owner) return MtkConnPowerBusy;
  Rails->RuntimeReady = 0;

  const MTK_CONN_RAIL_IO *Io = &Rails->Io;
  MTK_CONN_POWER_STATUS Status;
  if (Rails->Vcn13Captured) {
    if (!Io->Authorized(Io->Context, MtkConnProbeVcn13)) return MtkConnPowerNotReady;
    Status = ReleaseVcn13(Rails);
    if (Status != MtkConnPowerOk) return Status;
  }
  if (Rails->Vcn33.Captured) {
    if (!Io->Authorized(Io->Context, MtkConnRuntimeVcn33)) return MtkConnPowerNotReady;
    Status = ReleaseLdo(Io, &Vcn33, &Rails->Vcn33, 1);
    if (Status != MtkConnPowerOk) return Status;
  }
  if (Rails->Vcn18.Captured) {
    if (!Io->Authorized(Io->Context, MtkConnRuntimeVcn18)) return MtkConnPowerNotReady;
    if (Io->WaitUs(Io->Context, VCN18_OFF_US)) return MtkConnPowerIoError;
    Rails->Vcn18Ready = 0;
    Status = ReleaseLdo(Io, &Vcn18, &Rails->Vcn18, 1);
    if (Status != MtkConnPowerOk) return Status;
  }
  Rails->RuntimeOwner = 0;
  Rails->RuntimeAdie = 0;
  return MtkConnPowerOk;
}
