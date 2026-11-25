// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Library/MtkUsbPhyCore.h>

#define PHY_BYTES 0x700U
#define TRACE_LIMIT 8192U
#define CHECK(Condition) do { \
  ++Checks; \
  if (!(Condition)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #Condition); \
    abort(); \
  } \
} while (0)

enum {
  EventRead = 0,
  EventWrite,
  EventNow,
  EventDelay,
  EventResolve,
  EventPrepare,
  EventEnable,
  EventDisable,
  EventUnprepare,
  EventNvmem,
  EventCount
};

typedef struct {
  uint8_t Kind;
  uint32_t Offset;
  uint32_t Value;
} FAKE_TRACE;

typedef struct {
  uint32_t Registers[PHY_BYTES / sizeof(uint32_t)];
  FAKE_TRACE Trace[TRACE_LIMIT];
  uint32_t TraceCount;
  uint32_t FailWriteAfterEffect;
  uint32_t Calls[EventCount];
  uint32_t FailAt[EventCount];
  uint32_t ClockPresentMask;
  uint32_t PreparedMask;
  uint32_t EnabledMask;
  uint32_t NvmemMask;
  uint32_t NvmemValue;
  uint32_t FmmReadCount;
  uint32_t NvmemCalls;
  uint64_t TimeNs;
  uint8_t FreezeTime;
  char NvmemName[32];
} FAKE_PHY;

static unsigned Checks;

#define REG_U2PHYACR0 0x300U
#define REG_U2PHYACR1 0x304U
#define REG_U2PHYACR4 0x320U
#define REG_U2PHYACR5 0x314U
#define REG_U2PHYACR6 0x318U
#define REG_U2PHYDCR0 0x360U
#define REG_U2PHYDTM0 0x368U
#define REG_U2PHYDTM1 0x36cU
#define REG_FMOUT     0x10cU
#define REG_FMMONR1   0x110U

static void
Trace(FAKE_PHY *Bus, uint8_t Kind, uint32_t Offset, uint32_t Value)
{
  CHECK(Bus->TraceCount < TRACE_LIMIT);
  Bus->Trace[Bus->TraceCount].Kind = Kind;
  Bus->Trace[Bus->TraceCount].Offset = Offset;
  Bus->Trace[Bus->TraceCount].Value = Value;
  ++Bus->TraceCount;
}

static MTK_USB_PHY_STATUS
FailureStatus(uint32_t Event)
{
  switch (Event) {
    case EventResolve:
    case EventPrepare:
    case EventEnable:
    case EventDisable:
    case EventUnprepare:
      return MtkUsbPhyClockError;
    case EventNow:
      return MtkUsbPhyTimeError;
    case EventDelay:
      return MtkUsbPhyDelayError;
    case EventNvmem:
      return MtkUsbPhyNvmemError;
    default:
      return MtkUsbPhyIoError;
  }
}

static int
ShouldFail(FAKE_PHY *Bus, uint32_t Event)
{
  ++Bus->Calls[Event];
  return (Bus->FailAt[Event] != 0U) &&
         (Bus->Calls[Event] == Bus->FailAt[Event]);
}

