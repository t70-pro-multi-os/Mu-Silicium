/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Mfg.h"
#ifdef MALI_MFG_TEST_NATIVE
#include <stdio.h>
#endif
unsigned MtkMaliMfgAssertionCount;
static unsigned Failure;
#define CHECK(X) do{++MtkMaliMfgAssertionCount;if(!(X)&&!Failure)Failure=__LINE__;}while(0)
static uint32_t Regs[2][1024],Writes,Reads,Delays,Barriers,Authorized,Stuck,LastRegion,LastOffset;
static uint32_t ReadMfg(void *X,uint32_t R,uint32_t O) {CHECK(X==Regs && R<2 && O<4096 && !(O&3));++Reads;return Regs[R][O/4];}
static void WriteMfg(void *X,uint32_t R,uint32_t O,uint32_t V) {
    CHECK(X==Regs && R<2 && O<4096 && !(O&3));++Writes;LastRegion=R;LastOffset=O;Regs[R][O/4]=V;
    if(R==0 && O>=0x308 && O<=0x314) {
        uint32_t N=(O-0x308)/4,Mask=1u<<(N+2);
        if(Stuck!=1) {for(unsigned I=0;I<2;I++)if((V&12)==12)Regs[0][(0x16c+I*4)/4]|=Mask;else Regs[0][(0x16c+I*4)/4]&=~Mask;}
        if(Stuck!=2){if(V&256)Regs[0][O/4]|=4096;else Regs[0][O/4]&=~4096;}
    }
    if(R==1 && Stuck!=3) {
        if(O==0x2a8){Regs[1][0x258/4]|=V;}
        if(O==0x2ac)Regs[1][0x258/4]&=~V;
        if(O==0x2a0){Regs[1][0x228/4]|=V;}
        if(O==0x2a4)Regs[1][0x228/4]&=~V;
        if(O==0x714){Regs[1][0x724/4]|=V;}
        if(O==0x718)Regs[1][0x724/4]&=~V;
    }
}
static void BarrierMfg(void *X){CHECK(X==Regs);++Barriers;}
static void DelayMfg(void *X,uint32_t N){CHECK(X==Regs && (N==10 || N==100));++Delays;}
static int Authority(void *X,uint32_t N,uint32_t On){CHECK(X==Regs && N<4 && On<2);return (int)Authorized;}
static void InitMfg(MALI_MFG *D,uint32_t OnMask,uint32_t Extra) {
    for(unsigned R=0;R<2;R++)for(unsigned I=0;I<1024;I++)Regs[R][I]=0;
    Writes=Reads=Delays=Barriers=Stuck=0;Authorized=1;
    for(unsigned I=0;I<4;I++)Regs[0][(0x308+4*I)/4]=(OnMask&(1u<<I)?13:0x1112)|Extra;
    Regs[0][0x16c/4]=Regs[0][0x170/4]=OnMask<<2;
    if(!(OnMask&2)){Regs[1][0x258/4]=0x200000;Regs[1][0x228/4]=0x600000;Regs[1][0x724/4]=0xe0;}
    MALI_MFG_IO Io={Regs,ReadMfg,WriteMfg,BarrierMfg,DelayMfg,Authority};CHECK(MaliMfgInitialize(D,&Io)==MaliOk);
}
static void Transitions(void) {
    MALI_MFG D;
    for(uint32_t I=0;I<128;I++) {
        uint32_t Extra=(I*0x9e3779b1u)&~0x111fu;InitMfg(&D,0,Extra);
        for(unsigned N=0;N<4;N++) {
            unsigned W=Writes;CHECK(MaliMfgSetDomain(&D,N,1)==MaliOk && Writes==W+6+(N==1?4:0));
            CHECK((Regs[0][(0x308+4*N)/4]&~0x111fu)==Extra);
            W=Writes;CHECK(MaliMfgSetDomain(&D,N,1)==MaliOk && Writes==W);
        }
        CHECK(MaliMfgAllDomainsReady(&D)==MaliOk && Barriers==Writes && Delays==4);
        for(unsigned N=4;N-->0;) {
            unsigned W=Writes;CHECK(MaliMfgSetDomain(&D,N,0)==MaliOk && Writes==W+6+(N==1?4:0));
            W=Writes;CHECK(MaliMfgSetDomain(&D,N,0)==MaliOk && Writes==W);
        }
        CHECK(!Regs[0][0x16c/4] && !Regs[0][0x170/4] && MaliMfgAllDomainsReady(&D)==MaliBusy);
    }
}
static void InvalidAndTimeout(void) {
    MALI_MFG D;InitMfg(&D,0,0);CHECK(MaliMfgSetDomain(&D,1,1)==MaliBusy && !Writes);
    CHECK(MaliMfgSetDomain(&D,4,1)==MaliInvalid && MaliMfgSetDomain(&D,0,2)==MaliInvalid);
    Authorized=0;unsigned R=Reads;CHECK(MaliMfgSetDomain(&D,0,1)==MaliOffline && Reads==R && !Writes);
    CHECK(MaliMfgAllDomainsReady(&D)==MaliOffline && Reads==R);Authorized=1;
    Regs[0][0x170/4]=4;CHECK(MaliMfgSetDomain(&D,0,1)==MaliBusy && !Writes);
    InitMfg(&D,15,0);CHECK(MaliMfgSetDomain(&D,0,0)==MaliBusy && !Writes);
    Regs[0][0x308/4]|=2;CHECK(MaliMfgAllDomainsReady(&D)==MaliBusy);
    InitMfg(&D,0,0);Stuck=1;CHECK(MaliMfgSetDomain(&D,0,1)==MaliTimeout && D.Poisoned && Writes==2 && Delays==MALI_MFG_WAIT_POLLS);
    unsigned W=Writes;CHECK(MaliMfgSetDomain(&D,0,0)==MaliFault && Writes==W && MaliMfgAllDomainsReady(&D)==MaliFault);
    InitMfg(&D,0,0);Stuck=2;CHECK(MaliMfgSetDomain(&D,0,1)==MaliTimeout && Writes==6 && Delays==MALI_MFG_WAIT_POLLS+1);
    InitMfg(&D,3,0);Stuck=3;CHECK(MaliMfgSetDomain(&D,1,0)==MaliTimeout && Writes==1 && LastRegion==1 && LastOffset==0x2a8);
    CHECK(Delays==MALI_MFG_WAIT_POLLS);InitMfg(&D,15,0);Regs[1][0x724/4]=0x20;
    CHECK(MaliMfgAllDomainsReady(&D)==MaliBusy);
}
static uint32_t CRegs[2][1024],CRead,CWrite,CDelay,CBarrier,COp,CFail,CAllow,CBad,CStatus,CDrop;
static uint32_t CPrepared,CEnabled,CParent,CCalls,CReleaseFail;
static uint32_t ReadClock(void *X,uint32_t R,uint32_t O) {
    CHECK(X==CRegs && R>=2 && R<=3 && O<4096 && !(O&3));++CRead;
    if(R==3 && O==0x50) {
        if(CStatus++>=CBad)CRegs[1][O/4]|=0x40000;
        else CRegs[1][O/4]&=~0x40000;
    }
    return CRegs[R-2][O/4];
}
static void WriteClock(void *X,uint32_t R,uint32_t O,uint32_t V) {
    CHECK(X==CRegs && R==2 && O<4096 && !(O&3));++CWrite;
    if(CWrite!=CDrop)CRegs[0][O/4]=V;
}
static void BarrierClock(void *X){CHECK(X==CRegs);++CBarrier;}
static void DelayClock(void *X,uint32_t N){CHECK(X==CRegs && N==10);++CDelay;}
static int AuthClock(void *X,uint32_t N,uint32_t On){CHECK(X==CRegs && N==3 && On<2);return (int)CAllow;}
static int PrepareClock(void *X,uint32_t N) {
    CHECK(X==CRegs && N<2 && !(CPrepared&(1u<<N)));++CCalls;
    if(++COp==CFail){return -1;}
    CPrepared|=1u<<N;return 0;
}
static int EnableClock(void *X,uint32_t N) {
    CHECK(X==CRegs && N<2 && (CPrepared&(1u<<N)) && !(CEnabled&(1u<<N)));++CCalls;
    if(++COp==CFail){return -1;}
    CEnabled|=1u<<N;return 0;
}
static int DisableClock(void *X,uint32_t N) {
    CHECK(X==CRegs && N<2 && (CEnabled&(1u<<N)));++CCalls;
    if(CReleaseFail&(N?1u:4u)){return -1;}
    CEnabled&=~(1u<<N);return 0;
}
static int UnprepareClock(void *X,uint32_t N) {
    CHECK(X==CRegs && N<2 && (CPrepared&(1u<<N)) && !(CEnabled&(1u<<N)));++CCalls;
    if(CReleaseFail&(N?2u:8u)){return -1;}
    CPrepared&=~(1u<<N);return 0;
}
static int ParentClock(void *X,uint32_t N) {
    CHECK(X==CRegs && N<2 && (CPrepared&1) && (CEnabled&1));++CCalls;
    if(++COp==CFail){return -1;}
    CParent=N;return 0;
}
static void InitClock(MALI_MFG_CLOCK *D,uint32_t Seed,uint32_t Bad) {
    MALI_MFG_CLOCK Zero={0};*D=Zero;
    for(unsigned R=0;R<2;R++)for(unsigned I=0;I<1024;I++)CRegs[R][I]=Seed*0x9e3779b1u;
    CRead=CWrite=CDelay=CBarrier=COp=CFail=CStatus=CDrop=CPrepared=CEnabled=CCalls=CReleaseFail=0;
    CAllow=1;CBad=Bad;CParent=MaliMfgSubParent;
    MALI_MFG_CLOCK_IO Io={{CRegs,ReadClock,WriteClock,BarrierClock,DelayClock,AuthClock},
        PrepareClock,EnableClock,DisableClock,UnprepareClock,ParentClock};
    CHECK(MaliMfgClockInitialize(D,&Io)==MaliOk);
}
static void Clocks(void) {
    MALI_MFG_CLOCK D;
    for(unsigned Seed=0;Seed<64;Seed++)for(unsigned Bad=0;Bad<7;Bad++) {
        InitClock(&D,Seed,Bad);uint32_t Original=Seed*0x9e3779b1u;
        CHECK(MaliMfgClockReady(&D)==MaliBusy && !CRead && !CWrite && !CCalls);
        CHECK(MaliMfgClockSet(&D,1)==MaliOk && D.Stages==MaliMfgClocksOn && !D.Poisoned);
        CHECK(CWrite==6 && CBarrier==6 && CEnabled==3 && CPrepared==3 && !CParent);
        CHECK(CDelay==(Bad==0?1:Bad-1) && COp==5+(Bad>1?Bad-1:0));
        CHECK(CRegs[0][0x20/4]==(Original|0x3c00000));
        CHECK(CRegs[0][0xb0/4]==((Original&~0x700u)|0x80000));
        CHECK(CRegs[0][0xb4/4]==(Original|0x10) && CRegs[0][0x24/4]==(Original|1) && CRegs[0][0xf60/4]==(Original|1));
        unsigned Calls=CCalls;CHECK(MaliMfgClockReady(&D)==MaliOk && MaliMfgClockSet(&D,1)==MaliOk && CCalls==Calls && CWrite==6);
        CHECK(MaliMfgClockSet(&D,0)==MaliOk && !D.Stages && !CEnabled && !CPrepared && CParent==1 && CWrite==6);
        Calls=CCalls;CHECK(MaliMfgClockSet(&D,0)==MaliOk && CCalls==Calls);
    }
    InitClock(&D,0,7);CHECK(MaliMfgClockSet(&D,1)==MaliTimeout && D.Poisoned && D.Stages==7 && CDelay==6 && COp==9 && !CWrite);
    unsigned Calls=CCalls,R=CRead;CHECK(MaliMfgClockSet(&D,0)==MaliFault && CCalls==Calls && CRead==R);
    CHECK(MaliMfgClockReady(&D)==MaliFault && MaliMfgClockInitialize(&D,&D.Io)==MaliBusy);
    for(unsigned F=1;F<=5;F++) {
        InitClock(&D,0,0);CFail=F;CHECK(MaliMfgClockSet(&D,1)==MaliFault && D.Poisoned && !CWrite);
        CHECK(CPrepared==(F==1 || F==2?0:1) && CEnabled==(F==1 || F==2?0:1));
        Calls=CCalls;CHECK(MaliMfgClockSet(&D,1)==MaliFault && CCalls==Calls);
    }
    InitClock(&D,0,0);CHECK(MaliMfgClockSet(&D,1)==MaliOk);CFail=COp+1;
    CHECK(MaliMfgClockSet(&D,0)==MaliFault && D.Poisoned && CEnabled==1 && CPrepared==1 && D.Stages==39);
    static const uint32_t LeftStages[4]={63,47,35,33};
    for(unsigned F=0;F<4;F++) {
        InitClock(&D,0,0);CHECK(MaliMfgClockSet(&D,1)==MaliOk);CReleaseFail=1u<<F;
        CHECK(MaliMfgClockSet(&D,0)==MaliFault && D.Poisoned && D.Stages==LeftStages[F]);
        CHECK(CEnabled==(F==0?3u:F<3?1u:0u) && CPrepared==(F<2?3u:1u));
        Calls=CCalls;CHECK(MaliMfgClockSet(&D,0)==MaliFault && CCalls==Calls);
    }
    /* Failed enable + failed unprepare must retain the prepared lease. */
    InitClock(&D,0,0);CFail=2;CReleaseFail=8;
    CHECK(MaliMfgClockSet(&D,1)==MaliFault && D.Stages==1 && CPrepared==1 && !CEnabled);
    InitClock(&D,0,0);CFail=5;CReleaseFail=2;
    CHECK(MaliMfgClockSet(&D,1)==MaliFault && D.Stages==15 && CPrepared==3 && CEnabled==1);
    InitClock(&D,0,0);CAllow=0;CHECK(MaliMfgClockSet(&D,1)==MaliOffline && !CRead && !CWrite && !CCalls);
    CHECK(MaliMfgClockReady(&D)==MaliOffline && !CRead);
    CHECK(MaliMfgClockSet(&D,2)==MaliInvalid && MaliMfgClockSet(0,0)==MaliInvalid);
    for(unsigned Drop=1;Drop<=6;Drop++) {
        /* All controlled bits initially oppose their target values. */
        InitClock(&D,0,0);CRegs[0][0xb0/4]=0x700;CDrop=Drop;
        CHECK(MaliMfgClockSet(&D,1)==MaliFault && D.Poisoned && CWrite==6);
    }
    InitClock(&D,0,0);CHECK(MaliMfgClockSet(&D,1)==MaliOk);CRegs[0][0xb0/4]|=0x100;
    CHECK(MaliMfgClockReady(&D)==MaliBusy);
}
static MALI_MFG_POWER *PDevice;
static uint32_t PRails,PPrepared,PEnabled,PAllowed,PAllowOff,POps,PFail,PIdleBad,PIdleReads,PDcmDrop,PStuck,PReleaseFail;
static uint32_t ReadPower(void *X,uint32_t R,uint32_t O) {
    CHECK(X==PDevice);
    if(R<2)return ReadMfg(Regs,R,O);
    if(R==MaliMfgTop && O==0x178) {
        ++CRead;return PIdleReads++>=PIdleBad?4:0;
    }
    return ReadClock(CRegs,R,O);
}
static void WritePower(void *X,uint32_t R,uint32_t O,uint32_t V) {
    CHECK(X==PDevice);
    if(R<2) {
        uint32_t Old=Stuck;
        if(R==0 && O==0x308+4*PStuck)Stuck=1;
        WriteMfg(Regs,R,O,V);Stuck=Old;
    } else if(R==MaliMfgTop && O==0x130 && PDcmDrop)++CWrite;
    else WriteClock(CRegs,R,O,V);
}
static void BarrierPower(void *X){CHECK(X==PDevice);++Barriers;}
static void DelayPower(void *X,uint32_t N){CHECK(X==PDevice && (N==10 || N==100));++Delays;}
static int AuthPowerDomain(void *X,uint32_t N,uint32_t On) {
    CHECK(X==PDevice && N<4 && On<2);
    return PRails==3 && (N<2 || PEnabled);
}
static int AuthPowerClock(void *X,uint32_t N,uint32_t On) {
    CHECK(X==PDevice && N==3 && On<2);
    return PRails==3 && PEnabled && (Regs[0][0x16c/4]&0x3c)==0x3c;
}
static int AuthPower(void *X,uint32_t On){CHECK(X==PDevice && On<2);return PAllowed && (On || PAllowOff);}
static int RailPower(void *X,uint32_t N,uint32_t On) {
    CHECK(X==PDevice && N<2 && On<2);++POps;
    if(On)CHECK(PRails==(N?1u:0u) && !PPrepared && !PEnabled && !(Regs[0][0x16c/4]&0x3c));
    else CHECK(PRails==(N?3u:1u) && !PPrepared && !PEnabled && !CPrepared && !CEnabled && !(Regs[0][0x16c/4]&0x3c));
    if(POps==PFail)return -1;
    if(On)PRails|=1u<<N;else PRails&=~(1u<<N);return 0;
}
static int PreparePower(void *X) {
    CHECK(X==PDevice && PRails==3 && !PPrepared && !PEnabled && (Regs[0][0x16c/4]&0x3c)==12);++POps;
    if(POps==PFail){return -1;}
    PPrepared=1;return 0;
}
static int EnablePower(void *X) {
    CHECK(X==PDevice && PPrepared && !PEnabled && (Regs[0][0x16c/4]&0x3c)==12);++POps;
    if(POps==PFail){return -1;}
    PEnabled=1;return 0;
}
static int DisablePower(void *X) {
    CHECK(X==PDevice && PPrepared && PEnabled && !CEnabled && (Regs[0][0x16c/4]&0x3c)==12);++POps;
    if(PReleaseFail&1){return -1;}
    PEnabled=0;return 0;
}
static int UnpreparePower(void *X) {
    CHECK(X==PDevice && PPrepared && !PEnabled && (Regs[0][0x16c/4]&0x3c)==12);++POps;
    if(PReleaseFail&2){return -1;}
    PPrepared=0;return 0;
}
static int PreparePowerClock(void *X,uint32_t N){CHECK(X==PDevice);return PrepareClock(CRegs,N);}
static int EnablePowerClock(void *X,uint32_t N){CHECK(X==PDevice);return EnableClock(CRegs,N);}
static int DisablePowerClock(void *X,uint32_t N){CHECK(X==PDevice);return DisableClock(CRegs,N);}
static int UnpreparePowerClock(void *X,uint32_t N){CHECK(X==PDevice);return UnprepareClock(CRegs,N);}
static int ParentPowerClock(void *X,uint32_t N){CHECK(X==PDevice);return ParentClock(CRegs,N);}
static void InitPower(MALI_MFG_POWER *D,uint32_t Seed) {
    MALI_MFG Temp;MALI_MFG_CLOCK CTemp;MALI_MFG_POWER Zero={0};*D=Zero;
    InitMfg(&Temp,0,(Seed*0x9e3779b1u)&~0x111fu);InitClock(&CTemp,Seed,Seed%7);
    PDevice=D;PRails=PPrepared=PEnabled=POps=PFail=PIdleReads=PDcmDrop=PReleaseFail=0;
    PAllowed=PAllowOff=1;PIdleBad=Seed%5;PStuck=4;
    MALI_MFG_IO DomainIo={D,ReadPower,WritePower,BarrierPower,DelayPower,AuthPowerDomain};
    MALI_MFG_IO ClockIo={D,ReadPower,WritePower,BarrierPower,DelayPower,AuthPowerClock};
    MALI_MFG_CLOCK_IO Cio={ClockIo,PreparePowerClock,EnablePowerClock,DisablePowerClock,UnpreparePowerClock,ParentPowerClock};
    MALI_MFG_POWER_IO Io={DomainIo,Cio,AuthPower,RailPower,PreparePower,EnablePower,DisablePower,UnpreparePower};
    CHECK(MaliMfgPowerInitialize(D,&Io)==MaliOk);
}
static void Power(void) {
    MALI_MFG_POWER D;
    for(uint32_t Seed=0;Seed<32;Seed++) {
        InitPower(&D,Seed);
        CHECK(MaliMfgPowerSequenceReady(&D)==MaliBusy && !POps && !CRead && !Reads);
        CHECK(MaliMfgPowerReference(&D,0)==MaliInvalid && !POps && !CRead && !Reads);
        CHECK(MaliMfgPowerReference(&D,1)==MaliOk && D.Stages==MaliMfgPowerOn && D.References==1 && !D.Poisoned);
        CHECK(PRails==3 && PPrepared && PEnabled && CPrepared==3 && CEnabled==3 && CParent==0 && POps==4);
        CHECK(MaliMfgPowerSequenceReady(&D)==MaliOk);
        uint32_t W=Writes+ CWrite,Ops=POps,Clocks=CCalls;
        CHECK(MaliMfgPowerReference(&D,1)==MaliOk && D.References==2 && POps==Ops && CCalls==Clocks && Writes+CWrite==W);
        PAllowOff=0;
        CHECK(MaliMfgPowerReference(&D,0)==MaliOk && D.References==1 && POps==Ops && CCalls==Clocks && Writes+CWrite==W);
        CHECK(MaliMfgPowerReference(&D,0)==MaliOffline && D.References==1 && Writes+CWrite==W);PAllowOff=1;
        D.References=UINT32_MAX;CHECK(MaliMfgPowerReference(&D,1)==MaliInvalid && Writes+CWrite==W);D.References=1;
        CHECK(MaliMfgPowerReference(&D,0)==MaliOk && !D.Stages && !D.References && !D.Poisoned);
        CHECK(!PRails && !PPrepared && !PEnabled && !CPrepared && !CEnabled && CParent==1 && POps==8);
        CHECK(!(Regs[0][0x16c/4]&0x3c) && PIdleReads==PIdleBad+1 && CRegs[0][0xb4/4]==1 && CRegs[0][0x170/4]==3);
        CHECK(MaliMfgPowerSequenceReady(&D)==MaliBusy && MaliMfgPowerInitialize(&D,&D.Io)==MaliBusy);
        CHECK(MaliMfgPowerReference(&D,1)==MaliOk && D.References==1);
        CHECK(MaliMfgPowerReference(&D,0)==MaliOk && !D.References);
    }
    for(unsigned F=1;F<=4;F++) {
        InitPower(&D,0);PFail=F;
        CHECK(MaliMfgPowerReference(&D,1)==MaliFault && D.Poisoned && !D.References);
        CHECK(PRails==(F==1?0u:F==2?1u:3u));
        CHECK(!PPrepared && !PEnabled && D.Stages==(F==1?0u:F==2?1u:15u));
        uint32_t W=Writes+CWrite;CHECK(MaliMfgPowerReference(&D,0)==MaliFault && Writes+CWrite==W);
        CHECK(MaliMfgPowerInitialize(&D,&D.Io)==MaliBusy);
    }
    for(unsigned F=1;F<=5;F++) {
        InitPower(&D,0);CFail=F;CHECK(MaliMfgPowerReference(&D,1)==MaliFault && D.Poisoned);
        CHECK(D.Stages==255 && PRails==3 && PEnabled && (Regs[0][0x16c/4]&0x3c)==0x3c);
    }
    for(unsigned N=0;N<4;N++) {
        InitPower(&D,0);PStuck=N;
        CHECK(MaliMfgPowerReference(&D,1)==MaliTimeout && D.Poisoned && D.Domains.Poisoned && PRails==3);
        CHECK(D.Stages==(N==0?3u:N==1?7u:N==2?63u:127u));
    }
    InitPower(&D,0);CBad=7;CHECK(MaliMfgPowerReference(&D,1)==MaliTimeout && D.Poisoned && D.Clocks.Poisoned && D.Stages==255);
    InitPower(&D,0);PDcmDrop=1;CRegs[0][0x130/4]=0;
    CHECK(MaliMfgPowerReference(&D,1)==MaliBusy && D.Poisoned && D.References==1 && D.Stages==MaliMfgPowerOn);
    InitPower(&D,0);CHECK(MaliMfgPowerReference(&D,1)==MaliOk);PIdleBad=MALI_MFG_WAIT_POLLS;
    CHECK(MaliMfgPowerReference(&D,0)==MaliTimeout && D.Poisoned && D.Stages==MaliMfgPowerOn && D.References==1 && CEnabled==3);
    CHECK(PIdleReads==MALI_MFG_WAIT_POLLS && PRails==3 && PEnabled);
    for(unsigned F=7;F<=8;F++) {
        InitPower(&D,0);CHECK(MaliMfgPowerReference(&D,1)==MaliOk);PFail=F;
        CHECK(MaliMfgPowerReference(&D,0)==MaliFault && D.Poisoned && D.References==1 && !PEnabled);
        CHECK(D.Stages==(F==7?3u:1u) && PRails==D.Stages);
    }
    for(unsigned F=0;F<2;F++) {
        InitPower(&D,0);CHECK(MaliMfgPowerReference(&D,1)==MaliOk);PReleaseFail=1u<<F;
        CHECK(MaliMfgPowerReference(&D,0)==MaliFault && D.Poisoned && D.References==1);
        CHECK(D.Stages==(F==0?63u:31u) && PRails==3 && PPrepared && PEnabled==(F==0));
        CHECK((Regs[0][0x16c/4]&0x3c)==12 && !CEnabled && !CPrepared);
    }
    InitPower(&D,0);PFail=4;PReleaseFail=2;
    CHECK(MaliMfgPowerReference(&D,1)==MaliFault && D.Stages==31 && PPrepared && !PEnabled && PRails==3);
    InitPower(&D,0);PAllowed=0;
    CHECK(MaliMfgPowerReference(&D,1)==MaliOffline && !POps && !Reads && !CRead);
    CHECK(MaliMfgPowerReference(&D,2)==MaliInvalid && MaliMfgPowerReference(0,0)==MaliInvalid);
    MALI_MFG_POWER_IO Bad=D.Io;MALI_MFG_POWER Empty={0};Bad.Clocks.Registers.Context=0;
    CHECK(MaliMfgPowerInitialize(&Empty,&Bad)==MaliInvalid && !Empty.Initialized && !Empty.Domains.Initialized);
    Bad=D.Io;Bad.ReferencePrepare=0;
    CHECK(MaliMfgPowerInitialize(&Empty,&Bad)==MaliInvalid && !Empty.Initialized);
    Bad=D.Io;Bad.Clocks.Prepare=0;
    CHECK(MaliMfgPowerInitialize(&Empty,&Bad)==MaliInvalid && !Empty.Initialized && !Empty.Domains.Initialized);
}
/* Independent virtual WACS2 endpoint: the production transport emits real
   commands; this endpoint supplies documented FSM/response and PMIC registers. */
