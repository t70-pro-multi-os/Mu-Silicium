/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Soc.h"
#include "PmicPrm.h"
#define SOC_SIGNATURE 0x534f4330u
static const uint64_t SocBases[MALI_SOC_REGIONS]={0x10006000,0x10001000,0x13fbf000,0x10000000,0x1000c000};
static NTSTATUS SocExact(NTSTATUS S) {return S!=STATUS_SUCCESS && NT_SUCCESS(S)?STATUS_UNSUCCESSFUL:S;}
static int SocRange(const void *P,size_t N) {return P && N && (uintptr_t)P<=UINTPTR_MAX-N;}
static int SocOverlap(const void *A,size_t An,const void *B,size_t Bn) {
    return (uintptr_t)A<(uintptr_t)B+Bn && (uintptr_t)B<(uintptr_t)A+An;
}
static NTSTATUS SocLock(MALI_SOC_PROVIDER *S) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !SocRange(S,sizeof(*S)) || ((uintptr_t)S&7u) || S->Signature!=SOC_SIGNATURE)
        return STATUS_INVALID_PARAMETER;
    return SocExact(KeWaitForSingleObject(&S->Mutex,Executive,KernelMode,FALSE,0));
}
static NTSTATUS SocUnlock(MALI_SOC_PROVIDER *S,NTSTATUS Result) {
    KeReleaseMutex(&S->Mutex,FALSE);return SocExact(Result);
}
/* From MtkConnPower.c. The all-ones bus error is tested before masking. */
enum {
    ConnControl,ConnStatus,ConnStatusSecond,ConnProtect,ConnProtectAck,
    ConnProtectSecond,ConnProtectSecondAck,ConnGate,ConnEmi,ConnPeripheral,ConnBtifGate,ConnScoIrqMask
};
static const struct {
    MTK_CONN_REGION Region;
    ULONG Offset,Mask,OffValue;
} SocConnFields[MALI_SOC_CONN_FIELDS]={
    {MtkConnSpm,0x304,0x1f,0x12},
    {MtkConnSpm,0x16c,0x02,0},
    {MtkConnSpm,0x170,0x02,0},
    {MtkConnInfra,0x220,0x46000,0x46000},
    {MtkConnInfra,0x228,0x46000,0x46000},
    {MtkConnInfra,0x250,0x00400,0x00400},
    {MtkConnInfra,0x258,0x00400,0x00400},
    {MtkConnInfra,0x0c8,1u<<28,0},
    {MtkConnInfra,0x380,0xfffff,0},
    {MtkConnInfra,0x388,0xfffff,0},
    {MtkConnInfra,MTK_CONN_BTIF_GATE_STATUS,MTK_CONN_BTIF_GATE_MASK,0},
    /* Stock BTCVSD IRQ source mask; only this bit is borrower-owned. */
    {MtkConnInfra,0x0f00U,0x00000800U,0x00000800U}
};
static volatile ULONG *SocConnRegister(MALI_SOC_PROVIDER *S,MTK_CONN_REGION Region,ULONG Offset) {
    return (volatile ULONG *)((UCHAR *)S->Mappings[Region==MtkConnSpm?0:1]+Offset);
}
static NTSTATUS SocConnReadMasked(MALI_SOC_PROVIDER *S,MTK_CONN_REGION Region,ULONG Offset,ULONG Mask,PULONG Value) {
    ULONG Live=READ_REGISTER_ULONG(SocConnRegister(S,Region,Offset));
    if(Live==MAXULONG)return STATUS_DEVICE_HARDWARE_ERROR;
    *Value=Live&Mask;
    return STATUS_SUCCESS;
}
static NTSTATUS SocConnReadField(MALI_SOC_PROVIDER *S,ULONG Field,PULONG Value) {
    return SocConnReadMasked(S,SocConnFields[Field].Region,SocConnFields[Field].Offset,SocConnFields[Field].Mask,Value);
}
static NTSTATUS SocConnReady(const MALI_SOC_PROVIDER *S) {
    if(S->Closing || S->Removed)return STATUS_DEVICE_REMOVED;
    if(!S->Started || S->Stopping || S->Suspended)return STATUS_DEVICE_NOT_READY;
    return STATUS_SUCCESS;
}
static NTSTATUS SocConnLease(const MALI_SOC_PROVIDER *S,ULONGLONG Token) {
    NTSTATUS Result=SocConnReady(S);
    if(Result!=STATUS_SUCCESS)return Result;
    return Token && S->ConnToken==Token?STATUS_SUCCESS:STATUS_ACCESS_DENIED;
}
static NTSTATUS NTAPI SocConnAcquire(PVOID Context,ULONGLONG EmiBase,PULONGLONG Token) {
    MALI_SOC_PROVIDER *S=Context;
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(!SocRange(Token,sizeof(*Token)) || SocOverlap(S,sizeof(*S),Token,sizeof(*Token)) ||
       !EmiBase || (EmiBase&(MTK_CONN_EMI_ALIGN-1U)) || EmiBase>MTK_CONN_EMI_LIMIT-MTK_CONN_EMI_SIZE)
        return SocUnlock(S,STATUS_INVALID_PARAMETER);
    Result=SocConnReady(S);if(Result!=STATUS_SUCCESS)return SocUnlock(S,Result);
    if(S->ConnReferences<=0 || S->Quarantined || S->ReferenceFault)
        return SocUnlock(S,STATUS_DEVICE_NOT_READY);
    if(S->ConnToken)return SocUnlock(S,STATUS_DEVICE_BUSY);
    if(S->ConnGeneration==UINT64_MAX)return SocUnlock(S,STATUS_INSUFFICIENT_RESOURCES);
    for(ULONG Field=0;Field<MALI_SOC_CONN_FIELDS;Field++) {
        Result=SocConnReadField(S,Field,&S->ConnSaved[Field]);
        if(Result!=STATUS_SUCCESS)return SocUnlock(S,Result);
        if(Field<ConnGate && S->ConnSaved[Field]!=SocConnFields[Field].OffValue)
            return SocUnlock(S,STATUS_DEVICE_BUSY);
    }
    S->ConnEmiBase=EmiBase;
    S->ConnToken=++S->ConnGeneration;
    S->Lifetime.Reference(S->Lifetime.Context);
    *Token=S->ConnToken;
    return SocUnlock(S,STATUS_SUCCESS);
}
static NTSTATUS NTAPI SocConnRead(PVOID Context,ULONGLONG Token,MTK_CONN_REGION Region,ULONG Offset,PULONG Value) {
    MALI_SOC_PROVIDER *S=Context;
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(!SocRange(Value,sizeof(*Value)) || SocOverlap(S,sizeof(*S),Value,sizeof(*Value)))
        return SocUnlock(S,STATUS_INVALID_PARAMETER);
    Result=SocConnLease(S,Token);if(Result!=STATUS_SUCCESS)return SocUnlock(S,Result);
    if(Region==MtkConnSpm && Offset==0)
        return SocUnlock(S,SocConnReadMasked(S,Region,Offset,0xffff,Value));
    for(ULONG Field=0;Field<MALI_SOC_CONN_FIELDS;Field++) {
        if(Region==SocConnFields[Field].Region && Offset==SocConnFields[Field].Offset)
            return SocUnlock(S,SocConnReadField(S,Field,Value));
    }
    return SocUnlock(S,STATUS_ACCESS_DENIED);
}
static BOOLEAN SocConnAlias(ULONG Offset,ULONG Value) {
    switch(Offset) {
    case 0x0c0:case 0x0c4:return Value==SocConnFields[ConnGate].Mask;
    case 0x2a0:case 0x2a4:return Value==0x42000 || Value==0x04000;
    case 0x2a8:case 0x2ac:return Value==SocConnFields[ConnProtectSecond].Mask;
    case 0xc14:return Value==0; /* CONN status clear during reset. */
    default:return FALSE;
    }
}
/* The SPM register-control clock is shared. The stock key can only set
   bit 0 while preserving every other low-half field. */
