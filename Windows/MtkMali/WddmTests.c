/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/*
 * Actual Wddm.c + MaliJm.c bridge tests. The harness supplies only cache
 * maintenance mocks; PTE conversion, validation and table writes are production
 * code. Expose TestMain for the existing ARM64 no-CRT runner.
 */
#include "Wddm.h"
#include <stdint.h>

#if defined(_MSC_VER)
__declspec(align(4096)) static uint64_t TestTable[MALI_WDDM_ENTRIES_PER_TABLE];
#else
_Alignas(4096) static uint64_t TestTable[MALI_WDDM_ENTRIES_PER_TABLE];
#endif

static MALI_WDDM_BRIDGE TestBridge;
static DXGKARG_BUILDPAGINGBUFFER TestBuild;
static DXGK_PTE TestSource[4];
static DXGK_GPUMMUCAPS TestCaps;
static DXGK_PAGE_TABLE_LEVEL_DESC TestLevels[MALI_WDDM_PAGE_TABLE_LEVELS];
static MALI_WDDM_CACHE_OPS TestOps;
unsigned MtkMaliWddmAssertionCount;
unsigned MtkMaliWddmFailureCount;
static unsigned TestSequence;
static unsigned TestCleanCalls;
static unsigned TestBarrierCalls;
static const void *TestExpectedCleanAddress;
static size_t TestExpectedCleanLength;
static uint64_t TestExpectedFirstValue;
static int TestCleanSawWrittenData;

static unsigned FirstFailure;
#define TestCheck(C) TestCheckAt((C), __LINE__)
static void TestCheckAt(int Condition, unsigned Line) {
    ++MtkMaliWddmAssertionCount;
    if (!Condition) {
        ++MtkMaliWddmFailureCount;
        if(!FirstFailure) FirstFailure=Line;
    }
}

static void TestZero(void *Destination, size_t Length) {
    unsigned char *Bytes = (unsigned char *)Destination;
    for (size_t Index = 0; Index < Length; ++Index) {
        Bytes[Index] = 0;
    }
}

static void TestCleanForGpu(void *Context, const void *Address, size_t Length) {
    (void)Context;
    ++TestCleanCalls;
    TestSequence = TestSequence * 10u + 1u;
    TestCleanSawWrittenData =
        Address == TestExpectedCleanAddress &&
        Length == TestExpectedCleanLength &&
        TestExpectedCleanAddress != 0 &&
        *(const uint64_t *)TestExpectedCleanAddress == TestExpectedFirstValue;
}

static void TestStoreBarrier(void *Context) {
    (void)Context;
    ++TestBarrierCalls;
    TestSequence = TestSequence * 10u + 2u;
}

static void TestClearCallbacks(void) {
    TestSequence = 0;
    TestCleanCalls = 0;
    TestBarrierCalls = 0;
    TestExpectedCleanAddress = 0;
    TestExpectedCleanLength = 0;
    TestExpectedFirstValue = 0;
    TestCleanSawWrittenData = 0;
}

static void TestFillTable(uint64_t Value) {
    for (UINT Index = 0; Index < MALI_WDDM_ENTRIES_PER_TABLE; ++Index) {
        TestTable[Index] = Value;
    }
}

static void TestSetPte(DXGK_PTE *Pte, int Valid, uint64_t Address) {
    TestZero(Pte, sizeof(*Pte));
    Pte->Valid = Valid ? 1u : 0u;
    Pte->PageTablePageSize = DXGK_PTE_PAGE_TABLE_PAGE_4KB;
    Pte->PageAddress = Address;
}

static void TestSetBuild(UINT Level, UINT Start, UINT Count,
                         DXGK_PTE *Entries) {
    TestZero(&TestBuild, sizeof(TestBuild));
    TestBuild.Operation = DXGK_OPERATION_UPDATE_PAGE_TABLE;
    TestBuild.UpdatePageTable.PageTableLevel = Level;
    TestBuild.UpdatePageTable.PageTableAddress.CpuVirtual = TestTable;
    TestBuild.UpdatePageTable.pPageTableEntries = Entries;
    TestBuild.UpdatePageTable.StartIndex = Start;
    TestBuild.UpdatePageTable.NumPageTableEntries = Count;
    TestBuild.UpdatePageTable.UpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
}

static void TestResetBridge(void) {
    TestOps.Context = 0;
    TestOps.CleanForGpu = TestCleanForGpu;
    TestOps.StoreBarrier = TestStoreBarrier;
    TestCheck(MtkMaliWddmInitialize(&TestBridge, &TestOps) == MaliOk);
    TestClearCallbacks();
}

