// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_SPB_PROFILE_SERIAL_H
#define T70_AW_SPB_PROFILE_SERIAL_H
#include "AwSpb.h"
#include "AwPairSerial.h"
typedef enum {T70AwSerialInvalid,T70AwSerialBadProfile,T70AwSerialIdFailed,
 T70AwSerialWrongId,T70AwSerialReadFailed,T70AwSerialWriteFailed,
 T70AwSerialFieldsWritten} T70_AW_SERIAL_APPLY_CODE;
typedef struct {
 T70_AW_SERIAL_APPLY_CODE Code;int32_t TransportStatus;
 uint16_t FailedRegister;uint16_t WritesAttempted,WritesCompleted;
} T70_AW_SERIAL_APPLY_RESULT;
/* Apply only verified profile 06[11:0] and07[14:0] serial fields in stock order,
 * preserving opaque bits. No SYSCTRL/PLL/TX enable/reset/power/calibration.
 * NOT full profile activation, AFE compatibility or playback-ready verdict.
 * PASSIVE, immutable resident ACF, caller-owned verified open target associated
 * with Base/Address/Channel, externally serialized and pinned against removal.
 * Before invoking, the real owner must have stopped serial traffic and excluded
 * playback/TX and other register writers. This function does not establish that
 * state or perform a power transition. The selected ACF is the intended board
 * wire configuration, NOT inferred from an AFE memory sample format.
 * Failure retains exact partial-write progress. Failed writes may have applied;
 * no automatic retry or rollback. No pointers or target refs retained. */
T70_AW_SERIAL_APPLY_RESULT T70AwSpbApplyProfileSerial(const void *,size_t,
 uint64_t Base,uint16_t Address,uint16_t Channel,uint16_t Profile,WDFIOTARGET);
/* Read-only complete selected profile/order preflight; no target or I/O. */
int T70AwSpbValidateProfileSerial(const void *,size_t,uint64_t,
 uint16_t Address,uint16_t Channel,uint16_t Profile);
#endif
