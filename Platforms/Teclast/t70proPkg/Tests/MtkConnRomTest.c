// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <limits.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Library/MtkConnBoot.h>

#define EMI_BASE 0x50000000ULL
#define HEADER 48U
#define GUARD 0xd3U

static const unsigned Offset[] = {0x170000, 0x2a0000, 0};
static const char *const Name[] = {"bt", "wifi", "mcu"};
static unsigned char Emi[MTK_CONN_EMI_SIZE];
static unsigned Checks;
#define CHECK(Condition) do { ++Checks; assert(Condition); } while (0)

typedef struct {
  unsigned Window, Running, InReset, FirmwareVersion, IpDelay;
  unsigned Hashes, PowerReads, VersionReads, Waits, Writes, Barriers, Reads;
  unsigned HashFailure, WaitFailure, WriteFailure, ReadFailure, BadReadback;
  unsigned LoseResetAfter, LosePowerAfter, ResetBusError;
} MODEL;

static int ReadPower(void *Context, MTK_CONN_REGION Region, unsigned Reg, unsigned *Value) {
  MODEL *Model = Context;
  ++Model->PowerReads;
  if (Region == MtkConnSpm) {
    if (Reg == 0x16c || Reg == 0x170) *Value = Model->Running ? 2 : 0;
    else { CHECK(Reg == 0x304); *Value = 0xd; }
  } else {
    CHECK(Region == MtkConnInfra);
    switch (Reg) {
    case 0x220: case 0x250: case 0x0c8: *Value = 0; break;
    case 0x380: *Value = Model->Window; break;
    case 0x388: *Value = 0x1000; break;
    default: CHECK(0); return 1;
    }
  }
  return 0;
}

static int WritePower(void *Context, MTK_CONN_REGION Region, unsigned Reg, unsigned Value) {
  (void)Context; (void)Region; (void)Reg; (void)Value;
  CHECK(0); // Loading firmware must never write a control register.
  return 1;
}

static int ReadRom(void *Context, MTK_CONN_ROM_REGION Region, unsigned Reg, unsigned *Value) {
  MODEL *Model = Context;
  CHECK(Model->Running);
  switch (Region) {
  case MtkConnRomRgu:
    CHECK(Reg == 0x18);
    if (Model->ResetBusError) return 1;
    *Value = Model->InReset ? 0x1000 : 0;
    break;
  case MtkConnRomTop:
    CHECK(Reg == 0x10 && Model->InReset);
    *Value = ++Model->VersionReads > Model->IpDelay ? 0x100c0000U : 0;
    break;
  case MtkConnRomMcuCfg:
    CHECK(Reg == 4 && Model->InReset);
    *Value = Model->FirmwareVersion;
    break;
  default:
    CHECK(0);
    return 1;
  }
  return 0;
}

static int Wait(void *Context, unsigned Us) {
  MODEL *Model = Context;
  CHECK(Us == 20000 && Model->Running && Model->InReset);
  ++Model->Waits;
  return Model->WaitFailure != 0;
}

static void Barrier(void *Context) {
  MODEL *Model = Context;
  CHECK(++Model->Barriers == Model->Writes);
}

static int Hash(void *Context, const unsigned char *Data, unsigned Size, unsigned char Digest[32]) {
  MODEL *Model = Context;
  if (++Model->Hashes == Model->HashFailure) return 1;
  return SHA256(Data, Size, Digest) == NULL;
}

static int WriteEmi(void *Context, unsigned long long Address, const unsigned char *Data, unsigned Size) {
  MODEL *Model = Context;
  CHECK(Model->Hashes == 3 && Model->InReset && Model->Running);
  CHECK(Model->Writes < MtkConnRomPartCount && Model->Barriers == Model->Writes);
  CHECK(Address == EMI_BASE + Offset[Model->Writes] && Size && Address + Size <= EMI_BASE + sizeof(Emi));
  int Failed = ++Model->Writes == Model->WriteFailure;
  memcpy(Emi + (Address - EMI_BASE), Data, Failed ? Size / 2 : Size);
  if (Model->Writes == Model->LoseResetAfter) Model->InReset = 0;
  if (Model->Writes == Model->LosePowerAfter) Model->Running = 0;
  return Failed;
}

static int ReadEmi(void *Context, unsigned long long Address, unsigned char *Data, unsigned Size) {
  MODEL *Model = Context;
  CHECK(Model->Barriers == Model->Writes && Size <= 256 && Size);
  CHECK(Address >= EMI_BASE && Address + Size <= EMI_BASE + sizeof(Emi));
  if (++Model->Reads == Model->ReadFailure) return 1;
  memcpy(Data, Emi + (Address - EMI_BASE), Size);
  if (Model->Reads == Model->BadReadback) Data[Size - 1] ^= 1;
  return 0;
}

static MTK_CONN_ROM_SET ReadImages(const char *Directory, const char *Variant) {
  MTK_CONN_ROM_SET Set;
  for (unsigned Index = 0; Index < MtkConnRomPartCount; ++Index) {
    char Path[1024];
    int Length = snprintf(Path, sizeof(Path), "%s/soc2_2_ram_%s_%s_1_hdr.bin",
                          Directory, Name[Index], Variant);
    CHECK(Length > 0 && (size_t)Length < sizeof(Path));
    FILE *File = fopen(Path, "rb");
    CHECK(File && !fseek(File, 0, SEEK_END));
    long Bytes = ftell(File);
    CHECK(Bytes > HEADER && Bytes < MTK_CONN_EMI_SIZE && !fseek(File, 0, SEEK_SET));
    unsigned char *Data = malloc((size_t)Bytes);
    CHECK(Data && fread(Data, 1, (size_t)Bytes, File) == (size_t)Bytes && !fclose(File));
    Set.Image[Index] = (MTK_CONN_ROM_IMAGE){Data, (unsigned)Bytes};
  }
  return Set;
}

static void Reset(MODEL *Model, MTK_CONN_POWER *Power) {
  *Model = (MODEL){.Window = EMI_BASE >> 16, .Running = 1, .InReset = 1, .FirmwareVersion = 0x8a00};
  const MTK_CONN_POWER_IO Io = {Model, ReadPower, WritePower, Wait, Barrier};
  CHECK(MtkConnPowerInit(Power, &Io) == MtkConnPowerOk);
  Power->IslandOwned = 1;
  Power->State = MtkConnOn;
  Power->TargetRemap[0] = Model->Window;
  Power->TargetRemap[1] = 0x1000;
}

