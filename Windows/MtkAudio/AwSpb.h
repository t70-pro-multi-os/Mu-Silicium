// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_SPB_H
#define T70_AW_SPB_H
#include <ntddk.h>
#include <wdf.h>
#include <stdint.h>
#include "AwStatus.h"
/* Transport only. Caller owns an already opened, verified AW88261 SPB target,
 * serializes accesses, pins it until completion and calls at PASSIVE_LEVEL.
 * No PnP discovery, power/reset, register allowlist or profile sequencing here.
 * In particular, do not stream an ACF profile through WriteRegister without
 * separately implementing the stock register programming/calibration policy.
 * Failed writes may have reached hardware; there is deliberately no retry.
 * Read output is unchanged unless the complete transfer succeeds. */
NTSTATUS T70AwSpbReadRegister(WDFIOTARGET Target,USHORT Register,USHORT *Value);
NTSTATUS T70AwSpbWriteRegister(WDFIOTARGET Target,USHORT Register,USHORT Value);
/* Caller must hold its serialization lock across the whole read-modify-write.
 * PreserveMask uses stock semantics: set bits retain their old value. */
NTSTATUS T70AwSpbUpdatePreservedBits(WDFIOTARGET Target,USHORT Register,
                                    USHORT PreserveMask,USHORT Value);
/* Read-only identification; output changes only for a verified AW88261 ID.
 * This does not authorize register writes or establish board/power readiness. */
NTSTATUS T70AwSpbIdentify88261(WDFIOTARGET Target,USHORT *ChipId);
NTSTATUS T70AwSpbUpdateVolume(WDFIOTARGET Target,USHORT Initial,
                             uint32_t Requested,USHORT *AppliedRegister);
NTSTATUS T70AwSpbUpdateTx(WDFIOTARGET Target,unsigned Enabled,USHORT *AppliedRegister);
NTSTATUS T70AwSpbReadClockStatus(WDFIOTARGET Target,T70_AW_CLOCK_SNAPSHOT *Snapshot);
#endif
