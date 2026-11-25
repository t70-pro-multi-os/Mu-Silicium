// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_ADDRESS_MAPPED_OBSERVE_H
#define T70_ADDRESS_MAPPED_OBSERVE_H
#include "AfeMapping.h"
#include "AfePrepare.h"
enum {T70AddressMatchBaseLow=1,T70AddressMatchEndLow=2,
 T70AddressMatchBaseHigh=4,T70AddressMatchEndHigh=8,T70AddressMatchAll=15};
typedef struct {
 uint32_t BaseLow,EndLow,BaseHigh,EndHigh,MatchMask;
} T70_AFE_ADDRESS_OBSERVATION;
typedef enum {T70AddressObserveInvalid,T70AddressObserveBeginFailed,
 T70AddressObserveReadFailed,T70AddressObserveEndFailed,T70AddressObserved}
 T70_ADDRESS_OBSERVE_CODE;
typedef struct {
 T70_ADDRESS_OBSERVE_CODE Code;NTSTATUS BeginStatus,EndStatus;
 int32_t ReadStatus;
 uint32_t BeginAttempted,ReadsAttempted,ReadsCompleted,FailedOffset,
          EndAttempted,LeaseEnded;
} T70_ADDRESS_OBSERVE_RESULT;
/* Existing mapping only. Validate Expected, Begin, sequentially read 0x54,
 * 0x64,0x50,0x60, then End once on every normal return. This diagnostic order
 * follows the known configuration fields; it is NOT a coherent/latching read
 * protocol. Raw halves are not combined into an asserted live address.
 * Compare each whole raw word independently; unexpected high bits are retained,
 * not masked into a false match. MatchAll does not establish DMA activity, quiescence or readiness.
 * Publish Out only after all reads AND End succeed. On errors preserve Out,
 * report attempted/completed/failed-offset (0 absent) and unresolved lease.
 * Valid resident disjoint owner/Expected/Out outside mapping required; caller
 * owns accessibility, lifetime and external whole-call serialization.
 * No writes, retry, mapping, unmap, hardware exception handling or start/power. */
T70_ADDRESS_OBSERVE_RESULT T70AfeObserveMappedAddress(T70_AFE_MAPPING_OWNER *Owner,
 const T70_AFE_PREPARED *Expected,T70_AFE_ADDRESS_OBSERVATION *Out);
#endif