static uint16_t BRegs[32768];
static uint32_t BFsm,BWdata,BResponse,BStuck,BReads,BWrites,BBarriers,BCommands,BFailCommand;
static uint32_t BAllowed,BRailAllowed,BTenDelays,BEnableDelays,BDropWrite,BFreezeStatus;
static uint32_t BRegisterWrites,BVoltageWrites,BVoltageTrace[2*MALI_MFG_OPP_MAX_STEPS][2],BFreezeVoltage,BLastDelay;
static MALI_MFG_PMIC Bus;
static MALI_MFG_RAILS Rails;
static uint32_t ReadBus(void *X,uint32_t O) {
    CHECK(X==BRegs && (O==0x8a8 || O==0x894));++BReads;
    return O==0x8a8?0xa5000000u|(BFsm<<1):0xbeef0000u|BResponse;
}
static void WriteBus(void *X,uint32_t O,uint32_t V) {
    CHECK(X==BRegs);++BWrites;
    if(O==0x884){CHECK(V<=65535);BWdata=V;return;}
    if(O==0x8a4){CHECK(V==1 && BFsm==6);if(BStuck!=3)BFsm=0;return;}
    CHECK(O==0x880 && !(V&0xdfff0001u));uint32_t A=V&65535u;++BCommands;
    CHECK(BFsm==0);
    if(BCommands==BFailCommand){BFsm=2;BStuck=1;return;}
    if(V&0x20000000u) {
        ++BRegisterWrites;
        if(!BDropWrite)BRegs[A/2]=(uint16_t)BWdata;
        if(A==0x1526 || A==0x1bf4) {
            CHECK(BVoltageWrites<2*MALI_MFG_OPP_MAX_STEPS);BVoltageTrace[BVoltageWrites][0]=A;
            BVoltageTrace[BVoltageWrites++][1]=BWdata;
            if(A==0x1bf4 && !BDropWrite && !BFreezeVoltage)
                BRegs[0x1be2/2]=(uint16_t)((BRegs[0x1be2/2]&~0x7f00u)|((BWdata&0x7f)<<8));
        }
        if(!BFreezeStatus && !BDropWrite) {
            if(A==0x1508)BRegs[0x1520/2]=(BRegs[0x1520/2]&~1u)|(BWdata&1);
            if(A==0x1bc8)BRegs[0x1be4/2]=(BRegs[0x1be4/2]&~1u)|(BWdata&1);
        }
        BFsm=BStuck==4?2:0;
    } else {BResponse=BRegs[A/2];BFsm=BStuck==2?2:6;}
}
static void BarrierBus(void *X){CHECK(X==BRegs);++BBarriers;}
static void DelayBus(void *X,uint32_t N) {
    CHECK(X==BRegs && N && N<=240);BLastDelay=N;
    if(N==10)++BTenDelays;else if(N==200 || N==240)BEnableDelays|=N==200?1u:2u;
}
static int AuthBus(void *X){CHECK(X==BRegs);return (int)BAllowed;}
static MALI_RESULT BrokerReadResult,BrokerWriteResult;
static uint32_t BrokerReads,BrokerWrites;
static MALI_RESULT BrokerRead16(void *X,uint32_t A,uint16_t *V) {
    CHECK(X==BRegs);++BrokerReads;
    if(!V || A>UINT16_MAX || (A&1))return MaliInvalid;
    if(BrokerReadResult==MaliOk)*V=BRegs[A/2];
    return BrokerReadResult;
}
static MALI_RESULT BrokerWrite16(void *X,uint32_t A,uint16_t V) {
    CHECK(X==BRegs);++BrokerWrites;
    if(A>UINT16_MAX || (A&1))return MaliInvalid;
    if(BrokerWriteResult==MaliOk)BRegs[A/2]=V;
    return BrokerWriteResult;
}
static int AuthRail(void *X,uint32_t N,uint32_t On){CHECK(X==BRegs && N<2 && On<2);return (int)BRailAllowed;}
static void InitBus(uint32_t Borrowed,uint32_t Seed) {
    BRegisterWrites=0;
    MALI_MFG_PMIC Empty={0};Bus=Empty;MALI_MFG_RAILS NoRails={0};Rails=NoRails;
    /* Initialize only registers used by this fixture; the generic transfer
       sweep sets its selected word explicitly. Avoid clearing 64 KiB per case. */
    BFsm=BWdata=BResponse=BStuck=BReads=BWrites=BBarriers=BCommands=BFailCommand=0;
    BTenDelays=BEnableDelays=BDropWrite=BFreezeStatus=BVoltageWrites=BFreezeVoltage=BLastDelay=0;BAllowed=BRailAllowed=1;
    BRegs[0x1508/2]=(uint16_t)((Seed*0x9e37u)&~1u)|(Borrowed&1u);
    BRegs[0x1bc8/2]=(uint16_t)((Seed*0xb529u)&~1u)|((Borrowed>>1)&1u);
    BRegs[0x1520/2]=Borrowed&1u;BRegs[0x1be4/2]=(Borrowed>>1)&1u;
    BRegs[0x1526/2]=(uint16_t)(0x9b80u|(Seed&127));
    BRegs[0x1bf4/2]=(uint16_t)(0xb780u|((Seed+17)&127));
    BRegs[0x1be2/2]=(uint16_t)(0x805au|(((Seed+17)&127)<<8));
    MALI_MFG_PMIC_IO Io={BRegs,ReadBus,WriteBus,BarrierBus,DelayBus,AuthBus,0,0};
    CHECK(MaliMfgPmicInitialize(&Bus,&Io)==MaliOk);
    MALI_MFG_RAIL_IO Rio={&Bus,BRegs,AuthRail};CHECK(MaliMfgRailsInitialize(&Rails,&Rio)==MaliOk);
}
static int ActualPowerRail(void *X,uint32_t N,uint32_t On) {
    CHECK(X==PDevice && N<2 && On<2);
    MALI_RESULT R=MaliMfgRailSet(&Rails,N,On);
    if(!R){if(On)PRails|=1u<<N;else PRails&=~(1u<<N);}return (int)R;
}
static void Pmic(void) {
    uint16_t V=0xa55a;
    for(uint32_t I=0;I<128;I++) {
        InitBus(0,I);uint32_t A=I*2;BRegs[A/2]=(uint16_t)(I*0x1234u);
        CHECK(MaliMfgPmicRead16(&Bus,A,&V)==MaliOk && V==(uint16_t)(I*0x1234u));
        CHECK(MaliMfgPmicWrite16(&Bus,A,65535u-I)==MaliOk && BRegs[A/2]==65535u-I && !BFsm);
        CHECK(BWrites==BBarriers && BCommands==2 && !BTenDelays);
    }
    InitBus(0,0);BAllowed=0;
    CHECK(MaliMfgPmicRead16(&Bus,2,&V)==MaliOffline && !BReads && !BWrites);
    CHECK(MaliMfgPmicWrite16(&Bus,2,1)==MaliOffline && !BReads && !BWrites);
    CHECK(MaliMfgPmicRead16(&Bus,1,&V)==MaliInvalid && MaliMfgPmicRead16(&Bus,65536,&V)==MaliInvalid);
    CHECK(MaliMfgPmicRead16(&Bus,2,0)==MaliInvalid && MaliMfgPmicWrite16(&Bus,2,65536)==MaliInvalid);
    CHECK(!BReads && !BWrites);BAllowed=1;BFsm=6;
    CHECK(MaliMfgPmicRead16(&Bus,2,&V)==MaliOk && !BFsm && BWrites==3 && BBarriers==3);
    CHECK(MaliMfgPmicInitialize(&Bus,&Bus.Io)==MaliBusy);
    for(uint32_t F=1;F<=4;F++) {
        InitBus(0,0);BStuck=F;if(F==1)BFsm=2;V=0xa55a;
        MALI_RESULT R=F==4?MaliMfgPmicWrite16(&Bus,2,0x1234):MaliMfgPmicRead16(&Bus,2,&V);
        CHECK(R==MaliTimeout && Bus.Poisoned && BTenDelays==MALI_MFG_PMIC_WAIT_POLLS && V==0xa55a);
        uint32_t W=BWrites,Q=BReads;
        CHECK(MaliMfgPmicRead16(&Bus,2,&V)==MaliFault && BWrites==W && BReads==Q);
        CHECK(MaliMfgPmicInitialize(&Bus,&Bus.Io)==MaliBusy);
    }
    for(uint32_t Seed=0;Seed<128;Seed++)for(uint32_t Borrowed=0;Borrowed<4;Borrowed++) {
        InitBus(Borrowed,Seed);uint16_t A=BRegs[0x1508/2],B=BRegs[0x1bc8/2];
        CHECK(MaliMfgRailsReady(&Rails)==MaliBusy && !BReads && !BWrites);
        CHECK(MaliMfgRailSet(&Rails,1,1)==MaliBusy && !BReads && !BWrites);
        CHECK(MaliMfgRailSet(&Rails,0,1)==MaliOk && Rails.Acquired==1 && Rails.Owned==((Borrowed&1)?0u:1u));
        CHECK(Rails.Microvolts[0]==500000u+6250u*(Seed&127));
        CHECK(MaliMfgRailSet(&Rails,1,1)==MaliOk && Rails.Acquired==3 && Rails.Owned==(Borrowed^3));
        CHECK(Rails.Microvolts[1]==500000u+6250u*((Seed+17)&127));
        CHECK(BEnableDelays==(Borrowed^3) && !BTenDelays && MaliMfgRailsReady(&Rails)==MaliOk);
        CHECK(BRegs[0x1508/2]==(uint16_t)(A|1) && BRegs[0x1bc8/2]==(uint16_t)(B|1));
        uint32_t Q=BCommands;
        CHECK(MaliMfgRailSet(&Rails,0,0)==MaliBusy && BCommands==Q);
        CHECK(MaliMfgRailSet(&Rails,0,1)==MaliOk && Rails.Acquired==3 && Rails.Owned==(Borrowed^3));
        CHECK(MaliMfgRailsInitialize(&Rails,&Rails.Io)==MaliBusy);
        CHECK(MaliMfgRailSet(&Rails,1,0)==MaliOk && Rails.Acquired==1);
        CHECK(MaliMfgRailSet(&Rails,0,0)==MaliOk && !Rails.Acquired && !Rails.Owned);
        CHECK(BRegs[0x1508/2]==A && BRegs[0x1bc8/2]==B && !Rails.Microvolts[0] && !Rails.Microvolts[1]);
        Q=BCommands;CHECK(MaliMfgRailSet(&Rails,0,0)==MaliOk && BCommands==Q && BWrites==BBarriers);
    }
    InitBus(0,0);BRailAllowed=0;
    CHECK(MaliMfgRailSet(&Rails,0,1)==MaliOffline && !BReads && !BWrites);
    CHECK(MaliMfgRailSet(&Rails,2,1)==MaliInvalid && MaliMfgRailSet(&Rails,0,2)==MaliInvalid);
    InitBus(0,0);BDropWrite=1;
    CHECK(MaliMfgRailSet(&Rails,0,1)==MaliTimeout && Rails.Poisoned && Rails.Acquired==1 && Rails.Owned==1);
    CHECK(!Bus.Poisoned && BTenDelays==MALI_MFG_PMIC_WAIT_POLLS);
    uint32_t W=BWrites;CHECK(MaliMfgRailSet(&Rails,0,0)==MaliFault && MaliMfgRailsReady(&Rails)==MaliFault && BWrites==W);
    InitBus(0,0);BRegs[0x1be2/2]^=256;
    CHECK(MaliMfgRailSet(&Rails,0,1)==MaliOk && MaliMfgRailSet(&Rails,1,1)==MaliBusy);
    CHECK(Rails.Poisoned && Rails.Owned==3 && Rails.Acquired==3);
    for(uint32_t F=1;F<=8;F++) {
        InitBus(0,0);BFailCommand=F;
        CHECK(MaliMfgRailSet(&Rails,0,1)==MaliTimeout && Bus.Poisoned && Rails.Poisoned);
        CHECK(BCommands==F && Rails.Acquired==(F<3?0u:1u) && Rails.Owned==(F<3?0u:1u));
    }
    InitBus(0,0);CHECK(MaliMfgRailSet(&Rails,0,1)==MaliOk && MaliMfgRailSet(&Rails,1,1)==MaliOk);
    BRegs[0x1526/2]^=1;CHECK(MaliMfgRailsReady(&Rails)==MaliBusy);BRegs[0x1526/2]^=1;
    BRailAllowed=0;W=BWrites;uint32_t Q=BReads;
    CHECK(MaliMfgRailsReady(&Rails)==MaliOffline && BWrites==W && BReads==Q);BRailAllowed=1;
    BFreezeStatus=1;CHECK(MaliMfgRailSet(&Rails,1,0)==MaliTimeout && Rails.Poisoned && Rails.Owned==3 && Rails.Acquired==3);
    for(uint32_t Borrowed=0;Borrowed<4;Borrowed++) {
        MALI_MFG_POWER D;InitPower(&D,0);InitBus(Borrowed,0);D.Io.RailSet=ActualPowerRail;
        CHECK(MaliMfgPowerReference(&D,1)==MaliOk && MaliMfgRailsReady(&Rails)==MaliOk && D.References==1);
        CHECK(MaliMfgPowerSequenceReady(&D)==MaliOk && Rails.Owned==(Borrowed^3));
        CHECK(MaliMfgPowerReference(&D,0)==MaliOk && !D.References && !Rails.Acquired && !Rails.Owned && !PRails);
        CHECK((BRegs[0x1508/2]&1)==(Borrowed&1) && (BRegs[0x1bc8/2]&1)==((Borrowed>>1)&1));
    }
}
static void PmicBroker(void) {
    MALI_MFG_PMIC Device={0};MALI_MFG_PMIC_IO Io={0};uint16_t Value=0xa55a;
    InitBus(0,0);BRegs[1]=0x1234;BrokerReadResult=BrokerWriteResult=MaliOk;BrokerReads=BrokerWrites=0;
    Io.Context=BRegs;Io.DelayUs=DelayBus;Io.Authorized=AuthBus;Io.BrokerRead16=BrokerRead16;Io.BrokerWrite16=BrokerWrite16;
    CHECK(MaliMfgPmicInitialize(&Device,&Io)==MaliOk);
    CHECK(MaliMfgPmicRead16(&Device,2,&Value)==MaliOk && Value==0x1234 && BrokerReads==1);
    CHECK(MaliMfgPmicWrite16(&Device,2,0x4321)==MaliOk && BRegs[1]==0x4321 && BrokerWrites==1);
    CHECK(!BReads && !BWrites && !BBarriers);
    BrokerReadResult=MaliBusy;Value=0xa55a;
    CHECK(MaliMfgPmicRead16(&Device,2,&Value)==MaliBusy && Value==0xa55a && !Device.Poisoned);
    BrokerReadResult=MaliOffline;
    CHECK(MaliMfgPmicRead16(&Device,2,&Value)==MaliOffline && Value==0xa55a && !Device.Poisoned);
    BrokerReadResult=MaliInvalid;
    CHECK(MaliMfgPmicRead16(&Device,2,&Value)==MaliInvalid && Value==0xa55a && !Device.Poisoned);
    BrokerWriteResult=MaliUnsupported;
    CHECK(MaliMfgPmicWrite16(&Device,2,1)==MaliUnsupported && !Device.Poisoned);
    BrokerReadResult=MaliFault;
    CHECK(MaliMfgPmicRead16(&Device,2,&Value)==MaliFault && Device.Poisoned && Value==0xa55a);
    CHECK(MaliMfgPmicWrite16(&Device,2,1)==MaliFault && BrokerWrites==2 && !BReads && !BWrites);

    MALI_MFG_PMIC Timed={0};CHECK(MaliMfgPmicInitialize(&Timed,&Io)==MaliOk);
    BrokerReadResult=MaliTimeout;Value=0xa55a;
    CHECK(MaliMfgPmicRead16(&Timed,2,&Value)==MaliTimeout && Timed.Poisoned && Value==0xa55a);

    MALI_MFG_PMIC Fresh={0};MALI_MFG_PMIC_IO Bad=Io;Bad.BrokerWrite16=0;
    CHECK(MaliMfgPmicInitialize(&Fresh,&Bad)==MaliInvalid && !Fresh.Initialized);
    Bad=Io;Bad.Read32=ReadBus;Bad.Write32=WriteBus;Bad.Barrier=BarrierBus;
    CHECK(MaliMfgPmicInitialize(&Fresh,&Bad)==MaliInvalid && !Fresh.Initialized);
    Bad=Io;Bad.BrokerRead16=0;Bad.BrokerWrite16=0;Bad.Read32=ReadBus;Bad.Write32=WriteBus;Bad.Barrier=BarrierBus;
    CHECK(MaliMfgPmicInitialize(&Fresh,&Bad)==MaliOk);
    CHECK(MaliMfgPmicRead16(&Fresh,2,&Value)==MaliOk && BReads && BWrites);
}
/* SET/CLR MMIO model independent of the production provider; only the seven
   used register words are initialized so the ARM64 test stays bounded. */
