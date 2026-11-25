/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define MALI_VM_TEST_COUNTER MtkMaliVmAssertionCount
#include "TestVm.h"
static void MappingAndAccess(void) {
    Begin();uint64_t Va=0x12345678000;Tree(0,0,Va,0x50000000);CHECK(Vm.TableCount==4 && VmPoolLive==4 && Cleans==4 && Barriers==4);
    MALI_VM_HOLD H={0};CHECK(Enter(&Adapter)==STATUS_SUCCESS);
    CHECK(MtkMaliVmHold(&Vm,&H,Owner[0],OsPhysical[0])==STATUS_SUCCESS);
    CHECK(MtkMaliVmIsTablePage(&Vm,OsPhysical[3]+123) && !MtkMaliVmIsTablePage(&Vm,0x50000000));Leave(&Adapter);
    MALI_VM_PAGE P={0};CHECK(Lookup(&H,Va+19,&P)==STATUS_SUCCESS && P.Physical==0x50000013 && P.AllocationOffset==19 && P.Allocation==AllocationHandle);
    CHECK(P.AllocationKey && (P.Flags&MaliPteWrite) && !(P.Flags&MaliPteExecute));
    CHECK(Lookup(&H,Va+4096+33,&P)==STATUS_SUCCESS && P.Physical==0x50003021 && P.AllocationOffset==4096+33 && !(P.Flags&MaliPteWrite));
    uint64_t Token=0;CHECK(Enter(&Adapter)==STATUS_SUCCESS);
    CHECK(MtkMaliAllocationAcquireObject(&Adapter.Allocations,Dev[0],(HANDLE)0x1234,P.Allocation,P.AllocationKey,P.AllocationOffset,8,FALSE,&Token)==STATUS_SUCCESS);
    CHECK(MtkMaliAllocationAcquireObject(&Adapter.Allocations,Dev[1],(HANDLE)0x1235,P.Allocation,P.AllocationKey,0,8,TRUE,&Token)==STATUS_ACCESS_DENIED);
    CHECK(MtkMaliAllocationAcquireObject(&Adapter.Allocations,Dev[0],(HANDLE)0x1234,P.Allocation,P.AllocationKey+1,0,8,FALSE,&Token)==STATUS_INVALID_HANDLE);
    MALI_ALLOCATION_VIEW View={0};CHECK(MtkMaliAllocationReferenceView(&Adapter.Allocations,Token,&View)==STATUS_SUCCESS && View.Key==P.AllocationKey && View.BackingBytes==8192);
    CHECK(MtkMaliAllocationRelease(&Adapter.Allocations,Token)==STATUS_SUCCESS);
    CHECK(MtkMaliVmForgetProcess(&Vm,Owner[0])==STATUS_DEVICE_BUSY && MtkMaliVmShutdown(&Vm)==STATUS_DEVICE_BUSY);
    CHECK(MtkMaliVmDrop(&Vm,&H)==STATUS_SUCCESS && MtkMaliVmDrop(&Vm,&H)==STATUS_INVALID_HANDLE);Leave(&Adapter);
    CHECK(Lookup(&H,Va,&P)==STATUS_INVALID_PARAMETER);
    End();
}
static void FailureAtomicity(void) {
    Begin();uint64_t Va=0x80000000;Tree(0,0,Va,0x50000000);
    uint64_t Key=((ALLOCATION*)AllocationHandle)->Key;CHECK(Key==1);
    for(unsigned Kind=0;Kind<17;Kind++) {
        Entry(0,0x55000000,1,0);Update(3,0,0,0,1,0,AllocationHandle,Va);
        switch(Kind) {
            case 0:Entries[0].CacheCoherent=1;break;case 1:Entries[0].LargePage=1;break;
            case 2:Entries[0].Segment=1;break;case 3:Entries[0].PageAddress=MALI_JM_PA_LIMIT;break;
            case 4:Build.UpdatePageTable.AllocationOffsetInBytes=8192;break;
            case 5:Build.UpdatePageTable.AllocationOffsetInBytes=1;break;
            case 6:Build.UpdatePageTable.FirstPteVirtualAddress=Va+1;break;
            case 7:Build.UpdatePageTable.FirstPteVirtualAddress=Va+4096;break;
            case 8:Build.UpdatePageTable.hAllocation=(HANDLE)1;break;
            case 9:Build.UpdatePageTable.DriverProtection=1;break;
            case 10:Build.UpdatePageTable.hProcess=(HANDLE)1;break;
            case 11:OsPhysical[3]=MALI_JM_PA_LIMIT;break;
            case 12:Build.UpdatePageTable.NumPageTableEntries=513;break;
            case 13:Build.UpdatePageTable.Flags.NativeFence=1;break;
            case 14:Build.UpdatePageTable.PageTableLevel=4;break;
            case 15:Build.UpdatePageTable.PageTableAddress.CpuVirtual=(char*)OsTables[3]+8;break;
            case 16:Build.UpdatePageTable.Flags.InitialUpdate=1;VmPoolFail=VmPoolCalls+1;break;
        }
        RtlCopyMemory(Before,OsTables[3],sizeof(Before));unsigned C=Cleans,B=Barriers,N=Vm.TableCount,L=VmPoolLive;
        CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)!=STATUS_SUCCESS);
        CHECK(!Cleans || Cleans==C);CHECK(Barriers==B && Vm.TableCount==N && VmPoolLive==L);
        for(unsigned I=0;I<512;I++)CHECK(OsTables[3][I]==Before[I]);
        OsPhysical[3]=0x40004000;VmPoolFail=0;
        MALI_VM_HOLD H={0};CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliVmHold(&Vm,&H,Owner[0],OsPhysical[0])==STATUS_SUCCESS);Leave(&Adapter);
        MALI_VM_PAGE P={0};CHECK(Lookup(&H,Va,&P)==STATUS_SUCCESS && P.Physical==0x50000000 && P.AllocationKey==Key);
        CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliVmDrop(&Vm,&H)==STATUS_SUCCESS);Leave(&Adapter);
    }
    /* A failing InitialUpdate must not discard the previous valid mirror. */
    Entry(0,0x50000000,1,0);Entries[0].Reserved=1;Update(3,0,0,0,1,1,AllocationHandle,Va);
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_INVALID_PARAMETER && VmPoolLive==4);
    /* Unique IDs are never reused or wrapped, including failed creation. */
    MALI_ALLOCATION_DESC Desc={1,40,1,0,1,0,0,0,0};DXGK_ALLOCATIONINFO I={0};I.pPrivateDriverData=&Desc;I.PrivateDriverDataSize=40;
    DXGKARG_CREATEALLOCATION A={0};A.NumAllocations=1;A.pAllocationInfo=&I;
    Adapter.Allocations.NextAllocation=UINT64_MAX;unsigned L=TestPoolLive;
    CHECK(MtkMaliDdiCreateAllocation(&Adapter,&A)==STATUS_INSUFFICIENT_RESOURCES && TestPoolLive==L && !I.hAllocation);
    Adapter.Allocations.NextAllocation=Key;End();
}
static void UnknownStaleAndReusedTables(void) {
    Begin();uint64_t Va=0x80000000;Tree(0,0,Va,0x50000000);
    MALI_VM_HOLD H={0};MALI_VM_PAGE P={0};CHECK(Enter(&Adapter)==STATUS_SUCCESS);
    CHECK(MtkMaliVmHold(&Vm,&H,Owner[1],OsPhysical[0])==STATUS_INVALID_ADDRESS);
    CHECK(MtkMaliVmHold(&Vm,&H,Owner[0],OsPhysical[3])==STATUS_INVALID_ADDRESS);
    CHECK(MtkMaliVmHold(&Vm,&H,Owner[0],OsPhysical[0])==STATUS_SUCCESS);Leave(&Adapter);
    CHECK(Lookup(&H,Va+8192,&P)==STATUS_INVALID_ADDRESS);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliVmDrop(&Vm,&H)==STATUS_SUCCESS);Leave(&Adapter);
    RtlZeroMemory(Entries,sizeof(Entries));Update(3,0,0,0,1,0,0,Va);
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliVmHold(&Vm,&H,Owner[0],OsPhysical[0])==STATUS_SUCCESS);Leave(&Adapter);
    CHECK(Lookup(&H,Va,&P)==STATUS_GRAPHICS_ALLOCATION_INVALID);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliVmDrop(&Vm,&H)==STATUS_SUCCESS);Leave(&Adapter);
    Entry(0,0x50004000,1,0);Update(3,0,0,1,1,1,AllocationHandle,Va+4096);
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliVmHold(&Vm,&H,Owner[0],OsPhysical[0])==STATUS_SUCCESS);Leave(&Adapter);
    CHECK(Lookup(&H,Va,&P)==STATUS_INVALID_ADDRESS);
    CHECK(Lookup(&H,Va+4096,&P)==STATUS_SUCCESS && P.Physical==0x50004000);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliVmDrop(&Vm,&H)==STATUS_SUCCESS);Leave(&Adapter);
    /* An OS physical table page reused by another process replaces its identity. */
    Entry(0,OsPhysical[1],0,0);Update(0,3,1,0,1,0,0,0);
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_INVALID_PARAMETER);
    Build.UpdatePageTable.Flags.InitialUpdate=1;CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS && Vm.TableCount==4);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliVmHold(&Vm,&H,Owner[0],OsPhysical[0])==STATUS_INVALID_ADDRESS);
    CHECK(MtkMaliVmHold(&Vm,&H,Owner[1],OsPhysical[0])==STATUS_SUCCESS);Leave(&Adapter);
    CHECK(Lookup(&H,Va,&P)==STATUS_INVALID_ADDRESS);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliVmDrop(&Vm,&H)==STATUS_SUCCESS);Leave(&Adapter);
    End();
}
static void HoldsEvictionAndLimits(void) {
    Begin();uint64_t Va=0x80000000;Tree(0,0,Va,0x50000000);
    MALI_VM_HOLD H[34];RtlZeroMemory(H,sizeof(H));CHECK(Enter(&Adapter)==STATUS_SUCCESS);
    for(unsigned I=0;I<33;I++)CHECK(MtkMaliVmHold(&Vm,&H[I],Owner[0],OsPhysical[0])==STATUS_SUCCESS);
    CHECK(MtkMaliVmHold(&Vm,&H[33],Owner[0],OsPhysical[0])==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(MtkMaliVmHold(&Vm,&H[0],Owner[0],OsPhysical[0])==STATUS_INVALID_PARAMETER);Leave(&Adapter);
    Entry(0,0x55000000,1,0);Update(3,0,0,0,1,0,AllocationHandle,Va);unsigned C=Cleans;
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_DEVICE_BUSY && Cleans==C);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);
    CHECK(MtkMaliVmForgetProcess(&Vm,Owner[1])==STATUS_SUCCESS && Vm.TableCount==4);
    for(unsigned I=0;I<33;I++)CHECK(MtkMaliVmDrop(&Vm,&H[I])==STATUS_SUCCESS);Leave(&Adapter);
    Update(3,0,0,0,0,0,0,0);Build.UpdatePageTable.Flags.NotifyEviction=1;Build.UpdatePageTable.pPageTableEntries=0;
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS && Cleans==C && Vm.TableCount==3);
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS && Vm.TableCount==3);
    /* Full explicit invalidation drops a table without relying on eviction. */
    RtlZeroMemory(Entries,sizeof(Entries));Update(2,1,0,0,512,0,0,0);
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS && Vm.TableCount==2);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);Vm.TableCount=MALI_VM_TABLE_LIMIT;Leave(&Adapter);
    Entry(0,0x55000000,1,0);Update(7,0,0,0,1,1,AllocationHandle,Va);
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_INSUFFICIENT_RESOURCES);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);Vm.TableCount=2;Leave(&Adapter);
    CHECK(MtkMaliDdiAttachVm(&Adapter,&Vm)==STATUS_DEVICE_BUSY);
    Irql=DISPATCH_LEVEL;CHECK(MtkMaliVmShutdown(&Vm)==STATUS_INVALID_PARAMETER);CHECK(MtkMaliDdiAttachVm(&Adapter,&Vm)==STATUS_INVALID_PARAMETER);Irql=PASSIVE_LEVEL;
    End();
}
static void SnapshotAndObjectGeneration(void) {
    Begin();uint64_t Va=0x80000000;Tree(0,0,Va,0x50000000);
    Entry(0,0x55000000,1,0);Update(3,0,0,0,1,0,AllocationHandle,Va);MutatePtes=1;
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);MutatePtes=0;
    MALI_VM_HOLD H={0};MALI_VM_PAGE P={0};CHECK(Enter(&Adapter)==STATUS_SUCCESS);
    CHECK(MtkMaliVmHold(&Vm,&H,Owner[0],OsPhysical[0])==STATUS_SUCCESS);Leave(&Adapter);
    CHECK(Lookup(&H,Va,&P)==STATUS_SUCCESS && P.Physical==0x55000000 && Entries[0].PageAddress==0x666000);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliVmDrop(&Vm,&H)==STATUS_SUCCESS);
    /* Simulate stale mirror identity at an otherwise live object address. */
    ((ALLOCATION*)AllocationHandle)->Key++;
    CHECK(MtkMaliVmHold(&Vm,&H,Owner[0],OsPhysical[0])==STATUS_SUCCESS);Leave(&Adapter);
    CHECK(Lookup(&H,Va,&P)==STATUS_INVALID_HANDLE);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);((ALLOCATION*)AllocationHandle)->Key--;
    CHECK(MtkMaliVmDrop(&Vm,&H)==STATUS_SUCCESS);Leave(&Adapter);End();
}
static void DdiQueuedWorkAndProcessCleanup(void) {
    Begin();Tree(0,0,0x80000000,0x50000000);
    DXGKARG_CREATECONTEXT C={0};C.EngineAffinity=1;C.Flags.VirtualAddressing=1;
    CHECK(MtkMaliDdiCreateContext(Dev[0],&C)==STATUS_SUCCESS);Ctx=C.hContext;
    DXGKARG_SETROOTPAGETABLE Root={0};Root.hContext=Ctx;Root.Address.SegmentOffset=OsPhysical[0];Root.NumEntries=512;
    MtkMaliDdiSetRootPageTable(&Adapter,&Root);CHECK(Adapter.LastRootStatus==STATUS_SUCCESS);
    DXGKARG_SUBMITCOMMANDVIRTUAL S={0};S.hContext=Ctx;S.DmaBufferVirtualAddress=0x80000000;S.DmaBufferSize=4096;S.SubmissionFenceId=1;
    CHECK(MtkMaliDdiSubmitCommandVirtual(&Adapter,&S)==STATUS_SUCCESS);
    unsigned N=Vm.TableCount,Clean=Cleans;
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_DEVICE_BUSY && Cleans==Clean && Vm.TableCount==N);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);Completion(&G,0,1,0);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && Released==1);
    CHECK(MtkMaliDdiDestroyContext(Ctx)==STATUS_SUCCESS);
    MALI_VM_HOLD H={0};CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliVmHold(&Vm,&H,Owner[0],OsPhysical[0])==STATUS_SUCCESS);Leave(&Adapter);
    DXGKARG_CLOSEALLOCATION Close={1,&OpenHandle};CHECK(MtkMaliDdiCloseAllocation(Dev[0],&Close)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiDestroyDevice(Dev[0])==STATUS_SUCCESS);
    CHECK(MtkMaliDdiDestroyProcess(&Adapter,Proc[0])==STATUS_DEVICE_BUSY && Vm.TableCount==4);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliVmDrop(&Vm,&H)==STATUS_SUCCESS);Leave(&Adapter);
    CHECK(MtkMaliDdiDestroyProcess(&Adapter,Proc[0])==STATUS_SUCCESS && !Vm.TableCount && !VmPoolLive);
    DXGKARG_DESTROYALLOCATION A={0};A.NumAllocations=1;A.pAllocationList=&AllocationHandle;A.hResource=ResourceHandle;A.Flags.DestroyResource=1;
    CHECK(MtkMaliDdiDestroyAllocation(&Adapter,&A)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiDestroyDevice(Dev[1])==STATUS_SUCCESS && MtkMaliDdiDestroyProcess(&Adapter,Proc[1])==STATUS_SUCCESS);
    CHECK(MtkMaliDdiShutdown(&Adapter)==STATUS_SUCCESS && MtkMaliVmShutdown(&Vm)==STATUS_SUCCESS);
    CHECK(!TestPoolLive && !VmPoolLive && !GateHeld);
}
int TestMain(void) {
    MappingAndAccess();FailureAtomicity();UnknownStaleAndReusedTables();HoldsEvictionAndLimits();SnapshotAndObjectGeneration();DdiQueuedWorkAndProcessCleanup();
    return (int)FirstFailure;
}
