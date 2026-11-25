// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkConnRails.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {
  Vcn18Enable, Vcn18Status, Vs2Status, Voter, VoteVoltage, Elr0, Con1,
  Vcn18OpEn, Vcn18OpCfg, Vcn18Voltage,
  Vcn33Enable, Vcn33OpEn, Vcn33OpCfg, Vcn33Voltage, Vcn33Wifi, Vcn33Status, PmicCount
};
enum { Mode, Direction, Output, PinCount };

static const unsigned PmicAddress[] = {
  0x1c58, 0x1c66, 0x1720, 0x1724, 0x172a, 0x172e, 0x170a, 0x1c5a, 0x1c60, 0x1e8c,
  0x1d1c, 0x1d1e, 0x1d24, 0x1e28, 0x1d2a, 0x1d2c
};
static const unsigned PmicInitial[] = {
  0x4000, 0, 1, 0x81, 0x35, 0, 0x43, 0x4800, 0x2441, 0xac50,
  0x4100, 0x8040, 0x2111, 0x5203, 0x40, 0
};
static const unsigned PinOffset[] = {0x380, 0x20, 0x120};
static const unsigned PinInitial[] = {0x53051234, 0x404, 0x6210};

typedef struct {
  unsigned Pmic[PmicCount], Pin[PinCount];
  unsigned Ops, FailAt, ApplyFailedWrite, IgnoredWrite, Us, NeverReady, Authorized;
  unsigned NeverVcn33Ready, Vcn33ReadyAt, Vcn33OffAt, SecondaryOffAt, Runtime;
  unsigned Writes, WriteAddress[64];
} MODEL;

static unsigned Checks;
#define CHECK(Condition) do { ++Checks; assert(Condition); } while (0)

static unsigned RegIndex(const unsigned *Offsets, unsigned Count, unsigned Address)
{
  for (unsigned Index = 0; Index < Count; ++Index)
    if (Offsets[Index] == Address) return Index;
  CHECK(0);
  return 0;
}

static int Fails(MODEL *Model)
{
  return ++Model->Ops == Model->FailAt;
}

static int ReadPmic(void *Context, unsigned Address, unsigned *Value)
{
  MODEL *Model = Context;
  unsigned Index = RegIndex(PmicAddress, PmicCount, Address);
  if (Fails(Model)) return 1;
  *Value = Model->Pmic[Index];
  if (Index == Vcn18Status)
    *Value = (Model->Pmic[Vcn18Enable] & 1U) && Model->Us >= 290U && !Model->NeverReady ? 0x8000U : 0;
  if (Index == Vcn33Status && !Model->NeverVcn33Ready &&
      (((Model->Pmic[Vcn33Enable] & 1U) && Model->Us >= Model->Vcn33ReadyAt) ||
       (Model->Pmic[Vcn33Wifi] & 1U) || Model->Us < Model->Vcn33OffAt))
    *Value |= 0x8000U;
  return 0;
}

static int IsVcn13On(const MODEL *Model)
{
  return !(Model->Pin[Mode] & 0xf0000U) && (Model->Pin[Direction] & 0x10U) &&
         (Model->Pin[Output] & 0x10U);
}

static int LogWrite(MODEL *Model, unsigned Address)
{
  CHECK(Model->Writes < sizeof(Model->WriteAddress) / sizeof(Model->WriteAddress[0]));
  Model->WriteAddress[Model->Writes++] = Address;
  return Fails(Model);
}

