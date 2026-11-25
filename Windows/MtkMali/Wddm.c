/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Wddm.h"
#include <stdint.h>

static int MtkMaliWddmAddOverflows(uintptr_t Start, size_t Length) {
    return Length > (size_t)(UINTPTR_MAX - Start);
}

MALI_RESULT MtkMaliWddmGetMmuCaps(
    DXGK_GPUMMUCAPS *Caps,
    DXGK_PAGE_TABLE_LEVEL_DESC *Levels,
    UINT LevelCapacity) {
    DXGK_GPUMMUCAPS NewCaps;
    DXGK_PAGE_TABLE_LEVEL_DESC NewLevels[MALI_WDDM_PAGE_TABLE_LEVELS];

    if (Caps == 0 || Levels == 0 ||
        LevelCapacity < MALI_WDDM_PAGE_TABLE_LEVELS) {
        return MaliInvalid;
    }

    /* Initialize every field so reserved/capability bits are never inherited. */
    NewCaps.Value = 0;
    NewCaps.ReadOnlyMemorySupported = 1;
    NewCaps.NoExecuteMemorySupported = 1;
    NewCaps.PageTableUpdateRequireAddressSpaceIdle = 1;
    NewCaps.ExplicitPageTableInvalidation = 1;
    NewCaps.PageTableUpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    NewCaps.VirtualAddressBitCount = 48;
    NewCaps.LeafPageTableSizeFor64KPagesInBytes = 0;
    NewCaps.PageTableLevelCount = MALI_WDDM_PAGE_TABLE_LEVELS;
    NewCaps.LegacyBehaviors.SourcePageTableVaInTransfer = 0;
    NewCaps.LegacyBehaviors.Reserved = 0;

    for (UINT Level = 0; Level < MALI_WDDM_PAGE_TABLE_LEVELS; ++Level) {
        NewLevels[Level].PageTableIndexBitCount = 9;
        NewLevels[Level].PageTableSegmentId = 0;
        NewLevels[Level].PagingProcessPageTableSegmentId = 0;
        NewLevels[Level].PageTableSizeInBytes = MALI_WDDM_TABLE_BYTES;
        NewLevels[Level].PageTableAlignmentInBytes = MALI_WDDM_TABLE_BYTES;
    }

    *Caps = NewCaps;
    for (UINT Level = 0; Level < MALI_WDDM_PAGE_TABLE_LEVELS; ++Level) {
        Levels[Level] = NewLevels[Level];
    }
    return MaliOk;
}

MALI_RESULT MtkMaliWddmInitialize(
    MALI_WDDM_BRIDGE *Bridge,
    const MALI_WDDM_CACHE_OPS *CacheOps) {
    if (Bridge == 0) {
        return MaliInvalid;
    }

    /* A failed reinitialization must not leave stale cache callbacks usable. */
    Bridge->Cache.Context = 0;
    Bridge->Cache.CleanForGpu = 0;
    Bridge->Cache.StoreBarrier = 0;
    if (CacheOps == 0 ||
        CacheOps->CleanForGpu == 0 || CacheOps->StoreBarrier == 0) {
        return MaliInvalid;
    }
    Bridge->Cache = *CacheOps;
    return MaliOk;
}

static MALI_RESULT MtkMaliWddmConvertPte(
    const DXGK_PTE *Source,
    UINT MaliLevel,
    MALI_PTE *Destination) {
    if (Source == 0 || Destination == 0 || MaliLevel > 3) {
        return MaliInvalid;
    }

    if (Source->SystemReserved0 != 0 || Source->Reserved != 0) {
        return MaliInvalid;
    }

    if (Source->Valid == 0) {
        /* Invalid entries carry no page/segment attributes; always clear them. */
        Destination->Address = 0;
        Destination->Flags = 0;
        return MaliOk;
    }

    /* This bridge owns system RAM only and has one physical adapter. */
    if (Source->Segment != 0 || Source->PhysicalAdapterIndex != 0) {
        return MaliUnsupported;
    }
    if (Source->LargePage != 0 ||
        Source->PageTablePageSize != DXGK_PTE_PAGE_TABLE_PAGE_4KB) {
        return MaliUnsupported;
    }

    /* DXGK zero-fill PTEs require hardware support not provided by MaliJm. */
    if (Source->Zero != 0) {
        return MaliUnsupported;
    }

    /* The SoC's CPU/GPU cache-coherency contract is not established here. */
    if (Source->CacheCoherent != 0) {
        return MaliUnsupported;
    }

    if (MaliLevel < 3) {
        /* Mali table descriptors cannot encode WDDM per-subtree protections. */
        if (Source->ReadOnly != 0 || Source->NoExecute != 0) {
            return MaliUnsupported;
        }
        Destination->Address = Source->PageTableAddress;
        Destination->Flags = MaliPteValid | MaliPteTable;
        return MaliOk;
    }

    /* GPU cacheability is independent of DXGK CacheCoherent. */
    Destination->Address = Source->PageAddress;
    Destination->Flags = MaliPteValid | MaliPteRead | MaliPteCached;
    if (Source->ReadOnly == 0) {
        Destination->Flags |= MaliPteWrite;
    }
    if (Source->NoExecute == 0) {
        Destination->Flags |= MaliPteExecute;
    }
    return MaliOk;
}

