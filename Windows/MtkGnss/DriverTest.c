// SPDX-License-Identifier: BSD-2-Clause-Patent
/* Actual production UMDF callbacks, virtual WDF/receiver/clock only. */
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <wdf.h>
ULONGLONG TestClock(void);
VOID TestTime(FILETIME *Time);
#define GetTickCount64 TestClock
#define GetSystemTimeAsFileTime TestTime
#include "Driver.c"
static WDFFUNC Table[WdfFunctionTableNumEntries];
const WDFFUNC *WdfFunctions=Table;
PWDF_DRIVER_GLOBALS WdfDriverGlobals;
BOOLEAN WdfClientVersionHigherThanFramework;
ULONG WdfStructureCount;
WDF_STRUCT_INFO WdfStructures;
__declspec(dllexport) unsigned MtkGnssAssertionCount;
static unsigned Failure,Depth,LowerCalls,PollCalls,TimerStarts,TimerStops,CommandCalls,StopCalls,CookieCalls;
static unsigned AddStep,AddFailure,FailTimes,InfoVariant,UseLeaseOverride,WaitTimerStops;
static ULONG FailCode,FailCommand;
static NTSTATUS FailStatus;
static ULONGLONG Clock=1000,Wall,LeaseCounter=7,LeaseOverride,Generation=11;
static BOOLEAN ProviderEnabled,ProviderActive,HaveInput;
static GN_DEVICE Device;
static MTK_RX_FIX Fix,Mutated;
static GNSS_EVENT_2 MapOutput;
static WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
static WDF_FILEOBJECT_CONFIG FileConfig;
static WDF_IO_QUEUE_CONFIG ControlConfig;
#define DEV ((WDFDEVICE)(ULONG_PTR)0x1000)
#define TARGET ((WDFIOTARGET)(ULONG_PTR)0x2000)
#define GATE ((WDFWAITLOCK)(ULONG_PTR)0x3000)
#define WAITERS ((WDFQUEUE)(ULONG_PTR)0x4000)
#define CONTROL ((WDFQUEUE)(ULONG_PTR)0x5000)
#define TIMER ((WDFTIMER)(ULONG_PTR)0x6000)
#define FILE1 ((WDFFILEOBJECT)(ULONG_PTR)0x7000)
#define CHECK(X) do { MtkGnssAssertionCount++; if (!(X) && !Failure) Failure=__LINE__; } while (0)
typedef struct { UCHAR Buffer[sizeof(GNSS_EVENT_2)+32]; size_t In,Out; ULONG Complete; NTSTATUS Status; ULONG_PTR Bytes; WDFFILEOBJECT File; } REQUEST;
static REQUEST Requests[12],*Pending,*ExtraPending[8];
static ULONG ExtraCount;
#pragma function(memcpy,memset)
void *memcpy(void *To,const void *From,size_t N) { volatile UCHAR *A=To; const volatile UCHAR *B=From; while (N--) *A++=*B++; return To; }
void *memset(void *To,int Value,size_t N) { volatile UCHAR *A=To; while (N--) *A++=(UCHAR)Value; return To; }
void __cdecl __security_init_cookie(void) { CookieCalls++; }
ULONGLONG TestClock(void) { return Clock; }
VOID TestTime(FILETIME *F) { F->dwLowDateTime=(DWORD)Wall; F->dwHighDateTime=(DWORD)(Wall>>32); }
static uint64_t Bits(double V) { uint64_t B; memcpy(&B,&V,sizeof(B)); return B; }
static BOOLEAN Zero(const void *P,size_t N) { const UCHAR *B=P; size_t I; for (I=0;I<N;I++) if (B[I]) return FALSE; return TRUE; }
static BOOLEAN Filled(const void *P,size_t N,UCHAR Value) { const UCHAR *B=P; size_t I; for (I=0;I<N;I++) if (B[I]!=Value) return FALSE; return TRUE; }
static PVOID MockContext(PWDF_DRIVER_GLOBALS G,WDFOBJECT O,PCWDF_OBJECT_CONTEXT_TYPE_INFO I) { (void)G;(void)I;CHECK(O==(WDFOBJECT)DEV);return &Device; }
static NTSTATUS MockLock(PWDF_DRIVER_GLOBALS G,WDFWAITLOCK L,PLONGLONG Timeout) { (void)G;CHECK(L==GATE && !Timeout && !Depth);Depth++;return STATUS_SUCCESS; }
static VOID MockUnlock(PWDF_DRIVER_GLOBALS G,WDFWAITLOCK L) { (void)G;CHECK(L==GATE && Depth==1);Depth--; }
static WDFDEVICE MockQueueDevice(PWDF_DRIVER_GLOBALS G,WDFQUEUE Q) { (void)G;CHECK(Q==CONTROL);return DEV; }
static WDFOBJECT MockTimerParent(PWDF_DRIVER_GLOBALS G,WDFTIMER T) { (void)G;CHECK(T==TIMER);return (WDFOBJECT)DEV; }
static WDFDEVICE MockFileDevice(PWDF_DRIVER_GLOBALS G,WDFFILEOBJECT F) { (void)G;CHECK(F==FILE1);return DEV; }
static WDFFILEOBJECT MockRequestFile(PWDF_DRIVER_GLOBALS G,WDFREQUEST R) { (void)G;return ((REQUEST *)R)->File; }
static NTSTATUS MockInput(PWDF_DRIVER_GLOBALS G,WDFREQUEST R,size_t Minimum,PVOID *P,size_t *Length) {
  REQUEST *Q=(REQUEST *)R;(void)G;
  if (Q->In<Minimum) return STATUS_BUFFER_TOO_SMALL;
  *P=Q->Buffer;if (Length) *Length=Q->In;return STATUS_SUCCESS;
}
static NTSTATUS MockOutput(PWDF_DRIVER_GLOBALS G,WDFREQUEST R,size_t Minimum,PVOID *P,size_t *Length) {
  REQUEST *Q=(REQUEST *)R;(void)G;
  if (Q->Out<Minimum) return STATUS_BUFFER_TOO_SMALL;
  *P=Q->Buffer;if (Length) *Length=Q->Out;return STATUS_SUCCESS;
}
static VOID MockCompleteInfo(PWDF_DRIVER_GLOBALS G,WDFREQUEST R,NTSTATUS S,ULONG_PTR Bytes) {
  REQUEST *Q=(REQUEST *)R;(void)G;CHECK(Depth==0 && !Q->Complete && Bytes<=Q->Out);Q->Complete++;Q->Status=S;Q->Bytes=Bytes;
}
static VOID MockComplete(PWDF_DRIVER_GLOBALS G,WDFREQUEST R,NTSTATUS S) { MockCompleteInfo(G,R,S,0); }
static NTSTATUS MockForward(PWDF_DRIVER_GLOBALS G,WDFREQUEST R,WDFQUEUE Q) { (void)G;CHECK(Depth==1 && Q==WAITERS && ExtraCount<8);
  if (!Pending) Pending=(REQUEST *)R;
  else ExtraPending[ExtraCount++]=(REQUEST *)R;
  return STATUS_SUCCESS; }
