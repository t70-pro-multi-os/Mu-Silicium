// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_CONN_BOOT_H
#define MTK_CONN_BOOT_H

#include <Library/MtkConnRom.h>
#include <Library/MtkConnRails.h>

typedef struct {
  MTK_CONN_ROM_IO Rom;
  int (*Write32)(void *Context, MTK_CONN_ROM_REGION Region, unsigned Offset, unsigned Value);
} MTK_CONN_BOOT_IO;

typedef struct {
  MTK_CONN_POWER Power;
  MTK_CONN_BOOT_IO Io;
  MTK_CONN_PROBE_IO ProbeIo;
  MTK_CONN_RAILS *RuntimeRails;
  unsigned ProbeRails;
  unsigned SavedPins[5];
  unsigned PinsOwned;
  unsigned ResetOwned;
  unsigned SavedReset;
  unsigned Ready;
} MTK_CONN_BOOT;

/*
 * T70 Pro MT6789 CONN MCU startup with the stock non-RC, co-clock setup.
 * Initialize fresh storage and serialize all calls, including CONN clients
 * and shared GPIO, RGU, and SPM access. The platform supplies the detected
 * analog chip ID for Start, stable CONN clocks and reserved MMIO and EMI.
 * For Start it also keeps the analog supplies stable until Stop succeeds.
 * Write32 uses Rom.Context and returns zero on success.
 */
MTK_CONN_POWER_STATUS MtkConnBootInit(MTK_CONN_BOOT *Boot, const MTK_CONN_POWER_IO *PowerIo,
                                    const MTK_CONN_BOOT_IO *Io);
/*
 * Success means the MCU reached its startup idle state. Clients must still
 * initialize WMT and their own transports and radio functions. On failure
 * keep the supplied resources until Stop confirms shutdown and restoration.
 */
MTK_CONN_POWER_STATUS MtkConnBootStart(MTK_CONN_BOOT *Boot, unsigned long long EmiBase,
                                     unsigned EmiSize, unsigned Adie, const MTK_CONN_ROM_SET *Set);
/*
 * Start with managed T70 Pro PMIC/GPIO supplies for the detected chip.
 * Keep Rails and its resources alive until Stop releases them.
 */
MTK_CONN_POWER_STATUS MtkConnBootStartPowered(MTK_CONN_BOOT *Boot, unsigned long long EmiBase,
                                            unsigned EmiSize, unsigned Adie,
                                            const MTK_CONN_ROM_SET *Set, MTK_CONN_RAILS *Rails);
/*
 * Find the fitted MT6631 or MT6635 by RF SPI with MCU reset asserted.
 * The platform SetRail callback grants or restores one probe lease for the
 * specified stock regulator. It returns zero when that state is confirmed.
 * Release must be safe to retry, including after failed acquisition.
 * Supply the same reservation and serial ownership as for Start. Keep the
 * callback context and resources alive until Stop succeeds after any error.
 * Only publish Adie after a matching reply and a verified shutdown.
 */
MTK_CONN_POWER_STATUS MtkConnBootProbeAdie(MTK_CONN_BOOT *Boot, unsigned long long EmiBase,
                                         unsigned EmiSize, const MTK_CONN_PROBE_IO *Io,
                                         unsigned *Adie);
// Check live power, managed supplies, remap and MCU reset release after startup.
MTK_CONN_POWER_STATUS MtkConnBootCheck(const MTK_CONN_BOOT *Boot);
// Quiesce the clients first. Retry Stop on error with the same resources.
MTK_CONN_POWER_STATUS MtkConnBootStop(MTK_CONN_BOOT *Boot);

#endif
