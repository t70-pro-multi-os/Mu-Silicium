/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef T70_MALI_WDDM_H
#define T70_MALI_WDDM_H

#include <ntddk.h>
#include <windef.h>
#include <d3dkmddi.h>
#include "MaliJm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MALI_WDDM_PAGE_TABLE_LEVELS 4u
#define MALI_WDDM_ENTRIES_PER_TABLE 512u
#define MALI_WDDM_TABLE_BYTES MALI_JM_PAGE_SIZE

/*
 * This is only the callable page-table bridge used by a real KMD paging DDI.
 * It does not register a render miniport and does not implement GPU submission.
 * The owner must serialize bridge calls and provide real CPU-cache maintenance
 * for the page-table backing memory plus a store barrier. Callbacks are required;
 * there is intentionally no success-only fallback.
 */
/*
 * CleanForGpu must clean the addressed bytes (including cache-line rounding)
 * to the GPU-visible point of coherency and return after maintenance completes.
 * StoreBarrier must provide the required CPU store ordering/completion barrier.
 */
typedef void (*MALI_WDDM_CLEAN_FOR_GPU)(void *Context,
                                         const void *Address,
                                         size_t Length);
typedef void (*MALI_WDDM_STORE_BARRIER)(void *Context);

typedef struct {
    void *Context;
    MALI_WDDM_CLEAN_FOR_GPU CleanForGpu;
    MALI_WDDM_STORE_BARRIER StoreBarrier;
} MALI_WDDM_CACHE_OPS;

typedef struct {
    MALI_WDDM_CACHE_OPS Cache;
    /* Shared staging is safe only while the caller serializes every update. */
    MALI_PTE Converted[MALI_WDDM_ENTRIES_PER_TABLE];
    uint64_t Encoded[MALI_WDDM_ENTRIES_PER_TABLE];
} MALI_WDDM_BRIDGE;

/*
 * Advertises fixed 4 KiB, four-level, 48-bit GPUVA tables in system segment 0.
 * DXGK level 0 is the leaf; Mali level 3 is the leaf. No 64 KiB, large-page,
 * dual-PTE, zero-PTE, or CPU/GPU cache-coherent mode is advertised. Valid
 * leaf mappings use the Mali GPU-cacheable attribute independently of DXGK's
 * CacheCoherent bit; CPU/GPU data-buffer maintenance remains a separate path.
 */
MALI_RESULT MtkMaliWddmGetMmuCaps(
    DXGK_GPUMMUCAPS *Caps,
    DXGK_PAGE_TABLE_LEVEL_DESC *Levels,
    UINT LevelCapacity);

/* Requires real platform cache-clean and barrier callbacks. */
MALI_RESULT MtkMaliWddmInitialize(
    MALI_WDDM_BRIDGE *Bridge,
    const MALI_WDDM_CACHE_OPS *CacheOps);

/*
 * Consumes only DXGK_OPERATION_UPDATE_PAGE_TABLE in CPU_VIRTUAL mode, with a
 * NULL DMA buffer. PageTableAddress.CpuVirtual is a trusted kernel page-table
 * pointer supplied by Dxgkrnl; pPageTableEntries is likewise trusted kernel
 * input. Never pass a user pointer or an unowned scratch physical address.
 * The full input range is validated before the first destination write.
 */
MALI_RESULT MtkMaliWddmUpdatePageTable(
    MALI_WDDM_BRIDGE *Bridge,
    const DXGKARG_BUILDPAGINGBUFFER *Build);

#ifdef __cplusplus
}
#endif
#endif
