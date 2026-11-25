// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_PAIR_PROBE_H
#define T70_AW_PAIR_PROBE_H
#include "AwProbe.h"
typedef struct {
  uint64_t Base;
  uint16_t Address,Channel;
  T70_AW_STATUS_IO Io;
} T70_AW_PROBE_ENDPOINT;
typedef struct {T70_AW_PROBE_SNAPSHOT Channels[2];} T70_AW_PAIR_SNAPSHOT;
typedef enum {T70AwPairRead,T70AwPairInvalid,T70AwPairEndpointFailed} T70_AW_PAIR_CODE;
typedef struct {
  T70_AW_PAIR_CODE Code;
  unsigned FailedChannel; /* 0/1 only for EndpointFailed, otherwise 2. */
  T70_AW_PROBE_RESULT Endpoint;
} T70_AW_PAIR_RESULT;
/* Endpoints are ordered channel 0 then 1. Both tuples/read callbacks are checked
 * before I/O. Distinct non-NULL contexts are required to catch accidental target
 * reuse; this cannot prove the contexts route to distinct physical devices.
 * Caller verifies routing and serializes both targets for the entire call.
 * Endpoint descriptors must remain immutable throughout the call.
 * All pointers must remain valid; output must not overlap endpoints/contexts.
 * Reads channel 0 then 1, stopping at first failure. Entire output is unchanged
 * unless both succeed. No writes/reset/retry/allocation; no retained pointers.
 * Sequential observations, NOT an atomic stereo snapshot or readiness verdict. */
T70_AW_PAIR_RESULT T70Aw88261PairProbe(const T70_AW_PROBE_ENDPOINT *First,
                                     const T70_AW_PROBE_ENDPOINT *Second,
                                     T70_AW_PAIR_SNAPSHOT *Snapshot);
#endif