static MTK_USB_PHY_STATUS
FakeRead32(void *Context, uint32_t Offset, uint32_t *Value)
{
  FAKE_PHY *Bus = (FAKE_PHY *)Context;

  CHECK((Bus != NULL) && (Value != NULL));
  Trace(Bus, EventRead, Offset, 0U);
  if (ShouldFail(Bus, EventRead)) {
    return MtkUsbPhyIoError;
  }
  if (((Offset & 3U) != 0U) || ((Offset + 4U) > PHY_BYTES)) {
    return MtkUsbPhyIoError;
  }
  if (Offset == REG_FMMONR1) {
    ++Bus->FmmReadCount;
  }
  *Value = Bus->Registers[Offset / 4U];
  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
FakeWrite32(void *Context, uint32_t Offset, uint32_t Value)
{
  FAKE_PHY *Bus = (FAKE_PHY *)Context;

  CHECK(Bus != NULL);
  Trace(Bus, EventWrite, Offset, Value);
  int Fail = ShouldFail(Bus, EventWrite);
  if (Fail && !Bus->FailWriteAfterEffect) {
    return MtkUsbPhyIoError;
  }
  if (((Offset & 3U) != 0U) || ((Offset + 4U) > PHY_BYTES)) {
    return MtkUsbPhyIoError;
  }
  Bus->Registers[Offset / 4U] = Value;
  return Fail ? MtkUsbPhyIoError : MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
FakeNowNs(void *Context, uint64_t *Value)
{
  FAKE_PHY *Bus = (FAKE_PHY *)Context;

  CHECK((Bus != NULL) && (Value != NULL));
  Trace(Bus, EventNow, 0U, 0U);
  if (ShouldFail(Bus, EventNow)) {
    return MtkUsbPhyTimeError;
  }
  *Value = Bus->TimeNs;
  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
FakeDelayUs(void *Context, uint32_t Microseconds)
{
  FAKE_PHY *Bus = (FAKE_PHY *)Context;

  CHECK(Bus != NULL);
  Trace(Bus, EventDelay, 0U, Microseconds);
  if (ShouldFail(Bus, EventDelay)) {
    return MtkUsbPhyDelayError;
  }
  if (!Bus->FreezeTime) {
    Bus->TimeNs += (uint64_t)Microseconds * 1000ULL;
  }
  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
FakeResolveClock(
  void *Context,
  MTK_USB_PHY_CLOCK_SLOT Slot,
  uint8_t *Present
  )
{
  FAKE_PHY *Bus = (FAKE_PHY *)Context;

  CHECK((Bus != NULL) && (Present != NULL));
  Trace(Bus, EventResolve, (uint32_t)Slot, 0U);
  if (ShouldFail(Bus, EventResolve)) {
    return MtkUsbPhyClockError;
  }
  if ((uint32_t)Slot >= (uint32_t)MtkUsbPhyClockCount) {
    return MtkUsbPhyInvalid;
  }
  *Present = (uint8_t)((Bus->ClockPresentMask >> (uint32_t)Slot) & 1U);
  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
FakeClockAction(
  void *Context,
  MTK_USB_PHY_CLOCK_SLOT Slot,
  uint32_t Event,
  uint32_t *Mask,
  int Set
  )
{
  FAKE_PHY *Bus = (FAKE_PHY *)Context;
  uint32_t Bit;

  CHECK(Bus != NULL);
  Trace(Bus, (uint8_t)Event, (uint32_t)Slot, 0U);
  if (ShouldFail(Bus, Event)) {
    return FailureStatus(Event);
  }
  if ((uint32_t)Slot >= (uint32_t)MtkUsbPhyClockCount) {
    return MtkUsbPhyInvalid;
  }
  Bit = 1U << (uint32_t)Slot;
  if (Set) {
    *Mask |= Bit;
  } else {
    *Mask &= ~Bit;
  }
  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
FakeClockPrepare(void *Context, MTK_USB_PHY_CLOCK_SLOT Slot)
{
  FAKE_PHY *Bus = (FAKE_PHY *)Context;

  return FakeClockAction(
           Context, Slot, EventPrepare, &Bus->PreparedMask, 1
           );
}

static MTK_USB_PHY_STATUS
FakeClockEnable(void *Context, MTK_USB_PHY_CLOCK_SLOT Slot)
{
  FAKE_PHY *Bus = (FAKE_PHY *)Context;

  return FakeClockAction(
           Context, Slot, EventEnable, &Bus->EnabledMask, 1
           );
}

static MTK_USB_PHY_STATUS
FakeClockDisable(void *Context, MTK_USB_PHY_CLOCK_SLOT Slot)
{
  FAKE_PHY *Bus = (FAKE_PHY *)Context;

  return FakeClockAction(
           Context, Slot, EventDisable, &Bus->EnabledMask, 0
           );
}

static MTK_USB_PHY_STATUS
FakeClockUnprepare(void *Context, MTK_USB_PHY_CLOCK_SLOT Slot)
{
  FAKE_PHY *Bus = (FAKE_PHY *)Context;

  return FakeClockAction(
           Context, Slot, EventUnprepare, &Bus->PreparedMask, 0
           );
}

static MTK_USB_PHY_STATUS
FakeReadNvmemCell(
  void *Context,
  const char *Name,
  uint32_t Mask,
  uint32_t *RawValue
  )
{
  FAKE_PHY *Bus = (FAKE_PHY *)Context;
  size_t Index;

  CHECK((Bus != NULL) && (Name != NULL) && (RawValue != NULL));
  Trace(Bus, EventNvmem, Mask, 0U);
  if (ShouldFail(Bus, EventNvmem)) {
    return MtkUsbPhyNvmemError;
  }

  for (Index = 0U; (Name[Index] != '\0') &&
                   (Index + 1U < sizeof(Bus->NvmemName)); ++Index) {
    Bus->NvmemName[Index] = Name[Index];
  }
  Bus->NvmemName[Index] = '\0';
  Bus->NvmemMask = Mask;
  ++Bus->NvmemCalls;
  *RawValue = Bus->NvmemValue;
  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
FixtureInit(
  MTK_USB_PHY_CORE *Core,
  MTK_USB_PHY_IO *Io,
  MTK_USB_PHY_CONFIG *Config,
  FAKE_PHY *Bus
  )
{
  memset(Core, 0, sizeof(*Core));
  memset(Io, 0, sizeof(*Io));
  memset(Config, 0, sizeof(*Config));
  memset(Bus, 0, sizeof(*Bus));

  Io->Context = Bus;
  Io->PhyBase = MTK_USB_TPHY_T70_U2_RESOURCE_BASE;
  Io->PhyLength = PHY_BYTES;
  Io->Read32 = FakeRead32;
  Io->Write32 = FakeWrite32;
  Io->NowNs = FakeNowNs;
  Io->DelayUs = FakeDelayUs;
  Io->ResolveClock = FakeResolveClock;
  Io->ClockPrepare = FakeClockPrepare;
  Io->ClockEnable = FakeClockEnable;
  Io->ClockDisable = FakeClockDisable;
  Io->ClockUnprepare = FakeClockUnprepare;
  Io->ReadNvmemCell = FakeReadNvmemCell;

  Config->Revision = MTK_USB_PHY_CORE_REVISION;
  Config->PhyType = MTK_USB_TPHY_T70_U2_TYPE;
  Config->TphyVersion = MTK_USB_TPHY_VERSION_V2;
  Config->TuningPresent = MTK_USB_TPHY_TUNING_T70_REQUIRED;
  Config->EyeVrt = 4U;
  Config->EyeTerm = 4U;
  Config->DiscTh = 7U;
  Config->RxSqth = 5U;
  Config->Rev4 = 1U;
  Config->SetEfuseV1 = 1U;

  Bus->ClockPresentMask = 1U << MtkUsbPhyClockRef;
  Bus->NvmemValue = 0x1bU;
  return MtkUsbPhySuccess;
}

static MTK_USB_PHY_STATUS
InitializeFixture(
  MTK_USB_PHY_CORE *Core,
  MTK_USB_PHY_IO *Io,
  MTK_USB_PHY_CONFIG *Config,
  FAKE_PHY *Bus
  )
{
  MTK_USB_PHY_STATUS Status;

  FixtureInit(Core, Io, Config, Bus);
  Status = MtkUsbPhyCoreInitialize(Core, Io, Config);
  return Status;
}

static FAKE_TRACE
WriteAt(const FAKE_PHY *Bus, uint32_t WriteNumber)
{
  uint32_t Index;
  uint32_t Seen;

  Seen = 0U;
  for (Index = 0U; Index < Bus->TraceCount; ++Index) {
    if (Bus->Trace[Index].Kind == EventWrite) {
      if (Seen == WriteNumber) {
        return Bus->Trace[Index];
      }
      ++Seen;
    }
  }

  CHECK(0);
  return Bus->Trace[0];
}

static void
ResetFailureCounters(FAKE_PHY *Bus)
{
  memset(Bus->Calls, 0, sizeof(Bus->Calls));
  memset(Bus->FailAt, 0, sizeof(Bus->FailAt));
  Bus->FmmReadCount = 0U;
}

static void
TestResumeProfileFromCachedReadback(void)
{
  MTK_USB_PHY_RESUME_SNAPSHOT Snapshot = {
    0x00f80000U | (3U << 12) | (5U << 8),
    6U << 12,
    (2U << 30) | (1U << 28) | 0x00100000U | (7U << 4) | 5U
  };
  MTK_USB_PHY_CONFIG Config;
  MTK_USB_PHY_CONFIG BeforeConfig;
  MTK_USB_PHY_HOST_TUNING HostTuning;
  MTK_USB_PHY_HOST_TUNING BeforeHostTuning;

  memset(&Config, 0x5a, sizeof(Config));
  memset(&HostTuning, 0xa5, sizeof(HostTuning));
  BeforeConfig = Config;
  BeforeHostTuning = HostTuning;
  CHECK(MtkUsbPhyCoreBuildResumeProfile(&Snapshot,&Config,&HostTuning) ==
        MtkUsbPhySuccess);
  CHECK(Config.Revision == MTK_USB_PHY_CORE_REVISION &&
        Config.PhyType == MTK_USB_TPHY_T70_U2_TYPE &&
        Config.TphyVersion == MTK_USB_TPHY_VERSION_V2 &&
        Config.TuningPresent == MTK_USB_TPHY_TUNING_T70_REQUIRED &&
        Config.SetEfuseV1 == 1U && Config.EyeVrt == 3U &&
        Config.EyeTerm == 5U && Config.DiscTh == 7U &&
        Config.RxSqth == 5U && Config.Rev4 == 1U);
  CHECK(HostTuning.SlewControl == 6U && HostTuning.EyeVrt == 3U &&
        HostTuning.EyeTerm == 5U && HostTuning.Rev6 == 2U &&
        HostTuning.DisconnectThreshold == 7U);

  Snapshot.Acr1 |= 1U;
  Config = BeforeConfig;
  HostTuning = BeforeHostTuning;
  CHECK(MtkUsbPhyCoreBuildResumeProfile(&Snapshot,&Config,&HostTuning) ==
        MtkUsbPhyInvalid);
  CHECK(memcmp(&Config,&BeforeConfig,sizeof(Config)) == 0 &&
        memcmp(&HostTuning,&BeforeHostTuning,sizeof(HostTuning)) == 0);
  CHECK(MtkUsbPhyCoreBuildResumeProfile(NULL,&Config,&HostTuning) ==
        MtkUsbPhyInvalid);
  CHECK(MtkUsbPhyCoreBuildResumeProfile(&Snapshot,&Config,NULL) ==
        MtkUsbPhyInvalid);
}

static void
TestSupportedLifecycleAndRegisterOrder(void)
{
  MTK_USB_PHY_CORE Core;
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  FAKE_PHY Bus;
  MTK_USB_PHY_STATUS Status;
  FAKE_TRACE Write;

  FixtureInit(&Core, &Io, &Config, &Bus);
  Status = MtkUsbPhyCoreInitialize(&Core, &Io, &Config);
  CHECK(Status == MtkUsbPhySuccess);
  CHECK(MtkUsbPhyCoreIsInitialized(&Core));
  CHECK(!MtkUsbPhyCoreIsPowered(&Core));
  CHECK(Bus.NvmemCalls == 1U);
  CHECK(strcmp(Bus.NvmemName, MTK_USB_TPHY_T70_NVMEM_CELL) == 0);
  CHECK(Bus.NvmemMask == MTK_USB_TPHY_T70_NVMEM_MASK);
  CHECK(Bus.PreparedMask == (1U << MtkUsbPhyClockRef));
  CHECK(Bus.EnabledMask == (1U << MtkUsbPhyClockRef));
  CHECK((Core.ClockPresentMask & (1U << MtkUsbPhyClockDaRef)) == 0U);

  Write = WriteAt(&Bus, 0U);
  CHECK((Write.Offset == REG_U2PHYACR1) &&
        (((Write.Value >> 19) & 0x1fU) == 0x1bU));
  CHECK(WriteAt(&Bus, 1U).Offset == REG_U2PHYDTM0);
  CHECK(WriteAt(&Bus, 2U).Offset == REG_U2PHYDTM0);
  CHECK(WriteAt(&Bus, 3U).Offset == REG_U2PHYDTM0);
  CHECK(WriteAt(&Bus, 4U).Offset == REG_U2PHYDTM1);
  CHECK(WriteAt(&Bus, 5U).Offset == REG_U2PHYACR0);
  CHECK(WriteAt(&Bus, 6U).Offset == REG_U2PHYACR5);
  CHECK(WriteAt(&Bus, 7U).Offset == REG_U2PHYACR4);
  CHECK((WriteAt(&Bus, 7U).Value & 0x300U) == 0U);
  CHECK(WriteAt(&Bus, 8U).Offset == REG_U2PHYACR6);
  CHECK((WriteAt(&Bus, 8U).Value & 0x00800000U) == 0U);
  CHECK(WriteAt(&Bus, 9U).Offset == REG_U2PHYACR6);
  CHECK((WriteAt(&Bus, 9U).Value & 0xfU) == 2U);
  CHECK(WriteAt(&Bus, 10U).Offset == REG_U2PHYACR1);
  CHECK((WriteAt(&Bus, 10U).Value & 0x7000U) == 0x4000U);
  CHECK(WriteAt(&Bus, 11U).Offset == REG_U2PHYACR1);
  CHECK((WriteAt(&Bus, 11U).Value & 0x700U) == 0x400U);
  CHECK(WriteAt(&Bus, 12U).Offset == REG_U2PHYACR6);
  CHECK((WriteAt(&Bus, 12U).Value & 0xf0U) == 0x70U);
  CHECK(WriteAt(&Bus, 13U).Offset == REG_U2PHYACR6);
  CHECK((WriteAt(&Bus, 13U).Value & (1U << 28)) != 0U);
  CHECK(WriteAt(&Bus, 14U).Offset == REG_U2PHYACR6);
  CHECK((WriteAt(&Bus, 14U).Value & 0xfU) == 5U);

  Bus.Registers[REG_FMMONR1 / 4U] = 1U;
  Bus.Registers[REG_FMOUT / 4U] = 128U;
  Status = MtkUsbPhyCorePowerOn(&Core);
  CHECK(Status == MtkUsbPhySuccess);
  CHECK(MtkUsbPhyCoreIsPowered(&Core));
  Status = MtkUsbPhyCorePowerOn(&Core);
  CHECK(Status == MtkUsbPhyBusy);
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) == MtkUsbPhyBusy);

  CHECK(((Bus.Registers[REG_U2PHYACR1 / 4U] >> 19) & 0x1fU) == 0x1bU);
  CHECK(((Bus.Registers[REG_U2PHYACR1 / 4U] >> 12) & 0x7U) == 4U);
  CHECK(((Bus.Registers[REG_U2PHYACR1 / 4U] >> 8) & 0x7U) == 4U);
  CHECK(((Bus.Registers[REG_U2PHYACR6 / 4U] >> 4) & 0xfU) == 7U);
  CHECK((Bus.Registers[REG_U2PHYACR6 / 4U] & 0xfU) == 5U);
  CHECK((Bus.Registers[REG_U2PHYACR6 / 4U] & (1U << 28)) != 0U);
  CHECK(((Bus.Registers[REG_U2PHYACR5 / 4U] >> 12) & 0x7U) == 6U);

  Status = MtkUsbPhyCoreExit(&Core);
  CHECK(Status == MtkUsbPhyBusy);
  Status = MtkUsbPhyCorePowerOff(&Core);
  CHECK(Status == MtkUsbPhySuccess);
  CHECK(MtkUsbPhyCoreIsInitialized(&Core));
  CHECK(!MtkUsbPhyCoreIsPowered(&Core));
  CHECK((Bus.Registers[REG_U2PHYACR6 / 4U] & (1U << 20)) == 0U);
  CHECK((Bus.Registers[REG_U2PHYDTM1 / 4U] & 0x24U) == 0U);

  ResetFailureCounters(&Bus);
  Status = MtkUsbPhyCoreExit(&Core);
  CHECK(Bus.Calls[EventRead] == 0U && Bus.Calls[EventWrite] == 0U);
  CHECK(Status == MtkUsbPhySuccess);
  CHECK(!MtkUsbPhyCoreIsInitialized(&Core));
  CHECK(Bus.PreparedMask == 0U);
  CHECK(Bus.EnabledMask == 0U);
}

static void
TestMissingOptionalClocksAndZeroEfuse(void)
{
  MTK_USB_PHY_CORE Core;
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  FAKE_PHY Bus;
  MTK_USB_PHY_STATUS Status;
  uint32_t Index;
  uint32_t EfuseBefore;
  uint32_t TuningWrites = 0U;

  FixtureInit(&Core, &Io, &Config, &Bus);
  EfuseBefore = Bus.Registers[REG_U2PHYACR1 / 4U] & 0x00f80000U;
  Bus.ClockPresentMask = 0U;
  Bus.NvmemValue = 0U;
  Status = MtkUsbPhyCoreInitialize(&Core, &Io, &Config);
  CHECK(Status == MtkUsbPhySuccess);
  CHECK(Bus.PreparedMask == 0U);
  CHECK(Bus.EnabledMask == 0U);
  CHECK(Bus.NvmemCalls == 1U);
  CHECK(Bus.NvmemMask == 0x1fU);
  for (Index = 0U; Index < Bus.TraceCount; ++Index) {
    if (Bus.Trace[Index].Kind == EventWrite && Bus.Trace[Index].Offset == REG_U2PHYACR1) {
      CHECK((Bus.Trace[Index].Value & 0x00f80000U) == EfuseBefore);
      ++TuningWrites;
    }
  }
  CHECK(TuningWrites == 2U);
  CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);

  FixtureInit(&Core, &Io, &Config, &Bus);
  Bus.ClockPresentMask = 1U << MtkUsbPhyClockDaRef;
  Status = MtkUsbPhyCoreInitialize(&Core, &Io, &Config);
  CHECK(Status == MtkUsbPhySuccess);
  CHECK(Bus.PreparedMask == (1U << MtkUsbPhyClockDaRef));
  CHECK(Bus.EnabledMask == (1U << MtkUsbPhyClockDaRef));
  CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
}

static void
TestRejectUnsupportedBeforeAnyCallback(void)
{
  MTK_USB_PHY_CORE Core;
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  FAKE_PHY Bus;

  FixtureInit(&Core, &Io, &Config, &Bus);
  Config.PhyType = 2U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyUnsupported);
  CHECK(Bus.TraceCount == 0U);

  FixtureInit(&Core, &Io, &Config, &Bus);
  Config.TphyVersion = 1U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyUnsupported);
  CHECK(Bus.TraceCount == 0U);

  FixtureInit(&Core, &Io, &Config, &Bus);
  Io.PhyBase += 0x1000U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyUnsupported);
  CHECK(Bus.TraceCount == 0U);

  FixtureInit(&Core, &Io, &Config, &Bus);
  Config.TuningPresent &= ~MTK_USB_TPHY_TUNING_REV4;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyInvalid);
  CHECK(Bus.TraceCount == 0U);

  FixtureInit(&Core, &Io, &Config, &Bus);
  Config.SlewReferencePresent = 1U;
  Config.SlewReferenceMHz = 0xffffffffU;
  Config.SlewCoefficientPresent = 1U;
  Config.SlewCoefficient = 2U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyInvalid);
  CHECK(Bus.TraceCount == 0U);

  FixtureInit(&Core, &Io, &Config, &Bus);
  Config.SlewReferencePresent = 1U;
  Config.SlewReferenceMHz = 4194304U;
  Config.SlewCoefficientPresent = 1U;
  Config.SlewCoefficient = 1U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyInvalid);
  CHECK(Bus.TraceCount == 0U);
}