static void CheckMemory(const MTK_CONN_ROM_SET *Set) {
  const unsigned Order[] = {MtkConnRomMcu, MtkConnRomBluetooth, MtkConnRomWifi};
  unsigned End = 0;
  for (unsigned Index = 0; Index < MtkConnRomPartCount; ++Index) {
    unsigned Part = Order[Index], Start = Offset[Part];
    for (; End < Start; ++End) assert(Emi[End] == GUARD);
    CHECK(!memcmp(Emi + Start, Set->Image[Part].Data + HEADER, Set->Image[Part].Bytes - HEADER));
    End = Start + Set->Image[Part].Bytes - HEADER;
  }
  for (; End < sizeof(Emi); ++End) assert(Emi[End] == GUARD);
}

/*
 * Use real power/ROM/boot cores and the same complete stock images above.
 * The model keeps CONN registers inaccessible until the domain is live.
 */
enum { BootSpm = MtkConnRfSpi + 1, BootInfra, BootEmi, BootBanks = BootEmi };
enum { ResetBit = 0x1000, CcifBit = 0x10000000 };
typedef struct {
  unsigned Region, Offset, Value;
} BOOT_WRITE;

typedef struct {
  unsigned Reg[BootBanks][0x1000 / sizeof(unsigned)];
  BOOT_WRITE Log[256];
  unsigned Writes, Fences, Copies, Hashes, Starts, StartsAtIdle;
  unsigned PinWait, PowerWait, IdleWait, FinalWait, IdleAfter;
  unsigned FailWrite, DropWrite, FailWaitUs, StaleIdle, LoseRemapAtIdle;
  unsigned FailCleanup;
  unsigned Chip, IgnoreFirst, Rails, RailCalls, FailRailCall, ProbeAttempts, ProbeWaits;
  unsigned RfAddress[6], RfTriggers, RfReads, RfTriggered, FailRfTrigger, FailProbePowerOff;
  unsigned LeadDelay, ReplyDelay, LeadWaits, ReplyWaits, TotalLeadWaits, TotalReplyWaits;
  unsigned RuntimeAdie, RailTime, RailReadyAt, FailVcn18Off, LoseVcn18AtCopy, LoseVcn18;
  unsigned short Pmic[0x2000 / 2];
} BOOT_MODEL;

#define REG(Model, Bank, Offset) ((Model)->Reg[(Bank)][(Offset) / sizeof(unsigned)])
#define PMIC(Model, Address) ((Model)->Pmic[(Address) / 2])

static int BootOn(const BOOT_MODEL *Model) {
  return (REG(Model, BootSpm, 0x304) & 0x1fU) == 0xd &&
         !(REG(Model, BootInfra, 0x220) & 0x46000U) &&
         !(REG(Model, BootInfra, 0x250) & 0x400U);
}

static int BootReset(const BOOT_MODEL *Model) {
  return (REG(Model, MtkConnRomRgu, 0x18) & ResetBit) != 0;
}

static int RuntimeRailsOn(const BOOT_MODEL *Model) {
  if (Model->LoseVcn18 || !(PMIC(Model, 0x1c58) & 1U) ||
      (PMIC(Model, 0x1e8c) & 0xf0fU) != 0xc00U || Model->RailTime < Model->RailReadyAt)
    return 0;
  if (Model->RuntimeAdie == 0x6631U)
    return (PMIC(Model, 0x1d1c) & 1U) && (PMIC(Model, 0x1e28) & 0x30fU) == 0x100U;
  return (PMIC(Model, 0x1724) & 0x10U) && (REG(Model, MtkConnGpio, 0x120) & 0x10U);
}

static void BootRecord(BOOT_MODEL *Model, unsigned Bank, unsigned Offset, unsigned Value) {
  CHECK(Model->Writes == Model->Fences && Model->Writes < sizeof(Model->Log) / sizeof(*Model->Log));
  Model->Log[Model->Writes++] = (BOOT_WRITE){Bank, Offset, Value};
}

static int BootRead32(void *Context, MTK_CONN_ROM_REGION Region, unsigned Offset, unsigned *Value) {
  BOOT_MODEL *Model = Context;
  CHECK(Model->Writes == Model->Fences && Region <= MtkConnRfSpi && Offset < 0x1000 && !(Offset & 3));
  if (Region >= MtkConnRomTop && Region <= MtkConnMcuMem) CHECK(BootOn(Model));
  *Value = REG(Model, Region, Offset);
  if (Region == MtkConnRfSpi) {
    CHECK(BootOn(Model) && BootReset(Model) && (REG(Model, MtkConnSysram, 0x500) & 1U));
    CHECK(Offset == 0 || Offset == 0x58);
    if (Offset == 0) {
      *Value = Model->RfTriggered ? (Model->ReplyWaits < Model->ReplyDelay ? 0x20U : 0) :
                                   (Model->LeadWaits < Model->LeadDelay ? 1U : 0);
    } else {
      CHECK(Model->RfTriggered && Model->ReplyWaits >= Model->ReplyDelay);
      ++Model->RfReads;
      unsigned Address = REG(Model, MtkConnRfSpi, 0x50);
      *Value = 0x1234;
      if (Model->ProbeAttempts > Model->IgnoreFirst &&
          ((Address == 0xb024 && Model->Chip == 0x6631) ||
           (Address == 0xb02c && Model->Chip == 0x6635)))
        *Value |= Model->Chip << 16;
    }
  }
  if (Region == MtkConnRomMcuCfg && Offset == 0x600) {
    *Value = 0;
    if ((BootReset(Model) && Model->StaleIdle) ||
        (!BootReset(Model) && (REG(Model, MtkConnHostCsr, 0x1c) & 0x80) &&
         Model->IdleWait >= Model->IdleAfter)) {
      *Value = 0x1d1e;
      if (!BootReset(Model)) Model->StartsAtIdle = Model->Starts;
    }
  }
  return 0;
}

