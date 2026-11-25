/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Mfg.h"
#define PWR_STATUS 0x16cu
#define PWR_STATUS_2 0x170u
#define PWR_CONTROL(N) (0x308u+4u*(N))
#define CONTROL_MASK 0x111fu
#define CONTROL_ON 0xdu
#define CONTROL_OFF 0x1112u
static uint32_t MfgRead(MALI_MFG *D,uint32_t R,uint32_t O) {return D->Io.Read32(D->Io.Context,R,O);}
static void MfgWrite(MALI_MFG *D,uint32_t R,uint32_t O,uint32_t V) {
    D->Io.Write32(D->Io.Context,R,O,V);D->Io.Barrier(D->Io.Context);
}
static void MfgChange(MALI_MFG *D,uint32_t O,uint32_t Clear,uint32_t Set) {
    MfgWrite(D,MaliMfgSpm,O,(MfgRead(D,MaliMfgSpm,O)&~Clear)|Set);
}
static MALI_RESULT MfgWait(MALI_MFG *D,uint32_t R,uint32_t O,uint32_t Mask,uint32_t Value) {
    for(uint32_t I=0;I<MALI_MFG_WAIT_POLLS;I++) {
        if((MfgRead(D,R,O)&Mask)==Value)return MaliOk;
        D->Io.DelayUs(D->Io.Context,10);
    }
    D->Poisoned=1;return MaliTimeout;
}
static MALI_RESULT MfgWaitPower(MALI_MFG *D,uint32_t Mask,uint32_t Value) {
    for(uint32_t I=0;I<MALI_MFG_WAIT_POLLS;I++) {
        if((MfgRead(D,MaliMfgSpm,PWR_STATUS)&Mask)==Value &&
           (MfgRead(D,MaliMfgSpm,PWR_STATUS_2)&Mask)==Value)return MaliOk;
        D->Io.DelayUs(D->Io.Context,10);
    }
    D->Poisoned=1;return MaliTimeout;
}
MALI_RESULT MaliMfgInitialize(MALI_MFG *D,const MALI_MFG_IO *Io) {
    if(!D || !Io || !Io->Read32 || !Io->Write32 || !Io->Barrier || !Io->DelayUs || !Io->Authorized)return MaliInvalid;
    D->Io=*Io;D->Initialized=1;D->Poisoned=0;return MaliOk;
}
MALI_RESULT MaliMfgSetDomain(MALI_MFG *D,uint32_t N,uint32_t On) {
    if(!D || !D->Initialized || N>=MALI_MFG_DOMAINS || On>1)return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    if(!D->Io.Authorized(D->Io.Context,N,On))return MaliOffline;
    uint32_t A=MfgRead(D,MaliMfgSpm,PWR_STATUS)&0x3cu,B=MfgRead(D,MaliMfgSpm,PWR_STATUS_2)&0x3cu;
    if(A!=B)return MaliBusy;
    uint32_t Mask=1u<<(N+2),Parents=((1u<<N)-1u)<<2,Children=0x3cu&~(Parents|Mask),O=PWR_CONTROL(N);
    if((On && (A&Parents)!=Parents) || (!On && (A&Children)))return MaliBusy;
    uint32_t C=MfgRead(D,MaliMfgSpm,O)&CONTROL_MASK;
    if(!!(A&Mask)==On)return C==(On?CONTROL_ON:CONTROL_OFF)?MaliOk:MaliBusy;
    if(C!=(On?CONTROL_OFF:CONTROL_ON))return MaliBusy;
    MALI_RESULT R;
    if(On) {
        MfgChange(D,O,0,4);MfgChange(D,O,0,8);
        R=MfgWaitPower(D,Mask,Mask);if(R)return R;
        D->Io.DelayUs(D->Io.Context,100);
        MfgChange(D,O,16,0);MfgChange(D,O,2,0);MfgChange(D,O,0,1);MfgChange(D,O,256,0);
        R=MfgWait(D,MaliMfgSpm,O,4096,0);if(R)return R;
        if(N==1) {
            MfgWrite(D,MaliMfgInfraAo,0x718,0x80);
            MfgWrite(D,MaliMfgInfraAo,0x2a4,0x600000);
            MfgWrite(D,MaliMfgInfraAo,0x718,0x60);
            MfgWrite(D,MaliMfgInfraAo,0x2ac,0x200000);
        }
    } else {
        if(N==1) {
            MfgWrite(D,MaliMfgInfraAo,0x2a8,0x200000);
            R=MfgWait(D,MaliMfgInfraAo,0x258,0x200000,0x200000);if(R)return R;
            MfgWrite(D,MaliMfgInfraAo,0x714,0x60);
            R=MfgWait(D,MaliMfgInfraAo,0x724,0x60,0x60);if(R)return R;
            MfgWrite(D,MaliMfgInfraAo,0x2a0,0x600000);
            R=MfgWait(D,MaliMfgInfraAo,0x228,0x600000,0x600000);if(R)return R;
            MfgWrite(D,MaliMfgInfraAo,0x714,0x80);
            R=MfgWait(D,MaliMfgInfraAo,0x724,0x80,0x80);if(R)return R;
        }
        MfgChange(D,O,0,256);R=MfgWait(D,MaliMfgSpm,O,4096,4096);if(R)return R;
        MfgChange(D,O,0,2);MfgChange(D,O,0,16);MfgChange(D,O,1,0);MfgChange(D,O,4,0);MfgChange(D,O,8,0);
        R=MfgWaitPower(D,Mask,0);if(R)return R;
    }
    return MaliOk;
}
MALI_RESULT MaliMfgAllDomainsReady(MALI_MFG *D) {
    if(!D || !D->Initialized)return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    if(!D->Io.Authorized(D->Io.Context,3,1))return MaliOffline;
    if((MfgRead(D,MaliMfgSpm,PWR_STATUS)&0x3c)!=0x3c ||
       (MfgRead(D,MaliMfgSpm,PWR_STATUS_2)&0x3c)!=0x3c)return MaliBusy;
    for(uint32_t I=0;I<4;I++)if((MfgRead(D,MaliMfgSpm,PWR_CONTROL(I))&CONTROL_MASK)!=CONTROL_ON)return MaliBusy;
    if((MfgRead(D,MaliMfgInfraAo,0x258)&0x200000) || (MfgRead(D,MaliMfgInfraAo,0x228)&0x600000) ||
       (MfgRead(D,MaliMfgInfraAo,0x724)&0xe0))return MaliBusy;
    return MaliOk;
}

static uint32_t ClockRead(MALI_MFG_CLOCK *D,uint32_t R,uint32_t O) {
    return D->Io.Registers.Read32(D->Io.Registers.Context,R,O);
}
static void ClockChange(MALI_MFG_CLOCK *D,uint32_t O,uint32_t Clear,uint32_t Set) {
    MALI_MFG_IO *Io=&D->Io.Registers;
    Io->Write32(Io->Context,MaliMfgTop,O,(ClockRead(D,MaliMfgTop,O)&~Clear)|Set);
    Io->Barrier(Io->Context);
}
static int ClockRegistersReady(MALI_MFG_CLOCK *D) {
    return (ClockRead(D,MaliMfgTopckgen,0x50)&0x40000) &&
        (ClockRead(D,MaliMfgTop,0x20)&0x3c00000)==0x3c00000 &&
        (ClockRead(D,MaliMfgTop,0xb0)&0x80700)==0x80000 &&
        (ClockRead(D,MaliMfgTop,0xb4)&0x10) &&
        (ClockRead(D,MaliMfgTop,0x24)&1) && (ClockRead(D,MaliMfgTop,0xf60)&1);
}
MALI_RESULT MaliMfgClockInitialize(MALI_MFG_CLOCK *D,const MALI_MFG_CLOCK_IO *Io) {
    if(!D || !Io || !Io->Registers.Read32 || !Io->Registers.Write32 ||
       !Io->Registers.Barrier || !Io->Registers.DelayUs || !Io->Registers.Authorized ||
       !Io->Prepare || !Io->Enable || !Io->Disable || !Io->Unprepare || !Io->SetParent)return MaliInvalid;
    if(D->Initialized || D->Poisoned || D->Stages)return MaliBusy;
    D->Io=*Io;D->Initialized=1;return MaliOk;
}
MALI_RESULT MaliMfgClockReady(MALI_MFG_CLOCK *D) {
    if(!D || !D->Initialized)return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    if(!D->Io.Registers.Authorized(D->Io.Registers.Context,3,1))return MaliOffline;
    if(D->Stages!=MaliMfgClocksOn)return MaliBusy;
    return ClockRegistersReady(D)?MaliOk:MaliBusy;
}
MALI_RESULT MaliMfgClockSet(MALI_MFG_CLOCK *D,uint32_t On) {
    if(!D || !D->Initialized || On>1)return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    MALI_MFG_IO *Io=&D->Io.Registers;void *X=Io->Context;
    if(!Io->Authorized(X,3,On))return MaliOffline;
    if(On && D->Stages==MaliMfgClocksOn)return ClockRegistersReady(D)?MaliOk:MaliBusy;
    if(!On && !D->Stages)return MaliOk;
    if(D->Stages!=(On?0:MaliMfgClocksOn))return MaliBusy;
    if(On) {
        if(D->Io.Prepare(X,MaliMfgMuxClock))goto Fault;
        D->Stages|=MaliMfgMuxPrepared;
        if(D->Io.Enable(X,MaliMfgMuxClock)) {
            if(!D->Io.Unprepare(X,MaliMfgMuxClock)){D->Stages&=~MaliMfgMuxPrepared;}
            goto Fault;
        }
        D->Stages|=MaliMfgMuxEnabled;
        if(D->Io.SetParent(X,MaliMfgMainParent))goto Fault;
        D->Stages|=MaliMfgMainSelected;
        if(ClockRead(D,MaliMfgTopckgen,0x50)&0x40000)Io->DelayUs(X,10);
        else {
            /* Stock performs a second read, then at most six parent writes.
               The sixth miss aborts without another read; keep that boundary. */
            unsigned I;
            for(I=0;I<6;I++) {
                if(ClockRead(D,MaliMfgTopckgen,0x50)&0x40000)break;
                if(D->Io.SetParent(X,MaliMfgMainParent))goto Fault;
                Io->DelayUs(X,10);
            }
            if(I==6){D->Poisoned=1;return MaliTimeout;}
        }
        if(D->Io.Prepare(X,MaliMfgBg3dClock))goto Fault;
        D->Stages|=MaliMfgBgPrepared;
        if(D->Io.Enable(X,MaliMfgBg3dClock)) {
            if(!D->Io.Unprepare(X,MaliMfgBg3dClock)){D->Stages&=~MaliMfgBgPrepared;}
            goto Fault;
        }
        D->Stages|=MaliMfgBgEnabled;
        ClockChange(D,0x20,0,0x3c00000);ClockChange(D,0xb0,0x600,0);
        ClockChange(D,0xb4,0,0x10);ClockChange(D,0xb0,0x100,0x80000);
        ClockChange(D,0x24,0,1);ClockChange(D,0xf60,0,1);
        if(!ClockRegistersReady(D))goto Fault;
        D->Stages|=MaliMfgConfigured;
    } else {
        if(D->Io.Disable(X,MaliMfgBg3dClock)){goto Fault;}
        D->Stages&=~MaliMfgBgEnabled;
        if(D->Io.Unprepare(X,MaliMfgBg3dClock)){goto Fault;}
        D->Stages&=~MaliMfgBgPrepared;
        if(D->Io.SetParent(X,MaliMfgSubParent))goto Fault;
        D->Stages&=~MaliMfgMainSelected;
        if(D->Io.Disable(X,MaliMfgMuxClock)){goto Fault;}
        D->Stages&=~MaliMfgMuxEnabled;
        if(D->Io.Unprepare(X,MaliMfgMuxClock)){goto Fault;}
        D->Stages=0;
    }
    return MaliOk;
Fault:
    /* Retain successful acquisitions; a guessed unwind can clock a live GPU. */
    D->Poisoned=1;return MaliFault;
}