static void
TestInitCallbackFailures(void)
{
  MTK_USB_PHY_CORE Core;
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  FAKE_PHY Bus;

  FixtureInit(&Core, &Io, &Config, &Bus);
  Bus.FailAt[EventResolve] = 1U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyClockError);
  CHECK(Bus.TraceCount > 0U);
  CHECK(Bus.PreparedMask == 0U && Bus.EnabledMask == 0U);
  CHECK(Core.Signature == 0U);

  FixtureInit(&Core, &Io, &Config, &Bus);
  Bus.FailAt[EventPrepare] = 1U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyClockError);
  CHECK(Bus.PreparedMask == 0U && Bus.EnabledMask == 0U);
  CHECK(Core.Signature == 0U);

  FixtureInit(&Core, &Io, &Config, &Bus);
  Bus.FailAt[EventEnable] = 1U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyClockError);
  CHECK(Bus.PreparedMask == 0U && Bus.EnabledMask == 0U);
  CHECK(Core.Signature == 0U);

  FixtureInit(&Core, &Io, &Config, &Bus);
  Bus.FailAt[EventNvmem] = 1U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyNvmemError);
  CHECK(Bus.PreparedMask == 0U && Bus.EnabledMask == 0U);
  CHECK(Core.Signature == 0U);
  CHECK(Bus.TraceCount == 7U); /* Includes disable/unprepare after NVMEM failure. */
  CHECK(Bus.Trace[0].Kind == EventResolve && Bus.Trace[0].Offset == 0U);
  CHECK(Bus.Trace[1].Kind == EventResolve && Bus.Trace[1].Offset == 1U);
  CHECK(Bus.Trace[2].Kind == EventPrepare && Bus.Trace[2].Offset == 0U);
  CHECK(Bus.Trace[3].Kind == EventEnable && Bus.Trace[3].Offset == 0U);
  CHECK(Bus.Trace[4].Kind == EventNvmem);
  CHECK(Bus.Trace[5].Kind == EventDisable && Bus.Trace[5].Offset == 0U);
  CHECK(Bus.Trace[6].Kind == EventUnprepare && Bus.Trace[6].Offset == 0U);
  CHECK(Bus.Calls[EventRead] == 0U && Bus.Calls[EventWrite] == 0U);

  FixtureInit(&Core, &Io, &Config, &Bus);
  Bus.FailAt[EventRead] = 1U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyIoError);
  CHECK(Core.State == MTK_USB_TPHY_STATE_FAULTED);
  CHECK(Core.ClockEnabledMask == (1U << MtkUsbPhyClockRef));
  CHECK(Bus.PreparedMask == (1U << MtkUsbPhyClockRef));
  CHECK(Bus.EnabledMask == (1U << MtkUsbPhyClockRef));
  CHECK(!MtkUsbPhyCoreIsInitialized(&Core));
  ResetFailureCounters(&Bus);
  CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
  CHECK(Bus.PreparedMask == 0U && Bus.EnabledMask == 0U);

  FixtureInit(&Core, &Io, &Config, &Bus);
  Bus.FailAt[EventWrite] = 1U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyIoError);
  CHECK(Core.State == MTK_USB_TPHY_STATE_FAULTED);
  CHECK(Core.ClockEnabledMask == (1U << MtkUsbPhyClockRef));
  CHECK(Bus.PreparedMask == (1U << MtkUsbPhyClockRef));
  CHECK(Bus.EnabledMask == (1U << MtkUsbPhyClockRef));
  ResetFailureCounters(&Bus);
  CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
  CHECK(Bus.PreparedMask == 0U && Bus.EnabledMask == 0U);
}

