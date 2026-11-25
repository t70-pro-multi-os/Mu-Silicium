// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkConnBoot.h>

/*
 * T70 Pro vendor WMT_SOC.cfg selects co_clock_flag=1. The MT6789 WMT
 * driver's consys_is_rc_mode_enable_mt6789 returns zero. The initialization
 * below follows its IF pinmux, MCU reset and post-ROM setup.
 */
#define MCU_RESET_REG       0x018U
#define MCU_RESET_BIT       0x1000U
#define MCU_RESET_KEY       0x88000000U
#define SPM_ENABLE_KEY      0x0b160000U
#define CONN_STATUS_REG     0xc14U
#define MCU_IDLE_REG        0x600U
#define MCU_IDLE            0x1d1eU
#define MCU_IDLE_RETRIES    41U
#define MCU_IDLE_WAIT_US    5000U
#define WIFI_MODE_MASK      0x70U
#define PROBE_SETTLE_US     300U
#define RF_SPI_GATE         0x500U
#define RF_SPI_ENABLE       1U
#define RF_SPI_STATUS       0x00U
#define RF_SPI_BUSY         (1U << 0)
#define RF_SPI_READ_BUSY    (1U << 5)
#define RF_SPI_ADDR         0x50U
#define RF_SPI_TRIGGER      0x54U
#define RF_SPI_READ         0x58U
#define RF_SPI_WAIT_US      500U
#define RF_SPI_RETRIES      10U
#define ADIE_PROBE_ROUNDS   3U

typedef struct {
  MTK_CONN_ROM_REGION Region;
  unsigned Offset;
  unsigned Mask;
  unsigned SetAlias;
} CONN_PIN;

enum { InterfaceMode, WifiMode, InterfaceDrive, WifiDirection, WifiOutput };
static const CONN_PIN Pins[] = {
  {MtkConnGpio,  0x460U, 0x77777770U, 0},
  {MtkConnGpio,  0x470U, 0x00000077U, 0},
  {MtkConnIoCfg, 0x010U, 0x3ffffff8U, 0},
  {MtkConnGpio,  0x050U, 0x02000000U, 4},
  {MtkConnGpio,  0x150U, 0x02000000U, 4}
};
#define PIN_COUNT (sizeof(Pins) / sizeof(Pins[0]))

static int ReadReg(const MTK_CONN_BOOT *Boot, MTK_CONN_ROM_REGION Region,
                   unsigned Offset, unsigned *Value) {
  const MTK_CONN_ROM_IO *Io = &Boot->Io.Rom;
  return !Io->Read32(Io->Context, Region, Offset, Value) && *Value != ~0U;
}

static int WriteReg(const MTK_CONN_BOOT *Boot, MTK_CONN_ROM_REGION Region,
                    unsigned Offset, unsigned Value) {
  const MTK_CONN_POWER_IO *PowerIo = &Boot->Power.Io;
  int Error = Boot->Io.Write32(Boot->Io.Rom.Context, Region, Offset, Value);
  PowerIo->Barrier(PowerIo->Context);
  return !Error;
}

static int VerifyReg(const MTK_CONN_BOOT *Boot, MTK_CONN_ROM_REGION Region,
                     unsigned Offset, unsigned Mask, unsigned Value) {
  unsigned Current;
  return ReadReg(Boot, Region, Offset, &Current) && (Current & Mask) == Value;
}

static int UpdateReg(const MTK_CONN_BOOT *Boot, MTK_CONN_ROM_REGION Region,
                     unsigned Offset, unsigned Mask, unsigned Value) {
  unsigned Current;
  return ReadReg(Boot, Region, Offset, &Current) &&
         WriteReg(Boot, Region, Offset, (Current & ~Mask) | Value) &&
         VerifyReg(Boot, Region, Offset, Mask, Value);
}

static int SetPin(MTK_CONN_BOOT *Boot, unsigned Index, unsigned Mask, unsigned Value) {
  const CONN_PIN *Pin = &Pins[Index];
  // A failed bus write can still change the register.
  Boot->PinsOwned |= 1U << Index;
  if (Pin->SetAlias) {
    unsigned Alias = Value ? Pin->SetAlias : 2 * Pin->SetAlias;
    return WriteReg(Boot, Pin->Region, Pin->Offset + Alias, Mask) &&
           VerifyReg(Boot, Pin->Region, Pin->Offset, Mask, Value);
  }
  return UpdateReg(Boot, Pin->Region, Pin->Offset, Mask, Value);
}

