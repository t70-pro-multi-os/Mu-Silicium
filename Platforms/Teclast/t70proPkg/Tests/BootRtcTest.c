// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#include "../Library/UnavailableRealTimeClockLib/UnavailableRealTimeClockLib.c"
EFI_GUID gEfiRtPropertiesTableGuid=EFI_RT_PROPERTIES_TABLE_GUID;
EFI_GUID gFdtTableGuid={1},gEfiEventExitBootServicesGuid={2},gMediaTekPmicWrapperProtocolGuid={3};
EFI_GUID gEfiEventReadyToBootGuid={4},gEfiEventVirtualAddressChangeGuid={5},gEfiEventBeforeExitBootServicesGuid={7};
EFI_GUID gMtkPmicRtcRuntimeProtocolGuid={6};
EFI_DXE_SERVICES *gDS;
BOOLEAN EFIAPI CompareGuid(CONST GUID *A,CONST GUID *B) { return !memcmp(A,B,sizeof(*A)); }
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *Start,UINT64 *End) { if(Start)*Start=0;if(End)*End=MAX_UINT64;return 1000000000; }
static EFI_TPL Tpl=TPL_APPLICATION;
static unsigned Reads,FailAt,ExitAt,ReenterAt,Rollovers;
static UINT16 Raw[]={59,58,23,29,4,2,56};
static EFI_STATUS CreateStatus,LocateStatus;
static EFI_RT_PROPERTIES_TABLE *Props;
static EFI_EVENT_NOTIFY FdtNotify;
static VOID *FdtContext;
static EFI_TPL EFIAPI Raise(EFI_TPL N) { EFI_TPL Old=Tpl;CHECK(N>=Old);Tpl=N;return Old; }
static VOID EFIAPI Restore(EFI_TPL N) { Tpl=N; }
static EFI_STATUS EFIAPI Read(UINT16 Addr,UINT16 *Value) {
  unsigned Slot=Reads++%8;EFI_TIME T;
  CHECK(Tpl==TPL_APPLICATION);CHECK(Addr==(Slot==7?0x592:0x592+Slot*2));
  if(ReenterAt && Reads==ReenterAt) CHECK(LibGetTime(&T,NULL)==EFI_NOT_READY);
  if(ExitAt && Reads==ExitAt) StockRtcExit(NULL,NULL);
  if(FailAt && Reads==FailAt)return EFI_DEVICE_ERROR;
  *Value=Raw[Slot==7?0:Slot];
  if(Slot==7 && Rollovers) {--Rollovers;*Value=0;}
  return EFI_SUCCESS;
}
static MTK_PMIC_WRAPPER_PROTOCOL P={Read,NULL};
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *Registration,VOID **Out) {
  CHECK(CompareGuid(G,&gMediaTekPmicWrapperProtocolGuid));*Out=&P;return LocateStatus;
}
static EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY Fn,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *E) {
  CHECK(Type==EVT_NOTIFY_SIGNAL);
  if (CompareGuid(G,&gFdtTableGuid)) {
    CHECK(T==TPL_CALLBACK && Fn==StockRtcFdtChanged && C);
    FdtNotify=Fn; FdtContext=(VOID *)C;
  } else {
    CHECK(CompareGuid(G,&gEfiEventExitBootServicesGuid)&&T==TPL_NOTIFY&&Fn==StockRtcExit);
  }
  *E=EFI_ERROR(CreateStatus)?(VOID *)0xdead:(VOID *)1;return CreateStatus;
}
static EFI_STATUS EFIAPI InstallTable(EFI_GUID *G,VOID *Table) { Props=Table;return EFI_SUCCESS; }
int main(int argc,char **argv) {
  EFI_BOOT_SERVICES Bs={0};EFI_SYSTEM_TABLE St={0};EFI_CONFIGURATION_TABLE Ct={0};
  EFI_TIME T,Before;EFI_TIME_CAPABILITIES C;unsigned I,N,Y,M,Day;FILE *F;long Size;VOID *Dtb;
  CHECK(argc==2);F=fopen(argv[1],"rb");CHECK(F!=NULL);fseek(F,0,SEEK_END);Size=ftell(F);rewind(F);
  Dtb=malloc(Size);CHECK(Dtb&&fread(Dtb,1,Size,F)==Size);fclose(F);
  Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.LocateProtocol=Locate;Bs.CreateEventEx=Create;
  Bs.InstallConfigurationTable=InstallTable;St.BootServices=&Bs;
  Ct.VendorGuid=gFdtTableGuid;Ct.VendorTable=Dtb;St.ConfigurationTable=&Ct;St.NumberOfTableEntries=0;
  CHECK(RtcTree(Dtb));CHECK(!RtcTree(NULL));
  CHECK(LibGetTime(&T,NULL)==EFI_UNSUPPORTED);
  CreateStatus=EFI_OUT_OF_RESOURCES;CHECK(LibRtcInitialize(NULL,&St)==EFI_SUCCESS);
  CHECK(!mRtcBoot&&!mRtcExitEvent);CHECK(LibGetTime(&T,NULL)==EFI_UNSUPPORTED);
  CreateStatus=EFI_SUCCESS;CHECK(LibRtcInitialize(NULL,&St)==EFI_SUCCESS);
  CHECK(Props->RuntimeServicesSupported==0x3ff0);CHECK(mRtcBoot==&Bs);
  CHECK(!mRtcFdtValid && LibGetTime(&T,NULL)==EFI_UNSUPPORTED && Reads==0);
  CHECK(FdtNotify && FdtContext==&St);
  St.NumberOfTableEntries=1; FdtNotify(NULL,FdtContext);
  CHECK(mRtcFdtValid);
  memset(&T,0xa5,sizeof(T));T.TimeZone=EFI_UNSPECIFIED_TIMEZONE;T.Daylight=0;Before=T;
  CHECK(LibGetTime(NULL,NULL)==EFI_INVALID_PARAMETER);
  ReenterAt=1;CHECK(LibGetTime(&T,&C)==EFI_SUCCESS&&Reads==8);ReenterAt=0;
  CHECK(C.Resolution==1 && C.Accuracy==MAX_UINT32 && !C.SetsToZero);
  CHECK(T.Year==2024&&T.Month==2&&T.Day==29&&T.Second==59&&T.Minute==58&&T.Hour==23);
  CHECK(T.TimeZone==EFI_UNSPECIFIED_TIMEZONE&&!T.Daylight&&!T.Nanosecond);
  for(I=1;I<=8;I++) {Reads=0;FailAt=I;T=Before;CHECK(LibGetTime(&T,NULL)==EFI_DEVICE_ERROR);CHECK(!memcmp(&T,&Before,sizeof(T)));}
  FailAt=0;Reads=0;Rollovers=2;CHECK(LibGetTime(&T,NULL)==EFI_SUCCESS&&Reads==24);
  Reads=0;Rollovers=3;T=Before;CHECK(LibGetTime(&T,NULL)==EFI_NOT_READY&&Reads==24);CHECK(!memcmp(&T,&Before,sizeof(T)));
  Raw[6]=55;Reads=0;CHECK(LibGetTime(&T,NULL)==EFI_DEVICE_ERROR);Raw[6]=56;
  Raw[5]=0;Reads=0;CHECK(LibGetTime(&T,NULL)==EFI_DEVICE_ERROR);Raw[5]=2;
  for(Y=0;Y<128;Y++) for(M=1;M<=12;M++) for(Day=0;Day<=32;Day++) {
    static const unsigned Days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned Year=Y+1968,Limit=Days[M-1];BOOLEAN Valid;
    if(M==2 && Year%4==0 && (Year%100!=0 || Year%400==0)) Limit++;
    Valid=Day>=1 && Day<=Limit;Raw[6]=Y;Raw[5]=M;Raw[3]=Day;Reads=0;T=Before;
    CHECK(LibGetTime(&T,NULL)==(Valid?EFI_SUCCESS:EFI_DEVICE_ERROR));
    if(Valid) CHECK(T.Year==Year&&T.Month==M&&T.Day==Day);
    else CHECK(!memcmp(&T,&Before,sizeof(T)));
  }
  Raw[6]=56;Raw[5]=2;Raw[3]=29;
  { INT32 Len,Node=FdtPathOffset(Dtb,"/soc/pwrap@10026000/mt6366/mt6358rtc");
    CHAR8 *Compat=(CHAR8 *)FdtGetProp(Dtb,Node,"compatible",&Len);CHAR8 Saved=Compat[0];
    Compat[0]='X';CHECK(!RtcTree(Dtb));Compat[0]=Saved;CHECK(RtcTree(Dtb));
  }
  LocateStatus=EFI_NOT_FOUND;N=Reads;CHECK(LibGetTime(&T,NULL)==EFI_NOT_READY&&Reads==N);LocateStatus=0;
  T=Before;T.Year=2024;T.Month=2;T.Day=29;T.Hour=23;T.Minute=58;T.Second=59;T.Nanosecond=0;
  CHECK(LibSetTime(&T)==EFI_NOT_READY);CHECK(LibSetWakeupTime(FALSE,NULL)==EFI_UNSUPPORTED);
  Reads=0;ExitAt=1;T=Before;CHECK(LibGetTime(&T,NULL)==EFI_UNSUPPORTED&&Reads==1);
  CHECK(!memcmp(&T,&Before,sizeof(T)));CHECK(!mRtcBoot);
  CHECK(LibGetTime(&T,NULL)==EFI_UNSUPPORTED&&Reads==1);
  CHECK(LibRtcInitialize(NULL,&St)==EFI_SUCCESS&&!mRtcBoot);
  free(Dtb);printf("Boot RTC: %u checks passed (stock DT, fake transport, no hardware)\n",Checks);return 0;
}
