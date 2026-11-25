// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_KS_VOLUME_H
#define T70_AW_KS_VOLUME_H
#include "AwSpbResources.h"
/* Project adapter, NOT a registered topology property handler. Explicit owner
 * must already bind channel0/1 to verified board targets. PASSIVE, entire calls
 * externally serialized against profile/register changes, removal and teardown.
 * Immutable resident ACF and disjoint owner/input/output storage required.
 * KS level is signed 16.16 dB relative to the selected profile's effective
 * baseline min(decoded initial,720). Baseline above720 has no remaining range.
 * Requests clamp to [-((720-baseline)/8),0] dB, rounding attenuation upward to
 * 1/8dB steps. INT32_MIN clamps to finite attenuation, NOT an actual mute.
 * No profile activation, power, automatic retry, or hardware-ready claim.
 * SET channel -1 selects both; 0/1 selects one. Pair writes are sequential,
 * not atomic: CompletedMask reports completed channels on subsequent failure,
 * including an EndStatus failure after a completed write. Failed writes may
 * themselves have taken effect. Output publishes only on total success.
 * GET supports individual channels only: no invented aggregate for unequal
 * values. Noncanonical/out-of-profile register values fail without output.
 * No state is cached or retained. BasicSupport/property registration is absent. */
typedef struct { uint32_t ValidMask; int32_t Level[2]; } T70_AW_KS_LEVELS;
typedef struct {
 NTSTATUS Status;
 uint32_t AttemptedMask,CompletedMask,FailedChannel;
 T70_AW_SPB_VOLUME_RESULT Channel[2];
} T70_AW_KS_VOLUME_RESULT;
T70_AW_KS_VOLUME_RESULT T70AwKsVolumeSet(T70_AW_SPB_RESOURCES *,const void *,size_t,
 uint16_t Profile,int32_t Channel,int32_t Level,T70_AW_KS_LEVELS *Applied);
NTSTATUS T70AwKsVolumeGet(T70_AW_SPB_RESOURCES *,const void *,size_t,
 uint16_t Profile,uint32_t Channel,int32_t *Level);
#endif
