// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_TX_UPDATE_H
#define T70_AW_TX_UPDATE_H
#include "AwTxField.h"
typedef struct {
  void *Context;
  /* Synchronous completion: zero success, nonzero exact transport error. */
  int32_t (*Read)(void *Context,uint16_t Register,uint16_t *Value);
  int32_t (*Write)(void *Context,uint16_t Register,uint16_t Value);
} T70_AW_TX_IO;
typedef enum { T70AwTxUpdated,T70AwTxInvalid,T70AwTxReadFailed,T70AwTxWriteFailed } T70_AW_TX_UPDATE_CODE;
typedef struct { T70_AW_TX_UPDATE_CODE Code; int32_t TransportStatus; } T70_AW_TX_UPDATE_RESULT;
/* Caller provides a verified target and serializes the entire read/write pair.
 * Enabled must be 0 or 1. AppliedRegister changes only after write success.
 * No retry or rollback; failed writes may already affect hardware. Pointers
 * remain caller-owned and must be valid throughout callbacks and this call.
 * This operation does NOT establish when changing TX is safe or authorized.
 * It performs no power/reset sequencing or automatic target discovery. */
T70_AW_TX_UPDATE_RESULT T70Aw88261UpdateTx(const T70_AW_TX_IO *Io,unsigned Enabled,
                                         uint16_t *AppliedRegister);
#endif
