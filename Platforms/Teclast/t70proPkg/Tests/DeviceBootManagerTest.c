// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
static VOID *ShellLookupAllocation;
static VOID *ExitInputAllocation;
static UINTN ShellLookupFrees, Frees;
static EFI_TPL ConsoleTpl=TPL_APPLICATION;
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID) { return ConsoleTpl; }
static VOID EFIAPI TrackFreePool(VOID *Ptr)
{
  if (Ptr != NULL && Ptr == ShellLookupAllocation) {
    ShellLookupFrees++;
    ShellLookupAllocation=NULL;
  }
  Frees++;
  free(Ptr);
}
#define FreePool TrackFreePool
#include "../Library/DeviceBootManagerLib/DeviceBootManager.c"
#undef FreePool

EFI_GUID gEfiSimpleTextOutProtocolGuid = {1};
EFI_GUID gEfiSimpleTextInProtocolGuid = {2};
EFI_GUID gEfiAbsolutePointerProtocolGuid = {3};
EFI_GUID gEfiSimplePointerProtocolGuid = {4};
EFI_GUID gEfiUsbIoProtocolGuid = {5};
EFI_GUID gEfiSimpleFileSystemProtocolGuid = {6};
EFI_GUID gEfiGraphicsOutputProtocolGuid = {7};
EFI_GUID gEfiEventExitBootServicesGuid = {8};
EFI_GUID gEfiLoadedImageProtocolGuid = {9};
EFI_HANDLE gImageHandle = (EFI_HANDLE)0x1122;
static EFI_SYSTEM_TABLE TestSystemTable;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL TraceConsole;
EFI_SYSTEM_TABLE *gST = &TestSystemTable;
static UINTN LogoCalls;
static EFI_STATUS LogoStatus;
EFI_STATUS EFIAPI BootLogoEnableLogo(VOID) { LogoCalls++;return LogoStatus; }
static BOOLEAN TraceEnabled, TraceBadGop, TraceHugeCounts, TraceLookupError;
static UINT8 BdsRecord[72], BdsHistory[32][72];
static UINTN BdsFlushes, DiagnosticLookups;
static BOOLEAN TrialCookie, SetupLookup;
static UINTN TrialBad, WdtReloads, TrialCreates, TrialCancels, TrialCloses, SetupBoots, SetupFrees;
static EFI_STATUS TrialCreateStatus, TrialTimerStatus, SetupPathStatus, SetupLoadStatus, SetupStartStatus;
static UINTN SetupLoads, SetupUnloads, FirmwareWatchdogStops;
static EFI_STATUS FirmwareWatchdogStatus;
static EFI_EVENT_NOTIFY TrialNotify;
static UINTN InputCreates,InputExitCreates,InputRegisters,InputConnections,InputCloses;
static EFI_STATUS InputCreateStatus,InputExitStatus,InputRegisterStatus,InputConnectStatus,InputCloseStatus;
static UINTN OutputCreates,OutputRegisters,OutputConnections,GopConnections,OutputFailure,OutputCloses;
static BOOLEAN LateOutputMode,GopPresent,TextOutPresent,LiveOutput,ContinuousOutputNotify;
static BOOLEAN OutputRegistered,ErrorRegistered;
static BOOLEAN InputRegistered[7],InputReenter,HideLateInput,PublishLateInput,ContinuousInputNotify,ExitInputConnect;
static EFI_STATUS EFIAPI TestInputExitCreate(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,
  CONST VOID *Context,CONST EFI_GUID *Group,EFI_EVENT *Event) {
  CHECK(Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_NOTIFY && Notify==InputConsoleExit && Context==NULL);
  CHECK(Group==&gEfiEventExitBootServicesGuid);InputExitCreates++;
  if(EFI_ERROR(InputExitStatus))return InputExitStatus;
  *Event=(EFI_EVENT)0x8811;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestInputRegister(EFI_GUID *Guid,EFI_EVENT Event,VOID **Registration) {
  if(Guid==&gEfiGraphicsOutputProtocolGuid || Guid==&gEfiSimpleTextOutProtocolGuid) {
    CHECK(Registration && (Event==(EFI_EVENT)0x9900 || Event==(EFI_EVENT)0x9901));
    OutputRegisters++;if(OutputFailure==2)return EFI_DEVICE_ERROR;
    *Registration=(VOID *)0x99aa;return EFI_SUCCESS;
  }
  CHECK(Guid==&gEfiSimpleTextInProtocolGuid && Event==(EFI_EVENT)0x8899 && Registration);
  InputRegisters++;if(EFI_ERROR(InputRegisterStatus))return InputRegisterStatus;
  *Registration=(VOID *)0x88aa;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestInputConnect(EFI_HANDLE Handle,EFI_HANDLE *Drivers,
  EFI_DEVICE_PATH_PROTOCOL *Path,BOOLEAN Recursive) {
  UINTN Id=(UINTN)Handle;
  if(Id==1) {
    CHECK(!Drivers && !Path && (!Recursive || ConsoleTpl==TPL_APPLICATION));
    if(LateOutputMode && !TextOutPresent) {
      GopConnections++;TextOutPresent=TRUE;
      OutputConsoleNotify(mOutputNotifyEvents[1],NULL);
    } else {
      OutputConnections++;
      if(LateOutputMode) { CHECK(OutputRegistered && ErrorRegistered);LiveOutput=TRUE; }
    }
    if(ContinuousOutputNotify)OutputConsoleNotify(mOutputNotifyEvents[0],NULL);
    return EFI_SUCCESS;
  }
  CHECK(Id>=2 && Id<=4 && Drivers==NULL && Path==NULL && (!Recursive || ConsoleTpl==TPL_APPLICATION));
  CHECK(InputRegistered[Id]);InputConnections++;
  if(ContinuousInputNotify)InputConsoleNotify(mInputNotifyEvent,NULL);
  if(ExitInputConnect) { ExitInputConnect=FALSE;InputConsoleExit(mInputExitEvent,NULL); }
  if(PublishLateInput && Id==2) {
    PublishLateInput=FALSE;HideLateInput=FALSE;
    InputConsoleNotify(mInputNotifyEvent,NULL);
  }
  if(InputReenter) {
    InputReenter=FALSE;UINTN Before=InputConnections;
    InputConsoleNotify(mInputNotifyEvent,NULL);CHECK(InputConnections==Before);
  }
  return InputConnectStatus;
}
static EFI_STATUS EFIAPI StopFirmwareWatchdog(UINTN Timeout,UINT64 Code,UINTN Size,CHAR16 *Data) {
  CHECK(Timeout==0 && Code==0 && Size==0 && Data==NULL);
  FirmwareWatchdogStops++;return FirmwareWatchdogStatus;
}
static EFI_STATUS EFIAPI TestTrialCreate(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,
  VOID *Context,EFI_EVENT *Event) {
  if(Notify==OutputConsoleNotify) {
    CHECK(Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_CALLBACK && !Context);
    *Event=(EFI_EVENT)(UINTN)(0x9900+OutputCreates++);
    if(OutputFailure==1) { *Event=(EFI_EVENT)0xdead;return EFI_OUT_OF_RESOURCES; }
    return EFI_SUCCESS;
  }
  if(Notify==InputConsoleNotify) {
    CHECK(Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_CALLBACK && Context==NULL);
    InputCreates++;if(EFI_ERROR(InputCreateStatus)) { *Event=(EFI_EVENT)0xdead;return InputCreateStatus; }
    *Event=(EFI_EVENT)0x8899;return EFI_SUCCESS;
  }
  CHECK(Type==(EVT_TIMER|EVT_NOTIFY_SIGNAL) && Tpl==TPL_NOTIFY && Context==NULL);
  TrialCreates++;if(EFI_ERROR(TrialCreateStatus))return TrialCreateStatus;
  TrialNotify=Notify;*Event=(EFI_EVENT)0x7788;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestTrialSet(EFI_EVENT Event,EFI_TIMER_DELAY Type,UINT64 Period) {
  CHECK(Event==(EFI_EVENT)0x7788);
  if(Type==TimerCancel){CHECK(Period==0);TrialCancels++;return EFI_SUCCESS;}
  CHECK(Type==TimerPeriodic && Period==10000000ULL);return TrialTimerStatus;
}
static EFI_STATUS EFIAPI TestTrialClose(EFI_EVENT Event) {
  if(Event==(EFI_EVENT)0x9900 || Event==(EFI_EVENT)0x9901) { OutputCloses++;return EFI_SUCCESS; }
  if(Event==(EFI_EVENT)0x8899) { InputCloses++;return InputCloseStatus; }
  CHECK(Event==(EFI_EVENT)0x7788);TrialCloses++;return EFI_SUCCESS;
}
UINT64 EFIAPI MmioRead64(UINTN Address) {
  if(Address==0x43F7F000) return TraceEnabled?0x0000000148303754ULL:0;
  if(Address==T70_TRIAL_RECORD)return TrialCookie?T70_TRIAL_MAGIC:0;
  if(Address==T70_TRIAL_RECORD+8)return TrialBad==1?0:T70_TRIAL_INVERSE;
  if(Address==T70_TRIAL_RECORD+24)return TrialBad==4?3:2;
  if(Address==0x43F7F060) return 0x0000000147503754ULL;
  CHECK(Address==0x43F7F070);return TraceBadGop?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
UINT32 EFIAPI MmioRead32(UINTN Address) {
  if(Address==T70_WDT_BASE)return TrialBad==2?0x135:0x134;
  if(Address==T70_WDT_BASE+4)return TrialBad==3?0xF800:0xF000;
  if(Address==0x43F7F810){UINT32 Stage;memcpy(&Stage,BdsRecord+16,4);return Stage;}
  CHECK(Address==0x43F7F068);return 8;
}
UINT64 EFIAPI MmioWrite64(UINTN Address,UINT64 Value) {
  CHECK(Address==0x43F7F800);memcpy(BdsRecord,&Value,8);return Value;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value) {
  if(Address==T70_WDT_BASE+8){CHECK(Value==T70_WDT_RELOAD && TrialCookie && TrialBad==0);WdtReloads++;return Value;}
  CHECK(Address>=0x43F7F808 && Address<=0x43F7F844 && !(Address&3));
  memcpy(BdsRecord+(Address-0x43F7F800),&Value,4);return Value;
}
VOID EFIAPI MemoryFence(VOID) { __sync_synchronize(); }
VOID *EFIAPI WriteBackDataCacheRange(VOID *Address,UINTN Length) {
  UINT64 Magic;UINT32 Tail[2];
  CHECK((UINTN)Address==0x43F7F800 && (Length==72 || Length==8));
  if(Length==8){memcpy(&Magic,BdsRecord,8);CHECK(Magic==0 && BdsFlushes<32);
    memcpy(BdsHistory[BdsFlushes++],BdsRecord,72);return Address;}
  memcpy(&Magic,BdsRecord,8);memcpy(Tail,BdsRecord+64,8);
  CHECK(Magic==0x000000074C443754ULL && Tail[0]<=8 && Tail[1]<0x20000 && BdsFlushes<32);
  memcpy(BdsHistory[BdsFlushes++],BdsRecord,72);return Address;
}
static EFI_DEVICE_PATH_PROTOCOL Paths[7];
static UINTN Connects, Refreshes, Lookups, Registrations;
static UINTN ConOutCount, ErrOutCount, ConInCount;
static BOOLEAN Ready, Empty, Reenter;
static EFI_GUID *FailGuid;
static EFI_STATUS UpdateStatus;
static EFI_STATUS ShellLookupStatus, ShellInitStatus;
static UINTN ShellLookups, ShellInitializations, ShellBoots, ShellOptionFrees;
static BOOLEAN ShellReenter;
static EFI_LOADED_IMAGE_PROTOCOL RecoveryImage;
static EFI_STATUS RecoveryLoadStatus,RecoveryProtocolStatus;
static UINTN RecoveryLoads,RecoveryUnloads,LegacyBootCalls;
static EFI_STATUS EFIAPI RecoveryProtocol(EFI_HANDLE Image,EFI_GUID *Guid,VOID **Out) {
  CHECK(mRecoveryShellActive && Image==(EFI_HANDLE)0x7788 && Guid==&gEfiLoadedImageProtocolGuid);
  if(EFI_ERROR(RecoveryProtocolStatus))return RecoveryProtocolStatus;
  *Out=&RecoveryImage;return EFI_SUCCESS;
}

EFI_STATUS EFIAPI GetFileDevicePathFromAnyFv(CONST EFI_GUID *Guid,
  EFI_SECTION_TYPE Type, UINTN Instance, EFI_DEVICE_PATH_PROTOCOL **Path)
{
  SetupLookup=Guid!=NULL && Guid->Data1==mSetupGuid.Data1;
  if(SetupLookup) {
    CHECK(CompareGuid(Guid,&mSetupGuid) && mSetupAttempted && !mSetupActive);
    CHECK(Type==EFI_SECTION_PE32 && Instance==0 && Path!=NULL && *Path==NULL);
    if(EFI_ERROR(SetupPathStatus)) return SetupPathStatus;
    ShellLookupAllocation=malloc(sizeof(EFI_DEVICE_PATH_PROTOCOL));CHECK(ShellLookupAllocation!=NULL);
    *Path=ShellLookupAllocation;return EFI_SUCCESS;
  }
  CHECK(Guid != NULL && Guid->Data1 == 0x7c04a583 && Guid->Data2 == 0x9e3e &&
        Guid->Data3 == 0x4f1c && Guid->Data4[0] == 0xad && Guid->Data4[7] == 0xd1);
  CHECK(Type == EFI_SECTION_PE32 && Instance == 0 && Path != NULL);
  CHECK(*Path == NULL && !mRecoveryShellActive);
  ShellLookups++;
  if (EFI_ERROR(ShellLookupStatus)) return ShellLookupStatus;
  ShellLookupAllocation=malloc(sizeof(EFI_DEVICE_PATH_PROTOCOL));
  CHECK(ShellLookupAllocation != NULL);
  *Path=ShellLookupAllocation;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI EfiBootManagerInitializeLoadOption(
  EFI_BOOT_MANAGER_LOAD_OPTION *Option, UINTN Number,
  EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type, UINT32 Attributes, CHAR16 *Description,
  EFI_DEVICE_PATH_PROTOCOL *Path, UINT8 *Data, UINT32 DataSize)
{
  static CONST CHAR16 ExpectedArguments[]=L"-nostartup";
  static CONST CHAR16 ExpectedDescription[]=L"UEFI Shell";
  if(SetupLookup) {
    static CONST CHAR16 SetupName[]=L"UEFI Firmware Settings";
    CHECK(Option!=NULL && Number==LoadOptionNumberUnassigned && Type==LoadOptionTypeBoot);
    CHECK(Attributes==(LOAD_OPTION_ACTIVE|LOAD_OPTION_CATEGORY_APP));
    CHECK(Path==ShellLookupAllocation && Data==NULL && DataSize==0);
    CHECK(memcmp(Description,SetupName,sizeof(SetupName))==0);
    memset(Option,0,sizeof(*Option));Option->FilePath=malloc(sizeof(EFI_DEVICE_PATH_PROTOCOL));
    CHECK(Option->FilePath!=NULL);return EFI_SUCCESS;
  }
  CHECK(Option != NULL && Number == LoadOptionNumberUnassigned && Type == LoadOptionTypeBoot);
  CHECK(Attributes == (LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP));
  CHECK(Path == ShellLookupAllocation && Path != NULL && !mRecoveryShellActive);
  CHECK(DataSize == sizeof(ExpectedArguments) && memcmp(Data,ExpectedArguments,DataSize)==0);
  CHECK(memcmp(Description,ExpectedDescription,sizeof(ExpectedDescription))==0);
  ShellInitializations++;
  if (EFI_ERROR(ShellInitStatus)) return ShellInitStatus;
  memset(Option,0,sizeof(*Option));
  Option->FilePath=malloc(sizeof(EFI_DEVICE_PATH_PROTOCOL));CHECK(Option->FilePath != NULL);
  Option->OptionalData=malloc(DataSize);CHECK(Option->OptionalData != NULL);
  memcpy(Option->OptionalData,Data,DataSize);Option->OptionalDataSize=DataSize;
  return EFI_SUCCESS;
}

VOID EFIAPI EfiBootManagerBoot(EFI_BOOT_MANAGER_LOAD_OPTION *Option)
{
  LegacyBootCalls++;
  CHECK(!mSetupActive);
  CHECK(mRecoveryShellActive && ShellLookupAllocation==NULL && ShellLookupFrees==1);
  CHECK(Option->FilePath != NULL && Option->OptionalDataSize==sizeof(mRecoveryShellArguments));
  ShellBoots++;
  if (ShellReenter) {
    ShellReenter=FALSE;
    DeviceBootManagerUnableToBoot();
    CHECK(ShellBoots==1 && ShellLookups==1);
  }
  Option->Status=EFI_SUCCESS;
}

EFI_STATUS EFIAPI EfiBootManagerFreeLoadOption(EFI_BOOT_MANAGER_LOAD_OPTION *Option)
{
  if(SetupLookup) {
    CHECK(!mSetupActive && Option->FilePath!=NULL && Option->OptionalData==NULL);
    free(Option->FilePath);memset(Option,0,sizeof(*Option));SetupFrees++;return EFI_SUCCESS;
  }
  CHECK(!mRecoveryShellActive && Option->FilePath != NULL && Option->OptionalData != NULL);
  free(Option->FilePath);free(Option->OptionalData);memset(Option,0,sizeof(*Option));
  ShellOptionFrees++;
  return EFI_SUCCESS;
}


static EFI_STATUS EFIAPI SetupLoad(BOOLEAN Policy,EFI_HANDLE Parent,
  EFI_DEVICE_PATH_PROTOCOL *Path,VOID *Buffer,UINTN Size,EFI_HANDLE *Image) {
  if(mRecoveryShellActive) {
    CHECK(!Policy && Parent==gImageHandle && Path && !Buffer && !Size && Image && !*Image);
    RecoveryLoads++;
    if(RecoveryLoadStatus==EFI_SECURITY_VIOLATION)*Image=(EFI_HANDLE)0x7788;
    else if(EFI_ERROR(RecoveryLoadStatus))*Image=(EFI_HANDLE)0xdead;
    if(EFI_ERROR(RecoveryLoadStatus))return RecoveryLoadStatus;
    *Image=(EFI_HANDLE)0x7788;return EFI_SUCCESS;
  }
  CHECK(!Policy && Parent==gImageHandle && Path==ShellLookupAllocation && Path!=NULL);
  CHECK(Buffer==NULL && Size==0 && Image!=NULL && *Image==NULL);
  CHECK(!mSetupActive && mSetupLaunchPhase==3);
  SetupLoads++;
  if(SetupLoadStatus==EFI_SECURITY_VIOLATION)*Image=(EFI_HANDLE)0x5566;
  else if(EFI_ERROR(SetupLoadStatus))*Image=(EFI_HANDLE)0xdead;
  if(EFI_ERROR(SetupLoadStatus))return SetupLoadStatus;
  *Image=(EFI_HANDLE)0x5566;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetupUnload(EFI_HANDLE Image) {
  if(mRecoveryShellActive){CHECK(Image==(EFI_HANDLE)0x7788);RecoveryUnloads++;return EFI_SUCCESS;}
  CHECK(Image==(EFI_HANDLE)0x5566 && !mSetupActive);
  SetupUnloads++;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetupStart(EFI_HANDLE Image,UINTN *ExitSize,CHAR16 **ExitData) {
  if(mRecoveryShellActive) {
    CHECK(Image==(EFI_HANDLE)0x7788 && !ExitSize && !ExitData);
    CHECK(RecoveryImage.LoadOptionsSize==sizeof(mRecoveryShellArguments));
    CHECK(!memcmp(RecoveryImage.LoadOptions,mRecoveryShellArguments,sizeof(mRecoveryShellArguments)));
    ShellBoots++;
    if(ShellReenter){ShellReenter=FALSE;DeviceBootManagerUnableToBoot();CHECK(ShellBoots==1&&ShellLookups==1);}
    return EFI_SUCCESS;
  }
  CHECK(Image==(EFI_HANDLE)0x5566 && ExitSize==NULL && ExitData==NULL);
  CHECK(mSetupActive && !mRecoveryShellActive && FirmwareWatchdogStops==1 && ShellLookupAllocation==NULL);
  CHECK(mSetupLaunchPhase==7);
  SetupBoots++;
  RecordBdsProgress(10);DeviceBootManagerOnDemandConInConnect();
  UINT32 Stage;memcpy(&Stage,BdsRecord+16,4);CHECK(Stage==10);
  return SetupStartStatus;
}

BOOLEAN EFIAPI CompareGuid(CONST EFI_GUID *A, CONST EFI_GUID *B)
{ return memcmp(A,B,sizeof(*A)) == 0; }
EFI_DEVICE_PATH_PROTOCOL * EFIAPI DevicePathFromHandle(EFI_HANDLE Handle)
{
  UINTN Id = (UINTN)Handle;
  CHECK(Id >= 1 && Id <= 6);
  return Id == 5 ? NULL : &Paths[Id];
}
BOOLEAN EFIAPI IsDevicePathValid(CONST EFI_DEVICE_PATH_PROTOCOL *Path, UINTN MaxSize)
{
  CHECK(MaxSize == 0);
  return Path != &Paths[6];
}
static EFI_STATUS EFIAPI LocateHandles(EFI_LOCATE_SEARCH_TYPE Search, EFI_GUID *Guid,
  VOID *Key, UINTN *Count, EFI_HANDLE **Handles)
{
  static CONST UINTN OutputHandles[] = {1,5,6};
  static CONST UINTN InputHandles[] = {2,3,4,5,6};
  CONST UINTN *Ids;
  UINTN N, I;
  CHECK(Search == ByProtocol && Key == NULL);
  if(Guid==&gEfiGraphicsOutputProtocolGuid && LateOutputMode && !TraceEnabled) {
    if(!GopPresent)return EFI_NOT_FOUND;
    *Count=2;*Handles=malloc(2*sizeof(EFI_HANDLE));CHECK(*Handles);
    (*Handles)[0]=(EFI_HANDLE)1;(*Handles)[1]=(EFI_HANDLE)5;return EFI_SUCCESS;
  }
  if(Guid==&gEfiSimpleTextOutProtocolGuid && LateOutputMode && !TextOutPresent)return EFI_NOT_FOUND;
  if(Guid!=&gEfiSimpleTextInProtocolGuid && Guid!=&gEfiSimpleTextOutProtocolGuid) {
    CHECK(TraceEnabled);DiagnosticLookups++;
    if(TraceLookupError) return EFI_DEVICE_ERROR;
    if(Guid==&gEfiSimplePointerProtocolGuid) return EFI_NOT_FOUND;
    if(Guid==&gEfiAbsolutePointerProtocolGuid || Guid==&gEfiSimpleFileSystemProtocolGuid ||
       Guid==&gEfiGraphicsOutputProtocolGuid) *Count=1;
    else { CHECK(Guid==&gEfiUsbIoProtocolGuid);*Count=2; }
    if(TraceHugeCounts) *Count=65536;
    *Handles=malloc(sizeof(EFI_HANDLE));CHECK(*Handles!=NULL);return EFI_SUCCESS;
  }
  CHECK(Ready);
  Lookups++;
  if (Empty || Guid == FailGuid) return EFI_NOT_FOUND;
  if (Guid == &gEfiSimpleTextOutProtocolGuid) { Ids=OutputHandles; N=3; }
  else { CHECK(Guid == &gEfiSimpleTextInProtocolGuid); Ids=InputHandles; N=5; }
  *Count=N; *Handles=malloc(N*sizeof(EFI_HANDLE)); CHECK(*Handles != NULL);
  if (ExitInputConnect && Guid == &gEfiSimpleTextInProtocolGuid) ExitInputAllocation=*Handles;
  for (I=0; I<N; I++) {
    // The first input is SimpleTextIn-only; no SimpleTextInEx protocol is present.
    (*Handles)[I]=(EFI_HANDLE)((Guid==&gEfiSimpleTextInProtocolGuid && HideLateInput && Ids[I]==4)?5:Ids[I]);
  }
  return EFI_SUCCESS;
}
VOID EFIAPI EfiBootManagerConnectAll(VOID)
{
  Connects++;
  if (Reenter) {
    UINTN Count=Connects;
    Reenter=FALSE;
    CHECK(DeviceBootManagerBeforeConsole(NULL,NULL) == NULL);
    CHECK(Connects == Count);
  }
  // Model recursive connection finishing the actual GOP, USB HID and FAT
  // children before the policy queries protocols or refreshes boot options.
  Ready=TRUE;
}
EFI_STATUS EFIAPI EfiBootManagerUpdateConsoleVariable(CONSOLE_TYPE Type,
  EFI_DEVICE_PATH_PROTOCOL *Add, EFI_DEVICE_PATH_PROTOCOL *Remove)
{
  CHECK(Ready && Remove == NULL && Add != NULL);
  CHECK(Add != &Paths[5] && Add != &Paths[6]);
  if (Type == ConOut) { CHECK(Add == &Paths[1]); ConOutCount++;if(!EFI_ERROR(UpdateStatus))OutputRegistered=TRUE; }
  else if (Type == ErrOut) { CHECK(Add == &Paths[1]); ErrOutCount++;if(!EFI_ERROR(UpdateStatus))ErrorRegistered=TRUE; }
  else { CHECK(Type == ConIn && Add >= &Paths[2] && Add <= &Paths[4]); ConInCount++; }
  Registrations++;
  if(Type==ConIn && !EFI_ERROR(UpdateStatus))InputRegistered[Add-Paths]=TRUE;
  return UpdateStatus;
}
VOID EFIAPI EfiBootManagerRefreshAllBootOption(VOID)
{
  CHECK(Ready && !mConnecting);
  Refreshes++;
}
static VOID Reset(VOID)
{
  CHECK(ShellLookupAllocation==NULL);
  ShellLookupStatus=ShellInitStatus=EFI_SUCCESS;
  RecoveryLoadStatus=RecoveryProtocolStatus=EFI_SUCCESS;
  RecoveryLoads=RecoveryUnloads=LegacyBootCalls=0;
  memset(&RecoveryImage,0,sizeof RecoveryImage);Services.HandleProtocol=RecoveryProtocol;
  ShellLookups=ShellInitializations=ShellBoots=ShellOptionFrees=ShellLookupFrees=0;
  ShellReenter=mRecoveryShellActive=FALSE;
  Services.LocateHandleBuffer=LocateHandles;
  Services.CreateEventEx=TestInputExitCreate;Services.RegisterProtocolNotify=TestInputRegister;
  Services.ConnectController=TestInputConnect;
  InputCreates=InputExitCreates=InputRegisters=InputConnections=InputCloses=0;
  InputCreateStatus=InputExitStatus=InputRegisterStatus=InputConnectStatus=InputCloseStatus=EFI_SUCCESS;
  memset(InputRegistered,0,sizeof(InputRegistered));InputReenter=FALSE;
  mInputNotifyStarted=mInputNotifyActive=mInputServicesEnded=FALSE;
  mOutputNotifyStarted=mOutputRescanPending=FALSE;
  memset(mOutputNotifyEvents,0,sizeof(mOutputNotifyEvents));memset(mOutputNotifyActive,0,sizeof(mOutputNotifyActive));
  OutputCreates=OutputRegisters=OutputConnections=GopConnections=OutputFailure=OutputCloses=0;
  LateOutputMode=GopPresent=TextOutPresent=LiveOutput=ContinuousOutputNotify=FALSE;
  OutputRegistered=ErrorRegistered=FALSE;
  mInputNotifyEvent=mInputExitEvent=NULL;mInputRegistration=NULL;
  mInputRescanPending=FALSE; mConnecting=FALSE; Ready=Empty=Reenter=FALSE;HideLateInput=PublishLateInput=ContinuousInputNotify=ExitInputConnect=FALSE; FailGuid=NULL;
  TraceEnabled=TraceBadGop=TraceHugeCounts=TraceLookupError=FALSE;
  BdsFlushes=DiagnosticLookups=0;memset(BdsRecord,0,sizeof(BdsRecord));
  memset(&TestSystemTable,0,sizeof(TestSystemTable));
  mRegisteredInputCount=mRegisteredOutputCount=mTraceSequence=0;
  TrialCookie=SetupLookup=FALSE;TrialBad=WdtReloads=TrialCreates=TrialCancels=TrialCloses=SetupBoots=SetupFrees=0;
  TrialCreateStatus=TrialTimerStatus=SetupPathStatus=SetupLoadStatus=SetupStartStatus=EFI_SUCCESS;
  SetupLoads=SetupUnloads=mSetupLaunchPhase=mSetupLaunchStatus=0;
  TrialNotify=NULL;TimeNs=0;
  mSetupActive=mSetupAttempted=FALSE;
  FirmwareWatchdogStops=0;FirmwareWatchdogStatus=EFI_SUCCESS;
  Services.SetWatchdogTimer=StopFirmwareWatchdog;
  Services.CreateEvent=TestTrialCreate;Services.SetTimer=TestTrialSet;Services.CloseEvent=TestTrialClose;
  Services.LoadImage=SetupLoad;Services.StartImage=SetupStart;Services.UnloadImage=SetupUnload;
  Connects=Refreshes=Lookups=Registrations=0;
  ConOutCount=ErrOutCount=ConInCount=0; UpdateStatus=EFI_SUCCESS;
  LogoCalls=0;LogoStatus=EFI_SUCCESS;
}
static CHAR16 DiagnosticLine[1024];
static UINTN DiagnosticPrints, DiagnosticLength, DiagnosticColumns, DiagnosticLimit;
static EFI_STATUS DiagnosticPrintStatus, DiagnosticQueryStatus;
static BOOLEAN DiagnosticBeforeShell;
static EFI_STATUS EFIAPI DiagnosticQuery(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Mode, UINTN *Cols, UINTN *Rows) {
  (VOID)This;CHECK(Mode==0);*Cols=DiagnosticColumns;*Rows=25;
  return DiagnosticQueryStatus;
}
static EFI_STATUS EFIAPI DiagnosticOutput(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *Text) {
  (VOID)This;DiagnosticPrints++;
  if(DiagnosticBeforeShell)CHECK(ShellLookups==0 && ShellBoots==0);
  UINTN n=0,col=0;
  while(Text[n]){
    CHECK(DiagnosticLength<ARRAY_SIZE(DiagnosticLine)-1);
    if(Text[n]==L'\r'){CHECK(Text[n+1]==L'\n');col=0;}
    else if(Text[n]!=L'\n'){col++;CHECK(col<DiagnosticLimit);}
    DiagnosticLine[DiagnosticLength++]=Text[n++];
  }
  DiagnosticLine[DiagnosticLength]=0;
  ShowBootDiagnostic();
  return DiagnosticPrintStatus;
}
static void TestBootDiagnostic(void) {
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Out={0};EFI_SIMPLE_TEXT_OUTPUT_MODE Mode={0};
  Reset();DiagnosticPrints=DiagnosticLength=0;DiagnosticPrintStatus=DiagnosticQueryStatus=EFI_SUCCESS;
  ShowBootDiagnostic();CHECK(DiagnosticPrints==0);
  TestSystemTable.ConOut=&Out;ShowBootDiagnostic();CHECK(DiagnosticPrints==0);
  Out.OutputString=DiagnosticOutput;Out.QueryMode=DiagnosticQuery;Out.Mode=&Mode;TraceEnabled=TRUE;
  mRegisteredInputCount=3;mRegisteredOutputCount=1;mSetupLaunchPhase=4;mSetupLaunchStatus=0x10001;
  CONST CHAR16 Expected[]=L"T70D1I=0003O=0001F=0001P=00000004E=00010001";
  UINTN Widths[]={2,20,40,80,MAX_UINTN};
  for(UINTN w=0;w<ARRAY_SIZE(Widths)+3;w++) {
    DiagnosticPrints=DiagnosticLength=0;DiagnosticQueryStatus=EFI_SUCCESS;Out.Mode=&Mode;Out.QueryMode=DiagnosticQuery;
    DiagnosticColumns=w<ARRAY_SIZE(Widths)?Widths[w]:MAX_UINTN;
    DiagnosticLimit=w<ARRAY_SIZE(Widths)?Widths[w]:20;
    if(w==ARRAY_SIZE(Widths))DiagnosticQueryStatus=EFI_DEVICE_ERROR;
    if(w==ARRAY_SIZE(Widths)+1)Out.Mode=NULL;
    if(w==ARRAY_SIZE(Widths)+2)Out.QueryMode=NULL;
    ShowBootDiagnostic();
    CHAR16 Compact[80];UINTN n=0;
    for(UINTN i=0;i<DiagnosticLength;i++)if(DiagnosticLine[i]!=L' '&&DiagnosticLine[i]!=L'\r'&&DiagnosticLine[i]!=L'\n')Compact[n++]=DiagnosticLine[i];
    Compact[n]=0;CHECK(!memcmp(Compact,Expected,sizeof Expected));
    UINTN Lines=0;
    for(UINTN i=0;i<DiagnosticLength;i++)if(DiagnosticLine[i]==L'\n')Lines++;
    // Includes the initial blank line: one photo needs at most five rows at
    // 20 columns, three at 40, or two at 80. All fields survive wrapping.
    if(DiagnosticLimit==20)CHECK(Lines<=5);
    if(DiagnosticLimit==40)CHECK(Lines<=3);
    if(DiagnosticLimit==80)CHECK(Lines<=2);
  }
  Out.Mode=&Mode;Out.QueryMode=DiagnosticQuery;DiagnosticQueryStatus=EFI_SUCCESS;
  DiagnosticColumns=1;DiagnosticPrints=DiagnosticLength=0;ShowBootDiagnostic();CHECK(DiagnosticPrints==0);
  DiagnosticColumns=40;DiagnosticLimit=40;DiagnosticPrintStatus=EFI_DEVICE_ERROR;
  ShowBootDiagnostic();CHECK(DiagnosticPrints==1); // Stop at first output failure.
  DiagnosticPrintStatus=EFI_SUCCESS;mInputServicesEnded=TRUE;ShowBootDiagnostic();CHECK(DiagnosticPrints==1);
  Reset();TraceEnabled=TRUE;TestSystemTable.ConOut=&Out;ShellLookupStatus=EFI_NOT_FOUND;
  DiagnosticPrints=DiagnosticLength=0;DiagnosticBeforeShell=TRUE;
  DeviceBootManagerUnableToBoot();CHECK(DiagnosticPrints>1&&ShellLookups==1);
  DiagnosticBeforeShell=FALSE;
  Reset();
}
static UINT32 BdsWord(UINTN Index) {
  UINT32 Value;CHECK(Index<14);memcpy(&Value,BdsRecord+16+Index*4,4);return Value;
}
static void TestBdsProgress(void) {
  Reset();TraceEnabled=TRUE;Ready=TRUE;
  TestSystemTable.ConsoleInHandle=(VOID *)1;TestSystemTable.ConIn=(VOID *)2;
  TestSystemTable.ConsoleOutHandle=(VOID *)3;TestSystemTable.ConOut=&TraceConsole;
  DeviceBootManagerBdsEntry();
  CHECK(BdsFlushes==2 && DiagnosticLookups==5 && BdsWord(0)==1 && BdsWord(1)==1);
  CHECK(BdsWord(2)==0 && BdsWord(3)==0 && BdsWord(4)==1 && BdsWord(5)==0 &&
        BdsWord(6)==2 && BdsWord(7)==1 && BdsWord(8)==1 && BdsWord(9)==1 && BdsWord(10)==1);
  CHECK(DeviceBootManagerBeforeConsole(NULL,NULL)==NULL && BdsWord(0)==3);
  CHECK(BdsWord(2)==3 && BdsWord(3)==1 && BdsWord(11)==0);
  CHECK(DeviceBootManagerAfterConsole()==NULL && BdsWord(0)==5 && BdsWord(1)==5);
  CHECK(DeviceBootManagerOnDemandConInConnect()==NULL && BdsWord(0)==6);
  DeviceBootManagerUnableToBoot();
  CHECK(BdsWord(0)==8 && BdsWord(11)==0 && ShellBoots==1);
  UINT32 Stage,Flags;memcpy(&Stage,BdsHistory[BdsFlushes-2]+16,4);
  memcpy(&Flags,BdsHistory[BdsFlushes-2]+60,4);
  CHECK(Stage==7 && Flags==2);
  TraceBadGop=TRUE;UINTN Before=BdsFlushes;
  DeviceBootManagerBdsEntry();
  UINT64 StaleMagic;memcpy(&StaleMagic,BdsRecord,sizeof StaleMagic);
  CHECK(StaleMagic==0); // An earlier valid snapshot must not survive this boot entry.
  CHECK(BdsFlushes==Before+1);
  Reset();TraceEnabled=TRUE;TraceLookupError=TRUE;
  DeviceBootManagerBdsEntry();
  CHECK(BdsWord(0)==1 && BdsWord(4)==0 && BdsWord(6)==0 && BdsWord(9)==0);
  Reset();TraceEnabled=TRUE;TraceHugeCounts=TRUE;mTraceSequence=MAX_UINT16;
  DeviceBootManagerBdsEntry();
  CHECK(BdsWord(1)==MAX_UINT16 && BdsWord(4)==MAX_UINT16 && BdsWord(6)==MAX_UINT16);
}

static void TestInteractiveSetupSession(void) {
  Reset();CHECK(PrepareRamTrialSetup()==EFI_NOT_READY && FirmwareWatchdogStops==0);
  for(UINTN Bad=1;Bad<=4;Bad++) {
    if(Bad==3)continue; // Old length contents do not arm a disabled watchdog.
    Reset();TraceEnabled=TrialCookie=TRUE;TrialBad=Bad;
    CHECK(PrepareRamTrialSetup()==EFI_NOT_READY && FirmwareWatchdogStops==0);
  }
  Reset();TraceEnabled=TrialCookie=TRUE;TrialBad=3;
  CHECK(PrepareRamTrialSetup()==EFI_SUCCESS && FirmwareWatchdogStops==1);
  CHECK(TrialCreates==0 && WdtReloads==0 && TrialCancels==0);
  Reset();TraceEnabled=TrialCookie=TRUE;
  CHECK(PrepareRamTrialSetup()==EFI_SUCCESS && FirmwareWatchdogStops==1);
  // Waiting arbitrarily long creates no feed callback and arms no deadline.
  TimeNs=86400000000000ULL;
  CHECK(ValidRamTrial() && TrialCreates==0 && TrialNotify==NULL && WdtReloads==0);
  Reset();TraceEnabled=TrialCookie=TRUE;FirmwareWatchdogStatus=EFI_DEVICE_ERROR;
  CHECK(PrepareRamTrialSetup()==EFI_DEVICE_ERROR && FirmwareWatchdogStops==1);
  Reset();TraceEnabled=TrialCookie=TRUE;Ready=TRUE;
  LaunchRamTrialSetup();
  CHECK(SetupBoots==1 && SetupLoads==1 && SetupUnloads==0 && mSetupAttempted && !mSetupActive);
  CHECK(FirmwareWatchdogStops==1 && WdtReloads==0 && TrialCreates==0 && TrialCancels==0 && TrialCloses==0);
  CHECK(BdsWord(12)==8 && BdsWord(13)==0 && (BdsWord(11)&4)==0);
  LaunchRamTrialSetup();CHECK(SetupBoots==1 && FirmwareWatchdogStops==1);
  Reset();TraceEnabled=TrialCookie=TRUE;Ready=TRUE;FirmwareWatchdogStatus=EFI_DEVICE_ERROR;
  LaunchRamTrialSetup();CHECK(SetupBoots==0 && SetupUnloads==1 && WdtReloads==0);
  CHECK(BdsWord(12)==6 && BdsWord(13)==0x10007);
  Reset();TraceEnabled=TrialCookie=TRUE;Ready=TRUE;SetupPathStatus=EFI_NOT_FOUND;
  LaunchRamTrialSetup();CHECK(SetupLoads==0 && SetupBoots==0 && TrialCreates==0);
  CHECK(BdsWord(12)==2 && BdsWord(13)==0x1000e);
  Reset();TraceEnabled=TrialCookie=TRUE;Ready=TRUE;SetupLoadStatus=EFI_LOAD_ERROR;
  LaunchRamTrialSetup();CHECK(SetupLoads==1 && SetupBoots==0 && SetupUnloads==0 && TrialCreates==0);
  CHECK(BdsWord(12)==4 && BdsWord(13)==0x10001);
  Reset();TraceEnabled=TrialCookie=TRUE;Ready=TRUE;SetupLoadStatus=EFI_SECURITY_VIOLATION;
  LaunchRamTrialSetup();CHECK(SetupUnloads==1 && SetupBoots==0);
  CHECK(BdsWord(12)==4 && BdsWord(13)==0x1001a);
  Reset();TraceEnabled=TrialCookie=TRUE;Ready=TRUE;SetupStartStatus=EFI_UNSUPPORTED;
  LaunchRamTrialSetup();CHECK(SetupBoots==1 && TrialCloses==0);
  CHECK(BdsWord(12)==8 && BdsWord(13)==0x10003);
  DeviceBootManagerOnDemandConInConnect();CHECK(BdsWord(12)==8 && BdsWord(13)==0x10003);
}

static void TestLateOutputConsole(void)
{
  for(UINTN Failure=1;Failure<=2;Failure++) {
    Reset();Ready=TRUE;LateOutputMode=TRUE;OutputFailure=Failure;
    ConnectBoardDevices();GopPresent=TRUE;
    CHECK(!mOutputNotifyActive[0] && !mOutputNotifyActive[1]);
    OutputConsoleNotify(mOutputNotifyEvents[0],NULL);
    CHECK(!TextOutPresent && !LiveOutput && !GopConnections);
    CHECK(OutputCloses==(Failure==2?2:0));
  }
  // Role-separated mock: GOP publication alone cannot produce TextOut/live
  // ConSplitter. Actual GraphicsConsole code is not executed by this fixture.
  Reset();Ready=TRUE;LateOutputMode=TRUE;
  ConnectBoardDevices();
  CHECK(OutputRegisters==2 && !TextOutPresent && !LiveOutput);
  GopPresent=TRUE;
  OutputConsoleNotify(mOutputNotifyEvents[0],NULL);
  CHECK(GopConnections==1 && TextOutPresent && LiveOutput && OutputRegistered && ErrorRegistered);
  CHECK(!mConnecting && !mOutputRescanPending);
  UINTN Before=OutputConnections;
  InputConsoleExit(mInputExitEvent,NULL);
  OutputConsoleNotify(mOutputNotifyEvents[0],NULL);
  CHECK(OutputConnections==Before);
  Reset();Ready=TRUE;LateOutputMode=TRUE;ConnectBoardDevices();
  GopPresent=TRUE;ContinuousOutputNotify=TRUE;
  OutputConsoleNotify(mOutputNotifyEvents[0],NULL);
  CHECK(!mConnecting && mOutputRescanPending && OutputConnections<=16);
  ContinuousOutputNotify=FALSE;OutputConsoleNotify(mOutputNotifyEvents[1],NULL);
  CHECK(!mConnecting && !mOutputRescanPending && LiveOutput);
}

static void TestLateInputConsole(void)
{
  Reset();Ready=TRUE;ContinuousInputNotify=TRUE;LateOutputMode=TRUE;
  StartInputConsoleNotify();
  ConsoleTpl=TPL_CALLBACK;
  ConnectBoardDevices();
  CHECK(!Connects && mInputRescanPending);
  InputConsoleNotify(mInputNotifyEvent,NULL);
  ConsoleTpl=TPL_APPLICATION;
  CHECK(InputConnections==24 && mInputRescanPending && !mConnecting);
  ContinuousInputNotify=FALSE;InputConsoleNotify(mInputNotifyEvent,NULL);
  CHECK(InputConnections==27 && !mInputRescanPending && !mConnecting);
  Reset();Ready=TRUE;ContinuousInputNotify=ExitInputConnect=TRUE;
  StartInputConsoleNotify();
  UINTN ExitFrees=Frees;
  InputConsoleNotify(mInputNotifyEvent,NULL);
  CHECK(InputConnections==1 && mInputServicesEnded && !mConnecting && Frees==ExitFrees && ExitInputAllocation);
  free(ExitInputAllocation);
  ExitInputAllocation=NULL;
  UINTN EndedConnections=InputConnections;
  InputConsoleNotify(mInputNotifyEvent,NULL);ConnectBoardDevices();
  CHECK(InputConnections==EndedConnections);
  // At APPLICATION TPL a protocol notification can preempt ConnectController
  // after LocateHandleBuffer took its input snapshot.
  Reset();Ready=TRUE;HideLateInput=PublishLateInput=TRUE;
  ConnectBoardDevices();
  CHECK(InputRegistered[4]);
  Reset();Ready=TRUE;Empty=TRUE;
  StartInputConsoleNotify();
  CHECK(mInputNotifyActive && InputCreates==1 && InputRegisters==1 && InputExitCreates==1);
  StartInputConsoleNotify();CHECK(InputCreates==1);
  InputConsoleNotify(mInputNotifyEvent,NULL);CHECK(InputConnections==0 && Registrations==0);
  Empty=FALSE;InputReenter=TRUE;
  InputConsoleNotify(mInputNotifyEvent,NULL);
  CHECK(InputConnections==6 && ConInCount==6 && ConOutCount==0 && Connects==0 && !mConnecting);
  CHECK(mRegisteredInputCount==3);
  UINTN Before=InputConnections,BeforeRegistration=Registrations;
  mConnecting=TRUE;InputConsoleNotify(mInputNotifyEvent,NULL);mConnecting=FALSE;
  InputConsoleNotify(NULL,NULL);
  CHECK(InputConnections==Before && Registrations==BeforeRegistration);
  InputConsoleExit(mInputExitEvent,NULL);
  InputConsoleNotify(mInputNotifyEvent,NULL);ConnectBoardDevices();
  CHECK(InputConnections==Before && Registrations==BeforeRegistration && mInputServicesEnded);
  for(UINTN Failure=1;Failure<=4;Failure++) {
    Reset();Ready=TRUE;
    if(Failure==1)InputExitStatus=EFI_OUT_OF_RESOURCES;
    if(Failure==2)InputCreateStatus=EFI_OUT_OF_RESOURCES;
    if(Failure>=3)InputRegisterStatus=EFI_OUT_OF_RESOURCES;
    if(Failure==4)InputCloseStatus=EFI_DEVICE_ERROR;
    StartInputConsoleNotify();
    CHECK(!mInputNotifyActive && mInputNotifyStarted);
    if(Failure==2)CHECK(mInputNotifyEvent==NULL);
    InputConsoleNotify(mInputNotifyEvent,NULL);
    CHECK(InputConnections==0 && Registrations==0);
    if(Failure>=3)CHECK(InputCloses==1);
  }
  Reset();Ready=TRUE;UpdateStatus=EFI_OUT_OF_RESOURCES;
  StartInputConsoleNotify();InputConsoleNotify(mInputNotifyEvent,NULL);
  CHECK(InputConnections==0 && Registrations==3 && !mConnecting);
  Reset();Ready=TRUE;InputConnectStatus=EFI_DEVICE_ERROR;
  StartInputConsoleNotify();InputConsoleNotify(mInputNotifyEvent,NULL);
  CHECK(InputConnections==3 && Registrations==3 && !mConnecting);
}

static void TestBootLogoDisplay(void)
{
  Reset();Ready=TRUE;
  CHECK(DeviceBootManagerAfterConsole()==NULL);
  CHECK(LogoCalls==1U && Connects==1U && Refreshes==1U);
  Reset();Ready=TRUE;LogoStatus=EFI_NOT_FOUND;
  CHECK(DeviceBootManagerAfterConsole()==NULL);
  CHECK(LogoCalls==1U && Connects==1U && Refreshes==1U);
}

int main(void)
{
  EFI_DEVICE_PATH_PROTOCOL *Device=(VOID *)1;
  BDS_CONSOLE_CONNECT_ENTRY *Consoles=(VOID *)1;
  Reset(); Reenter=TRUE;
  CHECK(DeviceBootManagerBeforeConsole(&Device,&Consoles) == NULL);
  CHECK(Device == NULL && Consoles == NULL && Connects == 1 && Lookups == 4);
  CHECK(Registrations == 5 && ConOutCount == 1 && ErrOutCount == 1 && ConInCount == 3);
  CHECK(DeviceBootManagerAfterConsole() == NULL && Connects == 2 && Refreshes == 1);
  CHECK(Registrations == 10 && Lookups == 8);
  CHECK(DeviceBootManagerOnDemandConInConnect() == NULL && Connects == 3 &&
        Registrations == 15 && Lookups == 12);
  Reset(); Empty=TRUE;
  CHECK(DeviceBootManagerBeforeConsole(NULL,NULL) == NULL && Registrations == 0 && Lookups == 4);
  CHECK(DeviceBootManagerAfterConsole() == NULL && Refreshes == 1 && Lookups == 8);
  Reset(); FailGuid=&gEfiSimpleTextOutProtocolGuid;
  CHECK(DeviceBootManagerBeforeConsole(NULL,NULL) == NULL);
  CHECK(ConOutCount == 0 && ErrOutCount == 0 && ConInCount == 3 && Lookups == 4);
  Reset(); UpdateStatus=EFI_OUT_OF_RESOURCES;
  CHECK(DeviceBootManagerBeforeConsole(NULL,NULL) == NULL && Registrations == 5 && !mConnecting);
  CHECK(DeviceBootManagerPriorityBoot(NULL) == EFI_NOT_FOUND);
  Reset(); ShellReenter=TRUE;
  DeviceBootManagerUnableToBoot();
  CHECK(ShellLookups==1 && ShellInitializations==1 && ShellBoots==1 &&
        ShellOptionFrees==1 && ShellLookupFrees==1 && !mRecoveryShellActive);
  CHECK(LegacyBootCalls==0 && RecoveryLoads==1);
  Reset();RecoveryLoadStatus=EFI_OUT_OF_RESOURCES;DeviceBootManagerUnableToBoot();
  CHECK(RecoveryLoads==1&&RecoveryUnloads==0&&ShellBoots==0&&ShellOptionFrees==1&&LegacyBootCalls==0);
  Reset();RecoveryLoadStatus=EFI_SECURITY_VIOLATION;DeviceBootManagerUnableToBoot();
  CHECK(RecoveryUnloads==1&&ShellBoots==0&&ShellOptionFrees==1);
  Reset();RecoveryProtocolStatus=EFI_UNSUPPORTED;DeviceBootManagerUnableToBoot();
  CHECK(RecoveryUnloads==1&&ShellBoots==0&&ShellOptionFrees==1);
  Reset(); ShellLookupStatus=EFI_NOT_FOUND;
  DeviceBootManagerUnableToBoot();
  CHECK(ShellLookups==1 && ShellInitializations==0 && ShellBoots==0 &&
        ShellOptionFrees==0 && ShellLookupFrees==0);
  Reset(); ShellLookupStatus=EFI_ACCESS_DENIED;
  DeviceBootManagerUnableToBoot();
  CHECK(ShellLookups==1 && ShellBoots==0 && ShellLookupAllocation==NULL);
  Reset(); ShellInitStatus=EFI_OUT_OF_RESOURCES;
  DeviceBootManagerUnableToBoot();
  CHECK(ShellLookups==1 && ShellInitializations==1 && ShellBoots==0 &&
        ShellLookupFrees==1 && ShellOptionFrees==0 && !mRecoveryShellActive);
  Reset(); mRecoveryShellActive=TRUE;
  DeviceBootManagerUnableToBoot();
  CHECK(ShellLookups==0 && ShellBoots==0);
  mRecoveryShellActive=FALSE;
  TestBootDiagnostic();
  TestBdsProgress();
  TestInteractiveSetupSession();
  TestBootLogoDisplay();
  TestLateInputConsole();
  TestLateOutputConsole();
  printf("T70 board boot policy: %u checks passed (recursive storage/input connection, real console paths, empty/failing providers, reentry, filesystem enumeration, transient -nostartup recovery Shell, lookup/initialization failures and cleanup)\n",Checks);
  return 0;
}