static void TestCapsContract(void) {
    TestZero(&TestCaps, sizeof(TestCaps));
    TestCaps.Value = 0xa5a5a5a5u;
    TestLevels[0].PageTableSizeInBytes = 123u;
    TestCheck(MtkMaliWddmGetMmuCaps(&TestCaps, TestLevels, 3u) == MaliInvalid);
    TestCheck(TestCaps.Value == 0xa5a5a5a5u);
    TestCheck(TestLevels[0].PageTableSizeInBytes == 123u);

    TestCheck(MtkMaliWddmGetMmuCaps(
        &TestCaps, TestLevels, MALI_WDDM_PAGE_TABLE_LEVELS) == MaliOk);
    TestCheck(TestCaps.PageTableUpdateMode == DXGK_PAGETABLEUPDATE_CPU_VIRTUAL);
    TestCheck(TestCaps.VirtualAddressBitCount == 48u);
    TestCheck(TestCaps.PageTableLevelCount == 4u);
    TestCheck(TestCaps.ReadOnlyMemorySupported == 1u);
    TestCheck(TestCaps.NoExecuteMemorySupported == 1u);
    TestCheck(TestCaps.PageTableUpdateRequireAddressSpaceIdle == 1u);
    TestCheck(TestCaps.ExplicitPageTableInvalidation == 1u);
    TestCheck(TestCaps.ZeroInPteSupported == 0u);
    TestCheck(TestCaps.CacheCoherentMemorySupported == 0u);
    TestCheck(TestCaps.LargePageSupported == 0u);
    TestCheck(TestCaps.DualPteSupported == 0u);
    TestCheck(TestCaps.SysMem64KBPageSupported == 0u);
    TestCheck(TestCaps.LeafPageTableSizeFor64KPagesInBytes == 0u);
    for (UINT Level = 0; Level < MALI_WDDM_PAGE_TABLE_LEVELS; ++Level) {
        TestCheck(TestLevels[Level].PageTableIndexBitCount == 9u);
        TestCheck(TestLevels[Level].PageTableSegmentId == 0u);
        TestCheck(TestLevels[Level].PagingProcessPageTableSegmentId == 0u);
        TestCheck(TestLevels[Level].PageTableSizeInBytes == 4096u);
        TestCheck(TestLevels[Level].PageTableAlignmentInBytes == 4096u);
    }
}

static void TestLeafMappingAndInvalidation(void) {
    MALI_PTE Expected;
    uint64_t Encoded = 0;

    TestResetBridge();
    TestFillTable(UINT64_C(0xfeedfacecafebeef));
    TestSetPte(&TestSource[0], 1, UINT64_C(0x12345000));
    TestSetPte(&TestSource[1], 1, UINT64_C(0x23456000));
    TestSource[1].ReadOnly = 1;
    TestSource[1].NoExecute = 1;
    TestSetPte(&TestSource[2], 0, UINT64_C(0x34567000));
    TestSource[2].Zero = 1;

    TestExpectedCleanAddress = &TestTable[17];
    TestExpectedCleanLength = 3u * sizeof(uint64_t);
    Expected.Address = UINT64_C(0x12345000);
    Expected.Flags = MaliPteValid | MaliPteRead | MaliPteWrite |
                     MaliPteExecute | MaliPteCached;
    TestExpectedFirstValue = 0;
    TestCheck(MaliJmEncodePte(3u, &Expected, &Encoded) == MaliOk);
    TestExpectedFirstValue = Encoded;

    TestSetBuild(0u, 17u, 3u, TestSource);
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliOk);
    TestCheck(TestTable[17] == Encoded);

    Expected.Address = UINT64_C(0x23456000);
    Expected.Flags = MaliPteValid | MaliPteRead | MaliPteCached;
    TestCheck(MaliJmEncodePte(3u, &Expected, &Encoded) == MaliOk);
    TestCheck(TestTable[18] == Encoded);
    TestCheck(TestTable[19] == 0);
    TestCheck(TestTable[16] == UINT64_C(0xfeedfacecafebeef));
    TestCheck(TestTable[20] == UINT64_C(0xfeedfacecafebeef));
    TestCheck(TestCleanCalls == 1u);
    TestCheck(TestBarrierCalls == 1u);
    TestCheck(TestSequence == 12u);
    TestCheck(TestCleanSawWrittenData == 1);
    TestCheck(TestExpectedCleanAddress == &TestTable[17]);
    TestCheck(TestExpectedCleanLength == 24u);
}

