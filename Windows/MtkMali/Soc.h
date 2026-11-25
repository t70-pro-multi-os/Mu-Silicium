/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_SOC_H
#define T70_MALI_SOC_H
#include "Host.h"
#include "ConnPowerInterface.h"
#define MALI_SOC_REGIONS 5u
#define MALI_SOC_CONN_FIELDS 12u
enum {MaliSocProofOwner=1,MaliSocProofDirectDma=2,MaliSocProofIdle=4,MaliSocProofOpp=8};
/* Kernel-only qualification input. The real owner proves firmware/secure-world
   exclusion, live shared PMIC/VS2/26 MHz supply, direct DMA for this PDO and current limits.
   Entries are the device-qualified adjusted table, never the ROM default.
   Validate must not reenter the broker; references are nonfailing at PASSIVE.
   No user IOCTL, registry switch, default certificate or success-only backend. */
typedef struct {
    ULONG Size,Version;
    void *Context;
    void (*Reference)(void *);
    void (*Dereference)(void *);
    NTSTATUS (*Validate)(void *,PDEVICE_OBJECT,ULONG Proofs,uint64_t Hz,uint32_t VgpuUv,uint32_t VsramUv);
    const MALI_MFG_OPP *Entries;
    ULONG EntryCount;
    /* Version 2: optional real platform-supplied raw calibration. Entries then
       contain the unadjusted stock table. The broker copies only the result. */
    const MALI_MFG_CALIBRATION *Calibration;
    MALI_VM_ADDRESS_MODEL AddressModel;
    SIZE_T ApertureCommitLimit;
    uint64_t TimeoutTicks;
} MALI_SOC_QUALIFICATION;
typedef struct {
    ULONG Signature,Started,Stopping,Quarantined,Qualified,ReferenceFault,Suspended;
    volatile LONG Closing,Removed;
    KMUTEX Mutex;
    PKTHREAD AccessThread;
    uintptr_t Generation;
    void *OpenCookie,*ResourceCookie;
    PDEVICE_OBJECT GpuPdo;
    LONG References;
    LONG ConnReferences;
    ULONGLONG ConnGeneration,ConnToken,ConnEmiBase;
    ULONG ConnSaved[MALI_SOC_CONN_FIELDS];
    MALI_BOARD_LIFETIME Lifetime;
    void *Mappings[MALI_SOC_REGIONS];
    CM_PARTIAL_RESOURCE_DESCRIPTOR Raw[MALI_SOC_REGIONS],Translated[MALI_SOC_REGIONS];
    MALI_SOC_QUALIFICATION Qualification;
    MALI_MFG_OPP_POLICY Policy;
    MALI_MFG_PMIC Pmic;
    MALI_MFG_RAILS Rails;
    MALI_MFG_HW_CLOCK Clock;
    MALI_MFG_CONTROLLER Controller;
} MALI_SOC_PROVIDER;
/* OS-owned successful START resources only: five exact exclusive DT ranges in
   any order. Map all or unwind all; initialization/START/Open do no MMIO. */
NTSTATUS MtkMaliSocInitialize(MALI_SOC_PROVIDER *,const MALI_BOARD_LIFETIME *);
NTSTATUS MtkMaliSocStart(MALI_SOC_PROVIDER *,const CM_RESOURCE_LIST *,const CM_RESOURCE_LIST *);
NTSTATUS MtkMaliSocSetQualification(MALI_SOC_PROVIDER *,const MALI_SOC_QUALIFICATION *);
/* Caller has verified the GPU hardware ID; GetFactory independently checks it.
   Success owns one device-pinned reference and retains that GPU PDO. */
NTSTATUS MtkMaliSocGetFactory(MALI_SOC_PROVIDER *,PDEVICE_OBJECT,MALI_BOARD_PARENT_INTERFACE *);
NTSTATUS MtkMaliSocGetConnPower(MALI_SOC_PROVIDER *,MTK_CONN_POWER_INTERFACE *);
NTSTATUS MtkMaliSocSetStopping(MALI_SOC_PROVIDER *,ULONG Minor);
NTSTATUS MtkMaliSocCancelStopping(MALI_SOC_PROVIDER *,ULONG Minor);
NTSTATUS MtkMaliSocStop(MALI_SOC_PROVIDER *);
NTSTATUS MtkMaliSocFencePower(MALI_SOC_PROVIDER *);
NTSTATUS MtkMaliSocSuspend(MALI_SOC_PROVIDER *);
NTSTATUS MtkMaliSocResume(MALI_SOC_PROVIDER *);
/* Withdraw before waiting for active accesses, then prohibit all MMIO.
   Revoke the CONN token. Retain mappings until client references drain. */
NTSTATUS MtkMaliSocWithdraw(MALI_SOC_PROVIDER *);
NTSTATUS MtkMaliSocClose(MALI_SOC_PROVIDER *);
/* Trusted nonpaged Dxgkrnl page-table range, as required by Wddm.h. Stack MDL
   per page; real HAL cache clean and a completion barrier, no allocation. */
void MtkMaliSocCleanForGpu(void *,const void *,size_t);
void MtkMaliSocStoreBarrier(void *);
#endif