static uint32_t HRegs[3][1024],HReads,HWrites,HBarriers,HDelay1,HDelay20,HDrop,HAllowed;
static uint32_t HTrace[128][3];
static MALI_MFG_HW_CLOCK Hardware;
static uint32_t ReadHardware(void *X,uint32_t R,uint32_t O) {
    CHECK(X==HRegs && R>=2 && R<=4 && O<4096 && !(O&3));++HReads;return HRegs[R-2][O/4];
}
static void WriteHardware(void *X,uint32_t R,uint32_t O,uint32_t V) {
    CHECK(X==HRegs && R>=2 && R<=4 && O<4096 && !(O&3) && HWrites<128);
    HTrace[HWrites][0]=R;HTrace[HWrites][1]=O;HTrace[HWrites][2]=V;++HWrites;
    if(HWrites==HDrop)return;
    if(R==2){CHECK((O==4 || O==8) && V==1);if(O==4)HRegs[0][0]|=V;else HRegs[0][0]&=~V;}
    else if(R==3){CHECK(O==0x54 || O==0x58);if(O==0x54)HRegs[1][0x50/4]|=V;else HRegs[1][0x50/4]&=~V;}
    else {CHECK(O==0x268 || O==0x274 || O==0x26c);HRegs[2][O/4]=V;}
}
static void BarrierHardware(void *X){CHECK(X==HRegs);++HBarriers;}
static void DelayHardware(void *X,uint32_t N){CHECK(X==HRegs && (N==1 || N==20));if(N==1)++HDelay1;else ++HDelay20;}
static int AuthHardware(void *X,uint32_t N,uint32_t On){CHECK(X==HRegs && N<4 && On<2);return (int)HAllowed;}
static void InitHardware(uint32_t Seed,uint32_t Ref,uint32_t Div) {
    MALI_MFG_HW_CLOCK Empty={0};Hardware=Empty;HReads=HWrites=HBarriers=HDelay1=HDelay20=HDrop=0;HAllowed=1;
    uint32_t V=Seed*0x9e3779b1u;
    HRegs[0][0]=V|1;HRegs[1][0x50/4]=(V&~0x870000u)|(Ref<<16)|0x800000u;
    HRegs[2][0x268/4]=V&~1u;HRegs[2][0x274/4]=(V&~3u)|2;
    HRegs[2][0x26c/4]=(V&~0x073fffffu)|(Div<<24)|1260307u;
    HRegs[2][0x340/4]=V|0xff800001u;HRegs[2][0x34c/4]=(V&~3u)|1;
    MALI_MFG_HW_CLOCK_IO Io={HRegs,ReadHardware,WriteHardware,BarrierHardware,DelayHardware,AuthHardware};
    CHECK(MaliMfgHwClockInitialize(&Hardware,&Io)==MaliOk);
}
static void HardwareClocks(void) {
    uint64_t Hz;
    for(uint32_t P=0;P<128;P++)for(uint32_t D=0;D<8;D++) {
        uint32_t Pcw=P*33026u,Setting=0xf8c00000u|(D<<24)|Pcw;
        uint64_t Vco=(UINT64_C(26000000)*Pcw+16383)>>14;
        CHECK(MaliMfgPllDecodeRate(Setting,&Hz)==MaliOk && Hz==(Vco+((1u<<D)-1))/(1u<<D));
    }
    CHECK(MaliMfgPllDecodeRate(0,0)==MaliInvalid);
    for(uint32_t Seed=0;Seed<16;Seed++)for(uint32_t Ref=0;Ref<4;Ref++)for(uint32_t Div=0;Div<=4;Div++) {
        InitHardware(Seed,Ref,Div);uint32_t V=Seed*0x9e3779b1u,C=HRegs[2][0x26c/4];
        CHECK(MaliMfgHwClockReady(&Hardware)==MaliBusy && !HReads && !HWrites);
        CHECK(MaliMfgHwClockSet(&Hardware,1,1)==MaliBusy && !HWrites);
        CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk && Hardware.Held==1);
        CHECK(MaliMfgHwClockParent(&Hardware,0)==MaliOk && Hardware.Held==3);
        CHECK(MaliMfgHwClockSet(&Hardware,2,1)==MaliOk && Hardware.Held==7);
        CHECK(MaliMfgHwClockReady(&Hardware)==MaliOk && HWrites==7 && HBarriers==7 && HDelay1==2 && HDelay20==1);
        CHECK(HRegs[2][0x26c/4]==C && HRegs[2][0x268/4]==(V|1) && HRegs[2][0x274/4]==((V&~3u)|1));
        CHECK(HRegs[1][0x50/4]==((V&~0x870000u)|(Ref<<16)|0x40000u) && HRegs[0][0]==(V&~1u));
        uint32_t W=HWrites;
        CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk && MaliMfgHwClockSet(&Hardware,1,1)==MaliOk);
        CHECK(MaliMfgHwClockSet(&Hardware,2,1)==MaliOk && HWrites==W);
        CHECK(MaliMfgHwClockSet(&Hardware,0,0)==MaliBusy && MaliMfgHwClockSet(&Hardware,1,0)==MaliBusy);
        CHECK(MaliMfgHwClockParent(&Hardware,1)==MaliBusy && HWrites==W);
        CHECK(MaliMfgHwClockSet(&Hardware,2,0)==MaliOk && Hardware.Held==3);
        CHECK(MaliMfgHwClockParent(&Hardware,1)==MaliOk && Hardware.Held==1);
        CHECK(MaliMfgHwClockSet(&Hardware,0,0)==MaliOk && !Hardware.Held && !Hardware.Poisoned);
        CHECK(HWrites==14 && HBarriers==14 && HRegs[2][0x26c/4]==C);
        CHECK(HRegs[2][0x268/4]==(V&~1u) && HRegs[2][0x274/4]==((V&~3u)|2));
        CHECK(HRegs[1][0x50/4]==((V&~0x870000u)|(Ref<<16)|0x800000u) && HRegs[0][0]==(V|1));
        W=HWrites;CHECK(MaliMfgHwClockSet(&Hardware,0,0)==MaliOk && HWrites==W);
        CHECK(MaliMfgHwClockInitialize(&Hardware,&Hardware.Io)==MaliBusy);
    }
    InitHardware(0,0,0);HAllowed=0;
    CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOffline && MaliMfgHwClockParent(&Hardware,0)==MaliOffline && !HReads && !HWrites);
    CHECK(MaliMfgHwClockSet(&Hardware,3,1)==MaliInvalid && MaliMfgHwClockSet(&Hardware,0,2)==MaliInvalid);
    CHECK(MaliMfgHwClockSet(0,0,1)==MaliInvalid && MaliMfgHwClockParent(&Hardware,2)==MaliInvalid && !HReads);
    for(uint32_t Ref=1;Ref<4;Ref++)for(uint32_t Bad=0;Bad<2;Bad++) {
        InitHardware(0,Ref,0);HRegs[2][(Bad?0x34c:0x340)/4]=0;
        CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOffline && !Hardware.Held && !HWrites);
    }
    for(uint32_t Bad=0;Bad<3;Bad++) {
        InitHardware(0,0,0);CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk);
        HRegs[2][0x26c/4]=Bad==0?0:Bad==1?0x3fffffu:0x05000000u|1260307u;
        CHECK(MaliMfgHwClockParent(&Hardware,0)==MaliUnsupported && Hardware.Held==1 && HWrites==1 && !Hardware.Poisoned);
    }
    /* Lost writes retain the acquired clock for explicit board recovery. */
    InitHardware(0,0,0);HDrop=1;
    CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliFault && Hardware.Poisoned && Hardware.Held==1);
    for(uint32_t Drop=1;Drop<=3;Drop++) {
        InitHardware(0,0,0);CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk);HDrop=HWrites+Drop;
        CHECK(MaliMfgHwClockSet(&Hardware,1,1)==MaliFault && Hardware.Poisoned && Hardware.Held==3);
    }
    InitHardware(0,0,0);CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk);HDrop=6;
    CHECK(MaliMfgHwClockParent(&Hardware,0)==MaliFault && Hardware.Poisoned && Hardware.Held==3);
    InitHardware(0,0,0);CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk && MaliMfgHwClockParent(&Hardware,0)==MaliOk);HDrop=HWrites+1;
    CHECK(MaliMfgHwClockSet(&Hardware,2,1)==MaliFault && Hardware.Poisoned && Hardware.Held==7);
    for(uint32_t Drop=1;Drop<=3;Drop++) {
        InitHardware(0,0,0);CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk && MaliMfgHwClockSet(&Hardware,1,1)==MaliOk);
        HDrop=HWrites+Drop;
        CHECK(MaliMfgHwClockSet(&Hardware,1,0)==MaliFault && Hardware.Poisoned && Hardware.Held==3);
    }
    InitHardware(0,0,0);CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk);HDrop=HWrites+1;
    CHECK(MaliMfgHwClockSet(&Hardware,0,0)==MaliFault && Hardware.Poisoned && Hardware.Held==1);
    InitHardware(0,0,0);CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk && MaliMfgHwClockParent(&Hardware,0)==MaliOk);
    CHECK(MaliMfgHwClockSet(&Hardware,2,1)==MaliOk);HDrop=HWrites+1;
    CHECK(MaliMfgHwClockSet(&Hardware,2,0)==MaliFault && Hardware.Poisoned && Hardware.Held==7);
    uint32_t W=HWrites,Q=HReads;
    CHECK(MaliMfgHwClockSet(&Hardware,2,0)==MaliFault && MaliMfgHwClockReady(&Hardware)==MaliFault);
    CHECK(MaliMfgHwClockParent(&Hardware,1)==MaliFault && HWrites==W && HReads==Q);
    InitHardware(0,0,0);CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk && MaliMfgHwClockParent(&Hardware,0)==MaliOk);
    HDrop=HWrites+1;CHECK(MaliMfgHwClockParent(&Hardware,1)==MaliFault && Hardware.Held==3 && Hardware.Poisoned);
    InitHardware(0,0,0);CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk && MaliMfgHwClockParent(&Hardware,0)==MaliOk && MaliMfgHwClockSet(&Hardware,2,1)==MaliOk);
    HRegs[2][0x26c/4]^=1;CHECK(MaliMfgHwClockReady(&Hardware)==MaliBusy);
    HRegs[2][0x26c/4]^=1;HRegs[1][0x50/4]^=0x10000;CHECK(MaliMfgHwClockReady(&Hardware)==MaliBusy);
    HAllowed=0;W=HWrites;Q=HReads;CHECK(MaliMfgHwClockReady(&Hardware)==MaliOffline && HWrites==W && HReads==Q);
}
/* OPP authorization is an explicit fake board policy. It rejects unsafe
   intermediate pairs before the production core emits any PMIC command. */
