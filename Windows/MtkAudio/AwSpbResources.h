// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_SPB_RESOURCES_H
#define T70_AW_SPB_RESOURCES_H
#include "AwSpbTarget.h"
#include "AwSpbProfileSerial.h"
#include "AwSpbProfileVolume.h"
typedef struct {
 WDFDEVICE Device;
 const CM_PARTIAL_RESOURCE_DESCRIPTOR *Connection;
 uint64_t ControllerBase;
 uint16_t Address,Channel;
} T70_AW_SPB_RESOURCE;
typedef struct {
 T70_AW_SPB_TARGET_OWNER Target[2];
 unsigned Open,Attempted;
} T70_AW_SPB_RESOURCES;
typedef struct {
 NTSTATUS Status;
 unsigned ChannelsCompleted,FailedChannel;
 T70_AW_SERIAL_APPLY_RESULT Channel[2];
} T70_AW_SPB_RESOURCES_RESULT;
/* Explicit translated resources from their actual owning devices, ordered by
 * DT sound-channel 0/1, not resource enumeration order or physical left/right.
 * The Resource Hub descriptor must confirm I2C1, channel0=0x34 and
 * channel1=0x35. CM descriptors are borrowed only during Acquire.
 * Stable zeroed nonpaged owner, never copied. PASSIVE_LEVEL, all calls and
 * parent teardown externally serialized. Parents remain alive through Release.
 * Remote-target removal must be excluded by the real owner throughout use;
 * these leases do not implement WDF remote removal notification/drain.
 * Apply requires traffic/TX stopped and exclusive register ownership exactly
 * as AwSpbProfileSerial. Applies only serial fields, not power or playback.
 * A write failure can have taken effect; pair application is NOT atomic.
 * Once an application is attempted, automatic reapplication is rejected.
 * Release only closes OS targets: it does not undo writes or establish a safe
 * hardware retry. Reacquire/retry requires a new caller recovery decision. */
NTSTATUS T70AwSpbResourcesAcquire(T70_AW_SPB_RESOURCES *,const T70_AW_SPB_RESOURCE [2]);
T70_AW_SPB_RESOURCES_RESULT T70AwSpbResourcesApplySerial(T70_AW_SPB_RESOURCES *,const void *,size_t,uint16_t);
NTSTATUS T70AwSpbResourcesRelease(T70_AW_SPB_RESOURCES *);
typedef struct {
 NTSTATUS Status,EndStatus;
 T70_AW_PROFILE_VOLUME_RESULT Apply;
} T70_AW_SPB_VOLUME_RESULT;
/* Existing acquired pair only. Channel is DT sound-channel 0/1, never physical
 * speaker direction. Same external serialization/remote-removal exclusion as
 * above applies through End. Caller establishes profile baseline applicability.
 * These operations do not activate a profile or make hardware ready. Volume
 * output is published only after the operation AND End succeed. Apply retains
 * the exact lower-layer outcome, including a possibly effective failed write;
 * EndStatus is independent. No automatic retry or rollback. Read returns its
 * primary transport error ahead of an End error, and never publishes partial
 * output. Read assumes the caller has already verified the chip/target identity;
 * it does not read chip ID or establish readiness. Buffers are resident/disjoint from owner and immutable ACF input. */
T70_AW_SPB_VOLUME_RESULT T70AwSpbResourcesApplyVolume(T70_AW_SPB_RESOURCES *,
 const void *,size_t,uint16_t Profile,uint16_t Channel,uint32_t Attenuation,uint16_t *Applied);
NTSTATUS T70AwSpbResourcesReadVolume(T70_AW_SPB_RESOURCES *,uint16_t Channel,uint16_t *Raw);
#endif