static int BootWrite32(void *Context, MTK_CONN_ROM_REGION Region, unsigned Offset, unsigned Value) {
  BOOT_MODEL *Model = Context;
  unsigned Old = REG(Model, Region, Offset);
  CHECK(Region <= MtkConnRfSpi && Offset < 0x1000 && !(Offset & 3));
  BootRecord(Model, Region, Offset, Value);
  if (Model->Writes == Model->DropWrite) return 0;
  if (Region == MtkConnRomRgu) {
    CHECK(Offset == 0x18 && (Value & 0xff000000U) == 0x88000000U);
    CHECK((Value & 0x00ffefffU) == (Old & 0x00ffefffU));
    if (BootOn(Model) && (Old & ResetBit) && !(Value & ResetBit)) {
      CHECK(Model->Copies == 3 && Model->Hashes == 3 && Model->PowerWait);
      CHECK(!Model->RuntimeAdie || RuntimeRailsOn(Model));
      CHECK(REG(Model, MtkConnRomMcuCfg, 0x504) == 0xf0170000U);
      CHECK(REG(Model, MtkConnRomMcuCfg, 0x508) == 0xf02a0000U);
      CHECK(REG(Model, MtkConnRomMcuCfg, 0x440) == 0x90000002U);
      CHECK(REG(Model, MtkConnMcuMem, 0x284) == 0xa5a45a5aU);
      ++Model->Starts;
    }
    REG(Model, Region, Offset) = (Old & 0xff000000U) | (Value & 0xffffffU);
  } else if (Region == MtkConnRfSpi) {
    CHECK(BootOn(Model) && BootReset(Model) && (REG(Model, MtkConnSysram, 0x500) & 1U));
    if (Offset == 0x50) {
      CHECK(!Model->RfTriggered && Model->LeadWaits >= Model->LeadDelay);
      CHECK(Value == (Model->Rails & (1U << MtkConnProbeVcn13) ? 0xb02cU : 0xb024U));
      CHECK(Model->RfTriggers < sizeof(Model->RfAddress) / sizeof(*Model->RfAddress));
      Model->RfAddress[Model->RfTriggers] = Value;
    } else {
      CHECK(Offset == 0x54 && !Value && !Model->RfTriggered);
      CHECK(REG(Model, MtkConnRfSpi, 0x50) == Model->RfAddress[Model->RfTriggers]);
      Model->RfTriggered = 1;
      ++Model->RfTriggers;
      if (Model->FailRfTrigger) return 1;
    }
    REG(Model, Region, Offset) = Value;
  } else if (Region == MtkConnGpio &&
             (Offset == 0x54 || Offset == 0x58 || Offset == 0x154 || Offset == 0x158)) {
    CHECK(Value == 0x02000000U);
    unsigned Base = Offset & ~0xfU;
    if (Offset & 4) REG(Model, Region, Base) |= Value;
    else REG(Model, Region, Base) &= ~Value;
  } else {
    if (Region >= MtkConnRomTop && Region <= MtkConnMcuMem) {
      CHECK(BootOn(Model));
      CHECK(BootReset(Model) == (Region != MtkConnHostCsr));
    }
    REG(Model, Region, Offset) = Value;
  }
  return Model->Writes == Model->FailWrite ||
         (Model->FailCleanup && Region == MtkConnGpio && Offset == 0x460 && Value == 0x82a2000fU);
}

static int BootReadPower(void *Context, MTK_CONN_REGION Region, unsigned Offset, unsigned *Value) {
  BOOT_MODEL *Model = Context;
  CHECK(Model->Writes == Model->Fences && Offset < 0x1000 && !(Offset & 3));
  *Value = REG(Model, Region == MtkConnSpm ? BootSpm : BootInfra, Offset);
  return 0;
}

static int BootWritePower(void *Context, MTK_CONN_REGION Region, unsigned Offset, unsigned Value) {
  BOOT_MODEL *Model = Context;
  unsigned Bank = Region == MtkConnSpm ? BootSpm : BootInfra;
  BootRecord(Model, Bank, Offset, Value);
  if (Bank == BootSpm && Offset == 0) {
    CHECK(BootReset(Model) && REG(Model, BootSpm, 0x304) == 0x12);
    CHECK(Value == 0x0b165a51U);
    REG(Model, BootSpm, 0) = Value & 0xffffU;
  } else if (Bank == BootSpm) {
    CHECK(Offset == 0x304 && BootReset(Model) && (REG(Model, BootSpm, 0) & 1U));
    if ((Value & 0xc) == 4U && (REG(Model, Bank, Offset) & 0xc) == 0)
      CHECK(!Model->RuntimeAdie || RuntimeRailsOn(Model));
    REG(Model, Bank, Offset) = Value;
    REG(Model, Bank, 0x16c) = REG(Model, Bank, 0x170) = (Value & 0xc) == 0xc ? 2U : 0;
  } else {
    switch (Offset) {
    case 0x0c0: REG(Model, Bank, 0x0c8) |= Value; break;
    case 0x0c4: REG(Model, Bank, 0x0c8) &= ~Value; break;
    case 0x2a0: REG(Model, Bank, 0x220) |= Value; REG(Model, Bank, 0x228) |= Value; break;
    case 0x2a4: REG(Model, Bank, 0x220) &= ~Value; REG(Model, Bank, 0x228) &= ~Value; break;
    case 0x2a8: REG(Model, Bank, 0x250) |= Value; REG(Model, Bank, 0x258) |= Value; break;
    case 0x2ac: REG(Model, Bank, 0x250) &= ~Value; REG(Model, Bank, 0x258) &= ~Value; break;
    case 0x380: case 0x388: CHECK(!BootOn(Model)); break;
    case 0xc14: CHECK(BootOn(Model) && BootReset(Model) && Value == 0); break;
    default: CHECK(0);
    }
    REG(Model, Bank, Offset) = Value;
  }
  return Model->Writes == Model->FailWrite ||
         (Model->FailProbePowerOff && Offset == 0x2a0 && Model->RfTriggered);
}

static void BootFence(void *Context) {
  BOOT_MODEL *Model = Context;
  CHECK(++Model->Fences == Model->Writes);
}

static int BootWait(void *Context, unsigned Us) {
  BOOT_MODEL *Model = Context;
  CHECK(Model->Writes == Model->Fences);
  switch (Us) {
  case 150: CHECK(!BootOn(Model)); ++Model->PinWait; break;
  case 300:
    CHECK(!BootOn(Model) && !(Model->Rails & (1U << MtkConnProbeVcn13)));
    ++Model->ProbeWaits;
    break;
  case 500:
    CHECK(BootOn(Model) && BootReset(Model) && Model->Rails);
    if (Model->RfTriggered) { ++Model->ReplyWaits; ++Model->TotalReplyWaits; }
    else { ++Model->LeadWaits; ++Model->TotalLeadWaits; }
    break;
  case 100: case 10: CHECK(BootReset(Model)); break;
  case 5000:
    CHECK(BootOn(Model));
    if (BootReset(Model)) ++Model->PowerWait;
    else {
      ++Model->IdleWait;
      if (Model->LoseRemapAtIdle) REG(Model, BootInfra, 0x380) ^= 1;
    }
    break;
  case 20000:
    CHECK(BootOn(Model));
    if (!BootReset(Model)) ++Model->FinalWait;
    break;
  default: CHECK(0);
  }
  return Us == Model->FailWaitUs && (Us != 5000 || !BootReset(Model));
}

