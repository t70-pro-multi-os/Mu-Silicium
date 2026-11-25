// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <Library/MtkConnPower.h>

enum { OtherControl = 0x80001400, OtherAck = 0x40, OtherGate = 0x100,
       Ccif = 0x10000000, Protect0 = 0x46000, Protect1 = 0x400,
       EmiHigh = 0xabc00000, PeriHigh = 0x32100000 };
#define TEST_EMI_BASE 0x50000000ULL

typedef struct {
  MTK_CONN_REGION Region;
  unsigned Offset, Value;
} WRITE;

typedef struct {
  unsigned Control, StatusA, StatusB, Enable[2], Ack[2], CcifStatus;
  unsigned Remap[2], HoldRemap, DropEmiOnWait;
  unsigned AckPending[2], PowerPending, Target, Delayed;
  unsigned ReadError, FailWrite, FailWait, HoldPrimary, HoldSecondary;
  unsigned AckBlocked, HoldClock, HoldClear, HoldSet, HoldControl, IgnoreClearAck;
  unsigned Writes, Barriers, Reads, Wait10, Wait100;
  WRITE Log[48];
} MODEL;

static unsigned Checks;
#define CHECK(Condition) do { ++Checks; assert(Condition); } while (0)

static int Read(void *Context, MTK_CONN_REGION Region, unsigned Offset, unsigned *Value) {
  MODEL *Model = Context;
  ++Model->Reads;
  CHECK(Model->Writes == Model->Barriers);
  if (Model->ReadError == Offset) {
    *Value = UINT_MAX;
    return 0;
  }
  if (Region == MtkConnSpm) {
    switch (Offset) {
    case 0x304: *Value = Model->Control; return 0;
    case 0x16c: *Value = Model->StatusA; return 0;
    case 0x170: *Value = Model->StatusB; return 0;
    }
  } else {
    CHECK(Region == MtkConnInfra);
    switch (Offset) {
    case 0x220: *Value = Model->Enable[0]; return 0;
    case 0x228: *Value = Model->Ack[0]; return 0;
    case 0x250: *Value = Model->Enable[1]; return 0;
    case 0x258: *Value = Model->Ack[1]; return 0;
    case 0x0c8: *Value = Model->CcifStatus; return 0;
    case 0x380: *Value = Model->Remap[0]; return 0;
    case 0x388: *Value = Model->Remap[1]; return 0;
    }
  }
  CHECK(0);
  return 1;
}

static void UpdateStatus(MODEL *Model) {
  if (!Model->HoldPrimary) Model->StatusA = (Model->StatusA & ~2U) | Model->Target;
  if (!Model->HoldSecondary) Model->StatusB = (Model->StatusB & ~2U) | Model->Target;
  Model->PowerPending = 0;
}