static int ConfigurePins(MTK_CONN_BOOT *Boot, unsigned Adie) {
  unsigned Index, Current;
  // Save the shared fields before the first update. GPIO185 is used as an
  // output only on MT6635 during startup.
  unsigned Count = Adie == 0x6635U ? PIN_COUNT : WifiDirection;
  for (Index = 0; Index < Count; ++Index) {
    const CONN_PIN *Pin = &Pins[Index];
    if (!ReadReg(Boot, Pin->Region, Pin->Offset, &Current)) return 0;
    Boot->SavedPins[Index] = Current & Pin->Mask;
  }
  return SetPin(Boot, InterfaceMode, Pins[InterfaceMode].Mask, 0x11111110U) &&
         SetPin(Boot, WifiMode, Pins[WifiMode].Mask, 0x11U) &&
         SetPin(Boot, InterfaceDrive, Pins[InterfaceDrive].Mask, 0x1209U << 3);
}

static int SetMcuReset(const MTK_CONN_BOOT *Boot, unsigned Reset) {
  unsigned Current;
  if (!ReadReg(Boot, MtkConnRomRgu, MCU_RESET_REG, &Current)) return 0;
  if ((Current & MCU_RESET_BIT) == Reset) return 1;
  return WriteReg(Boot, MtkConnRomRgu, MCU_RESET_REG,
                  MCU_RESET_KEY | (Current & (0xffffffU & ~MCU_RESET_BIT)) | Reset) &&
         VerifyReg(Boot, MtkConnRomRgu, MCU_RESET_REG, MCU_RESET_BIT, Reset);
}

static int EnableSpmClock(const MTK_CONN_POWER_IO *Io) {
  unsigned Value;
  int Error;
  if (Io->Read(Io->Context, MtkConnSpm, 0, &Value) || Value == ~0U) return 0;
  if (Value & 1U) return 1;
  // SPM's register-control enable is shared with other power domains.
  Error = Io->Write(Io->Context, MtkConnSpm, 0, SPM_ENABLE_KEY | (Value & 0xffffU) | 1U);
  Io->Barrier(Io->Context);
  return !Error && !Io->Read(Io->Context, MtkConnSpm, 0, &Value) &&
         Value != ~0U && (Value & 1U);
}

static int ClearConnStatus(const MTK_CONN_POWER_IO *Io) {
  int Error = Io->Write(Io->Context, MtkConnInfra, CONN_STATUS_REG, 0);
  Io->Barrier(Io->Context);
  return !Error;
}

static int ConfigureXo(const MTK_CONN_BOOT *Boot) {
  return UpdateReg(Boot, MtkConnSysram, 0x240U, 0x80U, 0) &&
         UpdateReg(Boot, MtkConnSysram, 0x204U, 0xff00U, 0x600U) &&
         UpdateReg(Boot, MtkConnSysram, 0x200U, 0xffffU, 0x708U) &&
         UpdateReg(Boot, MtkConnSysram, 0x204U, 1U, 0);
}

static int ConfigureMcu(MTK_CONN_BOOT *Boot, unsigned Adie) {
  if (!UpdateReg(Boot, MtkConnSysram, 0x168U, 1U, 1U) ||
      !WriteReg(Boot, MtkConnRomMcuCfg, 0x504U, 0xf0170000U) ||
      !WriteReg(Boot, MtkConnRomMcuCfg, 0x508U, 0xf02a0000U) ||
      !VerifyReg(Boot, MtkConnRomMcuCfg, 0x504U, ~0U, 0xf0170000U) ||
      !VerifyReg(Boot, MtkConnRomMcuCfg, 0x508U, ~0U, 0xf02a0000U) ||
      !ConfigureXo(Boot) ||
      !UpdateReg(Boot, MtkConnSysram, 0x130U, 0x100U, Adie == 0x6631U ? 0x100U : 0))
    return 0;
  if (Adie == 0x6635U &&
      (!SetPin(Boot, WifiDirection, Pins[WifiDirection].Mask, Pins[WifiDirection].Mask) ||
       !SetPin(Boot, WifiOutput, Pins[WifiOutput].Mask, Pins[WifiOutput].Mask) ||
       !SetPin(Boot, WifiMode, WIFI_MODE_MASK, 0)))
    return 0;
  return WriteReg(Boot, MtkConnRomMcuCfg, 0x440U, 0x90000002U) &&
         UpdateReg(Boot, MtkConnMcuMem, 0x284U, 0x000f0000U, 0x00040000U);
}