static int WritePmic(void *Context, unsigned Address, unsigned Value)
{
  MODEL *Model = Context;
  int Failed = LogWrite(Model, Address);
  CHECK(!IsVcn13On(Model));
  if ((Failed && !Model->ApplyFailedWrite) || Model->IgnoredWrite == Model->Writes)
    return Failed;
  if (Address == 0x1726U || Address == 0x1728U) {
    CHECK(Value == 0x10U);
    if (Address == 0x1726U) Model->Pmic[Voter] |= Value;
    else Model->Pmic[Voter] &= ~Value;
    return Failed;
  }
  unsigned Index = RegIndex(PmicAddress, PmicCount, Address);
  CHECK(Index != Vcn18Status && Index != Vs2Status && Index != Voter &&
        Index != Vcn33Wifi && Index != Vcn33Status);
  CHECK(Value <= 0xffffU);
  if (Index == Vcn18Enable || Index == Vcn33Enable) {
    CHECK((Value & ~1U) == (Model->Pmic[Index] & ~1U));
    if (Index == Vcn18Enable) {
      if (Value & 1U) CHECK((Model->Pmic[Vcn18Voltage] & 0xf0fU) == 0xc00U);
      if (!(Value & 1U)) {
        CHECK(!(Model->Pmic[Voter] & 0x10U));
        if (Model->Runtime) {
          CHECK(!(Model->Pmic[Vcn33Enable] & 1U));
          CHECK(Model->Us - Model->SecondaryOffAt >= 300U);
        }
      }
    } else if (Value & 1U) {
      CHECK((Model->Pmic[Vcn33Voltage] & 0x30fU) == 0x100U);
      Model->Vcn33ReadyAt = Model->Us + 290U;
    } else {
      if (!Model->NeverVcn33Ready && Model->Us >= Model->Vcn33ReadyAt)
        Model->Vcn33OffAt = Model->Us + 20U;
      Model->SecondaryOffAt = Model->Us;
    }
  }
  if (Index == Vcn18Voltage || Index == Vcn33Voltage) {
    unsigned Mask = Index == Vcn18Voltage ? 0xf0fU : 0x30fU;
    CHECK((Value & ~Mask) == (Model->Pmic[Index] & ~Mask));
    if (Index == Vcn18Voltage) CHECK(!(Model->Pmic[Vcn18Enable] & 1U));
    else CHECK(!((Model->Pmic[Vcn33Enable] | Model->Pmic[Vcn33Wifi]) & 1U) &&
               !(Model->Pmic[Vcn33Status] & 0x8000U) && Model->Us >= Model->Vcn33OffAt);
  }
  if (Index == Vcn18OpEn || Index == Vcn18OpCfg || Index == Vcn33OpEn || Index == Vcn33OpCfg)
    CHECK((Value & ~3U) == (Model->Pmic[Index] & ~3U));
  Model->Pmic[Index] = Value;
  return Failed;
}

static int ReadPin(void *Context, unsigned Offset, unsigned *Value)
{
  MODEL *Model = Context;
  if (Fails(Model)) return 1;
  *Value = Model->Pin[RegIndex(PinOffset, PinCount, Offset)];
  return 0;
}

static int WritePin(void *Context, unsigned Offset, unsigned Value)
{
  MODEL *Model = Context;
  unsigned Index = RegIndex(PinOffset, PinCount, Offset);
  unsigned Mask = Index == Mode ? 0xf0000U : 0x10U;
  int Failed = LogWrite(Model, Offset);
  CHECK((Value & ~Mask) == (Model->Pin[Index] & ~Mask));
  if ((Failed && !Model->ApplyFailedWrite) || Model->IgnoredWrite == Model->Writes)
    return Failed;
  int WasOn = IsVcn13On(Model);
  Model->Pin[Index] = Value;
  if (WasOn && !IsVcn13On(Model)) Model->SecondaryOffAt = Model->Us;
  if (IsVcn13On(Model)) {
    CHECK(Model->Pmic[VoteVoltage] == 0x46U && Model->Pmic[Elr0] == 0x4dU && Model->Pmic[Con1] == 0x46U);
    CHECK((Model->Pmic[Voter] & 0x10U) && (Model->Pmic[Vcn18Enable] & 1U) && (Model->Pmic[Vs2Status] & 1U));
  }
  return Failed;
}

static int Wait(void *Context, unsigned Us)
{
  MODEL *Model = Context;
  CHECK(Us == 270U || Us == 10U || (Us == 300U && Model->Runtime));
  if (Fails(Model)) return 1;
  Model->Us += Us;
  return 0;
}

static int Authorized(void *Context, MTK_CONN_RAIL Rail)
{
  MODEL *Model = Context;
  return Model->Authorized & (1U << Rail);
}

static void Reset(MODEL *Model, MTK_CONN_RAILS *Rails)
{
  const MTK_CONN_RAIL_IO Io = {Model, ReadPmic, WritePmic, ReadPin, WritePin, Wait, Authorized};
  *Model = (MODEL){.Authorized = 3U};
  *Rails = (MTK_CONN_RAILS){0};
  memcpy(Model->Pmic, PmicInitial, sizeof(PmicInitial));
  memcpy(Model->Pin, PinInitial, sizeof(PinInitial));
  CHECK(MtkConnRailsInit(Rails, &Io) == MtkConnPowerOk && !Model->Ops);
}