static int Write(void *Context, MTK_CONN_REGION Region, unsigned Offset, unsigned Value) {
  MODEL *Model = Context;
  unsigned Bank;
  CHECK(Model->Writes == Model->Barriers && Model->Writes < sizeof(Model->Log) / sizeof(WRITE));
  Model->Log[Model->Writes++] = (WRITE){Region, Offset, Value};
  if (Region == MtkConnSpm) {
    CHECK(Offset == 0x304 && (Value & ~0x1fU) == OtherControl);
    if ((Value & 0x0cU) && !(Model->Control & 0x0cU)) {
      CHECK((Model->Remap[0] & 0xfffffU) == (TEST_EMI_BASE >> 16) ||
            (Model->Remap[0] & 0xfffffU) == 0x7f00U);
      CHECK((Model->Remap[1] & 0xfffffU) == 0x1000U);
    }
    // Never isolate a live domain before every protection has acknowledged.
    if ((Value & 2U) && !(Model->Control & 2U)) {
      CHECK((Model->Ack[0] & Protect0) == Protect0 && (Model->Ack[1] & Protect1) == Protect1);
    }
    if ((Value & 1U) && !(Model->Control & 1U))
      CHECK((Model->StatusA & Model->StatusB & 2U) == 2U && Model->Wait100);
    Model->Control = (Value & ~Model->HoldControl) | (Model->Control & Model->HoldControl);
    Model->Target = (Model->Control & 0x0c) == 0x0c ? 2U : 0;
    Model->PowerPending = 1;
    if (!Model->Delayed) UpdateStatus(Model);
  } else {
    CHECK(Region == MtkConnInfra);
    switch (Offset) {
    case 0x380:
    case 0x388:
      Bank = Offset == 0x388;
      CHECK(!((Model->StatusA | Model->StatusB) & 2U) && (Model->Control & 0x1fU) == 0x12U);
      CHECK((Model->Enable[0] & Protect0) == Protect0 &&
            (Model->Enable[1] & Protect1) == Protect1);
      CHECK((Model->Remap[Bank] & 0xfff00000U) == (Value & 0xfff00000U));
      if (!(Model->HoldRemap & (1U << Bank))) Model->Remap[Bank] = Value;
      break;
    case 0x0c0:
    case 0x0c4:
      CHECK(Value == Ccif);
      if (Offset == 0x0c0) CHECK(((Model->StatusA | Model->StatusB) & 2U) == 0);
      if (!Model->HoldClock) Model->CcifStatus = OtherGate | (Offset == 0x0c0 ? Ccif : 0);
      break;
    case 0x2a0:
    case 0x2a8:
      Bank = Offset == 0x2a8;
      CHECK(!(Value & ~(Bank ? Protect1 : Protect0)));
      Model->Enable[Bank] |= Value & ~Model->HoldSet;
      if (Model->Delayed) Model->AckPending[Bank] |= Value & ~Model->AckBlocked;
      else Model->Ack[Bank] |= Value & ~Model->AckBlocked;
      break;
    case 0x2a4:
    case 0x2ac:
      Bank = Offset == 0x2ac;
      CHECK(!(Value & ~(Bank ? Protect1 : Protect0)));
      CHECK((Model->Control & 0x1fU) == 0x0d && !((Model->CcifStatus) & Ccif));
      if (!Model->HoldClear) Model->Enable[Bank] &= ~Value;
      if (!Model->IgnoreClearAck) Model->Ack[Bank] &= ~Value;
      break;
    default:
      CHECK(0);
    }
  }
  // A failed write may still have reached hardware.
  return Model->Writes == Model->FailWrite;
}

static int Wait(void *Context, unsigned Us) {
  MODEL *Model = Context;
  CHECK(Model->Writes == Model->Barriers);
  CHECK(Us == 10 || Us == 100);
  if (Us == 10) ++Model->Wait10;
  else ++Model->Wait100;
  if (Us == 100 && Model->DropEmiOnWait) Model->Remap[0] ^= 1U;
  if (Model->PowerPending) UpdateStatus(Model);
  for (unsigned Bank = 0; Bank < 2; ++Bank) {
    Model->Ack[Bank] |= Model->AckPending[Bank];
    Model->AckPending[Bank] = 0;
  }
  return Model->FailWait != 0;
}

static void Barrier(void *Context) {
  MODEL *Model = Context;
  CHECK(Model->Writes == ++Model->Barriers);
}

static void Init(MTK_CONN_POWER *Power, MODEL *Model, int ClockGated) {
  const MTK_CONN_POWER_IO Io = {Model, Read, Write, Wait, Barrier};
  memset(Model, 0, sizeof(*Model));
  Model->Control = OtherControl | 0x12;
  Model->CcifStatus = OtherGate | (ClockGated ? Ccif : 0);
  Model->Remap[0] = EmiHigh | (unsigned)(TEST_EMI_BASE >> 16);
  Model->Remap[1] = PeriHigh | 0x1000U;
  Model->Enable[0] = Model->Ack[0] = Protect0 | OtherAck;
  Model->Enable[1] = Model->Ack[1] = Protect1 | OtherAck;
  CHECK(MtkConnPowerInit(Power, &Io) == MtkConnPowerOk);
  CHECK(MtkConnPowerCheck(Power) == MtkConnPowerNotReady && !Model->Reads);
}

