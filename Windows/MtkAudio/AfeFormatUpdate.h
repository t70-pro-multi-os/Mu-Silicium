// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_FORMAT_UPDATE_H
#define T70_AFE_FORMAT_UPDATE_H
#include "AfeFormat.h"
typedef struct {
  void *Context;
  int32_t (*Read32)(void *,uint32_t Offset,uint32_t *Value);
  int32_t (*Write32)(void *,uint32_t Offset,uint32_t Value);
} T70_AFE_FORMAT_IO;
typedef enum {T70AfeFieldsUpdated,T70AfeFieldsInvalid,T70AfeFieldsReadFailed,
              T70AfeFieldsWriteFailed} T70_AFE_FIELDS_CODE;
typedef struct {
  T70_AFE_FIELDS_CODE Code;int32_t TransportStatus;
  uint32_t FailedField,ReadsAttempted,WritesAttempted,WritesCompleted;
} T70_AFE_FIELDS_RESULT;
/* A callback return value of zero means complete success. DL1 only: mono, rate,
 * HD, alignment in
 * that order, each a fresh read/masked write at AFE-relative offset 0x4c.
 * FailedField is 0..3 on I/O failure, 4 otherwise. Invalid requests cause no I/O.
 * Caller establishes quiescent/exclusive ownership, valid ordered mapping and
 * serialization over the whole operation. This core does not verify those facts.
 * IO descriptor remains immutable; context stays alive and serialized for the
 * call but may maintain provider state. No retained pointers or allocation.
 * Fail immediately; no rollback/retry. A failing write may have taken effect;
 * completed writes remain in place. No DMA enable/disable, IRQ or power action.
 * Four successful writes do not establish playback readiness. */
T70_AFE_FIELDS_RESULT T70AfeDl1UpdateFormat(uint32_t Rate,uint32_t SampleFormat,
                                          uint32_t Channels,const T70_AFE_FORMAT_IO *Io);
#endif