MALI_RESULT MaliMfgPowerInitialize(MALI_MFG_POWER *D,const MALI_MFG_POWER_IO *Io) {
    if(!D || !Io || !Io->Authorized || !Io->RailSet || !Io->ReferencePrepare ||
       !Io->ReferenceEnable || !Io->ReferenceDisable || !Io->ReferenceUnprepare)return MaliInvalid;
    if(D->Initialized || D->Poisoned || D->Stages || D->References ||
       D->Domains.Initialized || D->Domains.Poisoned || D->Clocks.Initialized ||
       D->Clocks.Poisoned || D->Clocks.Stages)return MaliBusy;
    const MALI_MFG_IO *A=&Io->Domains,*B=&Io->Clocks.Registers;
    if(A->Context!=B->Context || A->Read32!=B->Read32 || A->Write32!=B->Write32 ||
       A->Barrier!=B->Barrier || A->DelayUs!=B->DelayUs)return MaliInvalid;
    MALI_MFG Domains={0};MALI_MFG_CLOCK Clocks={0};
    if(MaliMfgInitialize(&Domains,A)!=MaliOk || MaliMfgClockInitialize(&Clocks,&Io->Clocks)!=MaliOk)return MaliInvalid;
    D->Io=*Io;D->Domains=Domains;D->Clocks=Clocks;D->Initialized=1;return MaliOk;
}
MALI_RESULT MaliMfgPowerSequenceReady(MALI_MFG_POWER *D) {
    if(!D || !D->Initialized)return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    if(!D->References || D->Stages!=MaliMfgPowerOn)return MaliBusy;
    if(!D->Io.Authorized(D->Io.Domains.Context,1))return MaliOffline;
    MALI_RESULT R=MaliMfgAllDomainsReady(&D->Domains);if(R)return R;
    R=MaliMfgClockReady(&D->Clocks);if(R)return R;
    return (D->Io.Domains.Read32(D->Io.Domains.Context,MaliMfgTop,0x130)&3)==3?MaliOk:MaliBusy;
}
static MALI_RESULT PowerDomain(MALI_MFG_POWER *D,uint32_t N,uint32_t On) {
    static const uint32_t Bits[4]={MaliMfgPowerDomain0,MaliMfgPowerDomain1,MaliMfgPowerDomain2,MaliMfgPowerDomain3};
    MALI_RESULT R=MaliMfgSetDomain(&D->Domains,N,On);
    if(!R){if(On)D->Stages|=Bits[N];else D->Stages&=~Bits[N];}return R;
}
static void PowerWrite(MALI_MFG_POWER *D,uint32_t O,uint32_t V) {
    MALI_MFG_IO *Io=&D->Io.Domains;
    Io->Write32(Io->Context,MaliMfgTop,O,V);Io->Barrier(Io->Context);
}
MALI_RESULT MaliMfgPowerReference(MALI_MFG_POWER *D,uint32_t On) {
    if(!D || !D->Initialized || On>1)return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    MALI_RESULT R;void *X=D->Io.Domains.Context;
    if(On && D->References) {
        if(D->References==UINT32_MAX)return MaliInvalid;
        R=MaliMfgPowerSequenceReady(D);if(R)return R;
        ++D->References;return MaliOk;
    }
    if(!On) {
        if(!D->References)return MaliInvalid;
        if(D->References>1){--D->References;return MaliOk;}
        R=MaliMfgPowerSequenceReady(D);if(R)return R;
    }
    if(!D->Io.Authorized(X,On))return MaliOffline;
    if(On) {
        if(D->Stages || D->Clocks.Stages)return MaliBusy;
        if(D->Io.RailSet(X,0,1)){goto Fault;}
        D->Stages|=MaliMfgPowerRail0;
        if(D->Io.RailSet(X,1,1)){goto Fault;}
        D->Stages|=MaliMfgPowerRail1;
        R=PowerDomain(D,0,1);if(R)goto Failed;
        R=PowerDomain(D,1,1);if(R)goto Failed;
        if(D->Io.ReferencePrepare(X)){goto Fault;}
        D->Stages|=MaliMfgPowerRefPrepared;
        if(D->Io.ReferenceEnable(X)) {
            if(!D->Io.ReferenceUnprepare(X)){D->Stages&=~MaliMfgPowerRefPrepared;}
            goto Fault;
        }
        D->Stages|=MaliMfgPowerRefEnabled;
        R=PowerDomain(D,2,1);if(R)goto Failed;
        R=PowerDomain(D,3,1);if(R)goto Failed;
        R=MaliMfgClockSet(&D->Clocks,1);if(R)goto Failed;
        D->Stages|=MaliMfgPowerClocks;
        PowerWrite(D,0x130,3);D->Stages|=MaliMfgPowerDcm;D->References=1;
        R=MaliMfgPowerSequenceReady(D);if(R)goto Failed;
    } else {
        PowerWrite(D,0xb4,1);PowerWrite(D,0x170,3);
        uint32_t I;
        for(I=0;I<MALI_MFG_WAIT_POLLS;I++) {
            if(D->Io.Domains.Read32(X,MaliMfgTop,0x178)&4)break;
            D->Io.Domains.DelayUs(X,10);
        }
        if(I==MALI_MFG_WAIT_POLLS){R=MaliTimeout;goto Failed;}
        R=MaliMfgClockSet(&D->Clocks,0);if(R)goto Failed;
        D->Stages&=~(MaliMfgPowerClocks|MaliMfgPowerDcm);
        R=PowerDomain(D,3,0);if(R)goto Failed;
        R=PowerDomain(D,2,0);if(R)goto Failed;
        if(D->Io.ReferenceDisable(X)){goto Fault;}
        D->Stages&=~MaliMfgPowerRefEnabled;
        if(D->Io.ReferenceUnprepare(X)){goto Fault;}
        D->Stages&=~MaliMfgPowerRefPrepared;
        R=PowerDomain(D,1,0);if(R)goto Failed;
        R=PowerDomain(D,0,0);if(R)goto Failed;
        if(D->Io.RailSet(X,1,0)){goto Fault;}
        D->Stages&=~MaliMfgPowerRail1;
        if(D->Io.RailSet(X,0,0)){goto Fault;}
        D->Stages&=~MaliMfgPowerRail0;
        D->References=0;
    }
    return MaliOk;
Fault:
    R=MaliFault;
Failed:
    /* Abort is an error return. Keep every successful acquisition and partial
       domain/clock state; never power down a possibly live GPU by guessing. */
    D->Poisoned=1;return R;
}