static void TestRepeatMapping(void) {
    TestResetBridge();
    TestFillTable(UINT64_C(0xaaaaaaaaaaaaaaaa));
    TestSetPte(&TestSource[0], 1, UINT64_C(0x45678000));
    TestSetBuild(0u, 40u, 5u, TestSource);
    TestBuild.UpdatePageTable.Flags.Repeat = 1;
    TestExpectedCleanAddress = &TestTable[40];
    TestExpectedCleanLength = 5u * sizeof(uint64_t);

    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliOk);
    TestCheck(TestCleanCalls == 1u);
    TestCheck(TestBarrierCalls == 1u);
    for (UINT Index = 41; Index < 45; ++Index) {
        TestCheck(TestTable[Index] == TestTable[40]);
    }
    TestCheck(TestTable[39] == UINT64_C(0xaaaaaaaaaaaaaaaa));
    TestCheck(TestTable[45] == UINT64_C(0xaaaaaaaaaaaaaaaa));
}

static void TestLevelOrderAndTableDescriptor(void) {
    MALI_PTE Expected;
    uint64_t Encoded = 0;

    TestResetBridge();
    for (UINT WdkLevel = 1u; WdkLevel <= 3u; ++WdkLevel) {
        UINT MaliLevel = 3u - WdkLevel;
        UINT Index = WdkLevel;
        TestFillTable(0);
        TestSetPte(&TestSource[0], 1,
                   UINT64_C(0x8000) + (uint64_t)Index * 4096u);
        TestSetBuild(WdkLevel, Index, 1u, TestSource);
        Expected.Address = TestSource[0].PageTableAddress;
        Expected.Flags = MaliPteValid | MaliPteTable;
        TestCheck(MaliJmEncodePte(MaliLevel, &Expected, &Encoded) == MaliOk);
        TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliOk);
        TestCheck(TestTable[Index] == Encoded);
        TestCheck(TestCleanCalls == WdkLevel);
        TestCheck(TestBarrierCalls == WdkLevel);
    }
}

static void TestNoPartialWriteOnBadLaterEntry(void) {
    TestResetBridge();
    TestFillTable(UINT64_C(0x0123456789abcdef));
    TestSetPte(&TestSource[0], 1, UINT64_C(0x10000000));
    TestSetPte(&TestSource[1], 1, UINT64_C(0x20000000));
    TestSource[1].Segment = 1;
    TestSetBuild(0u, 7u, 2u, TestSource);

    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliUnsupported);
    TestCheck(TestTable[7] == UINT64_C(0x0123456789abcdef));
    TestCheck(TestTable[8] == UINT64_C(0x0123456789abcdef));
    TestCheck(TestCleanCalls == 0u && TestBarrierCalls == 0u);

    TestSource[1].Segment = 0;
    TestSource[1].PageAddress = MALI_JM_PA_LIMIT;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliInvalid);
    TestCheck(TestTable[7] == UINT64_C(0x0123456789abcdef));
    TestCheck(TestTable[8] == UINT64_C(0x0123456789abcdef));
    TestCheck(TestCleanCalls == 0u && TestBarrierCalls == 0u);

    TestBuild.UpdatePageTable.NumPageTableEntries = 1u;
    TestSource[0].PageAddress = UINT64_C(0x10000001);
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliInvalid);
    TestCheck(TestTable[7] == UINT64_C(0x0123456789abcdef));
    TestCheck(TestCleanCalls == 0u && TestBarrierCalls == 0u);
}

static void TestRejectUnsupportedPteFeatures(void) {
    TestResetBridge();
    TestFillTable(UINT64_C(0x5555555555555555));
    TestSetBuild(0u, 0u, 1u, TestSource);

    TestSetPte(&TestSource[0], 1, UINT64_C(0x10000000));
    TestSource[0].Zero = 1;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliUnsupported);
    TestCheck(TestTable[0] == UINT64_C(0x5555555555555555));

    TestSetPte(&TestSource[0], 1, UINT64_C(0x10000000));
    TestSource[0].CacheCoherent = 1;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliUnsupported);
    TestCheck(TestTable[0] == UINT64_C(0x5555555555555555));

    TestSetPte(&TestSource[0], 1, UINT64_C(0x10000000));
    TestSource[0].LargePage = 1;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliUnsupported);

    TestSetPte(&TestSource[0], 1, UINT64_C(0x10000000));
    TestSource[0].PhysicalAdapterIndex = 1;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliUnsupported);

    TestSetPte(&TestSource[0], 1, UINT64_C(0x10000000));
    TestSource[0].Reserved = 1;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliInvalid);

    TestSetPte(&TestSource[0], 1, UINT64_C(0x10000000));
    TestSource[0].PageTablePageSize = DXGK_PTE_PAGE_TABLE_PAGE_64KB;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliUnsupported);

    TestSetPte(&TestSource[0], 1, UINT64_C(0x10000000));
    TestBuild.UpdatePageTable.PageTableLevel = 1u;
    TestSource[0].ReadOnly = 1;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliUnsupported);
    TestCheck(TestTable[0] == UINT64_C(0x5555555555555555));
    TestCheck(TestCleanCalls == 0u && TestBarrierCalls == 0u);
}

