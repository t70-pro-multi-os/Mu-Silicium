// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_CONFIGURE_H
#define T70_AFE_CONFIGURE_H
#include "AfeAddress.h"
#include "AfeFormatUpdate.h"
typedef enum {T70AfeConfigInvalid,T70AfeConfigAddressFailed,
              T70AfeConfigFormatFailed,T70AfeConfigCallbacksCompleted} T70_AFE_CONFIG_CODE;
typedef struct {
  T70_AFE_CONFIG_CODE Code;
  /* A false Started flag means the corresponding result is not applicable. */
  uint32_t AddressStarted,FormatStarted;
  T70_AFE_ADDRESS_RESULT Address;
  T70_AFE_FIELDS_RESULT Format;
} T70_AFE_CONFIG_RESULT;
/* Validate the complete snapshot and both callbacks before any I/O. Same target
 * and context throughout: address writes, then mono/rate/HD/alignment RMWs.
 * Caller must establish quiescence, exclusive ownership and provider ordering
 * for the entire call, and retain the DMA allocation through failure recovery.
 * Prepared/IO immutable; context alive and serialized. No pointers retained.
 * First error stops later operations. Sub-results retain exact error and counts;
 * a failing write may have applied and previous writes remain. No rollback or
 * retry, no enable, power, IRQ or buffer clearing. CallbacksCompleted means only
 * callbacks succeeded, NOT hardware readiness, verification or playback.
 * This is the register subset of stock hw_params, not the whole operation.
 */
T70_AFE_CONFIG_RESULT T70AfeDl1Configure(const T70_AFE_PREPARED *Prepared,
                                        const T70_AFE_FORMAT_IO *Io);
#endif