static MTK_CONN_POWER_STATUS CheckMcuReset(const MTK_CONN_BOOT *Boot, unsigned Expected) {
  MTK_CONN_POWER_STATUS Status = MtkConnPowerCheck(&Boot->Power);
  unsigned Reset;
  if (Status != MtkConnPowerOk) return Status;
  if (!ReadReg(Boot, MtkConnRomRgu, MCU_RESET_REG, &Reset)) return MtkConnPowerIoError;
  if ((Reset & MCU_RESET_BIT) != Expected) return MtkConnPowerNotReady;
  return Boot->RuntimeRails ? MtkConnRailsCheckRuntime(Boot->RuntimeRails, Boot) : MtkConnPowerOk;
}

static MTK_CONN_POWER_STATUS WaitForIdle(MTK_CONN_BOOT *Boot) {
  const MTK_CONN_POWER_IO *Io = &Boot->Power.Io;
  unsigned Attempt, Value;
  for (Attempt = 0; ; ++Attempt) {
    MTK_CONN_POWER_STATUS Status = CheckMcuReset(Boot, 0);
    if (Status != MtkConnPowerOk) return Status;
    if (!ReadReg(Boot, MtkConnRomMcuCfg, MCU_IDLE_REG, &Value)) return MtkConnPowerIoError;
    if (Value == MCU_IDLE) return MtkConnPowerOk;
    if (Attempt == MCU_IDLE_RETRIES) return MtkConnPowerTimeout;
    if (Io->WaitUs(Io->Context, MCU_IDLE_WAIT_US)) return MtkConnPowerIoError;
  }
}

MTK_CONN_POWER_STATUS MtkConnBootInit(MTK_CONN_BOOT *Boot, const MTK_CONN_POWER_IO *PowerIo,
                                    const MTK_CONN_BOOT_IO *Io) {
  MTK_CONN_POWER_STATUS Status;
  if (!Boot || !Io || !Io->Write32 || !Io->Rom.Read32 || !Io->Rom.Sha256 ||
      !Io->Rom.WriteEmi || !Io->Rom.ReadEmi)
    return MtkConnPowerInvalid;
  Status = MtkConnPowerInit(&Boot->Power, PowerIo);
  if (Status != MtkConnPowerOk) return Status;
  Boot->Io = *Io;
  Boot->RuntimeRails = 0;
  Boot->Ready = Boot->PinsOwned = Boot->ResetOwned = Boot->ProbeRails = 0;
  return MtkConnPowerOk;
}

MTK_CONN_POWER_STATUS MtkConnBootCheck(const MTK_CONN_BOOT *Boot) {
  if (!Boot || !Boot->Io.Write32) return MtkConnPowerInvalid;
  return Boot->Ready ? CheckMcuReset(Boot, 0) : MtkConnPowerNotReady;
}

static MTK_CONN_POWER_STATUS ReleaseProbeRails(MTK_CONN_BOOT *Boot) {
  const MTK_CONN_POWER_IO *Io = &Boot->Power.Io;
  const MTK_CONN_PROBE_IO *ProbeIo = &Boot->ProbeIo;
  if (Boot->ProbeRails & (1U << MtkConnProbeVcn13)) {
    if (ProbeIo->SetRail(ProbeIo->Context, MtkConnProbeVcn13, 0)) return MtkConnPowerIoError;
    Boot->ProbeRails &= ~(1U << MtkConnProbeVcn13);
  }
  if (Boot->ProbeRails & (1U << MtkConnProbeVcn18)) {
    if (Io->WaitUs(Io->Context, PROBE_SETTLE_US) ||
        ProbeIo->SetRail(ProbeIo->Context, MtkConnProbeVcn18, 0))
      return MtkConnPowerIoError;
    Boot->ProbeRails &= ~(1U << MtkConnProbeVcn18);
  }
  return MtkConnPowerOk;
}

