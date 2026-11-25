// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_CONN_POWER_H
#define MTK_CONN_POWER_H

typedef enum {
  MtkConnSpm,   // MT6789 0x10006000
  MtkConnInfra  // MT6789 0x10001000
} MTK_CONN_REGION;

typedef struct {
  void *Context;
  int (*Read)(void *Context, MTK_CONN_REGION Region, unsigned Offset, unsigned *Value);
  int (*Write)(void *Context, MTK_CONN_REGION Region, unsigned Offset, unsigned Value);
  int (*WaitUs)(void *Context, unsigned Microseconds);
  void (*Barrier)(void *Context);
} MTK_CONN_POWER_IO;

typedef enum {
  MtkConnPowerOk, MtkConnPowerInvalid, MtkConnPowerBusy, MtkConnPowerNotReady,
  MtkConnPowerIoError, MtkConnPowerTimeout
} MTK_CONN_POWER_STATUS;

typedef enum {
  MtkConnOff, MtkConnOn, MtkConnFault
} MTK_CONN_POWER_STATE;

#define MTK_CONN_EMI_SIZE  0x00500000U
#define MTK_CONN_EMI_ALIGN 0x01000000U
#define MTK_CONN_EMI_LIMIT 0x80000000ULL

typedef struct {
  MTK_CONN_POWER_IO Io;
  MTK_CONN_POWER_STATE State;
  unsigned IslandOwned;
  unsigned RestoreCcifGate;
  unsigned RemapOwned;
  unsigned SavedRemap[2];
  unsigned TargetRemap[2];
} MTK_CONN_POWER;

/*
 * Use one serialized shared owner for Wi-Fi, Bluetooth and GNSS. It owns
 * the CONN power and CCIF4_AP bits, keeps the AXI and SPM parents on, and
 * prepares the supply and firmware before bringing the domain out of reset.
 * The caller owns the contiguous EMI reservation (and its access policy)
 * until Stop succeeds, including after a partially successful Start.
 * Callbacks return zero on success. WaitUs must wait at least the requested
 * time and return boundedly. Barrier orders the write before any later IO.
 * Initialize fresh storage only, with no outstanding hardware ownership.
 */
MTK_CONN_POWER_STATUS MtkConnPowerInit(MTK_CONN_POWER *Power, const MTK_CONN_POWER_IO *Io);

// Validate the exact EMI reservation and confirm the hardware domain is isolated.
MTK_CONN_POWER_STATUS MtkConnPowerCanStart(const MTK_CONN_POWER *Power,
                                         unsigned long long EmiBase, unsigned EmiSize);

// Start from an isolated domain and an exact board EMI reservation.
MTK_CONN_POWER_STATUS MtkConnPowerStart(MTK_CONN_POWER *Power,
                                      unsigned long long EmiBase, unsigned EmiSize);
// Check live power, CCIF, protection and EMI before accessing the domain.
MTK_CONN_POWER_STATUS MtkConnPowerCheck(const MTK_CONN_POWER *Power);
// Call only after every CONN client and its DMA/firmware are quiescent.
// A failed transition keeps ownership. Stop can retry an incomplete shutdown
// or unwind a failed Start while the same owner still holds the resources.
MTK_CONN_POWER_STATUS MtkConnPowerStop(MTK_CONN_POWER *Power);

#endif