static void
TestClockRollbackOwnershipAndRetry(void)
{
  MTK_USB_PHY_CORE Core;
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  FAKE_PHY Bus;

  FixtureInit(&Core, &Io, &Config, &Bus);
  Bus.ClockPresentMask = 3U;
  Bus.FailAt[EventPrepare] = 2U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyClockError);
  CHECK(Bus.PreparedMask == 0U);
  CHECK(Bus.EnabledMask == 0U);
  CHECK(Core.Signature == 0U);

  FixtureInit(&Core, &Io, &Config, &Bus);
  Bus.ClockPresentMask = 3U;
  Bus.FailAt[EventEnable] = 2U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyClockError);
  CHECK(Bus.PreparedMask == 0U);
  CHECK(Bus.EnabledMask == 0U);
  CHECK(Core.Signature == 0U);

  FixtureInit(&Core, &Io, &Config, &Bus);
  Bus.ClockPresentMask = 1U;
  Bus.FailAt[EventEnable] = 1U;
  Bus.FailAt[EventUnprepare] = 1U;
  CHECK(MtkUsbPhyCoreInitialize(&Core, &Io, &Config) ==
        MtkUsbPhyClockError);
  CHECK(Core.State == MTK_USB_TPHY_STATE_FAULTED);
  CHECK(Core.ClockPreparedMask == (1U << MtkUsbPhyClockRef));
  CHECK(Bus.PreparedMask == (1U << MtkUsbPhyClockRef));
  ResetFailureCounters(&Bus);
  CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
  CHECK(Bus.PreparedMask == 0U);
  CHECK(Bus.EnabledMask == 0U);
}