MTK_CONN_POWER_STATUS MtkConnBootStop(MTK_CONN_BOOT *Boot) {
  MTK_CONN_POWER_STATUS Status;
  unsigned Remaining;
  if (!Boot || !Boot->Io.Write32) return MtkConnPowerInvalid;
  Boot->Ready = 0;
  if (Boot->Power.State != MtkConnOff && Boot->ResetOwned &&
      !SetMcuReset(Boot, MCU_RESET_BIT)) return MtkConnPowerIoError;
  if (Boot->Power.State == MtkConnOn && !ClearConnStatus(&Boot->Power.Io))
    return MtkConnPowerIoError;
  Status = MtkConnPowerStop(&Boot->Power);
  if (Status != MtkConnPowerOk) return Status;
  for (Remaining = PIN_COUNT; Remaining > 0; --Remaining) {
    unsigned Index = Remaining - 1;
    unsigned Bit = 1U << Index;
    if (!(Boot->PinsOwned & Bit)) continue;
    if (!SetPin(Boot, Index, Pins[Index].Mask, Boot->SavedPins[Index]))
      return MtkConnPowerIoError;
    Boot->PinsOwned &= ~Bit;
  }
  if (Boot->ResetOwned && !SetMcuReset(Boot, Boot->SavedReset)) return MtkConnPowerIoError;
  Boot->ResetOwned = 0;
  if (Boot->RuntimeRails) {
    Status = MtkConnRailsReleaseRuntime(Boot->RuntimeRails, Boot);
    if (Status != MtkConnPowerOk) return Status;
    Boot->RuntimeRails = 0;
  }
  return ReleaseProbeRails(Boot);
}

static MTK_CONN_POWER_STATUS CanStartCore(const MTK_CONN_BOOT *Boot, unsigned long long EmiBase,
                                        unsigned EmiSize) {
  if (Boot->ResetOwned || Boot->PinsOwned || Boot->ProbeRails || Boot->RuntimeRails)
    return MtkConnPowerBusy;
  return MtkConnPowerCanStart(&Boot->Power, EmiBase, EmiSize);
}

static MTK_CONN_POWER_STATUS PowerOnCore(MTK_CONN_BOOT *Boot, unsigned long long EmiBase,
                                       unsigned EmiSize, unsigned Adie) {
  const MTK_CONN_POWER_IO *Io = &Boot->Power.Io;
  MTK_CONN_POWER_STATUS Status;
  unsigned Reset;
  if (!ConfigurePins(Boot, Adie) || Io->WaitUs(Io->Context, 150U) ||
      !ReadReg(Boot, MtkConnRomRgu, MCU_RESET_REG, &Reset))
    return MtkConnPowerIoError;
  Boot->SavedReset = Reset & MCU_RESET_BIT;
  Boot->ResetOwned = 1;
  if (!SetMcuReset(Boot, MCU_RESET_BIT) || !EnableSpmClock(Io))
    return MtkConnPowerIoError;

  Status = MtkConnPowerStart(&Boot->Power, EmiBase, EmiSize);
  if (Status != MtkConnPowerOk) return Status;
  if (!ClearConnStatus(Io) || Io->WaitUs(Io->Context, 5000U) || Io->WaitUs(Io->Context, 10U))
    return MtkConnPowerIoError;
  return MtkConnPowerOk;
}