static int PowerUp(const MTK_CONN_PROBE_IO *Probe)
{
  int Status = Probe->SetRail(Probe->Context, MtkConnProbeVcn18, 1);
  return Status ? Status : Probe->SetRail(Probe->Context, MtkConnProbeVcn13, 1);
}

static int PowerDown(const MTK_CONN_PROBE_IO *Probe)
{
  int Status = Probe->SetRail(Probe->Context, MtkConnProbeVcn13, 0);
  return Status ? Status : Probe->SetRail(Probe->Context, MtkConnProbeVcn18, 0);
}

static void CheckRestored(const MODEL *Model, const MTK_CONN_RAILS *Rails)
{
  CHECK(!Rails->Vcn13Captured && !Rails->Vcn18.Captured && !Rails->Vcn18Ready);
  CHECK(!Rails->Vcn33.Captured && !Rails->RuntimeReady && !Rails->RuntimeOwner);
  CHECK(memcmp(Model->Pmic, PmicInitial, sizeof(PmicInitial)) == 0);
  CHECK(memcmp(Model->Pin, PinInitial, sizeof(PinInitial)) == 0);
}

static void ResetRuntime(MODEL *Model, MTK_CONN_RAILS *Rails)
{
  Reset(Model, Rails);
  Model->Authorized = 0xfU;
  Model->Runtime = 1;
}

static void TestRuntime(unsigned Adie)
{
  static const unsigned Vcn33WriteOrder[] = {
    0x1c58, 0x1c5a, 0x1c60, 0x1e28, 0x1d1c, 0x1d1e, 0x1d24,
    0x1d24, 0x1d1e, 0x1d1c, 0x1e28, 0x1c60, 0x1c5a, 0x1c58
  };
  static const unsigned Vcn13WriteOrder[] = {
    0x1c58, 0x1c5a, 0x1c60, 0x120, 0x380, 0x20, 0x172a, 0x172e, 0x1726, 0x170a, 0x120,
    0x120, 0x1728, 0x172e, 0x170a, 0x172a, 0x380, 0x20, 0x120, 0x1c60, 0x1c5a, 0x1c58
  };
  MODEL Model;
  MTK_CONN_RAILS Rails;
  const void *Owner = &Model;
  const void *Other = &Rails;
  int UseVcn33 = Adie == 0x6631U;

  ResetRuntime(&Model, &Rails);
  CHECK(MtkConnRailsAcquireRuntime(&Rails, Adie, Owner) == MtkConnPowerOk);
  CHECK(Model.Us == (UseVcn33 ? 580U : 290U));
  unsigned StartOps = Model.Ops, StartWrites = Model.Writes;
  CHECK(MtkConnRailsAcquireRuntime(&Rails, Adie, Other) == MtkConnPowerBusy);
  CHECK(MtkConnRailsReleaseRuntime(&Rails, Other) == MtkConnPowerBusy);
  CHECK(MtkConnRailsSetProbeRail(&Rails, MtkConnProbeVcn18, 1) == MtkConnPowerBusy);
  CHECK(MtkConnRailsSetProbeRail(&Rails, MtkConnProbeVcn13, 0) == MtkConnPowerOk);
  CHECK(Model.Ops == StartOps);
  CHECK(MtkConnRailsReleaseRuntime(&Rails, Owner) == MtkConnPowerOk);
  unsigned TotalOps = Model.Ops;
  unsigned OrderBytes = UseVcn33 ? sizeof(Vcn33WriteOrder) : sizeof(Vcn13WriteOrder);
  CHECK(Model.Writes * sizeof(unsigned) == OrderBytes);
  CHECK(memcmp(Model.WriteAddress, UseVcn33 ? Vcn33WriteOrder : Vcn13WriteOrder, OrderBytes) == 0);
  CheckRestored(&Model, &Rails);
  CHECK(MtkConnRailsReleaseRuntime(&Rails, Owner) == MtkConnPowerOk && Model.Ops == TotalOps);

  for (unsigned Delivered = 0; Delivered < 2U; ++Delivered) {
    for (unsigned FailAt = 1; FailAt <= TotalOps; ++FailAt) {
      ResetRuntime(&Model, &Rails);
      Model.FailAt = FailAt;
      Model.ApplyFailedWrite = Delivered;
      MTK_CONN_POWER_STATUS Status = MtkConnRailsAcquireRuntime(&Rails, Adie, Owner);
      if (FailAt <= StartOps) CHECK(Status != MtkConnPowerOk);
      else {
        CHECK(Status == MtkConnPowerOk);
        CHECK(MtkConnRailsReleaseRuntime(&Rails, Owner) != MtkConnPowerOk && Rails.RuntimeOwner == Owner);
      }
      CHECK(MtkConnRailsReleaseRuntime(&Rails, Owner) == MtkConnPowerOk);
      CheckRestored(&Model, &Rails);
    }
  }
  for (unsigned Ignored = 1; Ignored <= StartWrites; ++Ignored) {
    ResetRuntime(&Model, &Rails);
    Model.IgnoredWrite = Ignored;
    CHECK(MtkConnRailsAcquireRuntime(&Rails, Adie, Owner) != MtkConnPowerOk);
    CHECK(MtkConnRailsReleaseRuntime(&Rails, Owner) == MtkConnPowerOk);
    CheckRestored(&Model, &Rails);
  }

  ResetRuntime(&Model, &Rails);
  Model.NeverReady = !UseVcn33;
  Model.NeverVcn33Ready = UseVcn33;
  CHECK(MtkConnRailsAcquireRuntime(&Rails, Adie, Owner) == MtkConnPowerTimeout);
  CHECK(MtkConnRailsCheckRuntime(&Rails, Owner) == MtkConnPowerNotReady);
  CHECK(MtkConnRailsReleaseRuntime(&Rails, Owner) == MtkConnPowerOk);
  CheckRestored(&Model, &Rails);

  ResetRuntime(&Model, &Rails);
  CHECK(MtkConnRailsAcquireRuntime(&Rails, Adie, Owner) == MtkConnPowerOk);
  CHECK(MtkConnRailsCheckRuntime(&Rails, Owner) == MtkConnPowerOk);
  CHECK(MtkConnRailsCheckRuntime(&Rails, Other) == MtkConnPowerBusy);
  if (UseVcn33) Model.NeverVcn33Ready = 1;
  else Model.Pmic[Vs2Status] = 0;
  CHECK(MtkConnRailsCheckRuntime(&Rails, Owner) == MtkConnPowerNotReady);
  if (!UseVcn33) Model.Pmic[Vs2Status] = PmicInitial[Vs2Status];
  Model.Authorized = 1U << MtkConnRuntimeVcn18;
  CHECK(MtkConnRailsReleaseRuntime(&Rails, Owner) == MtkConnPowerNotReady && (Model.Pmic[Vcn18Enable] & 1U));
  Model.Authorized = 0xfU;
  CHECK(MtkConnRailsReleaseRuntime(&Rails, Owner) == MtkConnPowerOk);
  CheckRestored(&Model, &Rails);
}

