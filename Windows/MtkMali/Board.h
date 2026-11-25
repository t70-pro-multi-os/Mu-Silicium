/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_BOARD_H
#define T70_MALI_BOARD_H
#include "Miniport.h"
#include "Mfg.h"
/* The PnP host supplies real resources associated with this same GPU stack.
   Acquire/Release retain a resource cookie on failure exactly as the miniport
   interface does. Validate proves the three translated IRQ associations,
   exclusive GPU ownership, direct address model and nonfailing PoC cache ops;
   it must work in D3 without GPU MMIO and must not depend on OPP approval.
   Enter/Leave bracket each hardware call with the global PWRAP/regulator/clock
   software locks. Enter failure owns no access lock and changes no hardware.
   Leave cannot fail; Quarantine must block other clients of poisoned shared
   hardware before dropping CPU locks. The long-lived resource cookie and child
   ownership journals survive failures. These are mandatory OS provider inputs,
   not inferred mappings, coherency, firmware handoff or permissive defaults. */
typedef struct {
    void *Context;
    NTSTATUS (*Acquire)(void *,MALI_BOARD_LEASE *);
    NTSTATUS (*Validate)(void *,const MALI_BOARD_LEASE *);
    NTSTATUS (*Enter)(void *,void *ResourceCookie);
    void (*Leave)(void *,void *ResourceCookie,BOOLEAN Quarantine);
    NTSTATUS (*Release)(void *,void *ResourceCookie);
} MALI_BOARD_RESOURCES;
/* Optional nonfailing OS object pins. Both callbacks or neither. They run at
   PASSIVE, may not reenter this provider, and outlive its last reference. */
typedef struct {void *Context;void (*Reference)(void *);void (*Dereference)(void *);} MALI_BOARD_LIFETIME;
typedef struct {MALI_MFG_CONTROLLER *Controller;MALI_BOARD_RESOURCES Resources;MALI_BOARD_LIFETIME Lifetime;} MALI_BOARD_CONFIG;
struct MALI_BOARD_PROVIDER;
typedef struct {struct MALI_BOARD_PROVIDER *Owner;uint32_t Power;} MALI_BOARD_REFERENCE;
typedef struct MALI_BOARD_PROVIDER {
    uint32_t Signature,AccessHeld,Stopping;
    volatile LONG Closing,Removed,PnpActive;
    volatile LONG Gate,References[2],ReferenceFault;
    MALI_BOARD_CONFIG Config;
    MALI_BOARD_REFERENCE Reference[2];
    MALI_BOARD_LEASE ResourceLease;
    uintptr_t Generation;
    void *ClientCookie;
    DEVICE_POWER_STATE PowerState;
    PDEVICE_OBJECT Pdo;
    CM_PARTIAL_RESOURCE_DESCRIPTOR Interrupts[3]; /* successful PnP binding */
} MALI_BOARD_PROVIDER;
/* Fresh disjoint zeroed nonpaged provider/config and initialized, unacquired
   controller. Immutable callbacks and all objects outlive both interfaces and
   every retained resource cookie. All methods, including ref/deref, are PASSIVE.
   Hardware methods are internally serialized; refcounts are independently atomic.
   The PnP host dispatches each interface GUID to its matching Get function.
   Successful Get owns one initial reference; requested version >=1 returns v1.
   PnP dispatch is provided below; attachment and the ACPI/shared-resource
   backend remain the owning driver's responsibility. */