static void CheckOff(const MTK_CONN_POWER *Power, const MODEL *Model, int ClockGated) {
  CHECK(Power->State == MtkConnOff && !Power->RestoreCcifGate &&
        !Power->IslandOwned && !Power->RemapOwned);
  CHECK(!((Model->StatusA | Model->StatusB) & 2U) && Model->Control == (OtherControl | 0x12));
  CHECK(Model->Enable[0] == (Protect0 | OtherAck) && Model->Enable[1] == (Protect1 | OtherAck));
  CHECK(Model->Ack[0] == (Protect0 | OtherAck) && Model->Ack[1] == (Protect1 | OtherAck));
  CHECK(Model->CcifStatus == (OtherGate | (ClockGated ? Ccif : 0)));
}

static void TestSequence(void) {
  const WRITE Expected[] = {
    {MtkConnInfra, 0x0c4, Ccif},
    {MtkConnSpm, 0x304, OtherControl | 0x16}, {MtkConnSpm, 0x304, OtherControl | 0x1e},
    {MtkConnSpm, 0x304, OtherControl | 0x0e}, {MtkConnSpm, 0x304, OtherControl | 0x0c},
    {MtkConnSpm, 0x304, OtherControl | 0x0d},
    {MtkConnInfra, 0x2ac, 0x00400}, {MtkConnInfra, 0x2a4, 0x04000},
    {MtkConnInfra, 0x2a4, 0x42000},
    {MtkConnInfra, 0x2a0, 0x42000}, {MtkConnInfra, 0x2a0, 0x04000},
    {MtkConnInfra, 0x2a8, 0x00400},
    {MtkConnSpm, 0x304, OtherControl | 0x0f}, {MtkConnSpm, 0x304, OtherControl | 0x0e},
    {MtkConnSpm, 0x304, OtherControl | 0x1e}, {MtkConnSpm, 0x304, OtherControl | 0x1a},
    {MtkConnSpm, 0x304, OtherControl | 0x12},
    {MtkConnInfra, 0x0c0, Ccif}
  };
  MTK_CONN_POWER Power;
  MODEL Model;
  Init(&Power, &Model, 1);
  Model.Delayed = 1;
  Model.IgnoreClearAck = 1;
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerOk && Power.State == MtkConnOn);
  CHECK(Model.Wait10 == 1 && Model.Wait100 == 1 && Model.Writes == 9);
  CHECK(Model.Ack[0] == (Protect0 | OtherAck) && Model.Ack[1] == (Protect1 | OtherAck));
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerBusy && Model.Writes == 9);
  CHECK(MtkConnPowerCheck(&Power) == MtkConnPowerOk && Model.Writes == 9);
  Model.CcifStatus |= Ccif;
  CHECK(MtkConnPowerCheck(&Power) == MtkConnPowerNotReady && Model.Writes == 9);
  Model.CcifStatus &= ~Ccif;
  Model.Ack[0] = Model.Ack[1] = OtherAck;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk && Model.Wait10 == 5);
  CheckOff(&Power, &Model, 1);
  CHECK(Model.Writes == sizeof(Expected) / sizeof(Expected[0]));
  for (unsigned Index = 0; Index < Model.Writes; ++Index) {
    CHECK(Model.Log[Index].Region == Expected[Index].Region);
    CHECK(Model.Log[Index].Offset == Expected[Index].Offset);
    CHECK(Model.Log[Index].Value == Expected[Index].Value);
  }
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk && Model.Writes == 18);

  Init(&Power, &Model, 0);
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerOk && Model.Writes == 8);
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk && Model.Writes == 16);
  CheckOff(&Power, &Model, 0);
}

static void TestFailedWrites(void) {
  MTK_CONN_POWER Power;
  MODEL Model;
  for (unsigned Step = 1; Step <= 18; ++Step) {
    Init(&Power, &Model, 1);
    Model.FailWrite = Step;
    if (Step <= 9) CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerIoError);
    else {
      CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerOk);
      CHECK(MtkConnPowerStop(&Power) == MtkConnPowerIoError);
    }
    CHECK(Power.State == MtkConnFault && Model.Writes == Step && Model.Barriers == Step);
    CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerBusy && Model.Writes == Step);
    CHECK(MtkConnPowerCheck(&Power) == MtkConnPowerNotReady);
    CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk);
    CheckOff(&Power, &Model, 1);
  }
}