static void
TestPowerCallbackFailuresAndRecovery(void)
{
  MTK_USB_PHY_CORE Core;
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  FAKE_PHY Bus;

  CHECK(InitializeFixture(&Core, &Io, &Config, &Bus) ==
        MtkUsbPhySuccess);
  Bus.Registers[REG_FMMONR1 / 4U] = 1U;
  Bus.Registers[REG_FMOUT / 4U] = 128U;
  ResetFailureCounters(&Bus);
  Bus.FailAt[EventRead] = 1U;
  CHECK(MtkUsbPhyCorePowerOn(&Core) == MtkUsbPhyIoError);
  CHECK(Core.State == MTK_USB_TPHY_STATE_INITIALIZED);
  CHECK(!MtkUsbPhyCoreIsPowered(&Core));

  ResetFailureCounters(&Bus);
  Bus.FailAt[EventWrite] = 1U;
  CHECK(MtkUsbPhyCorePowerOn(&Core) == MtkUsbPhyIoError);
  CHECK(Core.State == MTK_USB_TPHY_STATE_INITIALIZED);

  ResetFailureCounters(&Bus);
  Bus.FailAt[EventDelay] = 1U;
  CHECK(MtkUsbPhyCorePowerOn(&Core) == MtkUsbPhyDelayError);
  CHECK(Core.State == MTK_USB_TPHY_STATE_INITIALIZED);

  ResetFailureCounters(&Bus);
  Bus.FailAt[EventNow] = 1U;
  CHECK(MtkUsbPhyCorePowerOn(&Core) == MtkUsbPhyTimeError);
  CHECK(Core.State == MTK_USB_TPHY_STATE_INITIALIZED);

  ResetFailureCounters(&Bus);
  CHECK(MtkUsbPhyCorePowerOn(&Core) == MtkUsbPhySuccess);
  ResetFailureCounters(&Bus);
  Bus.FailAt[EventWrite] = 1U;
  CHECK(MtkUsbPhyCorePowerOff(&Core) == MtkUsbPhyIoError);
  CHECK(Core.State == MTK_USB_TPHY_STATE_FAULTED);
  CHECK(Core.PowerCleanupPending != 0U);
  ResetFailureCounters(&Bus);
  CHECK(MtkUsbPhyCorePowerOff(&Core) == MtkUsbPhySuccess);
  CHECK(Core.State == MTK_USB_TPHY_STATE_INITIALIZED);
  CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
}

static void
TestFrequencyPollBoundaries(void)
{
  MTK_USB_PHY_CORE Core;
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  FAKE_PHY Bus;
  const uint32_t Frequencies[] = {0U, 128U};

  for (uint32_t Frozen = 0U; Frozen < 2U; ++Frozen) {
    for (uint32_t I = 0U; I < 2U; ++I) {
      CHECK(InitializeFixture(&Core, &Io, &Config, &Bus) ==
            MtkUsbPhySuccess);
      Bus.FreezeTime = (uint8_t)Frozen;
      Bus.Registers[REG_FMMONR1 / 4U] = 0U;
      Bus.Registers[REG_FMOUT / 4U] = Frequencies[I];
      ResetFailureCounters(&Bus);
      CHECK(MtkUsbPhyCorePowerOn(&Core) == MtkUsbPhySuccess);
      if (Frozen) {
        CHECK(Bus.FmmReadCount >= 128U && Bus.FmmReadCount <= 132U);
      } else {
        CHECK(Bus.FmmReadCount < 128U);
      }
      CHECK(Core.State == MTK_USB_TPHY_STATE_POWERED);
      CHECK(MtkUsbPhyCoreIsPowered(&Core));
      CHECK(Core.CalibrationActive == 0U && Core.PowerCleanupPending == 0U);
      CHECK((Bus.Registers[0x100U / 4U] & (1U << 24)) == 0U);
      CHECK((Bus.Registers[REG_FMMONR1 / 4U] & (1U << 8)) == 0U);
      CHECK((Bus.Registers[REG_U2PHYACR5 / 4U] & (1U << 15)) == 0U);
      CHECK(((Bus.Registers[REG_U2PHYACR5 / 4U] >> 12) & 7U) ==
            (Frequencies[I] ? 6U : 4U));
      CHECK(Bus.EnabledMask == (1U << MtkUsbPhyClockRef));
      CHECK(MtkUsbPhyCorePowerOff(&Core) == MtkUsbPhySuccess);
      CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
      CHECK(Bus.EnabledMask == 0U && Bus.PreparedMask == 0U);
    }
  }
}

