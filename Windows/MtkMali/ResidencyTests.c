/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define MALI_VM_TEST_COUNTER MtkMaliResidencyAssertionCount
#include "TestResidency.h"
static void Lifecycle(void) {
    BeginResidency(1,1,0,4096);UINT Writes=G.Writes;CHECK(SubmitResidency(1)==STATUS_SUCCESS && G.Writes==Writes);
    if(FirstFailure)return;
    RESIDENCY_LEASE *L=Residency.Leases;CHECK(L && L->Sealed && L->DataCount==2 && L->TableCount==7);
    CHECK(Residency.Count==1 && Vm.HoldCount==1 && Adapter.Allocations.ReferenceCount==2);
    uint64_t Pa=0;uint32_t Flags=0;
    CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,TargetVa+7,&Pa,&Flags)==MaliOk && Pa==SharedPhysical[1]+7);
    CHECK((Flags&(MaliPteRead|MaliPteWrite))==(MaliPteRead|MaliPteWrite) && !(Flags&MaliPteExecute));
    CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,DmaVa,&Pa,&Flags)!=MaliOk);
    CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,RESIDENCY_SHADOW_VA,&Pa,&Flags)==MaliOk && !(Flags&MaliPteExecute));
    TestPut64(SharedData+48,0);CHECK(TestGet64((uint8_t*)L->Shadow.Cpu+48)==UINT64_C(0x0123456789abcdef));
    DXGKARG_CLOSEALLOCATION Close={1,&OpenHandle};CHECK(MtkMaliDdiCloseAllocation(Dev[0],&Close)==STATUS_DEVICE_BUSY);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliResidencyShutdown(&Residency)==STATUS_DEVICE_BUSY);Leave(&Adapter);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS && Core.Roots[0]==L->Tables[0].Physical);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);Ops.End(Ops.Context,L,MaliDdiCompleted);
    CHECK(Residency.LastReleaseStatus==STATUS_DEVICE_BUSY && Residency.Count==1);Leave(&Adapter);
    Completion(&G,1,1,0);TestVmSyncFailure=1;
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_UNSUCCESSFUL && !Core.BoundSpaces && Residency.Count==1 && SharedLive==2);
    TestVmSyncFailure=0;CHECK(MtkMaliDdiPump(&Adapter,3)==STATUS_SUCCESS && !Residency.Count && !SharedLive && !PrivateLive);
    CHECK(Residency.LastReleaseStatus==STATUS_SUCCESS && MdlCalls==MdlFrees && MemoryCalls==MemoryFrees);EndResidency();
}
static void FailureUnwinds(void) {
    for(UINT Case=0;Case<19;Case++) {
        BeginResidency(1,1,0,4096);
        if(Case==0)VmPoolFail=VmPoolCalls+1;
        if(Case==1)TestPoolFail=TestPoolCalls+1;
        if(Case==2)FailMdl=1;
        if(Case==3)FailMap=1;
        if(Case==4)FailPfn=1;
        if(Case>=5 && Case<=12)FailMemory=Case-4;
        if(Case>=13 && Case<=15)MemoryAlias=Case-12;
        if(Case==16)FailMdl=2;
        if(Case==17)FailMap=2;
        if(Case==18)FailPfn=2;
        UINT Writes=G.Writes;CHECK(!NT_SUCCESS(SubmitResidency(1)));
        CHECK(!Residency.Count && !PrivateLive && !SharedLive && !Vm.HoldCount && !Adapter.Allocations.ReferenceCount);
        CHECK(LockedPages==UnlockedPages && G.Writes==Writes && !Adapter.Queue.Count);
        CHECK(MemoryFrees==MemoryCalls-(FailMemory?1:0));EndResidency();
    }
}
static void MappingAndRightsFailures(void) {
    for(UINT Case=0;Case<8;Case++) {
        BeginResidency(1,1,0,4096);
        if(Case==0)SetMappings(1);
        if(Case==1){SharedPhysical[1]=SharedPhysical[0];SetMappings(0);}
        if(Case==2){SharedPhysical[0]=OsPhysical[0];SetMappings(0);}
        if(Case==3){SharedPhysical[1]=OsPhysical[3];SetMappings(0);}
        if(Case==4){SharedPhysical[1]=0x70000000;SetMappings(0);}
        if(Case==5){SharedPhysical[0]+=4096;/* Registry deliberately still has the old PFN. */}
        if(Case==6){CHECK(Enter(&Adapter)==STATUS_SUCCESS);((ALLOCATION*)AllocationHandle)->Backing=0;Leave(&Adapter);}
        if(Case==7) {
            DXGKARG_CLOSEALLOCATION C={1,&OpenHandle};CHECK(MtkMaliDdiCloseAllocation(Dev[0],&C)==STATUS_SUCCESS);
            DXGK_OPENALLOCATIONINFO I={0};I.hAllocation=10;DXGKARG_OPENALLOCATION O={0};O.NumAllocations=1;O.pOpenAllocation=&I;O.Flags.ReadOnly=1;
            CHECK(MtkMaliDdiOpenAllocation(Dev[0],&O)==STATUS_SUCCESS);OpenHandle=I.hDeviceSpecificAllocation;
        }
        UINT Writes=G.Writes;CHECK(!NT_SUCCESS(SubmitResidency(1)) && G.Writes==Writes);
        CHECK(!Residency.Count && !PrivateLive && !SharedLive && LockedPages==UnlockedPages);EndResidency();
    }
}
static void FaultAndResetRetention(void) {
    BeginResidency(1,1,0,4096);CHECK(SubmitResidency(1)==STATUS_SUCCESS && MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);
    Completion(&G,1,0x42,1);CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE);
    CHECK(Residency.Count==1 && SharedLive==2 && PrivateLive && Vm.HoldCount==1);
    DXGKARG_RESETENGINE R={0};G.ResetStuck=1;CHECK(MtkMaliDdiResetEngine(&Adapter,&R)==STATUS_IO_TIMEOUT && Residency.Count==1);
    G.ResetStuck=0;CHECK(MtkMaliDdiResetEngine(&Adapter,&R)==STATUS_SUCCESS && !Residency.Count);EndResidency();
    BeginResidency(1,1,0,4096);CHECK(SubmitResidency(2)==STATUS_SUCCESS && MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);
    Completion(&G,1,1,0);G.AsStuck=1;
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_IO_TIMEOUT && Residency.Count==1 && SharedLive==2 && Core.BoundSpaces);
    G.AsStuck=0;CHECK(MtkMaliDdiResetEngine(&Adapter,&R)==STATUS_SUCCESS && R.LastAbortedFenceId==2);EndResidency();
}
static void FullQueueAndBoundary(void) {
    BeginResidency(1,1,0,4096);
    for(UINT I=1;I<=32;I++)CHECK(SubmitResidency(I)==STATUS_SUCCESS);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);CHECK(SubmitResidency(33)==STATUS_SUCCESS && Residency.Count==33 && Vm.HoldCount==33);
    CHECK(SubmitResidency(34)==STATUS_DEVICE_BUSY);
    DXGKARG_PREEMPTCOMMAND P={0};P.PreemptionFenceId=100;Irql=DISPATCH_LEVEL;
    CHECK(MtkMaliDdiPreemptCommand(&Adapter,&P)==STATUS_SUCCESS);Irql=PASSIVE_LEVEL;
    TestVmFailSyncAt=TestVmSyncCalls+2;Completion(&G,1,1,0);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_UNSUCCESSFUL && Residency.Count==32 && SharedLive==64);
    CHECK(MtkMaliDdiPump(&Adapter,3)==STATUS_SUCCESS && !Residency.Count);EndResidency();
    BeginResidency(17,64,128,65536);CHECK(SubmitResidency(1)==STATUS_SUCCESS);
    RESIDENCY_LEASE *L=Residency.Leases;CHECK(L && L->DataCount==MALI_RESIDENCY_DATA_LIMIT && L->Sealed);
    for(UINT I=0;I<64;I++) {
        uint64_t Pa=0;uint32_t F=0;
        CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,TargetVa+(uint64_t)I*4096,&Pa,&F)==MaliOk && Pa==SharedPhysical[17+I]);
    }
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);Completion(&G,1,1,0);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && !Residency.Count);EndResidency();
}
static void ScatteredAddressTables(void) {
    BeginResidency(2,64,0,8192);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliVmForgetProcess(&Vm,Owner[0])==STATUS_SUCCESS);Leave(&Adapter);
    RtlZeroMemory(OsTables,sizeof(OsTables));
    UINT Next=1;
    for(UINT I=0;I<=64;I++) {
        uint64_t Va=I?((uint64_t)I<<39)+0x4000:DmaVa;UINT Table=0;
        for(UINT L=0;L<3;L++) {
            UINT Child=Next++;Entry(0,OsPhysical[Child],0,0);
            Update(Table,3-L,0,(UINT)((Va>>(39-L*9))&511u),1,Table?1:!I,0,0);
            CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);Table=Child;
        }
        UINT Page=I?I+1u:0;Entry(0,SharedPhysical[Page],1,0);if(!I)Entry(1,SharedPhysical[1],1,0);
        Update(Table,0,0,(UINT)((Va>>12)&511u),I?1:2,1,AllocationHandle,Va);
        Build.UpdatePageTable.AllocationOffsetInBytes=(uint64_t)Page*4096;
        CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);
        if(I)TestPut64(SharedData+(I-1u)*128+32,Va);
    }
    CHECK(Next==196 && Vm.TableCount==196 && SubmitResidency(1)==STATUS_SUCCESS);
    RESIDENCY_LEASE *L=Residency.Leases;if(!L){CHECK(0);return;}
    CHECK(L->TableCount==196 && L->DataCount==66);
    for(UINT I=1;I<=64;I++) {
        uint64_t Pa=0;uint32_t Flags=0;
        CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,((uint64_t)I<<39)+0x4000,&Pa,&Flags)==MaliOk && Pa==SharedPhysical[I+1u]);
    }
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);Completion(&G,1,1,0);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && !Residency.Count);
    CHECK(!PrivateLive && !SharedLive && LockedPages==UnlockedPages && VmPoolLive==196);
    CHECK(MtkMaliDdiDestroyContext(Ctx)==STATUS_SUCCESS);
    CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliResidencyShutdown(&Residency)==STATUS_SUCCESS);Leave(&Adapter);
    CHECK(MtkMaliPrepareShutdown(&TestPrepareService)==STATUS_SUCCESS);End();
}
static void ComputeMappings(void) {
    uint64_t Base=DmaVa;
    for(UINT I=0;I<SharedPages;I++)Entry(I,SharedPhysical[I],1,0);
    Entries[1].ReadOnly=1;Entries[2].ReadOnly=1;Entries[2].NoExecute=0;
    Update(3,0,0,(UINT)((Base>>12)&511u),SharedPages,1,AllocationHandle,Base);
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);
}
static void BeginCompute(UINT DataPages) {
    BeginResidency(1,3+DataPages,0,4096);Core.Caps.JobFeatures[1]|=16;G.Reg[0xc4/4]|=16;
    ComputeMappings();RtlZeroMemory(SharedData,4096);
    TestPut32(SharedData+16,(71u<<16)|8u);TestPut32(SharedData+32,63);TestPut32(SharedData+36,1u|(2u<<14));
    TestPut32(SharedData+40,2);TestPut32(SharedData+44,1);TestPut32(SharedData+48,1);
    TestPut32(SharedData+68,1);TestPut64(SharedData+96,0x80001084);
    TestPut64(SharedData+104,0x80001000);TestPut64(SharedData+112,0x80001040);TestPut64(SharedData+120,0x800010c0);
    /* Descriptor/code bytes are untrusted. These fixture values do not execute
       a shader: actual command, MDL, MMU and scheduler code runs around them. */
    TestPut64(SharedData+4096+8,0x80002000);
    Private.Version=2;Private.Bytes=MALI_COMMAND_PRIVATE_BYTES(1,3);Private.Flags=3;
    uint8_t *P=(uint8_t*)&Private+32;
    TestPut64(P,0x80001000);TestPut32(P+8,4096);TestPut32(P+12,MaliPteRead);
    TestPut64(P+16,0x80002000);TestPut32(P+24,4096);TestPut32(P+28,MaliPteRead|MaliPteExecute);
    TestPut64(P+32,0x80003000);TestPut32(P+40,DataPages*4096);TestPut32(P+44,MaliPteRead|MaliPteWrite);
}
static void ComputeLifecycle(void) {
    for(UINT Count=1;Count<=64;Count*=2) {
        BeginCompute(Count);CHECK(SubmitResidency(1)==STATUS_SUCCESS);RESIDENCY_LEASE *L=Residency.Leases;
        if(!L){CHECK(0);return;}
        CHECK(L->Sealed && L->DataCount==1 && L->BufferCount==3 && L->ResourcePages==Count+2 &&
            Adapter.Allocations.ReferenceCount==4 && SharedLive==4 && PrivateLive==8);
        UINT MdlIds[3];
        for(UINT I=0;I<3;I++) {
            MdlIds[I]=SharedIndex(L->Buffers[I].Mdl);CHECK(SharedMdls[MdlIds[I]].Cleans==1);
            CHECK(SharedMdls[MdlIds[I]].Invalidates==(I==2));
        }
        CHECK(L->Buffers[2].Mdl->ByteCount==Count*4096 && L->Buffers[2].Bytes==Count*4096);
        CHECK(L->HashCapacity>=(Count+2)*2 && ResourceAccess(L,SharedPhysical[2])==(MaliPteRead|MaliPteExecute));
        uint64_t Pa=0;uint32_t F=0;
        CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,0x80001000,&Pa,&F)==MaliOk &&
            Pa==SharedPhysical[1] && (F&(MaliPteRead|MaliPteWrite|MaliPteExecute))==MaliPteRead);
        CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,0x80002000,&Pa,&F)==MaliOk &&
            Pa==SharedPhysical[2] && (F&(MaliPteRead|MaliPteWrite|MaliPteExecute))==(MaliPteRead|MaliPteExecute));
        for(UINT I=0;I<Count;I++)CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,
            0x80003000+(uint64_t)I*4096+7,&Pa,&F)==MaliOk && Pa==SharedPhysical[3+I]+7 &&
            (F&(MaliPteRead|MaliPteWrite|MaliPteExecute))==(MaliPteRead|MaliPteWrite));
        CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,DmaVa,&Pa,&F)!=MaliOk);
        CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,0x80003000+(uint64_t)Count*4096,&Pa,&F)!=MaliOk);
        CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,0x13000000,&Pa,&F)!=MaliOk);
        CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,L->Tables[0].Physical,&Pa,&F)!=MaliOk);
        TestPut64(SharedData+104,UINT64_MAX);CHECK(TestGet64((uint8_t*)L->Shadow.Cpu+104)==0x80001000);
        CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS && Core.Roots[0]==L->Tables[0].Physical);
        Completion(&G,1,1,0);CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && !Residency.Count);
        for(UINT I=0;I<3;I++)CHECK(!SharedMdls[MdlIds[I]].Live && SharedMdls[MdlIds[I]].Cleans==1 &&
            SharedMdls[MdlIds[I]].Invalidates==(I==2?2u:0u));
        EndResidency();
    }
    BeginCompute(2);TestPut64(SharedData+24,DmaVa+128);
    uint8_t *P=SharedData+128;TestPut32(P+16,(72u<<16)|4u);TestPut32(P+20,71);
    TestPut64(P+32,0x80003000);TestPut32(P+40,7);TestPut64(P+48,42);
    CHECK(SubmitResidency(1)==STATUS_SUCCESS && Residency.Leases->BufferCount==3 && Residency.Leases->DataCount==1 && SharedLive==4);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);Completion(&G,1,1,0);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS);EndResidency();
}
static void LargeComputeResourceWire(void) {
    static __declspec(align(8)) uint8_t Wire[MALI_COMMAND_PRIVATE_BYTES(1,20)];
    BeginCompute(18);RtlCopyMemory(Wire,&Private,64);TestPut32(Wire+4,sizeof(Wire));TestPut32(Wire+12,20);
    for(UINT I=0;I<18;I++) {
        uint8_t *P=Wire+64+I*16;TestPut64(P,UINT64_C(0x80003000)+(uint64_t)I*4096);
        TestPut32(P+8,4096);TestPut32(P+12,MaliPteRead|MaliPteWrite);
    }
    CHECK(sizeof(Wire)>sizeof(MALI_EXEC_PACKET));
    DXGKARG_CREATECONTEXT C={0};C.EngineAffinity=1;C.Flags.VirtualAddressing=1;
    CHECK(MtkMaliDdiCreateContext(Dev[0],&C)==STATUS_SUCCESS && C.ContextInfo.DmaBufferPrivateDataSize==MALI_COMMAND_MAX_PRIVATE);
    CHECK(MtkMaliDdiDestroyContext(C.hContext)==STATUS_SUCCESS);
    DXGKARG_SUBMITCOMMANDVIRTUAL A={0};A.hContext=Ctx;A.DmaBufferVirtualAddress=DmaVa;A.DmaBufferSize=DmaLength;
    A.pDmaBufferPrivateData=Wire;A.DmaBufferPrivateDataSize=sizeof(Wire);A.SubmissionFenceId=1;
    CHECK(MtkMaliDdiSubmitCommandVirtual(&Adapter,&A)==STATUS_SUCCESS);
    CHECK(Residency.Count==1 && Residency.Leases->BufferCount==20 && Adapter.Allocations.ReferenceCount==21);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);Completion(&G,1,1,0);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && !Residency.Count);
    CHECK(MdlCalls==MdlFrees && LockedPages==UnlockedPages && !Adapter.Allocations.ReferenceCount);EndResidency();
}
static void ComputeMapPage(uint64_t Va,UINT Page,UINT *Next) {
    UINT Table=0;
    for(UINT Level=0;Level<3;Level++) {
        UINT Index=(UINT)((Va>>(39-Level*9))&511u);uint64_t Existing=OsTables[Table][Index];
        if(Existing) {
            UINT Child=0;for(UINT I=1;I<256;I++)if(OsPhysical[I]==(Existing&RESIDENCY_PA_MASK)){Child=I;break;}
            CHECK(Child);Table=Child;
        } else {
            UINT Child=(*Next)++;CHECK(Child<256);Entry(0,OsPhysical[Child],0,0);
            Update(Table,3-Level,0,Index,1,!VmFind(&Vm,OsPhysical[Table]),0,0);
            CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);Table=Child;
        }
    }
    Entry(0,SharedPhysical[Page],1,0);Update(Table,0,0,(UINT)((Va>>12)&511u),1,!VmFind(&Vm,OsPhysical[Table]),AllocationHandle,Va);
    Build.UpdatePageTable.AllocationOffsetInBytes=(uint64_t)Page*4096;
    CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);
}
static void ComputeBoundaryRanges(void) {
    for(UINT Bit=21;Bit<=39;Bit+=9) {
        BeginCompute(2);uint64_t Va=(UINT64_C(1)<<Bit)-4096;UINT Next=4;
        ComputeMapPage(Va,3,&Next);ComputeMapPage(Va+4096,4,&Next);
        TestPut64((uint8_t*)&Private+64,Va);CHECK(SubmitResidency(1)==STATUS_SUCCESS);
        RESIDENCY_LEASE *L=Residency.Leases;if(!L){CHECK(0);return;}
        uint64_t Pa=0;uint32_t F=0;
        for(UINT I=0;I<2;I++)CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,Va+(uint64_t)I*4096,
            &Pa,&F)==MaliOk && Pa==SharedPhysical[3+I] && (F&MaliPteWrite) && !(F&MaliPteExecute));
        CHECK(L->BufferCount==3 && L->ResourcePages==4 && L->Tables[0].Physical!=OsPhysical[0]);
        CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);Completion(&G,1,1,0);
        CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && !Residency.Count);
        CHECK(!PrivateLive && !SharedLive && LockedPages==UnlockedPages && VmPoolLive==Next);
        CHECK(MtkMaliDdiDestroyContext(Ctx)==STATUS_SUCCESS);
        CHECK(Enter(&Adapter)==STATUS_SUCCESS);CHECK(MtkMaliResidencyShutdown(&Residency)==STATUS_SUCCESS);Leave(&Adapter);
        CHECK(MtkMaliPrepareShutdown(&TestPrepareService)==STATUS_SUCCESS);End();
    }
}
static void ComputeRejects(void) {
    for(UINT Case=0;Case<24;Case++) {
        BeginCompute(2);
        if(Case<3)FailMdl=Case+2;
        if(Case>=3 && Case<6)FailMap=Case-1;
        if(Case>=6 && Case<9)FailPfn=Case-4;
        if(Case==9){FailPfn=4;FailPfnPage=1;}
        if(Case==10) {
            Entry(0,SharedPhysical[2],1,1);Update(3,0,0,2,1,0,AllocationHandle,0x80002000);
            Build.UpdatePageTable.AllocationOffsetInBytes=8192;CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);
        }
        if(Case==11) {
            Entry(0,SharedPhysical[4],1,1);Update(3,0,0,4,1,0,AllocationHandle,0x80004000);
            Build.UpdatePageTable.AllocationOffsetInBytes=16384;CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);
        }
        if(Case==12){SharedPhysical[1]=SharedPhysical[0];ComputeMappings();}
        if(Case==13){SharedPhysical[3]=OsPhysical[3];ComputeMappings();}
        if(Case==14){SharedPhysical[3]=0x70000000;ComputeMappings();}
        if(Case==15){SharedPhysical[3]=SharedPhysical[2];ComputeMappings();}
        if(Case==16){SharedPhysical[4]=SharedPhysical[2];ComputeMappings();}
        if(Case==17) {
            Entry(0,SharedPhysical[4],1,0);Update(3,0,0,4,1,0,AllocationHandle,0x80004000);
            Build.UpdatePageTable.AllocationOffsetInBytes=20480;CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);
        }
        if(Case==18) {
            DXGKARG_CLOSEALLOCATION C={1,&OpenHandle};CHECK(MtkMaliDdiCloseAllocation(Dev[0],&C)==STATUS_SUCCESS);
            DXGK_OPENALLOCATIONINFO I={0};I.hAllocation=10;DXGKARG_OPENALLOCATION O={0};O.NumAllocations=1;O.pOpenAllocation=&I;O.Flags.ReadOnly=1;
            CHECK(MtkMaliDdiOpenAllocation(Dev[0],&O)==STATUS_SUCCESS);OpenHandle=I.hDeviceSpecificAllocation;
        }
        if(Case==19)TestPut32((uint8_t*)&Private+72,9*4096);
        if(Case==20)VmPoolFail=VmPoolCalls+2;
        if(Case==21)MemoryAlias=4;
        if(Case>=22)FailMemory=Case-16;
        UINT Writes=G.Writes;CHECK(!NT_SUCCESS(SubmitResidency(1)) && G.Writes==Writes && !Adapter.Queue.Count);
        CHECK(!Residency.Count && !PrivateLive && !SharedLive && LockedPages==UnlockedPages);EndResidency();
        if(FirstFailure)return;
    }
}
static void ComputeCrossLeaseAndFault(void) {
    BeginCompute(2);SharedPhysical[5]=SharedPhysical[2];ComputeMappings();CHECK(SubmitResidency(1)==STATUS_SUCCESS);
    RESIDENCY_LEASE *First=Residency.Leases;CHECK(First && First->BufferCount==3);
    TestPut64((uint8_t*)&Private+64,0x80005000);TestPut32((uint8_t*)&Private+72,4096);
    CHECK(SubmitResidency(2)==STATUS_ACCESS_DENIED && Residency.Count==1 && Residency.Leases==First && SharedLive==4);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);Completion(&G,1,0x42,1);
    CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE && Residency.Count==1 && SharedLive==4);
    DXGKARG_RESETENGINE R={0};G.ResetStuck=1;
    CHECK(MtkMaliDdiResetEngine(&Adapter,&R)==STATUS_IO_TIMEOUT && Residency.Count==1 && SharedLive==4);
    G.ResetStuck=0;CHECK(MtkMaliDdiResetEngine(&Adapter,&R)==STATUS_SUCCESS && !Residency.Count);EndResidency();
    BeginCompute(2);CHECK(Enter(&Adapter)==STATUS_SUCCESS);
    RESIDENCY_LEASE *L=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*L),RESIDENCY_TAG);CHECK(L!=0);
    CHECK(GrowResourceHash(L,32)==STATUS_SUCCESS && L->HashCapacity==64);
    for(UINT I=0;I<32;I++)*HashSlot(L->ResourceHash,L->HashCapacity,0x40000000+(uint64_t)I*64*4096)=
        0x40000000+(uint64_t)I*64*4096+MaliPteRead;
    CHECK(GrowResourceHash(L,MALI_COMMAND_MAX_RESOURCE_PAGES)==STATUS_SUCCESS && L->HashCapacity==262144);
    for(UINT I=0;I<32;I++)CHECK(ResourceAccess(L,0x40000000+(uint64_t)I*64*4096)==MaliPteRead);
    ExFreePoolWithTag(L->ResourceHash,RESIDENCY_TAG);ExFreePoolWithTag(L,RESIDENCY_TAG);Leave(&Adapter);EndResidency();
}
static void RasterResidencyRecord(UINT Offset,UINT Type,UINT Index,uint64_t Next) {
    uint8_t *P=SharedData+Offset;UINT Bytes=Type==11?384:Type==7?256:128;
    RtlZeroMemory(P,Bytes);TestPut32(P+16,(Index<<16)|(Type<<1));TestPut64(P+24,Next);
    TestPut32(P+32,8u);TestPut32(P+44,3);TestPut64(P+56,0x80003000);
    TestPut32(P+108,127u|(127u<<16));
    if(Type==12){TestPut64(P+48,0x80001100);return;}
    TestPut32(P+48,1);TestPut32(P+52,Type==11?16:3);TestPut32(P+112,0x3f800000);
    TestPut32(P+132,0x1ffff);TestPut64(P+136,Type==11?1:0x80001201);
    TestPut32(P+144,Type==11?0:16);TestPut32(P+156,0x3f800000);
    for(UINT Offset=192;Offset<Bytes && Offset<320;Offset+=64) {
        TestPut64(P+Offset+40,0x80001000);TestPut64(P+Offset+48,0x80001040);
    }
}
static void BeginRasterResidency(void) {
    BeginCompute(2);RtlZeroMemory(SharedData,4096);
    Core.Caps.JobFeatures[1]|=(1u<<7)|(1u<<9)|(1u<<11)|(1u<<12);
    G.Reg[0xc4/4]|=(1u<<7)|(1u<<9)|(1u<<11)|(1u<<12);
    RasterResidencyRecord(0,11,71,DmaVa+384);RasterResidencyRecord(384,7,72,DmaVa+640);
    RasterResidencyRecord(640,12,73,DmaVa+768);
    uint8_t *P=SharedData+768;TestPut32(P+16,(74u<<16)|(9u<<1));TestPut64(P+40,0x80001001);
}
static void RasterResidencyLifecycle(void) {
    BeginRasterResidency();UINT Writes=G.Writes;
    CHECK(SubmitResidency(1)==STATUS_SUCCESS && G.Writes==Writes);
    RESIDENCY_LEASE *L=Residency.Leases;if(!L){CHECK(0);return;}
    CHECK(L->Sealed && L->Shadow.Bytes==MALI_COMMAND_SHADOW_BYTES && L->BufferCount==3 && L->ResourcePages==4);
    uint8_t *S=L->Shadow.Cpu;
    CHECK(TestGet64(S+24)==RESIDENCY_SHADOW_VA+384 && TestGet64(S+384+24)==RESIDENCY_SHADOW_VA+640 &&
        TestGet64(S+640+24)==RESIDENCY_SHADOW_VA+768 && !TestGet64(S+768+24));
    CHECK(TestGet64(S+296)==0x80001000 && TestGet64(S+304)==0x80001040);
    uint64_t Pa=0;uint32_t Flags=0;
    for(UINT I=0;I<MALI_COMMAND_SHADOW_BYTES/4096u;I++) {
        uint64_t Expected=0;CHECK(MtkMaliMemoryPhysical(&L->Shadow,I,&Expected)==MaliOk);
        CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,RESIDENCY_SHADOW_VA+I*4096+7,&Pa,&Flags)==MaliOk &&
            Pa==Expected+7 && (Flags&(MaliPteRead|MaliPteWrite|MaliPteExecute))==(MaliPteRead|MaliPteWrite));
    }
    CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,0x80002000,&Pa,&Flags)==MaliOk &&
        (Flags&(MaliPteRead|MaliPteWrite|MaliPteExecute))==(MaliPteRead|MaliPteExecute));
    CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,0x80003000,&Pa,&Flags)==MaliOk &&
        (Flags&(MaliPteRead|MaliPteWrite|MaliPteExecute))==(MaliPteRead|MaliPteWrite));
    CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,DmaVa,&Pa,&Flags)!=MaliOk);
    RtlZeroMemory(SharedData,4096);CHECK(TestGet64(S+296)==0x80001000 && TestGet64(S+640+48)==0x80001100);
    CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS && Core.Roots[0]==L->Tables[0].Physical);
    Completion(&G,1,1,0);CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && !Residency.Count);
    CHECK(!PrivateLive && !SharedLive && LockedPages==UnlockedPages);EndResidency();
    BeginRasterResidency();CHECK(SubmitResidency(1)==STATUS_SUCCESS && MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);
    Completion(&G,1,0x42,1);CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE && Residency.Count==1 && SharedLive==4);
    DXGKARG_RESETENGINE R={0};G.ResetStuck=1;
    CHECK(MtkMaliDdiResetEngine(&Adapter,&R)==STATUS_IO_TIMEOUT && Residency.Count==1 && SharedLive==4);
    G.ResetStuck=0;CHECK(MtkMaliDdiResetEngine(&Adapter,&R)==STATUS_SUCCESS && !Residency.Count);EndResidency();
}
static void BeginFragment(UINT Targets,UINT Zs) {
    BeginCompute(2);RtlZeroMemory(SharedData,128);
    Core.Caps.JobFeatures[1]|=1u<<9;G.Reg[0xc4/4]|=1u<<9;
    TestPut32(SharedData+16,(71u<<16)|18u);TestPut32(SharedData+32,1u|(2u<<16));
    TestPut32(SharedData+36,9u|(10u<<16));
    TestPut64(SharedData+40,UINT64_C(0x80001001)+((Targets-1u)<<2)+(Zs<<1));
}
static void FragmentLifecycle(void) {
    for(UINT Targets=1;Targets<=8;Targets++)for(UINT Zs=0;Zs<2;Zs++) {
        BeginFragment(Targets,Zs);uint64_t Fbd=TestGet64(SharedData+40);UINT Writes=G.Writes;
        CHECK(SubmitResidency(1)==STATUS_SUCCESS && G.Writes==Writes);
        RESIDENCY_LEASE *L=Residency.Leases;if(!L){CHECK(0);return;}
        CHECK(L->Sealed && L->BufferCount==3 && L->ResourcePages==4 && SharedLive==4 && Adapter.Allocations.ReferenceCount==4);
        CHECK(TestGet64((uint8_t*)L->Shadow.Cpu+40)==Fbd);
        uint64_t Pa=0;uint32_t Access=0;
        CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,Fbd&~UINT64_C(63),&Pa,&Access)==MaliOk &&
            Pa==SharedPhysical[1] && !(Access&(MaliPteWrite|MaliPteExecute)));
        CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,0x80003000,&Pa,&Access)==MaliOk &&
            Pa==SharedPhysical[3] && (Access&MaliPteWrite) && !(Access&MaliPteExecute));
        CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,0x13000000,&Pa,&Access)!=MaliOk);
        CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,L->Tables[0].Physical,&Pa,&Access)!=MaliOk);
        TestPut64(SharedData+40,UINT64_MAX);CHECK(TestGet64((uint8_t*)L->Shadow.Cpu+40)==Fbd);
        UINT Mdl=SharedIndex(L->Buffers[2].Mdl);
        CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS && Core.Roots[0]==L->Tables[0].Physical);
        Completion(&G,1,1,0);CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS && !Residency.Count);
        CHECK(!SharedMdls[Mdl].Live && SharedMdls[Mdl].Cleans==1 && SharedMdls[Mdl].Invalidates==2);
        CHECK(!PrivateLive && !SharedLive && LockedPages==UnlockedPages && !Adapter.Allocations.ReferenceCount);EndResidency();
    }
    for(UINT Case=0;Case<9;Case++) {
        BeginFragment(8,1);
        if(Case==0)TestPut64(SharedData+40,0x13000003);
        if(Case==1)TestPut64(SharedData+40,0x80001f43);
        if(Case==2)TestPut32(SharedData+36,9u|(10u<<16)|(1u<<31));
        if(Case==3)Core.Caps.JobFeatures[1]&=~(1u<<9);
        if(Case==4){SharedPhysical[1]=SharedPhysical[0];ComputeMappings();}
        if(Case==5){SharedPhysical[3]=OsPhysical[3];ComputeMappings();}
        if(Case==6){SharedPhysical[3]=SharedPhysical[2];ComputeMappings();}
        if(Case==7)FailMdl=2;
        if(Case==8)FailPfn=4;
        UINT Writes=G.Writes;CHECK(!NT_SUCCESS(SubmitResidency(1)) && G.Writes==Writes && !Adapter.Queue.Count);
        CHECK(!Residency.Count && !PrivateLive && !SharedLive && LockedPages==UnlockedPages);EndResidency();
        if(FirstFailure)return;
    }
    BeginFragment(1,0);CHECK(SubmitResidency(1)==STATUS_SUCCESS && MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);
    Completion(&G,1,0x42,1);CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE && Residency.Count==1 && SharedLive==4);
    DXGKARG_RESETENGINE R={0};G.ResetStuck=1;
    CHECK(MtkMaliDdiResetEngine(&Adapter,&R)==STATUS_IO_TIMEOUT && Residency.Count==1 && SharedLive==4);
    G.ResetStuck=0;CHECK(MtkMaliDdiResetEngine(&Adapter,&R)==STATUS_SUCCESS && !Residency.Count);EndResidency();
}
static void ClearLifecycle(void) {
    const UINT Formats[]={MaliClearBgra8,MaliClearBgrx8,MaliClearRgba8,MaliClearRgbx8};
    for(UINT F=0;F<4;F++)for(UINT Fault=0;Fault<2;Fault++) {
        BeginResidency(1,66,0,128);
        Core.Caps.JobFeatures[1]|=1u<<9;G.Reg[0xc4/4]|=1u<<9;Core.Caps.Tilers=1;
        MALI_ALLOCATION_DESC Desc={1,sizeof(Desc),MALI_ALLOCATION_LINEAR_2D,3,4096,17,19,128,Formats[F]};
        DXGK_ALLOCATIONINFO Info={0};Info.pPrivateDriverData=&Desc;Info.PrivateDriverDataSize=sizeof(Desc);
        DXGKARG_CREATEALLOCATION Create={0};Create.Flags.Resource=1;Create.NumAllocations=1;Create.pAllocationInfo=&Info;
        CHECK(MtkMaliDdiCreateAllocation(&Adapter,&Create)==STATUS_SUCCESS && Info.Size==4096);
        TestVmAllocation2=Info.hAllocation;
        DXGK_OPENALLOCATIONINFO OI={0};OI.hAllocation=11;
        DXGKARG_OPENALLOCATION Open={0};Open.NumAllocations=1;Open.pOpenAllocation=&OI;
        CHECK(MtkMaliDdiOpenAllocation(Dev[0],&Open)==STATUS_SUCCESS);TestVmOpen2=OI.hDeviceSpecificAllocation;
        DXGKARG_SETALLOCATIONBACKINGSTORE Backing={TestVmAllocation2,SharedData+8192};
        CHECK(MtkMaliDdiSetAllocationBackingStore(&Adapter,&Backing)==STATUS_SUCCESS);
        Entry(0,SharedPhysical[2],1,0);Update(3,0,0,2,1,0,TestVmAllocation2,0x80002000);
        CHECK(MtkMaliDdiBuildPagingBuffer(&Adapter,&Build)==STATUS_SUCCESS);
        MALI_COMMAND_CLEAR Clear={0};Clear.Bytes=sizeof(Clear);Clear.Width=17;Clear.Height=19;Clear.Pitch=128;
        Clear.Format=Formats[F];Clear.Slot=1;Clear.TargetVa=0x80002000;Clear.TargetBytes=4096;Clear.HeapVa=0x80003000;
        Clear.Color[0]=0x3f800000;Clear.Color[1]=0x3f000000;Clear.Color[2]=0x3e800000;Clear.Color[3]=0x3f800000;
        RtlZeroMemory(SharedData+3*4096,MALI_COMMAND_CLEAR_HEAP_BYTES);
        CHECK(MaliCommandsBuildClear(&Core.Caps,&Clear,SharedData,DmaVa,128,SharedData+4096,0x80001000,4096,
            &Private,MALI_COMMAND_CLEAR_PRIVATE_BYTES)==MaliOk);
        UINT Writes=G.Writes;CHECK(SubmitResidency(1)==STATUS_SUCCESS && G.Writes==Writes);
        RESIDENCY_LEASE *L=Residency.Leases;if(!L){CHECK(0);return;}
        CHECK(L->Sealed && L->BufferCount==3 && L->ResourcePages==66 && Adapter.Allocations.ReferenceCount==4);
        for(UINT I=0;I<66;I++) {
            uint64_t Pa=0;uint32_t Flags=0;
            CHECK(MaliJmTranslate(L,ResolvePrivate,L->Tables[0].Physical,0x80001000+(uint64_t)I*4096,&Pa,&Flags)==MaliOk);
            CHECK(Pa==SharedPhysical[I+1] && (Flags&(MaliPteRead|MaliPteWrite))==(MaliPteRead|MaliPteWrite) && !(Flags&MaliPteExecute));
        }
        UINT Mdls[3];for(UINT I=0;I<3;I++)Mdls[I]=SharedIndex(L->Buffers[I].Mdl);
        DXGKARG_CLOSEALLOCATION Close={1,&TestVmOpen2};
        CHECK(MtkMaliDdiCloseAllocation(Dev[0],&Close)==STATUS_DEVICE_BUSY);
        CHECK(MtkMaliDdiPump(&Adapter,1)==STATUS_SUCCESS);
        Completion(&G,1,Fault?0x42:1,Fault);
        if(Fault) {
            CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE && Residency.Count==1 && SharedLive==4);
            DXGKARG_RESETENGINE Reset={0};G.ResetStuck=1;
            CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_IO_TIMEOUT && Residency.Count==1 && SharedLive==4);
            CHECK(MtkMaliDdiCloseAllocation(Dev[0],&Close)==STATUS_DEVICE_BUSY);
            G.ResetStuck=0;CHECK(MtkMaliDdiResetEngine(&Adapter,&Reset)==STATUS_SUCCESS);
        } else CHECK(MtkMaliDdiPump(&Adapter,2)==STATUS_SUCCESS);
        CHECK(!Residency.Count && !PrivateLive && !SharedLive && !Adapter.Allocations.ReferenceCount);
        for(UINT I=0;I<3;I++)CHECK(!SharedMdls[Mdls[I]].Live && SharedMdls[Mdls[I]].Cleans==1 &&
            SharedMdls[Mdls[I]].Invalidates==2);
        CHECK(MtkMaliDdiCloseAllocation(Dev[0],&Close)==STATUS_SUCCESS);
        DXGKARG_DESTROYALLOCATION Destroy={0};Destroy.NumAllocations=1;Destroy.pAllocationList=&TestVmAllocation2;
        Destroy.hResource=Create.hResource;Destroy.Flags.DestroyResource=1;
        CHECK(MtkMaliDdiDestroyAllocation(&Adapter,&Destroy)==STATUS_SUCCESS);
        TestVmAllocation2=TestVmOpen2=0;EndResidency();
    }
}
int TestMain(void) {
    Lifecycle();if(FirstFailure)return (int)FirstFailure;
    FailureUnwinds();if(FirstFailure)return (int)FirstFailure;
    MappingAndRightsFailures();if(FirstFailure)return (int)FirstFailure;
    FaultAndResetRetention();if(FirstFailure)return (int)FirstFailure;
    FullQueueAndBoundary();if(FirstFailure)return (int)FirstFailure;
    ScatteredAddressTables();if(FirstFailure)return (int)FirstFailure;
    ComputeLifecycle();if(FirstFailure)return (int)FirstFailure;
    LargeComputeResourceWire();if(FirstFailure)return (int)FirstFailure;
    ComputeBoundaryRanges();if(FirstFailure)return (int)FirstFailure;
    ComputeRejects();if(FirstFailure)return (int)FirstFailure;
    ComputeCrossLeaseAndFault();if(FirstFailure)return (int)FirstFailure;
    RasterResidencyLifecycle();if(FirstFailure)return (int)FirstFailure;
    FragmentLifecycle();if(FirstFailure)return (int)FirstFailure;
    ClearLifecycle();
    return (int)FirstFailure;
}
