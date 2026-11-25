/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_RESIDENCY_H
#define T70_MALI_RESIDENCY_H
#include "Prepare.h"
#include "Memory.h"
#define MALI_RESIDENCY_DATA_LIMIT (17u+MALI_COMMAND_MAX_JOBS)
/* A contiguous region can straddle boundaries at all three table levels:
   at most two tables per level, plus its remaining full 2 MiB leaf spans. */
#define MALI_RESIDENCY_TABLE_LIMIT (1u+3u*(MALI_COMMAND_MAX_JOBS+2u)+6u*MALI_COMMAND_MAX_RESOURCES+MALI_COMMAND_MAX_RESOURCE_PAGES/512u)
struct MALI_RESIDENCY_LEASE;
typedef struct {
    uint32_t Signature, Count;
    MALI_DDI_ADAPTER *Adapter;
    MALI_VM *Vm;
    struct MALI_RESIDENCY_LEASE *Leases;
    void *WakeContext;
    void (*Wake)(void *);
    NTSTATUS LastReleaseStatus;
} MALI_RESIDENCY;
/* Call GetOps before Prepare/Ddi initialization, then initialize this new
   nonpaged backend after attaching the VM. Caller holds the adapter gate for
   every operation except Wake. OS shared backing must already be enabled.
   Uses fresh MDLs to lock shared backing, verifies each PFN against the held
   OS GPUVA mapping, and builds an exclusive per-submission physical root.
   Requires the parent's proven direct-system-physical GPU address model.
   Version 2 retains full resource MDLs and enforces requested GPU R/W/X,
   physical W^X across live submissions, and private MMU containment of compute. */
void MtkMaliResidencyGetOps(MALI_RESIDENCY *,MALI_PREPARE_OPS *);
NTSTATUS MtkMaliResidencyInitialize(MALI_RESIDENCY *,MALI_DDI_ADAPTER *,MALI_VM *,void *,void (*)(void *));
NTSTATUS MtkMaliResidencyShutdown(MALI_RESIDENCY *);
#endif