static int BootHash(void *Context, const unsigned char *Data, unsigned Size, unsigned char Digest[32]) {
  BOOT_MODEL *Model = Context;
  CHECK(BootOn(Model) && BootReset(Model));
  ++Model->Hashes;
  return SHA256(Data, Size, Digest) == NULL;
}

static int BootWriteEmi(void *Context, unsigned long long Address, const unsigned char *Data, unsigned Size) {
  BOOT_MODEL *Model = Context;
  CHECK(BootOn(Model) && BootReset(Model) && Model->Hashes == 3);
  CHECK(Model->Copies < 3 && Address == EMI_BASE + Offset[Model->Copies] &&
        Size <= MTK_CONN_EMI_SIZE - Offset[Model->Copies]);
  ++Model->Copies;
  if (Model->LoseVcn18AtCopy == Model->Copies) Model->LoseVcn18 = 1;
  BootRecord(Model, BootEmi, (unsigned)(Address - EMI_BASE), Size);
  memcpy(Emi + (Address - EMI_BASE), Data, Size);
  return Model->Writes == Model->FailWrite;
}

static int BootReadEmi(void *Context, unsigned long long Address, unsigned char *Data, unsigned Size) {
  BOOT_MODEL *Model = Context;
  CHECK(BootOn(Model) && BootReset(Model) && Model->Writes == Model->Fences);
  CHECK(Size && Size <= 256 && Address >= EMI_BASE && Address + Size <= EMI_BASE + sizeof(Emi));
  memcpy(Data, Emi + (Address - EMI_BASE), Size);
  return 0;
}

static void ResetBoot(BOOT_MODEL *Model, MTK_CONN_BOOT *Boot) {
  memset(Model, 0, sizeof(*Model));
  REG(Model, BootSpm, 0) = 0x5a50;
  REG(Model, BootSpm, 0x304) = 0x12;
  REG(Model, BootInfra, 0x220) = REG(Model, BootInfra, 0x228) = 0x46040;
  REG(Model, BootInfra, 0x250) = REG(Model, BootInfra, 0x258) = 0x402;
  REG(Model, BootInfra, 0xc8) = CcifBit | 0x8000U;
  REG(Model, BootInfra, 0x380) = 0xdab06300;
  REG(Model, BootInfra, 0x388) = 0x4ff04400;
  REG(Model, BootInfra, 0xc14) = 0x11223344;
  REG(Model, MtkConnRomRgu, 0x18) = 0x92344021;
  REG(Model, MtkConnRomTop, 0x10) = 0x100c0000;
  REG(Model, MtkConnRomMcuCfg, 4) = 0x8a00;
  REG(Model, MtkConnGpio, 0x460) = 0x82a2000fU;
  REG(Model, MtkConnGpio, 0x470) = 0x2011a82d;
  REG(Model, MtkConnGpio, 0x50) = 0x04040000;
  REG(Model, MtkConnGpio, 0x150) = 0x10006000;
  REG(Model, MtkConnIoCfg, 0x10) = 0xf4a65555;
  REG(Model, MtkConnSysram, 0x130) = 0xa4b0f185;
  REG(Model, MtkConnSysram, 0x168) = 0xa16e0b60;
  REG(Model, MtkConnSysram, 0x200) = 0x13336666;
  REG(Model, MtkConnSysram, 0x204) = 0xa4c28ba5;
  REG(Model, MtkConnSysram, 0x240) = 0x650519cc;
  REG(Model, MtkConnMcuMem, 0x284) = 0xa5a55a5a;
  REG(Model, MtkConnHostCsr, 0x1c) = 0xabcdef01 & ~0x80U;
  REG(Model, MtkConnSysram, 0x500) = 0x4a38;
  const MTK_CONN_POWER_IO PowerIo = {Model, BootReadPower, BootWritePower, BootWait, BootFence};
  const MTK_CONN_BOOT_IO Io = {
    {Model, BootRead32, BootHash, BootWriteEmi, BootReadEmi}, BootWrite32
  };
  CHECK(MtkConnBootInit(Boot, &PowerIo, &Io) == MtkConnPowerOk);
  CHECK(MtkConnBootCheck(Boot) == MtkConnPowerNotReady && !Model->Writes);
}

static void CheckClosed(BOOT_MODEL *Model, MTK_CONN_BOOT *Boot) {
  CHECK(Boot->Power.State == MtkConnOff && !Boot->ResetOwned && !Boot->PinsOwned);
  CHECK(!BootOn(Model) && REG(Model, BootSpm, 0x304) == 0x12);
  CHECK(!Boot->RuntimeRails && !Boot->ProbeRails && !Model->Rails &&
        REG(Model, MtkConnSysram, 0x500) == 0x4a38);
  CHECK(REG(Model, BootInfra, 0x220) == 0x46040 && REG(Model, BootInfra, 0x228) == 0x46040);
  CHECK(REG(Model, BootInfra, 0x250) == 0x402 && REG(Model, BootInfra, 0x258) == 0x402);
  CHECK(REG(Model, BootInfra, 0xc8) == (CcifBit | 0x8000U));
  CHECK(REG(Model, BootInfra, 0x380) == 0xdab06300 && REG(Model, BootInfra, 0x388) == 0x4ff04400);
  CHECK(REG(Model, MtkConnRomRgu, 0x18) == 0x92344021);
  CHECK(REG(Model, MtkConnGpio, 0x460) == 0x82a2000fU && REG(Model, MtkConnGpio, 0x470) == 0x2011a82d);
  CHECK(REG(Model, MtkConnGpio, 0x50) == 0x04040000 && REG(Model, MtkConnGpio, 0x150) == 0x10006000);
  CHECK(REG(Model, MtkConnIoCfg, 0x10) == 0xf4a65555U);
  CHECK(MtkConnBootCheck(Boot) == MtkConnPowerNotReady);
  unsigned Writes = Model->Writes;
  CHECK(MtkConnBootStop(Boot) == MtkConnPowerOk && Model->Writes == Writes);
}