static NTSTATUS MockNext(PWDF_DRIVER_GLOBALS G,WDFQUEUE Q,WDFREQUEST *R) {
  (void)G;CHECK(Depth==1 && Q==WAITERS);
  if (!Pending) return STATUS_NO_MORE_ENTRIES;
  *R=(WDFREQUEST)Pending;
  Pending=ExtraCount?ExtraPending[0]:NULL;
  if (ExtraCount) { for (ULONG I=1;I<ExtraCount;I++) ExtraPending[I-1]=ExtraPending[I]; ExtraPending[--ExtraCount]=NULL; }
  return STATUS_SUCCESS;
}
static WDF_IO_QUEUE_STATE MockQueueState(PWDF_DRIVER_GLOBALS G,WDFQUEUE Q,PULONG Count,PULONG Owned) {
  (void)G;CHECK(Q==WAITERS && Depth==1);if (Count) *Count=(Pending!=NULL)+ExtraCount;if (Owned) *Owned=0;return WdfIoQueueAcceptRequests|WdfIoQueueDispatchRequests;
}
static BOOLEAN MockTimerStart(PWDF_DRIVER_GLOBALS G,WDFTIMER T,LONGLONG Due) { (void)G;CHECK(T==TIMER && Due<0 && Depth==1);TimerStarts++;return FALSE; }
static BOOLEAN MockTimerStop(PWDF_DRIVER_GLOBALS G,WDFTIMER T,BOOLEAN Wait) {
  (void)G;CHECK(T==TIMER && (Wait?Depth==0:Depth==1));TimerStops++;if(Wait)WaitTimerStops++;return TRUE;
}
static NTSTATUS MockLower(PWDF_DRIVER_GLOBALS G,WDFIOTARGET Target,WDFREQUEST R,ULONG Code,PWDF_MEMORY_DESCRIPTOR In,PWDF_MEMORY_DESCRIPTOR Out,PWDF_REQUEST_SEND_OPTIONS Options,PULONG_PTR Bytes) {
  PVOID Input=In?In->u.BufferType.Buffer:NULL,Output=Out?Out->u.BufferType.Buffer:NULL;
  ULONG InBytes=In?In->u.BufferType.Length:0,OutBytes=Out?Out->u.BufferType.Length:0;
  (void)G;CHECK(Target==TARGET && !R && Depth==1 && Options && Options->Flags==WDF_REQUEST_SEND_OPTION_TIMEOUT && Options->Timeout==-5000000);
  if (In) CHECK(In->Type==WdfMemoryDescriptorTypeBuffer);
  if (Out) CHECK(Out->Type==WdfMemoryDescriptorTypeBuffer);
  LowerCalls++;*Bytes=0;
  if (FailTimes && Code==FailCode && (!FailCommand || (Input && ((GNSS_DRIVERCOMMAND_PARAM *)Input)->CommandType==FailCommand))) { FailTimes--;return FailStatus; }
  if (Code==MTK_RX_QUERY) {
    MTK_RX_INFO *I=Output;CHECK(!InBytes && OutBytes==sizeof(*I));memset(I,0,sizeof(*I));
    I->Size=sizeof(*I);I->Version=MTK_RX_VERSION;I->Commands=7;I->Format=1;I->Generation=Generation;
    I->Chipset.Size=sizeof(I->Chipset);I->Chipset.Version=GNSS_DRIVER_VERSION_6;
    I->Chipset.ManufacturerID[0]='S';I->Chipset.HardwareID[0]='T';I->Chipset.FirmwareVersion[0]='V';
    I->Chipset.ManufacturerID[2]='X';I->Chipset.Unused[0]=0xa5;
    if (InfoVariant==1) I->Size--;
    if (InfoVariant==2) I->Version++;
    if (InfoVariant==3) I->Format++;
    if (InfoVariant==4) I->Commands=3;
    if (InfoVariant==5) I->Generation=0;
    if (InfoVariant==6) I->Chipset.ManufacturerID[24]='X';
    *Bytes=InfoVariant==7?sizeof(*I)-1:sizeof(*I);return STATUS_SUCCESS;
  }
  if (Code==MTK_RX_COMMAND) {
    GNSS_DRIVERCOMMAND_PARAM *C=Input;CHECK(InBytes>=FIELD_OFFSET(GNSS_DRIVERCOMMAND_PARAM,CommandData) && !OutBytes);CommandCalls++;
    if (C->CommandType==GNSS_SetLocationServiceEnabled) {
      BOOL Enable;memcpy(&Enable,C->CommandData,sizeof(Enable));ProviderEnabled=(BOOLEAN)Enable;
      if (!Enable) ProviderActive=FALSE;
    } else if (C->CommandType==GNSS_ResetEngine) ProviderActive=FALSE;
    return STATUS_SUCCESS;
  }
  if (Code==MTK_RX_START || Code==MTK_RX_MODIFY) {
    CHECK(InBytes==(Code==MTK_RX_START?sizeof(GNSS_FIXSESSION_PARAM):sizeof(MTK_RX_MODIFY_PARAM)) && OutBytes==sizeof(ULONGLONG) && ProviderEnabled);
    if (Code==MTK_RX_MODIFY) CHECK(((MTK_RX_MODIFY_PARAM *)Input)->Lease==Device.Lease);
    *(ULONGLONG *)Output=UseLeaseOverride?LeaseOverride:++LeaseCounter;*Bytes=sizeof(ULONGLONG);ProviderActive=TRUE;return STATUS_SUCCESS;
  }
  if (Code==MTK_RX_STOP) { CHECK(InBytes==sizeof(ULONGLONG) && !OutBytes);StopCalls++;ProviderActive=FALSE;return STATUS_SUCCESS; }
  if (Code==MTK_RX_POLL) {
    CHECK(InBytes==sizeof(ULONGLONG) && *(ULONGLONG *)Input==Device.Lease && OutBytes==sizeof(Fix));PollCalls++;
    if (!HaveInput) return STATUS_NO_MORE_ENTRIES;
    memcpy(Output,&Fix,sizeof(Fix));*Bytes=sizeof(Fix);HaveInput=FALSE;
    if (Fix.Flags&MTK_RX_FINAL) ProviderActive=FALSE;
    return STATUS_SUCCESS;
  }
  CHECK(0);return STATUS_NOT_SUPPORTED;
}
static BOOLEAN AddFails(void) { return ++AddStep==AddFailure; }
static VOID MockPnp(PWDF_DRIVER_GLOBALS G,PWDFDEVICE_INIT Init,PWDF_PNPPOWER_EVENT_CALLBACKS P) { (void)G;(void)Init;memcpy(&Pnp,P,sizeof(Pnp)); }
static VOID MockFileConfig(PWDF_DRIVER_GLOBALS G,PWDFDEVICE_INIT Init,PWDF_FILEOBJECT_CONFIG F,PWDF_OBJECT_ATTRIBUTES A) { (void)G;(void)Init;CHECK(!A);memcpy(&FileConfig,F,sizeof(FileConfig)); }
static NTSTATUS MockDeviceCreate(PWDF_DRIVER_GLOBALS G,PWDFDEVICE_INIT *Init,PWDF_OBJECT_ATTRIBUTES A,WDFDEVICE *D) {
  (void)G;CHECK(A->ExecutionLevel==WdfExecutionLevelPassive && A->SynchronizationScope==WdfSynchronizationScopeNone);
  if (AddFails()) return STATUS_INSUFFICIENT_RESOURCES;
  *Init=NULL;*D=DEV;return STATUS_SUCCESS;
}
static WDFIOTARGET MockTarget(PWDF_DRIVER_GLOBALS G,WDFDEVICE D) { (void)G;CHECK(D==DEV);return TARGET; }
static NTSTATUS MockLockCreate(PWDF_DRIVER_GLOBALS G,PWDF_OBJECT_ATTRIBUTES A,WDFWAITLOCK *L) { (void)G;CHECK(A->ParentObject==(WDFOBJECT)DEV);if (AddFails()) return STATUS_INSUFFICIENT_RESOURCES;*L=GATE;return STATUS_SUCCESS; }
static NTSTATUS MockQueueCreate(PWDF_DRIVER_GLOBALS G,WDFDEVICE D,PWDF_IO_QUEUE_CONFIG Q,PWDF_OBJECT_ATTRIBUTES A,WDFQUEUE *O) {
  (void)G;(void)A;CHECK(D==DEV && Q->PowerManaged==WdfFalse);
  if (AddFails()) return STATUS_INSUFFICIENT_RESOURCES;
  if (Q->DispatchType==WdfIoQueueDispatchManual) { CHECK(O);*O=WAITERS; }
  else { CHECK(Q->DispatchType==WdfIoQueueDispatchSequential && Q->DefaultQueue && !O);memcpy(&ControlConfig,Q,sizeof(ControlConfig)); }
  return STATUS_SUCCESS;
}
static NTSTATUS MockTimerCreate(PWDF_DRIVER_GLOBALS G,PWDF_TIMER_CONFIG T,PWDF_OBJECT_ATTRIBUTES A,WDFTIMER *O) {
  (void)G;CHECK(T->EvtTimerFunc==GnPoll && !T->Period && !T->AutomaticSerialization && A->ParentObject==(WDFOBJECT)DEV && A->ExecutionLevel==WdfExecutionLevelPassive);
  if (AddFails()) return STATUS_INSUFFICIENT_RESOURCES;*O=TIMER;return STATUS_SUCCESS;
}
static NTSTATUS MockInterface(PWDF_DRIVER_GLOBALS G,WDFDEVICE D,LPCGUID Guid,PCUNICODE_STRING Reference) {
  (void)G;CHECK(D==DEV && Guid->Data1==GUID_DEVINTERFACE_GNSS.Data1 && !Reference);return AddFails()?STATUS_INSUFFICIENT_RESOURCES:STATUS_SUCCESS;
}
static NTSTATUS MockDriverCreate(PWDF_DRIVER_GLOBALS G,PDRIVER_OBJECT D,PCUNICODE_STRING Path,PWDF_OBJECT_ATTRIBUTES A,PWDF_DRIVER_CONFIG C,WDFDRIVER *Out) {
  (void)G;(void)D;(void)Path;CHECK(!A && !Out && C->EvtDriverDeviceAdd==GnDeviceAdd);return STATUS_SUCCESS;
}
static VOID Setup(void) {
#define SET(N,F) Table[N##TableIndex]=(WDFFUNC)F
  SET(WdfObjectGetTypedContextWorker,MockContext);SET(WdfWaitLockAcquire,MockLock);SET(WdfWaitLockRelease,MockUnlock);
  SET(WdfIoQueueGetDevice,MockQueueDevice);SET(WdfTimerGetParentObject,MockTimerParent);SET(WdfFileObjectGetDevice,MockFileDevice);SET(WdfRequestGetFileObject,MockRequestFile);
  SET(WdfRequestRetrieveInputBuffer,MockInput);SET(WdfRequestRetrieveOutputBuffer,MockOutput);SET(WdfRequestComplete,MockComplete);SET(WdfRequestCompleteWithInformation,MockCompleteInfo);
  SET(WdfRequestForwardToIoQueue,MockForward);SET(WdfIoQueueRetrieveNextRequest,MockNext);SET(WdfIoQueueGetState,MockQueueState);SET(WdfTimerStart,MockTimerStart);SET(WdfTimerStop,MockTimerStop);
  SET(WdfIoTargetSendIoctlSynchronously,MockLower);SET(WdfDeviceInitSetPnpPowerEventCallbacks,MockPnp);SET(WdfDeviceInitSetFileObjectConfig,MockFileConfig);SET(WdfDeviceCreate,MockDeviceCreate);
  SET(WdfDeviceGetIoTarget,MockTarget);SET(WdfWaitLockCreate,MockLockCreate);SET(WdfIoQueueCreate,MockQueueCreate);SET(WdfTimerCreate,MockTimerCreate);SET(WdfDeviceCreateDeviceInterface,MockInterface);SET(WdfDriverCreate,MockDriverCreate);
#undef SET
}
static VOID Blank(void) {
  CHECK(!Depth);memset(&Device,0,sizeof(Device));memset(Requests,0,sizeof(Requests));Pending=NULL;
  ProviderEnabled=ProviderActive=HaveInput=FALSE;UseLeaseOverride=0;AddStep=AddFailure=FailTimes=InfoVariant=0;
  FailCode=FailCommand=0;FailStatus=STATUS_IO_DEVICE_ERROR;Clock=1000;WaitTimerStops=0;CHECK(GnTimestamp(INT64_C(764426119000),&Wall));
}
static VOID StartDevice(void) {
  Blank();CHECK(GnDeviceAdd(NULL,(PWDFDEVICE_INIT)(ULONG_PTR)0x8000)==STATUS_SUCCESS);
  CHECK(Pnp.EvtDeviceD0Entry==GnD0Entry && Pnp.EvtDeviceD0Exit==GnD0Exit && Pnp.EvtDeviceSurpriseRemoval==GnRemoved);
  CHECK(FileConfig.EvtDeviceFileCreate==GnFileCreate && FileConfig.EvtFileCleanup==GnFileCleanup && ControlConfig.EvtIoDeviceControl==GnIoctl);
  CHECK(GnD0Entry(DEV,WdfPowerDeviceD3)==STATUS_SUCCESS && Device.Ready && !ProviderEnabled && !ProviderActive);
  GnFileCreate(DEV,(WDFREQUEST)&Requests[7],FILE1);CHECK(Requests[7].Complete==1 && Requests[7].Status==STATUS_SUCCESS && Device.Owner==FILE1);
}
static REQUEST *Issue(unsigned Slot,ULONG Code,const void *Input,size_t In,size_t Out) {
  REQUEST *R=&Requests[Slot];memset(R,0,sizeof(*R));memset(R->Buffer,0xa5,sizeof(R->Buffer));
  CHECK(In<=sizeof(R->Buffer) && Out<=sizeof(R->Buffer));if (In) memcpy(R->Buffer,Input,In);R->In=In;R->Out=Out;R->File=FILE1;
  GnIoctl(CONTROL,(WDFREQUEST)R,Out,In,Code);CHECK(!Depth);return R;
}
static VOID Platform(void) {
  GNSS_PLATFORM_CAPABILITY P;REQUEST *R;memset(&P,0,sizeof(P));P.Size=sizeof(P);P.Version=GNSS_DRIVER_VERSION_6;
  R=Issue(0,IOCTL_GNSS_SEND_PLATFORM_CAPABILITY,&P,sizeof(P),0);CHECK(R->Complete==1 && R->Status==STATUS_SUCCESS && Device.PlatformKnown);
}
static REQUEST *Command(ULONG Type,BOOL Enable) {
  static UCHAR Buffer[FIELD_OFFSET(GNSS_DRIVERCOMMAND_PARAM,CommandData)+8];GNSS_DRIVERCOMMAND_PARAM *P=(void *)Buffer;ULONG Length;
  memset(Buffer,0,sizeof(Buffer));Length=sizeof(*P);P->Size=Length;P->Version=GNSS_DRIVER_VERSION_6;P->CommandType=Type;
  if (Type==GNSS_SetLocationServiceEnabled) { P->CommandDataSize=sizeof(Enable);memcpy(P->CommandData,&Enable,sizeof(Enable)); }
  return Issue(0,IOCTL_GNSS_SEND_DRIVERCOMMAND,P,Length,0);
}
static GNSS_FIXSESSION_PARAM Parameters(void) {
  GNSS_FIXSESSION_PARAM P;memset(&P,0,sizeof(P));P.Size=sizeof(P);P.Version=GNSS_DRIVER_VERSION_6;
  P.FixSessionID=42;P.SessionType=GNSS_FixSession_SingleShot;P.FixLevelOfDetails=7;P.HorizontalAccuracy=10;P.HorizontalConfidence=95;
  P.SingleShotParam.Size=sizeof(P.SingleShotParam);P.SingleShotParam.Version=GNSS_DRIVER_VERSION_6;P.SingleShotParam.ResponseTime=0;
  return P;
}
static VOID Session(void) {
  GNSS_FIXSESSION_PARAM P=Parameters();REQUEST *R;
  Platform();CHECK(Command(GNSS_SetLocationServiceEnabled,TRUE)->Status==STATUS_SUCCESS);
  R=Issue(0,IOCTL_GNSS_START_FIXSESSION,&P,sizeof(P),0);CHECK(R->Status==STATUS_SUCCESS && Device.Active && Device.Lease && ProviderActive);
}
static REQUEST *Get(unsigned Slot) { ULONG Id=42;return Issue(Slot,IOCTL_GNSS_GET_FIXDATA,&Id,sizeof(Id),sizeof(GNSS_EVENT_2)); }
static REQUEST *Stop(void) {
  GNSS_STOPFIXSESSION_PARAM P;memset(&P,0,sizeof(P));P.Size=sizeof(P);P.Version=GNSS_DRIVER_VERSION_6;P.FixSessionID=42;
  return Issue(0,IOCTL_GNSS_STOP_FIXSESSION,&P,sizeof(P),0);
}
static VOID MakeFix(ULONGLONG Sequence) {
  memset(&Fix,0,sizeof(Fix));Fix.Size=sizeof(Fix);Fix.Version=MTK_RX_VERSION;Fix.Generation=Device.Info.Generation;
  Fix.Lease=Device.Lease;Fix.Sequence=Sequence;Fix.Flags=MTK_RX_UTC|MTK_RX_MSL|MTK_RX_HDOP|MTK_RX_SATELLITES;
  Fix.Location.Flags=31;Fix.Location.UtcMilliseconds=764426119000;
  Fix.Location.LatitudeBits=UINT64_C(0x40480f03afb7e910);Fix.Location.LongitudeBits=UINT64_C(0x4027088888888889);
  Fix.Location.AltitudeBits=Bits(592.3);Fix.Location.SpeedBits=Bits(10.0);Fix.Location.BearingBits=Bits(90.0);
  Fix.Location.HorizontalAccuracyBits=Bits(1.0);Fix.MslAltitudeBits=Bits(545.4);Fix.HdopBits=Bits(0.9);
  Fix.SatelliteCount=3;Fix.Satellites[0].Svid=1;Fix.Satellites[0].Constellation=1;Fix.Satellites[0].Cn0Bits=0x41a00000;
  Fix.Satellites[0].ElevationBits=0x42340000;Fix.Satellites[0].AzimuthBits=0x42b40000;Fix.Satellites[0].Flags=4;
  memcpy(&Fix.Satellites[1],&Fix.Satellites[0],sizeof(Fix.Satellites[0]));Fix.Satellites[1].Cn0Bits=0x42200000;Fix.Satellites[1].Flags=0;
  memcpy(&Fix.Satellites[2],&Fix.Satellites[0],sizeof(Fix.Satellites[0]));Fix.Satellites[2].Constellation=2;
}
static VOID Poll(void) { Clock+=200;GnPoll(TIMER);CHECK(!Depth && Zero(&Device.Input,sizeof(Device.Input))); }
static VOID LifecycleTest(void) {
  unsigned I,Before,BeforeWait;REQUEST *R;GNSS_DEVICE_CAPABILITY *C;GNSS_CHIPSETINFO *Chip;GNSS_FIXSESSION_PARAM P;
  CHECK(DriverEntry(NULL,NULL)==STATUS_SUCCESS);CHECK(GnDllMain(NULL,DLL_PROCESS_ATTACH,NULL) && CookieCalls==1);
  for (I=1;I<=6;I++) { Blank();AddFailure=I;CHECK(GnDeviceAdd(NULL,(PWDFDEVICE_INIT)(ULONG_PTR)0x8000)==STATUS_INSUFFICIENT_RESOURCES); }
  for (I=1;I<=7;I++) {
    Blank();CHECK(GnDeviceAdd(NULL,(PWDFDEVICE_INIT)(ULONG_PTR)0x8000)==STATUS_SUCCESS);InfoVariant=I;
    CHECK(!NT_SUCCESS(GnD0Entry(DEV,WdfPowerDeviceD3)) && !Device.Ready && !ProviderActive);
  }
  StartDevice();P=Parameters();R=Issue(0,IOCTL_GNSS_START_FIXSESSION,&P,sizeof(P),0);CHECK(R->Status==STATUS_DEVICE_NOT_READY);
  R=Issue(0,IOCTL_GNSS_GET_DEVICE_CAPABILITY,NULL,0,sizeof(*C));C=(void *)R->Buffer;
  CHECK(R->Status==STATUS_SUCCESS && R->Bytes==sizeof(*C) && C->Version==6 && !C->SupportMultipleFixSessions && !C->SupportContinuousTracking && !C->SupportSuplV2);
  R=Issue(0,IOCTL_GNSS_GET_CHIPSETINFO,NULL,0,sizeof(*Chip));Chip=(void *)R->Buffer;
  CHECK(R->Status==STATUS_SUCCESS && Chip->ManufacturerID[0]=='S' && Chip->ManufacturerID[2]==0 && Zero(Chip->Unused,sizeof(Chip->Unused)));
  Session();R=Get(1);CHECK(!R->Complete && Pending==R);Before=LowerCalls;
  BeforeWait=WaitTimerStops;
  GnRemoved(DEV);CHECK(R->Complete==1 && R->Status==STATUS_DEVICE_REMOVED && LowerCalls==Before && !Device.Ready && !Device.Active && WaitTimerStops==BeforeWait+1);
  Poll();CHECK(LowerCalls==Before);CHECK(GnD0Exit(DEV,WdfPowerDeviceD3)==STATUS_SUCCESS && LowerCalls==Before);
  StartDevice();Session();R=Get(1);Before=WaitTimerStops;GnFileCleanup(FILE1);
  CHECK(R->Complete==1 && R->Status==STATUS_CANCELLED && !Device.Owner && !Device.Enabled && !Device.Active && !ProviderEnabled && !ProviderActive && WaitTimerStops==Before+1);
  StartDevice();Session();R=Get(1);Before=WaitTimerStops;
  CHECK(GnD0Exit(DEV,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(R->Complete==1 && R->Status==STATUS_POWER_STATE_INVALID && !Device.Ready && !ProviderActive && !ProviderEnabled && WaitTimerStops==Before+1);
  CHECK(GnD0Entry(DEV,WdfPowerDeviceD3)==STATUS_SUCCESS && ProviderEnabled && !ProviderActive && !Device.Active);
}
static VOID SessionTest(void) {
  REQUEST *R,*Q;unsigned Before,I;GNSS_EVENT_2 *E;GNSS_FIXSESSION_PARAM P;ULONGLONG Old;
  StartDevice();Session();R=Get(1);CHECK(!R->Complete && Pending==R);
  Poll();CHECK(!R->Complete && Pending==R);
  MakeFix(1);HaveInput=TRUE;Poll();E=(void *)R->Buffer;
  CHECK(R->Complete==1 && R->Status==STATUS_SUCCESS && R->Bytes==sizeof(*E) && !Device.HaveEvent && Zero(&Device.Event,sizeof(Device.Event)));
  CHECK(E->Version==6 && E->EventType==GNSS_Event_FixAvailable_2 && E->FixData2.FixSessionID==42 && E->FixData2.FixLevelOfDetails==7);
  CHECK(E->FixData2.BasicData.Altitude==545.4 && E->FixData2.BasicData.AltitudeEllipsoid==592.3);
  CHECK(Bits(E->FixData2.AccuracyData.HorizontalAccuracy)==MTK_RX_NAN && E->FixData2.AccuracyData.HorizontalDilutionOfPrecision==0.9);
  CHECK(E->FixData2.SatelliteData.SatelliteCount==2 && E->FixData2.SatelliteData.SatelliteArray[0].SatelliteId==0x10001 && E->FixData2.SatelliteData.SatelliteArray[1].SatelliteId==0x20001);
  CHECK(E->FixData2.SatelliteData.SatelliteArray[0].UsedInPositiong && E->FixData2.SatelliteData.SatelliteArray[0].SignalToNoiseRatio==40.0);
  R=Get(1);CHECK(Pending==R);Pending=NULL;MockComplete(NULL,(WDFREQUEST)R,STATUS_CANCELLED);
  MakeFix(2);HaveInput=TRUE;Poll();CHECK(Device.HaveEvent && R->Complete==1);Q=Get(2);CHECK(Q->Status==STATUS_SUCCESS && Q->Complete==1);
  Old=Device.Lease;P=Parameters();P.HorizontalAccuracy=5;R=Get(1);
  Q=Issue(0,IOCTL_GNSS_MODIFY_FIXSESSION,&P,sizeof(P),0);CHECK(Q->Status==STATUS_SUCCESS && Device.Lease>Old && Pending==R && !R->Complete);
  MakeFix(1);Fix.Lease=Old;HaveInput=TRUE;Poll();CHECK(Pending==R && !R->Complete);
  for (I=0;I<4;I++) {
    MakeFix(1);if (I==0) Fix.Generation++;if (I==1) Fix.Location.UtcMilliseconds-=5001;if (I==2) Fix.Location.UtcMilliseconds+=1001;if (I==3) Fix.Flags&=~MTK_RX_UTC;
    HaveInput=TRUE;Poll();CHECK(Pending==R && !R->Complete && !Device.Faulted && !Device.HaveEvent);
  }
  MakeFix(1);Fix.Flags|=MTK_RX_FINAL|MTK_RX_HORIZONTAL_95;Fix.HorizontalAccuracy95Bits=Bits(7.0);HaveInput=TRUE;Before=TimerStarts;Poll();E=(void *)R->Buffer;
  CHECK(R->Status==STATUS_SUCCESS && E->FixData2.IsFinalFix && Device.FinalSeen && E->FixData2.AccuracyData.HorizontalAccuracy==7.0 && !ProviderActive && TimerStarts==Before);
  Q=Get(2);CHECK(Q->Status==STATUS_NO_MORE_ENTRIES && Q->Complete==1);
  CHECK(Stop()->Status==STATUS_SUCCESS && !Device.Active && !Device.Lease);
}
static VOID PendingFixOrderTest(void) {
  REQUEST *A,*B; GNSS_EVENT_2 *E;
  StartDevice(); Session(); A=Get(1); B=Get(2);
  CHECK(!A->Complete && !B->Complete && Pending==A);
  MakeFix(1); Fix.Location.LatitudeBits=Bits(12.0); HaveInput=TRUE; Poll();
  E=(void *)A->Buffer;
  CHECK(A->Complete==1 && A->Status==STATUS_SUCCESS && E->FixData2.BasicData.Latitude==12.0);
  CHECK(!B->Complete && Pending==B);
  MakeFix(2); Fix.Location.LatitudeBits=Bits(13.0); HaveInput=TRUE; Poll();
  E=(void *)B->Buffer;
  CHECK(B->Complete==1 && B->Status==STATUS_SUCCESS && E->FixData2.BasicData.Latitude==13.0 && !Pending);
  A=Get(1); B=Get(2);
  CHECK(Stop()->Status==STATUS_SUCCESS && A->Complete==1 && B->Complete==1 &&
        A->Status==STATUS_CANCELLED && B->Status==STATUS_CANCELLED && !Pending);
  StartDevice(); Session(); A=Get(1); B=Get(2);
  MakeFix(1); Fix.Flags|=MTK_RX_FINAL; HaveInput=TRUE; Poll();
  CHECK(A->Complete==1 && A->Status==STATUS_SUCCESS && B->Complete==1 &&
        B->Status==STATUS_NO_MORE_ENTRIES && !Pending && Device.FinalSeen);
  StartDevice(); Session();
  for (unsigned I=1;I<=8;I++) CHECK(!Get(I)->Complete);
  A=Get(9); CHECK(A->Complete==1 && A->Status==STATUS_DEVICE_BUSY);
  CHECK(Stop()->Status==STATUS_SUCCESS && !Pending && !ExtraCount);
  for (unsigned I=1;I<=8;I++) CHECK(Requests[I].Complete==1 && Requests[I].Status==STATUS_CANCELLED);

}
static VOID CachedFixOrderTest(void) {
  REQUEST *R; GNSS_EVENT_2 *E; unsigned Before;
  StartDevice(); Session();
  MakeFix(1); Fix.Location.LatitudeBits=Bits(12.0); HaveInput=TRUE; Poll();
  CHECK(Device.HaveEvent && Device.Sequence==1);
  MakeFix(2); Fix.Location.LatitudeBits=Bits(13.0); Fix.Flags|=MTK_RX_FINAL;
  HaveInput=TRUE; Before=PollCalls; Poll();
  CHECK(PollCalls==Before && HaveInput && Device.HaveEvent && Device.Sequence==1 && !Device.FinalSeen);
  R=Get(1); E=(void *)R->Buffer;
  CHECK(R->Complete==1 && R->Status==STATUS_SUCCESS && !E->FixData2.IsFinalFix &&
    E->FixData2.BasicData.Latitude==12.0);
  R=Get(2); CHECK(Pending==R && !R->Complete);
  Poll(); E=(void *)R->Buffer;
  CHECK(PollCalls==Before+1 && !HaveInput && R->Complete==1 && R->Status==STATUS_SUCCESS &&
    E->FixData2.IsFinalFix && E->FixData2.BasicData.Latitude==13.0 && Device.FinalSeen);
  CHECK(Stop()->Status==STATUS_SUCCESS);

  StartDevice(); Session(); MakeFix(1); HaveInput=TRUE; Poll();
  MakeFix(2); Fix.Location.LatitudeBits=Bits(14.0); HaveInput=TRUE;
  Clock+=MTK_RX_MAX_AGE_MS; Before=PollCalls; Poll();
  CHECK(PollCalls==Before+1 && !HaveInput && Device.HaveEvent && Device.Sequence==2);
  R=Get(1); E=(void *)R->Buffer;
  CHECK(R->Complete==1 && R->Status==STATUS_SUCCESS && E->FixData2.BasicData.Latitude==14.0);
}
static VOID FailureTest(void) {
  REQUEST *R,*Q;GNSS_FIXSESSION_PARAM P;unsigned Before,I;
  StartDevice();Session();R=Get(1);MakeFix(1);Fix.Location.LatitudeBits=MTK_RX_NAN;HaveInput=TRUE;Poll();
  CHECK(R->Complete==1 && R->Status==STATUS_DATA_ERROR && Device.Faulted && !Device.Active && !ProviderActive && !ProviderEnabled);
  P=Parameters();Q=Issue(0,IOCTL_GNSS_START_FIXSESSION,&P,sizeof(P),0);CHECK(Q->Status==STATUS_DEVICE_NOT_READY);
  CHECK(Command(GNSS_ResetEngine,FALSE)->Status==STATUS_SUCCESS && !Device.Faulted && ProviderEnabled);
  Q=Issue(0,IOCTL_GNSS_START_FIXSESSION,&P,sizeof(P),0);CHECK(Q->Status==STATUS_SUCCESS);
  R=Get(1);FailCode=MTK_RX_STOP;FailTimes=1;Q=Stop();CHECK(Q->Status==STATUS_IO_DEVICE_ERROR && R->Complete==1 && Device.Faulted && !ProviderActive);
  CHECK(Command(GNSS_ClearAgnssData,FALSE)->Status==STATUS_DEVICE_NOT_READY);
  CHECK(Command(GNSS_ResetEngine,FALSE)->Status==STATUS_SUCCESS && !Device.Faulted && !Device.Lease);
  for (I=0;I<3;I++) {
    StartDevice();Session();CHECK(Stop()->Status==STATUS_SUCCESS);UseLeaseOverride=1;LeaseOverride=I==0?0:I==1?Device.LastLease:Device.LastLease-1;
    Q=Issue(0,IOCTL_GNSS_START_FIXSESSION,&P,sizeof(P),0);CHECK(Q->Status==STATUS_DATA_ERROR && Device.Faulted && !ProviderActive);
  }
  StartDevice();Session();R=Get(1);Before=LowerCalls;Clock=999;GnPoll(TIMER);
  CHECK(R->Status==STATUS_DATA_ERROR && Device.Faulted && LowerCalls==Before+1);
  StartDevice();Session();R=Get(1);FailCode=MTK_RX_COMMAND;FailCommand=GNSS_SetLocationServiceEnabled;FailTimes=1;
  CHECK(!NT_SUCCESS(GnD0Exit(DEV,WdfPowerDeviceD3)) && !Device.Ready && Device.Faulted && R->Complete==1);
  StartDevice();Session();CHECK(Command(GNSS_SetLocationServiceEnabled,FALSE)->Status==STATUS_SUCCESS && !Device.Active && !ProviderActive && !Device.Enabled);
  CHECK(Command(GNSS_SetLocationServiceEnabled,2)->Status==STATUS_INVALID_PARAMETER);
  CHECK(Command(GNSS_CustomCommand,FALSE)->Status==STATUS_NOT_SUPPORTED);
}
static VOID MapTest(void) {
  unsigned I;NTSTATUS S;ULONGLONG Stamp;
  StartDevice();Session();MakeFix(1);
  CHECK(GnTimestamp(-11644473600000LL,&Stamp) && !Stamp && !GnTimestamp(INT64_MIN,&Stamp) && !GnTimestamp(INT64_MAX,&Stamp));
  CHECK(GnFresh(Wall+10000000,Wall) && !GnFresh(Wall+10010000,Wall) && GnFresh(Wall-50000000,Wall) && !GnFresh(Wall-50010000,Wall));
  for (I=0;I<17;I++) {
    memcpy(&Mutated,&Fix,sizeof(Fix));memset(&MapOutput,0xa5,sizeof(MapOutput));
    switch (I) {
      case 0:Mutated.Size--;break;case 1:Mutated.Version++;break;case 2:Mutated.Flags|=64;break;
      case 3:Mutated.Location.LatitudeBits=Bits(90.1);break;case 4:Mutated.Location.LongitudeBits=Bits(-180.1);break;
      case 5:Mutated.Location.SpeedBits=MTK_RX_NAN;break;case 6:Mutated.Location.BearingBits=Bits(360.0);break;
      case 7:Mutated.MslAltitudeBits=MTK_RX_NAN;break;case 8:Mutated.HdopBits=Bits(-1.0);break;
      case 9:Mutated.Flags|=MTK_RX_HORIZONTAL_95;Mutated.HorizontalAccuracy95Bits=MTK_RX_NAN;break;
      case 10:Mutated.SatelliteCount=65;break;case 11:Mutated.Satellites[1].Svid=0;break;
      case 12:Mutated.Satellites[1].Constellation=0;break;case 13:Mutated.Satellites[1].Cn0Bits=0x7fc00000;break;
      case 14:Mutated.Satellites[1].ElevationBits=0x7f800000;break;case 15:Mutated.Satellites[1].AzimuthBits=0x43b48000;break;
      default:Mutated.Flags&=~MTK_RX_SATELLITES;break;
    }
    S=GnMap(&Mutated,Device.Info.Generation,Device.Lease,0,42,7,Wall,&MapOutput);
    CHECK(S==STATUS_DATA_ERROR && Filled(&MapOutput,sizeof(MapOutput),0xa5));
  }
  MakeFix(1);Fix.Location.Flags=1;Fix.Flags=MTK_RX_UTC;Fix.SatelliteCount=0;
  CHECK(GnMap(&Fix,Device.Info.Generation,Device.Lease,0,42,7,Wall,&MapOutput)==STATUS_SUCCESS);
  CHECK(MapOutput.FixData2.FixLevelOfDetails==GNSS_FIXDETAIL_BASIC && Bits(MapOutput.FixData2.BasicData.Altitude)==MTK_RX_NAN && Bits(MapOutput.FixData2.BasicData.AltitudeEllipsoid)==MTK_RX_NAN && Bits(MapOutput.FixData2.BasicData.Speed)==MTK_RX_NAN);
}
/* Failed provider queries must not erase the last accepted lease. Cached
 * fixes also expire on monotonic time if wall time stalls or moves back. */
static VOID InputValidationTest(void) {
  GNSS_FIXSESSION_PARAM P;GNSS_PLATFORM_CAPABILITY PlatformData;GNSS_STOPFIXSESSION_PARAM StopData;
  GNSS_DRIVERCOMMAND_PARAM C;REQUEST *R;unsigned I,Before;ULONG Id;
  StartDevice();Before=LowerCalls;
  GnFileCreate(DEV,(WDFREQUEST)&Requests[6],(WDFFILEOBJECT)(ULONG_PTR)0x7100);
  CHECK(Requests[6].Complete==1 && Requests[6].Status==STATUS_DEVICE_NOT_READY && Device.Owner==FILE1);
  memset(&PlatformData,0,sizeof(PlatformData));PlatformData.Size=sizeof(PlatformData);PlatformData.Version=5;
  CHECK(Issue(0,IOCTL_GNSS_SEND_PLATFORM_CAPABILITY,&PlatformData,sizeof(PlatformData),0)->Status==STATUS_NOT_SUPPORTED && !Device.PlatformKnown);
  PlatformData.Version=6;PlatformData.SupportAgnssInjection=2;
  CHECK(Issue(0,IOCTL_GNSS_SEND_PLATFORM_CAPABILITY,&PlatformData,sizeof(PlatformData),0)->Status==STATUS_NOT_SUPPORTED);
  CHECK(LowerCalls==Before);Session();CHECK(Stop()->Status==STATUS_SUCCESS);
  for (I=0;I<9;I++) {
    P=Parameters();switch (I) {
      case 0:P.Size--;break;case 1:P.Version=5;break;
      case 2:P.SessionType=GNSS_FixSession_ContinuousTracking;break;
      case 3:P.FixLevelOfDetails=8;break;case 4:P.HorizontalConfidence=101;break;
      case 5:P.SingleShotParam.Size--;break;case 6:P.SingleShotParam.Version=5;break;
      default:break;
    }
    Before=LowerCalls;
    R=Issue(0,IOCTL_GNSS_START_FIXSESSION,&P,sizeof(P)-(I==7),0);
    if (I==8) CHECK(R->Status==STATUS_SUCCESS && Device.Active && LowerCalls==Before+1);
    else CHECK(R->Status==STATUS_INVALID_PARAMETER && !Device.Active && LowerCalls==Before);
  }
  Before=LowerCalls;R=Get(1);CHECK(!R->Complete);
  Id=43;CHECK(Issue(2,IOCTL_GNSS_GET_FIXDATA,&Id,sizeof(Id),sizeof(GNSS_EVENT_2))->Status==STATUS_INVALID_DEVICE_STATE && Pending==R);
  Id=42;CHECK(Issue(2,IOCTL_GNSS_GET_FIXDATA,&Id,sizeof(Id),sizeof(GNSS_EVENT_2)-1)->Status==STATUS_BUFFER_TOO_SMALL && Pending==R);
  memset(&StopData,0,sizeof(StopData));StopData.Size=sizeof(StopData);StopData.Version=6;StopData.FixSessionID=43;
  CHECK(Issue(2,IOCTL_GNSS_STOP_FIXSESSION,&StopData,sizeof(StopData),0)->Status==STATUS_INVALID_PARAMETER && Pending==R && Device.Active && LowerCalls==Before);
  CHECK(Stop()->Status==STATUS_SUCCESS && R->Complete==1);
  for (I=0;I<7;I++) {
    memset(&C,0,sizeof(C));C.Size=sizeof(C);C.Version=6;C.CommandType=GNSS_ResetEngine;
    switch (I) {
      case 0:C.Size--;break;case 1:C.Version=5;break;case 2:C.Reserved=1;break;
      case 3:C.CommandDataSize=0xffffffffU;break;case 4:C.CommandDataSize=8;break;
      case 5:C.CommandType=GNSS_SetLocationServiceEnabled;C.CommandDataSize=1;break;
      default:break;
    }
    Before=LowerCalls;
    R=Issue(0,IOCTL_GNSS_SEND_DRIVERCOMMAND,&C,I==6?FIELD_OFFSET(GNSS_DRIVERCOMMAND_PARAM,CommandData)-1:sizeof(C),0);
    CHECK(R->Status==STATUS_INVALID_PARAMETER && LowerCalls==Before);
  }
  CHECK(Issue(0,IOCTL_GNSS_GET_CHIPSETINFO,NULL,0,sizeof(GNSS_CHIPSETINFO)-1)->Status==STATUS_BUFFER_TOO_SMALL);
  CHECK(Issue(0,0xffffffffU,NULL,0,0)->Status==STATUS_NOT_SUPPORTED);
}
static VOID LifetimeTest(void) {
  GNSS_FIXSESSION_PARAM P=Parameters();REQUEST *R;ULONGLONG Old;unsigned Before;
  StartDevice();Session();Old=Device.LastLease;
  CHECK(GnD0Exit(DEV,WdfPowerDeviceD3)==STATUS_SUCCESS);
  InfoVariant=1;CHECK(!NT_SUCCESS(GnD0Entry(DEV,WdfPowerDeviceD3)) && Device.LastLease==Old);
  InfoVariant=0;CHECK(GnD0Entry(DEV,WdfPowerDeviceD3)==STATUS_SUCCESS && Device.LastLease==Old);
  UseLeaseOverride=1;LeaseOverride=Old;
  R=Issue(0,IOCTL_GNSS_START_FIXSESSION,&P,sizeof(P),0);
  CHECK(R->Status==STATUS_DATA_ERROR && Device.Faulted && !ProviderActive);
  UseLeaseOverride=0;CHECK(GnD0Exit(DEV,WdfPowerDeviceD3)==STATUS_SUCCESS);
  Generation++;CHECK(GnD0Entry(DEV,WdfPowerDeviceD3)==STATUS_SUCCESS && !Device.LastLease && Device.Generation==Generation);
  R=Issue(0,IOCTL_GNSS_START_FIXSESSION,&P,sizeof(P),0);CHECK(R->Status==STATUS_SUCCESS && Device.Active);
  R=Get(1);FailCode=MTK_RX_POLL;FailStatus=STATUS_DEVICE_NOT_READY;FailTimes=1;Before=TimerStarts;Poll();
  CHECK(R->Complete==1 && R->Status==STATUS_DEVICE_NOT_READY && Device.Faulted && !ProviderEnabled && !ProviderActive && TimerStarts==Before);
  StartDevice();Session();MakeFix(1);HaveInput=TRUE;Poll();CHECK(Device.HaveEvent);
  Wall-=5000000;Clock+=5001;R=Get(1);CHECK(!R->Complete && Pending==R && !Device.HaveEvent && Zero(&Device.Event,sizeof(Device.Event)));
  CHECK(Stop()->Status==STATUS_SUCCESS && R->Complete==1 && R->Status==STATUS_CANCELLED);
  StartDevice();Session();MakeFix(1);HaveInput=TRUE;Poll();Clock--;
  R=Get(1);CHECK(!R->Complete && !Device.HaveEvent && Zero(&Device.Event,sizeof(Device.Event)));
  StartDevice();Session();MakeFix(1);Fix.Flags|=MTK_RX_FINAL;HaveInput=TRUE;Poll();Clock+=5001;
  R=Get(1);CHECK(R->Complete==1 && R->Status==STATUS_IO_TIMEOUT && Device.FinalSeen && !Device.HaveEvent);
}
static VOID NmeaBridgeTest(void) {
  static MTK_GNSS_NMEA_STREAM Stream;static MTK_GNSS_NMEA Report;unsigned Used;
  const unsigned char Rmc[]="$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n";
  const unsigned char Gga[]="$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
  StartDevice();Session();MakeFix(1);Fix.SatelliteCount=0;Fix.Flags=MTK_RX_UTC;
  CHECK(MtkGnssNmeaStreamInit(&Stream,1900,1000)==MtkGnssOk);
  CHECK(MtkGnssNmeaStreamReceive(&Stream,Stream.Epoch,1,Rmc,sizeof(Rmc)-1,&Used,&Report)==MtkGnssFrameReady);
  memcpy(&Fix.Location,&Report.Location,sizeof(Fix.Location));
  CHECK(GnMap(&Fix,Device.Info.Generation,Device.Lease,0,42,7,Wall,&MapOutput)==STATUS_SUCCESS && Bits(MapOutput.FixData2.BasicData.Altitude)==MTK_RX_NAN);
  CHECK(MtkGnssNmeaStreamReceive(&Stream,Stream.Epoch,2,Gga,sizeof(Gga)-1,&Used,&Report)==MtkGnssFrameReady && Report.Kind==3);
  memcpy(&Fix.Location,&Report.Location,sizeof(Fix.Location));Fix.Flags=MTK_RX_UTC|MTK_RX_MSL|MTK_RX_HDOP;
  Fix.MslAltitudeBits=Report.MslAltitudeBits;Fix.HdopBits=Report.HdopBits;
  CHECK(GnMap(&Fix,Device.Info.Generation,Device.Lease,0,42,7,Wall,&MapOutput)==STATUS_SUCCESS);
  CHECK(MapOutput.FixData2.BasicData.Altitude==545.4 && MapOutput.FixData2.BasicData.AltitudeEllipsoid==592.3 && Bits(MapOutput.FixData2.AccuracyData.HorizontalAccuracy)==MTK_RX_NAN);
}
int TestMain(void) {
  Setup();LifecycleTest();SessionTest();PendingFixOrderTest();CachedFixOrderTest();FailureTest();MapTest();InputValidationTest();LifetimeTest();NmeaBridgeTest();
  CHECK(!Depth);return (int)Failure;
}
