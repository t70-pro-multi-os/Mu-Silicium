// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_ROUTE_OBSERVE_H
#define T70_AFE_ROUTE_OBSERVE_H
#include <stdint.h>
typedef struct {
  void *Context;
  int32_t (*Read32)(void *,uint32_t Offset,uint32_t *Value);
} T70_AFE_ROUTE_READER;
typedef struct {
  uint32_t Conn0,Conn1;
  uint32_t Dl1Channel1Selected,Dl1Channel2Selected;
} T70_AFE_ROUTE_OBSERVATION;
typedef enum {T70AfeRouteInvalid,T70AfeRouteReadFailed,
              T70AfeRouteObserved} T70_AFE_ROUTE_CODE;
typedef struct {
  T70_AFE_ROUTE_CODE Code;
  int32_t TransportStatus;
  uint32_t ReadsAttempted,FailedRead; /* failure index 0/1; otherwise 2 */
} T70_AFE_ROUTE_RESULT;
/* Read-only diagnostic: AFE-relative CONN0 then CONN1. Zero callback status is
 * success. Caller provides a valid, accessible mapping and owns its lifetime,
 * serialization and hardware accessibility for the entire call. Descriptor is
 * immutable; context may maintain provider state. Inputs/context/output must be
 * disjoint. No pointer retained. Any failure preserves the entire output.
 * Two sequential observations, not an atomic hardware snapshot. Selected bits
 * do NOT prove exclusive routing, serial format, clocks, amp readiness or sound.
 * No write callback, allocation, power/DMA action, retry or resource discovery. */
T70_AFE_ROUTE_RESULT T70AfeObserveDl1I2s3(const T70_AFE_ROUTE_READER *Io,
                                        T70_AFE_ROUTE_OBSERVATION *Observation);
#endif