static void
TestExitClockReleaseFailures(void)
{
  MTK_USB_PHY_CORE Core;
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  FAKE_PHY Bus;

  CHECK(InitializeFixture(&Core, &Io, &Config, &Bus) ==
        MtkUsbPhySuccess);
  ResetFailureCounters(&Bus);
  Bus.FailAt[EventDisable] = 1U;
  CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhyClockError);
  CHECK(Core.State == MTK_USB_TPHY_STATE_FAULTED);
  CHECK(Core.ClockEnabledMask == (1U << MtkUsbPhyClockRef));
  CHECK(Bus.EnabledMask == (1U << MtkUsbPhyClockRef));
  ResetFailureCounters(&Bus);
  CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
  CHECK(Bus.EnabledMask == 0U && Bus.PreparedMask == 0U);

  CHECK(InitializeFixture(&Core, &Io, &Config, &Bus) ==
        MtkUsbPhySuccess);
  ResetFailureCounters(&Bus);
  Bus.FailAt[EventUnprepare] = 1U;
  CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhyClockError);
  CHECK(Core.ClockEnabledMask == 0U);
  CHECK(Core.ClockPreparedMask == (1U << MtkUsbPhyClockRef));
  CHECK(Bus.PreparedMask == (1U << MtkUsbPhyClockRef));
  ResetFailureCounters(&Bus);
  CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
  CHECK(Bus.PreparedMask == 0U);
}

static void
PowerModeFixture(MTK_USB_PHY_CORE *Core, MTK_USB_PHY_IO *Io,
                 MTK_USB_PHY_CONFIG *Config, FAKE_PHY *Bus)
{
  CHECK(InitializeFixture(Core, Io, Config, Bus) == MtkUsbPhySuccess);
  Bus->Registers[REG_FMMONR1 / 4U] = 1U;
  Bus->Registers[REG_FMOUT / 4U] = 128U;
  CHECK(MtkUsbPhyCorePowerOn(Core) == MtkUsbPhySuccess);
  ResetFailureCounters(Bus);
  Bus->TraceCount = 0U;
}

static void
TestUsbModesAndValidation(void)
{
  MTK_USB_PHY_CORE Core;
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  FAKE_PHY Bus;
  MTK_USB_PHY_HOST_TUNING Host = { 7U, 6U, 5U, 3U, 15U };
  MTK_USB_PHY_HOST_TUNING Bad;
  uint32_t Seed, Before, Index;
  const uint32_t Initial[] = { 0U, 0xffffffffU, 0xaaaaaaaaU, 0x55555555U };
  const MTK_USB_PHY_USB_MODE Modes[] = {
    MtkUsbPhyModeHost, MtkUsbPhyModeDevice, MtkUsbPhyModeOtg
  };

  FixtureInit(&Core, &Io, &Config, &Bus);
  CHECK(MtkUsbPhyCoreSetUsbMode(NULL, MtkUsbPhyModeHost, &Host) == MtkUsbPhyInvalid);
  CHECK(MtkUsbPhyCoreSetUsbMode(&Core, MtkUsbPhyModeHost, &Host) == MtkUsbPhyInvalid);
  PowerModeFixture(&Core, &Io, &Config, &Bus);
  CHECK(MtkUsbPhyCoreSetUsbMode(&Core, (MTK_USB_PHY_USB_MODE)0, NULL) == MtkUsbPhyUnsupported);
  CHECK(MtkUsbPhyCoreSetUsbMode(&Core, (MTK_USB_PHY_USB_MODE)2, &Host) == MtkUsbPhyUnsupported);
  CHECK(MtkUsbPhyCoreSetUsbMode(&Core, MtkUsbPhyModeHost, NULL) == MtkUsbPhyInvalid);
  CHECK(MtkUsbPhyCoreSetUsbMode(&Core, MtkUsbPhyModeDevice, &Host) == MtkUsbPhyInvalid);
  CHECK(MtkUsbPhyCoreSetUsbMode(&Core, MtkUsbPhyModeOtg, &Host) == MtkUsbPhyInvalid);
  for (Index = 0U; Index < 5U; ++Index) {
    Bad = Host;
    switch (Index) {
      case 0U: Bad.SlewControl = 8U; break;
      case 1U: Bad.EyeVrt = 8U; break;
      case 2U: Bad.EyeTerm = 8U; break;
      case 3U: Bad.Rev6 = 4U; break;
      default: Bad.DisconnectThreshold = 16U; break;
    }
    CHECK(MtkUsbPhyCoreSetUsbMode(&Core, MtkUsbPhyModeHost, &Bad) == MtkUsbPhyInvalid);
  }
  CHECK(Bus.TraceCount == 0U);

  for (Seed = 0U; Seed < sizeof(Initial) / sizeof(Initial[0]); ++Seed) {
    for (Index = 0U; Index < sizeof(Modes) / sizeof(Modes[0]); ++Index) {
      MTK_USB_PHY_USB_MODE Mode = Modes[Index];
      Bus.Registers[REG_U2PHYDTM1 / 4U] = Before = Initial[Seed];
      Bus.Registers[REG_U2PHYACR1 / 4U] = Initial[Seed];
      Bus.Registers[REG_U2PHYACR5 / 4U] = Initial[Seed];
      Bus.Registers[REG_U2PHYACR6 / 4U] = Initial[Seed];
      Bus.TraceCount = 0U;
      CHECK(MtkUsbPhyCoreSetUsbMode(&Core, Mode,
            Mode == MtkUsbPhyModeHost ? &Host : NULL) == MtkUsbPhySuccess);
      CHECK(MtkUsbPhyCoreIsPowered(&Core));
      CHECK(Bus.Trace[0].Kind == EventRead && Bus.Trace[0].Offset == REG_U2PHYDTM1);
      CHECK(Bus.Trace[Bus.TraceCount - 1U].Kind == EventWrite);
      CHECK(Bus.Trace[Bus.TraceCount - 1U].Offset == REG_U2PHYDTM1);
      if (Mode == MtkUsbPhyModeHost) {
        CHECK(Bus.Registers[REG_U2PHYDTM1 / 4U] == ((Before & ~2U) | 0x200U));
        CHECK((Bus.Registers[REG_U2PHYACR5 / 4U] & 0x7000U) == 0x7000U);
        CHECK((Bus.Registers[REG_U2PHYACR1 / 4U] & 0x7700U) == 0x6500U);
        CHECK((Bus.Registers[REG_U2PHYACR6 / 4U] & 0xc00000f0U) == 0xc00000f0U);
      } else if (Mode == MtkUsbPhyModeDevice) {
        CHECK(Bus.Registers[REG_U2PHYDTM1 / 4U] == (Before | 0x202U));
        CHECK((Bus.Registers[REG_U2PHYACR1 / 4U] & 0x7700U) == 0x4400U);
        CHECK((Bus.Registers[REG_U2PHYACR6 / 4U] & 0x100000ffU) == 0x10000075U);
      } else {
        CHECK(Bus.Registers[REG_U2PHYDTM1 / 4U] == (Before & ~0x202U));
        CHECK(Bus.TraceCount == 2U);
      }
    }
  }
  /* Zero optional fields leave every host tuning register untouched. */
  memset(&Host, 0, sizeof(Host));
  Bus.TraceCount = 0U;
  CHECK(MtkUsbPhyCoreSetUsbMode(&Core, MtkUsbPhyModeHost, &Host) == MtkUsbPhySuccess);
  CHECK(Bus.TraceCount == 2U);
  CHECK(MtkUsbPhyCorePowerOff(&Core) == MtkUsbPhySuccess);
  Bus.TraceCount = 0U;
  CHECK(MtkUsbPhyCoreSetUsbMode(&Core, MtkUsbPhyModeHost, &Host) == MtkUsbPhyNotReady);
  CHECK(Bus.TraceCount == 0U);
  CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
}