/* MT6789's stock PWRAP master uses capability bit 15 (0x8800), separate
   write/read data registers, and a three-bit FSM at status bits 3:1. */
#define PMIC_COMMAND 0x880u
#define PMIC_WRITE_DATA 0x884u
#define PMIC_READ_DATA 0x894u
#define PMIC_VALID_CLEAR 0x8a4u
#define PMIC_STATUS 0x8a8u
static void PmicWrite(MALI_MFG_PMIC *D,uint32_t O,uint32_t V) {
    D->Io.Write32(D->Io.Context,O,V);D->Io.Barrier(D->Io.Context);
}
static uint32_t PmicState(MALI_MFG_PMIC *D) {
    return (D->Io.Read32(D->Io.Context,PMIC_STATUS)>>1)&7u;
}
static MALI_RESULT PmicWait(MALI_MFG_PMIC *D,uint32_t State) {
    for(uint32_t I=0;I<MALI_MFG_PMIC_WAIT_POLLS;I++) {
        if(PmicState(D)==State)return MaliOk;
        D->Io.DelayUs(D->Io.Context,10);
    }
    D->Poisoned=1;return MaliTimeout;
}
static MALI_RESULT PmicBegin(MALI_MFG_PMIC *D,uint32_t A) {
    if(!D || !D->Initialized || A>UINT16_MAX || (A&1))return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    if(!D->Io.Authorized(D->Io.Context))return MaliOffline;
    /* An acknowledged lease may inherit an unread response, never an unknown
       active command. Clear only the documented response-valid state. */
    if(PmicState(D)==6)PmicWrite(D,PMIC_VALID_CLEAR,1);
    return PmicWait(D,0);
}
MALI_RESULT MaliMfgPmicInitialize(MALI_MFG_PMIC *D,const MALI_MFG_PMIC_IO *Io) {
    int AnyRaw,Raw,AnyBroker,Broker;
    if(!D || !Io || !Io->DelayUs || !Io->Authorized)return MaliInvalid;
    AnyRaw=Io->Read32 || Io->Write32 || Io->Barrier;
    Raw=Io->Read32 && Io->Write32 && Io->Barrier;
    AnyBroker=Io->BrokerRead16 || Io->BrokerWrite16;
    Broker=Io->BrokerRead16 && Io->BrokerWrite16;
    if((AnyRaw && (!Raw || AnyBroker)) || (AnyBroker && !Broker) || (!AnyRaw && !AnyBroker))
        return MaliInvalid;
    if(D->Initialized || D->Poisoned)return MaliBusy;
    D->Io=*Io;D->Initialized=1;return MaliOk;
}
MALI_RESULT MaliMfgPmicRead16(MALI_MFG_PMIC *D,uint32_t A,uint16_t *V) {
    uint16_t Value;
    MALI_RESULT R;
    if(!V)return MaliInvalid;
    if(!D || !D->Initialized || A>UINT16_MAX || (A&1))return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    if(!D->Io.Authorized(D->Io.Context))return MaliOffline;
    if(D->Io.BrokerRead16) {
        R=D->Io.BrokerRead16(D->Io.Context,A,&Value);
        if(R==MaliOk)*V=Value;
        else if(R!=MaliBusy && R!=MaliOffline && R!=MaliInvalid && R!=MaliUnsupported)D->Poisoned=1;
        return R;
    }
    R=PmicBegin(D,A);if(R)return R;
    PmicWrite(D,PMIC_COMMAND,A);R=PmicWait(D,6);if(R)return R;
    Value=(uint16_t)D->Io.Read32(D->Io.Context,PMIC_READ_DATA);
    PmicWrite(D,PMIC_VALID_CLEAR,1);R=PmicWait(D,0);if(R)return R;
    *V=Value;return MaliOk;
}
MALI_RESULT MaliMfgPmicWrite16(MALI_MFG_PMIC *D,uint32_t A,uint32_t V) {
    MALI_RESULT R;
    if(!D || !D->Initialized || A>UINT16_MAX || (A&1))return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    if(V>UINT16_MAX)return MaliInvalid;
    if(!D->Io.Authorized(D->Io.Context))return MaliOffline;
    if(D->Io.BrokerWrite16) {
        R=D->Io.BrokerWrite16(D->Io.Context,A,(uint16_t)V);
        if(R!=MaliOk && R!=MaliBusy && R!=MaliOffline && R!=MaliInvalid && R!=MaliUnsupported)
            D->Poisoned=1;
        return R;
    }
    R=PmicBegin(D,A);if(R)return R;
    PmicWrite(D,PMIC_WRITE_DATA,V);PmicWrite(D,PMIC_COMMAND,0x20000000u|A);
    return PmicWait(D,0);
}
/* Values from the actual stock mt6358-regulator.ko descriptor table, not
   inferred from a nearby PMIC. DA selectors differ for buck and SRAM LDO. */
typedef struct {uint16_t Enable,Status,Select,Da;uint32_t DaShift,Delay;} MFG_RAIL;
static const MFG_RAIL MfgRails[2]={
    {0x1508,0x1520,0x1526,0x1526,0,200},
    {0x1bc8,0x1be4,0x1bf4,0x1be2,8,240}
};
static MALI_RESULT RailVoltage(MALI_MFG_RAILS *D,uint32_t N,uint32_t Da,uint32_t *Uv) {
    const MFG_RAIL *R=&MfgRails[N];uint16_t V;
    MALI_RESULT E=MaliMfgPmicRead16(D->Io.Pmic,Da?R->Da:R->Select,&V);if(E)return E;
    *Uv=500000u+6250u*((V>>(Da?R->DaShift:0))&0x7fu);return MaliOk;
}
static MALI_RESULT RailWait(MALI_MFG_RAILS *D,uint32_t N,uint32_t On) {
    for(uint32_t I=0;I<MALI_MFG_PMIC_WAIT_POLLS;I++) {
        uint16_t V;MALI_RESULT E=MaliMfgPmicRead16(D->Io.Pmic,MfgRails[N].Status,&V);if(E)return E;
        if((V&1)==On)return MaliOk;
        D->Io.Pmic->Io.DelayUs(D->Io.Pmic->Io.Context,10);
    }
    return MaliTimeout;
}
static MALI_RESULT RailReadyAt(MALI_MFG_RAILS *D,uint32_t N,uint32_t Want) {
    uint16_t V;uint32_t Uv;
    MALI_RESULT R=MaliMfgPmicRead16(D->Io.Pmic,MfgRails[N].Enable,&V);if(R)return R;
    if(!(V&1))return MaliBusy;
    R=MaliMfgPmicRead16(D->Io.Pmic,MfgRails[N].Status,&V);if(R)return R;
    if(!(V&1))return MaliBusy;
    R=RailVoltage(D,N,0,&Uv);if(R)return R;
    if(Uv!=Want)return MaliBusy;
    R=RailVoltage(D,N,1,&Uv);if(R)return R;
    return Uv==Want?MaliOk:MaliBusy;
}
static MALI_RESULT RailReady(MALI_MFG_RAILS *D,uint32_t N) {
    return RailReadyAt(D,N,D->Microvolts[N]);
}
MALI_RESULT MaliMfgRailsInitialize(MALI_MFG_RAILS *D,const MALI_MFG_RAIL_IO *Io) {
    if(!D || !Io || !Io->Pmic || !Io->Pmic->Initialized || Io->Pmic->Poisoned || !Io->Authorized)return MaliInvalid;
    if(D->Initialized || D->Poisoned || D->Acquired || D->Owned)return MaliBusy;
    D->Io=*Io;D->Initialized=1;return MaliOk;
}
MALI_RESULT MaliMfgRailsReady(MALI_MFG_RAILS *D) {
    if(!D || !D->Initialized)return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    if(D->Acquired!=3)return MaliBusy;
    for(uint32_t N=0;N<2;N++)if(!D->Io.Authorized(D->Io.Context,N,1))return MaliOffline;
    for(uint32_t N=0;N<2;N++){MALI_RESULT R=RailReady(D,N);if(R)return R;}
    return MaliOk;
}
MALI_RESULT MaliMfgRailsReleaseReady(MALI_MFG_RAILS *D) {
    if(!D || !D->Initialized)return MaliInvalid;
    if(D->Poisoned || D->Io.Pmic->Poisoned)return MaliFault;
    uint32_t Borrowed=D->Acquired&~D->Owned;
    /* Preflight the entire pair before allowing the first release: a mixed
       owned/borrowed pair may still be needed for its authorized restoration. */
    for(uint32_t N=0;N<2;N++)if(Borrowed&(1u<<N))
        if(D->Microvolts[N]!=D->OriginalMicrovolts[N])return MaliBusy;
    for(uint32_t N=0;N<2;N++)if(Borrowed&(1u<<N)) {
        if(!D->Io.Authorized(D->Io.Context,N,0))return MaliOffline;
        MALI_RESULT R=RailReadyAt(D,N,D->OriginalMicrovolts[N]);
        if(R) {
            if(R!=MaliBusy && R!=MaliOffline)D->Poisoned=1;
            return R;
        }
    }
    return MaliOk;
}
MALI_RESULT MaliMfgRailSet(MALI_MFG_RAILS *D,uint32_t N,uint32_t On) {
    if(!D || !D->Initialized || N>1 || On>1)return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    if(!D->Io.Authorized(D->Io.Context,N,On))return MaliOffline;
    uint32_t Bit=1u<<N;MALI_RESULT R;uint16_t V;
    if(On && (D->Acquired&Bit))return RailReady(D,N);
    if(!On && !(D->Acquired&Bit))return MaliOk;
    if((On && N==1 && !(D->Acquired&1)) || (!On && N==0 && (D->Acquired&2)))return MaliBusy;
    if(On) {
        R=RailVoltage(D,N,0,&D->Microvolts[N]);if(R)goto Failed;
        D->OriginalMicrovolts[N]=D->Microvolts[N];
        R=MaliMfgPmicRead16(D->Io.Pmic,MfgRails[N].Enable,&V);if(R)goto Failed;
        /* Retain ownership before a write that might have reached the PMIC. */
        D->Acquired|=Bit;
        if(!(V&1)) {
            D->Owned|=Bit;R=MaliMfgPmicWrite16(D->Io.Pmic,MfgRails[N].Enable,V|1u);if(R)goto Failed;
            D->Io.Pmic->Io.DelayUs(D->Io.Pmic->Io.Context,MfgRails[N].Delay);
        }
        R=RailWait(D,N,1);if(R)goto Failed;
        R=RailReady(D,N);if(R)goto Failed;
    } else {
        R=MaliMfgRailsReleaseReady(D);if(R)return R;
        if(D->Owned&Bit) {
            R=MaliMfgPmicRead16(D->Io.Pmic,MfgRails[N].Enable,&V);if(R)goto Failed;
            R=MaliMfgPmicWrite16(D->Io.Pmic,MfgRails[N].Enable,V&~1u);if(R)goto Failed;
            R=RailWait(D,N,0);if(R)goto Failed;
            D->Owned&=~Bit;
        }
        D->Acquired&=~Bit;D->Microvolts[N]=D->OriginalMicrovolts[N]=0;
    }
    return MaliOk;
Failed:
    D->Poisoned=1;return R;
}

