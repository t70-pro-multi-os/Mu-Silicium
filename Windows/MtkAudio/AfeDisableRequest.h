// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AFE_DISABLE_REQUEST_H
#define T70_AFE_DISABLE_REQUEST_H
#include "AfeFormatUpdate.h"
typedef enum {T70DisableInvalid,T70DisableReadFailed,T70DisableAliasUnresolved,
 T70DisableAlreadySetObserved,T70DisableWriteFailed,T70DisableWriteReturned} T70_AFE_DISABLE_CODE;
typedef struct {
 T70_AFE_DISABLE_CODE Code;
 int32_t TransportStatus;
 uint32_t ReadAttempted,WriteAttempted,AliasValue;
} T70_AFE_DISABLE_RESULT;
/* DL1 disable REQUEST only. Verified T70 exclusive accessible AFE callbacks,
 * ordered accesses, immutable Io and caller-owned context required throughout.
 * Read 0x1b08, then write bit 4 only when readback is zero. A readback of
 * 0x10 returns AlreadySetObserved without writing (not stopped proof). Refuse
 * any other readback bit: never replay unknown clear-alias bits to hardware.
 * This restrictive policy may reject a legitimate readback; it is NOT a claim
 * about alias read semantics. No IRQ clear/disable, poll, address reset, power,
 * memory release or lease End. WriteReturned is NOT idle/drained/stopped proof.
 * Failed writes may apply. Caller retains allocations/leases on EVERY result;
 * no automatic retry and no authorization to free/restart. No I/O if invalid.
 */
T70_AFE_DISABLE_RESULT T70AfeDl1RequestDisable(const T70_AFE_FORMAT_IO *Io);
#endif