static void TestRejectedAndTimedOut(void) {
  MTK_CONN_POWER Power;
  MODEL Model;
  Init(&Power, &Model, 1);
  Model.StatusB = 2;
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerNotReady && !Model.Writes && Power.State == MtkConnOff);
  Model.StatusB = 0;
  Model.Enable[0] &= ~0x2000U;
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerNotReady && !Model.Writes);
  Model.Enable[0] |= 0x2000U;
  Model.ReadError = 0x0c8;
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerIoError && !Model.Writes);

  Init(&Power, &Model, 1);
  Model.HoldClock = 1;
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerTimeout && Model.Writes == 1 && Model.Wait10 == 100000);
  Model.HoldClock = 0;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk);
  CheckOff(&Power, &Model, 1);

  Init(&Power, &Model, 1);
  Model.HoldSecondary = 1;
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerTimeout && Model.Writes == 3 && !Model.Wait100);
  Model.HoldSecondary = 0;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk);
  CheckOff(&Power, &Model, 1);

  Init(&Power, &Model, 1);
  Model.FailWait = 1;
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerIoError && Model.Writes == 3 && Model.Wait100 == 1);
  Model.FailWait = 0;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk);
  CheckOff(&Power, &Model, 1);

  Init(&Power, &Model, 1);
  Model.HoldClear = 1;
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerTimeout && Model.Writes == 7 && Model.Wait10 == 100000);
  Model.HoldClear = 0;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk);
  CheckOff(&Power, &Model, 1);

  Init(&Power, &Model, 1);
  Model.HoldControl = 2;
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerNotReady && Model.Writes == 6);
  CHECK(Model.Enable[0] == (Protect0 | OtherAck) && Model.Enable[1] == (Protect1 | OtherAck));
  Model.HoldControl = 0;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk);
  CheckOff(&Power, &Model, 1);

  Init(&Power, &Model, 1);
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerOk);
  Model.HoldSet = 0x2000;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerTimeout && Model.Writes == 10 &&
        (Model.Control & 0x1fU) == 0x0d);
  Model.HoldSet = 0;
  Model.HoldControl = 0x10;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerNotReady && Model.Writes == 16 &&
        (Model.Control & 0x0cU) == 0x0cU);
  Model.HoldControl = 0;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk);
  CheckOff(&Power, &Model, 1);

  for (unsigned Step = 0; Step < 3; ++Step) {
    const unsigned Ack[] = {0x2000, 0x4000, 0x0400};
    Init(&Power, &Model, 1);
    CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerOk);
    Model.AckBlocked = Ack[Step];
    CHECK(MtkConnPowerStop(&Power) == MtkConnPowerTimeout && Model.Writes == 10 + Step &&
          (Model.Control & 0x1fU) == 0x0d && !(Model.CcifStatus & Ccif));
    Model.AckBlocked = 0;
    CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk);
    CheckOff(&Power, &Model, 1);
  }
  Init(&Power, &Model, 1);
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerOk);
  Model.HoldPrimary = 1;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerTimeout && Model.Writes == 17 &&
        !(Model.CcifStatus & Ccif));
  Model.HoldPrimary = 0;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk);
  CheckOff(&Power, &Model, 1);
}

