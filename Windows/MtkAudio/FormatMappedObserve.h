// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_FORMAT_MAPPED_OBSERVE_H
#define T70_FORMAT_MAPPED_OBSERVE_H
#include "AfeMapping.h"
#include "AfePrepare.h"
enum {T70FormatMatchMono=1,T70FormatMatchRate=2,T70FormatMatchHd=4,
      T70FormatMatchAlignment=8,T70FormatMatchAll=15};
typedef struct {
 uint32_t Raw,Mono,RateCode,HdMode,AlignmentCode,MatchMask;
} T70_AFE_FORMAT_OBSERVATION;
typedef enum {T70FormatObserveInvalid,T70FormatObserveBeginFailed,
 T70FormatObserveReadFailed,T70FormatObserveEndFailed,T70FormatObserved}
 T70_FORMAT_OBSERVE_CODE;
typedef struct {
 T70_FORMAT_OBSERVE_CODE Code;NTSTATUS BeginStatus,EndStatus;
 int32_t ReadStatus;
 uint32_t BeginAttempted,ReadAttempted,EndAttempted,LeaseEnded;
} T70_FORMAT_OBSERVE_RESULT;
/* Existing accessible AFE owner only. Validate the immutable numeric snapshot,
 * Begin, read configuration register 0x4c once, End on every normal read return.
 * Not a status/ready register: MatchMask reports independent field equality,
 * including partial matches; all four matching is NOT playback readiness.
 * Reserved HD/rate encodings are exposed as raw field values, not supported
 * formats. Out is published only when read and End succeed; otherwise unchanged.
 * Caller provides valid disjoint ordinary owner/snapshot/output storage outside
 * the mapping, and external serialization/lifetimes/accessibility throughout.
 * End failure leaves the lease unresolved; no retry or unmap. No writes, mapping,
 * power/start, hardware fault recovery or atomic concurrent publication. */
T70_FORMAT_OBSERVE_RESULT T70AfeObserveMappedFormat(T70_AFE_MAPPING_OWNER *Owner,
 const T70_AFE_PREPARED *Expected,T70_AFE_FORMAT_OBSERVATION *Out);
#endif
