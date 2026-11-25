// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_ADDRESS_H
#define T70_AFE_ADDRESS_H
#include "AfePrepare.h"
typedef struct {
  void *Context;
  /* Write one 32-bit value at an AFE-relative offset, zero for complete success.
   * The provider owns the verified MMIO mapping, ordering/barriers and exclusive
   * access. This core has no MMIO implementation and does not verify ownership. */
  int32_t (*Write32)(void *Context,uint32_t Offset,uint32_t Value);
} T70_AFE_ADDRESS_IO;
typedef enum {T70AfeAddressConfigured,T70AfeAddressInvalid,T70AfeAddressWriteFailed} T70_AFE_ADDRESS_CODE;
typedef struct {
  T70_AFE_ADDRESS_CODE Code;
  int32_t TransportStatus;
  uint32_t WritesAttempted,WritesCompleted,FailedOffset;
} T70_AFE_ADDRESS_RESULT;
/* DL1 only. CALLER MUST establish DMA quiescence and exclusive resource ownership
 * before calling and retain it through all writes and failure recovery. This is
 * a precondition, not a boolean proof supplied to this function. No enable,
 * disable, power, IRQ or CUR write. Revalidates the snapshot before I/O.
 * Input/IO remain immutable, context alive; no internal retained pointers.
 * Stops on first write error. A failed callback may have changed hardware;
 * no rollback or retry is attempted. Completed writes are not undone. Failure
 * must not be interpreted as permitting DMA/memory release or stream restart.
 * Result is returned by value; Configured means only four callbacks succeeded,
 * not playback readiness. See AFE-ADDRESS-EVIDENCE.md. */
T70_AFE_ADDRESS_RESULT T70AfeDl1SetAddress(const T70_AFE_PREPARED *Prepared,
                                         const T70_AFE_ADDRESS_IO *Io);
#endif
