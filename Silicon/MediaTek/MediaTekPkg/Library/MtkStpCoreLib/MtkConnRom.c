// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/MtkConnRom.h>

#define MCU_RESET_REG    0x18U
#define MCU_RESET_BIT    0x1000U
#define IP_VERSION_REG   0x10U
#define IP_VERSION       0x100c0000U
#define FW_VERSION_REG   0x04U
#define ROM_HEADER_BYTES 48U
#define IP_RETRIES       10U
#define IP_RETRY_US      20000U

typedef struct {
  unsigned Bytes;
  unsigned char Digest[32];
} CONN_ROM_IDENTITY;

typedef struct {
  unsigned Offset;
  CONN_ROM_IDENTITY Variant[2];
} CONN_ROM_PART;

// Original wmt_drv.ko takes the low 24 bits of the address from the WMT
// loader, which clears its low byte. The copy starts after the 48-byte header.
// Match the exact T70 Pro blobs so the validated offset never comes from input.
static const CONN_ROM_PART Parts[MtkConnRomPartCount] = {
  {0x170000U, {
    {689840, {0x51, 0x68, 0xbd, 0xb4, 0xc9, 0xc3, 0x42, 0x10, 0x15, 0xa4, 0x45, 0x5d, 0xec, 0x55, 0xc9, 0x5c,
              0x70, 0x2e, 0x52, 0x7c, 0x40, 0x15, 0xc7, 0xf4, 0x37, 0x35, 0xcc, 0x49, 0x8d, 0x94, 0x07, 0x2f}},
    {691248, {0x43, 0x72, 0xd8, 0x9d, 0x31, 0xe6, 0x0c, 0x9a, 0x06, 0x9b, 0x0d, 0xd2, 0x2c, 0x49, 0x59, 0xab,
              0x73, 0xa3, 0x1b, 0x81, 0x13, 0xeb, 0x64, 0x84, 0xa1, 0xa2, 0x80, 0x2f, 0x13, 0x4c, 0x46, 0x24}}
  }},
  {0x2a0000U, {
    {241680, {0x8f, 0x1c, 0xa0, 0x5e, 0x8f, 0x00, 0x46, 0x5d, 0x06, 0x93, 0x98, 0x86, 0x55, 0x4e, 0xa8, 0x3b,
              0x03, 0xe8, 0x8c, 0x32, 0x96, 0x45, 0x3f, 0xd5, 0xb3, 0xbc, 0x9b, 0x08, 0xdb, 0xdc, 0xd2, 0x33}},
    {243556, {0xcd, 0x0c, 0x46, 0xdf, 0x3b, 0xfd, 0x5b, 0x6d, 0x07, 0xaa, 0x4d, 0x74, 0x18, 0x59, 0x17, 0x0f,
              0xdb, 0x2a, 0x19, 0xc0, 0x17, 0xe6, 0xad, 0x02, 0x48, 0xec, 0x7e, 0x08, 0x2b, 0xc7, 0x39, 0xc8}}
  }},
  {0, {
    {198012, {0x2e, 0x73, 0x55, 0x83, 0x76, 0x20, 0x86, 0xd5, 0xbc, 0x91, 0xee, 0xfe, 0x04, 0x84, 0x19, 0xe6,
              0x24, 0xec, 0x6b, 0x3f, 0xcf, 0xa5, 0x5c, 0x4b, 0xf7, 0x52, 0x50, 0xa9, 0xca, 0x80, 0x98, 0x2f}},
    {197924, {0x4c, 0x1a, 0x4b, 0x1f, 0xc1, 0xa8, 0x65, 0x0d, 0x72, 0xc2, 0xd4, 0xf9, 0xac, 0x53, 0x15, 0x9e,
              0x48, 0xd6, 0x7d, 0x65, 0xba, 0xd8, 0xbc, 0xaf, 0xdb, 0x4d, 0x0c, 0x7d, 0x35, 0x7e, 0x82, 0xc0}}
  }}
};

unsigned MtkConnRomImageBytes(unsigned Adie, MTK_CONN_ROM_PART Part) {
  if ((unsigned)Part >= MtkConnRomPartCount || (Adie != 0x6631U && Adie != 0x6635U))
    return 0;
  return Parts[Part].Variant[Adie == 0x6635U].Bytes;
}

static int SameBytes(const unsigned char *Left, const unsigned char *Right, unsigned Size) {
  unsigned Index;
  for (Index = 0; Index < Size; ++Index) {
    if (Left[Index] != Right[Index]) return 0;
  }
  return 1;
}