static int RailValidVoltage(uint32_t Uv) {
    return Uv>=500000 && Uv<=1293750 && !((Uv-500000)%6250);
}
static MALI_RESULT RailSelect(MALI_MFG_RAILS *D,uint32_t N,uint32_t Uv) {
    uint16_t V;MALI_RESULT R=MaliMfgPmicRead16(D->Io.Pmic,MfgRails[N].Select,&V);if(R)return R;
    return MaliMfgPmicWrite16(D->Io.Pmic,MfgRails[N].Select,(V&~0x7fu)|((Uv-500000)/6250));
}
MALI_RESULT MaliMfgRailsVoltage(MALI_MFG_RAILS *D,uint32_t Vgpu,uint32_t Vsram,
                               const MALI_MFG_VOLTAGE_IO *Io) {
    if(!D || !D->Initialized || !Io || !Io->Authorized ||
       !RailValidVoltage(Vgpu) || !RailValidVoltage(Vsram))return MaliInvalid;
    if(D->Poisoned || D->Io.Pmic->Poisoned)return MaliFault;
    if(D->Acquired!=3)return MaliBusy;
    uint32_t OldGpu=D->Microvolts[0],OldSram=D->Microvolts[1];
    if((Vgpu>OldGpu && Vsram<OldSram) || (Vgpu<OldGpu && Vsram>OldSram) ||
       (Vgpu==OldGpu && Vsram!=OldSram))return MaliInvalid;
    uint32_t MiddleGpu=Vgpu>OldGpu?OldGpu:Vgpu,MiddleSram=Vgpu>OldGpu?Vsram:OldSram;
    if(!Io->Authorized(Io->Context,OldGpu,OldSram) ||
       !Io->Authorized(Io->Context,MiddleGpu,MiddleSram) ||
       !Io->Authorized(Io->Context,Vgpu,Vsram))return MaliOffline;
    MALI_RESULT R=MaliMfgRailsReady(D);if(R)return R;
    if(Vgpu==OldGpu)return MaliOk;
    uint32_t First=Vgpu>OldGpu?1u:0u,DelayGpu,DelaySram;
    R=RailSelect(D,First,First?Vsram:Vgpu);if(R)goto Failed;
    R=RailSelect(D,First^1u,First?Vgpu:Vsram);if(R)goto Failed;
    if(Vgpu>OldGpu){DelayGpu=(Vgpu-OldGpu)/8000+9;DelaySram=(Vsram-OldSram)/8000+8;}
    else {DelayGpu=(OldGpu-Vgpu)/4000+9;DelaySram=(OldSram-Vsram)/4000+8;}
    D->Io.Pmic->Io.DelayUs(D->Io.Pmic->Io.Context,DelayGpu>DelaySram?DelayGpu:DelaySram);
    R=RailReadyAt(D,0,Vgpu);if(R)goto Failed;
    R=RailReadyAt(D,1,Vsram);if(R)goto Failed;
    D->Microvolts[0]=Vgpu;D->Microvolts[1]=Vsram;return MaliOk;
Failed:
    D->Poisoned=1;return R;
}
MALI_RESULT MaliMfgPllEncodeRate(uint64_t Hz,uint32_t *Setting,uint64_t *Actual) {
    if(!Setting || !Actual)return MaliInvalid;
    if(Hz<UINT64_C(93750000) || Hz>UINT64_C(3800000000))return MaliUnsupported;
    uint32_t Div=0;
    while((Hz<<Div)<UINT64_C(1500000000))++Div;
    uint64_t Pcw=((Hz<<(Div+14))+13000000)/26000000;
    uint32_t Want=(Div<<24)|(uint32_t)Pcw;uint64_t Vco,Rate;
    MaliMfgPllDecodeRate((uint32_t)Pcw,&Vco);
    if(Div>4 || Pcw>0x3fffffu || Vco<UINT64_C(1500000000) || Vco>UINT64_C(3800000000))return MaliUnsupported;
    MaliMfgPllDecodeRate(Want,&Rate);*Setting=Want;*Actual=Rate;return MaliOk;
}