static MTK_CONN_POWER_STATUS Start(MTK_CONN_BOOT *Boot, unsigned long long EmiBase,
                                   unsigned EmiSize, unsigned Adie, const MTK_CONN_ROM_SET *Set,
                                   MTK_CONN_RAILS *Rails) {
  const MTK_CONN_POWER_IO *Io;
  MTK_CONN_POWER_STATUS Status, StopStatus;
  unsigned Idle;
  if (!Boot || !Boot->Io.Write32 || !Set || (Adie != 0x6631U && Adie != 0x6635U))
    return MtkConnPowerInvalid;
  Status = CanStartCore(Boot, EmiBase, EmiSize);
  if (Status != MtkConnPowerOk) return Status;
  if (Rails) {
    if (Rails->RuntimeOwner) return MtkConnPowerBusy;
    Status = MtkConnRailsAcquireRuntime(Rails, Adie, Boot);
    if (Rails->RuntimeOwner == Boot) Boot->RuntimeRails = Rails;
    if (Status != MtkConnPowerOk) goto Fail;
  }

  Io = &Boot->Power.Io;
  Status = PowerOnCore(Boot, EmiBase, EmiSize, Adie);
  if (Status != MtkConnPowerOk) goto Fail;
  Status = MtkConnRomLoad(&Boot->Power, &Boot->Io.Rom, Adie, Set);
  if (Status != MtkConnPowerOk) goto Fail;
  if (!ConfigureMcu(Boot, Adie) || !ReadReg(Boot, MtkConnRomMcuCfg, MCU_IDLE_REG, &Idle)) {
    Status = MtkConnPowerIoError;
    goto Fail;
  }
  if (Idle == MCU_IDLE) {
    // A retained sample cannot establish that this startup executed.
    Status = MtkConnPowerNotReady;
    goto Fail;
  }
  Status = CheckMcuReset(Boot, MCU_RESET_BIT);
  if (Status != MtkConnPowerOk) goto Fail;
  if (!SetMcuReset(Boot, 0) || !UpdateReg(Boot, MtkConnHostCsr, 0x1cU, 0x80U, 0x80U)) {
    Status = MtkConnPowerIoError;
    goto Fail;
  }
  Status = WaitForIdle(Boot);
  if (Status != MtkConnPowerOk) goto Fail;
  if ((Adie == 0x6635U && !SetPin(Boot, WifiMode, WIFI_MODE_MASK, 0x10U)) ||
      Io->WaitUs(Io->Context, 20000U)) {
    Status = MtkConnPowerIoError;
    goto Fail;
  }
  Status = CheckMcuReset(Boot, 0);
  if (Status != MtkConnPowerOk) goto Fail;
  Boot->Ready = 1;
  return MtkConnPowerOk;

Fail:
  StopStatus = MtkConnBootStop(Boot);
  return StopStatus == MtkConnPowerOk ? Status : StopStatus;
}

MTK_CONN_POWER_STATUS MtkConnBootStart(MTK_CONN_BOOT *Boot, unsigned long long EmiBase,
                                     unsigned EmiSize, unsigned Adie, const MTK_CONN_ROM_SET *Set) {
  return Start(Boot, EmiBase, EmiSize, Adie, Set, 0);
}

MTK_CONN_POWER_STATUS MtkConnBootStartPowered(MTK_CONN_BOOT *Boot, unsigned long long EmiBase,
                                            unsigned EmiSize, unsigned Adie,
                                            const MTK_CONN_ROM_SET *Set, MTK_CONN_RAILS *Rails) {
  if (!Rails) return MtkConnPowerInvalid;
  return Start(Boot, EmiBase, EmiSize, Adie, Set, Rails);
}

static int AcquireProbeRail(MTK_CONN_BOOT *Boot, MTK_CONN_PROBE_RAIL Rail) {
  const MTK_CONN_PROBE_IO *Io = &Boot->ProbeIo;
  Boot->ProbeRails |= 1U << Rail;
  return !Io->SetRail(Io->Context, Rail, 1);
}

static MTK_CONN_POWER_STATUS WaitRfSpi(const MTK_CONN_BOOT *Boot, unsigned BusyMask) {
  const MTK_CONN_POWER_IO *Io = &Boot->Power.Io;
  unsigned Attempt, Value;
  for (Attempt = 0; ; ++Attempt) {
    MTK_CONN_POWER_STATUS Status = CheckMcuReset(Boot, MCU_RESET_BIT);
    if (Status != MtkConnPowerOk) return Status;
    if (!ReadReg(Boot, MtkConnRfSpi, RF_SPI_STATUS, &Value)) return MtkConnPowerIoError;
    if (!(Value & BusyMask)) return MtkConnPowerOk;
    if (Attempt == RF_SPI_RETRIES) return MtkConnPowerTimeout;
    if (Io->WaitUs(Io->Context, RF_SPI_WAIT_US)) return MtkConnPowerIoError;
  }
}

static MTK_CONN_POWER_STATUS TransferAdie(const MTK_CONN_BOOT *Boot, unsigned Candidate,
                                         unsigned *Part) {
  MTK_CONN_POWER_STATUS Status = WaitRfSpi(Boot, RF_SPI_BUSY);
  unsigned Reply, Address = Candidate == 0x6631U ? 0xb024U : 0xb02cU;
  if (Status != MtkConnPowerOk) return Status;
  if (!WriteReg(Boot, MtkConnRfSpi, RF_SPI_ADDR, Address) ||
      !WriteReg(Boot, MtkConnRfSpi, RF_SPI_TRIGGER, 0))
    return MtkConnPowerIoError;
  Status = WaitRfSpi(Boot, RF_SPI_READ_BUSY);
  if (Status != MtkConnPowerOk) return Status;
  if (!ReadReg(Boot, MtkConnRfSpi, RF_SPI_READ, &Reply)) return MtkConnPowerIoError;
  *Part = Reply >> 16;
  return MtkConnPowerOk;
}