static NTSTATUS SocConnEnableSpmClock(MALI_SOC_PROVIDER *S,ULONG Value) {
    const ULONG Key=0x0b160000;
    if((Value&0xffff0001)!= (Key|1))return STATUS_ACCESS_DENIED;
    volatile ULONG *Register=SocConnRegister(S,MtkConnSpm,0);
    ULONG Live=READ_REGISTER_ULONG(Register);
    if(Live==MAXULONG)return STATUS_DEVICE_HARDWARE_ERROR;
    if(Value!= (Key|(Live&0xffff)|1))return STATUS_ACCESS_DENIED;
    if(!(Live&1))WRITE_REGISTER_ULONG(Register,Value);
    return STATUS_SUCCESS;
}
/* The stock DT binds btifc and apdmac to INFRACFG_AO clock IDs 18 and 61.
   clk-mt6789-bus.ko also puts the APDMA pseudo gate on their AXI parent.
   The I2C controllers share APDMA, so only the dedicated BTIF gate is written. */
static NTSTATUS SocConnBtifParentsReady(MALI_SOC_PROVIDER *S) {
    volatile ULONG *AxiRegister=(volatile ULONG *)((PUCHAR)S->Mappings[3]+0x10);
    ULONG Axi=READ_REGISTER_ULONG(AxiRegister);
    ULONG Dma=READ_REGISTER_ULONG(SocConnRegister(S,MtkConnInfra,0xd8));
    ULONG Pseudo=READ_REGISTER_ULONG(SocConnRegister(S,MtkConnInfra,0x90));
    if(Axi==MAXULONG || Dma==MAXULONG || Pseudo==MAXULONG)return STATUS_DEVICE_HARDWARE_ERROR;
    ULONG Parent=Axi&7u;
    if(Parent>5 || (Dma&(1u<<31)) || (Pseudo&(1u<<12)))return STATUS_DEVICE_NOT_READY;
    if(Parent) {
        volatile ULONG *Pll=(volatile ULONG *)((PUCHAR)S->Mappings[4]+0x340);
        ULONG Control=READ_REGISTER_ULONG(Pll);
        ULONG Setting=READ_REGISTER_ULONG(Pll+1);
        ULONG Power=READ_REGISTER_ULONG(Pll+3);
        if(Control==MAXULONG || Setting==MAXULONG || Power==MAXULONG)return STATUS_DEVICE_HARDWARE_ERROR;
        ULONGLONG Vco=(26000000ull*(Setting&0x3fffffu))>>14;
        if((Control&0xff800001u)!=0xff800001u || (Power&3u)!=1 ||
           Vco<1500000000ull || Vco>3800000000ull)return STATUS_DEVICE_NOT_READY;
    }
    return READ_REGISTER_ULONG(AxiRegister)==Axi?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
}

