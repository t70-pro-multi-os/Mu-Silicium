// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_PAIR_INSPECT_H
#define T70_AW_PAIR_INSPECT_H
#include "AwPairProfile.h"
#include "AwPairProbe.h"
typedef struct {
  T70_AW_PAIR_PROFILE_VIEW Profiles;
  T70_AW_PAIR_SNAPSHOT Observations;
} T70_AW_PAIR_INSPECTION;
typedef enum {T70AwInspectInvalid,T70AwInspectProfileFailed,
              T70AwInspectProbeFailed,T70AwInspectObserved} T70_AW_INSPECT_CODE;
typedef struct {
  T70_AW_INSPECT_CODE Code;
  uint32_t ProbeStarted;
  T70_AW_PAIR_RESULT Probe; /* applicable only when ProbeStarted is nonzero */
} T70_AW_INSPECT_RESULT;
/* Consumer of explicit caller-owned endpoints: validate profile catalog first,
 * then perform the existing sequential read-only pair probe. Publish profile
 * views AND observations together only on success. No resource creation, writes,
 * profile application or readiness claim. Observed means read callbacks returned
 * observations, not that the amplifiers are configured or usable for playback.
 * Caller provides verified routing, target lifetime and serialization. ACF must
 * stay alive/immutable for the borrowed views; endpoint descriptors immutable
 * during call, all inputs/output/contexts disjoint. No pointers retained by core.
 * Failure leaves output entirely unchanged, but prior reads may have occurred.
 * No retry, power/reset or allocation. Missing providers are not fabricated. */
T70_AW_INSPECT_RESULT T70Aw88261InspectPair(const void *Acf,size_t Bytes,
 const T70_AW_PROBE_ENDPOINT *First,const T70_AW_PROBE_ENDPOINT *Second,
 uint16_t Profile,T70_AW_PAIR_INSPECTION *Inspection);
#endif