static void CheckStockTrace(const BOOT_MODEL *Model, unsigned Variant) {
  // MMIO writes from isolated consys_*_mt6789_gen in stock wmt_drv.ko.
  const BOOT_WRITE Expected[] = {
    {MtkConnGpio, 0x460, 0x9191111fU},
    {MtkConnGpio, 0x470, 0x2011a819U},
    {MtkConnIoCfg, 0x10, 0xc000904dU},
    {MtkConnRomRgu, 0x18, 0x88345021U},
    {BootSpm, 0, 0x0b165a51U},
    {MtkConnSysram, 0x168, 0xa16e0b61U},
    {MtkConnRomMcuCfg, 0x504, 0xf0170000U},
    {MtkConnRomMcuCfg, 0x508, 0xf02a0000U},
    {MtkConnSysram, 0x240, 0x6505194cU},
    {MtkConnSysram, 0x204, 0xa4c206a5U},
    {MtkConnSysram, 0x200, 0x13330708U},
    {MtkConnSysram, 0x204, 0xa4c206a4U},
    {MtkConnSysram, 0x130, Variant ? 0xa4b0f085U : 0xa4b0f185U},
    {MtkConnGpio, 0x54, 0x02000000U}, {MtkConnGpio, 0x154, 0x02000000U},
    {MtkConnGpio, 0x470, 0x2011a809U},
    {MtkConnRomMcuCfg, 0x440, 0x90000002U},
    {MtkConnMcuMem, 0x284, 0xa5a45a5aU},
    {MtkConnRomRgu, 0x18, 0x88344021U},
    {MtkConnHostCsr, 0x1c, 0xabcdef81U},
    {MtkConnGpio, 0x470, 0x2011a819U}
  };
  unsigned Index = 0;
  for (unsigned Step = 0; Step < Model->Writes; ++Step) {
    const BOOT_WRITE *Write = &Model->Log[Step];
    if (Write->Region == BootInfra || Write->Region == BootEmi ||
        (Write->Region == BootSpm && Write->Offset))
      continue;
    if (!Variant && Index == 13) Index = 16;
    CHECK(Index < sizeof(Expected) / sizeof(Expected[0]));
    CHECK(Write->Region == Expected[Index].Region && Write->Offset == Expected[Index].Offset &&
          Write->Value == Expected[Index].Value);
    ++Index;
  }
  CHECK(Index == (Variant ? 21U : 20U));
}

static void TestBoot(const MTK_CONN_ROM_SET Set[2]) {
  BOOT_MODEL Model;
  MTK_CONN_BOOT Boot;
  const unsigned Adie[] = {0x6631, 0x6635};
  unsigned StartWrites, StopWrites, FirstEntry = 0;
  for (unsigned Variant = 0; Variant < 2; ++Variant) {
    ResetBoot(&Model, &Boot);
    Model.IdleAfter = Variant ? 41 : 0;
    memset(Emi, GUARD, sizeof(Emi));
    CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, Adie[Variant], &Set[Variant]) == MtkConnPowerOk);
    CheckMemory(&Set[Variant]);
    CHECK(MtkConnBootCheck(&Boot) == MtkConnPowerOk && Model.Starts == 1 && Model.StartsAtIdle == 1);
    CHECK(Model.PinWait == 1 && Model.PowerWait == 1 && Model.IdleWait == Model.IdleAfter && Model.FinalWait == 1);
    CHECK(REG(&Model, BootSpm, 0) == 0x5a51U && REG(&Model, BootInfra, 0xc14) == 0);
    CHECK((REG(&Model, MtkConnSysram, 0x168) & 1U) != 0);
    CHECK(REG(&Model, MtkConnSysram, 0x130) == (Variant ? 0xa4b0f085U : 0xa4b0f185U));
    CHECK(REG(&Model, MtkConnSysram, 0x200) == 0x13330708U);
    CHECK(REG(&Model, MtkConnSysram, 0x204) == 0xa4c206a4U);
    CHECK(REG(&Model, MtkConnSysram, 0x240) == 0x6505194cU);
    CHECK((REG(&Model, MtkConnGpio, 0x470) & 0x77U) == 0x11U);
    CHECK((REG(&Model, MtkConnGpio, 0x50) & 0x02000000U) == (Variant ? 0x02000000U : 0));
    CHECK((REG(&Model, MtkConnGpio, 0x150) & 0x02000000U) == (Variant ? 0x02000000U : 0));
    CheckStockTrace(&Model, Variant);
    for (unsigned Index = 0; Index < Model.Writes; ++Index)
      if (Model.Log[Index].Region == MtkConnRomMcuCfg && Model.Log[Index].Offset == 0x504)
        FirstEntry = Index + 1;
    CHECK(FirstEntry && Model.Log[FirstEntry - 2].Region == MtkConnSysram &&
          Model.Log[FirstEntry - 2].Offset == 0x168 && Model.Log[FirstEntry - 3].Region == BootEmi);
    StartWrites = Model.Writes;
    CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, Adie[Variant], &Set[Variant]) == MtkConnPowerBusy);
    CHECK(Model.Writes == StartWrites);
    CHECK(MtkConnBootStop(&Boot) == MtkConnPowerOk);
    StopWrites = Model.Writes;
    CHECK(Model.Log[StartWrites].Region == MtkConnRomRgu && Model.Log[StartWrites].Value == 0x88345021U);
    CheckClosed(&Model, &Boot);
  }

  for (unsigned Step = 1; Step <= StartWrites; ++Step) {
    ResetBoot(&Model, &Boot);
    Model.FailWrite = Step;
    CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, 0x6635, &Set[1]) == MtkConnPowerIoError);
    CheckClosed(&Model, &Boot);
  }
  for (unsigned Step = StartWrites + 1; Step <= StopWrites; ++Step) {
    ResetBoot(&Model, &Boot);
    CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, 0x6635, &Set[1]) == MtkConnPowerOk);
    Model.FailWrite = Step;
    CHECK(MtkConnBootStop(&Boot) == MtkConnPowerIoError && (Boot.ResetOwned || Boot.PinsOwned));
    CHECK(MtkConnBootCheck(&Boot) == MtkConnPowerNotReady);
    CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, 0x6635, &Set[1]) == MtkConnPowerBusy);
    CHECK(MtkConnBootStop(&Boot) == MtkConnPowerOk);
    CheckClosed(&Model, &Boot);
  }

  ResetBoot(&Model, &Boot);
  CHECK(MtkConnBootStart(&Boot, EMI_BASE + 1, MTK_CONN_EMI_SIZE, 0x6631, Set) == MtkConnPowerInvalid);
  REG(&Model, BootSpm, 0x16c) = 2;
  CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, 0x6631, Set) == MtkConnPowerNotReady);
  CHECK(!Model.Writes && !Model.Hashes);

  ResetBoot(&Model, &Boot);
  CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, 0x6635, Set) == MtkConnPowerInvalid);
  CHECK(!Model.Starts && !Model.Copies);
  CheckClosed(&Model, &Boot);

  ResetBoot(&Model, &Boot);
  Model.StaleIdle = 1;
  CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, 0x6635, &Set[1]) == MtkConnPowerNotReady);
  CHECK(!Model.Starts && !Model.StartsAtIdle);
  CheckClosed(&Model, &Boot);

  ResetBoot(&Model, &Boot);
  Model.DropWrite = FirstEntry;
  CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, 0x6635, &Set[1]) == MtkConnPowerIoError);
  CHECK(!Model.Starts);
  CheckClosed(&Model, &Boot);

  ResetBoot(&Model, &Boot);
  Model.IdleAfter = UINT_MAX;
  CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, 0x6635, &Set[1]) == MtkConnPowerTimeout);
  CHECK(Model.IdleWait == 41 && Model.Starts == 1 && !Model.StartsAtIdle && !Model.FinalWait);
  CheckClosed(&Model, &Boot);

  ResetBoot(&Model, &Boot);
  Model.IdleAfter = 1;
  Model.LoseRemapAtIdle = 1;
  CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, 0x6635, &Set[1]) == MtkConnPowerNotReady);
  CHECK(Model.IdleWait == 1 && !Model.StartsAtIdle);
  CheckClosed(&Model, &Boot);

  ResetBoot(&Model, &Boot);
  Model.IdleAfter = 1;
  Model.FailWaitUs = 5000;
  Model.FailCleanup = 1;
  CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, 0x6635, &Set[1]) == MtkConnPowerIoError);
  CHECK(!BootOn(&Model) && BootReset(&Model) && Boot.PinsOwned && Boot.ResetOwned);
  CHECK(MtkConnBootCheck(&Boot) == MtkConnPowerNotReady);
  Model.FailCleanup = 0;
  CHECK(MtkConnBootStop(&Boot) == MtkConnPowerOk);
  CheckClosed(&Model, &Boot);
}