static void TestEmi(void) {
  const unsigned long long InvalidBase[] = {
    0, TEST_EMI_BASE + 1, MTK_CONN_EMI_LIMIT, 1ULL << 40, ~0ULL
  };
  const unsigned OldEmi = EmiHigh | 0x1234, OldPeri = PeriHigh | 0x2345;
  MTK_CONN_POWER Power;
  MODEL Model;
  Init(&Power, &Model, 1);
  for (unsigned Index = 0; Index < sizeof(InvalidBase) / sizeof(InvalidBase[0]); ++Index)
    CHECK(MtkConnPowerStart(&Power, InvalidBase[Index], MTK_CONN_EMI_SIZE) == MtkConnPowerInvalid);
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE + 1) == MtkConnPowerInvalid);
  CHECK(!Model.Reads && !Model.Writes);
  Model.ReadError = 0x388;
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerIoError);
  CHECK(!Model.Writes && Power.State == MtkConnOff);

  Init(&Power, &Model, 1);
  Model.Remap[0] = OldEmi;
  Model.Remap[1] = OldPeri;
  CHECK(MtkConnPowerStart(&Power, MTK_CONN_EMI_LIMIT - MTK_CONN_EMI_ALIGN,
                          MTK_CONN_EMI_SIZE) == MtkConnPowerOk);
  CHECK(Model.Log[0].Offset == 0x380 && Model.Log[0].Value == (EmiHigh | 0x7f00));
  CHECK(Model.Log[1].Offset == 0x388 && Model.Log[1].Value == (PeriHigh | 0x1000));
  CHECK(Model.Log[2].Offset == 0x0c4 && Model.Writes == 11);
  Model.Remap[0] ^= 1U;
  CHECK(MtkConnPowerCheck(&Power) == MtkConnPowerNotReady && Model.Writes == 11);
  Model.Remap[1] ^= 1U << 28;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk && Model.Writes == 22);
  CHECK(Model.Log[20].Offset == 0x388 && Model.Log[21].Offset == 0x380);
  CHECK(Model.Remap[0] == OldEmi && Model.Remap[1] == (OldPeri ^ (1U << 28)));
  CheckOff(&Power, &Model, 1);

  Init(&Power, &Model, 1);
  Model.Remap[0] = OldEmi;
  Model.Remap[1] = OldPeri;
  Model.HoldRemap = 2;
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerNotReady);
  CHECK(Model.Writes == 2 && !Power.IslandOwned && Power.State == MtkConnFault);
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk && Model.Writes == 3);
  CHECK(Model.Remap[0] == OldEmi && Model.Remap[1] == OldPeri);
  CheckOff(&Power, &Model, 1);

  Init(&Power, &Model, 1);
  Model.Remap[0] = OldEmi;
  Model.DropEmiOnWait = 1;
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerNotReady);
  CHECK(Model.Writes == 4 && (Model.Control & 0x1fU) == 0x1eU);
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk && Model.Remap[0] == OldEmi);
  CheckOff(&Power, &Model, 1);

  for (unsigned Index = 0; Index < 4; ++Index) {
    const unsigned Step = Index < 2 ? Index + 1 : Index + 19;
    Init(&Power, &Model, 1);
    Model.Remap[0] = OldEmi;
    Model.Remap[1] = OldPeri;
    Model.FailWrite = Step;
    if (Index < 2)
      CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerIoError);
    else {
      CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerOk);
      CHECK(MtkConnPowerStop(&Power) == MtkConnPowerIoError);
    }
    CHECK(!Power.IslandOwned && Power.State == MtkConnFault && Model.Writes == Step);
    CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk);
    CHECK(Model.Remap[0] == OldEmi && Model.Remap[1] == OldPeri);
    CheckOff(&Power, &Model, 1);
  }

  Init(&Power, &Model, 1);
  Model.Remap[0] = OldEmi;
  CHECK(MtkConnPowerStart(&Power, TEST_EMI_BASE, MTK_CONN_EMI_SIZE) == MtkConnPowerOk);
  Model.HoldSecondary = 1;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerTimeout && Model.Writes == 18);
  CHECK(Power.IslandOwned && Power.RemapOwned && Model.Remap[0] == (EmiHigh | 0x5000));
  Model.HoldSecondary = 0;
  CHECK(MtkConnPowerStop(&Power) == MtkConnPowerOk && Model.Remap[0] == OldEmi);
  CheckOff(&Power, &Model, 1);
}

int main(void) {
  TestSequence();
  TestFailedWrites();
  TestRejectedAndTimedOut();
  TestEmi();
  printf("MT6789 CONN power: %u checks passed with mocked SPM and INFRACFG\n", Checks);
  return 0;
}
