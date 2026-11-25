// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_CONN_ROM_H
#define MTK_CONN_ROM_H
#include <Library/MtkConnPower.h>

typedef enum {
  MtkConnRomBluetooth,
  MtkConnRomWifi,
  MtkConnRomMcu,
  MtkConnRomPartCount
} MTK_CONN_ROM_PART;

typedef struct {
  const unsigned char *Data;
  unsigned Bytes;
} MTK_CONN_ROM_IMAGE;

typedef struct {
  MTK_CONN_ROM_IMAGE Image[MtkConnRomPartCount];
} MTK_CONN_ROM_SET;

// Exact shipped length, or zero for an unsupported variant or part.
unsigned MtkConnRomImageBytes(unsigned Adie, MTK_CONN_ROM_PART Part);

typedef enum {
  MtkConnRomRgu,     // 0x10007000
  MtkConnRomTop,     // 0x180b1000
  MtkConnRomMcuCfg,  // 0x18002000
  // Additional register windows used by MtkConnBoot.
  MtkConnHostCsr,   // 0x18007000
  MtkConnSysram,   // 0x18004000
  MtkConnMcuMem,   // 0x180c0000
  MtkConnGpio,     // 0x10005000
  MtkConnIoCfg,    // 0x11ea0000
  MtkConnRfSpi     // 0x180c6000
} MTK_CONN_ROM_REGION;

typedef struct {
  void *Context;
  int (*Read32)(void *Context, MTK_CONN_ROM_REGION Region, unsigned Offset, unsigned *Value);
  int (*Sha256)(void *Context, const unsigned char *Data, unsigned Size, unsigned char Digest[32]);
  int (*WriteEmi)(void *Context, unsigned long long Address, const unsigned char *Data, unsigned Size);
  int (*ReadEmi)(void *Context, unsigned long long Address, unsigned char *Data, unsigned Size);
} MTK_CONN_ROM_IO;

// Wait for the MT6789 IP revision with power and MCU reset held.
MTK_CONN_POWER_STATUS MtkConnRomWaitIp(const MTK_CONN_POWER *Power, const MTK_CONN_ROM_IO *Io);

/*
 * Load the exact shipped MT6789 soc2_2 ROM images for the detected analog chip
 * (6631 or 6635) into the live CONN EMI window. All callbacks return zero on
 * success. Use the same exclusive owner and wait/barrier as MtkConnPower.
 * Keep the MCU in reset and all competing firmware/DMA stopped throughout.
 * Keep the EMI mapping pinned, make writes visible to CONN, and supply stable
 * non-aliasing image buffers until return. This call does not release reset.
 * After an error, keep reset asserted until recovery or a complete reload.
 */
MTK_CONN_POWER_STATUS MtkConnRomLoad(const MTK_CONN_POWER *Power, const MTK_CONN_ROM_IO *Io,
                                   unsigned Adie, const MTK_CONN_ROM_SET *Set);

#endif