MALI_RESULT MaliMfgPllDecodeRate(uint32_t Setting,uint64_t *Hz) {
    if(!Hz)return MaliInvalid;
    uint64_t Vco=(UINT64_C(26000000)*(Setting&0x3fffffu)+16383u)>>14;
    uint32_t Div=(Setting>>24)&7u;
    *Hz=(Vco+((UINT64_C(1)<<Div)-1))>>Div;return MaliOk;
}
static uint32_t HwRead(MALI_MFG_HW_CLOCK *D,uint32_t R,uint32_t O) {
    return D->Io.Read32(D->Io.Context,R,O);
}
static void HwWrite(MALI_MFG_HW_CLOCK *D,uint32_t R,uint32_t O,uint32_t V) {
    D->Io.Write32(D->Io.Context,R,O,V);D->Io.Barrier(D->Io.Context);
}
static void HwChange(MALI_MFG_HW_CLOCK *D,uint32_t O,uint32_t Clear,uint32_t Set) {
    HwWrite(D,MaliMfgApmixed,O,(HwRead(D,MaliMfgApmixed,O)&~Clear)|Set);
}
static int HwReferenceReady(MALI_MFG_HW_CLOCK *D) {
    uint32_t C=HwRead(D,MaliMfgTopckgen,0x50);
    if((C&0x800000u) || (C&0x30000u)!=D->RefSetting)return 0;
    if(!D->RefSetting)return 1;
    /* Ref selectors 1/2/3 use MAINPLL factors. Its enable outputs/reset and
       power must already be stable; this provider never switches that PLL. */
    return (HwRead(D,MaliMfgApmixed,0x340)&0xff800001u)==0xff800001u &&
           (HwRead(D,MaliMfgApmixed,0x34c)&3u)==1;
}
static int HwPllReady(MALI_MFG_HW_CLOCK *D) {
    return (HwRead(D,MaliMfgApmixed,0x268)&1) &&
           (HwRead(D,MaliMfgApmixed,0x274)&3u)==1 &&
           (HwRead(D,MaliMfgApmixed,0x26c)&0x073fffffu)==D->PllSetting;
}
MALI_RESULT MaliMfgHwClockInitialize(MALI_MFG_HW_CLOCK *D,const MALI_MFG_HW_CLOCK_IO *Io) {
    if(!D || !Io || !Io->Read32 || !Io->Write32 || !Io->Barrier || !Io->DelayUs || !Io->Authorized)return MaliInvalid;
    if(D->Initialized || D->Poisoned || D->Held)return MaliBusy;
    D->Io=*Io;D->Initialized=1;return MaliOk;
}
MALI_RESULT MaliMfgHwClockReady(MALI_MFG_HW_CLOCK *D) {
    if(!D || !D->Initialized)return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    if(D->Held!=7)return MaliBusy;
    for(uint32_t N=0;N<3;N++)if(!D->Io.Authorized(D->Io.Context,N,1))return MaliOffline;
    if(!HwReferenceReady(D) || !HwPllReady(D) ||
       !(HwRead(D,MaliMfgTopckgen,0x50)&0x40000u) || (HwRead(D,MaliMfgTop,0)&1))return MaliBusy;
    return MaliOk;
}
MALI_RESULT MaliMfgHwClockSet(MALI_MFG_HW_CLOCK *D,uint32_t N,uint32_t On) {
    if(!D || !D->Initialized || N>2 || On>1)return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    if(!D->Io.Authorized(D->Io.Context,N,On))return MaliOffline;
    uint32_t Bit=1u<<N;
    if(!On && !(D->Held&Bit))return MaliOk;
    if(N==MaliMfgHwReference) {
        if(On) {
            uint32_t C=HwRead(D,MaliMfgTopckgen,0x50);
            if(D->Held&Bit)return HwReferenceReady(D)?MaliOk:MaliBusy;
            D->RefSetting=C&0x30000u;
            if(D->RefSetting && ((HwRead(D,MaliMfgApmixed,0x340)&0xff800001u)!=0xff800001u ||
               (HwRead(D,MaliMfgApmixed,0x34c)&3u)!=1))return MaliOffline;
            D->Held|=Bit;HwWrite(D,MaliMfgTopckgen,0x58,0x800000);
            if(!HwReferenceReady(D))goto Fault;
        } else {
            if((D->Held&6) || (HwRead(D,MaliMfgTopckgen,0x50)&0x40000))return MaliBusy;
            HwWrite(D,MaliMfgTopckgen,0x54,0x800000);
            if(!(HwRead(D,MaliMfgTopckgen,0x50)&0x800000u))goto Fault;
            D->Held&=~Bit;
        }
    } else if(N==MaliMfgHwPll) {
        if(On) {
            if(!(D->Held&1) || !HwReferenceReady(D))return MaliBusy;
            if(D->Held&Bit)return HwPllReady(D)?MaliOk:MaliBusy;
            uint32_t C=HwRead(D,MaliMfgApmixed,0x26c);uint64_t Vco;
            MaliMfgPllDecodeRate(C&~0x07000000u,&Vco);
            if(((C>>24)&7)>4 || Vco<UINT64_C(1500000000) || Vco>UINT64_C(3800000000))return MaliUnsupported;
            D->PllSetting=C&0x073fffffu;D->Held|=Bit;
            HwChange(D,0x274,0,1);D->Io.DelayUs(D->Io.Context,1);
            HwChange(D,0x274,2,0);D->Io.DelayUs(D->Io.Context,1);
            HwChange(D,0x268,0,1);D->Io.DelayUs(D->Io.Context,20);
            if(!HwPllReady(D))goto Fault;
        } else {
            if((D->Held&4) || (HwRead(D,MaliMfgTopckgen,0x50)&0x40000))return MaliBusy;
            HwChange(D,0x268,1,0);HwChange(D,0x274,0,2);HwChange(D,0x274,1,0);
            if((HwRead(D,MaliMfgApmixed,0x268)&1) || (HwRead(D,MaliMfgApmixed,0x274)&3u)!=2)goto Fault;
            D->Held&=~Bit;
        }
    } else {
        if(On) {
            if((D->Held&3)!=3 || !HwReferenceReady(D) || !HwPllReady(D) ||
               !(HwRead(D,MaliMfgTopckgen,0x50)&0x40000))return MaliBusy;
            if(D->Held&Bit)return !(HwRead(D,MaliMfgTop,0)&1)?MaliOk:MaliBusy;
            D->Held|=Bit;HwWrite(D,MaliMfgTop,8,1);
            if(HwRead(D,MaliMfgTop,0)&1)goto Fault;
        } else {
            HwWrite(D,MaliMfgTop,4,1);
            if(!(HwRead(D,MaliMfgTop,0)&1))goto Fault;
            D->Held&=~Bit;
        }
    }
    return MaliOk;
Fault:
    D->Poisoned=1;return MaliFault;
}
MALI_RESULT MaliMfgHwClockParent(MALI_MFG_HW_CLOCK *D,uint32_t Parent) {
    if(!D || !D->Initialized || Parent>1)return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    if(!D->Io.Authorized(D->Io.Context,MaliMfgHwParent,Parent==MaliMfgMainParent))return MaliOffline;
    if(!(D->Held&1) || (D->Held&4) || !HwReferenceReady(D))return MaliBusy;
    MALI_RESULT R;
    if(Parent==MaliMfgMainParent) {R=MaliMfgHwClockSet(D,MaliMfgHwPll,1);if(R)return R;}
    uint32_t Want=Parent==MaliMfgMainParent?0x40000u:0;
    if((HwRead(D,MaliMfgTopckgen,0x50)&0x40000u)!=Want) {
        HwWrite(D,MaliMfgTopckgen,0x58,0x40000);HwWrite(D,MaliMfgTopckgen,0x54,Want);
        if((HwRead(D,MaliMfgTopckgen,0x50)&0x40000u)!=Want){D->Poisoned=1;return MaliFault;}
    }
    if(Parent==MaliMfgSubParent) {R=MaliMfgHwClockSet(D,MaliMfgHwPll,0);if(R)return R;}
    return MaliOk;
}

MALI_RESULT MaliMfgHwClockProgramRate(MALI_MFG_HW_CLOCK *D,uint64_t Hz,const MALI_MFG_RATE_IO *Io) {
    uint32_t Setting;uint64_t Actual,OldHz;
    if(!D || !D->Initialized || !Io || !Io->Authorized)return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    MALI_RESULT R=MaliMfgPllEncodeRate(Hz,&Setting,&Actual);if(R)return R;
    if(!D->Io.Authorized(D->Io.Context,MaliMfgHwPll,1))return MaliOffline;
    if(D->Held!=1)return MaliBusy;
    if(!HwReferenceReady(D) || (HwRead(D,MaliMfgTopckgen,0x50)&0x40000u) ||
       !(HwRead(D,MaliMfgTop,0)&1) || (HwRead(D,MaliMfgApmixed,0x268)&1) ||
       (HwRead(D,MaliMfgApmixed,0x274)&3u)!=2)return MaliBusy;
    uint32_t Old=HwRead(D,MaliMfgApmixed,0x26c);
    MaliMfgPllDecodeRate(Old,&OldHz);
    if(!Io->Authorized(Io->Context,OldHz,Hz,Actual))return MaliOffline;
    if((Old&0x073fffffu)==Setting){D->PllSetting=Setting;return MaliOk;}
    HwWrite(D,MaliMfgApmixed,0x26c,(Old&~0x073fffffu)|Setting);
    if((HwRead(D,MaliMfgApmixed,0x26c)&0x073fffffu)!=Setting)goto Failed;
    HwWrite(D,MaliMfgApmixed,0x26c,HwRead(D,MaliMfgApmixed,0x26c)|0x80000000u);
    D->Io.DelayUs(D->Io.Context,20);
    if((HwRead(D,MaliMfgApmixed,0x26c)&0x073fffffu)!=Setting || !HwReferenceReady(D) ||
       (HwRead(D,MaliMfgTopckgen,0x50)&0x40000u) || !(HwRead(D,MaliMfgTop,0)&1) ||
       (HwRead(D,MaliMfgApmixed,0x268)&1) || (HwRead(D,MaliMfgApmixed,0x274)&3u)!=2)goto Failed;
    D->PllSetting=Setting;return MaliOk;
Failed:
    D->Poisoned=1;return MaliFault;
}

