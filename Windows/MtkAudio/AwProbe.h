// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_PROBE_H
#define T70_AW_PROBE_H
#include "AwStatus.h"
typedef struct {uint16_t ChipId;T70_AW_CLOCK_SNAPSHOT Clock;} T70_AW_PROBE_SNAPSHOT;
typedef enum {T70AwProbeRead,T70AwProbeInvalid,T70AwProbeIdReadFailed,
              T70AwProbeWrongId,T70AwProbeStatusReadFailed} T70_AW_PROBE_CODE;
typedef struct {T70_AW_PROBE_CODE Code;int32_t TransportStatus;} T70_AW_PROBE_RESULT;
/* Explicit board tuple + caller-owned read target; tuple validation does NOT
 * prove that the callback actually targets that bus/address. Caller must ensure
 * this association and serialize the pair, keeping all pointers valid/disjoint.
 * Read callback returns zero only for complete success. Reads ID then SYSST;
 * stops on first error, no retry/write/reset/poll/allocation. Entire output is
 * unchanged on failure. Successful snapshot is not ready/fault-free/playable.
 * The two register reads are sequential, not an atomic hardware snapshot. */
T70_AW_PROBE_RESULT T70Aw88261Probe(uint64_t Base,uint16_t Address,uint16_t Channel,
                                   const T70_AW_STATUS_IO *Io,
                                   T70_AW_PROBE_SNAPSHOT *Snapshot);
#endif