static MTK_CONN_POWER_STATUS ReadAdie(const MTK_CONN_BOOT *Boot, unsigned Candidate, unsigned *Part) {
  MTK_CONN_POWER_STATUS Status, Held;
  unsigned Gate;
  Status = CheckMcuReset(Boot, MCU_RESET_BIT);
  if (Status != MtkConnPowerOk) return Status;
  if (!ReadReg(Boot, MtkConnSysram, RF_SPI_GATE, &Gate)) return MtkConnPowerIoError;
  if (Gate & RF_SPI_ENABLE) return MtkConnPowerBusy;

  // The stock flow toggles SYSRAM's RF SPI gate while the MCU is held.
  Status = MtkConnPowerIoError;
  if (WriteReg(Boot, MtkConnSysram, RF_SPI_GATE, Gate | RF_SPI_ENABLE) &&
      VerifyReg(Boot, MtkConnSysram, RF_SPI_GATE, RF_SPI_ENABLE, RF_SPI_ENABLE))
    Status = TransferAdie(Boot, Candidate, Part);
  Held = CheckMcuReset(Boot, MCU_RESET_BIT);
  if (Held != MtkConnPowerOk) return Held;
  if (!UpdateReg(Boot, MtkConnSysram, RF_SPI_GATE, RF_SPI_ENABLE, 0)) return MtkConnPowerIoError;
  return Status;
}

static MTK_CONN_POWER_STATUS ProbeCandidate(MTK_CONN_BOOT *Boot, unsigned long long EmiBase,
                                           unsigned EmiSize, unsigned Candidate, unsigned *Part) {
  const MTK_CONN_POWER_IO *Io = &Boot->Power.Io;
  MTK_CONN_POWER_STATUS Status;
  if (!AcquireProbeRail(Boot, MtkConnProbeVcn18) || Io->WaitUs(Io->Context, PROBE_SETTLE_US) ||
      (Candidate == 0x6635U && !AcquireProbeRail(Boot, MtkConnProbeVcn13)))
    return MtkConnPowerIoError;
  Status = PowerOnCore(Boot, EmiBase, EmiSize, Candidate);
  if (Status != MtkConnPowerOk) return Status;
  Status = MtkConnRomWaitIp(&Boot->Power, &Boot->Io.Rom);
  if (Status != MtkConnPowerOk) return Status;
  if (!UpdateReg(Boot, MtkConnSysram, 0x168U, 1U, 1U) || !ConfigureXo(Boot))
    return MtkConnPowerIoError;
  return ReadAdie(Boot, Candidate, Part);
}

MTK_CONN_POWER_STATUS MtkConnBootProbeAdie(MTK_CONN_BOOT *Boot, unsigned long long EmiBase,
                                         unsigned EmiSize, const MTK_CONN_PROBE_IO *Io,
                                         unsigned *Adie) {
  static const unsigned Candidate[] = {0x6631U, 0x6635U};
  MTK_CONN_POWER_STATUS Status, Shutdown, Result = MtkConnPowerNotReady;
  unsigned Round, Index, Part;
  if (!Boot || !Boot->Io.Write32 || !Io || !Io->SetRail || !Adie) return MtkConnPowerInvalid;
  *Adie = 0;
  for (Round = 0; Round < ADIE_PROBE_ROUNDS; ++Round) {
    for (Index = 0; Index < sizeof(Candidate) / sizeof(Candidate[0]); ++Index) {
      Status = CanStartCore(Boot, EmiBase, EmiSize);
      if (Status != MtkConnPowerOk) return Status;
      Boot->ProbeIo = *Io;
      Part = 0;
      Status = ProbeCandidate(Boot, EmiBase, EmiSize, Candidate[Index], &Part);
      Shutdown = MtkConnBootStop(Boot);
      if (Shutdown != MtkConnPowerOk) return Shutdown;
      if (Status == MtkConnPowerOk && Part == Candidate[Index]) {
        *Adie = Part;
        return MtkConnPowerOk;
      }
      if (Status == MtkConnPowerTimeout) Result = Status;
      else if (Status != MtkConnPowerOk) return Status;
    }
  }
  return Result;
}
