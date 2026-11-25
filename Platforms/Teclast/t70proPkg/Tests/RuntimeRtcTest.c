// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute both production runtime modules with fake EFI services and WACS2.
#include "MediaTekTest.h"
#define EFI_CALLER_ID_GUID {0}
#define EDKII_DSC_PLATFORM_GUID {0}
#include "../Library/UnavailableRealTimeClockLib/UnavailableRealTimeClockLib.c"
#include "../../../../Silicon/MediaTek/MediaTekPkg/Drivers/MtkUfsPrm/MtkUfsPrm.c"

#define TEST_VIRTUAL_BASE 0x70000000U

EFI_GUID gEfiRtPropertiesTableGuid=EFI_RT_PROPERTIES_TABLE_GUID;
EFI_GUID gFdtTableGuid={1},gEfiEventExitBootServicesGuid={2},gMediaTekPmicWrapperProtocolGuid={3};
EFI_GUID gEfiEventReadyToBootGuid={4},gEfiEventVirtualAddressChangeGuid={5},gEfiEventBeforeExitBootServicesGuid={7};
EFI_GUID gMtkPmicRtcRuntimeProtocolGuid={6},gPrmConfigProtocolGuid={7};
static EFI_DXE_SERVICES DxeServices;
EFI_DXE_SERVICES *gDS=&DxeServices;

static EFI_EVENT_NOTIFY ExitNotify, ReadyNotify, BeforeExitNotify, VirtualNotify, FdtNotify;
static VOID *RtcContext;
static EFI_RT_PROPERTIES_TABLE *Published;
static MTK_PMIC_RTC_RUNTIME_PROTOCOL *Provider;
static BOOLEAN RuntimeMapped, Virtual, AfterExit;
static unsigned FailConversion, Conversions, ConversionsToVirtual;
static UINT32 Machine, InitDone;
static unsigned Commands, Writes, Clears, TriggerCount, BusyPolls, SecondsReads;
static unsigned StallRead, StallWrite, Rollovers, BusyAfterTrigger;
static BOOLEAN BusyAtEntry;
static UINT16 Data, WriteData, Calendar[MTK_RTC_FIELDS], Shadow[MTK_RTC_FIELDS];
static UFS_RUNTIME_RANGES ConvertedRanges;

BOOLEAN EFIAPI CompareGuid(CONST GUID *A, CONST GUID *B) {
  return !memcmp(A,B,sizeof(*A));
}
GUID *EFIAPI CopyGuid(GUID *D, CONST GUID *S) {
  return CopyMem(D,S,sizeof(*D));
}
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *Start, UINT64 *End) {
  if (Start) *Start=0;
  if (End) *End=MAX_UINT64;
  return 1000000000;
}
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *Target, UINT32 Compare, UINT32 Set) {
  UINT32 Previous=*Target;
  if (Previous==Compare) *Target=Set;
  return Previous;
}
VOID EFIAPI MemoryFence(VOID) {}
VOID ArmCallSmc(ARM_SMC_ARGS *Args) {
  (VOID)Args;
  CHECK(FALSE);
}

static UINTN Offset(UINTN Address) {
  UINTN Base=Virtual?TEST_VIRTUAL_BASE:RTC_PW_BASE;
  CHECK(AfterExit && Address>=Base && Address<Base+RTC_PW_SIZE);
  return Address-Base;
}

UINT32 EFIAPI MmioRead32(UINTN Address) {
  CHECK(mPowerLock==1);
  switch (Offset(Address)) {
    case 0: return InitDone;
    case 0x8A8: return Machine<<1;
    case 0x894: CHECK(Machine==6); return Data;
    default: CHECK(FALSE); return 0;
  }
}

UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value) {
  UINT16 Reg=(UINT16)Value;
  UINTN OffsetInRange=Offset(Address);
  CHECK(mPowerLock==1);
  if (OffsetInRange==0x8A4) {
    CHECK(Value==1 && Machine==6);
    Machine=0;
    Clears++;
    return Value;
  }
  CHECK(!Machine);
  if (OffsetInRange==0x884) {
    CHECK(Value<=MAX_UINT16);
    WriteData=(UINT16)Value;
    return Value;
  }
  CHECK(OffsetInRange==0x880 && !(Value&~0x2000FFFFU));
  Commands++;
  if (Value&0x20000000U) {
    Writes++;
    if (Reg==MTK_RTC_TRIGGER) {
      CHECK(WriteData==1);
      TriggerCount++;
      CopyMem(Calendar,Shadow,sizeof(Calendar));
      BusyPolls=BusyAfterTrigger;
    } else {
      CHECK(RtcCalendarRegister(Reg));
      Shadow[(Reg-MTK_RTC_SECONDS)/2]=WriteData;
    }
    if (StallWrite==Writes) Machine=4;
  } else {
    if (Reg==MTK_RTC_BBPU) {
      Data=(BusyAtEntry || BusyPolls)?MTK_RTC_BUSY:0;
      if (BusyPolls) BusyPolls--;
    } else {
      CHECK(RtcCalendarRegister(Reg));
      Data=Calendar[(Reg-MTK_RTC_SECONDS)/2];
      if (Reg==MTK_RTC_SECONDS && ++SecondsReads%2==0 && Rollovers) {
        Rollovers--;
        Calendar[0]=(Calendar[0]+1)%60;
        Data=Calendar[0];
      }
    }
    Machine=StallRead==Commands?4:6;
  }
  return Value;
}