static MTK_CONN_POWER_STATUS CheckReset(const MTK_CONN_POWER *Power, const MTK_CONN_ROM_IO *Io) {
  MTK_CONN_POWER_STATUS Status = MtkConnPowerCheck(Power);
  unsigned Reset;
  if (Status != MtkConnPowerOk) return Status;
  if (Io->Read32(Io->Context, MtkConnRomRgu, MCU_RESET_REG, &Reset) || Reset == ~0U)
    return MtkConnPowerIoError;
  return (Reset & MCU_RESET_BIT) ? MtkConnPowerOk : MtkConnPowerNotReady;
}

MTK_CONN_POWER_STATUS MtkConnRomWaitIp(const MTK_CONN_POWER *Power, const MTK_CONN_ROM_IO *Io) {
  unsigned Attempt, Version;
  if (!Power || !Io || !Io->Read32) return MtkConnPowerInvalid;
  for (Attempt = 0; ; ++Attempt) {
    MTK_CONN_POWER_STATUS Status = CheckReset(Power, Io);
    if (Status != MtkConnPowerOk) return Status;
    if (Io->Read32(Io->Context, MtkConnRomTop, IP_VERSION_REG, &Version))
      return MtkConnPowerIoError;
    if (Version == IP_VERSION) return MtkConnPowerOk;
    if (Attempt == IP_RETRIES) return MtkConnPowerTimeout;
    if (Power->Io.WaitUs(Power->Io.Context, IP_RETRY_US)) return MtkConnPowerIoError;
  }
}

static int LoadAndVerify(const MTK_CONN_POWER *Power, const MTK_CONN_ROM_IO *Io,
                         unsigned long long Address, const MTK_CONN_ROM_IMAGE *Image) {
  const unsigned char *Payload = Image->Data + ROM_HEADER_BYTES;
  unsigned Remaining = Image->Bytes - ROM_HEADER_BYTES;
  unsigned char Readback[256];
  int Error = Io->WriteEmi(Io->Context, Address, Payload, Remaining);
  Power->Io.Barrier(Power->Io.Context);
  if (Error) return 0;
  while (Remaining) {
    unsigned Size = Remaining < sizeof(Readback) ? Remaining : sizeof(Readback);
    if (Io->ReadEmi(Io->Context, Address, Readback, Size) ||
        !SameBytes(Payload, Readback, Size))
      return 0;
    Remaining -= Size;
    Address += Size;
    Payload += Size;
  }
  return 1;
}

MTK_CONN_POWER_STATUS MtkConnRomLoad(const MTK_CONN_POWER *Power, const MTK_CONN_ROM_IO *Io,
                                   unsigned Adie, const MTK_CONN_ROM_SET *Set) {
  unsigned Index, Variant, Version;
  unsigned long long Base;
  MTK_CONN_POWER_STATUS Status;
  if (!Power || !Io || !Io->Read32 || !Io->Sha256 || !Io->WriteEmi || !Io->ReadEmi || !Set)
    return MtkConnPowerInvalid;
  if (Adie == 0x6631U) Variant = 0;
  else if (Adie == 0x6635U) Variant = 1;
  else return MtkConnPowerInvalid;
  if (!Power->IslandOwned) return MtkConnPowerNotReady;
  for (Index = 0; Index < MtkConnRomPartCount; ++Index) {
    const MTK_CONN_ROM_IMAGE *Image = &Set->Image[Index];
    const CONN_ROM_IDENTITY *Identity = &Parts[Index].Variant[Variant];
    unsigned char Actual[32];
    if (!Image->Data || Image->Bytes != Identity->Bytes) return MtkConnPowerInvalid;
    if (Io->Sha256(Io->Context, Image->Data, Image->Bytes, Actual)) return MtkConnPowerIoError;
    if (!SameBytes(Actual, Identity->Digest, sizeof(Actual))) return MtkConnPowerInvalid;
  }
  Status = MtkConnRomWaitIp(Power, Io);
  if (Status != MtkConnPowerOk) return Status;
  if (Io->Read32(Io->Context, MtkConnRomMcuCfg, FW_VERSION_REG, &Version) || Version == ~0U)
    return MtkConnPowerIoError;
  // The stock WMT catalog compares the low byte of the firmware version.
  if (Version & 0xffU) return MtkConnPowerNotReady;
  Base = (unsigned long long)Power->TargetRemap[0] << 16;
  if (!Base || (Base & (MTK_CONN_EMI_ALIGN - 1U)) ||
      Base > MTK_CONN_EMI_LIMIT - MTK_CONN_EMI_SIZE)
    return MtkConnPowerInvalid;
  for (Index = 0; Index < MtkConnRomPartCount; ++Index) {
    Status = CheckReset(Power, Io);
    if (Status != MtkConnPowerOk) return Status;
    if (!LoadAndVerify(Power, Io, Base + Parts[Index].Offset, &Set->Image[Index]))
      return MtkConnPowerIoError;
  }
  return CheckReset(Power, Io);
}