static uint32_t VAllowed,VMaxGap,VAuthCalls,RateAllowed,RateAuthCalls;
static int AuthVoltage(void *X,uint32_t Gpu,uint32_t Sram) {
    CHECK(X==&Rails);++VAuthCalls;
    return VAllowed && Sram>=Gpu && Sram-Gpu<=VMaxGap;
}
static int AuthRate(void *X,uint64_t Old,uint64_t Requested,uint64_t Actual) {
    CHECK(X==&Hardware && Old && Requested>=93750000 && Actual>=93750000);
    ++RateAuthCalls;return (int)RateAllowed;
}
static void InitVoltage(uint32_t Gpu,uint32_t Sram,uint32_t Borrowed) {
    InitBus(Borrowed,0);VAllowed=1;VMaxGap=200000;VAuthCalls=0;
    BRegs[0x1526/2]=(uint16_t)(0x9b80u|((Gpu-500000)/6250));
    BRegs[0x1bf4/2]=(uint16_t)(0xb780u|((Sram-500000)/6250));
    BRegs[0x1be2/2]=(uint16_t)(0x805au|(((Sram-500000)/6250)<<8));
    CHECK(MaliMfgRailSet(&Rails,0,1)==MaliOk && MaliMfgRailSet(&Rails,1,1)==MaliOk);
    BLastDelay=0;
}
static void VoltagePairs(void) {
    const MALI_MFG_VOLTAGE_IO Io={&Rails,AuthVoltage};
    const uint32_t Values[][2]={{675000,750000},{687500,750000},{700000,750000},
        {750000,750000},{756250,756250},{825000,825000},{900000,900000}};
    for(uint32_t Borrowed=0;Borrowed<4;Borrowed++)for(uint32_t A=0;A<7;A++)for(uint32_t B=0;B<7;B++) {
        uint32_t Gpu=Values[B][0],Sram=Values[B][1],OldGpu=Values[A][0],OldSram=Values[A][1];
        InitVoltage(OldGpu,OldSram,Borrowed);
        uint32_t MiddleGpu=Gpu>OldGpu?OldGpu:Gpu,MiddleSram=Gpu>OldGpu?Sram:OldSram;
        uint32_t Q=BCommands;
        MALI_RESULT R=MaliMfgRailsVoltage(&Rails,Gpu,Sram,&Io);
        if(MiddleSram-MiddleGpu>VMaxGap) {
            CHECK(R==MaliOffline && BCommands==Q && !BVoltageWrites && !Rails.Poisoned);continue;
        }
        CHECK(R==MaliOk && VAuthCalls==3 && !Rails.Poisoned && Rails.Acquired==3 && Rails.Owned==(Borrowed^3));
        CHECK(Rails.Microvolts[0]==Gpu && Rails.Microvolts[1]==Sram && MaliMfgRailsReady(&Rails)==MaliOk);
        CHECK(BRegs[0x1526/2]==(uint16_t)(0x9b80|((Gpu-500000)/6250)));
        CHECK(BRegs[0x1bf4/2]==(uint16_t)(0xb780|((Sram-500000)/6250)));
        CHECK((BRegs[0x1be2/2]&0x80ff)==0x805a);
        if(A==B)CHECK(!BVoltageWrites && !BLastDelay);
        else {
            uint32_t First=Gpu>OldGpu?0x1bf4u:0x1526u;
            CHECK(BVoltageWrites==2 && BVoltageTrace[0][0]==First && BVoltageTrace[1][0]!=(uint32_t)First);
            uint32_t Dg=Gpu>OldGpu?(Gpu-OldGpu)/8000+9:(OldGpu-Gpu)/4000+9;
            uint32_t Ds=Gpu>OldGpu?(Sram-OldSram)/8000+8:(OldSram-Sram)/4000+8;
            CHECK(BLastDelay==(Dg>Ds?Dg:Ds));
        }
        if(((Borrowed&1) && Gpu!=OldGpu) || ((Borrowed&2) && Sram!=OldSram)) {
            uint32_t Before=BCommands;
            CHECK(MaliMfgRailsReleaseReady(&Rails)==MaliBusy && MaliMfgRailSet(&Rails,1,0)==MaliBusy &&
                BCommands==Before && Rails.Acquired==3 && Rails.Owned==(Borrowed^3) && !Rails.Poisoned);
            CHECK(MaliMfgRailsVoltage(&Rails,OldGpu,OldSram,&Io)==MaliOk);
        }
        CHECK(MaliMfgRailSet(&Rails,1,0)==MaliOk && MaliMfgRailSet(&Rails,0,0)==MaliOk);
        CHECK(!Rails.Acquired && !Rails.Owned && !Rails.OriginalMicrovolts[0] && !Rails.OriginalMicrovolts[1] &&
            (BRegs[0x1508/2]&1)==(Borrowed&1));
    }
    InitVoltage(700000,750000,0);uint32_t Q=BCommands;
    CHECK(MaliMfgRailsVoltage(&Rails,499999,750000,&Io)==MaliInvalid && BCommands==Q);
    CHECK(MaliMfgRailsVoltage(&Rails,700001,750000,&Io)==MaliInvalid && BCommands==Q);
    CHECK(MaliMfgRailsVoltage(&Rails,1293751,1293750,&Io)==MaliInvalid && BCommands==Q);
    CHECK(MaliMfgRailsVoltage(&Rails,706250,743750,&Io)==MaliInvalid && BCommands==Q);
    CHECK(MaliMfgRailsVoltage(&Rails,700000,756250,&Io)==MaliInvalid && BCommands==Q);
    CHECK(MaliMfgRailsVoltage(&Rails,706250,750000,0)==MaliInvalid && BCommands==Q);
    VAllowed=0;CHECK(MaliMfgRailsVoltage(&Rails,706250,750000,&Io)==MaliOffline && BCommands==Q);
    VAllowed=1;VMaxGap=50000;CHECK(MaliMfgRailsVoltage(&Rails,900000,900000,&Io)==MaliOffline && BCommands==Q);
    for(uint32_t F=1;F<=20;F++) {
        InitVoltage(700000,750000,0);Q=BCommands;BFailCommand=Q+F;
        CHECK(MaliMfgRailsVoltage(&Rails,756250,756250,&Io)==MaliTimeout);
        CHECK(Bus.Poisoned && Rails.Acquired==3 && Rails.Owned==3 && BCommands==Q+F);
        CHECK(Rails.Microvolts[0]==700000 && Rails.Microvolts[1]==750000);
        Q=BCommands;CHECK(MaliMfgRailsVoltage(&Rails,700000,750000,&Io)==MaliFault && BCommands==Q);
    }
    for(uint32_t F=0;F<2;F++) {
        InitVoltage(700000,750000,3);BDropWrite=F==0;BFreezeVoltage=F==1;
        CHECK(MaliMfgRailsVoltage(&Rails,756250,756250,&Io)==MaliBusy && Rails.Poisoned);
        CHECK(Rails.Microvolts[0]==700000 && Rails.Microvolts[1]==750000 && Rails.Acquired==3 && !Rails.Owned);
        Q=BCommands;CHECK(MaliMfgRailSet(&Rails,1,0)==MaliFault && BCommands==Q);
    }
    InitBus(0,0);Q=BCommands;
    CHECK(MaliMfgRailsVoltage(&Rails,700000,750000,&Io)==MaliBusy && BCommands==Q);
}
static void ProgramRates(void) {
    const MALI_MFG_RATE_IO Io={&Hardware,AuthRate};
    const uint64_t Rates[]={390000000,948000000,962000000,1100000000,93750000,
        187500000,375000000,750000000,1500000000,2600000000,3799999000};
    RateAllowed=1;RateAuthCalls=0;
    for(uint32_t Seed=0;Seed<16;Seed++)for(uint32_t I=0;I<sizeof(Rates)/sizeof(Rates[0]);I++) {
        uint32_t Setting=0;uint64_t Actual=0,Requested=Rates[I];
        CHECK(MaliMfgPllEncodeRate(Requested,&Setting,&Actual)==MaliOk);
        CHECK(Actual+1600>=Requested && Actual<=Requested+1600 && (Setting>>24)<=4);
        uint64_t Decoded;CHECK(MaliMfgPllDecodeRate(Setting,&Decoded)==MaliOk && Decoded==Actual);
        if(I==0)CHECK(Setting==0x020f0000u && Actual==390000000);
        InitHardware(Seed,Seed%4,1);CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk);
        uint32_t Old=HRegs[2][0x26c/4],W=HWrites;
        CHECK(MaliMfgHwClockProgramRate(&Hardware,Requested,&Io)==MaliOk && Hardware.Held==1 && !Hardware.Poisoned);
        CHECK(HWrites==W+2 && HDelay20==1 && HBarriers==HWrites && Hardware.PllSetting==Setting);
        CHECK(HRegs[2][0x26c/4]==((Old&~0x073fffffu)|Setting|0x80000000u));
        W=HWrites;CHECK(MaliMfgHwClockProgramRate(&Hardware,Requested,&Io)==MaliOk && HWrites==W);
        CHECK(MaliMfgHwClockParent(&Hardware,0)==MaliOk && MaliMfgHwClockSet(&Hardware,2,1)==MaliOk);
        CHECK(MaliMfgHwClockReady(&Hardware)==MaliOk && Hardware.PllSetting==Setting);
        W=HWrites;CHECK(MaliMfgHwClockProgramRate(&Hardware,Requested,&Io)==MaliBusy && HWrites==W);
    }
    uint32_t Setting=0xa55a;uint64_t Actual=0x12345678;
    CHECK(MaliMfgPllEncodeRate(93749999,&Setting,&Actual)==MaliUnsupported && Setting==0xa55a && Actual==0x12345678);
    CHECK(MaliMfgPllEncodeRate(UINT64_MAX,&Setting,&Actual)==MaliUnsupported && Setting==0xa55a && Actual==0x12345678);
    CHECK(MaliMfgPllEncodeRate(390000000,0,&Actual)==MaliInvalid && Actual==0x12345678);
    CHECK(MaliMfgPllEncodeRate(390000000,&Setting,0)==MaliInvalid && Setting==0xa55a);
    InitHardware(0,0,1);uint32_t Q=HReads;
    HAllowed=0;CHECK(MaliMfgHwClockProgramRate(&Hardware,390000000,&Io)==MaliOffline && HReads==Q && !HWrites);
    HAllowed=1;CHECK(MaliMfgHwClockProgramRate(&Hardware,390000000,&Io)==MaliBusy && !HWrites);
    CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk);RateAllowed=0;
    CHECK(MaliMfgHwClockProgramRate(&Hardware,390000000,&Io)==MaliOffline && HWrites==1 && !Hardware.Poisoned);
    RateAllowed=1;
    for(uint32_t Bad=0;Bad<5;Bad++) {
        InitHardware(0,0,1);CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk);
        if(Bad==0)HRegs[0][0]&=~1u;
        if(Bad==1)HRegs[1][0x50/4]|=0x40000;
        if(Bad==2)HRegs[2][0x268/4]|=1;
        if(Bad==3)HRegs[2][0x274/4]=1;
        if(Bad==4)HRegs[1][0x50/4]|=0x800000;
        CHECK(MaliMfgHwClockProgramRate(&Hardware,390000000,&Io)==MaliBusy && HWrites==1 && !Hardware.Poisoned);
    }
    InitHardware(0,0,1);CHECK(MaliMfgHwClockSet(&Hardware,0,1)==MaliOk);HDrop=2;
    CHECK(MaliMfgHwClockProgramRate(&Hardware,390000000,&Io)==MaliFault && Hardware.Poisoned && Hardware.Held==1);
    Q=HReads;uint32_t W=HWrites;
    CHECK(MaliMfgHwClockProgramRate(&Hardware,390000000,&Io)==MaliFault && HReads==Q && HWrites==W);
    CHECK(MaliMfgHwClockParent(&Hardware,0)==MaliFault && HReads==Q && HWrites==W);
}
/* Exercise all production power, PMIC, rail and clock-register cores together;
   the mapped registers and authorization/OS ownership remain virtual fixtures. */