static void TestSharedVoltages(void)
{
  MODEL Model;
  MTK_CONN_RAILS Rails;
  Reset(&Model, &Rails);
  Model.Pmic[Vcn18Voltage] = 0xa850;
  CHECK(MtkConnRailsSetProbeRail(&Rails, MtkConnProbeVcn18, 1) == MtkConnPowerOk);
  CHECK(Model.WriteAddress[0] == 0x1e8cU && Model.WriteAddress[1] == 0x1c58U);
  CHECK(MtkConnRailsSetProbeRail(&Rails, MtkConnProbeVcn18, 0) == MtkConnPowerOk);
  CHECK(Model.Pmic[Vcn18Voltage] == 0xa850U && Model.Pmic[Vcn18Enable] == PmicInitial[Vcn18Enable]);

  Reset(&Model, &Rails);
  Model.Pmic[Vcn18Enable] |= 1U;
  Model.Pmic[Vcn18Voltage] = 0xad50;
  CHECK(MtkConnRailsSetProbeRail(&Rails, MtkConnProbeVcn18, 1) == MtkConnPowerBusy && !Model.Writes);

  ResetRuntime(&Model, &Rails);
  Model.Pmic[Vcn33Wifi] |= 1U;
  CHECK(MtkConnRailsAcquireRuntime(&Rails, 0x6631, &Model) == MtkConnPowerBusy &&
        !Rails.Vcn33.Captured && Model.Writes == 3U);
  CHECK(MtkConnRailsReleaseRuntime(&Rails, &Model) == MtkConnPowerOk);
  CHECK(Model.Pmic[Vcn33Voltage] == PmicInitial[Vcn33Voltage]);

  ResetRuntime(&Model, &Rails);
  Model.Pmic[Vcn33Voltage] = 0x5100U;
  Model.Pmic[Vcn33Wifi] |= 1U;
  CHECK(MtkConnRailsAcquireRuntime(&Rails, 0x6631, &Model) == MtkConnPowerOk);
  CHECK(MtkConnRailsReleaseRuntime(&Rails, &Model) == MtkConnPowerOk);
  CHECK(Model.Pmic[Vcn33Wifi] == (PmicInitial[Vcn33Wifi] | 1U));
  CHECK(Model.Pmic[Vcn33Voltage] == 0x5100U && Model.Pmic[Vcn33Enable] == PmicInitial[Vcn33Enable]);

  ResetRuntime(&Model, &Rails);
  CHECK(MtkConnRailsAcquireRuntime(&Rails, 0x6631, &Model) == MtkConnPowerOk);
  Model.Pmic[Vcn33Wifi] |= 1U;
  CHECK(MtkConnRailsReleaseRuntime(&Rails, &Model) == MtkConnPowerBusy);
  CHECK(Rails.Vcn33.Captured && Rails.RuntimeOwner == &Model && (Model.Pmic[Vcn18Enable] & 1U));
  CHECK((Model.Pmic[Vcn33Voltage] & 0x30fU) == 0x100U);
  Model.Pmic[Vcn33Wifi] &= ~1U;
  CHECK(MtkConnRailsReleaseRuntime(&Rails, &Model) == MtkConnPowerOk);
  CheckRestored(&Model, &Rails);

  ResetRuntime(&Model, &Rails);
  Model.Authorized = 1U << MtkConnRuntimeVcn18;
  CHECK(MtkConnRailsAcquireRuntime(&Rails, 0x6631, &Model) == MtkConnPowerNotReady);
  CHECK(!Model.Ops && !Rails.RuntimeOwner);

  ResetRuntime(&Model, &Rails);
  Model.Pmic[Vcn18Enable] |= 1U;
  Model.Pmic[Vcn33Enable] |= 1U;
  Model.Pmic[Vcn33Voltage] = 0x5100U;
  Model.Us = 290U;
  CHECK(MtkConnRailsAcquireRuntime(&Rails, 0x6631, &Model) == MtkConnPowerOk);
  CHECK(MtkConnRailsReleaseRuntime(&Rails, &Model) == MtkConnPowerOk);
  CHECK(Model.Pmic[Vcn18Enable] == (PmicInitial[Vcn18Enable] | 1U));
  CHECK(Model.Pmic[Vcn33Enable] == (PmicInitial[Vcn33Enable] | 1U) && Model.Us == 590U);

  ResetRuntime(&Model, &Rails);
  CHECK(MtkConnRailsAcquireRuntime(&Rails, 0x6631, &Model) == MtkConnPowerOk);
  Model.Pmic[Vcn33Status] = 0x8000U;
  CHECK(MtkConnRailsReleaseRuntime(&Rails, &Model) == MtkConnPowerTimeout);
  CHECK(Rails.Vcn33.Captured && Rails.RuntimeOwner == &Model);
  CHECK((Model.Pmic[Vcn33Voltage] & 0x30fU) == 0x100U && (Model.Pmic[Vcn18Enable] & 1U));
  Model.Pmic[Vcn33Status] = 0;
  CHECK(MtkConnRailsReleaseRuntime(&Rails, &Model) == MtkConnPowerOk);
  CheckRestored(&Model, &Rails);
}