static EFI_STATUS EFIAPI Publish(EFI_GUID *Guid, VOID *Table) {
  CHECK(CompareGuid(Guid,&gEfiRtPropertiesTableGuid) && !AfterExit);
  Published=Table;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI InstallProtocol(EFI_HANDLE *Handle, EFI_GUID *Guid,
                                         EFI_INTERFACE_TYPE Type, VOID *Interface) {
  CHECK(Type==EFI_NATIVE_INTERFACE && !AfterExit);
  if (CompareGuid(Guid,&gPrmConfigProtocolGuid)) {
    CHECK(Interface==&mConfiguration);
    *Handle=(EFI_HANDLE)&mConfiguration;
  } else {
    CHECK(CompareGuid(Guid,&gMtkPmicRtcRuntimeProtocolGuid) && *Handle==&mConfiguration);
    Provider=Interface;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid, VOID *Registration, VOID **Interface) {
  (VOID)Registration;
  CHECK(!AfterExit && CompareGuid(Guid,&gMtkPmicRtcRuntimeProtocolGuid));
  *Interface=Provider;
  return Provider?EFI_SUCCESS:EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI Descriptor(EFI_PHYSICAL_ADDRESS Address, EFI_GCD_MEMORY_SPACE_DESCRIPTOR *Result) {
  CHECK(!AfterExit && Address==RTC_PW_BASE);
  *Result=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){
    .BaseAddress=Address, .Length=RTC_PW_SIZE, .GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo,
    .Attributes=RuntimeMapped?EFI_MEMORY_RUNTIME:0
  };
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI Convert(UINTN Flags, VOID **Address) {
  CHECK(!Flags && AfterExit && Conversions<2);
  Conversions++;
  if (Conversions==FailConversion) return EFI_NOT_FOUND;
  if (*Address==(VOID *)RTC_PW_BASE) {
    *Address=(VOID *)TEST_VIRTUAL_BASE;
  } else {
    CHECK(*Address==(VOID *)RuntimeRtcAccess);
    ConversionsToVirtual++;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI Create(UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Notify,
                               CONST VOID *Context, CONST EFI_GUID *Group, EFI_EVENT *Event) {
  CHECK(Type==EVT_NOTIFY_SIGNAL && !AfterExit);
  if (CompareGuid(Group,&gEfiEventExitBootServicesGuid)) {
    CHECK(Tpl==TPL_NOTIFY && !ExitNotify && !Context);
    ExitNotify=Notify;
    *Event=&ExitNotify;
  } else if (CompareGuid(Group,&gEfiEventReadyToBootGuid)) {
    CHECK(Tpl==TPL_CALLBACK && !ReadyNotify && Context==RtcContext);
    ReadyNotify=Notify;
    *Event=&ReadyNotify;
  } else if (CompareGuid(Group,&gEfiEventBeforeExitBootServicesGuid)) {
    CHECK(Tpl==TPL_CALLBACK && !BeforeExitNotify && Context==RtcContext);
    BeforeExitNotify=Notify;
    *Event=&BeforeExitNotify;
  } else if (CompareGuid(Group,&gFdtTableGuid)) {
    CHECK(Tpl==TPL_CALLBACK && !FdtNotify && Context);
    FdtNotify=Notify; RtcContext=(VOID *)Context;
    *Event=&FdtNotify;
  } else {
    CHECK(CompareGuid(Group,&gEfiEventVirtualAddressChangeGuid) && Tpl==TPL_NOTIFY && !VirtualNotify);
    VirtualNotify=Notify;
    *Event=&VirtualNotify;
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI Close(EFI_EVENT Event) { (VOID)Event; CHECK(FALSE); return EFI_SUCCESS; }
static EFI_TPL EFIAPI Raise(EFI_TPL Tpl) { (VOID)Tpl; CHECK(FALSE); return TPL_APPLICATION; }
static VOID EFIAPI Restore(EFI_TPL Tpl) { (VOID)Tpl; CHECK(FALSE); }

static VOID Boot(VOID *Fdt, UINT32 Supported) {
  EFI_RUNTIME_SERVICES Rt={.ConvertPointer=Convert};
  // Time services retain the runtime table for the later virtual event.
  static EFI_RUNTIME_SERVICES Runtime;
  EFI_RT_PROPERTIES_TABLE In={EFI_RT_PROPERTIES_TABLE_VERSION,sizeof(In),Supported};
  static EFI_CONFIGURATION_TABLE Tables[2];
  static EFI_SYSTEM_TABLE System;
  Tables[0]=(EFI_CONFIGURATION_TABLE){gEfiRtPropertiesTableGuid,&In};
  Tables[1]=(EFI_CONFIGURATION_TABLE){gFdtTableGuid,Fdt};
  System=(EFI_SYSTEM_TABLE){.BootServices=gBS,.RuntimeServices=&Runtime,
    .NumberOfTableEntries=1,.ConfigurationTable=Tables};
  Runtime=Rt;
  *gBS=(EFI_BOOT_SERVICES){
    .InstallConfigurationTable=Publish,.InstallProtocolInterface=InstallProtocol,
    .LocateProtocol=Locate,.CreateEventEx=Create,.CloseEvent=Close,.RaiseTPL=Raise,.RestoreTPL=Restore
  };
  gDS->GetMemorySpaceDescriptor=Descriptor;
  mRtcEnded=mRtcReading=mRtcWriteFault=mRuntimeRtcWriteFault=FALSE;
  mRtcExitEvent=mRtcVirtualEvent=NULL;
  mRtcBoot=NULL;
  mRtcRuntimeAccess=NULL;
  mRtcRuntimeBase=0;
  mRtcWacs=RtcWacsIdle;
  ExitNotify=ReadyNotify=BeforeExitNotify=VirtualNotify=FdtNotify=NULL;
  RtcContext=NULL;
  Provider=NULL;
  RuntimeMapped=TRUE;
  AfterExit=Virtual=BusyAtEntry=FALSE;
  FailConversion=Conversions=ConversionsToVirtual=0;
  TimeNs=Machine=Commands=Writes=Clears=TriggerCount=BusyPolls=SecondsReads=0;
  StallRead=StallWrite=Rollovers=BusyAfterTrigger=0;
  InitDone=1;
  const UINT16 Initial[]={59,58,23,29,5,2,56};
  CopyMem(Calendar,Initial,sizeof(Calendar));
  CopyMem(Shadow,Initial,sizeof(Shadow));
  CHECK(MtkUfsPrmInitialize(NULL,&System)==EFI_SUCCESS && Provider);
  ConvertedRanges=mRuntimeRanges;
  for (size_t I=0;I<RUNTIME_BANK_COUNT;I++) {
    ConvertedRanges.Range[I].VirtualBaseAddress=
      I==MtkUfsPowerBankPwrap?TEST_VIRTUAL_BASE:0x1000000+I*0x10000;
  }
  for (size_t I=0;I<ARRAY_SIZE(mContextBuffers);I++)
    mContextBuffers[I].RuntimeMmioRanges=(PRM_RUNTIME_MMIO_RANGES *)&ConvertedRanges;
  CHECK(LibRtcInitialize(NULL,&System)==EFI_SUCCESS);
  CHECK(Published->RuntimeServicesSupported==(Supported&~TIME_SERVICES));
  CHECK(ExitNotify && ReadyNotify && BeforeExitNotify && VirtualNotify && FdtNotify && !Commands);
  CHECK(!mRtcFdtValid);
  System.NumberOfTableEntries=2; // direct shell boot uses BeforeExitBootServices
}

static VOID EnterRuntime(BOOLEAN EnterVirtual) {
  BeforeExitNotify(NULL,RtcContext);
  ExitNotify(NULL,NULL);
  AfterExit=TRUE;
  if (EnterVirtual) {
    VirtualNotify(NULL,NULL);
    Virtual=TRUE;
  }
}

static VOID CheckPeers(BOOLEAN Busy) {
  UFS_RUNTIME_IO_CONTEXT Io;
  MTK_PMIC_BROKER_REQUEST Request={
    .Size=sizeof(Request),.Version=MTK_PMIC_BROKER_VERSION_SD_POWER,.Operation=MtkPmicBrokerQuery
  };
  CHECK(BuildRuntimeContext(&mContextBuffers[0],&Request,&mHandlerGuid,&Io));
  CHECK(RuntimeTryLock(&Io)==!Busy);
  if (!Busy) RuntimeUnlock(NULL);
  CHECK(MtkPmicPrmHandler(&Request,&mContextBuffers[1])==EFI_SUCCESS);
  CHECK(Request.Result==(Busy?MtkPmicBrokerResultBusy:MtkPmicBrokerResultSuccess));
  CHECK(!mPowerLock);
}

static VOID ReadCalendar(VOID) {
  EFI_TIME Time={.TimeZone=-60,.Daylight=EFI_TIME_IN_DAYLIGHT};
  CHECK(LibGetTime(&Time,NULL)==EFI_SUCCESS);
  CHECK(Time.Year==2024 && Time.Month==2 && Time.Day==29 && Time.Hour==23);
  CHECK(Time.Minute==58 && Time.Second==Calendar[0] && Time.Nanosecond==0);
  CHECK(Time.TimeZone==-60 && Time.Daylight==EFI_TIME_IN_DAYLIGHT);
}

int main(int argc, char **argv) {
  static const EFI_TIME Date={.Year=2026,.Month=1,.Day=2,.Hour=3,.Minute=4,.Second=5,
                             .TimeZone=EFI_UNSPECIFIED_TIMEZONE};
  EFI_TIME Time=Date;
  CHECK(argc==2);
  FILE *File=fopen(argv[1],"rb");
  CHECK(File && fseek(File,0,SEEK_END)==0);
  long Length=ftell(File);
  CHECK(Length>0 && fseek(File,0,SEEK_SET)==0);
  VOID *Fdt=malloc((size_t)Length);
  CHECK(Fdt && fread(Fdt,1,(size_t)Length,File)==(size_t)Length);
  CHECK(fclose(File)==0);

  Boot(Fdt,ALL_SERVICES);
  EnterRuntime(FALSE);
  CHECK(Published->RuntimeServicesSupported==(ALL_SERVICES&~(TIME_SERVICES&~CALENDAR_SERVICES)));
  ReadCalendar();
  CHECK(Commands==8 && Clears==8 && !Writes);
  EFI_TIME_CAPABILITIES Capability;
  CHECK(LibGetTime(&Time,&Capability)==EFI_SUCCESS && Commands==16);
  CHECK(Capability.Resolution==1 && Capability.Accuracy==MAX_UINT32 && !Capability.SetsToZero);
  CHECK(LibGetTime(NULL,NULL)==EFI_INVALID_PARAMETER && Commands==16);

  Boot(Fdt,ALL_SERVICES);
  EnterRuntime(TRUE);
  CHECK(Conversions==2 && ConversionsToVirtual==1);
  Rollovers=1;
  ReadCalendar();
  CHECK(Commands==16 && Clears==16 && SecondsReads==4);
  Time=Date;
  Time.Day=0;
  CHECK(LibSetTime(&Time)==EFI_INVALID_PARAMETER && Commands==16);
  Time=Date;
  BusyAtEntry=TRUE;
  CHECK(LibSetTime(&Time)==EFI_NOT_READY && !Writes && !mRuntimeRtcWriteFault);
  BusyAtEntry=FALSE;
  BusyAfterTrigger=2;
  CHECK(LibSetTime(&Time)==EFI_SUCCESS && Writes==8 && TriggerCount==1 && !mRuntimeRtcWriteFault);
  CHECK(TimeNs==2000000 && Commands==29 && Clears==21);
  CHECK(LibGetTime(&Time,NULL)==EFI_SUCCESS);
  CHECK(Time.Year==Date.Year && Time.Month==Date.Month && Time.Day==Date.Day &&
        Time.Hour==Date.Hour && Time.Minute==Date.Minute && Time.Second==Date.Second);

  for (unsigned Case=0;Case<4;Case++) {
    Boot(Fdt,ALL_SERVICES);
    if (Case==0) RuntimeMapped=FALSE;
    if (Case==1) Provider=NULL;
    if (Case>=2) FailConversion=Case-1;
    EnterRuntime(TRUE);
    CHECK(!(Published->RuntimeServicesSupported&CALENDAR_SERVICES));
    CHECK(LibGetTime(&Time,NULL)==EFI_UNSUPPORTED && LibSetTime(&Time)==EFI_UNSUPPORTED && !Commands);
    CHECK(Conversions==(Case>=2?Case-1:0));
  }

  Boot(Fdt,ALL_SERVICES&~EFI_RT_SUPPORTED_SET_TIME&~EFI_RT_SUPPORTED_SET_VARIABLE);
  EnterRuntime(TRUE);
  CHECK(Published->RuntimeServicesSupported&EFI_RT_SUPPORTED_GET_TIME);
  CHECK(!(Published->RuntimeServicesSupported&(EFI_RT_SUPPORTED_SET_TIME|EFI_RT_SUPPORTED_SET_VARIABLE)));
  CHECK(LibSetTime(&Time)==EFI_UNSUPPORTED && !Commands);
  ReadCalendar();

  Boot(Fdt,ALL_SERVICES);
  EnterRuntime(TRUE);
  mPowerLock=1;
  CHECK(LibGetTime(&Time,NULL)==EFI_NOT_READY && !Commands && mPowerLock==1);
  mPowerLock=0;
  mPowerCore.WacsStage=1;
  CHECK(LibGetTime(&Time,NULL)==EFI_NOT_READY && !Commands && !mPowerLock);
  mPowerCore.WacsStage=0;
  Machine=6;
  CHECK(LibGetTime(&Time,NULL)==EFI_NOT_READY && !Clears && !Commands);
  Machine=0;
  InitDone=MAX_UINT32;
  CHECK(LibGetTime(&Time,NULL)==EFI_NOT_READY && !Commands);
  InitDone=1;

  StallRead=1;
  CHECK(LibGetTime(&Time,NULL)==EFI_TIMEOUT && Commands==1 && !Clears && TimeNs==10000000);
  CheckPeers(TRUE);
  CHECK(!Clears && Commands==1 && mRtcWacs==RtcWacsRead);
  Machine=6;
  CheckPeers(FALSE);
  CHECK(Clears==1 && Commands==1 && mRtcWacs==RtcWacsIdle);
  ReadCalendar();
  CHECK(Clears==9 && Commands==9);

  Boot(Fdt,ALL_SERVICES);
  EnterRuntime(TRUE);
  StallWrite=2;
  Time=Date;
  CHECK(LibSetTime(&Time)==EFI_TIMEOUT && Writes==2 && !TriggerCount && mRuntimeRtcWriteFault);
  CheckPeers(TRUE);
  Machine=0;
  CheckPeers(FALSE);
  CHECK(LibGetTime(&Time,NULL)==EFI_DEVICE_ERROR && LibSetTime(&Time)==EFI_DEVICE_ERROR);
  CHECK(Commands==3 && Clears==1 && Writes==2 && !TriggerCount);

  Boot(Fdt,ALL_SERVICES);
  EnterRuntime(TRUE);
  BusyAfterTrigger=2000;
  CHECK(LibSetTime(&Time)==EFI_TIMEOUT && TimeNs==1000000000ULL && TriggerCount==1);
  CHECK(mRuntimeRtcWriteFault && mRtcWacs==RtcWacsIdle);
  CheckPeers(FALSE);
  free(Fdt);
  printf("Runtime RTC: %u checks passed (EFI events, mapped time, clock commit, WACS2 ownership)\n",Checks);
  return 0;
}
