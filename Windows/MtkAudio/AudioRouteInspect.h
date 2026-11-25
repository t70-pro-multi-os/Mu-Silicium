// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AUDIO_ROUTE_INSPECT_H
#define T70_AUDIO_ROUTE_INSPECT_H
#include "AudioInspect.h"
#include "AfeRouteObserve.h"
typedef struct {
 T70_AUDIO_INSPECTION Audio;
 T70_AFE_ROUTE_OBSERVATION Route;
} T70_AUDIO_ROUTE_INSPECTION;
typedef enum {T70AudioRouteInvalid,T70AudioRouteAudioFailed,
 T70AudioRouteReadFailed,T70AudioRouteCollected} T70_AUDIO_ROUTE_CODE;
typedef struct {
 T70_AUDIO_ROUTE_CODE Code;
 uint32_t AudioStarted,RouteStarted;
 T70_AUDIO_INSPECT_RESULT Audio; /* only applicable when AudioStarted */
 T70_AFE_ROUTE_RESULT Route; /* only applicable when RouteStarted */
} T70_AUDIO_ROUTE_RESULT;
/* Separate opt-in observer; original T70AudioInspect remains unchanged.
 * Validate distinct non-NULL route/amp contexts, collect AudioInspect, then
 * observe route. Publish entire result only after success (not an atomic CPU
 * store or hardware snapshot). Failure preserves Out, not prior I/O effects.
 * Dl1Values is a validated input COPY, never an AFE hardware observation.
 * Collected and set route bits imply neither compatibility nor playback ready.
 * Caller owns accessible read targets, serializes the whole call, keeps all
 * contexts alive and descriptors immutable. Inputs/contexts/output disjoint.
 * Context identity checks cannot prove physical target identity or lifetime.
 * ACF stays immutable/alive for borrowed views. No context pointers retained,
 * writes, retry, allocation, discovery, power actions or automatic setup. */
T70_AUDIO_ROUTE_RESULT T70AudioInspectWithRoute(const T70_AFE_PREPARED *Dl1,
 const void *Acf,size_t Bytes,const T70_AW_PROBE_ENDPOINT *First,
 const T70_AW_PROBE_ENDPOINT *Second,uint16_t Profile,
 const T70_AFE_ROUTE_READER *Route,T70_AUDIO_ROUTE_INSPECTION *Out);
#endif