static int ProbeRail(void *Context, MTK_CONN_PROBE_RAIL Rail, int Enable) {
  BOOT_MODEL *Model = Context;
  unsigned Bit = 1U << Rail;
  CHECK(Rail == MtkConnProbeVcn18 || Rail == MtkConnProbeVcn13);
  CHECK(REG(Model, BootSpm, 0x304) == 0x12 && !BootOn(Model));
  if (Rail == MtkConnProbeVcn13)
    CHECK(Model->Rails & (1U << MtkConnProbeVcn18));
  if (Enable) {
    CHECK(!(Model->Rails & Bit));
    if (Rail == MtkConnProbeVcn18) {
      CHECK(!Model->Rails);
      ++Model->ProbeAttempts;
      Model->RfTriggered = Model->LeadWaits = Model->ReplyWaits = 0;
    } else {
      CHECK(Model->ProbeWaits == 2 * Model->ProbeAttempts - 1);
    }
    Model->Rails |= Bit;
  } else {
    if (Rail == MtkConnProbeVcn18) CHECK(!(Model->Rails & (1U << MtkConnProbeVcn13)));
    Model->Rails &= ~Bit;
  }
  // Like a PMIC write, the rail transition can finish despite an IO error.
  return ++Model->RailCalls == Model->FailRailCall;
}

static int ReadRailPmic(void *Context, unsigned Address, unsigned *Value) {
  BOOT_MODEL *Model = Context;
  CHECK(Address < 0x2000 && !(Address & 1U));
  *Value = PMIC(Model, Address);
  if (Address == 0x1c66)
    *Value = !Model->LoseVcn18 && (PMIC(Model, 0x1c58) & 1U) &&
             Model->RailTime >= Model->RailReadyAt ? 0x8000U : 0;
  if (Address == 0x1d2c)
    *Value = PMIC(Model, 0x1d1c) & 1U ? 0x8000U : 0;
  return 0;
}

static int WriteRailPmic(void *Context, unsigned Address, unsigned Value) {
  BOOT_MODEL *Model = Context;
  CHECK(!BootOn(Model) && Address < 0x2000 && !(Address & 1U) && Value <= 0xffffU);
  if (Address == 0x1c58 && (Value & 1U)) Model->RailReadyAt = Model->RailTime + 270U;
  if (Address == 0x1c58 && !(Value & 1U) && Model->FailVcn18Off) return 1;
  if (Address == 0x1726 || Address == 0x1728) {
    if (Address == 0x1726) PMIC(Model, 0x1724) |= Value;
    else PMIC(Model, 0x1724) &= ~Value;
  } else {
    PMIC(Model, Address) = (unsigned short)Value;
  }
  if (!Model->RuntimeAdie && Address == 0x1c58)
    return ProbeRail(Model, MtkConnProbeVcn18, Value & 1U);
  return 0;
}

static int ReadRailPin(void *Context, unsigned Offset, unsigned *Value) {
  BOOT_MODEL *Model = Context;
  CHECK(Offset == 0x20 || Offset == 0x120 || Offset == 0x380);
  *Value = REG(Model, MtkConnGpio, Offset);
  return 0;
}

static int WriteRailPin(void *Context, unsigned Offset, unsigned Value) {
  BOOT_MODEL *Model = Context;
  unsigned Before;
  CHECK(!BootOn(Model) && !ReadRailPin(Model, Offset, &Before));
  REG(Model, MtkConnGpio, Offset) = Value;
  if (!Model->RuntimeAdie && Offset == 0x120 && ((Before ^ Value) & 0x10U))
    return ProbeRail(Model, MtkConnProbeVcn13, (Value & 0x10U) != 0);
  return 0;
}

static int WaitRail(void *Context, unsigned Us) {
  BOOT_MODEL *Model = Context;
  CHECK(!BootOn(Model) && (Us == 270U || Us == 10U || Us == 300U));
  if (Us == 300U) {
    CHECK(!(PMIC(Model, 0x1d1c) & 1U) && !(PMIC(Model, 0x1724) & 0x10U));
    CHECK(!(REG(Model, MtkConnGpio, 0x120) & 0x10U));
  }
  Model->RailTime += Us;
  return 0;
}

static int AuthorizeRail(void *Context, MTK_CONN_RAIL Rail) {
  (void)Context;
  return Rail == MtkConnProbeVcn18 || Rail == MtkConnProbeVcn13 ||
         Rail == MtkConnRuntimeVcn18 || Rail == MtkConnRuntimeVcn33;
}

