// SPDX-License-Identifier: BSD-2-Clause-Patent
// Calendar writes are fake callbacks only. No WACS/PMIC/device access.
#include "MediaTekTest.h"
#include "../Library/UnavailableRealTimeClockLib/UnavailableRealTimeClockLib.c"
EFI_GUID gEfiRtPropertiesTableGuid=EFI_RT_PROPERTIES_TABLE_GUID;
EFI_GUID gFdtTableGuid={1},gEfiEventExitBootServicesGuid={2},gMediaTekPmicWrapperProtocolGuid={3};
EFI_GUID gEfiEventReadyToBootGuid={4},gEfiEventVirtualAddressChangeGuid={5},gEfiEventBeforeExitBootServicesGuid={7};
EFI_GUID gMtkPmicRtcRuntimeProtocolGuid={6};
EFI_DXE_SERVICES *gDS;
BOOLEAN EFIAPI CompareGuid(CONST GUID *A,CONST GUID *B) { return !memcmp(A,B,sizeof(*A)); }
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *Start,UINT64 *End) { if(Start)*Start=0;if(End)*End=MAX_UINT64;return 1000000000; }
static EFI_TPL Tpl;
EFI_GUID gEfiCallerIdGuid={0},gEfiRealTimeClockArchProtocolGuid={0};
EFI_BOOT_SERVICES *gBS;
static unsigned VariableWrites;
EFI_STATUS EFIAPI EfiGetVariable(CHAR16 *N,EFI_GUID *G,UINT32 *A,UINTN *S,VOID *D) { return EFI_NOT_FOUND; }
EFI_STATUS EFIAPI EfiSetVariable(CHAR16 *N,EFI_GUID *G,UINT32 A,UINTN S,VOID *D) { ++VariableWrites;return EFI_SUCCESS; }
#include "../../../../Mu_Basecore/EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.c"
#include "../../../../Mu_Basecore/EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClock.c"
static EFI_TIME Input={2024,2,29,23,58,59,0,0,EFI_UNSPECIFIED_TIMEZONE,0,0};
static unsigned Writes,Reads,FailWrite,FailRead,ExitWrite,ExitRead,BusyPolls,ReenterWrite;
static BOOLEAN PreBusy;
static UINT16 Addresses[8],Values[8];
static EFI_TPL EFIAPI Raise(EFI_TPL N) { EFI_TPL Old=Tpl;CHECK(N>=Old);Tpl=N;return Old; }
static VOID EFIAPI Restore(EFI_TPL N) { Tpl=N; }
static EFI_STATUS EFIAPI Read(UINT16 Addr,UINT16 *V) {
  CHECK(Tpl==TPL_APPLICATION);CHECK(Addr==0x588);Reads++;TimeNs+=50000;
  if(ExitRead==Reads)StockRtcExit(NULL,NULL);
  if(FailRead==Reads)return EFI_DEVICE_ERROR;
  *V=(Writes==0?PreBusy:BusyPolls>0)?0x40:0;
  if(Writes && BusyPolls)BusyPolls--;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write(UINT16 Addr,UINT16 V) {
  EFI_TIME T=Input;CHECK(Tpl==TPL_APPLICATION);CHECK(Writes<8);
  Addresses[Writes]=Addr;Values[Writes++]=V;
  if(ReenterWrite==Writes) {
    EFI_TPL OldTpl;
    CHECK(LibSetTime(&Input)==EFI_NOT_READY);
    OldTpl=Raise(TPL_CALLBACK);
    CHECK(LibGetTime(&T,NULL)==EFI_NOT_READY);
    Restore(OldTpl);
  }
  if(ExitWrite==Writes)StockRtcExit(NULL,NULL);
  // Even an error can have changed the shadow bank; never roll it back/commit.
  return Writes==FailWrite?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static MTK_PMIC_WRAPPER_PROTOCOL P={Read,Write};
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **Out) { CHECK(CompareGuid(G,&gMediaTekPmicWrapperProtocolGuid));*Out=&P;return EFI_SUCCESS; }
static EFI_BOOT_SERVICES Bs;
static VOID Fresh(void) {
  // Test-only new-boot fixture. Production has no fault-reset API.
  memset(&Bs,0,sizeof(Bs));Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.LocateProtocol=Locate;
  mRtcBoot=&Bs;mRtcFdtValid=TRUE;mRtcEnded=mRtcReading=mRtcWriteFault=FALSE;Tpl=TPL_APPLICATION;
  Writes=Reads=FailWrite=FailRead=ExitWrite=ExitRead=BusyPolls=ReenterWrite=0;PreBusy=FALSE;TimeNs=0;
}
static VOID Quarantined(void) {
  EFI_TIME T=Input;unsigned Before=Reads+Writes;
  CHECK(mRtcWriteFault);CHECK(!mRtcReading);CHECK(Tpl==TPL_APPLICATION);
  CHECK(LibSetTime(&Input)==EFI_DEVICE_ERROR);CHECK(LibGetTime(&T,NULL)==EFI_DEVICE_ERROR);
  CHECK(Reads+Writes==Before);
}
int main(void) {
  unsigned I,Y,M,D,ExpectedDow=2;EFI_TIME T,Before;
  Fresh();T=Input;T.TimeZone=60;
  CHECK(SetTime(&T)==EFI_SUCCESS&&Writes==8&&VariableWrites==1);
  Fresh();T.Day=0;CHECK(SetTime(&T)==EFI_INVALID_PARAMETER&&!Writes&&!Reads&&VariableWrites==1);
  Fresh();T=Input;T.TimeZone=120;FailWrite=1;
  CHECK(SetTime(&T)==EFI_DEVICE_ERROR&&Writes==1&&VariableWrites==1);
  Fresh();Before=Input;ReenterWrite=4;BusyPolls=2;
  CHECK(LibSetTime(&Input)==EFI_SUCCESS);CHECK(!memcmp(&Input,&Before,sizeof(Input)));
  CHECK(Writes==8&&Reads==4&&!mRtcWriteFault&&TimeNs>=2000000);
  for(I=0;I<7;I++)CHECK(Addresses[I]==0x592+2*I);
  CHECK(Addresses[7]==0x5c2&&Values[7]==1);
  CHECK(Values[0]==59&&Values[1]==58&&Values[2]==23&&Values[3]==29&&Values[4]==5&&Values[5]==2&&Values[6]==56);
  // Independent calendar anchors, including the Gregorian leap century 2000.
  static const unsigned Known[][4]={
    {1968,1,1,2},{1970,1,1,5},{1999,12,31,6},{2000,1,1,7},
    {2000,2,28,2},{2000,2,29,3},{2000,3,1,4},{2024,2,29,5},
    {2026,11,22,1},{2095,12,31,7}
  };
  for(I=0;I<sizeof(Known)/sizeof(Known[0]);I++) {
    Fresh();T=Input;T.Year=Known[I][0];T.Month=Known[I][1];T.Day=Known[I][2];Before=T;
    CHECK(LibSetTime(&T)==EFI_SUCCESS);CHECK(Values[4]==Known[I][3]);
    CHECK(!memcmp(&T,&Before,sizeof(T)));
  }
  // Walk every supported date from the known Monday anchor; no reuse of the
  // implementation's ordinal arithmetic. Invalid dates must not advance it.
  for(Y=1967;Y<=2096;Y++) for(M=0;M<=13;M++) for(D=0;D<=32;D++) {
    static const unsigned Days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned Limit=M>=1&&M<=12?Days[M-1]:0;BOOLEAN Valid;
    if(M==2 && Y%4==0 && (Y%100!=0 || Y%400==0))Limit++;
    Valid=Y>=1968&&Y<=2095&&M>=1&&M<=12&&D>=1&&D<=Limit;
    Fresh();T=Input;T.Year=Y;T.Month=M;T.Day=D;
    CHECK(LibSetTime(&T)==(Valid?EFI_SUCCESS:EFI_INVALID_PARAMETER));
    CHECK(Writes==(Valid?8:0));CHECK(Reads==(Valid?2:0));
    if(Valid){CHECK(Values[4]==ExpectedDow);ExpectedDow=ExpectedDow%7+1;}
  }
  for(I=0;I<6;I++) {
    Fresh();T=Input;
    switch(I){case 0:T.Hour=24;break;case 1:T.Minute=60;break;case 2:T.Second=60;break;
      case 3:T.Nanosecond=1000000000;break;case 4:T.TimeZone=1441;break;case 5:T.Daylight=4;break;}
    CHECK(LibSetTime(&T)==EFI_INVALID_PARAMETER&&Writes==0&&Reads==0);
  }
  Fresh();CHECK(LibSetTime(NULL)==EFI_INVALID_PARAMETER&&!Writes&&!Reads);
  PreBusy=TRUE;CHECK(LibSetTime(&Input)==EFI_NOT_READY&&!Writes&&!mRtcWriteFault);
  Fresh();FailRead=1;CHECK(LibSetTime(&Input)==EFI_DEVICE_ERROR&&!Writes&&!mRtcWriteFault);
  for(I=1;I<=8;I++){Fresh();FailWrite=I;CHECK(LibSetTime(&Input)==EFI_DEVICE_ERROR&&Writes==I);Quarantined();}
  Fresh();FailRead=2;CHECK(LibSetTime(&Input)==EFI_DEVICE_ERROR&&Writes==8);Quarantined();
  Fresh();BusyPolls=100000;CHECK(LibSetTime(&Input)==EFI_TIMEOUT&&Writes==8&&Reads<=1002);CHECK(TimeNs>=1000000000);Quarantined();
  for(I=1;I<=8;I++){Fresh();ExitWrite=I;CHECK(LibSetTime(&Input)==EFI_UNSUPPORTED&&Writes==I);CHECK(LibSetTime(&Input)==EFI_UNSUPPORTED&&Writes==I);}
  Fresh();ExitRead=1;CHECK(LibSetTime(&Input)==EFI_UNSUPPORTED&&!Writes&&Reads==1);
  Fresh();ExitRead=2;CHECK(LibSetTime(&Input)==EFI_UNSUPPORTED&&Writes==8&&Reads==2);
  CHECK(LibSetTime(&Input)==EFI_UNSUPPORTED&&Writes==8&&Reads==2);
  Fresh();StockRtcExit(NULL,NULL);CHECK(LibSetTime(&Input)==EFI_UNSUPPORTED&&!Writes&&!Reads);
  Fresh();Tpl=TPL_NOTIFY;CHECK(LibSetTime(&Input)==EFI_NOT_READY&&!Writes&&!Reads);
  printf("Set RTC: %u checks passed (calendar/one commit/quarantine, fake transport only)\n",Checks);
  return 0;
}