static NTSTATUS SocConnSetBtifGate(MALI_SOC_PROVIDER *S,ULONG Offset,ULONG Value) {
    BOOLEAN Gated=Offset==MTK_CONN_BTIF_GATE_SET;
    if(Value!=MTK_CONN_BTIF_GATE_MASK || (Gated && !S->ConnSaved[ConnBtifGate]))
        return STATUS_ACCESS_DENIED;
    if(!Gated) {
        NTSTATUS Status=SocConnBtifParentsReady(S);
        if(Status!=STATUS_SUCCESS)return Status;
    }
    ULONG Live,Target=Gated?MTK_CONN_BTIF_GATE_MASK:0;
    NTSTATUS Status=SocConnReadField(S,ConnBtifGate,&Live);
    if(Status!=STATUS_SUCCESS || Live==Target)return Status;
    WRITE_REGISTER_ULONG(SocConnRegister(S,MtkConnInfra,Offset),Value);
    Status=SocConnReadField(S,ConnBtifGate,&Live);
    if(Status!=STATUS_SUCCESS)return Status;
    return Live==Target?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR;
}
static NTSTATUS NTAPI SocConnWrite(PVOID Context,ULONGLONG Token,MTK_CONN_REGION Region,ULONG Offset,ULONG Value) {
    MALI_SOC_PROVIDER *S=Context;
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    Result=SocConnLease(S,Token);if(Result!=STATUS_SUCCESS)return SocUnlock(S,Result);
    if(Region==MtkConnSpm && Offset==0)
        return SocUnlock(S,SocConnEnableSpmClock(S,Value));
    if(Region==MtkConnInfra && (Offset==MTK_CONN_BTIF_GATE_SET || Offset==MTK_CONN_BTIF_GATE_CLEAR))
        return SocUnlock(S,SocConnSetBtifGate(S,Offset,Value));
    if(Region==MtkConnInfra && Offset==SocConnFields[ConnScoIrqMask].Offset) {
        const ULONG Mask=SocConnFields[ConnScoIrqMask].Mask;
        if((Value&~Mask) ||
           (Value!=S->ConnSaved[ConnScoIrqMask] &&
            Value!=(S->ConnSaved[ConnScoIrqMask]^Mask)))
            return SocUnlock(S,STATUS_ACCESS_DENIED);
        volatile ULONG *Register=SocConnRegister(S,Region,Offset);
        ULONG Live=READ_REGISTER_ULONG(Register);
        if(Live==MAXULONG)return SocUnlock(S,STATUS_DEVICE_HARDWARE_ERROR);
        ULONG Updated=(Live&~Mask)|Value;
        if(Updated!=Live)WRITE_REGISTER_ULONG(Register,Updated);
        Live=READ_REGISTER_ULONG(Register);
        if(Live==MAXULONG)return SocUnlock(S,STATUS_DEVICE_HARDWARE_ERROR);
        return SocUnlock(S,(Live&Mask)==Value?STATUS_SUCCESS:STATUS_DEVICE_HARDWARE_ERROR);
    }
    if(Region==MtkConnInfra && SocConnAlias(Offset,Value)) {
        WRITE_REGISTER_ULONG(SocConnRegister(S,Region,Offset),Value);
        return SocUnlock(S,STATUS_SUCCESS);
    }

    ULONG Field;
    if(Region==MtkConnSpm && Offset==SocConnFields[ConnControl].Offset)Field=ConnControl;
    else if(Region==MtkConnInfra && Offset==SocConnFields[ConnEmi].Offset)Field=ConnEmi;
    else if(Region==MtkConnInfra && Offset==SocConnFields[ConnPeripheral].Offset)Field=ConnPeripheral;
    else return SocUnlock(S,STATUS_ACCESS_DENIED);
    ULONG Mask=SocConnFields[Field].Mask;
    if((Value&~Mask) ||
       (Field==ConnEmi && Value!=S->ConnSaved[Field] && Value!= (ULONG)(S->ConnEmiBase>>16)) ||
       (Field==ConnPeripheral && Value!=S->ConnSaved[Field] && Value!=0x1000))
        return SocUnlock(S,STATUS_ACCESS_DENIED);
    volatile ULONG *Register=SocConnRegister(S,Region,Offset);
    ULONG Live=READ_REGISTER_ULONG(Register);
    if(Live==MAXULONG)return SocUnlock(S,STATUS_DEVICE_HARDWARE_ERROR);
    WRITE_REGISTER_ULONG(Register,(Live&~Mask)|Value);
    return SocUnlock(S,STATUS_SUCCESS);
}
static NTSTATUS NTAPI SocConnRelease(PVOID Context,ULONGLONG Token) {
    MALI_SOC_PROVIDER *S=Context;
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    Result=SocConnLease(S,Token);if(Result!=STATUS_SUCCESS)return SocUnlock(S,Result);
    for(ULONG Field=0;Field<MALI_SOC_CONN_FIELDS;Field++) {
        ULONG Live;
        Result=SocConnReadField(S,Field,&Live);
        if(Result!=STATUS_SUCCESS)return SocUnlock(S,Result);
        if(Live!=S->ConnSaved[Field])return SocUnlock(S,STATUS_DEVICE_BUSY);
    }
    MALI_BOARD_LIFETIME Life=S->Lifetime;
    S->ConnToken=0;
    SocUnlock(S,STATUS_SUCCESS);
    Life.Dereference(Life.Context);
    return STATUS_SUCCESS;
}
static void SocConnHold(MALI_SOC_PROVIDER *S) {
    ++S->ConnReferences;
    S->Lifetime.Reference(S->Lifetime.Context);
}
static VOID NTAPI SocConnReference(PVOID Context) {
    MALI_SOC_PROVIDER *S=Context;
    if(SocLock(S)!=STATUS_SUCCESS)return;
    if(S->ConnReferences>0 && S->ConnReferences<MAXLONG)SocConnHold(S);
    else S->ReferenceFault=1;
    SocUnlock(S,STATUS_SUCCESS);
}
static VOID NTAPI SocConnDereference(PVOID Context) {
    MALI_SOC_PROVIDER *S=Context;
    if(SocLock(S)!=STATUS_SUCCESS)return;
    if(S->ConnReferences<=0) {
        S->ReferenceFault=1;
        SocUnlock(S,STATUS_DRIVER_INTERNAL_ERROR);
        return;
    }
    --S->ConnReferences;
    MALI_BOARD_LIFETIME Life=S->Lifetime;
    SocUnlock(S,STATUS_SUCCESS);
    Life.Dereference(Life.Context);
}
NTSTATUS MtkMaliSocGetConnPower(MALI_SOC_PROVIDER *S,MTK_CONN_POWER_INTERFACE *Out) {
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(!SocRange(Out,sizeof(*Out)) || SocOverlap(S,sizeof(*S),Out,sizeof(*Out)))
        return SocUnlock(S,STATUS_INVALID_PARAMETER);
    Result=SocConnReady(S);if(Result!=STATUS_SUCCESS)return SocUnlock(S,Result);
    if(S->Quarantined || S->ReferenceFault)return SocUnlock(S,STATUS_DEVICE_NOT_READY);
    if(S->ConnReferences==MAXLONG)return SocUnlock(S,STATUS_INSUFFICIENT_RESOURCES);
    MTK_CONN_POWER_INTERFACE Interface={
        {sizeof(Interface),MTK_CONN_POWER_INTERFACE_VERSION,S,SocConnReference,SocConnDereference},
        SocConnAcquire,SocConnRead,SocConnWrite,SocConnRelease
    };
    SocConnHold(S);
    *Out=Interface;
    return SocUnlock(S,STATUS_SUCCESS);
}
static int SocClean(const MALI_SOC_PROVIDER *S) {
    const MALI_MFG_CONTROLLER *C=&S->Controller;
    return !C->Poisoned && !C->Power.Poisoned && !C->Power.Domains.Poisoned &&
        !C->Power.Clocks.Poisoned && !S->Pmic.Poisoned && !S->Rails.Poisoned && !S->Clock.Poisoned &&
        !C->Power.References && !C->Power.Stages && !C->Power.Clocks.Stages &&
        !C->Prepared && !C->Enabled && !C->ReferencePrepared && !C->ReferenceEnabled &&
        !S->Rails.Acquired && !S->Rails.Owned && !S->Clock.Held;
}
static int SocAccess(MALI_SOC_PROVIDER *S) {
    return S && S->Signature==SOC_SIGNATURE && S->Started && !S->Removed && !S->Suspended && !S->Quarantined &&
        S->OpenCookie && S->ResourceCookie && S->AccessThread==KeGetCurrentThread();
}
static NTSTATUS SocProof(MALI_SOC_PROVIDER *S,ULONG Proofs,uint64_t Hz,uint32_t Gpu,uint32_t Sram) {
    if(!S->Qualified || !S->GpuPdo || !S->Qualification.Validate)return STATUS_DEVICE_NOT_READY;
    return SocExact(S->Qualification.Validate(S->Qualification.Context,S->GpuPdo,Proofs,Hz,Gpu,Sram));
}
static uint32_t SocRead(void *X,uint32_t Region,uint32_t Offset) {
    MALI_SOC_PROVIDER *S=X;
    if(!SocAccess(S) || Region>=MALI_SOC_REGIONS || Offset>4092 || (Offset&3)) {
        if(S)S->Quarantined=1;return 0;
    }
    return READ_REGISTER_ULONG((volatile ULONG *)((UCHAR *)S->Mappings[Region]+Offset));
}
static void SocWrite(void *X,uint32_t Region,uint32_t Offset,uint32_t Value) {
    MALI_SOC_PROVIDER *S=X;
    if(!SocAccess(S) || Region>=MALI_SOC_REGIONS || Offset>4092 || (Offset&3)) {
        if(S)S->Quarantined=1;return;
    }
    WRITE_REGISTER_ULONG((volatile ULONG *)((UCHAR *)S->Mappings[Region]+Offset),Value);
}
static void SocBarrier(void *X) {(void)X;__dsb(15);}
static void SocDelay(void *X,uint32_t Us) {
    MALI_SOC_PROVIDER *S=X;
    if(!SocAccess(S) || !Us || Us>1000){S->Quarantined=1;return;}
    KeStallExecutionProcessor(Us);
}
static MALI_RESULT SocPmicRead16(void *X,uint32_t Address,uint16_t *Value) {
    if(!Value)return MaliInvalid;
    if(!SocAccess(X))return MaliOffline;
    return MtkMaliPmicPrmRead16(Address,Value);
}
static MALI_RESULT SocPmicWrite16(void *X,uint32_t Address,uint16_t Value) {
    if(!SocAccess(X))return MaliOffline;
    return MtkMaliPmicPrmWrite16(Address,Value);
}
static int SocPmicAuthorized(void *X) {return SocAccess(X);}
static int SocPowerAuthorized(void *X,uint32_t On) {
    MALI_SOC_PROVIDER *S=X;
    return On<2 && SocAccess(S) &&
        SocProof(S,MaliSocProofOwner|MaliSocProofDirectDma|(On?0:MaliSocProofIdle),0,0,0)==STATUS_SUCCESS;
}
static int SocRailAuthorized(void *X,uint32_t Rail,uint32_t On) {
    return Rail<2 && SocPowerAuthorized(X,On);
}
static int SocClockAuthorized(void *X,uint32_t Clock,uint32_t On) {
    return Clock<4 && SocPowerAuthorized(X,On);
}
static int SocDomainAuthorized(void *X,uint32_t Domain,uint32_t On) {
    MALI_SOC_PROVIDER *S=X;
    return Domain<4 && S->Rails.Acquired==3 &&
        (Domain<2 || (S->Controller.ReferenceEnabled && (S->Clock.Held&1))) && SocPowerAuthorized(X,On);
}
static int SocOperatingPoint(void *X,uint64_t Hz,uint32_t Gpu,uint32_t Sram) {
    MALI_SOC_PROVIDER *S=X;
    if(!SocAccess(S) || S->Policy.Signature!=MALI_MFG_OPP_SIGNATURE)return 0;
    for(uint32_t I=0;I<S->Policy.Count;I++)
        if(S->Policy.ActualHz[I]==Hz && S->Policy.Entries[I].VgpuUv==Gpu && S->Policy.Entries[I].VsramUv==Sram)
            return SocProof(S,MaliSocProofOwner|MaliSocProofDirectDma|MaliSocProofOpp,Hz,Gpu,Sram)==STATUS_SUCCESS;
    return 0;
}
void MtkMaliSocCleanForGpu(void *Context,const void *Address,size_t Length) {
    (void)Context;
    /* Wddm.c accepts only trusted nonpaged CPU_VIRTUAL Dxgkrnl tables. Split
       at page boundaries, so the stack MDL always has exactly one PFN slot. */
    struct {MDL Mdl;PFN_NUMBER Pfn;} Storage;
    while(Length) {
        size_t Part=PAGE_SIZE-((uintptr_t)Address&(PAGE_SIZE-1u));if(Part>Length)Part=Length;
        MmInitializeMdl(&Storage.Mdl,(void *)Address,(ULONG)Part);
        MmBuildMdlForNonPagedPool(&Storage.Mdl);
        KeFlushIoBuffers(&Storage.Mdl,FALSE,TRUE);
        Address=(const UCHAR *)Address+Part;Length-=Part;
    }
    __dsb(15);
}
void MtkMaliSocStoreBarrier(void *Context) {(void)Context;__dsb(15);}
static NTSTATUS SocValidate(void *X,const MALI_BOARD_LEASE *L) {
    MALI_SOC_PROVIDER *S=X;
    if(!SocAccess(S) || !L || L->Cookie!=S->ResourceCookie ||
        L->AddressModel!=MaliVmDirectSystemPhysical || L->Cache.Context!=S ||
        L->Cache.CleanForGpu!=MtkMaliSocCleanForGpu || L->Cache.StoreBarrier!=MtkMaliSocStoreBarrier ||
        L->ApertureCommitLimit!=S->Qualification.ApertureCommitLimit || L->TimeoutTicks!=S->Qualification.TimeoutTicks)
        return STATUS_DEVICE_NOT_READY;
    return SocProof(S,MaliSocProofOwner|MaliSocProofDirectDma,0,0,0);
}
static NTSTATUS SocAcquire(void *X,MALI_BOARD_LEASE *L) {
    MALI_SOC_PROVIDER *S=X;NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(!SocRange(L,sizeof(*L)) || SocOverlap(S,sizeof(*S),L,sizeof(*L)))return SocUnlock(S,STATUS_INVALID_PARAMETER);
    if(!S->Started || S->Closing || S->Removed || S->Stopping || S->Suspended || S->Quarantined || S->ReferenceFault ||
       !S->OpenCookie || !S->References)return SocUnlock(S,STATUS_DEVICE_NOT_READY);
    if(S->ResourceCookie || S->AccessThread || !SocClean(S))return SocUnlock(S,STATUS_DEVICE_BUSY);
    if(S->Generation==UINTPTR_MAX)return SocUnlock(S,STATUS_INSUFFICIENT_RESOURCES);
    Result=SocProof(S,MaliSocProofOwner|MaliSocProofDirectDma,0,0,0);
    if(Result!=STATUS_SUCCESS)return SocUnlock(S,Result);
    MALI_BOARD_LEASE Value={0};S->ResourceCookie=(void *)++S->Generation;
    Value.Cookie=S->ResourceCookie;Value.AddressModel=MaliVmDirectSystemPhysical;
    Value.Cache.Context=S;Value.Cache.CleanForGpu=MtkMaliSocCleanForGpu;Value.Cache.StoreBarrier=MtkMaliSocStoreBarrier;
    Value.ApertureCommitLimit=S->Qualification.ApertureCommitLimit;Value.TimeoutTicks=S->Qualification.TimeoutTicks;
    *L=Value;return SocUnlock(S,STATUS_SUCCESS);
}
static NTSTATUS SocEnter(void *X,void *Cookie) {
    MALI_SOC_PROVIDER *S=X;NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(!Cookie || Cookie!=S->ResourceCookie)return SocUnlock(S,STATUS_INVALID_PARAMETER);
    if(!S->Started || S->Closing || S->Removed || S->Stopping || S->Suspended || S->Quarantined || S->ReferenceFault)
        return SocUnlock(S,STATUS_DEVICE_NOT_READY);
    if(S->AccessThread)return SocUnlock(S,STATUS_DEVICE_BUSY);
    Result=SocProof(S,MaliSocProofOwner|MaliSocProofDirectDma,0,0,0);
    if(Result!=STATUS_SUCCESS)return SocUnlock(S,Result);
    S->AccessThread=KeGetCurrentThread();return STATUS_SUCCESS; /* mutex spans the whole hardware operation */
}
static void SocLeave(void *X,void *Cookie,BOOLEAN Quarantine) {
    MALI_SOC_PROVIDER *S=X;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !S || S->Signature!=SOC_SIGNATURE ||
       !Cookie || Cookie!=S->ResourceCookie || S->AccessThread!=KeGetCurrentThread()) {
        if(S)S->Quarantined=1;return; /* Never release another thread's mutex. */
    }
    if(Quarantine || S->Controller.Poisoned || S->Controller.Power.Poisoned ||
       S->Controller.Power.Domains.Poisoned || S->Controller.Power.Clocks.Poisoned ||
       S->Pmic.Poisoned || S->Clock.Poisoned || S->Rails.Poisoned)
        S->Quarantined=1;
    S->AccessThread=0;KeReleaseMutex(&S->Mutex,FALSE);
}
static NTSTATUS SocRelease(void *X,void *Cookie) {
    MALI_SOC_PROVIDER *S=X;NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(!Cookie || Cookie!=S->ResourceCookie)return SocUnlock(S,STATUS_INVALID_PARAMETER);
    if(S->Removed || S->Quarantined || S->ReferenceFault || S->AccessThread || !SocClean(S))
        return SocUnlock(S,STATUS_DEVICE_BUSY);
    S->ResourceCookie=0;return SocUnlock(S,STATUS_SUCCESS);
}
static void SocReference(void *X) {
    MALI_SOC_PROVIDER *S=X;NTSTATUS Result=SocLock(S);
    if(Result!=STATUS_SUCCESS){if(S && S->Signature==SOC_SIGNATURE)InterlockedExchange((volatile LONG *)&S->ReferenceFault,1);return;}
    if(S->References<=0 || S->References==MAXLONG){S->ReferenceFault=1;SocUnlock(S,STATUS_DRIVER_INTERNAL_ERROR);return;}
    ++S->References;S->Lifetime.Reference(S->Lifetime.Context);SocUnlock(S,STATUS_SUCCESS);
}
static void SocDereference(void *X) {
    MALI_SOC_PROVIDER *S=X;NTSTATUS Result=SocLock(S);
    if(Result!=STATUS_SUCCESS){if(S && S->Signature==SOC_SIGNATURE)InterlockedExchange((volatile LONG *)&S->ReferenceFault,1);return;}
    if(S->References<=0){S->ReferenceFault=1;SocUnlock(S,STATUS_DRIVER_INTERNAL_ERROR);return;}
    --S->References;
    if(!S->References) {
        if(S->OpenCookie)S->ReferenceFault=1;
        else {PDEVICE_OBJECT Pdo=S->GpuPdo;S->GpuPdo=0;ObDereferenceObject(Pdo);}
    }
    MALI_BOARD_LIFETIME Life=S->Lifetime;SocUnlock(S,STATUS_SUCCESS);
    Life.Dereference(Life.Context); /* may queue final destruction; no S access after this */
}
static NTSTATUS SocOpen(void *X,PDEVICE_OBJECT Pdo,MALI_BOARD_CONFIG *Out,void **Cookie) {
    MALI_SOC_PROVIDER *S=X;NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(!SocRange(Out,sizeof(*Out)) || !SocRange(Cookie,sizeof(*Cookie)) ||
       SocOverlap(S,sizeof(*S),Out,sizeof(*Out)) || SocOverlap(S,sizeof(*S),Cookie,sizeof(*Cookie)) ||
       SocOverlap(Out,sizeof(*Out),Cookie,sizeof(*Cookie)))return SocUnlock(S,STATUS_INVALID_PARAMETER);
    if(!S->Started || S->Closing || S->Removed || S->Stopping || S->Suspended || S->Quarantined || S->ReferenceFault ||
       S->References<=0 || !Pdo || Pdo!=S->GpuPdo)return SocUnlock(S,STATUS_DEVICE_NOT_READY);
    if(S->OpenCookie || S->ResourceCookie || S->AccessThread || !SocClean(S))return SocUnlock(S,STATUS_DEVICE_BUSY);
    if(S->Generation==UINTPTR_MAX)return SocUnlock(S,STATUS_INSUFFICIENT_RESOURCES);
    /* Only a fully closed, clean client is reconstructed. A live/failed child
       is never zeroed to make initialization pass. No initializer does MMIO. */
    RtlZeroMemory(&S->Pmic,sizeof(S->Pmic));RtlZeroMemory(&S->Rails,sizeof(S->Rails));
    RtlZeroMemory(&S->Clock,sizeof(S->Clock));RtlZeroMemory(&S->Controller,sizeof(S->Controller));
    MALI_MFG_PMIC_IO Pmic={0};
    Pmic.Context=S;Pmic.DelayUs=SocDelay;Pmic.Authorized=SocPmicAuthorized;
    Pmic.BrokerRead16=SocPmicRead16;Pmic.BrokerWrite16=SocPmicWrite16;
    MALI_MFG_RAIL_IO Rails={&S->Pmic,S,SocRailAuthorized};
    MALI_MFG_HW_CLOCK_IO Clock={S,SocRead,SocWrite,SocBarrier,SocDelay,SocClockAuthorized};
    MALI_MFG_CONTROLLER_IO Controller={{S,SocRead,SocWrite,SocBarrier,SocDelay,SocDomainAuthorized},
        &S->Rails,&S->Clock,SocPowerAuthorized,SocOperatingPoint};
    if(MaliMfgPmicInitialize(&S->Pmic,&Pmic)!=MaliOk || MaliMfgRailsInitialize(&S->Rails,&Rails)!=MaliOk ||
       MaliMfgHwClockInitialize(&S->Clock,&Clock)!=MaliOk ||
       MaliMfgControllerInitialize(&S->Controller,&Controller)!=MaliOk) {
        S->Quarantined=1;return SocUnlock(S,STATUS_DRIVER_INTERNAL_ERROR);
    }
    MALI_BOARD_CONFIG Value={0};Value.Controller=&S->Controller;
    Value.Resources.Context=S;Value.Resources.Acquire=SocAcquire;Value.Resources.Validate=SocValidate;
    Value.Resources.Enter=SocEnter;Value.Resources.Leave=SocLeave;Value.Resources.Release=SocRelease;
    S->OpenCookie=(void *)++S->Generation;*Out=Value;*Cookie=S->OpenCookie;
    return SocUnlock(S,STATUS_SUCCESS);
}
static NTSTATUS SocCloseClient(void *X,void *Cookie) {
    MALI_SOC_PROVIDER *S=X;NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(!Cookie || Cookie!=S->OpenCookie)return SocUnlock(S,STATUS_INVALID_PARAMETER);
    if(S->ResourceCookie || S->AccessThread || S->Quarantined || S->ReferenceFault || !SocClean(S))
        return SocUnlock(S,STATUS_DEVICE_BUSY);
    S->OpenCookie=0;return SocUnlock(S,STATUS_SUCCESS);
}
static NTSTATUS SocGpuId(PDEVICE_OBJECT Pdo) {
    WCHAR Buffer[128],Text[]=L"ACPI\\HANA678E";ULONG Bytes=0;
    UNICODE_STRING Expected={sizeof(Text)-sizeof(WCHAR),sizeof(Text),Text};
    NTSTATUS Result=IoGetDeviceProperty(Pdo,DevicePropertyHardwareID,sizeof(Buffer),Buffer,&Bytes);
    if(Result!=STATUS_SUCCESS)return SocExact(Result);
    if(Bytes<2*sizeof(WCHAR) || Bytes>sizeof(Buffer) || (Bytes&1) ||
       Buffer[Bytes/sizeof(WCHAR)-1] || Buffer[Bytes/sizeof(WCHAR)-2])return STATUS_DEVICE_CONFIGURATION_ERROR;
    for(ULONG Offset=0,Count=Bytes/sizeof(WCHAR);Offset<Count && Buffer[Offset];) {
        ULONG End=Offset;while(End<Count && Buffer[End])++End;
        UNICODE_STRING Id={(USHORT)((End-Offset)*sizeof(WCHAR)),(USHORT)((End-Offset)*sizeof(WCHAR)),Buffer+Offset};
        if(RtlEqualUnicodeString(&Id,&Expected,TRUE))return STATUS_SUCCESS;
        Offset=End+1;
    }
    return STATUS_NO_SUCH_DEVICE;
}
NTSTATUS MtkMaliSocInitialize(MALI_SOC_PROVIDER *S,const MALI_BOARD_LIFETIME *Life) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !SocRange(S,sizeof(*S)) || ((uintptr_t)S&7u) ||
       !SocRange(Life,sizeof(*Life)) || SocOverlap(S,sizeof(*S),Life,sizeof(*Life)) ||
       !Life->Reference || !Life->Dereference)return STATUS_INVALID_PARAMETER;
    if(S->Signature || S->Started || S->Closing || S->Removed || S->OpenCookie || S->ResourceCookie ||
       S->References || S->ConnReferences || S->ConnToken || S->AccessThread || S->Qualified ||
       S->Quarantined || S->ReferenceFault)return STATUS_DEVICE_BUSY;
    S->Lifetime=*Life;KeInitializeMutex(&S->Mutex,0);S->Signature=SOC_SIGNATURE;return STATUS_SUCCESS;
}
NTSTATUS MtkMaliSocStart(MALI_SOC_PROVIDER *S,const CM_RESOURCE_LIST *Raw,const CM_RESOURCE_LIST *Translated) {
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(S->Closing || S->Removed || S->Quarantined || S->ReferenceFault)return SocUnlock(S,STATUS_DELETE_PENDING);
    if(S->Started || S->References || S->ConnReferences || S->ConnToken || S->GpuPdo ||
       S->OpenCookie || S->ResourceCookie || S->AccessThread || !SocClean(S))
        return SocUnlock(S,STATUS_DEVICE_BUSY);
    if(!Raw || !Translated || Raw->Count!=1 || Translated->Count!=1 ||
       Raw->List[0].PartialResourceList.Version!=1 || Raw->List[0].PartialResourceList.Revision!=1 ||
       Translated->List[0].PartialResourceList.Version!=1 || Translated->List[0].PartialResourceList.Revision!=1 ||
       Raw->List[0].PartialResourceList.Count!=MALI_SOC_REGIONS ||
       Translated->List[0].PartialResourceList.Count!=MALI_SOC_REGIONS)return SocUnlock(S,STATUS_DEVICE_CONFIGURATION_ERROR);
    ULONG Seen=0;
    for(ULONG I=0;I<MALI_SOC_REGIONS;I++) {
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *A=&Raw->List[0].PartialResourceList.PartialDescriptors[I];
        const CM_PARTIAL_RESOURCE_DESCRIPTOR *B=&Translated->List[0].PartialResourceList.PartialDescriptors[I];
        if(A->Type!=CmResourceTypeMemory || B->Type!=CmResourceTypeMemory ||
           A->ShareDisposition!=CmResourceShareDeviceExclusive || B->ShareDisposition!=CmResourceShareDeviceExclusive ||
           (A->Flags&CM_RESOURCE_MEMORY_WRITEABILITY_MASK)!=CM_RESOURCE_MEMORY_READ_WRITE ||
           (B->Flags&CM_RESOURCE_MEMORY_WRITEABILITY_MASK)!=CM_RESOURCE_MEMORY_READ_WRITE ||
           A->u.Memory.Length!=4096 || B->u.Memory.Length!=4096 || A->u.Memory.Start.QuadPart!=B->u.Memory.Start.QuadPart)
            return SocUnlock(S,STATUS_DEVICE_CONFIGURATION_ERROR);
        ULONG Index=0;while(Index<MALI_SOC_REGIONS && (uint64_t)A->u.Memory.Start.QuadPart!=SocBases[Index])++Index;
        if(Index==MALI_SOC_REGIONS || (Seen&(1u<<Index)))return SocUnlock(S,STATUS_DEVICE_CONFIGURATION_ERROR);
        Seen|=1u<<Index;S->Raw[Index]=*A;S->Translated[Index]=*B;
    }
    for(ULONG I=0;I<MALI_SOC_REGIONS;I++) {
        S->Mappings[I]=MmMapIoSpaceEx(S->Translated[I].u.Memory.Start,4096,PAGE_READWRITE|PAGE_NOCACHE);
        if(!S->Mappings[I]) {
            for(ULONG J=I;J-->0;){MmUnmapIoSpace(S->Mappings[J],4096);S->Mappings[J]=0;}
            return SocUnlock(S,STATUS_INSUFFICIENT_RESOURCES);
        }
    }
    S->Started=1;S->Stopping=0;S->Suspended=0;return SocUnlock(S,STATUS_SUCCESS);
}
NTSTATUS MtkMaliSocSetQualification(MALI_SOC_PROVIDER *S,const MALI_SOC_QUALIFICATION *Q) {
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(!SocRange(Q,sizeof(*Q)) || SocOverlap(S,sizeof(*S),Q,sizeof(*Q)) ||
       Q->Size!=sizeof(*Q) || Q->Version!=2 || !Q->Reference || !Q->Dereference || !Q->Validate ||
       !SocRange(Q->Entries,(size_t)Q->EntryCount*sizeof(*Q->Entries)) || Q->EntryCount>MALI_MFG_OPP_MAX ||
       SocOverlap(S,sizeof(*S),Q->Entries,(size_t)Q->EntryCount*sizeof(*Q->Entries)) ||
       Q->AddressModel!=MaliVmDirectSystemPhysical || !Q->TimeoutTicks ||
       Q->ApertureCommitLimit<4096 || Q->ApertureCommitLimit>MALI_ALLOCATION_MAX_BYTES || (Q->ApertureCommitLimit&4095u))
        return SocUnlock(S,STATUS_INVALID_PARAMETER);
    if(!S->Started || S->Closing || S->Removed || S->Stopping || S->Suspended || S->Quarantined || S->ReferenceFault)
        return SocUnlock(S,STATUS_DEVICE_NOT_READY);
    if(S->Qualified || S->References || S->OpenCookie || S->ResourceCookie || S->AccessThread || !SocClean(S))
        return SocUnlock(S,STATUS_DEVICE_BUSY);
    if(Q->Calibration && (!SocRange(Q->Calibration,sizeof(*Q->Calibration)) ||
       SocOverlap(S,sizeof(*S),Q->Calibration,sizeof(*Q->Calibration)) ||
       SocOverlap(Q,sizeof(*Q),Q->Calibration,sizeof(*Q->Calibration))))return SocUnlock(S,STATUS_INVALID_PARAMETER);
    MALI_RESULT Built=Q->Calibration?MaliMfgOppCalibrate(&S->Policy,Q->Entries,Q->EntryCount,Q->Calibration):
        MaliMfgOppBuild(&S->Policy,Q->Entries,Q->EntryCount);
    if(Built!=MaliOk)return SocUnlock(S,STATUS_INVALID_PARAMETER);
    Q->Reference(Q->Context);S->Qualification=*Q;S->Qualification.Entries=S->Policy.Entries;
    S->Qualification.EntryCount=S->Policy.Count;S->Qualification.Calibration=0;S->Qualified=1;
    return SocUnlock(S,STATUS_SUCCESS);
}
NTSTATUS MtkMaliSocGetFactory(MALI_SOC_PROVIDER *S,PDEVICE_OBJECT Pdo,MALI_BOARD_PARENT_INTERFACE *Out) {
    if(!Pdo || !SocRange(Out,sizeof(*Out)))return STATUS_INVALID_PARAMETER;
    NTSTATUS Result=SocGpuId(Pdo);if(Result!=STATUS_SUCCESS)return Result;
    Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(SocOverlap(S,sizeof(*S),Out,sizeof(*Out)))return SocUnlock(S,STATUS_INVALID_PARAMETER);
    if(!S->Started || S->Closing || S->Removed || S->Stopping || S->Suspended || S->Quarantined || S->ReferenceFault)
        return SocUnlock(S,STATUS_DEVICE_NOT_READY);
    if(S->References==MAXLONG)return SocUnlock(S,STATUS_INSUFFICIENT_RESOURCES);
    if(S->GpuPdo && S->GpuPdo!=Pdo)return SocUnlock(S,STATUS_DEVICE_BUSY);
    if(!S->GpuPdo){S->GpuPdo=Pdo;ObReferenceObject(Pdo);}
    ++S->References;S->Lifetime.Reference(S->Lifetime.Context);
    MALI_BOARD_PARENT_INTERFACE Value={0};Value.Header.Size=sizeof(Value);Value.Header.Version=1;
    Value.Header.Context=S;Value.Header.InterfaceReference=SocReference;Value.Header.InterfaceDereference=SocDereference;
    Value.Open=SocOpen;Value.Close=SocCloseClient;*Out=Value;return SocUnlock(S,STATUS_SUCCESS);
}
static int SocIdle(const MALI_SOC_PROVIDER *S) {
    return !S->References && !S->ConnReferences && !S->ConnToken && !S->GpuPdo && !S->OpenCookie && !S->ResourceCookie &&
        !S->AccessThread && !S->Quarantined && !S->ReferenceFault && SocClean(S);
}
NTSTATUS MtkMaliSocSetStopping(MALI_SOC_PROVIDER *S,ULONG Minor) {
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(Minor!=IRP_MN_QUERY_STOP_DEVICE && Minor!=IRP_MN_QUERY_REMOVE_DEVICE && Minor!=IRP_MN_STOP_DEVICE)
        return SocUnlock(S,STATUS_INVALID_PARAMETER);
    if(S->Closing || S->Removed)return SocUnlock(S,STATUS_DELETE_PENDING);
    if(!SocIdle(S) || (S->Stopping && S->Stopping!=Minor &&
        !(Minor==IRP_MN_STOP_DEVICE && S->Stopping==IRP_MN_QUERY_STOP_DEVICE)))return SocUnlock(S,STATUS_DEVICE_BUSY);
    S->Stopping=Minor;return SocUnlock(S,STATUS_SUCCESS);
}
NTSTATUS MtkMaliSocCancelStopping(MALI_SOC_PROVIDER *S,ULONG Minor) {
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(Minor!=IRP_MN_CANCEL_STOP_DEVICE && Minor!=IRP_MN_CANCEL_REMOVE_DEVICE)return SocUnlock(S,STATUS_INVALID_PARAMETER);
    if(!S->Closing && !S->Removed &&
       ((Minor==IRP_MN_CANCEL_STOP_DEVICE && S->Stopping==IRP_MN_QUERY_STOP_DEVICE) ||
        (Minor==IRP_MN_CANCEL_REMOVE_DEVICE && S->Stopping==IRP_MN_QUERY_REMOVE_DEVICE)))S->Stopping=0;
    return SocUnlock(S,STATUS_SUCCESS);
}
static void SocUnmap(MALI_SOC_PROVIDER *S) {
    for(ULONG I=MALI_SOC_REGIONS;I-->0;)if(S->Mappings[I]){MmUnmapIoSpace(S->Mappings[I],4096);S->Mappings[I]=0;}
    S->Started=0;
    if(S->Qualified) {
        MALI_SOC_QUALIFICATION Q=S->Qualification;S->Qualified=0;RtlZeroMemory(&S->Qualification,sizeof(S->Qualification));
        Q.Dereference(Q.Context);
    }
    RtlZeroMemory(&S->Policy,sizeof(S->Policy));
}
NTSTATUS MtkMaliSocStop(MALI_SOC_PROVIDER *S) {
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(!SocIdle(S))return SocUnlock(S,STATUS_DEVICE_BUSY);
    SocUnmap(S);S->Stopping=IRP_MN_STOP_DEVICE;return SocUnlock(S,STATUS_SUCCESS);
}
NTSTATUS MtkMaliSocWithdraw(MALI_SOC_PROVIDER *S) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !S || S->Signature!=SOC_SIGNATURE)return STATUS_INVALID_PARAMETER;
    InterlockedExchange(&S->Closing,1);
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    InterlockedExchange(&S->Removed,1);
    ULONGLONG Revoked=S->ConnToken;
    MALI_BOARD_LIFETIME Life=S->Lifetime;
    S->ConnToken=0;
    SocUnlock(S,STATUS_SUCCESS);
    if(Revoked)Life.Dereference(Life.Context);
    return STATUS_SUCCESS;
}
NTSTATUS MtkMaliSocClose(MALI_SOC_PROVIDER *S) {
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;S->Closing=1;
    if(!SocIdle(S))return SocUnlock(S,STATUS_DEVICE_BUSY);
    SocUnmap(S);return SocUnlock(S,STATUS_SUCCESS);
}