static void
TestUsbModeAccessFailures(void)
{
  const MTK_USB_PHY_USB_MODE Modes[] = {
    MtkUsbPhyModeHost, MtkUsbPhyModeDevice, MtkUsbPhyModeOtg
  };
  MTK_USB_PHY_HOST_TUNING Host = { 7U, 6U, 5U, 3U, 15U };
  MTK_USB_PHY_CORE Core;
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  FAKE_PHY Bus;
  uint32_t ModeIndex, Event, Step, Reads, Writes, TraceCount;

  for (ModeIndex = 0U; ModeIndex < 3U; ++ModeIndex) {
    MTK_USB_PHY_USB_MODE Mode = Modes[ModeIndex];
    const MTK_USB_PHY_HOST_TUNING *Tuning = Mode == MtkUsbPhyModeHost ? &Host : NULL;
    PowerModeFixture(&Core, &Io, &Config, &Bus);
    CHECK(MtkUsbPhyCoreSetUsbMode(&Core, Mode, Tuning) == MtkUsbPhySuccess);
    Reads = Bus.Calls[EventRead];
    Writes = Bus.Calls[EventWrite];
    CHECK(MtkUsbPhyCorePowerOff(&Core) == MtkUsbPhySuccess);
    CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
    for (Event = EventRead; Event <= EventWrite; ++Event) {
      for (Step = 1U; Step <= (Event == EventRead ? Reads : Writes); ++Step) {
        PowerModeFixture(&Core, &Io, &Config, &Bus);
        Bus.FailAt[Event] = Step;
        CHECK(MtkUsbPhyCoreSetUsbMode(&Core, Mode, Tuning) == MtkUsbPhyIoError);
        CHECK(Bus.Calls[Event] == Step);
        CHECK(Core.State == MTK_USB_TPHY_STATE_FAULTED && Core.PowerCleanupPending);
        CHECK(!MtkUsbPhyCoreIsPowered(&Core) && !MtkUsbPhyCoreIsInitialized(&Core));
        CHECK(Bus.Calls[EventDisable] == 0U && Bus.Calls[EventUnprepare] == 0U);
        CHECK(Bus.EnabledMask == 1U && Bus.PreparedMask == 1U);
        TraceCount = Bus.TraceCount;
        CHECK(MtkUsbPhyCoreSetUsbMode(&Core, Mode, Tuning) == MtkUsbPhyNotReady);
        CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhyBusy);
        CHECK(Bus.TraceCount == TraceCount);
        ResetFailureCounters(&Bus);
        CHECK(MtkUsbPhyCorePowerOff(&Core) == MtkUsbPhySuccess);
        CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
        CHECK(Bus.EnabledMask == 0U && Bus.PreparedMask == 0U);
      }
    }
  }
}

static void
PrepareMusbOperation(MTK_USB_PHY_CORE *Core, MTK_USB_PHY_IO *Io,
                     MTK_USB_PHY_CONFIG *Config, FAKE_PHY *Bus,
                     MTK_USB_PHY_MUSB_SESSION *Session, unsigned Op)
{
  PowerModeFixture(Core, Io, Config, Bus);
  *Session = (MTK_USB_PHY_MUSB_SESSION){0};
  Bus->Registers[REG_U2PHYDTM1 / 4U] = 0x02003f2dU;
  if (Op > 0U)
    CHECK(MtkUsbPhyCoreSetMusbSession(Core, Session, MtkUsbPhyMusbInvalid) == MtkUsbPhySuccess);
  if (Op > 1U)
    CHECK(MtkUsbPhyCoreSetMusbSession(Core, Session, MtkUsbPhyMusbHost) == MtkUsbPhySuccess);
  ResetFailureCounters(Bus);
  Bus->TraceCount = 0U;
}

