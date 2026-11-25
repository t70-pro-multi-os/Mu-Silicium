/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_MINIPORT_H
#define T70_MALI_MINIPORT_H
#include "Runtime.h"
#define MALI_MINIPORT_POOL_TAG 0x50303754u
/* The SoC provider owns shared PMIC/SPM/clock registers and serialized leases.
   QUERY_INTERFACE success owns one reference. Acquire may return a cookie on
   failure: it then retains hardware and Release must succeed before dereference.
   The provider belongs to this same GPU stack and must associate the three
   translated resources with stock GPU/MMU/JOB before granting a lease.
   Ready proves that association, live exclusive power/clocks and a safe OPP.
   AddressModel and nonfailing PoC cache operations are explicit provider proofs,
   never inferred from a successful MMIO map or Android's old boot state.
   All methods run serialized at PASSIVE; interface/cookie survive failed Stop.
   A provider implementing these operations is still required on the real board. */
typedef struct {
    void *Cookie;
    MALI_VM_ADDRESS_MODEL AddressModel;
    MALI_WDDM_CACHE_OPS Cache;
    SIZE_T ApertureCommitLimit;
    uint64_t TimeoutTicks;
} MALI_BOARD_LEASE;
typedef struct {
    INTERFACE Header;
    NTSTATUS (*Acquire)(void *,MALI_BOARD_LEASE *);
    NTSTATUS (*Ready)(void *,void *Cookie);
    NTSTATUS (*Release)(void *,void *Cookie);
} MALI_BOARD_INTERFACE;
/* Separate GUID preserves the original board interface. Optional at Start;
   without it D3 is rejected before MMIO. QUERY_INTERFACE success owns a second
   reference from this same device stack. SetPower validates the board Cookie.
   D3 succeeds only after exclusive GPU rails/clocks/domains are safely down;
   D0 succeeds only after the original address/cache/IRQ/OPP proofs are restored.
   Every call is serialized PASSIVE with Core stopped, IRQs masked/disconnected,
   worker stopped and MMIO callbacks closed/drained. Repeated D0/D3 and retry
   after a partially failed call must be supported. Failure retains the cookie,
   resources and provider state; the miniport treats board power as unknown.
   Release of the original board lease must also handle D3/unknown state without
   GPU MMIO. No device register may be accessed until D0 is proved again. */
typedef struct {
    INTERFACE Header;
    NTSTATUS (*SetPower)(void *,void *Cookie,DEVICE_POWER_STATE,POWER_ACTION);
} MALI_BOARD_POWER_INTERFACE;
/* QUERY_INTERFACE supplemental input. The PnP provider must match these
   translated IRQs against its own successful START_DEVICE raw/translated pair,
   from this same PDO. It never treats a translated vector as a GIC GSIV. */
typedef struct {
    ULONG Size,Version;
    PDEVICE_OBJECT Pdo;
    CM_PARTIAL_RESOURCE_DESCRIPTOR Interrupts[3]; /* GPU, MMU, JOB */
} MALI_BOARD_QUERY;
#define MALI_BOARD_INTERFACE_GUID_INIT {0xbefb6789,0xb701,0x4f45,{0x9a,0x70,0x5c,0x29,0x83,0x01,0x7b,0x10}}
#define MALI_BOARD_POWER_INTERFACE_GUID_INIT {0x7f90436a,0x5e32,0x4b1b,{0x94,0x3f,0x08,0x87,0xf0,0x70,0xea,0x4c}}
extern const GUID MtkMaliBoardInterfaceGuid;
extern const GUID MtkMaliBoardPowerInterfaceGuid;
typedef struct {
    MALI_RUNTIME Runtime; /* hAdapter == AddDevice context, including real DDIs */
    MALI_JM Core;
    uint32_t Signature,Stages,CoreTouched;
    volatile LONG Started;
    DEVICE_POWER_STATE PowerState;
    PDEVICE_OBJECT Pdo;
    DXGKRNL_INTERFACE Dxgk;
    DXGK_START_INFO StartInfo;
    MALI_BOARD_INTERFACE Board;
    MALI_BOARD_POWER_INTERFACE Power;
    MALI_BOARD_LEASE Lease;
    CM_PARTIAL_RESOURCE_DESCRIPTOR Interrupts[3];
    void *Gpu;
    volatile LONG PnpActive,Closing,ActiveCallbacks,IoFault;
    NTSTATUS StartStatus,CleanupStatus,PowerStatus;
} MALI_MINIPORT;
DRIVER_INITIALIZE DriverEntry;
DXGKDDI_ADD_DEVICE MtkMaliMiniportAddDevice;
DXGKDDI_START_DEVICE MtkMaliMiniportStartDevice;
DXGKDDI_STOP_DEVICE MtkMaliMiniportStopDevice;
DXGKDDI_REMOVE_DEVICE MtkMaliMiniportRemoveDevice;
DXGKDDI_INTERRUPT_ROUTINE MtkMaliMiniportInterrupt;
DXGKDDI_DPC_ROUTINE MtkMaliMiniportDpc;
#endif