NTSTATUS MtkMaliSocSuspend(MALI_SOC_PROVIDER *S) {
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(!S->Started || S->Closing || S->Removed || S->Quarantined || S->ReferenceFault)
        return SocUnlock(S,STATUS_DEVICE_NOT_READY);
    /* A D3 GPU may retain its software interfaces/lease, but no active or
       uncertain child hardware journal may cross the owner's power boundary. */
    if(S->ConnToken || S->AccessThread || !SocClean(S))return SocUnlock(S,STATUS_DEVICE_BUSY);
    S->Suspended=1;return SocUnlock(S,STATUS_SUCCESS);
}
NTSTATUS MtkMaliSocResume(MALI_SOC_PROVIDER *S) {
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(!S->Started || S->Closing || S->Removed || S->Quarantined || S->ReferenceFault)
        return SocUnlock(S,STATUS_DEVICE_NOT_READY);
    if(S->AccessThread)return SocUnlock(S,STATUS_DEVICE_BUSY);
    if(S->ResourceCookie) {
        Result=SocProof(S,MaliSocProofOwner|MaliSocProofDirectDma,0,0,0);
        if(Result!=STATUS_SUCCESS)return SocUnlock(S,Result);
    }
    if(!SocClean(S)) {
        if(!S->ResourceCookie || S->Controller.State!=MaliMfgControllerOn)return SocUnlock(S,STATUS_DEVICE_BUSY);
        /* A repeated parent D0 may keep an ON child. Reprove its real state
           under the same mutex before reopening access after lower completion. */
        S->Suspended=0;S->AccessThread=KeGetCurrentThread();
        MALI_RESULT Ready=MaliMfgControllerReady(&S->Controller);S->AccessThread=0;
        if(Ready!=MaliOk){S->Suspended=1;return SocUnlock(S,STATUS_DEVICE_NOT_READY);}
    }
    S->Suspended=0;return SocUnlock(S,STATUS_SUCCESS);
}
NTSTATUS MtkMaliSocFencePower(MALI_SOC_PROVIDER *S) {
    NTSTATUS Result=SocLock(S);if(Result!=STATUS_SUCCESS)return Result;
    if(!S->Started || S->Closing || S->Removed || S->Quarantined || S->ReferenceFault)
        return SocUnlock(S,STATUS_DEVICE_NOT_READY);
    if(S->ConnToken || S->AccessThread)return SocUnlock(S,STATUS_DEVICE_BUSY);
    S->Suspended=1;return SocUnlock(S,STATUS_SUCCESS);
}