static void TestManagedBoot(const MTK_CONN_ROM_SET Set[2]) {
  BOOT_MODEL Model;
  MTK_CONN_BOOT Boot;
  const MTK_CONN_RAIL_IO Io = {
    &Model, ReadRailPmic, WriteRailPmic, ReadRailPin, WriteRailPin, WaitRail, AuthorizeRail
  };
  for (unsigned Variant = 0; Variant < 2; ++Variant) {
    MTK_CONN_RAILS Rails = {0};
    const MTK_CONN_PROBE_IO ProbeIo = {&Rails, MtkConnRailsSetProbeRail};
    unsigned Chip = 0;
    ResetBoot(&Model, &Boot);
    PMIC(&Model, 0x1720) = 1;
    Model.Chip = Variant ? 0x6635U : 0x6631U;
    CHECK(MtkConnRailsInit(&Rails, &Io) == MtkConnPowerOk);
    CHECK(MtkConnBootProbeAdie(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, &ProbeIo, &Chip) == MtkConnPowerOk);
    CHECK(Chip == Model.Chip && !PMIC(&Model, 0x1e8c));
    CheckClosed(&Model, &Boot);
    Model.RuntimeAdie = Chip;
    CHECK(MtkConnBootStartPowered(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, Chip, &Set[Variant], &Rails) ==
          MtkConnPowerOk);
    CHECK(Model.Starts == 1 && Model.Copies == 3 && MtkConnBootCheck(&Boot) == MtkConnPowerOk);
    Model.LoseVcn18 = 1;
    CHECK(MtkConnBootCheck(&Boot) == MtkConnPowerNotReady);
    Model.FailVcn18Off = 1;
    CHECK(MtkConnBootStop(&Boot) == MtkConnPowerIoError);
    CHECK(!BootOn(&Model) && Boot.RuntimeRails == &Rails && Rails.RuntimeOwner == &Boot);
    CHECK(MtkConnBootStartPowered(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, Chip, &Set[Variant], &Rails) ==
          MtkConnPowerBusy);
    Model.FailVcn18Off = 0;
    CHECK(MtkConnBootStop(&Boot) == MtkConnPowerOk);
    CHECK(!Rails.RuntimeOwner && !PMIC(&Model, 0x1c58) && !PMIC(&Model, 0x1e8c));
    CHECK(!PMIC(&Model, 0x1d1c) && !PMIC(&Model, 0x1e28) && !PMIC(&Model, 0x1724));
    CheckClosed(&Model, &Boot);

    ResetBoot(&Model, &Boot);
    PMIC(&Model, 0x1720) = 1;
    Model.RuntimeAdie = Chip;
    Model.LoseVcn18AtCopy = 3;
    CHECK(MtkConnBootStartPowered(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, Chip, &Set[Variant], &Rails) ==
          MtkConnPowerNotReady);
    CHECK(!Model.Starts && Model.Copies == 3 && !Rails.RuntimeOwner && !PMIC(&Model, 0x1c58));
    CheckClosed(&Model, &Boot);
  }
}

static void TestAdie(const MTK_CONN_ROM_SET Set[2]) {
  BOOT_MODEL Model;
  MTK_CONN_BOOT Boot;
  const MTK_CONN_PROBE_IO Io = {&Model, ProbeRail};
  unsigned Chip;

  for (unsigned Variant = 0; Variant < 2; ++Variant) {
    ResetBoot(&Model, &Boot);
    Model.Chip = Variant ? 0x6635 : 0x6631;
    Model.LeadDelay = Variant ? 2 : 0;
    Model.ReplyDelay = Variant ? 10 : 0;
    CHECK(MtkConnBootProbeAdie(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, &Io, &Chip) == MtkConnPowerOk);
    CHECK(Chip == Model.Chip && Model.ProbeAttempts == Variant + 1);
    CHECK(Model.RailCalls == (Variant ? 6U : 2U) && Model.ProbeWaits == 2 * Model.ProbeAttempts);
    CHECK(Model.RfTriggers == Model.ProbeAttempts && Model.RfReads == Model.RfTriggers);
    CHECK(Model.RfAddress[0] == 0xb024 && (!Variant || Model.RfAddress[1] == 0xb02c));
    CHECK(Model.TotalLeadWaits == Model.LeadDelay * Model.ProbeAttempts);
    CHECK(Model.TotalReplyWaits == Model.ReplyDelay * Model.ProbeAttempts);
    CHECK(!Model.Hashes && !Model.Copies && !Model.Starts);
    CheckClosed(&Model, &Boot);
    CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, Chip, &Set[Variant]) == MtkConnPowerOk);
    CHECK(Model.Copies == 3 && Model.Starts == 1 && MtkConnBootCheck(&Boot) == MtkConnPowerOk);
    CHECK(MtkConnBootStop(&Boot) == MtkConnPowerOk);
    CheckClosed(&Model, &Boot);
  }

  ResetBoot(&Model, &Boot);
  Model.Chip = 0x6631;
  Model.IgnoreFirst = 4;
  CHECK(MtkConnBootProbeAdie(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, &Io, &Chip) == MtkConnPowerOk);
  CHECK(Chip == 0x6631 && Model.ProbeAttempts == 5 && Model.RfTriggers == 5);
  for (unsigned Attempt = 0; Attempt < 5; ++Attempt)
    CHECK(Model.RfAddress[Attempt] == (Attempt & 1 ? 0xb02cU : 0xb024U));
  CheckClosed(&Model, &Boot);

  ResetBoot(&Model, &Boot);
  CHECK(MtkConnBootProbeAdie(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, &Io, &Chip) == MtkConnPowerNotReady);
  CHECK(!Chip && Model.ProbeAttempts == 6 && Model.RfReads == 6 && !Model.Starts);
  CheckClosed(&Model, &Boot);

  for (unsigned AfterTrigger = 0; AfterTrigger < 2; ++AfterTrigger) {
    ResetBoot(&Model, &Boot);
    Model.Chip = 0x6635;
    Model.LeadDelay = AfterTrigger ? 0 : UINT_MAX;
    Model.ReplyDelay = AfterTrigger ? UINT_MAX : 0;
    CHECK(MtkConnBootProbeAdie(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, &Io, &Chip) == MtkConnPowerTimeout);
    CHECK(!Chip && Model.ProbeAttempts == 6 && Model.RfTriggers == 6 * AfterTrigger && !Model.RfReads);
    CHECK(Model.TotalLeadWaits == 60 * (1 - AfterTrigger) && Model.TotalReplyWaits == 60 * AfterTrigger);
    CheckClosed(&Model, &Boot);
  }

  ResetBoot(&Model, &Boot);
  Model.FailRfTrigger = 1;
  CHECK(MtkConnBootProbeAdie(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, &Io, &Chip) == MtkConnPowerIoError);
  CHECK(!Chip && Model.RfTriggers == 1 && !Model.RfReads);
  CheckClosed(&Model, &Boot);

  ResetBoot(&Model, &Boot);
  Model.FailRailCall = 1;
  CHECK(MtkConnBootProbeAdie(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, &Io, &Chip) == MtkConnPowerIoError);
  CHECK(!Chip && Model.RailCalls == 2 && !Model.Writes);
  CheckClosed(&Model, &Boot);

  ResetBoot(&Model, &Boot);
  Model.Chip = 0x6635;
  Model.FailRailCall = 5;
  CHECK(MtkConnBootProbeAdie(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, &Io, &Chip) == MtkConnPowerIoError);
  CHECK(!Chip && Boot.ProbeRails == 3 && !BootOn(&Model) && !Boot.ResetOwned);
  CHECK(MtkConnBootStart(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, 0x6635, &Set[1]) == MtkConnPowerBusy);
  CHECK(MtkConnBootProbeAdie(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, &Io, &Chip) == MtkConnPowerBusy);
  CHECK(MtkConnBootStop(&Boot) == MtkConnPowerOk && Model.RailCalls == 7);
  CheckClosed(&Model, &Boot);

  ResetBoot(&Model, &Boot);
  Model.Chip = 0x6631;
  Model.FailProbePowerOff = 1;
  CHECK(MtkConnBootProbeAdie(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, &Io, &Chip) == MtkConnPowerIoError);
  CHECK(!Chip && Model.Rails == 1 && Model.RailCalls == 1 && BootReset(&Model));
  Model.FailProbePowerOff = 0;
  CHECK(MtkConnBootStop(&Boot) == MtkConnPowerOk);
  CheckClosed(&Model, &Boot);

  ResetBoot(&Model, &Boot);
  REG(&Model, BootSpm, 0x170) = 2;
  CHECK(MtkConnBootProbeAdie(&Boot, EMI_BASE, MTK_CONN_EMI_SIZE, &Io, &Chip) == MtkConnPowerNotReady);
  CHECK(!Chip && !Model.RailCalls && !Model.Writes);
}

