/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* OS pool fixture only. Production uses ExAllocatePool2/ExFreePoolWithTag. */
#ifndef T70_MALI_TEST_POOL_H
#define T70_MALI_TEST_POOL_H
#include "TestInterrupt.h"
#define TEST_POOL_SLOTS (MALI_ALLOCATION_LIMIT+MALI_ALLOCATION_BATCH+8u)
static __declspec(align(16)) uint8_t TestPoolBytes[TEST_POOL_SLOTS][256];
static uint8_t TestPoolUsed[TEST_POOL_SLOTS];
static unsigned TestPoolCursor,TestPoolCalls,TestPoolFrees,TestPoolLive,TestPoolFail;
static void (*TestPoolMutation)(void);
static void *TestPoolAllocate(POOL_FLAGS Flags,SIZE_T Bytes,ULONG Tag) {
    CHECK(Irql==PASSIVE_LEVEL && GateHeld && Flags==POOL_FLAG_NON_PAGED && Tag==0x41303754u && Bytes<=256);
    ++TestPoolCalls;if(TestPoolCalls==TestPoolFail)return 0;
    for(unsigned N=0;N<TEST_POOL_SLOTS;N++) {
        unsigned I=TestPoolCursor++;if(TestPoolCursor==TEST_POOL_SLOTS)TestPoolCursor=0;
        if(!TestPoolUsed[I]) {
            TestPoolUsed[I]=1;++TestPoolLive;RtlZeroMemory(TestPoolBytes[I],Bytes);
            if(TestPoolMutation)TestPoolMutation();
            return TestPoolBytes[I];
        }
    }
    return 0;
}
static void TestPoolFree(void *P,ULONG Tag) {
    CHECK(Irql==PASSIVE_LEVEL && GateHeld && Tag==0x41303754u);
    uintptr_t V=(uintptr_t)P,B=(uintptr_t)TestPoolBytes;
    CHECK(V>=B && V-B<sizeof(TestPoolBytes) && !((V-B)%256));
    if(V<B || V-B>=sizeof(TestPoolBytes) || (V-B)%256)return;
    unsigned I=(unsigned)((V-B)/256);CHECK(TestPoolUsed[I] && TestPoolLive);
    if(TestPoolUsed[I]){TestPoolUsed[I]=0;--TestPoolLive;++TestPoolFrees;}
}
static PHYSICAL_ADDRESS TestPoolUnexpectedPhysical(PVOID P) {
    PHYSICAL_ADDRESS A;A.QuadPart=0;CHECK(!P);return A;
}
#ifndef MmGetPhysicalAddress
#define MmGetPhysicalAddress TestPoolUnexpectedPhysical
#endif
#define ExAllocatePool2 TestPoolAllocate
#define ExFreePoolWithTag TestPoolFree
#endif