int main(void)
{
  static const unsigned ProbeWriteOrder[] = {
    0x1c58, 0x120, 0x380, 0x20, 0x172a, 0x172e, 0x1726, 0x170a, 0x120,
    0x120, 0x1728, 0x172e, 0x170a, 0x172a, 0x380, 0x20, 0x120, 0x1c58
  };
  MODEL Model;
  MTK_CONN_RAILS Rails;
  const MTK_CONN_PROBE_IO Probe = {&Rails, MtkConnRailsSetProbeRail};

  Reset(&Model, &Rails);
  CHECK(Probe.SetRail(&Rails, MtkConnProbeVcn13, 1) == MtkConnPowerNotReady && !Model.Ops);
  CHECK(PowerUp(&Probe) == MtkConnPowerOk && Model.Us == 290U && IsVcn13On(&Model));
  unsigned StartOps = Model.Ops, StartWrites = Model.Writes;
  CHECK(Probe.SetRail(&Rails, MtkConnProbeVcn18, 0) == MtkConnPowerBusy && Model.Ops == StartOps);
  CHECK(PowerDown(&Probe) == MtkConnPowerOk);
  unsigned FullOps = Model.Ops;
  CHECK(Model.Writes == sizeof(ProbeWriteOrder) / sizeof(ProbeWriteOrder[0]));
  CHECK(memcmp(Model.WriteAddress, ProbeWriteOrder, sizeof(ProbeWriteOrder)) == 0);
  CheckRestored(&Model, &Rails);
  CHECK(PowerDown(&Probe) == MtkConnPowerOk && Model.Ops == FullOps);

  for (unsigned Apply = 0; Apply < 2; ++Apply) {
    for (unsigned FailAt = 1; FailAt <= FullOps; ++FailAt) {
      Reset(&Model, &Rails);
      Model.FailAt = FailAt;
      Model.ApplyFailedWrite = Apply;
      if (FailAt <= StartOps) CHECK(PowerUp(&Probe) != MtkConnPowerOk);
      else {
        CHECK(PowerUp(&Probe) == MtkConnPowerOk);
        CHECK(PowerDown(&Probe) != MtkConnPowerOk);
      }
      CHECK(PowerDown(&Probe) == MtkConnPowerOk);
      CheckRestored(&Model, &Rails);
    }
  }
  for (unsigned Ignored = 1; Ignored <= StartWrites; ++Ignored) {
    Reset(&Model, &Rails);
    Model.IgnoredWrite = Ignored;
    CHECK(PowerUp(&Probe) != MtkConnPowerOk);
    CHECK(PowerDown(&Probe) == MtkConnPowerOk);
    CheckRestored(&Model, &Rails);
  }

  Reset(&Model, &Rails);
  Model.NeverReady = 1;
  CHECK(PowerUp(&Probe) == MtkConnPowerTimeout && Model.Us == 1270U);
  CHECK(PowerDown(&Probe) == MtkConnPowerOk);
  CheckRestored(&Model, &Rails);

  Reset(&Model, &Rails);
  Model.Pmic[Vcn18Enable] |= 1U;
  Model.Us = 290U;
  CHECK(PowerUp(&Probe) == MtkConnPowerOk);
  Model.Pmic[Voter] |= 0x100U;
  Model.Pin[Mode] |= 0x80U;
  Model.Authorized = 1U;
  CHECK(PowerDown(&Probe) == MtkConnPowerNotReady && Rails.Vcn13Captured && IsVcn13On(&Model));
  Model.Authorized = 3U;
  CHECK(PowerDown(&Probe) == MtkConnPowerOk);
  CHECK(Model.Pmic[Vcn18Enable] == (PmicInitial[Vcn18Enable] | 1U) && Model.Pmic[Voter] == 0x181U);
  CHECK(Model.Pin[Mode] == (PinInitial[Mode] | 0x80U) && Model.Us == 290U);

  for (unsigned Case = 0; Case < 3U; ++Case) {
    Reset(&Model, &Rails);
    if (Case == 0) Model.Pmic[Vs2Status] = 0;
    if (Case == 1) Model.Pmic[Voter] |= 0x10U;
    if (Case == 2) {
      Model.Pin[Mode] &= ~0xf0000U;
      Model.Pin[Direction] |= 0x10U;
    }
    // Establish VCN18 first, then introduce the live conflict for VCN13.
    if (Case == 2) Model.Pin[Output] &= ~0x10U;
    CHECK(Probe.SetRail(&Rails, MtkConnProbeVcn18, 1) == MtkConnPowerOk);
    if (Case == 2) Model.Pin[Output] |= 0x10U;
    CHECK(Probe.SetRail(&Rails, MtkConnProbeVcn13, 1) ==
          (Case == 0 ? MtkConnPowerNotReady : MtkConnPowerBusy));
    CHECK(!Rails.Vcn13Captured && Model.Writes == 1U);
  }

  TestRuntime(0x6631U);
  TestRuntime(0x6635U);
  TestSharedVoltages();
  printf("CONN probe and runtime rails: %u checks passed\n", Checks);
  return 0;
}