int main(int Argc, char **Argv) {
  CHECK(Argc == 2);
  const unsigned Adie[] = {0x6631, 0x6635};
  MTK_CONN_ROM_SET Set[] = {ReadImages(Argv[1], "1d"), ReadImages(Argv[1], "1e")};
  MODEL Model;
  MTK_CONN_POWER Power;
  const MTK_CONN_ROM_IO Io = {&Model, ReadRom, Hash, WriteEmi, ReadEmi};
  for (unsigned Variant = 0; Variant < 2; ++Variant) {
    Reset(&Model, &Power);
    Model.IpDelay = Variant ? 10 : 0;
    memset(Emi, GUARD, sizeof(Emi));
    CHECK(MtkConnRomLoad(&Power, &Io, Adie[Variant], &Set[Variant]) == MtkConnPowerOk);
    CHECK(Model.Hashes == 3 && Model.Writes == 3 && Model.InReset);
    CHECK(Model.Waits == Model.IpDelay && Model.VersionReads == Model.IpDelay + 1);
    CheckMemory(&Set[Variant]);

    Reset(&Model, &Power);
    CHECK(MtkConnRomLoad(&Power, &Io, Adie[Variant], &Set[1 - Variant]) == MtkConnPowerInvalid);
    CHECK(!Model.Writes && !Model.PowerReads);

    Reset(&Model, &Power);
    MTK_CONN_ROM_IMAGE *Mcu = &Set[Variant].Image[MtkConnRomMcu];
    unsigned char *Tail = (unsigned char *)Mcu->Data + Mcu->Bytes - 1;
    *Tail ^= 1;
    CHECK(MtkConnRomLoad(&Power, &Io, Adie[Variant], &Set[Variant]) == MtkConnPowerInvalid);
    CHECK(!Model.Writes && Model.Hashes == 3 && !Model.PowerReads);
    *Tail ^= 1;
  }

  Reset(&Model, &Power);
  CHECK(MtkConnRomLoad(&Power, &Io, 0x6630, Set) == MtkConnPowerInvalid);
  CHECK(!Model.Hashes && !Model.PowerReads && !Model.Writes);

  Reset(&Model, &Power);
  Power.IslandOwned = 0;
  CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerNotReady);
  CHECK(!Model.Hashes && !Model.PowerReads && !Model.Writes);

  Reset(&Model, &Power);
  Model.HashFailure = 3;
  CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerIoError);
  CHECK(!Model.PowerReads && !Model.Writes);

  Reset(&Model, &Power);
  Model.IpDelay = UINT_MAX;
  CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerTimeout);
  CHECK(Model.VersionReads == 11 && Model.Waits == 10 && !Model.Writes);

  Reset(&Model, &Power);
  Model.IpDelay = Model.WaitFailure = 1;
  CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerIoError);
  CHECK(Model.VersionReads == 1 && Model.Waits == 1 && !Model.Writes);

  Reset(&Model, &Power);
  Model.FirmwareVersion |= 1;
  CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerNotReady && !Model.Writes);

  Reset(&Model, &Power);
  Model.InReset = 0;
  CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerNotReady);
  CHECK(!Model.VersionReads && !Model.Writes);

  Reset(&Model, &Power);
  Model.ResetBusError = 1;
  CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerIoError && !Model.Writes);

  Reset(&Model, &Power);
  Model.Window++;
  CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerNotReady && !Model.Writes);

  for (unsigned Part = 1; Part <= MtkConnRomPartCount; ++Part) {
    Reset(&Model, &Power);
    Model.LoseResetAfter = Part;
    CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerNotReady && Model.Writes == Part);

    Reset(&Model, &Power);
    Model.LosePowerAfter = Part;
    CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerNotReady && Model.Writes == Part);

    Reset(&Model, &Power);
    Model.WriteFailure = Part;
    memset(Emi, GUARD, sizeof(Emi));
    CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerIoError && Model.Writes == Part);
    CHECK(Model.Barriers == Model.Writes && Model.InReset);
    Reset(&Model, &Power);
    CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerOk);
    CheckMemory(Set);
  }

  Reset(&Model, &Power);
  Model.BadReadback = 2;
  CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerIoError && Model.Writes == 1);
  Reset(&Model, &Power);
  Model.ReadFailure = 2;
  CHECK(MtkConnRomLoad(&Power, &Io, *Adie, Set) == MtkConnPowerIoError && Model.Writes == 1);

  printf("CONN ROM: %u checks passed\n", Checks);
  Checks = 0;
  TestBoot(Set);
  printf("CONN boot: %u checks passed\n", Checks);
  Checks = 0;
  TestAdie(Set);
  printf("CONN ADIE: %u checks passed\n", Checks);
  Checks = 0;
  TestManagedBoot(Set);

  for (unsigned Variant = 0; Variant < 2; ++Variant)
    for (unsigned Part = 0; Part < MtkConnRomPartCount; ++Part)
      free((void *)Set[Variant].Image[Part].Data);
  printf("CONN managed probe and boot: %u checks passed\n", Checks);
  return 0;
}