static void
TestMusbSessionSequenceAndFaults(void)
{
  MTK_USB_PHY_CORE Core;
  MTK_USB_PHY_IO Io;
  MTK_USB_PHY_CONFIG Config;
  MTK_USB_PHY_MUSB_SESSION Session;
  FAKE_PHY Bus;
  const uint32_t Seeds[] = {0U, 0xffffffffU, 0x02003f2dU, 0xdead0000U};
  for (unsigned Seed = 0U; Seed < 4U; ++Seed) {
    PrepareMusbOperation(&Core, &Io, &Config, &Bus, &Session, 0U);
    uint32_t Original = Seeds[Seed], Value = Original, Writes = 0U;
    uint32_t Expected[] = {Value | 0x11U, (Value | 0x11U) & ~0x2eU,
                          ((Value | 0x11U) & ~0x2eU) | 0x3f00U};
    Bus.Registers[REG_U2PHYDTM1 / 4U] = Original;
    CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, MtkUsbPhyMusbHost) == MtkUsbPhyBusy);
    CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, MtkUsbPhyMusbRestore) == MtkUsbPhyBusy);
    CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, MtkUsbPhyMusbAcquire) == MtkUsbPhySuccess);
    CHECK(Session.Owned == 1U && Session.OriginalDtm1 == Original && Bus.TraceCount == 1U);
    CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, MtkUsbPhyMusbAcquire) == MtkUsbPhyBusy);
    CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, MtkUsbPhyMusbHost) == MtkUsbPhySuccess);
    CHECK(Session.Owned == 2U);
    CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, MtkUsbPhyMusbRestore) == MtkUsbPhySuccess);
    CHECK(Session.Owned == 0U && Bus.Registers[REG_U2PHYDTM1 / 4U] == Original);
    CHECK(MtkUsbPhyCorePowerOff(&Core) == MtkUsbPhySuccess);
    CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);

    PrepareMusbOperation(&Core, &Io, &Config, &Bus, &Session, 0U);
    Bus.Registers[REG_U2PHYDTM1 / 4U] = Original;
    CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, MtkUsbPhyMusbRestore) == MtkUsbPhyBusy);
    CHECK(Bus.TraceCount == 0U);
    CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, MtkUsbPhyMusbInvalid) == MtkUsbPhySuccess);
    CHECK(Session.Owned == 1U && Session.OriginalDtm1 == Original);
    for (unsigned I = 0U; I < Bus.TraceCount; ++I) if (Bus.Trace[I].Kind == EventWrite) {
      CHECK(Writes < 3U && Bus.Trace[I].Offset == REG_U2PHYDTM1 &&
            Bus.Trace[I].Value == Expected[Writes]);
      ++Writes;
    }
    CHECK(Writes == 3U && Bus.TraceCount == 7U);
    unsigned Before = Bus.TraceCount;
    CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, MtkUsbPhyMusbInvalid) == MtkUsbPhyBusy);
    CHECK(Bus.TraceCount == Before);
    Value = Expected[2]; Expected[0] = Value & ~0x12U;
    Expected[1] = Expected[0] | 0x2dU;Expected[2] = Expected[1] | 0x3f00U;
    Bus.TraceCount = 0U;Writes = 0U;
    CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, MtkUsbPhyMusbHost) == MtkUsbPhySuccess);
    for (unsigned I = 0U; I < Bus.TraceCount; ++I) if (Bus.Trace[I].Kind == EventWrite) {
      CHECK(Writes < 3U && Bus.Trace[I].Offset == REG_U2PHYDTM1 &&
            Bus.Trace[I].Value == Expected[Writes]);
      ++Writes;
    }
    CHECK(Writes == 3U && Bus.TraceCount == 6U && Session.Owned == 2U);
    Bus.Registers[REG_U2PHYDTM1 / 4U] ^= 0x80000000U; // unrelated live bit must survive restore
    uint32_t Unowned = Bus.Registers[REG_U2PHYDTM1 / 4U] & ~0x3f3fU;
    CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, MtkUsbPhyMusbRestore) == MtkUsbPhySuccess);
    CHECK(Session.Owned == 0U && Session.OriginalDtm1 == 0U);
    CHECK(Bus.Registers[REG_U2PHYDTM1 / 4U] == (Unowned | (Original & 0x3f3fU)));
    CHECK(MtkUsbPhyCoreIsPowered(&Core));
    CHECK(MtkUsbPhyCorePowerOff(&Core) == MtkUsbPhySuccess);
    CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
  }

  for (unsigned Op = 0U; Op < 3U; ++Op) {
    PrepareMusbOperation(&Core, &Io, &Config, &Bus, &Session, Op);
    CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, (MTK_USB_PHY_MUSB_OPERATION)Op) == MtkUsbPhySuccess);
    unsigned Reads = Bus.Calls[EventRead], Writes = Bus.Calls[EventWrite];
    CHECK(MtkUsbPhyCorePowerOff(&Core) == MtkUsbPhySuccess);
    CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
    for (unsigned Event = EventRead; Event <= EventWrite; ++Event)
      for (unsigned Step = 1U; Step <= (Event == EventRead ? Reads : Writes); ++Step)
        for (unsigned After = 0U; After < (Event == EventWrite ? 2U : 1U); ++After) {
          PrepareMusbOperation(&Core, &Io, &Config, &Bus, &Session, Op);
          Bus.FailAt[Event] = Step;Bus.FailWriteAfterEffect = After;
          CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, (MTK_USB_PHY_MUSB_OPERATION)Op) == MtkUsbPhyIoError);
          CHECK(Core.State == MTK_USB_TPHY_STATE_FAULTED && Core.PowerCleanupPending);
          CHECK(Bus.EnabledMask == 1U && Bus.PreparedMask == 1U);
          CHECK(Session.Owned == ((Op == 0U && Event == EventRead && Step == 1U) ? 0U : Op == 0U ? 1U : Op == 1U ? 2U : 2U));
          unsigned Count = Bus.TraceCount;
          CHECK(MtkUsbPhyCoreSetMusbSession(&Core, &Session, (MTK_USB_PHY_MUSB_OPERATION)Op) == MtkUsbPhyNotReady);
          CHECK(Bus.TraceCount == Count);
          ResetFailureCounters(&Bus);
          CHECK(MtkUsbPhyCorePowerOff(&Core) == MtkUsbPhySuccess);
          CHECK(MtkUsbPhyCoreExit(&Core) == MtkUsbPhySuccess);
          CHECK(Bus.EnabledMask == 0U && Bus.PreparedMask == 0U);
        }
  }
}

int
main(void)
{
  TestResumeProfileFromCachedReadback();
  TestSupportedLifecycleAndRegisterOrder();
  TestMissingOptionalClocksAndZeroEfuse();
  TestRejectUnsupportedBeforeAnyCallback();
  TestInitCallbackFailures();
  TestClockRollbackOwnershipAndRetry();
  TestPowerCallbackFailuresAndRecovery();
  TestFrequencyPollBoundaries();
  TestExitClockReleaseFailures();
  TestUsbModesAndValidation();
  TestUsbModeAccessFailures();
  TestMusbSessionSequenceAndFaults();
  fprintf(stderr, "MtkUsbPhyCoreTest checks: %u\n", Checks);
  return 0;
}
