// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#define _PCD_VALUE_PcdStorageIsEMMC FALSE
#define _PCD_VALUE_PcdMsdcHostMask 2U
#define _PCD_VALUE_PcdMsdcReadOnly TRUE
#define _PCD_VALUE_PcdMsdcMaxClockHz 25000000U
#define _PCD_VALUE_PcdSdDetectPin 7U
#define _PCD_VALUE_PcdSdDetectActiveLow FALSE
#define _PCD_VALUE_PcdSdVmchEint 1U
EFI_GUID gMediaTekGpioProtocolGuid={1}, gMediaTekClockProtocolGuid={2}, gMediaTekPmicProtocolGuid={3};
#ifdef TEST_MT6768
#include "../../../../Silicon/MediaTek/MT6768Pkg/Library/MsdcImplLib/MsdcImplLib.c"
#else
#include "../../../../Silicon/MediaTek/MT6789Pkg/Library/MsdcImplLib/MsdcImplLib.c"
#endif
static unsigned Calls, Mutations, FailMutation, ClockCalls, FailClock;
static UINT32 Pin;
static BOOLEAN Level, RailOn[2];
#ifndef TEST_MT6768
static BOOLEAN ClocksOn[3];
static unsigned RailOffOrder[4], RailOffCount;
#endif
static unsigned DetectWrites;
static BOOLEAN DetectOutput;
static EFI_STATUS ReadDetectStatus;
static unsigned PadEvents[64],PadEventCount,PadAcquireCalls,PadConfigureCalls,PadReleaseCalls;
static EFI_STATUS PadAcquireStatus,PadConfigureStatus,PadReleaseStatus;
static BOOLEAN PadCanRelease;
static UINT64 PadToken;
static UINT32 PadRequested[6];
static UINT32 Uv[2]={2900000,1800000};
static EFI_STATUS EFIAPI Mode(UINT32 pin,UINT32 mode) { Calls++; DetectWrites++; Pin=pin; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Dir(UINT32 pin,BOOLEAN out) { Calls++; DetectWrites++; CHECK(pin==7 && !out); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI GetDir(UINT32 pin,BOOLEAN *out) { Calls++; CHECK(pin==7); *out=DetectOutput; return ReadDetectStatus; }
static EFI_STATUS EFIAPI State(UINT32 pin,BOOLEAN *level) { Calls++; CHECK(pin==7); *level=Level; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI AcquirePadPins(CONST UINT32 *pins,UINTN count,UINT64 *token) {
  PadAcquireCalls++; PadEvents[PadEventCount++]=1;
  CHECK(pins!=NULL && count==6 && token!=NULL);
  for(UINTN i=0;i<count;i++) { PadRequested[i]=pins[i]; }
  if(EFI_ERROR(PadAcquireStatus)) { *token=0; return PadAcquireStatus; }
  *token=PadToken; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ConfigurePadPins(UINT64 token,BOOLEAN *canRelease) {
  PadConfigureCalls++; PadEvents[PadEventCount++]=2;
  CHECK(token==PadToken && canRelease!=NULL);
  *canRelease=PadCanRelease;
  return PadConfigureStatus;
}
static EFI_STATUS EFIAPI ReleasePadPins(UINT64 token) {
  PadReleaseCalls++; PadEvents[PadEventCount++]=3;
  CHECK(token==PadToken);
  return PadReleaseStatus;
}
static EFI_STATUS EFIAPI GetId(CONST CHAR8 *name, UINT32 *id) {
  Calls++;
  CHECK(strstr(name,"MSDC0")==NULL && strstr(name,"50_0")==NULL);
#ifndef TEST_MT6768
  CHECK(!strcmp(name,"TOP_MSDC30_1_SEL") || !strcmp(name,"IFRAO_MSDC1") || !strcmp(name,"IFRAO_MSDC1_SRC"));
  *id=!strcmp(name,"IFRAO_MSDC1")?2:1;
#else
  *id=1;
#endif
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetClock(UINT32 id,BOOLEAN enable) {
  if(++ClockCalls==FailClock) return EFI_TIMEOUT;
#ifndef TEST_MT6768
  CHECK(id==1 || id==2);
  if(id==2 && enable) CHECK(ClocksOn[1]);
  if(id==1 && !enable) CHECK(!ClocksOn[2]);
  ClocksOn[id]=enable;
#endif
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI IsClock(UINT32 id,BOOLEAN *on) {
#ifndef TEST_MT6768
  CHECK(id==1 || id==2);
  *on=ClocksOn[id];
#else
  *on=FALSE;
#endif
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Freq(UINT32 id,UINT64 *hz) { *hz=200000000; return EFI_SUCCESS; }
static unsigned Rail(CONST CHAR8 *s) { CHECK(!strcmp(s,"vmch_eint_low") || !strcmp(s,"ldo_vmc")); return !strcmp(s,"ldo_vmc"); }
static EFI_STATUS EFIAPI SetRail(CONST CHAR8 *s,BOOLEAN on) {
  unsigned Id=Rail(s);
#ifndef TEST_MT6768
  if (!on) {
    if (Id==0) CHECK(!RailOn[1]);
    if (RailOffCount<ARRAY_SIZE(RailOffOrder)) RailOffOrder[RailOffCount++]=Id;
  }
#endif
  RailOn[Id]=on;
  return ++Mutations==FailMutation?EFI_TIMEOUT:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI IsRail(CONST CHAR8 *s,BOOLEAN *on) { *on=RailOn[Rail(s)]; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI SetUv(CONST CHAR8 *s,UINT32 uv) {
  unsigned Id=Rail(s);
#ifndef TEST_MT6768
  CHECK(!RailOn[Id]);
#endif
  Uv[Id]=uv;
  return ++Mutations==FailMutation?EFI_TIMEOUT:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GetUv(CONST CHAR8 *s,UINT32 *uv) { *uv=Uv[Rail(s)]; return EFI_SUCCESS; }
static MTK_GPIO_PROTOCOL Gpio={.SetMode=Mode,.SetDir=Dir,.GetState=State,.GetDir=GetDir,
  .AcquirePins=AcquirePadPins,.ReleasePins=ReleasePadPins,
  .ConfigureMsdc1PadsWithLease=ConfigurePadPins};
static MTK_CLOCK_PROTOCOL Clock={.GetId=GetId,.SetEnable=SetClock,.GetFrequency=Freq,.IsEnabled=IsClock};
static MTK_PMIC_PROTOCOL Pmic={.RegulatorSetEnable=SetRail,.RegulatorIsEnabled=IsRail,.RegulatorSetVoltage=SetUv,.RegulatorGetVoltage=GetUv};
static EFI_STATUS EFIAPI Locate(EFI_GUID *g,VOID *registration,VOID **out) {
  if (g==&gMediaTekGpioProtocolGuid) *out=&Gpio;
  else if (g==&gMediaTekClockProtocolGuid) *out=&Clock;
  else if (g==&gMediaTekPmicProtocolGuid) *out=&Pmic;
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}
int main(void) {
  BOOLEAN present; UINTN hz;
  Services.LocateProtocol=Locate;
  CHECK(MsdcLibConstructor()==EFI_SUCCESS && Mutations==0);
  CHECK(InitGpio(0)==EFI_UNSUPPORTED && Calls==0);
  CHECK(ClockControl(0,TRUE)==EFI_UNSUPPORTED && Calls==0);
  CHECK(SourceClockControl(0,TRUE)==EFI_UNSUPPORTED && Calls==0);
  CHECK(PowerControl(0,TRUE)==EFI_UNSUPPORTED && Mutations==0);
  CHECK(GetSourceClockRate(0,&hz)==EFI_UNSUPPORTED && Calls==0);
  CHECK(GetCardPresent(0,&present)==EFI_UNSUPPORTED && Calls==0);
  Level=TRUE; CHECK(GetCardPresent(1,&present)==EFI_SUCCESS && present && Pin==7);
  Level=FALSE; CHECK(GetCardPresent(1,&present)==EFI_SUCCESS && !present);
#ifndef TEST_MT6768
  CHECK(DetectWrites==2);
  for(unsigned n=0;n<100;n++) CHECK(GetCardPresent(1,&present)==EFI_SUCCESS && !present);
  CHECK(DetectWrites==2);
  DetectOutput=TRUE; present=TRUE;
  CHECK(GetCardPresent(1,&present)==EFI_ACCESS_DENIED && !present && DetectWrites==2);
  DetectOutput=FALSE; ReadDetectStatus=EFI_DEVICE_ERROR; present=TRUE;
  CHECK(GetCardPresent(1,&present)==EFI_DEVICE_ERROR && !present && DetectWrites==2);
  ReadDetectStatus=EFI_SUCCESS;
#endif
#ifndef TEST_MT6768
  PadToken=0x713F;
  PadCanRelease=TRUE;
  PadAcquireStatus=EFI_ACCESS_DENIED;
  CHECK(InitGpio(1)==EFI_ACCESS_DENIED && PadAcquireCalls==1 &&
        PadConfigureCalls==0 && PadReleaseCalls==0);
  CHECK(PadEventCount==1 && PadEvents[0]==1);
  PadAcquireStatus=EFI_SUCCESS;

  PadEventCount=PadAcquireCalls=PadConfigureCalls=PadReleaseCalls=0;
  PadConfigureStatus=EFI_DEVICE_ERROR; PadReleaseStatus=EFI_SUCCESS;
  CHECK(InitGpio(1)==EFI_DEVICE_ERROR && PadAcquireCalls==1 &&
        PadConfigureCalls==1 && PadReleaseCalls==1);
  CHECK(PadEventCount==3 && PadEvents[0]==1 && PadEvents[1]==2 && PadEvents[2]==3);
  CHECK(PadRequested[0]==71 && PadRequested[1]==72 && PadRequested[2]==73 &&
        PadRequested[3]==74 && PadRequested[4]==75 && PadRequested[5]==76);

  // A failed release after successful hardware setup is retried without a new
  // allocation/configuration; the completed setup then returns success.
  PadEventCount=PadAcquireCalls=PadConfigureCalls=PadReleaseCalls=0;
  PadConfigureStatus=EFI_SUCCESS; PadReleaseStatus=EFI_TIMEOUT;
  CHECK(InitGpio(1)==EFI_TIMEOUT && PadAcquireCalls==1 &&
        PadConfigureCalls==1 && PadReleaseCalls==1);
  PadReleaseStatus=EFI_SUCCESS;
  CHECK(InitGpio(1)==EFI_SUCCESS && PadAcquireCalls==1 &&
        PadConfigureCalls==1 && PadReleaseCalls==2);

  // If config rolled back but release failed, retry release before acquiring
  // a new token and retrying setup.
  mSdPadsConfigured=FALSE; mSdPadLeaseToken=0; mSdPadLeaseUnsafe=FALSE;
  PadEventCount=PadAcquireCalls=PadConfigureCalls=PadReleaseCalls=0;
  PadConfigureStatus=EFI_DEVICE_ERROR; PadCanRelease=TRUE;
  PadReleaseStatus=EFI_TIMEOUT;
  CHECK(InitGpio(1)==EFI_TIMEOUT && PadAcquireCalls==1 &&
        PadConfigureCalls==1 && PadReleaseCalls==1);
  PadConfigureStatus=EFI_SUCCESS; PadReleaseStatus=EFI_SUCCESS;
  CHECK(InitGpio(1)==EFI_SUCCESS && PadAcquireCalls==2 &&
        PadConfigureCalls==2 && PadReleaseCalls==3);

  // An unverifiable rollback is quarantined: ownership is never released and
  // repeated InitGpio calls do not allocate a second lease.
  mSdPadsConfigured=FALSE; mSdPadLeaseToken=0; mSdPadLeaseUnsafe=FALSE;
  PadEventCount=PadAcquireCalls=PadConfigureCalls=PadReleaseCalls=0;
  PadConfigureStatus=EFI_DEVICE_ERROR; PadCanRelease=FALSE;
  CHECK(InitGpio(1)==EFI_DEVICE_ERROR && PadAcquireCalls==1 &&
        PadConfigureCalls==1 && PadReleaseCalls==0);
  CHECK(InitGpio(1)==EFI_DEVICE_ERROR && PadAcquireCalls==1 &&
        PadConfigureCalls==1 && PadReleaseCalls==0);
#endif
  CHECK(GetSourceClockRate(1,NULL)==EFI_INVALID_PARAMETER);
  CHECK(ClockControl(1,TRUE)==EFI_SUCCESS && ClockCalls==2);
  ClockCalls=0; FailClock=1; CHECK(ClockControl(1,TRUE)==EFI_TIMEOUT && ClockCalls==1);
#ifndef TEST_MT6768
  ClockCalls=FailClock=0;
  CHECK(ClockControl(1,FALSE)==EFI_SUCCESS && ClockCalls==2 && !ClocksOn[1] && !ClocksOn[2]);
  ClockCalls=0; FailClock=2;
  CHECK(ClockControl(1,TRUE)==EFI_TIMEOUT && ClockCalls==3 && !ClocksOn[1] && !ClocksOn[2]);
#endif
  for (unsigned n=1;n<=4;n++) {
    RailOn[0]=RailOn[1]=FALSE; Uv[0]=2900000; Uv[1]=1800000; Mutations=0; FailMutation=n;
    CHECK(PowerControl(1,TRUE)==EFI_TIMEOUT);
    CHECK(!RailOn[0] && !RailOn[1] && Uv[0]==2900000 && Uv[1]==1800000);
  }
  FailMutation=0; Mutations=0; RailOn[0]=TRUE; Uv[0]=2900000;
  CHECK(PowerControl(1,TRUE)==EFI_ACCESS_DENIED && Mutations==0);
  RailOn[0]=FALSE; CHECK(PowerControl(1,TRUE)==EFI_SUCCESS && RailOn[0] && RailOn[1] && Uv[0]==3300000 && Uv[1]==3300000);
#ifdef TEST_MT6768
  printf("MSDC PLATFORM MT6768: %u checks passed (legacy host/GPIO/supply behavior preserved)\n",Checks);
#else
  unsigned PreviousMutations=Mutations;
  CHECK(PowerControl(1,TRUE)==EFI_SUCCESS && Mutations==PreviousMutations);
  RailOffCount=0;
  CHECK(PowerControl(1,FALSE)==EFI_SUCCESS && !RailOn[0] && !RailOn[1]);
  CHECK(RailOffCount==2 && RailOffOrder[0]==1 && RailOffOrder[1]==0);
  printf("MSDC PLATFORM MT6789: %u checks passed (host exclusion, GPIO7, MSDC1 pad lease/retry/quarantine, supply restoration)\n",Checks);
#endif
}