MALI_RESULT MtkMaliWddmUpdatePageTable(
    MALI_WDDM_BRIDGE *Bridge,
    const DXGKARG_BUILDPAGINGBUFFER *Build) {
    const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *Update;
    uintptr_t TableAddress;
    uintptr_t EntriesAddress;
    size_t InputCount;
    size_t InputBytes;
    size_t WriteOffset;
    size_t WriteBytes;
    UINT MaliLevel;
    MALI_RESULT Result;

    if (Bridge == 0 || Build == 0 ||
        Bridge->Cache.CleanForGpu == 0 ||
        Bridge->Cache.StoreBarrier == 0) {
        return MaliInvalid;
    }
    if (Build->Operation != DXGK_OPERATION_UPDATE_PAGE_TABLE) {
        return MaliUnsupported;
    }
    if (Build->pDmaBuffer != 0) {
        return MaliUnsupported;
    }

    Update = &Build->UpdatePageTable;
    if (Update->UpdateMode != DXGK_PAGETABLEUPDATE_CPU_VIRTUAL) {
        return MaliUnsupported;
    }
    if (Update->Reserved0 != 0 || Update->Flags.Reserved != 0) {
        return MaliInvalid;
    }
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM3_1)
    if (Update->Flags.NativeFence != 0) {
        return MaliUnsupported;
    }
#endif
    /* Eviction is a lifetime notification, not a PTE write. The VM tracker
       drops its mirror; no source entries or cache operation are required. */
    if (Update->Flags.NotifyEviction) {
        return Update->Flags.InitialUpdate ? MaliInvalid : MaliOk;
    }
    if (Update->Flags.Use64KBPages != 0 ||
        Update->pPageTableEntries64KB != 0) {
        return MaliUnsupported;
    }
    if (Update->PageTableLevel >= MALI_WDDM_PAGE_TABLE_LEVELS ||
        Update->NumPageTableEntries == 0 ||
        Update->StartIndex > MALI_WDDM_ENTRIES_PER_TABLE ||
        Update->NumPageTableEntries >
            MALI_WDDM_ENTRIES_PER_TABLE - Update->StartIndex) {
        return MaliInvalid;
    }
    if (Update->PageTableAddress.CpuVirtual == 0 ||
        Update->pPageTableEntries == 0) {
        return MaliInvalid;
    }

    TableAddress = (uintptr_t)Update->PageTableAddress.CpuVirtual;
    if ((TableAddress & (MALI_WDDM_TABLE_BYTES - 1u)) != 0 ||
        MtkMaliWddmAddOverflows(TableAddress, MALI_WDDM_TABLE_BYTES)) {
        return MaliInvalid;
    }

    InputCount = Update->Flags.Repeat ? 1u : Update->NumPageTableEntries;
    InputBytes = InputCount * sizeof(DXGK_PTE);
    EntriesAddress = (uintptr_t)Update->pPageTableEntries;
    if ((EntriesAddress & (__alignof(DXGK_PTE) - 1u)) != 0 ||
        MtkMaliWddmAddOverflows(EntriesAddress, InputBytes)) {
        return MaliInvalid;
    }

    WriteOffset = (size_t)Update->StartIndex * sizeof(uint64_t);
    WriteBytes = (size_t)Update->NumPageTableEntries * sizeof(uint64_t);
    if (WriteOffset > MALI_WDDM_TABLE_BYTES ||
        WriteBytes > MALI_WDDM_TABLE_BYTES - WriteOffset ||
        MtkMaliWddmAddOverflows(TableAddress + WriteOffset, WriteBytes)) {
        return MaliInvalid;
    }

    /* WDK level 0 is leaf; Mali level 3 is leaf. */
    MaliLevel = (MALI_WDDM_PAGE_TABLE_LEVELS - 1u) - Update->PageTableLevel;

    /* Convert every source PTE first. No target table bytes have been touched. */
    for (UINT Index = 0; Index < InputCount; ++Index) {
        Result = MtkMaliWddmConvertPte(
            &Update->pPageTableEntries[Index], MaliLevel,
            &Bridge->Converted[Index]);
        if (Result != MaliOk) {
            return Result;
        }
    }

    /* MaliJmWritePtes validates and encodes its complete input before writing. */
    Result = MaliJmWritePtes(
        (uint64_t *)Update->PageTableAddress.CpuVirtual,
        MaliLevel,
        Update->StartIndex,
        Update->NumPageTableEntries,
        Bridge->Converted,
        Update->Flags.Repeat ? 1 : 0,
        Bridge->Encoded,MALI_WDDM_ENTRIES_PER_TABLE);
    if (Result != MaliOk) {
        return Result;
    }

    Bridge->Cache.CleanForGpu(
        Bridge->Cache.Context,
        (const void *)(TableAddress + WriteOffset),
        WriteBytes);
    Bridge->Cache.StoreBarrier(Bridge->Cache.Context);
    return MaliOk;
}
