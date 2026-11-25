// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_CONN_RAILS_H
#define MTK_CONN_RAILS_H

#include <Library/MtkConnPower.h>

typedef enum {
  MtkConnProbeVcn18,
  MtkConnProbeVcn13,
  MtkConnRuntimeVcn18,
  MtkConnRuntimeVcn33
} MTK_CONN_RAIL;

typedef MTK_CONN_RAIL MTK_CONN_PROBE_RAIL;

typedef struct {
  void *Context;
  int (*SetRail)(void *Context, MTK_CONN_PROBE_RAIL Rail, int Enable);
} MTK_CONN_PROBE_IO;

typedef struct {
  void *Context;
  int (*ReadPmic)(void *Context, unsigned Address, unsigned *Value);
  int (*WritePmic)(void *Context, unsigned Address, unsigned Value);
  int (*ReadGpio)(void *Context, unsigned Offset, unsigned *Value);
  int (*WriteGpio)(void *Context, unsigned Offset, unsigned Value);
  int (*WaitUs)(void *Context, unsigned Microseconds);
  int (*Authorized)(void *Context, MTK_CONN_RAIL Rail);
} MTK_CONN_RAIL_IO;

typedef struct {
  unsigned Captured, Enable, OpEnable, OpConfig, Voltage;
} MTK_CONN_LDO_STATE;

typedef struct {
  MTK_CONN_RAIL_IO Io;
  unsigned Vcn18Ready;
  unsigned Vcn13Captured, SavedPin[3], SavedVs2[3];
  MTK_CONN_LDO_STATE Vcn18, Vcn33;
  const void *RuntimeOwner;
  unsigned RuntimeAdie, RuntimeReady;
} MTK_CONN_RAILS;

/*
 * Initialize zeroed storage. Use the existing serial PMIC and GPIO owners.
 * Authorized returns nonzero only with exclusive ownership of the affected
 * fields. VCN13 needs GPIO68 and the shared VS2 selectors. LDOs need their
 * enable and voltage fields, plus operation mode for runtime. VCN33 also
 * needs serialization of the shared voltage selector and Wi-Fi vote.
 * Other callbacks return zero on success. Writes are ordered and can fail
 * after reaching hardware. WaitUs must wait at least the requested interval.
 */
MTK_CONN_POWER_STATUS MtkConnRailsInit(MTK_CONN_RAILS *Rails, const MTK_CONN_RAIL_IO *Io);

/*
 * MTK_CONN_PROBE_IO.SetRail callback for the T70 Pro non-RC ADIE probe.
 * Hold the authorized resources until release succeeds. Only release
 * VCN13 after stopping the CONN island. Release VCN18 last. Releasing
 * an untouched rail is a no-op. Repeat release after failed acquisition
 * or shutdown with the same context and lease.
 */
int MtkConnRailsSetProbeRail(void *Context, MTK_CONN_PROBE_RAIL Rail, int Enable);

/*
 * Hold the platform's PMIC/GPIO reservation and serialize calls until
 * Release succeeds, including after a partial Acquire. Use the same owner
 * identity for all calls. Release only after the island is isolated.
 */
MTK_CONN_POWER_STATUS MtkConnRailsAcquireRuntime(MTK_CONN_RAILS *Rails, unsigned Adie, const void *Owner);
MTK_CONN_POWER_STATUS MtkConnRailsCheckRuntime(const MTK_CONN_RAILS *Rails, const void *Owner);
MTK_CONN_POWER_STATUS MtkConnRailsReleaseRuntime(MTK_CONN_RAILS *Rails, const void *Owner);

#endif