static uint32_t ReadCombined(void *X,uint32_t R,uint32_t O) {
    CHECK(X==PDevice);if(R>=3 || (R==2 && O==0))return ReadHardware(HRegs,R,O);return ReadPower(X,R,O);
}
static void WriteCombined(void *X,uint32_t R,uint32_t O,uint32_t V) {
    CHECK(X==PDevice);if(R>=3 || (R==2 && (O==4 || O==8)))WriteHardware(HRegs,R,O,V);else WritePower(X,R,O,V);
}
static int EnableCombinedClock(void *X,uint32_t N) {
    if(N==1 && MaliMfgHwClockSet(&Hardware,2,1)!=MaliOk){return -1;}
    return EnablePowerClock(X,N);
}
static int DisableCombinedClock(void *X,uint32_t N) {
    if(N==1 && MaliMfgHwClockSet(&Hardware,2,0)!=MaliOk){return -1;}
    return DisablePowerClock(X,N);
}
static int ParentCombined(void *X,uint32_t N) {
    if(MaliMfgHwClockParent(&Hardware,N)!=MaliOk){return -1;}
    return ParentPowerClock(X,N);
}
static int EnableCombinedReference(void *X) {
    if(MaliMfgHwClockSet(&Hardware,0,1)!=MaliOk){return -1;}
    return EnablePower(X);
}
static int DisableCombinedReference(void *X) {
    if(MaliMfgHwClockSet(&Hardware,0,0)!=MaliOk){return -1;}
    return DisablePower(X);
}
static void CombinedPower(void) {
    for(uint32_t Borrowed=0;Borrowed<4;Borrowed++)for(uint32_t Ref=0;Ref<4;Ref++) {
        MALI_MFG_POWER D;InitPower(&D,0);InitBus(Borrowed,0);InitHardware(0,Ref,1);
        MALI_MFG_POWER_IO Io=D.Io;MALI_MFG_POWER Empty={0};D=Empty;
        Io.Domains.Read32=Io.Clocks.Registers.Read32=ReadCombined;
        Io.Domains.Write32=Io.Clocks.Registers.Write32=WriteCombined;
        Io.RailSet=ActualPowerRail;Io.ReferenceEnable=EnableCombinedReference;Io.ReferenceDisable=DisableCombinedReference;
        Io.Clocks.Enable=EnableCombinedClock;Io.Clocks.Disable=DisableCombinedClock;Io.Clocks.SetParent=ParentCombined;
        CHECK(MaliMfgPowerInitialize(&D,&Io)==MaliOk);
        CHECK(MaliMfgPowerReference(&D,1)==MaliOk && MaliMfgPowerSequenceReady(&D)==MaliOk);
        CHECK(MaliMfgRailsReady(&Rails)==MaliOk && MaliMfgHwClockReady(&Hardware)==MaliOk);
        CHECK(D.Stages==1023 && Hardware.Held==7 && Rails.Acquired==3 && Rails.Owned==(Borrowed^3));
        CHECK(MaliMfgPowerReference(&D,0)==MaliOk && !D.Stages && !D.References && !Hardware.Held && !Rails.Acquired && !Rails.Owned);
        CHECK((BRegs[0x1508/2]&1)==(Borrowed&1) && (BRegs[0x1bc8/2]&1)==((Borrowed>>1)&1));
    }
}
/* Qualified-table fixtures exercise the planner and actual PMIC/clock cores;
   none of these values are installed as a device OPP policy. */