static void TestRejectInvalidUpdateShape(void) {
    TestResetBridge();
    TestFillTable(UINT64_C(0x7777777777777777));
    TestSetPte(&TestSource[0], 1, UINT64_C(0x10000000));
    TestSetBuild(0u, 0u, 1u, TestSource);

    TestBuild.UpdatePageTable.UpdateMode = DXGK_PAGETABLEUPDATE_GPU_VIRTUAL;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliUnsupported);

    TestBuild.UpdatePageTable.UpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    TestBuild.pDmaBuffer = TestTable;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliUnsupported);

    TestBuild.pDmaBuffer = 0;
    TestBuild.UpdatePageTable.PageTableLevel = 4u;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliInvalid);

    TestBuild.UpdatePageTable.PageTableLevel = 0u;
    TestBuild.UpdatePageTable.StartIndex = 512u;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliInvalid);

    TestBuild.UpdatePageTable.StartIndex = 511u;
    TestBuild.UpdatePageTable.NumPageTableEntries = 2u;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliInvalid);

    TestBuild.UpdatePageTable.StartIndex = 0u;
    TestBuild.UpdatePageTable.NumPageTableEntries = 1u;
    TestBuild.UpdatePageTable.Flags.Use64KBPages = 1;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliUnsupported);

    TestBuild.UpdatePageTable.Flags.Use64KBPages = 0;
    TestBuild.UpdatePageTable.pPageTableEntries64KB = TestSource;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliUnsupported);

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM3_1)
    TestBuild.UpdatePageTable.pPageTableEntries64KB = 0;
    TestBuild.UpdatePageTable.Flags.NativeFence = 1;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliUnsupported);
    TestBuild.UpdatePageTable.Flags.NativeFence = 0;
#endif

    TestBuild.UpdatePageTable.Flags.Reserved = 1;
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliInvalid);

    TestCheck(TestTable[0] == UINT64_C(0x7777777777777777));
    TestCheck(TestCleanCalls == 0u && TestBarrierCalls == 0u);
}

static void TestRejectMissingMaintenanceAndAlignment(void) {
    MALI_WDDM_CACHE_OPS BadOps;

    TestZero(&BadOps, sizeof(BadOps));
    TestCheck(MtkMaliWddmInitialize(&TestBridge, &BadOps) == MaliInvalid);
    TestSetPte(&TestSource[0], 1, UINT64_C(0x10000000));
    TestSetBuild(0u, 0u, 1u, TestSource);
    TestFillTable(UINT64_C(0x8888888888888888));
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliInvalid);
    TestCheck(TestTable[0] == UINT64_C(0x8888888888888888));

    TestResetBridge();
    TestFillTable(UINT64_C(0x8888888888888888));
    TestSetPte(&TestSource[0], 1, UINT64_C(0x10000000));
    TestSetBuild(0u, 0u, 1u, TestSource);
    TestBuild.UpdatePageTable.PageTableAddress.CpuVirtual =
        (void *)((uintptr_t)TestTable + sizeof(uint64_t));
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliInvalid);
    TestCheck(TestTable[0] == UINT64_C(0x8888888888888888));

    TestBuild.UpdatePageTable.PageTableAddress.CpuVirtual = TestTable;
    TestBuild.UpdatePageTable.pPageTableEntries =
        (DXGK_PTE *)((uintptr_t)TestSource + 1u);
    TestCheck(MtkMaliWddmUpdatePageTable(&TestBridge, &TestBuild) == MaliInvalid);
    TestCheck(TestTable[0] == UINT64_C(0x8888888888888888));
    TestCheck(TestCleanCalls == 0u && TestBarrierCalls == 0u);
}

int TestMain(void) {
    MtkMaliWddmAssertionCount = 0;
    MtkMaliWddmFailureCount = 0;
    TestCapsContract();
    TestLeafMappingAndInvalidation();
    TestRepeatMapping();
    TestLevelOrderAndTableDescriptor();
    TestNoPartialWriteOnBadLaterEntry();
    TestRejectUnsupportedPteFeatures();
    TestRejectInvalidUpdateShape();
    TestRejectMissingMaintenanceAndAlignment();
    return (int)FirstFailure;
}
