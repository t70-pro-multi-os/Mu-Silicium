// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_ROUTE_MAPPED_H
#define T70_ROUTE_MAPPED_H
#include "AfeMapping.h"
#include "AfeRouteObserve.h"
typedef enum {T70MappedRouteInvalid,T70MappedRouteBeginFailed,
 T70MappedRouteReadFailed,T70MappedRouteEndFailed,T70MappedRouteObserved}
 T70_MAPPED_ROUTE_CODE;
typedef struct {
 T70_MAPPED_ROUTE_CODE Code;
 NTSTATUS BeginStatus,EndStatus;
 uint32_t BeginAttempted,ReadStarted,EndAttempted,LeaseEnded;
 T70_AFE_ROUTE_RESULT Route; /* applicable only when ReadStarted */
} T70_MAPPED_ROUTE_RESULT;
/* Existing owner only: Begin, two sequential reads, End once on every normal
 * read return. Publish Out only if BOTH reads and End succeed; otherwise Out is
 * unchanged, while result retains read/error/lease details. End failure leaves
 * an unresolved lease for caller recovery, never auto retry/unmap. No writes to
 * registers. Exceptions/bus faults are not caught. Out must be valid ordinary
 * writable memory disjoint from owner/mapping; caller owns lifetimes, device
 * accessibility and serialization of the WHOLE call, including publication.
 * Owner must not mutate, move or be unmapped during use. Snapshot is not atomic
 * hardware sampling or an atomic memory store; bits do not imply readiness,
 * exclusive routing, compatibility or playback. No map/start/power action. */
T70_MAPPED_ROUTE_RESULT T70AfeObserveMappedRoute(T70_AFE_MAPPING_OWNER *Owner,
                                               T70_AFE_ROUTE_OBSERVATION *Out);
#endif