static const MALI_MFG_OPP TestOpps[]={
    {1100000000,1000000,1000000},{1040000000,950000,950000},
    {980000000,900000,900000},{920000000,850000,900000},
    {860000000,800000,900000},{800000000,750000,875000},
    {700000000,700000,850000},{600000000,675000,825000},
    {500000000,650000,800000},{400000000,625000,775000},
    {300000000,625000,775000}};
#define TEST_OPPS ((uint32_t)(sizeof(TestOpps)/sizeof(TestOpps[0])))
static MALI_MFG_OPP_POLICY OppPolicy,BadPolicy;
static MALI_MFG_OPP Entries[MALI_MFG_OPP_MAX];
static MALI_MFG_OPP_PLAN Plan;
static MALI_MFG_DVFS Dvfs;
static uint32_t DAllowed,DCalls;
static void ZeroTest(void *P,size_t N){for(size_t I=0;I<N;I++)((uint8_t*)P)[I]=0;}
static int BytesTest(const void *P,size_t N,uint8_t V) {
    for(size_t I=0;I<N;I++) {
        if(((const uint8_t*)P)[I]!=V)return 0;
    }
    return 1;
}
static void OppPolicies(void) {
    ZeroTest(&OppPolicy,sizeof(OppPolicy));
    CHECK(MaliMfgOppBuild(&OppPolicy,TestOpps,TEST_OPPS)==MaliOk);
    CHECK(OppPolicy.Count==TEST_OPPS && OppPolicy.Up[10]==7 && OppPolicy.Down[0]==4);
    for(uint32_t A=0;A<TEST_OPPS;A++)for(uint32_t B=0;B<TEST_OPPS;B++) {
        CHECK(MaliMfgOppPlan(&OppPolicy,A,B,&Plan)==MaliOk);
        CHECK(Plan.Initial==A && Plan.Target==B && Plan.Count && Plan.Count<=TEST_OPPS+1);
        uint32_t G=TestOpps[A].VgpuUv,S=TestOpps[A].VsramUv,Rates=0;
        for(uint32_t I=0;I<Plan.Count;I++) {
            const MALI_MFG_OPP_STEP *T=&Plan.Steps[I];CHECK(T->Index<TEST_OPPS);
            CHECK(T->Hz==TestOpps[T->Index].Hz && T->VgpuUv==TestOpps[T->Index].VgpuUv);
            if(T->Kind==MaliMfgOppRate) {
                CHECK(T->Index==B && (B<A?I==Plan.Count-1:I==0));++Rates;
                if(B<A)CHECK(G==TestOpps[B].VgpuUv && S==TestOpps[B].VsramUv);
                else CHECK(G==TestOpps[A].VgpuUv && S==TestOpps[A].VsramUv);
            } else {
                CHECK(T->Kind==MaliMfgOppVoltage);
                CHECK(T->VsramUv>=T->VgpuUv && T->VsramUv-T->VgpuUv<=200000);
                if(B<A)CHECK(T->VgpuUv>=G && T->VsramUv>=S && T->VsramUv-G<=200000);
                if(B>A)CHECK(T->VgpuUv<=G && T->VsramUv<=S && S-T->VgpuUv<=200000);
                G=T->VgpuUv;S=T->VsramUv;
            }
        }
        CHECK(G==TestOpps[B].VgpuUv && S==TestOpps[B].VsramUv && Rates==(A!=B));
    }
    CHECK(MaliMfgOppPlan(&OppPolicy,10,0,&Plan)==MaliOk && Plan.Count==5);
    CHECK(MaliMfgOppPlan(&OppPolicy,9,10,&Plan)==MaliOk && Plan.Count==1 && Plan.Steps[0].Kind==MaliMfgOppRate);
    CHECK(MaliMfgOppPlan(&OppPolicy,9,9,&Plan)==MaliOk && Plan.Count==1 && Plan.Steps[0].Kind==MaliMfgOppVoltage);
    CHECK(MaliMfgOppBuild(&OppPolicy,TestOpps,TEST_OPPS)==MaliBusy);
    for(uint32_t F=0;F<11;F++) {
        ZeroTest(&BadPolicy,sizeof(BadPolicy));
        for(uint32_t I=0;I<TEST_OPPS;I++)Entries[I]=TestOpps[I];
        MALI_RESULT Expected=MaliInvalid;
        if(F==0)Entries[0].VgpuUv=499999;
        if(F==1)Entries[0].VgpuUv=1000001;
        if(F==2)Entries[0].VsramUv=993750;
        if(F==3)Entries[10].Hz=93749999,Expected=MaliUnsupported;
        if(F==4)Entries[1].Hz=Entries[0].Hz,Expected=MaliUnsupported;
        if(F==5)Entries[1].VgpuUv=1006250,Entries[1].VsramUv=1006250;
        if(F==6)Entries[1].VsramUv=1006250;
        if(F==7)Entries[9].VgpuUv=Entries[10].VgpuUv,Entries[9].VsramUv=781250;
        if(F==8)Entries[10].VsramUv=831250;
        if(F==9)Entries[0].VsramUv=1250000;
        if(F==10)Entries[0].VgpuUv=1200000,Entries[0].VsramUv=1200000,Expected=MaliUnsupported;
        CHECK(MaliMfgOppBuild(&BadPolicy,Entries,TEST_OPPS)==Expected);
        CHECK(BytesTest(&BadPolicy,sizeof(BadPolicy),0));
    }
    ZeroTest(&BadPolicy,sizeof(BadPolicy));
    CHECK(MaliMfgOppBuild(&BadPolicy,Entries,0)==MaliInvalid && MaliMfgOppBuild(&BadPolicy,Entries,65)==MaliInvalid);
    CHECK(MaliMfgOppBuild(&BadPolicy,(const MALI_MFG_OPP*)&BadPolicy,1)==MaliInvalid);
    CHECK(MaliMfgOppBuild((MALI_MFG_OPP_POLICY*)((uint8_t*)&BadPolicy+1),TestOpps,1)==MaliInvalid);
    CHECK(MaliMfgOppBuild(&BadPolicy,(const MALI_MFG_OPP*)(UINTPTR_MAX-7u),1)==MaliInvalid);
    for(size_t I=0;I<sizeof(Plan);I++)((uint8_t*)&Plan)[I]=0xa5;
    CHECK(MaliMfgOppPlan(&OppPolicy,TEST_OPPS,0,&Plan)==MaliInvalid && BytesTest(&Plan,sizeof(Plan),0xa5));
    CHECK(MaliMfgOppPlan(&BadPolicy,0,0,&Plan)==MaliInvalid && BytesTest(&Plan,sizeof(Plan),0xa5));
    CHECK(MaliMfgOppPlan(&OppPolicy,0,1,(MALI_MFG_OPP_PLAN*)&OppPolicy)==MaliInvalid);
    BadPolicy=OppPolicy;BadPolicy.Up[10]=10;
    CHECK(MaliMfgOppPlan(&BadPolicy,10,0,&Plan)==MaliInvalid && BytesTest(&Plan,sizeof(Plan),0xa5));
    BadPolicy=OppPolicy;BadPolicy.Down[0]=0;
    CHECK(MaliMfgOppPlan(&BadPolicy,0,10,&Plan)==MaliInvalid && BytesTest(&Plan,sizeof(Plan),0xa5));
    ZeroTest(&BadPolicy,sizeof(BadPolicy));
    for(uint32_t I=0;I<64;I++)Entries[I]=(MALI_MFG_OPP){1000000000u-I*10000000u,1000000-I*6250,1000000-I*6250};
    CHECK(MaliMfgOppBuild(&BadPolicy,Entries,64)==MaliOk);
    CHECK(MaliMfgOppPlan(&BadPolicy,63,0,&Plan)==MaliOk && Plan.Count>=3 && Plan.Count<=65);
    CHECK(MaliMfgOppPlan(&BadPolicy,0,63,&Plan)==MaliOk && Plan.Count>=3 && Plan.Count<=65);
}
static int AuthDvfs(void *X,uint32_t A,uint32_t B) {
    CHECK(X==&Dvfs && A<TEST_OPPS && B<TEST_OPPS);++DCalls;return (int)DAllowed;
}
static MALI_MFG_DVFS_IO DvfsIo(void) {
    MALI_MFG_DVFS_IO Io={&Dvfs,AuthDvfs,{&Rails,AuthVoltage},{&Hardware,AuthRate}};return Io;
}
static void InitDvfs(uint32_t Index,uint32_t Borrowed) {
    ZeroTest(&Dvfs,sizeof(Dvfs));DAllowed=1;DCalls=0;RateAllowed=1;RateAuthCalls=0;
    InitVoltage(TestOpps[Index].VgpuUv,TestOpps[Index].VsramUv,Borrowed);
    InitHardware(Index,0,1);
    MALI_MFG_RATE_IO Rate={&Hardware,AuthRate};
    CHECK(MaliMfgHwClockSet(&Hardware,MaliMfgHwReference,1)==MaliOk);
    CHECK(MaliMfgHwClockProgramRate(&Hardware,TestOpps[Index].Hz,&Rate)==MaliOk);
    CHECK(MaliMfgHwClockParent(&Hardware,MaliMfgMainParent)==MaliOk);
    CHECK(MaliMfgHwClockSet(&Hardware,MaliMfgHwBg3d,1)==MaliOk);
    MALI_MFG_DVFS_IO Io=DvfsIo();
    CHECK(MaliMfgDvfsInitialize(&Dvfs,&OppPolicy,&Rails,&Hardware,&Io,Index)==MaliOk);
    CHECK(Dvfs.Initialized && Dvfs.Current==Index && !Dvfs.Poisoned && DCalls==1);
    HWrites=BVoltageWrites=RateAuthCalls=VAuthCalls=0;
}
static void DvfsTransitions(void) {
    for(uint32_t A=0;A<TEST_OPPS;A++)for(uint32_t B=0;B<TEST_OPPS;B++) {
        InitDvfs(A,(A+B)%4);
        CHECK(MaliMfgDvfsCommit(&Dvfs,B)==MaliOk && !Dvfs.Poisoned && Dvfs.Current==B);
        CHECK(Dvfs.Target==B && Dvfs.Completed==Dvfs.Plan.Count && DCalls==2);
        CHECK(Hardware.Held==7 && Rails.Acquired==3 && Rails.Owned==(((A+B)%4)^3u));
        CHECK(Hardware.PllSetting && Rails.Microvolts[0]==TestOpps[B].VgpuUv && Rails.Microvolts[1]==TestOpps[B].VsramUv);
        uint64_t Hz=0;CHECK(MaliMfgPllDecodeRate(Hardware.PllSetting,&Hz)==MaliOk && Hz==OppPolicy.ActualHz[B]);
        CHECK(RateAuthCalls==(A!=B));
        if(A==B)CHECK(!HWrites && !BVoltageWrites);
        else if(A<B)CHECK(Dvfs.Plan.Steps[0].Kind==MaliMfgOppRate);
        else CHECK(Dvfs.Plan.Steps[Dvfs.Plan.Count-1].Kind==MaliMfgOppRate);
    }
    MALI_MFG_DVFS_IO Io=DvfsIo();
    InitDvfs(10,0);uint32_t Q=BCommands,W=HWrites;
    DAllowed=0;CHECK(MaliMfgDvfsCommit(&Dvfs,0)==MaliOffline && !Dvfs.Poisoned && BCommands==Q && HWrites==W);
    DAllowed=1;CHECK(MaliMfgDvfsCommit(&Dvfs,0)==MaliOk);
    CHECK(MaliMfgDvfsInitialize(&Dvfs,&OppPolicy,&Rails,&Hardware,&Io,0)==MaliBusy);
    InitDvfs(10,3);Q=BCommands;W=HWrites;
    Dvfs.Current=64;CHECK(MaliMfgDvfsCommit(&Dvfs,0)==MaliInvalid && BCommands==Q && HWrites==W);
    for(uint32_t F=0;F<4;F++) {
        InitDvfs(10,0);Dvfs.Initialized=0;DAllowed=F!=0;
        if(F==1)Rails.Microvolts[0]+=6250;
        if(F==2)HRegs[2][0x26c/4]^=32;
        Q=BCommands;W=HWrites;
        CHECK(MaliMfgDvfsInitialize(&Dvfs,&OppPolicy,&Rails,&Hardware,&Io,10)==(F==0?MaliOffline:(F==3?MaliOk:MaliBusy)));
        CHECK(HWrites==W && (!F?BCommands==Q:1));
    }
    /* Drop each actual clock write, in both directions. Unchanged writes may
       already hold their intended value; every real uncertainty is retained. */
    for(uint32_t Direction=0;Direction<2;Direction++)for(uint32_t F=1;F<=14;F++) {
        uint32_t A=Direction?0:10,B=Direction?10:0;
        InitDvfs(A,3);HDrop=F;MALI_RESULT R=MaliMfgDvfsCommit(&Dvfs,B);
        if(R) {
            CHECK(Dvfs.Poisoned && Dvfs.Current==A && Dvfs.Target==B && Rails.Acquired==3 && !Rails.Owned);
            Q=BCommands;W=HWrites;
            CHECK(MaliMfgDvfsCommit(&Dvfs,A)==MaliFault && BCommands==Q && HWrites==W);
            CHECK(MaliMfgDvfsInitialize(&Dvfs,&OppPolicy,&Rails,&Hardware,&Io,A)==MaliBusy);
        } else CHECK(Dvfs.Current==B && !Dvfs.Poisoned && Hardware.Held==7);
    }
    for(uint32_t Direction=0;Direction<2;Direction++)for(uint32_t F=0;F<5;F++) {
        uint32_t A=Direction?0:10,B=Direction?10:0;
        InitDvfs(A,0);
        if(F==0)VAllowed=0;
        if(F==1)RateAllowed=0;
        if(F==2)BDropWrite=1;
        if(F==3)BFreezeVoltage=1;
        if(F==4)BFailCommand=BCommands+25;
        CHECK(MaliMfgDvfsCommit(&Dvfs,B)!=MaliOk && Dvfs.Poisoned && Dvfs.Current==A);
        CHECK(Rails.Acquired==3 && Rails.Owned==3 && Dvfs.Target==B);
        Q=BCommands;W=HWrites;
        CHECK(MaliMfgDvfsCommit(&Dvfs,A)==MaliFault && BCommands==Q && HWrites==W);
    }
}
static void BorrowedRailHandoff(void) {
    const MALI_MFG_VOLTAGE_IO Io={&Rails,AuthVoltage};
    CHECK(MaliMfgRailsReleaseReady(0)==MaliInvalid);
    for(uint32_t Borrowed=1;Borrowed<=3;Borrowed++) {
        InitVoltage(700000,750000,Borrowed);
        CHECK(Rails.OriginalMicrovolts[0]==700000 && Rails.OriginalMicrovolts[1]==750000);
        CHECK(MaliMfgRailsVoltage(&Rails,800000,850000,&Io)==MaliOk);
        uint32_t Before=BCommands,W=BWrites;
        CHECK(MaliMfgRailsReleaseReady(&Rails)==MaliBusy && MaliMfgRailSet(&Rails,1,0)==MaliBusy);
        CHECK(BCommands==Before && BWrites==W && Rails.Acquired==3 && !Rails.Poisoned);
        CHECK(MaliMfgRailsVoltage(&Rails,700000,750000,&Io)==MaliOk);
        CHECK(Rails.OriginalMicrovolts[0]==700000 && Rails.OriginalMicrovolts[1]==750000);
        CHECK(MaliMfgRailsReleaseReady(&Rails)==MaliOk);
        CHECK(MaliMfgRailSet(&Rails,1,0)==MaliOk && MaliMfgRailSet(&Rails,0,0)==MaliOk);
        CHECK(!Rails.Acquired && !Rails.Owned && !Rails.OriginalMicrovolts[0] && !Rails.OriginalMicrovolts[1]);
    }
    InitVoltage(700000,750000,3);BRegs[0x1be2/2]^=0x100;uint32_t W=BWrites;
    CHECK(MaliMfgRailsReleaseReady(&Rails)==MaliBusy && MaliMfgRailSet(&Rails,1,0)==MaliBusy &&
        !Rails.Poisoned && Rails.Acquired==3 && !BVoltageWrites && (BRegs[0x1508/2]&1) && (BRegs[0x1bc8/2]&1));
    BRegs[0x1be2/2]^=0x100;CHECK(MaliMfgRailsReleaseReady(&Rails)==MaliOk);
    BFailCommand=BCommands+1;CHECK(MaliMfgRailsReleaseReady(&Rails)==MaliTimeout && Rails.Poisoned && Rails.Acquired==3);
    W=BWrites;CHECK(MaliMfgRailSet(&Rails,1,0)==MaliFault && BWrites==W);
    InitVoltage(700000,750000,3);BRailAllowed=0;W=BWrites;
    CHECK(MaliMfgRailsReleaseReady(&Rails)==MaliOffline && Rails.Acquired==3 && !Rails.Poisoned && BWrites==W);
}
static MALI_MFG_CONTROLLER Controller;
static MALI_MFG_POWER ControllerAnchor;
static MALI_MFG_CONTROLLER_IO ControllerIo;
static uint32_t ControllerLease,ControllerIdle,ControllerOppAllowed,ControllerOppCalls,ControllerOppDenyAt;
static uint32_t ControllerExpectedGpu,ControllerExpectedSram;
static uint64_t ControllerExpectedHz;
static int ControllerAuthorization(void *X,uint32_t On) {
    CHECK(X==&ControllerAnchor && On<2);return ControllerLease && (On || ControllerIdle);
}
static int ControllerDomainAuthorization(void *X,uint32_t N,uint32_t On) {
    CHECK(X==&ControllerAnchor && N<4 && On<2 && Rails.Acquired==3);
    if(N>=2)CHECK(Controller.ReferenceEnabled && (Hardware.Held&1));
    return ControllerLease && (On || ControllerIdle);
}
static int ControllerOperatingPoint(void *X,uint64_t Hz,uint32_t Gpu,uint32_t Sram) {
    CHECK(X==&ControllerAnchor && Hz==ControllerExpectedHz && Gpu==ControllerExpectedGpu && Sram==ControllerExpectedSram);
    ++ControllerOppCalls;return ControllerOppAllowed && ControllerOppCalls!=ControllerOppDenyAt;
}
static void InitController(uint32_t Borrowed,uint32_t Ref) {
    InitPower(&ControllerAnchor,0);InitBus(Borrowed,Ref);InitHardware(Ref,Ref,1);
    ZeroTest(&Controller,sizeof(Controller));ZeroTest(&ControllerIo,sizeof(ControllerIo));
    ControllerLease=ControllerIdle=ControllerOppAllowed=1;ControllerOppCalls=ControllerOppDenyAt=0;
    ControllerExpectedGpu=500000+Ref*6250;ControllerExpectedSram=500000+(Ref+17)*6250;
    CHECK(MaliMfgPllDecodeRate(HRegs[2][0x26c/4],&ControllerExpectedHz)==MaliOk);
    ControllerIo.Registers=ControllerAnchor.Io.Domains;
    ControllerIo.Registers.Read32=ReadCombined;ControllerIo.Registers.Write32=WriteCombined;
    ControllerIo.Registers.Authorized=ControllerDomainAuthorization;
    ControllerIo.Rails=&Rails;ControllerIo.Clock=&Hardware;
    ControllerIo.Authorized=ControllerAuthorization;ControllerIo.OperatingPoint=ControllerOperatingPoint;
    CHECK(MaliMfgControllerInitialize(&Controller,&ControllerIo)==MaliOk && Controller.Initialized && !Controller.Power.References);
}
static uint32_t ControllerWrites(void){return Writes+CWrite+HWrites+BWrites;}
static uint32_t ControllerReads(void){return Reads+CRead+HReads+BReads;}
static void ProductionControllerCycles(void) {
    for(uint32_t Borrowed=0;Borrowed<4;Borrowed++)for(uint32_t Ref=0;Ref<4;Ref++) {
        InitController(Borrowed,Ref);uint32_t W=ControllerWrites(),R=ControllerReads();
        CHECK(MaliMfgControllerReady(&Controller)==MaliBusy && MaliMfgControllerSet(&Controller,0)==MaliBusy);
        CHECK(ControllerWrites()==W && ControllerReads()==R && !Controller.Power.References && !Rails.Acquired && !Hardware.Held);
        uint32_t Pll=HRegs[2][0x26c/4],GpuEnable=BRegs[0x1508/2],SramEnable=BRegs[0x1bc8/2];
        for(uint32_t Cycle=0;Cycle<3;Cycle++) {
            CHECK(MaliMfgControllerSet(&Controller,1)==MaliOk && Controller.State==MaliMfgControllerOn && !Controller.Poisoned);
            CHECK(MaliMfgControllerReady(&Controller)==MaliOk && Controller.Power.References==1 && Controller.Power.Stages==MaliMfgPowerOn);
            CHECK(Rails.Acquired==3 && Rails.Owned==(Borrowed^3) && Hardware.Held==7 && Controller.Prepared==3 && Controller.Enabled==3);
            CHECK(Controller.ReferencePrepared && Controller.ReferenceEnabled && !BVoltageWrites);
            W=Writes+CWrite+HWrites;CHECK(MaliMfgControllerSet(&Controller,1)==MaliOk && Writes+CWrite+HWrites==W && Controller.Power.References==1);
            ControllerIdle=0;W=ControllerWrites();R=ControllerReads();
            CHECK(MaliMfgControllerSet(&Controller,0)==MaliOffline && ControllerWrites()==W && ControllerReads()==R);
            CHECK(Controller.State==MaliMfgControllerOn && Controller.Power.References==1 && !Controller.Poisoned);ControllerIdle=1;
            CHECK(MaliMfgControllerSet(&Controller,0)==MaliOk && Controller.State==MaliMfgControllerOff);
            CHECK(!Controller.Power.Stages && !Controller.Power.References && !Controller.Prepared && !Controller.Enabled && !Controller.ReferencePrepared && !Controller.ReferenceEnabled);
            CHECK(!Rails.Acquired && !Rails.Owned && !Hardware.Held && !Controller.Poisoned);
            CHECK(BRegs[0x1508/2]==GpuEnable && BRegs[0x1bc8/2]==SramEnable && HRegs[2][0x26c/4]==Pll);
            CHECK((HRegs[1][0x50/4]&0x870000u)==((Ref<<16)|0x800000u));
            W=ControllerWrites();R=ControllerReads();
            CHECK(MaliMfgControllerReady(&Controller)==MaliBusy && MaliMfgControllerSet(&Controller,0)==MaliOk);
            CHECK(ControllerWrites()==W && ControllerReads()==R);
        }
        CHECK(MaliMfgControllerInitialize(&Controller,&ControllerIo)==MaliBusy);if(Failure)return;
    }
}
static void ControllerPreflightAndBorrowedRestore(void) {
    InitController(0,0);HRegs[2][0x26c/4]=(HRegs[2][0x26c/4]&~0x07000000u)|0x07000000u;
    CHECK(MaliMfgControllerSet(&Controller,1)==MaliUnsupported && !Rails.Acquired && !Hardware.Held && !ControllerWrites() && !Controller.Poisoned);
    InitController(0,1);BRegs[0x1bf4/2]&=~0x7fu;
    CHECK(MaliMfgControllerSet(&Controller,1)==MaliInvalid && !Rails.Acquired && !Hardware.Held && !Writes && !CWrite && !HWrites && !Controller.Poisoned && !ControllerOppCalls);
    InitController(0,0);ControllerLease=0;
    CHECK(MaliMfgControllerSet(&Controller,1)==MaliOffline && !ControllerWrites() && !ControllerReads() && !Controller.Poisoned);
    ControllerLease=1;ControllerOppAllowed=0;
    CHECK(MaliMfgControllerSet(&Controller,1)==MaliOffline && !Writes && !CWrite && !HWrites && !Rails.Acquired && !Hardware.Held && !Controller.Poisoned);
    ControllerOppAllowed=1;
    CHECK(MaliMfgControllerSet(&Controller,1)==MaliOk);
    ControllerOppAllowed=0;CHECK(MaliMfgControllerReady(&Controller)==MaliOffline && !Controller.Poisoned);
    CHECK(MaliMfgControllerSet(&Controller,0)==MaliOk && !Controller.Power.Stages);
    InitController(0,0);ControllerOppDenyAt=2;
    CHECK(MaliMfgControllerSet(&Controller,1)==MaliOffline && Controller.State==MaliMfgControllerUnknown && Controller.Power.References==1);
    CHECK(!Controller.Poisoned && Rails.Acquired==3 && Hardware.Held==7 && MaliMfgControllerReady(&Controller)==MaliBusy);
    ControllerOppDenyAt=0;CHECK(MaliMfgControllerSet(&Controller,1)==MaliOk && Controller.State==MaliMfgControllerOn && Controller.Power.References==1);
    CHECK(MaliMfgControllerSet(&Controller,0)==MaliOk);
    for(uint32_t Borrowed=1;Borrowed<=3;Borrowed++) {
        InitController(Borrowed,1);CHECK(MaliMfgControllerSet(&Controller,1)==MaliOk);
        const MALI_MFG_VOLTAGE_IO V={&Rails,AuthVoltage};VAllowed=1;VMaxGap=200000;VAuthCalls=0;
        uint32_t OldGpu=Rails.Microvolts[0],OldSram=Rails.Microvolts[1];
        CHECK(MaliMfgRailsVoltage(&Rails,OldGpu+6250,OldSram+6250,&V)==MaliOk);
        uint32_t W=ControllerWrites(),R=ControllerReads();
        CHECK(MaliMfgControllerSet(&Controller,0)==MaliBusy && Controller.State==MaliMfgControllerOn && !Controller.Poisoned);
        CHECK(ControllerWrites()==W && ControllerReads()==R && Hardware.Held==7 && Rails.Acquired==3 && Controller.Power.Stages==MaliMfgPowerOn);
        CHECK(MaliMfgRailsVoltage(&Rails,OldGpu,OldSram,&V)==MaliOk);
        CHECK(MaliMfgControllerSet(&Controller,0)==MaliOk && !Rails.Acquired && !Hardware.Held && !Controller.Power.Stages);
    }
}
static void ControllerPartialFailureRetention(void) {
    for(uint32_t F=0;F<9;F++) {
        InitController(0,F==5?1:0);
        switch(F) {
        case 0:BFailCommand=1;break;case 1:BDropWrite=1;break;
        case 2:PStuck=0;break;case 3:HDrop=1;break;
        case 4:PDcmDrop=1;break;case 5:HRegs[2][0x340/4]=0;break;
        default:CHECK(MaliMfgControllerSet(&Controller,1)==MaliOk);
            if(F==6)PIdleBad=MALI_MFG_WAIT_POLLS;
            if(F==7)HDrop=HWrites+1;
            if(F==8)BFreezeStatus=1;
        }
        MALI_RESULT Result=MaliMfgControllerSet(&Controller,F<6?1:0);
        CHECK(Result!=MaliOk && Controller.Poisoned && Controller.State==MaliMfgControllerUnknown);
        if(F==0)CHECK(Result==MaliTimeout && Bus.Poisoned && !Rails.Acquired && !Controller.Power.Stages);
        if(F==1)CHECK(Result==MaliTimeout && Rails.Poisoned && Rails.Acquired==1 && Rails.Owned==1);
        if(F==2)CHECK(Result==MaliTimeout && Controller.Power.Domains.Poisoned && Rails.Acquired==3 && !Hardware.Held);
        if(F==3)CHECK(Hardware.Poisoned && Hardware.Held==1 && Controller.ReferencePrepared);
        if(F==4)CHECK(Controller.Power.References==1 && Hardware.Held==7 && Rails.Acquired==3);
        if(F==5)CHECK(Result==MaliOffline && Controller.Power.Poisoned && Rails.Acquired==3 && !Hardware.Held);
        if(F==6)CHECK(Result==MaliTimeout && Controller.Power.References==1 && Hardware.Held==7);
        if(F==7)CHECK(Hardware.Poisoned && Hardware.Held==7 && Controller.Power.References==1);
        if(F==8)CHECK(Result==MaliTimeout && Rails.Poisoned && Rails.Acquired==3 && !Hardware.Held && Controller.Power.References==1);
        uint32_t W=ControllerWrites(),R=ControllerReads();
        CHECK(MaliMfgControllerReady(&Controller)==MaliFault && MaliMfgControllerSet(&Controller,0)==MaliFault && MaliMfgControllerSet(&Controller,1)==MaliFault);
        CHECK(MaliMfgControllerInitialize(&Controller,&ControllerIo)!=MaliOk && ControllerWrites()==W && ControllerReads()==R);
        if(Failure)return;
    }
}
static void ControllerInvalidInput(void) {
    InitController(0,0);
    CHECK(MaliMfgControllerSet(0,1)==MaliInvalid && MaliMfgControllerReady(0)==MaliInvalid && MaliMfgControllerSet(&Controller,2)==MaliInvalid);
    for(uint32_t F=0;F<20;F++) {
        InitController(0,0);MALI_MFG_CONTROLLER_IO Io=ControllerIo;ZeroTest(&Controller,sizeof(Controller));
        switch(F) {
        case 0:Io.Registers.Read32=0;break;case 1:Io.Registers.Write32=0;break;
        case 2:Io.Registers.Authorized=0;break;case 3:Io.Authorized=0;break;case 4:Io.OperatingPoint=0;break;
        case 5:Io.Clock=0;break;case 6:Io.Rails=0;break;case 7:Io.Rails=(MALI_MFG_RAILS*)&Controller;break;
        case 8:Hardware.Initialized=0;break;case 9:Bus.Poisoned=1;break;case 10:Rails.Acquired=1;break;
        case 11:Rails.Io.Pmic=(MALI_MFG_PMIC*)((uint8_t*)&Bus+1);break;
        case 12:Rails.Io.Pmic=(MALI_MFG_PMIC*)&Controller;break;
        case 13:Io.Clock=(MALI_MFG_HW_CLOCK*)&Rails;break;case 14:Rails.Io.Pmic=0;break;
        case 15:Io.Rails=(MALI_MFG_RAILS*)&Io;break;case 16:Io.Clock=(MALI_MFG_HW_CLOCK*)&Io;break;
        case 17:Rails.Io.Pmic=(MALI_MFG_PMIC*)&Rails;break;
        case 18:Rails.Io.Pmic=(MALI_MFG_PMIC*)&Hardware;break;case 19:Rails.Io.Pmic=(MALI_MFG_PMIC*)&Io;break;
        }
        CHECK(MaliMfgControllerInitialize(&Controller,&Io)==(F==10?MaliBusy:MaliInvalid));
        CHECK(!Controller.Initialized && !Controller.Power.Initialized && !ControllerWrites() && !ControllerReads());
    }
    InitController(0,0);ZeroTest(&Controller,sizeof(Controller));
    CHECK(MaliMfgControllerInitialize(&Controller,(MALI_MFG_CONTROLLER_IO*)&Controller)==MaliInvalid);
    CHECK(MaliMfgControllerInitialize((MALI_MFG_CONTROLLER*)((uint8_t*)&Controller+1),&ControllerIo)==MaliInvalid);
}
static void CalibrationFixture(MALI_MFG_OPP *E,MALI_MFG_CALIBRATION *C) {
    static const MALI_MFG_OPP Stock[MALI_MFG_STOCK_OPP_COUNT]={
        {UINT64_C(1100000000),900000u,900000u},
        {UINT64_C(1086000000),893750u,893750u},
        {UINT64_C(1072000000),887500u,887500u},
        {UINT64_C(1058000000),881250u,881250u},
        {UINT64_C(1045000000),875000u,875000u},
        {UINT64_C(1031000000),868750u,868750u},
        {UINT64_C(1017000000),862500u,862500u},
        {UINT64_C(1003000000),856250u,856250u},
        {UINT64_C(990000000),850000u,850000u},
        {UINT64_C(976000000),843750u,843750u},
        {UINT64_C(962000000),831250u,831250u},
        {UINT64_C(948000000),825000u,825000u},
        {UINT64_C(935000000),818750u,818750u},
        {UINT64_C(921000000),812500u,812500u},
        {UINT64_C(907000000),806250u,806250u},
        {UINT64_C(893000000),800000u,800000u},
        {UINT64_C(880000000),793750u,793750u},
        {UINT64_C(868000000),787500u,787500u},
        {UINT64_C(857000000),781250u,781250u},
        {UINT64_C(846000000),775000u,775000u},
        {UINT64_C(835000000),768750u,768750u},
        {UINT64_C(823000000),762500u,762500u},
        {UINT64_C(812000000),756250u,756250u},
        {UINT64_C(801000000),756250u,756250u},
        {UINT64_C(790000000),750000u,750000u},
        {UINT64_C(778000000),743750u,750000u},
        {UINT64_C(767000000),737500u,750000u},
        {UINT64_C(756000000),731250u,750000u},
        {UINT64_C(745000000),725000u,750000u},
        {UINT64_C(733000000),718750u,750000u},
        {UINT64_C(722000000),712500u,750000u},
        {UINT64_C(711000000),706250u,750000u},
        {UINT64_C(700000000),700000u,750000u},
        {UINT64_C(674000000),700000u,750000u},
        {UINT64_C(648000000),700000u,750000u},
        {UINT64_C(622000000),693750u,750000u},
        {UINT64_C(596000000),693750u,750000u},
        {UINT64_C(570000000),693750u,750000u},
        {UINT64_C(545000000),687500u,750000u},
        {UINT64_C(519000000),687500u,750000u},
        {UINT64_C(493000000),687500u,750000u},
        {UINT64_C(467000000),681250u,750000u},
        {UINT64_C(441000000),681250u,750000u},
        {UINT64_C(415000000),681250u,750000u},
        {UINT64_C(390000000),675000u,750000u},
    };
    for(uint32_t I=0;I<MALI_MFG_STOCK_OPP_COUNT;I++)E[I]=Stock[I];
    ZeroTest(C,sizeof(*C));C->Size=sizeof(*C);C->Version=1;C->Segment=2;C->PresentMask=7;
}
static void CalibrationPolicies(void) {
    MALI_MFG_OPP E[MALI_MFG_STOCK_OPP_COUNT];MALI_MFG_CALIBRATION C;
    for(uint32_t Segment=1;Segment<=2;Segment++)for(uint32_t Mcl=0;Mcl<2;Mcl++)for(uint32_t Aging=0;Aging<2;Aging++) {
        CalibrationFixture(E,&C);C.Segment=Segment;C.Mcl50Load=Mcl;C.Aging=Aging;
        if(Aging)C.AvsWords[2]=(390u<<8)|96u; /* synthetic, monotonic aging fixture */
        MALI_MFG_OPP Saved[MALI_MFG_STOCK_OPP_COUNT];
        for(uint32_t I=0;I<MALI_MFG_STOCK_OPP_COUNT;I++)Saved[I]=E[I];
        MALI_MFG_OPP_POLICY P;ZeroTest(&P,sizeof(P));
        CHECK(MaliMfgOppCalibrate(&P,E,MALI_MFG_STOCK_OPP_COUNT,&C)==MaliOk);
        uint32_t First=Segment==1 || Mcl?7u:0u;
        CHECK(P.Signature==MALI_MFG_OPP_SIGNATURE && P.Count==45u-First);
        CHECK(P.Entries[0].Hz==E[First].Hz && P.Entries[P.Count-1].Hz==E[44].Hz);
        for(uint32_t I=0;I<45;I++)CHECK(E[I].Hz==Saved[I].Hz && E[I].VgpuUv==Saved[I].VgpuUv && E[I].VsramUv==Saved[I].VsramUv);
        MALI_MFG_OPP_PLAN Plan;
        CHECK(MaliMfgOppPlan(&P,0,P.Count-1,&Plan)==MaliOk);
        CHECK(MaliMfgOppPlan(&P,P.Count-1,0,&Plan)==MaliOk);
        CHECK(MaliMfgOppCalibrate(&P,E,45,&C)==MaliBusy);
    }
    CalibrationFixture(E,&C);C.Aging=1;
    MALI_MFG_OPP_POLICY Unsafe;ZeroTest(&Unsafe,sizeof(Unsafe));
    /* Stock aging drops on a shallow AVS segment can reverse voltage order. */
    CHECK(MaliMfgOppCalibrate(&Unsafe,E,45,&C)==MaliInvalid && !Unsafe.Signature && !Unsafe.Count);
    for(uint32_t Bad=0;Bad<20;Bad++) {
        CalibrationFixture(E,&C);MALI_MFG_OPP_POLICY P;ZeroTest(&P,sizeof(P));
        P.ActualHz[63]=UINT64_C(0x12345678);uint32_t Count=45;
        const MALI_MFG_OPP *Input=E;const MALI_MFG_CALIBRATION *Cal=&C;
        switch(Bad) {
        case 0:C.Size--;break;case 1:C.Version=0;break;case 2:C.Segment=0;break;
        case 3:C.Segment=3;break;case 4:C.Mcl50Load=2;break;case 5:C.Aging=2;break;
        case 6:C.PresentMask=3;break;case 7:C.PresentMask=15;break;case 8:Count=44;break;
        case 9:C.AvsWords[0]=(1099u<<8)|144u;break;case 10:C.AvsWords[1]=(699u<<8)|112u;break;
        case 11:C.AvsWords[2]=(389u<<8)|108u;break;case 12:E[0].Hz=1099000000;break;
        case 13:E[7].VgpuUv=1;break;case 14:E[7].Hz=E[6].Hz;break;
        case 15:E[7].Hz++;break;case 16:Input=(const MALI_MFG_OPP *)&P;break;
        case 17:Cal=(const MALI_MFG_CALIBRATION *)&P;break;case 18:Cal=0;break;
        case 19:Input=0;break;
        }
        CHECK(MaliMfgOppCalibrate(&P,Input,Count,Cal)!=MaliOk);
        CHECK(!P.Signature && !P.Count && P.ActualHz[63]==UINT64_C(0x12345678));
        uint32_t Changed=0;for(uint32_t I=0;I<sizeof(P);I++)
            if(((uint8_t *)&P)[I] && !(I>=((size_t)((uint8_t *)P.ActualHz-(uint8_t *)&P))+63*sizeof(uint64_t) &&
                I<((size_t)((uint8_t *)P.ActualHz-(uint8_t *)&P))+64*sizeof(uint64_t)))++Changed;
        CHECK(!Changed);
    }
}
int TestMain(void){CalibrationPolicies();BorrowedRailHandoff();Transitions();InvalidAndTimeout();Clocks();Power();Pmic();PmicBroker();HardwareClocks();VoltagePairs();ProgramRates();CombinedPower();OppPolicies();DvfsTransitions();ControllerInvalidInput();ProductionControllerCycles();ControllerPreflightAndBorrowedRestore();ControllerPartialFailureRetention();return (int)Failure;}
#ifdef MALI_MFG_TEST_NATIVE
int main(void){int R=TestMain();printf("MFG domains/clocks/power/PMIC/PLL/voltage/OPP/DVFS: %u assertions, first failure line %d (ASan/UBSan)\n",MtkMaliMfgAssertionCount,R);return R?1:0;}
#endif
