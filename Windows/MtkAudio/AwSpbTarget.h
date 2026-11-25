// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_AW_SPB_TARGET_H
#define T70_AW_SPB_TARGET_H
#include "AwSpb.h"
typedef struct {WDFIOTARGET Target;uint32_t ActiveUses;} T70_AW_SPB_TARGET_OWNER;
/* Zero-initialized stable owner, never copied/edited while owned. Explicit
 * caller-assigned WDFDEVICE and resource-hub connection ID only; no discovery.
 * The MtkI2c SPB controller validates the ACPI descriptor during target open.
 * Parent device must remain alive until Release finishes.
 * All methods PASSIVE_LEVEL, externally serialized with target use and parent
 * teardown. Begin/End bracket entire synchronous operation. No saved handle use
 * after End; no external close/delete or asynchronous outstanding requests.
 * Leases block only this owner's Release, NOT framework remote-target removal.
 * No query-remove/remove-complete/reopen callbacks are installed. Caller must
 * separately exclude remote removal for the entire use interval; Begin does not
 * prove the remote target is still open. Unsupported if that exclusion cannot
 * be provided. This is NOT a concurrent removal/drain service.
 * Parent cleanup must first exclude new calls, finish leases, then Release.
 * Open success establishes an OS connection only, never chip/power readiness. */
NTSTATUS T70AwSpbTargetAcquire(T70_AW_SPB_TARGET_OWNER *Owner,WDFDEVICE Device,
 uint64_t ConnectionId,uint64_t Base,uint16_t Address,uint16_t Channel);
NTSTATUS T70AwSpbTargetBegin(T70_AW_SPB_TARGET_OWNER *Owner);
NTSTATUS T70AwSpbTargetEnd(T70_AW_SPB_TARGET_OWNER *Owner);
NTSTATUS T70AwSpbTargetRelease(T70_AW_SPB_TARGET_OWNER *Owner);
#endif
