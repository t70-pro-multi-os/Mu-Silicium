// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_SPB_PAIR_H
#define T70_AW_SPB_PAIR_H
#include "AwSpb.h"
#include "AwPairProbe.h"
/* Explicit borrowed targets, already opened to the caller-verified connection
 * for each board tuple. Distinct handles do NOT prove distinct physical devices.
 * Caller pins both targets against close/removal, holds serialization across the
 * ENTIRE call, and provides accessible resident output disjoint from all inputs.
 * No automatic enumeration, ownership acquisition, writes or readiness verdict.
 * PASSIVE_LEVEL only. No callbacks/descriptors are retained after return.
 * Pair.Code Invalid denotes preflight rejection (including IRQL); otherwise
 * exact stage/transport failure is retained. Snapshot unchanged on any failure. */
T70_AW_PAIR_RESULT T70AwSpbProbePair(uint64_t Base,uint16_t Address0,
 uint16_t Address1,WDFIOTARGET Target0,WDFIOTARGET Target1,
 T70_AW_PAIR_SNAPSHOT *Snapshot);
#endif
