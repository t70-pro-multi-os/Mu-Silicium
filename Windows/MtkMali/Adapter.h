/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_ADAPTER_H
#define T70_MALI_ADAPTER_H
#include "Allocation.h"
#define MALI_ADAPTER_ABI_VERSION 1u
#define MALI_ADAPTER_IMMEDIATE_WRITE 1u
#define MALI_ADAPTER_TIMESTAMP 2u
#define MALI_ADAPTER_CACHE_JOB 4u
#define MALI_ADAPTER_RESOURCE_COMPUTE 8u
#define MALI_ADAPTER_RESOURCE_FRAGMENT 16u /* arch-9 MFBD, no tile-enable maps */
#define MALI_ADAPTER_RESOURCE_TILER 32u
#define MALI_ADAPTER_RESOURCE_MALLOC_VERTEX 64u
#define MALI_ADAPTER_RESOURCE_FULLSCREEN 128u
/* UMD wire data: no kernel pointers, physical addresses or native handles. */
typedef struct {uint32_t Version,Bytes,Flags,Reserved;} MALI_ADAPTER_QUERY;
typedef struct {
    uint32_t Version,Bytes,GpuId,Features;
    uint32_t VaBits,PaBits,PageBytes,AllocationVersion;
    uint32_t CommandVersion,MaxDmaBytes,MaxJobs,MaxStages;
    uint32_t MaxAllocationBytes,RequiredAllocationFlags,Reserved[2];
} MALI_ADAPTER_CAPS;
struct MALI_DDI_ADAPTER;
/* Parent-selected, page-aligned aperture budget, <= 1 GiB. Configure once after
   attaching the VM and enabling OS shared backing, before exposing queries.
   These capabilities describe the bounded command engine, not D3D levels. */
NTSTATUS MtkMaliDdiConfigureAdapter(struct MALI_DDI_ADAPTER *,SIZE_T CommitLimit);
#endif
