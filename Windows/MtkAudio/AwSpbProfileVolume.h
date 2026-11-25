// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_SPB_PROFILE_VOLUME_H
#define T70_AW_SPB_PROFILE_VOLUME_H
#include "AwSpb.h"
#include "AwProfile.h"
#include "AwVolumeUpdate.h"
typedef enum {T70AwProfileVolumeInvalid,T70AwProfileVolumeBadProfile,
 T70AwProfileVolumeIdReadFailed,T70AwProfileVolumeWrongId,
 T70AwProfileVolumeReadFailed,T70AwProfileVolumeWriteFailed,
 T70AwProfileVolumeApplied} T70_AW_PROFILE_VOLUME_CODE;
typedef struct {
 T70_AW_PROFILE_VOLUME_CODE Code;
 int32_t TransportStatus;
 uint32_t WriteAttempted;
} T70_AW_PROFILE_VOLUME_RESULT;
/* Explicit existing target, PASSIVE, caller pins against removal and serializes
 * the ENTIRE call. Caller verifies target->board tuple association and that this
 * selected profile's volume baseline is appropriate for its existing amp state.
 * This is only profile-relative attenuation, NOT profile activation or amp
 * initialization. No SYSCTRL, reset, TX, calibration, power or retry operation.
 * Validate immutable ACF/tuple and a UNIQUE 0x05 record before I/O, identify
 * 0x2113, then preserve current upper6 bits and apply stock volume arithmetic.
 * Output unchanged on failure, including possibly partially applied writes.
 * All buffers are resident and disjoint; input lifetime lasts through return.
 * No target/ref/resource is retained, acquired, or invented by this function. */
T70_AW_PROFILE_VOLUME_RESULT T70AwSpbApplyProfileVolume(const void *Acf,size_t Bytes,
 uint64_t Base,uint16_t Address,uint16_t Channel,uint16_t Profile,
 WDFIOTARGET Target,uint32_t Attenuation,uint16_t *AppliedRegister);
#endif