/* The springboard block in stock gpufreq probe uses unsigned rail deltas. */
static int OppRange(const void *P,size_t N) {
    return P && N && (uintptr_t)P<=UINTPTR_MAX-N;
}
static int OppOverlap(const void *A,size_t N,const void *B,size_t M) {
    return (uintptr_t)A<(uintptr_t)B+M && (uintptr_t)B<(uintptr_t)A+N;
}
static int OppPair(uint32_t G,uint32_t S) {
    return RailValidVoltage(G) && RailValidVoltage(S) && S>=G && S-G<=MALI_MFG_RAIL_GAP_UV;
}
static uint32_t ControllerRead(void *X,uint32_t R,uint32_t O) {
    MALI_MFG_CONTROLLER *D=X;return D->Io.Registers.Read32(D->Io.Registers.Context,R,O);
}
static void ControllerWrite(void *X,uint32_t R,uint32_t O,uint32_t V) {
    MALI_MFG_CONTROLLER *D=X;D->Io.Registers.Write32(D->Io.Registers.Context,R,O,V);
}
static void ControllerBarrier(void *X) {
    MALI_MFG_CONTROLLER *D=X;D->Io.Registers.Barrier(D->Io.Registers.Context);
}
static void ControllerDelay(void *X,uint32_t Us) {
    MALI_MFG_CONTROLLER *D=X;D->Io.Registers.DelayUs(D->Io.Registers.Context,Us);
}
static int ControllerOwner(MALI_MFG_CONTROLLER *D,uint32_t On) {
    return D->Io.Authorized(D->Io.Registers.Context,On);
}
static int ControllerDomain(void *X,uint32_t N,uint32_t On) {
    MALI_MFG_CONTROLLER *D=X;
    return ControllerOwner(D,On) && D->Io.Rails->Acquired==3 &&
        (N<2 || D->ReferenceEnabled) &&
        D->Io.Registers.Authorized(D->Io.Registers.Context,N,On);
}
static int ControllerClockRegisters(void *X,uint32_t N,uint32_t On) {
    MALI_MFG_CONTROLLER *D=X;
    return N==3 && ControllerOwner(D,On) && D->Io.Rails->Acquired==3 && D->ReferenceEnabled &&
        MaliMfgAllDomainsReady(&D->Power.Domains)==MaliOk;
}
static int ControllerResult(MALI_MFG_CONTROLLER *D,MALI_RESULT R) {
    if(R && !D->LastResult){D->LastResult=R;}
    return (int)R;
}
static int ControllerPowerOwner(void *X,uint32_t On) {return ControllerOwner(X,On);}
static int ControllerRail(void *X,uint32_t N,uint32_t On) {
    MALI_MFG_CONTROLLER *D=X;return ControllerResult(D,MaliMfgRailSet(D->Io.Rails,N,On));
}
static int ControllerReferencePrepare(void *X) {
    MALI_MFG_CONTROLLER *D=X;
    if(!ControllerOwner(D,1))return ControllerResult(D,MaliOffline);
    if(D->ReferencePrepared || D->ReferenceEnabled)return ControllerResult(D,MaliBusy);
    D->ReferencePrepared=1;return 0;
}
static int ControllerReferenceEnable(void *X) {
    MALI_MFG_CONTROLLER *D=X;
    if(!D->ReferencePrepared || D->ReferenceEnabled)return ControllerResult(D,MaliBusy);
    MALI_RESULT R=MaliMfgHwClockSet(D->Io.Clock,MaliMfgHwReference,1);
    if(!R){D->ReferenceEnabled=1;}
    return ControllerResult(D,R);
}
static int ControllerReferenceDisable(void *X) {
    MALI_MFG_CONTROLLER *D=X;
    if(!D->ReferencePrepared || !D->ReferenceEnabled)return ControllerResult(D,MaliBusy);
    MALI_RESULT R=MaliMfgHwClockSet(D->Io.Clock,MaliMfgHwReference,0);
    if(!R){D->ReferenceEnabled=0;}
    return ControllerResult(D,R);
}
static int ControllerReferenceUnprepare(void *X) {
    MALI_MFG_CONTROLLER *D=X;
    if(!D->ReferencePrepared || D->ReferenceEnabled || D->Io.Clock->Poisoned || (D->Io.Clock->Held&1))
        return ControllerResult(D,MaliBusy);
    D->ReferencePrepared=0;return 0;
}
static int ControllerPrepare(void *X,uint32_t N) {
    MALI_MFG_CONTROLLER *D=X;if(N>1)return ControllerResult(D,MaliInvalid);
    uint32_t Bit=1u<<N;
    if(!ControllerOwner(D,1))return ControllerResult(D,MaliOffline);
    if((D->Prepared&Bit) || !D->ReferenceEnabled)return ControllerResult(D,MaliBusy);
    D->Prepared|=Bit;return 0;
}
static int ControllerEnable(void *X,uint32_t N) {
    MALI_MFG_CONTROLLER *D=X;if(N>1)return ControllerResult(D,MaliInvalid);
    uint32_t Bit=1u<<N;
    if(!ControllerOwner(D,1))return ControllerResult(D,MaliOffline);
    if(!(D->Prepared&Bit) || (D->Enabled&Bit))return ControllerResult(D,MaliBusy);
    MALI_RESULT R=N==MaliMfgBg3dClock?MaliMfgHwClockSet(D->Io.Clock,MaliMfgHwBg3d,1):MaliOk;
    if(!R){D->Enabled|=Bit;}
    return ControllerResult(D,R);
}
static int ControllerDisable(void *X,uint32_t N) {
    MALI_MFG_CONTROLLER *D=X;if(N>1)return ControllerResult(D,MaliInvalid);
    uint32_t Bit=1u<<N;
    if(!ControllerOwner(D,0))return ControllerResult(D,MaliOffline);
    if(!(D->Enabled&Bit))return ControllerResult(D,MaliBusy);
    MALI_RESULT R;
    if(N==MaliMfgBg3dClock)R=MaliMfgHwClockSet(D->Io.Clock,MaliMfgHwBg3d,0);
    else R=(D->Io.Clock->Held&6)?MaliBusy:MaliOk;
    if(!R){D->Enabled&=~Bit;}
    return ControllerResult(D,R);
}
static int ControllerUnprepare(void *X,uint32_t N) {
    MALI_MFG_CONTROLLER *D=X;if(N>1)return ControllerResult(D,MaliInvalid);
    uint32_t Bit=1u<<N;
    if(!(D->Prepared&Bit) || (D->Enabled&Bit) || D->Io.Clock->Poisoned ||
       (D->Io.Clock->Held&(N==MaliMfgBg3dClock?4u:6u)))return ControllerResult(D,MaliBusy);
    D->Prepared&=~Bit;return 0;
}
static int ControllerParent(void *X,uint32_t Parent) {
    MALI_MFG_CONTROLLER *D=X;
    if((D->Prepared&1)==0 || (D->Enabled&3)!=1)return ControllerResult(D,MaliBusy);
    return ControllerResult(D,MaliMfgHwClockParent(D->Io.Clock,Parent));
}
static int ControllerPoisoned(MALI_MFG_CONTROLLER *D) {
    return D->Power.Poisoned || D->Power.Domains.Poisoned || D->Power.Clocks.Poisoned ||
        D->Io.Rails->Poisoned || D->Io.Rails->Io.Pmic->Poisoned || D->Io.Clock->Poisoned;
}
static MALI_RESULT ControllerFinish(MALI_MFG_CONTROLLER *D,MALI_RESULT R) {
    if(ControllerPoisoned(D))D->Poisoned=1;
    D->LastResult=R;return R;
}
MALI_RESULT MaliMfgControllerInitialize(MALI_MFG_CONTROLLER *D,const MALI_MFG_CONTROLLER_IO *Io) {
    if(!OppRange(D,sizeof(*D)) || !OppRange(Io,sizeof(*Io)) ||
       ((uintptr_t)D&7u) || ((uintptr_t)Io&7u) || OppOverlap(D,sizeof(*D),Io,sizeof(*Io)))return MaliInvalid;
    if(!OppRange(Io->Rails,sizeof(*Io->Rails)) || !OppRange(Io->Clock,sizeof(*Io->Clock)) ||
       ((uintptr_t)Io->Rails&7u) || ((uintptr_t)Io->Clock&7u) ||
       OppOverlap(D,sizeof(*D),Io->Rails,sizeof(*Io->Rails)) ||
       OppOverlap(D,sizeof(*D),Io->Clock,sizeof(*Io->Clock)) ||
       OppOverlap(Io,sizeof(*Io),Io->Rails,sizeof(*Io->Rails)) ||
       OppOverlap(Io,sizeof(*Io),Io->Clock,sizeof(*Io->Clock)) ||
       OppOverlap(Io->Rails,sizeof(*Io->Rails),Io->Clock,sizeof(*Io->Clock)))return MaliInvalid;
    if(!Io->Registers.Read32 || !Io->Registers.Write32 || !Io->Registers.Barrier ||
       !Io->Registers.DelayUs || !Io->Registers.Authorized || !Io->Authorized || !Io->OperatingPoint ||
       !Io->Rails->Initialized || Io->Rails->Poisoned ||
       !Io->Clock->Initialized || Io->Clock->Poisoned)return MaliInvalid;
    if(!OppRange(Io->Rails->Io.Pmic,sizeof(*Io->Rails->Io.Pmic)) ||
       ((uintptr_t)Io->Rails->Io.Pmic&7u) ||
       OppOverlap(D,sizeof(*D),Io->Rails->Io.Pmic,sizeof(*Io->Rails->Io.Pmic)) ||
       OppOverlap(Io,sizeof(*Io),Io->Rails->Io.Pmic,sizeof(*Io->Rails->Io.Pmic)) ||
       OppOverlap(Io->Rails,sizeof(*Io->Rails),Io->Rails->Io.Pmic,sizeof(*Io->Rails->Io.Pmic)) ||
       OppOverlap(Io->Clock,sizeof(*Io->Clock),Io->Rails->Io.Pmic,sizeof(*Io->Rails->Io.Pmic)) ||
       !Io->Rails->Io.Pmic->Initialized || Io->Rails->Io.Pmic->Poisoned)return MaliInvalid;
    if(D->Initialized || D->Poisoned || D->State || D->LastResult ||
       D->Power.Initialized || D->Power.Poisoned || D->Power.Stages || D->Power.References ||
       D->Power.Domains.Initialized || D->Power.Domains.Poisoned || D->Power.Clocks.Initialized ||
       D->Power.Clocks.Poisoned || D->Power.Clocks.Stages || D->Prepared || D->Enabled || D->ReferencePrepared || D->ReferenceEnabled ||
       Io->Rails->Acquired || Io->Rails->Owned || Io->Clock->Held)return MaliBusy;
    D->Io=*Io;
    MALI_MFG_IO Domains={D,ControllerRead,ControllerWrite,ControllerBarrier,ControllerDelay,ControllerDomain};
    MALI_MFG_IO Clocks=Domains;Clocks.Authorized=ControllerClockRegisters;
    MALI_MFG_CLOCK_IO C={Clocks,ControllerPrepare,ControllerEnable,ControllerDisable,ControllerUnprepare,ControllerParent};
    MALI_MFG_POWER_IO P={Domains,C,ControllerPowerOwner,ControllerRail,ControllerReferencePrepare,
        ControllerReferenceEnable,ControllerReferenceDisable,ControllerReferenceUnprepare};
    MALI_RESULT R=MaliMfgPowerInitialize(&D->Power,&P);
    if(!R){D->Initialized=1;D->State=MaliMfgControllerUnknown;}return R;
}
static MALI_RESULT ControllerPoint(MALI_MFG_CONTROLLER *D) {
    uint32_t G,S;uint64_t Hz,Vco;
    uint32_t Setting=ControllerRead(D,MaliMfgApmixed,0x26c)&0x073fffffu;
    MaliMfgPllDecodeRate(Setting&~0x07000000u,&Vco);MaliMfgPllDecodeRate(Setting,&Hz);
    if(((Setting>>24)&7)>4 || Vco<UINT64_C(1500000000) || Vco>UINT64_C(3800000000))return MaliUnsupported;
    MALI_RESULT R=RailVoltage(D->Io.Rails,0,0,&G);if(R)return R;
    R=RailVoltage(D->Io.Rails,1,0,&S);if(R)return R;
    if(!OppPair(G,S))return MaliInvalid;
    return D->Io.OperatingPoint(D->Io.Registers.Context,Hz,G,S)?MaliOk:MaliOffline;
}
static MALI_RESULT ControllerValidateOn(MALI_MFG_CONTROLLER *D) {
    MALI_RESULT R=MaliMfgPowerSequenceReady(&D->Power);if(R)return R;
    R=MaliMfgRailsReady(D->Io.Rails);if(R)return R;
    R=MaliMfgHwClockReady(D->Io.Clock);if(R)return R;
    return ControllerPoint(D);
}
MALI_RESULT MaliMfgControllerReady(MALI_MFG_CONTROLLER *D) {
    if(!D || !D->Initialized)return MaliInvalid;
    if(D->Poisoned || ControllerPoisoned(D))return ControllerFinish(D,MaliFault);
    if(D->State!=MaliMfgControllerOn)return MaliBusy;
    return ControllerFinish(D,ControllerValidateOn(D));
}
MALI_RESULT MaliMfgControllerSet(MALI_MFG_CONTROLLER *D,uint32_t On) {
    if(!D || !D->Initialized || On>1)return MaliInvalid;
    if(D->Poisoned || ControllerPoisoned(D))return ControllerFinish(D,MaliFault);
    D->LastResult=MaliOk;
    if(!ControllerOwner(D,On))return ControllerFinish(D,MaliOffline);
    if(On && D->Power.References) {
        if(D->Power.References!=1)return ControllerFinish(D,MaliBusy);
        MALI_RESULT R=ControllerValidateOn(D);if(!R)D->State=MaliMfgControllerOn;
        return ControllerFinish(D,R);
    }
    if(!On && !D->Power.References)return ControllerFinish(D,D->State==MaliMfgControllerOff?MaliOk:MaliBusy);
    MALI_RESULT R;
    if(On)R=ControllerPoint(D);
    else R=MaliMfgRailsReleaseReady(D->Io.Rails);
    if(R)return ControllerFinish(D,R);
    D->State=MaliMfgControllerUnknown;
    R=MaliMfgPowerReference(&D->Power,On);
    if(R && D->LastResult)R=D->LastResult;
    if(!R && On)R=ControllerValidateOn(D);
    if(!R && !On && (D->Power.Stages || D->Power.References || D->Power.Clocks.Stages ||
       D->Prepared || D->Enabled || D->ReferencePrepared || D->ReferenceEnabled ||
       D->Io.Rails->Acquired || D->Io.Rails->Owned || D->Io.Clock->Held)) {D->Poisoned=1;R=MaliFault;}
    if(!R)D->State=On?MaliMfgControllerOn:MaliMfgControllerOff;
    return ControllerFinish(D,R);
}