NTSTATUS MtkMaliBoardProviderInitialize(MALI_BOARD_PROVIDER *,const MALI_BOARD_CONFIG *);
NTSTATUS MtkMaliBoardGetInterface(MALI_BOARD_PROVIDER *,USHORT Version,USHORT Bytes,MALI_BOARD_INTERFACE *);
NTSTATUS MtkMaliBoardGetPowerInterface(MALI_BOARD_PROVIDER *,USHORT Version,USHORT Bytes,MALI_BOARD_POWER_INTERFACE *);
/* PnP host calls Bind only after lower START_DEVICE completed successfully,
   with that PDO's OS-owned, complete raw/translated lists (never caller data).
   Copies the exact GPU/MMU/JOB association; no MMIO, mapping or power claim.
   Both lists have one full descriptor and exactly one fixed GPU range plus
   three exclusive level IRQs. Raw vectors must be stock GSIV 428/429/430;
   translated vectors/IRQL/affinity come exclusively from the paired OS list.
   Pdo is borrowed from the host, which retains it until unbind and rundown. */
NTSTATUS MtkMaliBoardBindResources(MALI_BOARD_PROVIDER *,PDEVICE_OBJECT,
    const CM_RESOURCE_LIST *Raw,const CM_RESOURCE_LIST *Translated);
NTSTATUS MtkMaliBoardUnbindResources(MALI_BOARD_PROVIDER *,PDEVICE_OBJECT);
/* IRP_MN_QUERY_INTERFACE routing for a hosting PnP driver. TRUE owns the query
   and sets IoStatus; the host must complete it. FALSE leaves the IRP untouched
   and the host forwards it. A known GUID requires MALI_BOARD_QUERY matching
   the same bound PDO and every translated IRQ field. No completion/forwarding,
   filter attachment or remove rundown is installed by this helper. */
BOOLEAN MtkMaliBoardQueryInterfaceIrp(MALI_BOARD_PROVIDER *,PDEVICE_OBJECT,PIRP);
/* Withdraws new queries/acquisitions. Existing references may still power down
   and release. Success requires no references, resource/access cookie, hardware
   journal, PnP binding or reference fault. The host must also drain PnP entries before freeing
   storage. A live/poisoned provider must never be zeroed to bypass this check. */
NTSTATUS MtkMaliBoardProviderClose(MALI_BOARD_PROVIDER *);
/* Actual synchronous WDM PnP dispatch for an owning lower-filter driver.
   The host calls at PASSIVE, with its real same-stack PDO/lower device and an
   OS-owned IRP with a next stack location. It must pin provider/controller/
   callback storage (including driver code) across this call and every returned
   interface/resource lease, serialize host detach, and hold its IO_REMOVE_LOCK.
   No heap allocation is needed for lower completion; even asynchronous lower
   requests are drained before the IRP is completed here. START publishes only
   after lower success and exact resource validation. Successful QUERY_STOP or
   QUERY_REMOVE fences queries/acquisitions until matching successful CANCEL;
   STOP withdraws the binding, START can rebind it. Live references, leases or
   controller journals veto orderly stop/remove before forwarding.
   SURPRISE/REMOVE withdraw new work, drain the hardware gate before forwarding,
   and prohibit subsequent MMIO. REMOVE cannot be vetoed: live/poisoned resources
   are retained. The host must quarantine such storage, not detach/free it merely
   because this IRP completed; Close success and its own dispatch/interface
   rundown are both required. This does not attach a filter or provide shared
   clocks/PMIC/address/cache/OPP resources. Other PnP requests pass synchronously
   to the lower driver with their incoming IoStatus preserved. */
/* Same processing without completing the IRP, for a host that must drain its
   remove lock and detach before the final IoCompleteRequest. */
/* Shared nonalertable lower-PnP completion drain; caller validates/pins IRP and lower. */
NTSTATUS MtkMaliBoardForwardPnp(PDEVICE_OBJECT Lower,PIRP);
NTSTATUS MtkMaliBoardProcessPnp(MALI_BOARD_PROVIDER *,PDEVICE_OBJECT Pdo,
    PDEVICE_OBJECT Lower,PIRP);
NTSTATUS MtkMaliBoardDispatchPnp(MALI_BOARD_PROVIDER *,PDEVICE_OBJECT Pdo,
    PDEVICE_OBJECT Lower,PIRP);

#endif