MALI_RESULT MaliMfgOppBuild(MALI_MFG_OPP_POLICY *P,const MALI_MFG_OPP *E,uint32_t N) {
    if(!N || N>MALI_MFG_OPP_MAX || !OppRange(P,sizeof(*P)) ||
       !OppRange(E,(size_t)N*sizeof(*E)) || ((uintptr_t)P&7u) || ((uintptr_t)E&7u) ||
       OppOverlap(P,sizeof(*P),E,(size_t)N*sizeof(*E)))return MaliInvalid;
    if(P->Signature || P->Count)return MaliBusy;
    uint64_t PreviousActual=UINT64_MAX;
    for(uint32_t I=0;I<N;I++) {
        uint32_t Setting;uint64_t Actual;
        if(!OppPair(E[I].VgpuUv,E[I].VsramUv))return MaliInvalid;
        MALI_RESULT R=MaliMfgPllEncodeRate(E[I].Hz,&Setting,&Actual);if(R)return R;
        if(I && Actual>=PreviousActual)return MaliUnsupported;
        PreviousActual=Actual;
        if(I && (E[I-1].Hz<=E[I].Hz || E[I-1].VgpuUv<E[I].VgpuUv ||
            E[I-1].VsramUv<E[I].VsramUv ||
            (E[I-1].VgpuUv==E[I].VgpuUv && E[I-1].VsramUv!=E[I].VsramUv)))return MaliInvalid;
        /* Even adjacent points must be reachable with the stock pair order.
           Otherwise the original springboard can fail to advance forever. */
        if(I && !OppPair(E[I].VgpuUv,E[I-1].VsramUv))return MaliUnsupported;
    }
    for(size_t I=0;I<sizeof(*P);I++)((uint8_t*)P)[I]=0;
    for(uint32_t I=0;I<N;I++) {
        uint32_t Setting;P->Entries[I]=E[I];
        MaliMfgPllEncodeRate(E[I].Hz,&Setting,&P->ActualHz[I]);
        uint32_t Up=0,Down=N-1;
        while(Up<N && !OppPair(E[I].VgpuUv,E[Up].VsramUv))++Up;
        while(Down && !OppPair(E[Down].VgpuUv,E[I].VsramUv))--Down;
        P->Up[I]=Up;P->Down[I]=Down;
    }
    P->Count=N;P->Signature=MALI_MFG_OPP_SIGNATURE;return MaliOk;
}
MALI_RESULT MaliMfgOppCalibrate(MALI_MFG_OPP_POLICY *P,const MALI_MFG_OPP *Base,
                               uint32_t Count,const MALI_MFG_CALIBRATION *C) {
    if(Count!=MALI_MFG_STOCK_OPP_COUNT || !OppRange(P,sizeof(*P)) ||
       !OppRange(Base,(size_t)Count*sizeof(*Base)) || !OppRange(C,sizeof(*C)) ||
       ((uintptr_t)P&7u) || ((uintptr_t)Base&7u) || ((uintptr_t)C&3u) ||
       OppOverlap(P,sizeof(*P),Base,(size_t)Count*sizeof(*Base)) ||
       OppOverlap(P,sizeof(*P),C,sizeof(*C)) ||
       OppOverlap(Base,(size_t)Count*sizeof(*Base),C,sizeof(*C)) ||
       C->Size!=sizeof(*C) || C->Version!=1 || (C->Segment!=1 && C->Segment!=2) ||
       C->Mcl50Load>1 || C->Aging>1 || C->PresentMask!=7)return MaliInvalid;
    if(P->Signature || P->Count)return MaliBusy;
    MALI_MFG_OPP E[MALI_MFG_STOCK_OPP_COUNT];
    static const uint32_t Anchors[3]={0,32,44};
    for(uint32_t I=0;I<Count;I++) {
        if(!OppPair(Base[I].VgpuUv,Base[I].VsramUv) || Base[I].Hz%1000 ||
           Base[I].Hz>UINT64_C(1100000000) || Base[I].Hz<UINT64_C(390000000) ||
           (I && (Base[I-1].Hz<=Base[I].Hz || Base[I-1].VgpuUv<Base[I].VgpuUv ||
                  Base[I-1].VsramUv<Base[I].VsramUv)))return MaliInvalid;
        E[I]=Base[I];
    }
    if(E[0].Hz!=UINT64_C(1100000000) || E[32].Hz!=UINT64_C(700000000) ||
       E[44].Hz!=UINT64_C(390000000))return MaliUnsupported;
    uint32_t V[3]={0};
    for(uint32_t I=0;I<3;I++)if(C->AvsWords[I]) {
        uint64_t Hz=(uint64_t)((C->AvsWords[I]>>8)&2047u)*1000000u;
        if(Hz!=E[Anchors[I]].Hz)return MaliUnsupported;
        /* Shipped units are 10 microvolts, low byte is a 6.25mV step.
           A zero selector is an absent adjustment, not a request for 0V. */
        V[I]=(C->AvsWords[I]&255u)*6250u;
        if(V[I]>E[Anchors[I]].VgpuUv)V[I]=E[Anchors[I]].VgpuUv;
    }
    for(uint32_t I=3;I-->0;)if(V[I]) {
        if(I<2 && V[I]<V[I+1])V[I]=V[I+1]+12500u;
        if(V[I]>E[Anchors[I]].VgpuUv)V[I]=E[Anchors[I]].VgpuUv;
        E[Anchors[I]].VgpuUv=V[I];
        E[Anchors[I]].VsramUv=V[I]>750000u?V[I]:750000u;
    }
    for(uint32_t A=0;A<2;A++) {
        uint32_t Hi=Anchors[A],Lo=Anchors[A+1];
        uint32_t Hf=(uint32_t)(E[Hi].Hz/1000000u),Lf=(uint32_t)(E[Lo].Hz/1000000u);
        /* Preserve the stock slope truncation in 10uV units, then round up
           to a regulator step. Combining divisions changes the result. */
        if(Hf<=Lf || E[Hi].VgpuUv<E[Lo].VgpuUv)return MaliUnsupported;
        uint32_t Slope=((E[Hi].VgpuUv-E[Lo].VgpuUv)/10u)*100u/(Hf-Lf);
        for(uint32_t I=Lo;--I>Hi;) {
            uint32_t F=(uint32_t)(E[I].Hz/1000000u);
            if(F<Lf || F>Hf)return MaliUnsupported;
            uint32_t Value=((F-Lf)*Slope+(E[Lo].VgpuUv/10u)*100u)/100u;
            Value=((Value+624u)/625u)*6250u;
            if(Value<E[I+1].VgpuUv)return MaliUnsupported;
            E[I].VgpuUv=Value;E[I].VsramUv=Value>750000u?Value:750000u;
        }
    }
    uint32_t First=C->Segment==1 || C->Mcl50Load?7u:0u;
    if(C->Aging)for(uint32_t I=First;I<Count;I++) {
        uint32_t Drop=I<20?18750u:I<36?12500u:6250u;
        if(E[I].VgpuUv<Drop)return MaliUnsupported;
        E[I].VgpuUv-=Drop;
        E[I].VsramUv=E[I].VgpuUv>750000u?E[I].VgpuUv:750000u;
    }
    return MaliMfgOppBuild(P,E+First,Count-First);
}
static int OppValid(const MALI_MFG_OPP_POLICY *P) {
    return OppRange(P,sizeof(*P)) && !((uintptr_t)P&7u) &&
        P->Signature==MALI_MFG_OPP_SIGNATURE && P->Count && P->Count<=MALI_MFG_OPP_MAX;
}
static void OppStep(MALI_MFG_OPP_PLAN *Out,uint32_t N,uint32_t Kind,uint32_t Index,
                    const MALI_MFG_OPP *E) {
    if(!Out)return;
    MALI_MFG_OPP_STEP *S=&Out->Steps[N];S->Kind=Kind;S->Index=Index;
    S->Hz=E->Hz;S->VgpuUv=E->VgpuUv;S->VsramUv=E->VsramUv;
}
static MALI_RESULT OppWalk(const MALI_MFG_OPP_POLICY *P,uint32_t Initial,uint32_t Target,
                           MALI_MFG_OPP_PLAN *Out,uint32_t *Count) {
    uint32_t Current=Initial,N=0;const MALI_MFG_OPP *Goal=&P->Entries[Target];
    if(Initial==Target) {OppStep(Out,N++,MaliMfgOppVoltage,Target,Goal);}
    else {
        if(Target>Initial)OppStep(Out,N++,MaliMfgOppRate,Target,Goal);
        while(P->Entries[Current].VgpuUv!=Goal->VgpuUv) {
            uint32_t Next;
            if(N>=MALI_MFG_OPP_MAX_STEPS)return MaliInvalid;
            if(Target<Initial) {
                Next=P->Up[Current];if(Next<Target)Next=Target;
                if(Next>=Current || Next<Target || Next>=P->Count)return MaliInvalid;
            } else {
                Next=P->Down[Current];if(Next>Target)Next=Target;
                if(Next<=Current || Next>Target || Next>=P->Count)return MaliInvalid;
            }
            const MALI_MFG_OPP *Old=&P->Entries[Current],*New=&P->Entries[Next];
            if(!OppPair(New->VgpuUv,New->VsramUv) ||
               !OppPair(Target<Initial?Old->VgpuUv:New->VgpuUv,
                        Target<Initial?New->VsramUv:Old->VsramUv))return MaliInvalid;
            OppStep(Out,N++,MaliMfgOppVoltage,Next,New);Current=Next;
        }
        if(P->Entries[Current].VsramUv!=Goal->VsramUv)return MaliInvalid;
        if(Target<Initial) {
            if(N>=MALI_MFG_OPP_MAX_STEPS)return MaliInvalid;
            OppStep(Out,N++,MaliMfgOppRate,Target,Goal);
        }
    }
    if(Count){*Count=N;}
    return MaliOk;
}
MALI_RESULT MaliMfgOppPlan(const MALI_MFG_OPP_POLICY *P,uint32_t Initial,uint32_t Target,MALI_MFG_OPP_PLAN *Out) {
    if(!OppValid(P) || Initial>=P->Count || Target>=P->Count ||
       !OppRange(Out,sizeof(*Out)) || ((uintptr_t)Out&7u) ||
       OppOverlap(P,sizeof(*P),Out,sizeof(*Out)))return MaliInvalid;
    uint32_t N;MALI_RESULT R=OppWalk(P,Initial,Target,0,&N);if(R)return R;
    for(size_t I=0;I<sizeof(*Out);I++)((uint8_t*)Out)[I]=0;
    OppWalk(P,Initial,Target,Out,0);Out->Count=N;Out->Initial=Initial;Out->Target=Target;return MaliOk;
}
static MALI_RESULT DvfsAt(MALI_MFG_DVFS *D,uint32_t Index) {
    const MALI_MFG_OPP *E=&D->Policy->Entries[Index];uint64_t Hz;
    MALI_RESULT R=MaliMfgRailsReady(D->Rails);if(R)return R;
    R=MaliMfgHwClockReady(D->Clock);if(R)return R;
    R=MaliMfgPllDecodeRate(D->Clock->PllSetting,&Hz);if(R)return R;
    return Hz==D->Policy->ActualHz[Index] && D->Rails->Microvolts[0]==E->VgpuUv &&
        D->Rails->Microvolts[1]==E->VsramUv?MaliOk:MaliBusy;
}
MALI_RESULT MaliMfgDvfsInitialize(MALI_MFG_DVFS *D,const MALI_MFG_OPP_POLICY *P,
    MALI_MFG_RAILS *Rails,MALI_MFG_HW_CLOCK *Clock,const MALI_MFG_DVFS_IO *Io,uint32_t Initial) {
    if(!OppRange(D,sizeof(*D)) || ((uintptr_t)D&7u) || !OppValid(P) ||
       !OppRange(Rails,sizeof(*Rails)) || ((uintptr_t)Rails&7u) ||
       !OppRange(Clock,sizeof(*Clock)) || ((uintptr_t)Clock&7u) ||
       !OppRange(Io,sizeof(*Io)) || ((uintptr_t)Io&7u) ||
       OppOverlap(D,sizeof(*D),P,sizeof(*P)) || OppOverlap(D,sizeof(*D),Rails,sizeof(*Rails)) ||
       OppOverlap(D,sizeof(*D),Clock,sizeof(*Clock)) || OppOverlap(D,sizeof(*D),Io,sizeof(*Io)) ||
       !Io->Authorized || !Io->Voltage.Authorized || !Io->Rate.Authorized || Initial>=P->Count)return MaliInvalid;
    if(D->Initialized || D->Poisoned)return MaliBusy;
    if(!Io->Authorized(Io->Context,Initial,Initial))return MaliOffline;
    const MALI_MFG_OPP *E=&P->Entries[Initial];uint64_t Hz;
    MALI_RESULT R=MaliMfgRailsReady(Rails);if(R)return R;
    R=MaliMfgHwClockReady(Clock);if(R)return R;
    R=MaliMfgPllDecodeRate(Clock->PllSetting,&Hz);if(R)return R;
    if(Hz!=P->ActualHz[Initial] || Rails->Microvolts[0]!=E->VgpuUv || Rails->Microvolts[1]!=E->VsramUv)return MaliBusy;
    D->Policy=P;D->Rails=Rails;D->Clock=Clock;D->Io=*Io;
    D->Current=D->Target=Initial;D->Completed=0;D->Initialized=1;return MaliOk;
}
MALI_RESULT MaliMfgDvfsCommit(MALI_MFG_DVFS *D,uint32_t Target) {
    if(!D || !D->Initialized || !OppValid(D->Policy) || D->Current>=D->Policy->Count || Target>=D->Policy->Count)return MaliInvalid;
    if(D->Poisoned)return MaliFault;
    if(!D->Io.Authorized(D->Io.Context,D->Current,Target))return MaliOffline;
    MALI_RESULT R=DvfsAt(D,D->Current);if(R)return R;
    R=MaliMfgOppPlan(D->Policy,D->Current,Target,&D->Plan);if(R)return R;
    D->Target=Target;D->Completed=0;
    for(uint32_t I=0;I<D->Plan.Count;I++) {
        MALI_MFG_OPP_STEP *S=&D->Plan.Steps[I];
        if(S->Kind==MaliMfgOppVoltage)R=MaliMfgRailsVoltage(D->Rails,S->VgpuUv,S->VsramUv,&D->Io.Voltage);
        else {
            R=MaliMfgHwClockSet(D->Clock,MaliMfgHwBg3d,0);
            if(!R)R=MaliMfgHwClockParent(D->Clock,MaliMfgSubParent);
            if(!R)R=MaliMfgHwClockProgramRate(D->Clock,S->Hz,&D->Io.Rate);
            if(!R)R=MaliMfgHwClockParent(D->Clock,MaliMfgMainParent);
            if(!R)R=MaliMfgHwClockSet(D->Clock,MaliMfgHwBg3d,1);
        }
        if(R){D->Poisoned=1;return R;}
        D->Completed=I+1;
    }
    R=DvfsAt(D,Target);if(R){D->Poisoned=1;return R;}
    D->Current=Target;return MaliOk;
}
