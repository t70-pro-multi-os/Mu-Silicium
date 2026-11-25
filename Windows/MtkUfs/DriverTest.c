/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <storport.h>
#include <stdarg.h>
ULONG TestInitialize(PVOID,PVOID,PHW_INITIALIZATION_DATA,PVOID);
PVOID TestMap(PVOID,INTERFACE_TYPE,ULONG,STOR_PHYSICAL_ADDRESS,ULONG,BOOLEAN);
VOID TestUnmap(PVOID,PVOID);
PVOID TestDma(PVOID,PPORT_CONFIGURATION_INFORMATION,ULONG);
STOR_PHYSICAL_ADDRESS TestPhysical(PVOID,PSCSI_REQUEST_BLOCK,PVOID,PULONG);
ULONG TestRead(PVOID,PULONG);
VOID TestWrite(PVOID,PULONG,ULONG);
VOID TestNotify(SCSI_NOTIFICATION_TYPE,PVOID,...);
VOID TestTime(PLARGE_INTEGER);
ULONG TestPerformanceCounter(PVOID,PLARGE_INTEGER,PLARGE_INTEGER);
VOID TestStall(ULONG);
BOOLEAN TestSynchronize(PVOID,PSTOR_SYNCHRONIZED_ACCESS,PVOID);
BOOLEAN TestEnablePassiveInitialization(PVOID,PHW_PASSIVE_INITIALIZE_ROUTINE);
VOID TestInitializeDpc(PVOID,PSTOR_DPC,PHW_DPC_ROUTINE);
BOOLEAN TestIssueDpc(PVOID,PSTOR_DPC,PVOID,PVOID);
VOID TestAcquireSpinLock(PVOID,STOR_SPINLOCK,PVOID,PSTOR_LOCK_HANDLE);
VOID TestReleaseSpinLock(PVOID,PSTOR_LOCK_HANDLE);
#if defined(__clang__) || defined(__GNUC__)
typedef __typeof__(&StorPortEnablePassiveInitialization) SDK_ENABLE_PASSIVE_INIT;
typedef __typeof__(&StorPortInitializeDpc) SDK_INITIALIZE_DPC;
typedef __typeof__(&StorPortIssueDpc) SDK_ISSUE_DPC;
typedef __typeof__(&StorPortAcquireSpinLock) SDK_ACQUIRE_SPIN_LOCK;
typedef __typeof__(&StorPortReleaseSpinLock) SDK_RELEASE_SPIN_LOCK;
typedef __typeof__(&StorPortSynchronizeAccess) SDK_SYNCHRONIZE_ACCESS;
typedef __typeof__(&StorPortQueryPerformanceCounter) SDK_PERFORMANCE_COUNTER;
_Static_assert(__builtin_types_compatible_p(__typeof__(&TestEnablePassiveInitialization),SDK_ENABLE_PASSIVE_INIT),
               "passive-init mock must match WDK StorPort signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&TestInitializeDpc),SDK_INITIALIZE_DPC),
               "DPC-init mock must match WDK StorPort signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&TestIssueDpc),SDK_ISSUE_DPC),
               "DPC-issue mock must match WDK StorPort signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&TestAcquireSpinLock),SDK_ACQUIRE_SPIN_LOCK),
               "lock-acquire mock must match WDK StorPort signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&TestReleaseSpinLock),SDK_RELEASE_SPIN_LOCK),
               "lock-release mock must match WDK StorPort signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&TestSynchronize),SDK_SYNCHRONIZE_ACCESS),
               "synchronize-access mock must match WDK StorPort signature");
_Static_assert(__builtin_types_compatible_p(__typeof__(&TestPerformanceCounter),SDK_PERFORMANCE_COUNTER),
               "performance-counter mock must match WDK StorPort signature");
#endif
#define StorPortInitialize TestInitialize
#define StorPortGetDeviceBase TestMap
#define StorPortFreeDeviceBase TestUnmap
#define StorPortGetUncachedExtension TestDma
#define StorPortGetPhysicalAddress TestPhysical
#undef StorPortReadRegisterUlong
#undef StorPortWriteRegisterUlong
#define StorPortReadRegisterUlong TestRead
#define StorPortWriteRegisterUlong TestWrite
#define StorPortNotification TestNotify
#define StorPortQuerySystemTime TestTime
#define StorPortQueryPerformanceCounter TestPerformanceCounter
#define StorPortStallExecution TestStall
#define StorPortSynchronizeAccess TestSynchronize
#undef StorPortEnablePassiveInitialization
#undef StorPortInitializeDpc
#undef StorPortIssueDpc
#undef StorPortAcquireSpinLock
#undef StorPortReleaseSpinLock
#define StorPortEnablePassiveInitialization TestEnablePassiveInitialization
#define StorPortInitializeDpc TestInitializeDpc
#define StorPortIssueDpc TestIssueDpc
#define StorPortAcquireSpinLock TestAcquireSpinLock
#define StorPortReleaseSpinLock TestReleaseSpinLock
#include "Driver.c"
#pragma function(memcpy,memset)
static void MaybeInjectFatalDuringCopy(void *,size_t);
static BOOLEAN ShouldInjectFatalDuringCopy(void *,size_t);
void *memcpy(void *A,const void *B,size_t N) {
  volatile UCHAR *D=A; const volatile UCHAR *S=B; size_t I=0;
  if(ShouldInjectFatalDuringCopy(A,N)) {
    while(I+8<=N/2) {
      D[I]=S[I]; D[I+1]=S[I+1]; D[I+2]=S[I+2]; D[I+3]=S[I+3];
      D[I+4]=S[I+4]; D[I+5]=S[I+5]; D[I+6]=S[I+6]; D[I+7]=S[I+7]; I+=8;
    }
    while(I<N/2) { D[I]=S[I]; I++; }
    MaybeInjectFatalDuringCopy(A,I);
  }
  while(I+8<=N) {
    D[I]=S[I]; D[I+1]=S[I+1]; D[I+2]=S[I+2]; D[I+3]=S[I+3];
    D[I+4]=S[I+4]; D[I+5]=S[I+5]; D[I+6]=S[I+6]; D[I+7]=S[I+7]; I+=8;
  }
  while(I<N) { D[I]=S[I]; I++; }
  return A;
}
void *memset(void *A,int C,size_t N) {
  volatile UCHAR *D=A; UCHAR V=(UCHAR)C; size_t I=0;
  while(I+8<=N) {
    D[I]=V; D[I+1]=V; D[I+2]=V; D[I+3]=V;
    D[I+4]=V; D[I+5]=V; D[I+6]=V; D[I+7]=V; I+=8;
  }
  while(I<N) { D[I]=V; I++; }
  return A;
}
static ULONG Failure,Checks;
#define CHECK(X) do { Checks++; if(!(X) && !Failure) Failure=__LINE__; } while(0)
static UFS_DEVICE D;
static ULONG Reg[UFS_MMIO_BYTES/4],Handoff[4];
static __declspec(align(4096)) UCHAR Dma[MTK_UFS_DMA_BYTES+4096];
static PORT_CONFIGURATION_INFORMATION Config;
static ACCESS_RANGE Ranges[2];
static SCSI_REQUEST_BLOCK S,Second;
static UCHAR Data[MTK_UFS_DATA_BYTES],Sense[32];
typedef struct {
  SRB_IO_CONTROL Header;
  UCHAR Request[MTK_UFS_POWER_REQUEST_BYTES];
} UFS_POWER_TEST_BUFFER;
C_ASSERT(sizeof(UFS_POWER_TEST_BUFFER)==92);
C_ASSERT(FIELD_OFFSET(UFS_POWER_TEST_BUFFER,Request)==28);
static UFS_POWER_TEST_BUFFER PowerBuffer;
static MTK_UFS_POWER_REQUEST PowerRequest;
static UCHAR DmaPrefix[0x100];
static ULONG Maps,Unmaps,MapFail,DmaFail,PhysicalCalls,PhysicalBad,WriteCount,Completes,Timers,Stalls;
static ULONG TimerArmCount,TimerCancelCount;
static ULONG CompletedStatus,CompletedBytes,LockDepth,SyncCalls;
static ULONG DpcDepth, DpcIssueCount, DpcDispatchCount, DpcInitCount, PassiveInitCount;
static ULONG AcquireCount, InjectFatalAcquireAt, LinkDownCount;
static BOOLEAN HceStuck, IeStuck, DpcQueued, PassiveInitAllowed, InjectFatalDuringCopy, TimerScheduled;
static BOOLEAN InjectStopDuringCopy, ReenterOnComplete, ReentrantStartResult;
static SCSI_ADAPTER_CONTROL_STATUS StopDuringCopyStatus;
static ULONGLONG Clock,PerfClock,PerfFrequency;
static ULONG WallTimeCalls,PerfQueryStatus;
static HW_INITIALIZATION_DATA Registered;
static PHW_PASSIVE_INITIALIZE_ROUTINE SavedPassiveInitialize;
static PHW_DPC_ROUTINE SavedDpcRoutine;
static PHW_STARTIO NativeStartIo=UfsStartIo;
static PHW_TIMER NativeTimer=UfsTimer;
static BOOLEAN ShouldInjectFatalDuringCopy(void *Destination,size_t Length) {
  return (BOOLEAN)(Destination==Data && Length==4096 &&
                   (InjectStopDuringCopy || InjectFatalDuringCopy));
}
static void MaybeInjectFatalDuringCopy(void *Destination,size_t Offset) {
  if(InjectStopDuringCopy && Destination==Data && Offset==4096/2) {
    InjectStopDuringCopy=FALSE; LockDepth++;
    StopDuringCopyStatus=UfsControl(&D,ScsiStopAdapter,NULL);
    LockDepth--;
  }
  if(InjectFatalDuringCopy && Destination==Data && Offset==4096/2) {
    InjectFatalDuringCopy=FALSE; Reg[0x20/4]|=MTK_UFS_ERROR_INTERRUPTS;
    LockDepth++; (void)UfsInterrupt(&D); LockDepth--;
  }
}
static void Reset(void) {
  RtlZeroMemory(&D,sizeof(D)); RtlZeroMemory(Reg,sizeof(Reg)); RtlZeroMemory(&Config,sizeof(Config));
  RtlZeroMemory(Ranges,sizeof(Ranges)); RtlZeroMemory(&S,sizeof(S)); RtlZeroMemory(&Second,sizeof(Second));
  RtlFillMemory(Dma,sizeof(Dma),0xa5); RtlFillMemory(Data,sizeof(Data),0x7a); RtlFillMemory(Sense,sizeof(Sense),0x7b);
  Reg[0]=0x0100001f; Reg[2]=0x210; Reg[0x34/4]=1; Reg[0x30/4]=15;
  Reg[0x2240/4]=0x10450000;
  Handoff[0]=1; Handoff[1]=(ULONG)UFS_MMIO; Handoff[2]=2; Handoff[3]=1;
  Ranges[0].RangeStart.QuadPart=UFS_MMIO; Ranges[0].RangeLength=UFS_MMIO_BYTES; Ranges[0].RangeInMemory=TRUE;
  Ranges[1].RangeStart.QuadPart=UFS_HANDOFF; Ranges[1].RangeLength=16; Ranges[1].RangeInMemory=TRUE;
  Config.Length=sizeof(Config); Config.AdapterInterfaceType=ACPIBus; Config.NumberOfAccessRanges=2;
  Config.AccessRanges=(ACCESS_RANGE (*)[])Ranges; Config.BusInterruptVector=169; Config.InterruptMode=LevelSensitive;
  Maps=Unmaps=MapFail=DmaFail=PhysicalCalls=PhysicalBad=WriteCount=Completes=Timers=Stalls=0;
  TimerArmCount=TimerCancelCount=0; TimerScheduled=FALSE;
  LockDepth=SyncCalls=DpcDepth=DpcIssueCount=DpcDispatchCount=DpcInitCount=PassiveInitCount=0;
  AcquireCount=InjectFatalAcquireAt=LinkDownCount=0;
  DpcQueued=FALSE; PassiveInitAllowed=TRUE;
  HceStuck=IeStuck=InjectFatalDuringCopy=InjectStopDuringCopy=FALSE;
  StopDuringCopyStatus=ScsiAdapterControlUnsuccessful;
  ReenterOnComplete=ReentrantStartResult=FALSE;
  SavedPassiveInitialize=NULL; SavedDpcRoutine=NULL; Clock=100000000;
  PerfClock=100000000; PerfFrequency=10000000; WallTimeCalls=0; PerfQueryStatus=STOR_STATUS_SUCCESS;
}
ULONG TestInitialize(PVOID A,PVOID B,PHW_INITIALIZATION_DATA H,PVOID C) {
  UNREFERENCED_PARAMETER(A); UNREFERENCED_PARAMETER(B); CHECK(!C); Registered=*H; return 0;
}
PVOID TestMap(PVOID E,INTERFACE_TYPE Bus,ULONG Number,STOR_PHYSICAL_ADDRESS Address,ULONG Bytes,BOOLEAN Port) {
  CHECK(E==&D && Bus==ACPIBus && !Number && !Port); Maps++;
  if(MapFail==Maps) return NULL;
  if((ULONGLONG)Address.QuadPart==UFS_HANDOFF) { CHECK(Bytes==16); return Handoff; }
  CHECK((ULONGLONG)Address.QuadPart==UFS_MMIO && Bytes==UFS_MMIO_BYTES); return Reg;
}
VOID TestUnmap(PVOID E,PVOID Base) { CHECK(E==&D && (Base==Handoff||Base==Reg)); Unmaps++; }
PVOID TestDma(PVOID E,PPORT_CONFIGURATION_INFORMATION C,ULONG N) {
  CHECK(E==&D && C==&Config && N==MTK_UFS_DMA_BYTES+1023);
  CHECK(C->Master && C->ScatterGather && C->MapBuffers==3 && C->DmaAddressWidth==36);
  CHECK(C->SynchronizationModel==StorSynchronizeFullDuplex && C->MaxNumberOfIO==1 && !C->VirtualDevice);
  return DmaFail?NULL:Dma;
}
STOR_PHYSICAL_ADDRESS TestPhysical(PVOID E,PSCSI_REQUEST_BLOCK Request,PVOID Buffer,PULONG Length) {
  STOR_PHYSICAL_ADDRESS P;
  CHECK(E==&D && !Request && (PUCHAR)Buffer>=Dma && (PUCHAR)Buffer<Dma+sizeof(Dma));
  PhysicalCalls++;
  P.QuadPart=0x80000000ULL+((PUCHAR)Buffer-Dma);
  if(PhysicalBad==1) *Length=16;
  if(PhysicalBad==2) P.QuadPart=0x1000000000ULL+((PUCHAR)Buffer-Dma);
  if(PhysicalBad==3 && PhysicalCalls==2) P.QuadPart+=1024;
  return P;
}
ULONG TestRead(PVOID E,PULONG Address) {
  CHECK(E==&D);
  CHECK((Address>=Reg && Address<Reg+ARRAYSIZE(Reg)) || (Address>=Handoff && Address<Handoff+4));
  return *Address;
}
VOID TestWrite(PVOID E,PULONG Address,ULONG Value) {
  ULONG Offset=(ULONG)((PUCHAR)Address-(PUCHAR)Reg);
  CHECK(E==&D && Offset<sizeof(Reg) && !(Offset&3) && (LockDepth==1 || DpcDepth==1)); WriteCount++;
  if(Offset==0x20) *Address&=~Value;
  else if(Offset==0x24 && !Value && IeStuck) { }
  else if(Offset==0x34 && !Value) {
    if(!HceStuck) { *Address=0; Reg[0x58/4]=Reg[0x78/4]=0; }
  } else *Address=Value;
}
VOID TestNotify(SCSI_NOTIFICATION_TYPE Type,PVOID E,...) {
  va_list A; CHECK(E==&D && LockDepth<=1 && DpcDepth<=1); va_start(A,E);
  if(Type==RequestComplete) {
    PSCSI_REQUEST_BLOCK Request=va_arg(A,PSCSI_REQUEST_BLOCK);
    CHECK((Request==&S || Request==&Second) && ((LockDepth==1 && !DpcDepth) || (!LockDepth && DpcDepth==1))); Completes++;
    CompletedStatus=Request->SrbStatus; CompletedBytes=Request->DataTransferLength;
    if(ReenterOnComplete && Request==&S) {
      ReenterOnComplete=FALSE; ReentrantStartResult=NativeStartIo(&D,&Second);
    }
  } else if(Type==RequestTimerCall) {
    PHW_TIMER Timer=va_arg(A,PHW_TIMER); ULONG Interval=va_arg(A,ULONG);
    CHECK(Timer==NativeTimer && LockDepth==1);
    if(!Interval) { CHECK(!D.TimerArmed); TimerScheduled=FALSE; TimerCancelCount++; }
    else {
      CHECK(D.TimerArmed && Interval==((D.StopRetryPending ||
            (D.PendingShutdown && D.ShutdownPhase==UfsShutdownHibernate8) ||
            D.Transport.StopInProgress)?
            UFS_STOP_RETRY_US:UFS_TICK_US)); TimerScheduled=TRUE; TimerArmCount++;
    }
    Timers++;
  } else if(Type==LinkDown) {
    CHECK(!LockDepth && DpcDepth==1); LinkDownCount++;
  } else CHECK(FALSE);
  va_end(A);
}
VOID TestTime(PLARGE_INTEGER T) { WallTimeCalls++; T->QuadPart=Clock; }
ULONG TestPerformanceCounter(PVOID E,PLARGE_INTEGER F,PLARGE_INTEGER C) {
  CHECK(E==&D && C!=NULL);
  if(F) F->QuadPart=(LONGLONG)PerfFrequency;
  C->QuadPart=(LONGLONG)PerfClock;
  return PerfQueryStatus;
}
VOID TestStall(ULONG Us) { CHECK(Us==10 && LockDepth==1); Stalls++; }
BOOLEAN TestSynchronize(PVOID E,PSTOR_SYNCHRONIZED_ACCESS Callback,PVOID Context) {
  BOOLEAN Result; CHECK(E==&D && LockDepth==0); LockDepth++; SyncCalls++;
  Result=Callback(E,Context); LockDepth--; return Result;
}
BOOLEAN TestEnablePassiveInitialization(PVOID E,PHW_PASSIVE_INITIALIZE_ROUTINE Callback) {
  CHECK(E==&D && Callback==UfsPassiveInitialize && !LockDepth && !DpcDepth);
  PassiveInitCount++; SavedPassiveInitialize=Callback; return PassiveInitAllowed;
}
VOID TestInitializeDpc(PVOID E,PSTOR_DPC Dpc,PHW_DPC_ROUTINE Callback) {
  CHECK(E==&D && Dpc==&D.Dpc && Callback==UfsDpc && !LockDepth && !DpcDepth);
  DpcInitCount++; SavedDpcRoutine=Callback;
}
BOOLEAN TestIssueDpc(PVOID E,PSTOR_DPC Dpc,PVOID Arg1,PVOID Arg2) {
  CHECK(E==&D && Dpc==&D.Dpc && !Arg1 && !Arg2 && LockDepth==1);
  DpcIssueCount++;
  if(DpcQueued) return FALSE;
  DpcQueued=TRUE; return TRUE;
}
VOID TestAcquireSpinLock(PVOID E,STOR_SPINLOCK LockType,PVOID Parameters,PSTOR_LOCK_HANDLE Handle) {
  CHECK(E==&D && LockType==InterruptLock && !Parameters && !LockDepth);
  LockDepth++; AcquireCount++;
  if(InjectFatalAcquireAt && AcquireCount==InjectFatalAcquireAt) {
    InjectFatalAcquireAt=0; Reg[0x20/4]|=MTK_UFS_ERROR_INTERRUPTS;
    CHECK(UfsInterrupt(&D));
  }
  UNREFERENCED_PARAMETER(Handle);
}
VOID TestReleaseSpinLock(PVOID E,PSTOR_LOCK_HANDLE Handle) {
  CHECK(E==&D && LockDepth==1); LockDepth--;
  UNREFERENCED_PARAMETER(Handle);
}
static void RunDpcs(void) {
  ULONG I=0;
  CHECK(!LockDepth && !DpcDepth);
  while(DpcQueued && I++<16) {
    DpcQueued=FALSE; DpcDispatchCount++; DpcDepth++;
    SavedDpcRoutine(&D.Dpc,&D,NULL,NULL);
    DpcDepth--;
  }
  CHECK(!DpcQueued);
}
static BOOLEAN TestStartIo(PVOID E,PSCSI_REQUEST_BLOCK Request) {
  BOOLEAN Result=NativeStartIo(E,Request);
  RunDpcs(); return Result;
}
static void TestTick(PVOID E) {
  TimerScheduled=FALSE; NativeTimer(E); RunDpcs();
}
#define UfsStartIo TestStartIo
#define UfsTimer TestTick
static ULONG Find(void) { BOOLEAN Again=TRUE; ULONG R=UfsFind(&D,NULL,NULL,NULL,&Config,&Again); CHECK(!Again); return R; }
static BOOLEAN Initialize(void) {
  BOOLEAN R;
  CHECK(!LockDepth && !DpcDepth);
  R=UfsInitialize(&D);
  if(!R) return FALSE;
  CHECK(SavedPassiveInitialize==UfsPassiveInitialize && !LockDepth && !DpcDepth);
  if(!SavedPassiveInitialize(&D)) return FALSE;
  RunDpcs();
  return D.Stage==UfsNop && !D.DmaQuarantined;
}
static BOOLEAN InterruptOnly(void) {
  BOOLEAN R; CHECK(!LockDepth && !DpcDepth); LockDepth++; R=UfsInterrupt(&D); LockDepth--; return R;
}
static BOOLEAN Interrupt(void) { BOOLEAN R=InterruptOnly(); if(R) RunDpcs(); return R; }
static void TickOnly(void) { CHECK(!LockDepth && !DpcDepth); TimerScheduled=FALSE; NativeTimer(&D); }
static void QueueWork(ULONG Work) {
  CHECK(!LockDepth && !DpcDepth); LockDepth++; UfsQueueDpcLocked(&D,Work); LockDepth--;
}
static SCSI_ADAPTER_CONTROL_STATUS Control(SCSI_ADAPTER_CONTROL_TYPE T,PVOID P) {
  SCSI_ADAPTER_CONTROL_STATUS R; CHECK(!LockDepth && !DpcDepth); LockDepth++; R=UfsControl(&D,T,P); LockDepth--; RunDpcs(); return R;
}
static SCSI_ADAPTER_CONTROL_STATUS ControlOnly(SCSI_ADAPTER_CONTROL_TYPE T,PVOID P) {
  SCSI_ADAPTER_CONTROL_STATUS R; CHECK(!LockDepth && !DpcDepth); LockDepth++; R=UfsControl(&D,T,P); LockDepth--; return R;
}
static void Finish(BOOLEAN Nop) {
  UCHAR *P=Dma+MTK_UFS_RESPONSE_OFFSET;
  RtlZeroMemory(P,64); P[0]=Nop?0x20:0x21; P[2]=Nop?0:D.Transport.ActiveLun; P[3]=D.Transport.ActiveTag;
  Dma[8]=0; Reg[0x58/4]=0; Reg[0x20/4]|=1;
}
static void CheckCondition(UCHAR SenseKey,UCHAR Asc) {
  UCHAR *Response=Dma+MTK_UFS_RESPONSE_OFFSET;
  Finish(FALSE);
  Response[7]=2;
  Response[11]=20;
  Response[33]=18;
  Response[34]=0x70;
  Response[36]=SenseKey;
  Response[46]=Asc;
}
static void FinishQuery(UCHAR Opcode,UCHAR Value) {
  UCHAR *P=Dma+MTK_UFS_RESPONSE_OFFSET;
  RtlZeroMemory(P,64); P[0]=0x36; P[3]=D.Transport.ActiveTag;
  P[5]=Opcode==5?1:0x81; P[12]=Opcode; P[13]=4; P[23]=Value;
  Dma[8]=0; Reg[0x58/4]=0; Reg[0x20/4]|=1;
}
static void Capacity(void) {
  ULONGLONG Last=MTK_UFS_USER_BYTES/MTK_UFS_BLOCK_BYTES-1; ULONG I;
  UCHAR *P=Dma+MTK_UFS_DATA_OFFSET; RtlZeroMemory(P,32);
  for(I=0;I<8;I++) P[7-I]=(UCHAR)(Last>>(I*8)); P[10]=0x10;
}
static void Ready(void) {
  CHECK(Find()==SP_RETURN_FOUND && !WriteCount);
  CHECK(Initialize() && D.Stage==UfsNop && Timers==1 && TimerArmCount==1);
  Finish(TRUE); CHECK(Interrupt() && D.Stage==UfsCapacity && D.Transport.Active);
  CHECK(Dma[MTK_UFS_COMMAND_OFFSET+16]==0x9e && Dma[MTK_UFS_COMMAND_OFFSET+2]==2);
  Capacity(); Finish(FALSE); CHECK(Interrupt() && D.Stage==UfsReady && !D.Transport.Active);
  UfsTimer(&D); CHECK(!D.TimerArmed && !TimerScheduled);
}
static void ReadRequest(void);
static void ShutdownToHibernate8(void) {
  ReadRequest(); S.Function=SRB_FUNCTION_SHUTDOWN; S.DataTransferLength=0; S.SrbFlags=0;
  CHECK(UfsStartIo(&D,&S) && D.Pending==&S && D.PendingShutdown &&
        D.ShutdownPhase==UfsShutdownCache && !Completes);
  CHECK(Dma[MTK_UFS_COMMAND_OFFSET+16]==0x35 && Dma[MTK_UFS_COMMAND_OFFSET+2]==MTK_UFS_USER_LUN);
  Finish(FALSE); CHECK(Interrupt() && D.Pending==&S && D.Transport.Active &&
        D.ShutdownPhase==UfsShutdownPowerDown && D.Transport.ActiveLun==MTK_UFS_DEVICE_WLUN);
  CHECK(Dma[MTK_UFS_COMMAND_OFFSET+2]==MTK_UFS_DEVICE_WLUN &&
        Dma[MTK_UFS_COMMAND_OFFSET+16]==0x1b && Dma[MTK_UFS_COMMAND_OFFSET+20]==0x30 &&
        Reg[0x34/4]==1 && !Completes);
  Finish(FALSE); CHECK(Interrupt() && D.Pending==&S && !D.Transport.Active &&
        D.ShutdownPhase==UfsShutdownHibernate8 && D.Transport.UicPending);
  CHECK(Reg[0x90/4]==0x17 && !Reg[0x94/4] && !Reg[0x98/4] && !Reg[0x9c/4] &&
        Reg[0x34/4]==1 && !Completes);
}
static void ReadRequest(void) {
  RtlZeroMemory(&S,sizeof(S)); S.Length=sizeof(S); S.Function=SRB_FUNCTION_EXECUTE_SCSI;
  S.DataBuffer=Data; S.DataTransferLength=4096; S.SrbFlags=SRB_FLAGS_DATA_IN; S.TimeOutValue=1;
  S.CdbLength=10; S.Cdb[0]=0x28; S.Cdb[5]=1; S.Cdb[8]=1;
  S.SenseInfoBuffer=Sense; S.SenseInfoBufferLength=sizeof(Sense);
}
static void InitPowerRequest(USHORT Operation,ULONGLONG Generation) {
  RtlZeroMemory(&PowerBuffer,sizeof(PowerBuffer));
  RtlZeroMemory(&PowerRequest,sizeof(PowerRequest));
  PowerBuffer.Header.HeaderLength=sizeof(SRB_IO_CONTROL);
  RtlCopyMemory(PowerBuffer.Header.Signature,MTK_UFS_POWER_SRB_SIGNATURE,8);
  PowerBuffer.Header.ControlCode=MTK_UFS_POWER_SRB_CONTROL_CODE;
  PowerBuffer.Header.Length=sizeof(PowerRequest);
  PowerRequest.Size=sizeof(PowerRequest); PowerRequest.Version=MTK_UFS_POWER_VERSION;
  PowerRequest.Operation=Operation; PowerRequest.Generation=Generation;
  RtlCopyMemory(PowerBuffer.Request,&PowerRequest,sizeof(PowerRequest));
  RtlZeroMemory(&S,sizeof(S)); S.Length=sizeof(S); S.Function=SRB_FUNCTION_IO_CONTROL;
  S.DataBuffer=&PowerBuffer; S.DataTransferLength=sizeof(PowerBuffer);
}
static BOOLEAN SubmitPowerRequest(void) {
  BOOLEAN Result;
  RtlCopyMemory(PowerBuffer.Request,&PowerRequest,sizeof(PowerRequest));
  Result=NativeStartIo(&D,&S);
  RtlCopyMemory(&PowerRequest,PowerBuffer.Request,sizeof(PowerRequest));
  return Result;
}
static void CheckPowerEndpoint(void) {
  MTK_UFS_POWER_REQUEST *R=&PowerRequest;
  ULONG Before;
  ULONGLONG Generation;
  Reset(); Ready();

  InitPowerRequest(MtkUfsMiniPowerQuery,0); Before=Completes;
  CHECK(SubmitPowerRequest() && Completes==Before+1 && CompletedStatus==SRB_STATUS_SUCCESS &&
        CompletedBytes==sizeof(PowerBuffer) && PowerBuffer.Header.ReturnCode==MtkUfsPowerSuccess);
  CHECK(R->State==MtkUfsMiniPowerActive && R->Generation==1 && R->Result==MtkUfsPowerSuccess &&
        R->Capabilities==MTK_UFS_POWER_CAP_PROGRESS && !R->Flags);

  /* The private wire is rejected before state changes for malformed headers,
   * nonzero input-output fields, reserved input bytes, or truncated buffers. */
  Generation=D.MiniPowerGeneration;
  InitPowerRequest(MtkUfsMiniPowerPrepareDevice,Generation); PowerBuffer.Header.HeaderLength--;
  Before=Completes; CHECK(SubmitPowerRequest() && Completes==Before+1 &&
        CompletedStatus==SRB_STATUS_INVALID_REQUEST && D.MiniPowerGeneration==Generation && !D.MiniPowerActive);
  InitPowerRequest(MtkUfsMiniPowerPrepareDevice,Generation); PowerBuffer.Header.Signature[0]^=1;
  CHECK(SubmitPowerRequest() && CompletedStatus==SRB_STATUS_INVALID_REQUEST &&
        D.MiniPowerGeneration==Generation && !D.MiniPowerActive);
  InitPowerRequest(MtkUfsMiniPowerPrepareDevice,Generation); PowerBuffer.Header.ControlCode^=1;
  CHECK(SubmitPowerRequest() && CompletedStatus==SRB_STATUS_INVALID_REQUEST && D.MiniPowerGeneration==Generation);
  InitPowerRequest(MtkUfsMiniPowerPrepareDevice,Generation); PowerRequest.Capabilities=1;
  CHECK(SubmitPowerRequest() && CompletedStatus==SRB_STATUS_INVALID_REQUEST && D.MiniPowerGeneration==Generation);
  InitPowerRequest(MtkUfsMiniPowerPrepareDevice,Generation); PowerRequest.Reserved[2]=1;
  CHECK(SubmitPowerRequest() && CompletedStatus==SRB_STATUS_INVALID_REQUEST && D.MiniPowerGeneration==Generation);
  InitPowerRequest(MtkUfsMiniPowerPrepareDevice,Generation); S.DataTransferLength--;
  CHECK(SubmitPowerRequest() && CompletedStatus==SRB_STATUS_INVALID_REQUEST &&
        !CompletedBytes && D.MiniPowerGeneration==Generation);

  InitPowerRequest(0x7fff,Generation); Before=Completes;
  CHECK(SubmitPowerRequest() && Completes==Before+1 && CompletedStatus==SRB_STATUS_SUCCESS &&
        PowerBuffer.Header.ReturnCode==MtkUfsPowerUnsupported && R->Result==MtkUfsPowerUnsupported &&
        D.MiniPowerGeneration==Generation);
  InitPowerRequest(MtkUfsMiniPowerPrepareDevice,Generation+1);
  CHECK(SubmitPowerRequest() && PowerBuffer.Header.ReturnCode==MtkUfsPowerStaleGeneration &&
        D.MiniPowerGeneration==Generation && !D.MiniPowerActive);
  InitPowerRequest(MtkUfsMiniPowerStopHost,Generation);
  CHECK(SubmitPowerRequest() && PowerBuffer.Header.ReturnCode==MtkUfsPowerWrongState &&
        D.MiniPowerGeneration==Generation && !D.MiniPowerActive);

  InitPowerRequest(MtkUfsMiniPowerPrepareDevice,Generation); Before=Completes;
  CHECK(SubmitPowerRequest() && Completes==Before+1 && CompletedStatus==SRB_STATUS_SUCCESS &&
        PowerBuffer.Header.ReturnCode==MtkUfsPowerInProgress && R->Generation==Generation+1 &&
        R->State==MtkUfsMiniPowerDevicePreparing && D.MiniPowerActive && D.MiniPowerAdmissionClosed);
  Generation=R->Generation;
  InitPowerRequest(MtkUfsMiniPowerPrepareDevice,Generation); Before=D.MiniPowerGeneration;
  CHECK(SubmitPowerRequest() && PowerBuffer.Header.ReturnCode==MtkUfsPowerInProgress &&
        R->Generation==Generation && D.MiniPowerGeneration==Before && D.MiniPowerActive);
  InitPowerRequest(MtkUfsMiniPowerStopHost,Generation);
  CHECK(SubmitPowerRequest() && PowerBuffer.Header.ReturnCode==MtkUfsPowerBusy &&
        D.MiniPowerOperation==UfsPowerOpPrepareDevice && D.MiniPowerGeneration==Generation);
  RunDpcs(); CHECK(D.MiniPowerActive && D.MiniPowerPhase==UfsPowerPhaseReadBkops && D.Transport.Active);
  InitPowerRequest(MtkUfsMiniPowerQuery,0);
  CHECK(SubmitPowerRequest() && R->State==MtkUfsMiniPowerDevicePreparing &&
        R->Result==MtkUfsPowerSuccess && R->Generation==Generation);
  CHECK(Control(ScsiStopAdapter,NULL)==ScsiAdapterControlUnsuccessful && D.MiniPowerActive);

  /* Rollback before PRM PrepareStop keeps HCE and the transport live. It must
   * issue only WLUN ACTIVE, then READ CAPACITY; it must not send a second NOP. */
  Reset(); Ready(); D.MiniPowerState=MtkUfsMiniPowerDevicePrepared;
  D.MiniPowerFlags=MTK_UFS_POWER_DEVICE_DOWN_ACKED; D.MiniPowerAdmissionClosed=TRUE;
  D.MiniPowerOperation=UfsPowerOpNone; D.MiniPowerLastOperation=UfsPowerOpPrepareDevice;
  D.MiniPowerResult=MtkUfsPowerSuccess;
  Generation=D.MiniPowerGeneration;
  InitPowerRequest(MtkUfsMiniPowerRestartHost,Generation);
  CHECK(SubmitPowerRequest() && R->Result==MtkUfsPowerInProgress &&
        R->State==MtkUfsMiniPowerHostRestarting && R->Flags==MTK_UFS_POWER_DEVICE_DOWN_ACKED &&
        D.MiniPowerPhase==UfsPowerPhaseRestartLiveActive && D.Stage==UfsReady && D.Transport.Attached);
  RunDpcs();
  CHECK(D.MiniPowerPhase==UfsPowerPhaseRestartActive && D.Transport.Active &&
        Dma[MTK_UFS_COMMAND_OFFSET+16]==0x1b && Dma[MTK_UFS_COMMAND_OFFSET+20]==0x10 &&
        D.Stage==UfsReady && D.MiniPowerAdmissionClosed);
  Finish(FALSE); CHECK(Interrupt());
  CHECK(D.MiniPowerPhase==UfsPowerPhaseRestartCapacity && D.Transport.Active &&
        Dma[MTK_UFS_COMMAND_OFFSET+16]==0x9e && Dma[MTK_UFS_COMMAND_OFFSET+2]==MTK_UFS_USER_LUN &&
        D.Stage==UfsReady && D.MiniPowerAdmissionClosed);
  const ULONGLONG ResumeDeadline=D.MiniPowerStarted;
  CheckCondition(6,0);
  CHECK(Interrupt() && D.MiniPowerPhase==UfsPowerPhaseRestartCapacity &&
        D.Transport.Active && D.CapacityAttempts==2 && D.MiniPowerStarted==ResumeDeadline);
  CheckCondition(2,4);
  CHECK(Interrupt() && D.MiniPowerPhase==UfsPowerPhaseRestartCapacity &&
        D.Transport.Active && D.CapacityAttempts==3 && D.MiniPowerStarted==ResumeDeadline);
  Capacity(); Finish(FALSE); CHECK(Interrupt());
  CHECK(!D.MiniPowerActive && D.MiniPowerState==MtkUfsMiniPowerActive && !D.MiniPowerFlags &&
        D.MiniPowerResult==MtkUfsPowerSuccess && D.Stage==UfsReady && D.Transport.Attached && !D.Transport.Active);
  InitPowerRequest(MtkUfsMiniPowerQuery,0);
  CHECK(SubmitPowerRequest() && R->State==MtkUfsMiniPowerActive && !R->Flags &&
        R->Generation==Generation+1 && R->Result==MtkUfsPowerSuccess);

  /* After PRM restored a stopped host, the endpoint must relink through NOP,
   * then device ACTIVE and READ CAPACITY. This path is distinct from the
   * live-host rollback above and must not use the live-resume shortcut. */
  Reset(); Ready();
  D.MiniPowerState=MtkUfsMiniPowerDevicePrepared;
  D.MiniPowerFlags=MTK_UFS_POWER_DEVICE_DOWN_ACKED; D.MiniPowerAdmissionClosed=TRUE;
  InitPowerRequest(MtkUfsMiniPowerStopHost,D.MiniPowerGeneration);
  CHECK(SubmitPowerRequest() && D.MiniPowerActive);
  RunDpcs();
  for(ULONG Retry=0;Retry<4 && D.MiniPowerActive;Retry++) UfsTimer(&D);
  CHECK(!D.MiniPowerActive && D.MiniPowerState==MtkUfsMiniPowerHostStopped &&
        D.MiniPowerResult==MtkUfsPowerSuccess && D.Transport.Faulted &&
        !D.Transport.Attached && !D.Transport.Active && !D.Transport.StopInProgress);
  /* Model PRM's post-restore contract: HCE/link are live, while the miniport
   * has no UTRL/UTMRL run state, interrupt mask, doorbells, or stale bases. */
  Reg[0x34/4]=1; Reg[0x30/4]=0xf; Reg[0x24/4]=0;
  Reg[0x58/4]=Reg[0x78/4]=Reg[0x60/4]=Reg[0x80/4]=0;
  Reg[0x50/4]=Reg[0x54/4]=Reg[0x70/4]=Reg[0x74/4]=0;
  Reg[0x18/4]=0; Reg[0x20/4]=0;
  CHECK(D.Stage==UfsStopped && D.MiniPowerFlags==MTK_UFS_POWER_DEVICE_DOWN_ACKED &&
        D.MiniPowerAdmissionClosed && D.MiniPowerOperation==UfsPowerOpNone &&
        D.MiniPowerLastOperation==UfsPowerOpStopHost &&
        D.MiniPowerOperationGeneration==D.MiniPowerGeneration);
  /* A generic restart must not clear the fault latch, even after PRM restore. */
  { ULONG Writes=WriteCount;
    CHECK(!UfsStartTransport(&D,FALSE) && D.Transport.Faulted &&
          !D.Transport.Attached && WriteCount==Writes);
  }
  Generation=D.MiniPowerGeneration;
  InitPowerRequest(MtkUfsMiniPowerRestartHost,Generation);
  CHECK(SubmitPowerRequest() && R->Result==MtkUfsPowerInProgress &&
        D.MiniPowerPhase==UfsPowerPhaseRestartNop && D.Stage==UfsStopped &&
        D.MiniPowerAdmissionClosed && !D.Transport.Attached);
  /* Reject a foreign post-PRM layout before replacing the stopped transport
   * or touching descriptors. A refused preflight leaves the fault latch set. */
  { ULONG Writes=WriteCount;
    DpcDepth++;
    D.DmaQuarantined=TRUE;
    CHECK(!UfsStartTransport(&D,TRUE) && D.Transport.Faulted && WriteCount==Writes);
    D.DmaQuarantined=FALSE;
    Dma[0]=0xa5; Dma[MTK_UFS_DMA_BYTES-1]=0x5a;
    Reg[0x50/4]=0x50001000;
    CHECK(!UfsStartTransport(&D,TRUE) && D.Transport.Faulted &&
          !D.Transport.Attached && WriteCount==Writes &&
          Dma[0]==0xa5 && Dma[MTK_UFS_DMA_BYTES-1]==0x5a);
    Reg[0x50/4]=0; Reg[0x24/4]=MTK_UFS_INTERRUPTS;
    CHECK(!UfsStartTransport(&D,TRUE) && D.Transport.Faulted &&
          !D.Transport.Attached && WriteCount==Writes &&
          Dma[0]==0xa5 && Dma[MTK_UFS_DMA_BYTES-1]==0x5a);
    Reg[0x24/4]=0;
    DpcDepth--;
  }
  RunDpcs();
  CHECK(D.MiniPowerPhase==UfsPowerPhaseRestartNop && D.Stage==UfsNop && D.Transport.Attached &&
        D.Transport.Active && Dma[MTK_UFS_COMMAND_OFFSET]==0 &&
        Dma[MTK_UFS_COMMAND_OFFSET+2]==0);
  Finish(TRUE); CHECK(Interrupt());
  CHECK(D.MiniPowerPhase==UfsPowerPhaseRestartActive && D.Transport.Active &&
        Dma[MTK_UFS_COMMAND_OFFSET+16]==0x1b && Dma[MTK_UFS_COMMAND_OFFSET+20]==0x10);
  Finish(FALSE); CHECK(Interrupt());
  CHECK(D.MiniPowerPhase==UfsPowerPhaseRestartCapacity && D.Transport.Active &&
        Dma[MTK_UFS_COMMAND_OFFSET+16]==0x9e && Dma[MTK_UFS_COMMAND_OFFSET+2]==MTK_UFS_USER_LUN);
  Capacity(); Finish(FALSE); CHECK(Interrupt());
  CHECK(!D.MiniPowerActive && D.MiniPowerState==MtkUfsMiniPowerActive &&
        D.MiniPowerResult==MtkUfsPowerSuccess && !D.MiniPowerFlags &&
        D.Stage==UfsReady && D.Transport.Attached && !D.Transport.Active);

  /* Generation exhaustion never wraps to a token that could be replayed. */
  Reset(); Ready(); D.MiniPowerGeneration=MAXULONGLONG;
  InitPowerRequest(MtkUfsMiniPowerPrepareDevice,MAXULONGLONG);
  CHECK(SubmitPowerRequest() && PowerBuffer.Header.ReturnCode==MtkUfsPowerBusy &&
        D.MiniPowerGeneration==MAXULONGLONG && !D.MiniPowerActive);
}
static void CheckPowerPrepareFlush(void) {
  ULONG Bkops;
  for(Bkops=0;Bkops<=1;Bkops++) {
    Reset(); Ready();
    InitPowerRequest(MtkUfsMiniPowerPrepareDevice,D.MiniPowerGeneration);
    CHECK(SubmitPowerRequest() && D.MiniPowerActive);
    RunDpcs();
    CHECK(D.MiniPowerPhase==UfsPowerPhaseReadBkops && D.Transport.Active &&
          Dma[MTK_UFS_COMMAND_OFFSET]==0x16 && Dma[MTK_UFS_COMMAND_OFFSET+12]==5);
    FinishQuery(5,(UCHAR)Bkops); CHECK(Interrupt());
    if(Bkops) {
      CHECK(D.MiniPowerPhase==UfsPowerPhaseDisableBkops && D.Transport.Active &&
            Dma[MTK_UFS_COMMAND_OFFSET]==0x16 && Dma[MTK_UFS_COMMAND_OFFSET+12]==7);
      FinishQuery(7,0); CHECK(Interrupt());
      CHECK(D.MiniPowerPhase==UfsPowerPhaseVerifyBkops && D.Transport.Active &&
            Dma[MTK_UFS_COMMAND_OFFSET+12]==5);
      FinishQuery(5,0); CHECK(Interrupt());
    }
    CHECK(D.MiniPowerPhase==UfsPowerPhaseFlush && D.Transport.Active &&
          Dma[MTK_UFS_COMMAND_OFFSET+16]==0x35 && !Dma[MTK_UFS_COMMAND_OFFSET+1]);
    Finish(FALSE); CHECK(Interrupt());
    CHECK(D.MiniPowerPhase==UfsPowerPhasePowerDown && D.Transport.Active &&
          Dma[MTK_UFS_COMMAND_OFFSET+2]==MTK_UFS_DEVICE_WLUN &&
          Dma[MTK_UFS_COMMAND_OFFSET+16]==0x1b && Dma[MTK_UFS_COMMAND_OFFSET+20]==0x30);
    Finish(FALSE); CHECK(Interrupt());
    CHECK(!D.MiniPowerActive && D.MiniPowerState==MtkUfsMiniPowerDevicePrepared &&
          D.MiniPowerResult==MtkUfsPowerSuccess && D.Stage==UfsReady && D.Transport.Attached);
  }
}
static BOOLEAN DmaHasValue(UCHAR Value) {
  ULONG I;
  for(I=0;I<MTK_UFS_DMA_BYTES;I++) if(Dma[I]!=Value) return FALSE;
  return TRUE;
}
static void SaveDmaPrefix(void) {
  ULONG I;
  for(I=0;I<sizeof(DmaPrefix);I++) DmaPrefix[I]=Dma[I];
}
static BOOLEAN DmaPrefixUnchanged(void) {
  ULONG I;
  for(I=0;I<sizeof(DmaPrefix);I++) if(DmaPrefix[I]!=Dma[I]) return FALSE;
  return TRUE;
}
ULONG TestMain(void) {
  ULONG I,Before,WritesBefore;
  Reset(); CHECK(DriverEntry(NULL,NULL)==STATUS_NOT_SUPPORTED);
  CHECK(DriverEntry((PDRIVER_OBJECT)&D,(PUNICODE_STRING)&S)==0);
  CHECK(Registered.HwFindAdapter==(PVOID)UfsFind && Registered.HwStartIo==NativeStartIo);
  CHECK(Registered.HwInitialize==UfsInitialize && Registered.HwInterrupt==UfsInterrupt && Registered.HwResetBus==UfsReset);
  CHECK(Registered.AdapterInterfaceType==ACPIBus && Registered.SrbExtensionSize==16 && Registered.NumberOfAccessRanges==2);
  CHECK(Registered.SrbTypeFlags==1 && !Registered.HwBuildIo && !Registered.HwFreeAdapterResources);
  for(I=0;I<13;I++) {
    Reset();
    switch(I) {
    case 0: Config.Length--; break; case 1: Config.NumberOfAccessRanges=1; break;
    case 2: Config.AccessRanges=NULL; break; case 3: Config.InterruptMode=Latched; break;
    case 4: Config.BusInterruptVector=0; break; case 5: Ranges[0].RangeInMemory=FALSE; break;
    case 6: Ranges[0].RangeLength=0x3000; break; case 7: Ranges[1]=Ranges[0]; break;
    case 8: Ranges[1].RangeStart.QuadPart++; break;
    case 9: Ranges[1].RangeLength++; break; case 10: Ranges[0].RangeStart.QuadPart++; break;
    case 11: Ranges[1].RangeInMemory=FALSE; break; case 12: Config.NumberOfAccessRanges=3; break;
    }
    CHECK(Find()==SP_RETURN_BAD_CONFIG && !Maps && !WriteCount);
  }
  Reset(); { BOOLEAN Again=TRUE; CHECK(UfsFind(&D,NULL,NULL,"dump=1",&Config,&Again)==SP_RETURN_BAD_CONFIG && !Maps && !WriteCount); }
  for(I=0;I<4;I++) { Reset(); Handoff[I]^=3; CHECK(Find()==SP_RETURN_NOT_FOUND && Unmaps==1 && Maps==1 && !WriteCount); }
  for(I=1;I<3;I++) { Reset(); MapFail=I; CHECK(Find()==SP_RETURN_ERROR && Unmaps==I-1 && !WriteCount); }
  Reset(); DmaFail=1; CHECK(Find()==SP_RETURN_ERROR && Unmaps==2 && !WriteCount);
  for(I=1;I<4;I++) { Reset(); PhysicalBad=I; CHECK(Find()==SP_RETURN_ERROR && Unmaps==2 && !WriteCount); }
  /* Passive registration and DPC setup fail closed before any controller or DMA write. */
  Reset(); CHECK(Find()==SP_RETURN_FOUND); PassiveInitAllowed=FALSE;
  CHECK(!Initialize() && D.Stage==UfsStopped && !DpcInitCount && !WriteCount && !DpcQueued);
  Reset(); CHECK(Find()==SP_RETURN_FOUND);
  CHECK(Initialize() && DpcInitCount==1 && D.Stage==UfsNop && D.Transport.Active);
  /* Storport retries the same boot read when the host is still starting. */
  Reset(); CHECK(Find()==SP_RETURN_FOUND && UfsInitialize(&D) && D.Stage==UfsStarting);
  ReadRequest();
  CHECK(UfsStartIo(&D,&S) && D.Stage==UfsStarting && !D.Pending &&
        Completes==1 && CompletedStatus==SRB_STATUS_BUSY && !WriteCount);
  CHECK(CompletedBytes==MTK_UFS_BLOCK_BYTES && S.DataTransferLength==MTK_UFS_BLOCK_BYTES);
  CHECK(SavedPassiveInitialize(&D) && D.Stage==UfsStarting && DpcQueued);
  RunDpcs(); CHECK(D.Stage==UfsNop && D.Transport.Attached && D.Transport.Active);
  Finish(TRUE); CHECK(Interrupt() && D.Stage==UfsCapacity);
  Capacity(); Finish(FALSE); CHECK(Interrupt() && D.Stage==UfsReady);
  CHECK(UfsStartIo(&D,&S) && D.Pending==&S && D.Transport.Active && Completes==1);
  CHECK(Dma[MTK_UFS_COMMAND_OFFSET+16]==0x28 && Dma[MTK_UFS_COMMAND_OFFSET+21]==1);
  RtlFillMemory(Dma+MTK_UFS_DATA_OFFSET,MTK_UFS_BLOCK_BYTES,0x31);
  Finish(FALSE); CHECK(Interrupt() && Completes==2 && CompletedStatus==SRB_STATUS_SUCCESS &&
                       CompletedBytes==MTK_UFS_BLOCK_BYTES && Data[0]==0x31 && Data[4095]==0x31);
  /* IssueDpc(FALSE) means the callback is already queued; the owner drains it. */
  Reset(); CHECK(Find()==SP_RETURN_FOUND && UfsInitialize(&D)); DpcQueued=TRUE;
  CHECK(SavedPassiveInitialize(&D) && D.DpcScheduled && D.WorkFlags==UFS_DPC_WORK_INITIALIZE);
  RunDpcs(); CHECK(D.Stage==UfsNop && D.Transport.Active && DpcDispatchCount==1 && !DpcQueued);
  /* A valid handoff is only a hint: powered-off HCI is rejected before DMA/MMIO writes. */
  Reset(); CHECK(Find()==SP_RETURN_FOUND && !WriteCount);
  Reg[0x34/4]=0; RtlFillMemory(Dma,MTK_UFS_DMA_BYTES,0x5a); Before=WriteCount;
  CHECK(!Initialize() && D.Stage==UfsStopped && !D.Transport.Attached && !D.Transport.Active);
  CHECK(Handoff[0]==1 && Handoff[1]==(ULONG)UFS_MMIO && Handoff[2]==2 && Handoff[3]==1);
  CHECK(WriteCount==Before && DmaHasValue(0x5a) && !D.Restartable);
  Reset(); Reg[0x50/4]=0x1234; CHECK(Find()==SP_RETURN_FOUND && !WriteCount);
  CHECK(!Initialize() && !WriteCount && !D.Transport.Attached);
  Reset(); Ready(); CHECK(SyncCalls>=2 && PassiveInitCount==1 && DpcInitCount==1 &&
                             DpcDispatchCount>=4 && DpcIssueCount>=4);
  /* Work arriving while a DPC owns the device is coalesced and reissued at its tail. */
  Before=DpcIssueCount; D.DpcRunning=TRUE;
  QueueWork(UFS_DPC_WORK_TIMER);
  CHECK(!DpcQueued && !D.DpcScheduled && D.WorkFlags==UFS_DPC_WORK_TIMER &&
        DpcIssueCount==Before);
  UfsFinishDpc(&D);
  CHECK(DpcQueued && D.DpcScheduled && DpcIssueCount==Before+1);
  RunDpcs(); CHECK(!DpcQueued && !D.DpcScheduled && !D.WorkFlags && !D.DpcRunning);
  Reset(); Ready();
  ReadRequest(); Before=WriteCount;
  CHECK(NativeStartIo(&D,&S) && D.Pending==&S && !D.Transport.Active && DpcQueued &&
        !Completes && S.SrbStatus==SRB_STATUS_PENDING && WriteCount==Before);
  RunDpcs(); CHECK(D.Transport.Active && Dma[MTK_UFS_COMMAND_OFFSET+16]==0x28);
  Second=S; Second.Cdb[5]=2;
  CHECK(UfsStartIo(&D,&Second) && Completes==1 && CompletedStatus==SRB_STATUS_BUSY && D.Pending==&S);
  CHECK(CompletedBytes==MTK_UFS_BLOCK_BYTES && Second.DataTransferLength==MTK_UFS_BLOCK_BYTES);
  for(I=0;I<4096;I++) Dma[MTK_UFS_DATA_OFFSET+I]=(UCHAR)I;
  Finish(FALSE); Before=Completes;
  CHECK(InterruptOnly() && D.Transport.Active && D.Pending==&S && DpcQueued && Completes==Before && Data[0]==0x7a);
  RunDpcs(); CHECK(Completes==2 && CompletedStatus==SRB_STATUS_SUCCESS && CompletedBytes==4096 && !D.Pending);
  for(I=0;I<4096;I++) CHECK(Data[I]==(UCHAR)I);
  CHECK(Data[4096]==0x7a && Dma[MTK_UFS_DMA_BYTES]==0xa5);
  CHECK(!Interrupt()); UfsTimer(&D); CHECK(Completes==2 && !D.TimerArmed);
  CHECK(UfsStartIo(&D,&Second) && D.Pending==&Second && D.Transport.Active &&
        Dma[MTK_UFS_COMMAND_OFFSET+21]==2);
  RtlFillMemory(Dma+MTK_UFS_DATA_OFFSET,MTK_UFS_BLOCK_BYTES,0x32);
  Finish(FALSE); CHECK(Interrupt() && Completes==3 && CompletedStatus==SRB_STATUS_SUCCESS &&
                       CompletedBytes==MTK_UFS_BLOCK_BYTES && Data[0]==0x32 && Data[4095]==0x32);
  /* Wall-clock jumps do not expire or extend a request; only QPC elapsed time does. */
  Reset(); Ready(); ReadRequest(); CHECK(UfsStartIo(&D,&S)); Before=Completes;
  Clock=0; UfsTimer(&D);
  CHECK(D.Pending==&S && !D.StopRequested && Completes==Before);
  Clock=MAXULONGLONG; UfsTimer(&D);
  CHECK(D.Pending==&S && !D.StopRequested && Completes==Before && WallTimeCalls==0);
  PerfClock+=10000000; UfsTimer(&D);
  CHECK(D.StopRequested && D.Pending==&S && Completes==Before);
  UfsTimer(&D); CHECK(CompletedStatus==SRB_STATUS_TIMEOUT && !D.Pending && !D.DmaQuarantined);
  /* A broken performance-counter provider fails closed instead of hanging an I/O. */
  Reset(); Ready(); ReadRequest(); CHECK(UfsStartIo(&D,&S)); PerfQueryStatus=STOR_STATUS_UNSUCCESSFUL;
  UfsTimer(&D); CHECK(D.StopRequested && D.Pending==&S);
  UfsTimer(&D); CHECK(CompletedStatus==SRB_STATUS_TIMEOUT && !D.Pending);
  /* Timer callbacks only mark work; timeout polling and Stop start in the DPC. */
  Reset(); Ready();
  ReadRequest(); CHECK(UfsStartIo(&D,&S)); PerfClock+=10000000;
  Before=Completes; TickOnly();
  CHECK(D.Pending==&S && !D.StopRequested && DpcQueued && Completes==Before);
  RunDpcs(); CHECK(D.StopRequested && D.Pending==&S && Completes==Before);
  UfsTimer(&D); CHECK(CompletedStatus==SRB_STATUS_TIMEOUT && !D.Pending && !D.DmaQuarantined);
  /* A fatal interrupt injected halfway through the DPC copy cannot complete the read successfully. */
  Reset(); Ready(); ReadRequest(); CHECK(UfsStartIo(&D,&S));
  for(I=0;I<4096;I++) Dma[MTK_UFS_DATA_OFFSET+I]=(UCHAR)I;
  Finish(FALSE); Before=Completes; InjectFatalDuringCopy=TRUE;
  CHECK(InterruptOnly() && DpcQueued && Completes==Before);
  RunDpcs(); CHECK(!InjectFatalDuringCopy && Completes==Before+1 &&
                    CompletedStatus==SRB_STATUS_ERROR && !CompletedBytes && !D.Pending &&
                    D.Stage==UfsStopped && !D.DmaQuarantined && !D.Transport.Attached);
  /* StopAdapter may preempt the copy; the DPC retains SRB ownership and drains the stop. */
  Reset(); Ready(); ReadRequest(); CHECK(UfsStartIo(&D,&S));
  for(I=0;I<4096;I++) Dma[MTK_UFS_DATA_OFFSET+I]=(UCHAR)I;
  Finish(FALSE); Before=Completes; InjectStopDuringCopy=TRUE;
  CHECK(InterruptOnly()); RunDpcs();
  CHECK(!InjectStopDuringCopy && StopDuringCopyStatus==ScsiAdapterControlUnsuccessful &&
        D.Pending==&S && D.StopRequested && Completes==Before && TimerScheduled);
  UfsTimer(&D);
  CHECK(
        Completes==Before+1 && CompletedStatus==SRB_STATUS_ABORTED && !CompletedBytes &&
        !D.Pending && D.Stage==UfsStopped && !D.DmaQuarantined && !D.Transport.Attached);
  /* The completion claim rechecks a fatal ISR that arrives immediately before notification. */
  Reset(); Ready(); ReadRequest(); CHECK(UfsStartIo(&D,&S)); Finish(FALSE);
  Before=Completes; InjectFatalAcquireAt=AcquireCount+2;
  CHECK(InterruptOnly()); RunDpcs();
  CHECK(!InjectFatalAcquireAt && Completes==Before+1 && CompletedStatus==SRB_STATUS_ERROR &&
        !D.Pending && D.Stage==UfsStopped && !D.DmaQuarantined);
  /* Reentrant StartIo from completion is queued for a second DPC after owner state is released. */
  Reset(); Ready(); ReadRequest(); CHECK(UfsStartIo(&D,&S)); Second=S; Finish(FALSE);
  Before=Completes; ReenterOnComplete=TRUE;
  CHECK(Interrupt() && !ReenterOnComplete && ReentrantStartResult && D.Pending==&Second &&
        D.Transport.Active && Completes==Before+1 && DpcDispatchCount>=5);
  Finish(FALSE); CHECK(Interrupt() && Completes==Before+2 && !D.Pending);
  /* Actual WRITE(10) takes a bounded data snapshot; caller memory is never DMA. */
  ReadRequest(); S.SrbFlags=SRB_FLAGS_DATA_OUT; S.Cdb[0]=0x2a;
  CHECK(UfsStartIo(&D,&S) && D.Pending==&S && D.PendingWrite && S.SrbStatus==SRB_STATUS_PENDING);
  CHECK(Dma[MTK_UFS_COMMAND_OFFSET+1]==0x20 && Dma[3]==0x13);
  for(I=0;I<4096;I++) CHECK(Dma[MTK_UFS_DATA_OFFSET+I]==Data[I]);
  Dma[MTK_UFS_DATA_OFFSET]=0xa9; Finish(FALSE); CHECK(Interrupt() && CompletedStatus==SRB_STATUS_SUCCESS && CompletedBytes==4096);
  CHECK(Data[0]==0 && !D.Pending);
  Before=WriteCount; ReadRequest(); S.SrbFlags=SRB_FLAGS_DATA_OUT; S.Cdb[0]=0x42;
  CHECK(UfsStartIo(&D,&S) && !D.Pending && (CompletedStatus&SRB_STATUS_AUTOSENSE_VALID));
  CHECK(Sense[2]==5 && Sense[12]==0x24 && !CompletedBytes && WriteCount==Before);
  ReadRequest(); S.Lun=1; CHECK(UfsStartIo(&D,&S) && CompletedStatus==SRB_STATUS_NO_DEVICE && WriteCount==Before);
  ReadRequest(); S.DataBuffer=NULL; CHECK(UfsStartIo(&D,&S) && !D.Pending && WriteCount==Before);
  ReadRequest(); S.Cdb[0]=0xff; CHECK(UfsStartIo(&D,&S) && Sense[2]==5 && Sense[12]==0x20 && WriteCount==Before);
  /* A short transfer reports exact bytes; untouched tail is not copied. */
  ReadRequest(); CHECK(UfsStartIo(&D,&S)); RtlFillMemory(Dma+MTK_UFS_DATA_OFFSET,4096,0x45);
  Finish(FALSE); Dma[MTK_UFS_RESPONSE_OFFSET+1]=0x20; Dma[MTK_UFS_RESPONSE_OFFSET+14]=8;
  CHECK(Interrupt() && CompletedStatus==SRB_STATUS_DATA_OVERRUN && CompletedBytes==2048 && Data[2048]!=(UCHAR)0x45);
  /* DEVICE CHECK CONDITION returns sense, zero bytes, no stale bounce copy. */
  ReadRequest(); CHECK(UfsStartIo(&D,&S)); Finish(FALSE); Dma[MTK_UFS_RESPONSE_OFFSET+7]=2;
  Dma[MTK_UFS_RESPONSE_OFFSET+11]=20; Dma[MTK_UFS_RESPONSE_OFFSET+33]=18;
  Dma[MTK_UFS_RESPONSE_OFFSET+34]=0x70; Dma[MTK_UFS_RESPONSE_OFFSET+36]=3;
  CHECK(Interrupt() && !CompletedBytes && Sense[2]==3 && S.ScsiStatus==2 && (S.SrbStatus&SRB_STATUS_AUTOSENSE_VALID));
  /* MODE SENSE preserves the actual device write-protect flag. */
  ReadRequest(); S.CdbLength=6; RtlZeroMemory(S.Cdb,16); S.Cdb[0]=0x1a; S.Cdb[4]=8; S.DataTransferLength=8;
  CHECK(UfsStartIo(&D,&S)); Dma[MTK_UFS_DATA_OFFSET+2]=0x80; Finish(FALSE); CHECK(Interrupt() && Data[2]==0x80 && CompletedBytes==8);
  ReadRequest(); S.CdbLength=12; RtlZeroMemory(S.Cdb,16); S.Cdb[0]=0xa0; S.Cdb[9]=16; S.DataTransferLength=16;
  Before=WriteCount; CHECK(UfsStartIo(&D,&S) && CompletedBytes==16 && Data[3]==8 && Data[9]==0 && WriteCount==Before);
  /* FLUSH becomes a real non-IMMED SYNCHRONIZE CACHE; completion waits for reply. */
  ReadRequest(); S.Function=SRB_FUNCTION_FLUSH; S.DataTransferLength=0; S.SrbFlags=0;
  Before=Completes; CHECK(UfsStartIo(&D,&S) && D.Pending==&S && Completes==Before);
  CHECK(Dma[MTK_UFS_COMMAND_OFFSET+16]==0x35 && !Dma[MTK_UFS_COMMAND_OFFSET+1]);
  Finish(FALSE); CHECK(Interrupt() && CompletedStatus==SRB_STATUS_SUCCESS && !CompletedBytes &&
                       D.Stage==UfsReady && Reg[0x34/4]==1 && !D.PendingShutdown && Reg[0x90/4]==0);
  /* Idle stop/restart preserves link, clears all old DMA bases, then NOP/capacity revalidates. */
  CHECK(Control(ScsiStopAdapter,NULL)==ScsiAdapterControlSuccess && D.Restartable && Reg[0x34/4]==1 && !Reg[0x50/4]);
  CHECK(!D.TimerArmed && Control(ScsiStopAdapter,NULL)==ScsiAdapterControlSuccess && D.Restartable);
  CHECK(Control(ScsiRestartAdapter,NULL)==ScsiAdapterControlSuccess && D.Stage==UfsNop);
  Finish(TRUE); CHECK(Interrupt()); Capacity(); Finish(FALSE); CHECK(Interrupt() && D.Stage==UfsReady);
  /* Stop/restart is allowed only while every inherited HCI gate remains live. */
  for(I=0;I<13;I++) {
    Reset(); Ready();
    CHECK(Control(ScsiStopAdapter,NULL)==ScsiAdapterControlSuccess && D.Restartable);
    RtlFillMemory(Dma,MTK_UFS_DMA_BYTES,0x5a);
    switch(I) {
    case 0: Reg[0x34/4]=0; break;                 /* HCE lost */
    case 1: Reg[0x30/4]=7; break;                 /* HCS not ready */
    case 2: Reg[0]=MAXULONG; break;                /* powered-off/all-ones read */
    case 3: Reg[0x58/4]=1; break;                 /* UTRL doorbell */
    case 4: Reg[0x78/4]=1; break;                 /* UTMRL doorbell */
    case 5: Reg[0x60/4]=1; break;                 /* UTRL run/stop */
    case 6: Reg[0x80/4]=1; break;                 /* UTMRL run/stop */
    case 7: Reg[0x50/4]=1; break;                 /* UTRL base low */
    case 8: Reg[0x54/4]=1; break;                 /* UTRL base high */
    case 9: Reg[0x70/4]=1; break;                 /* UTMRL base low */
    case 10: Reg[0x74/4]=1; break;                /* UTMRL base high */
    case 11: Reg[0x20/4]=MTK_UFS_ERROR_INTERRUPTS; break; /* IS error */
    default: Reg[0x24/4]=1; break;                /* IE still enabled */
    }
    Before=WriteCount;
    CHECK(Control(ScsiRestartAdapter,NULL)==ScsiAdapterControlSuccess);
    CHECK(WriteCount==Before && DmaHasValue(0x5a));
    CHECK(D.Stage==UfsStopped && !D.Restartable && !D.Transport.Attached &&
          !D.Transport.Active && !D.DmaQuarantined);
    CHECK(Control(ScsiRestartAdapter,NULL)==ScsiAdapterControlUnsuccessful &&
          WriteCount==Before && DmaHasValue(0x5a));
  }
  /* Active Stop aborts once, stops DMA, and leaves the caller's read buffer intact. */
  Reset(); Ready(); ReadRequest(); CHECK(UfsStartIo(&D,&S) && D.Pending==&S);
  Before=Completes; SaveDmaPrefix();
  CHECK(ControlOnly(ScsiStopAdapter,NULL)==ScsiAdapterControlUnsuccessful);
  CHECK(Completes==Before && D.Pending==&S && D.StopRequested && D.DmaQuarantined &&
        Reg[0x34/4] && DpcQueued);
  RunDpcs(); CHECK(!Reg[0x34/4] && D.Pending==&S && Completes==Before);
  UfsTimer(&D);
  CHECK(Completes==Before+1 && CompletedStatus==SRB_STATUS_ABORTED && !CompletedBytes);
  CHECK(!D.Pending && D.Stage==UfsStopped && !D.Transport.Attached && !D.Transport.Active &&
        !D.DmaQuarantined && !D.Restartable && DmaPrefixUnchanged() && !D.TimerArmed &&
        !TimerScheduled && TimerCancelCount>=1);
  Before=Completes; TickOnly(); CHECK(!DpcQueued && Completes==Before);
  for(I=0;I<4096;I++) CHECK(Data[I]==0x7a);
  CHECK(Control(ScsiRestartAdapter,NULL)==ScsiAdapterControlUnsuccessful);
  Second=S; WritesBefore=WriteCount; Before=Completes;
  CHECK(UfsStartIo(&D,&Second) && CompletedStatus==SRB_STATUS_NO_DEVICE &&
        Completes==Before+1 && WriteCount==WritesBefore && DmaPrefixUnchanged());
  for(I=0;I<4096;I++) CHECK(Data[I]==0x7a);
  /* An un-stoppable active HCI quarantines DMA and cannot be restarted or reused. */
  Reset(); Ready(); ReadRequest(); CHECK(UfsStartIo(&D,&S) && D.Pending==&S);
  SaveDmaPrefix(); HceStuck=TRUE; Before=Completes;
  CHECK(Control(ScsiStopAdapter,NULL)==ScsiAdapterControlUnsuccessful);
  CHECK(Completes==Before && D.Pending==&S && D.StopAttempts==1);
  for(I=0;I<99;I++) UfsTimer(&D);
  CHECK(Completes==Before+1 && CompletedStatus==SRB_STATUS_ERROR && !CompletedBytes);
  CHECK(D.DmaQuarantined && D.Transport.Attached && D.Transport.Active && D.Transport.Faulted &&
        D.Stage==UfsStopped && !D.Restartable && D.StopAttempts==100 && !Stalls &&
        D.LastHce==1 && DmaPrefixUnchanged());
  for(I=0;I<4096;I++) CHECK(Data[I]==0x7a);
  WritesBefore=WriteCount;
  CHECK(Control(ScsiRestartAdapter,NULL)==ScsiAdapterControlUnsuccessful && WriteCount==WritesBefore);
  Second=S; Before=Completes;
  CHECK(UfsStartIo(&D,&Second) && CompletedStatus==SRB_STATUS_NO_DEVICE &&
        Completes==Before+1 && WriteCount==WritesBefore && DmaPrefixUnchanged());
  Reset(); Ready();
  /* Time-out stops DMA before completing once; later ISR/timer cannot reuse it. */
  ReadRequest(); CHECK(UfsStartIo(&D,&S)); PerfClock+=10000000; UfsTimer(&D);
  CHECK(D.StopRequested && D.Pending==&S && !Completes);
  UfsTimer(&D);
  CHECK(D.Stage==UfsStopped && CompletedStatus==SRB_STATUS_TIMEOUT && !D.Pending && !Reg[0x34/4]);
  Before=Completes; UfsTimer(&D); CHECK(!Interrupt() && Completes==Before && !Initialize());
  Reset(); Ready(); ReadRequest(); CHECK(UfsStartIo(&D,&S)); HceStuck=TRUE; PerfClock+=10000000; UfsTimer(&D);
  CHECK(D.DmaQuarantined && D.Transport.Attached && D.StopAttempts==1 && !Completes);
  for(I=0;I<99;I++) UfsTimer(&D);
  CHECK(D.StopPermanent && D.StopAttempts==100 && CompletedStatus==SRB_STATUS_ERROR);
  Before=WriteCount; ReadRequest(); CHECK(UfsStartIo(&D,&S) && CompletedStatus==SRB_STATUS_NO_DEVICE && WriteCount==Before);
  CHECK(Control(ScsiRestartAdapter,NULL)==ScsiAdapterControlUnsuccessful);
  /* A stopped controller acknowledges only owned pending status and does not queue service. */
  DpcDispatchCount=Before=0; Reg[0x20/4]=1;
  CHECK(InterruptOnly() && !DpcQueued && DpcDispatchCount==Before);
  CHECK(!InterruptOnly() && !DpcQueued);
  /* All-ones ISR status is not claimed; the DPC later detects inaccessible owned MMIO and stops. */
  Reset(); Ready(); Reg[0x20/4]=MAXULONG;
  CHECK(!InterruptOnly() && !DpcQueued && !D.StopRequested);
  QueueWork(UFS_DPC_WORK_TIMER); CHECK(DpcQueued && !D.StopRequested); RunDpcs();
  CHECK(D.Stage==UfsStopped && D.StopRequested && D.DmaQuarantined && D.Transport.StopInProgress);
  UfsTimer(&D);
  CHECK(!D.StopRequested && !D.DmaQuarantined &&
        !Reg[0x34/4] && !Reg[0x24/4] && LinkDownCount==1);
  /* Failure to mask IE is bounded and quarantines DMA instead of leaving a hot level source. */
  Reset(); Ready(); ReadRequest(); CHECK(UfsStartIo(&D,&S)); SaveDmaPrefix(); IeStuck=TRUE; Reg[0x20/4]=1;
  Before=Completes; CHECK(InterruptOnly() && DpcQueued); RunDpcs();
  CHECK(D.StopRequested && D.DmaQuarantined && D.Pending==&S && !Completes);
  for(I=0;I<100 && !D.StopPermanent;I++) UfsTimer(&D);
  CHECK(D.StopPermanent && D.StopAttempts==UFS_STOP_RETRY_MAX && D.DmaQuarantined &&
        D.Transport.Attached && D.Pending==NULL && Completes==Before+1 &&
        CompletedStatus==SRB_STATUS_ERROR && Reg[0x24/4]==MTK_UFS_INTERRUPTS &&
        D.LastIe==MTK_UFS_INTERRUPTS && DmaPrefixUnchanged());
  Before=WriteCount; CHECK(Control(ScsiRestartAdapter,NULL)==ScsiAdapterControlUnsuccessful &&
                            WriteCount==Before);
  Reset(); Ready(); Reg[0x20/4]=1<<16; CHECK(Interrupt() && D.Stage==UfsStopped && !Reg[0x34/4] && D.StopRequested);
  UfsTimer(&D); CHECK(!D.StopRequested && !D.DmaQuarantined);
  Reset(); Ready(); ReadRequest(); CHECK(UfsStartIo(&D,&S));
  CHECK(!UfsReset(&D,0)); RunDpcs(); CHECK(D.StopRequested && !Completes);
  UfsTimer(&D); CHECK(CompletedStatus==SRB_STATUS_BUS_RESET && !D.Pending);
  /* Capacity mismatch must never expose the disk. */
  Reset(); CHECK(Find()==SP_RETURN_FOUND && Initialize()); Finish(TRUE); CHECK(Interrupt());
  Capacity(); Dma[MTK_UFS_DATA_OFFSET+7]^=1; Finish(FALSE); CHECK(Interrupt() && D.Stage==UfsStopped);
  /* A bounded UNIT ATTENTION retry is issued as another read-only command. */
  Reset(); CHECK(Find()==SP_RETURN_FOUND && Initialize()); Finish(TRUE); CHECK(Interrupt());
  Finish(FALSE); Dma[MTK_UFS_RESPONSE_OFFSET+7]=2; Dma[MTK_UFS_RESPONSE_OFFSET+11]=20;
  Dma[MTK_UFS_RESPONSE_OFFSET+33]=18; Dma[MTK_UFS_RESPONSE_OFFSET+34]=0x70; Dma[MTK_UFS_RESPONSE_OFFSET+36]=6;
  CHECK(Interrupt() && D.Stage==UfsCapacity && D.CapacityAttempts==2 && D.Transport.Active);
  Capacity(); Finish(FALSE); CHECK(Interrupt() && D.Stage==UfsReady);
  /* SHUTDOWN flushes LU2, powers down only the device WLUN, enters Hibern8,
   * validates the T70 link probe, then stops HCE before completing once. */
  Reset(); Ready(); Before=Completes; ShutdownToHibernate8();
  Reg[0x20/4]|=1U<<10; Reg[0x98/4]=0; Reg[0x22c8/4]=0x20000000;
  UfsTimer(&D); CHECK(D.Pending==&S && D.ShutdownPhase==UfsShutdownHibernate8 &&
                      Completes==Before && Reg[0x34/4]==1);
  Reg[0x22c8/4]=0x30000000; UfsTimer(&D);
  CHECK(Completes==Before && D.Pending==&S && D.StopRequested && !Reg[0x34/4]);
  UfsTimer(&D);
  CHECK(Completes==Before+1 && CompletedStatus==SRB_STATUS_SUCCESS && !CompletedBytes &&
        !D.Pending && !D.PendingShutdown && D.Stage==UfsStopped && !Reg[0x34/4] &&
        !Reg[0x24/4] && !Reg[0x58/4] && !Reg[0x78/4] &&
        !Reg[0x50/4] && !Reg[0x54/4] && !Reg[0x70/4] && !Reg[0x74/4] && !D.DmaQuarantined);
  Before=Completes; CHECK(!Interrupt()); UfsTimer(&D); CHECK(Completes==Before);
  /* A failed cache flush or device-WLUN powerdown stops the controller and
   * returns device sense exactly once; it never begins Hibern8. */
  for(I=0;I<2;I++) {
    Reset(); Ready(); Before=Completes;
    ReadRequest(); S.Function=SRB_FUNCTION_SHUTDOWN; S.DataTransferLength=0; S.SrbFlags=0;
    CHECK(UfsStartIo(&D,&S) && D.Pending==&S);
    if(I==1) { Finish(FALSE); CHECK(Interrupt() && D.Transport.ActiveLun==MTK_UFS_DEVICE_WLUN); }
    Finish(FALSE); Dma[MTK_UFS_RESPONSE_OFFSET+7]=2;
    Dma[MTK_UFS_RESPONSE_OFFSET+11]=20; Dma[MTK_UFS_RESPONSE_OFFSET+33]=18;
    Dma[MTK_UFS_RESPONSE_OFFSET+34]=0x70; Dma[MTK_UFS_RESPONSE_OFFSET+36]=3;
    CHECK(Interrupt() && Completes==Before && D.Pending==&S && D.StopRequested &&
          (S.SrbStatus==SRB_STATUS_PENDING));
    UfsTimer(&D);
    CHECK(Completes==Before+1 && CompletedStatus==(SRB_STATUS_ERROR|SRB_STATUS_AUTOSENSE_VALID) &&
          !D.Pending && D.Stage==UfsStopped && !D.DmaQuarantined && !Reg[0x34/4] &&
          !D.Transport.UicPending && !Reg[0x90/4] && Sense[2]==3);
    Before=Completes; UfsTimer(&D); CHECK(Completes==Before);
  }
  /* A stuck HCE after a SHUTDOWN sense result keeps autosense and SCSI status. */
  Reset(); Ready(); Before=Completes; ReadRequest(); S.Function=SRB_FUNCTION_SHUTDOWN;
  S.DataTransferLength=0; S.SrbFlags=0; HceStuck=TRUE;
  CHECK(UfsStartIo(&D,&S) && D.Pending==&S);
  Finish(FALSE); Dma[MTK_UFS_RESPONSE_OFFSET+7]=2; Dma[MTK_UFS_RESPONSE_OFFSET+11]=20;
  Dma[MTK_UFS_RESPONSE_OFFSET+33]=18; Dma[MTK_UFS_RESPONSE_OFFSET+34]=0x70; Dma[MTK_UFS_RESPONSE_OFFSET+36]=3;
  CHECK(Interrupt() && D.Pending==&S && D.StopRequested && !Completes);
  for(I=0;I<100;I++) UfsTimer(&D);
  CHECK(Completes==Before+1 && CompletedStatus==(SRB_STATUS_ERROR|SRB_STATUS_AUTOSENSE_VALID) &&
        S.ScsiStatus==2 && Sense[2]==3 && !D.Pending && D.StopPermanent && D.DmaQuarantined);
  /* Invalid UIC completion and a link probe that never reaches Hibern8 both
   * fail the shutdown without exposing a false successful power transition. */
  Reset(); Ready(); Before=Completes; ShutdownToHibernate8();
  Reg[0x20/4]|=1U<<10; Reg[0x98/4]=1; UfsTimer(&D);
  CHECK(Completes==Before && D.StopRequested);
  UfsTimer(&D);
  CHECK(Completes==Before+1 && CompletedStatus==SRB_STATUS_ERROR && D.Stage==UfsStopped && !D.DmaQuarantined);
  Reset(); Ready(); Before=Completes; ShutdownToHibernate8();
  Reg[0x20/4]|=1U<<10; Reg[0x98/4]=0; Reg[0x22c8/4]=0;
  UfsTimer(&D); CHECK(Completes==Before);
  PerfClock+=UFS_HIBERN8_TIMEOUT_100NS; UfsTimer(&D);
  CHECK(Completes==Before && D.StopRequested);
  UfsTimer(&D);
  CHECK(Completes==Before+1 && CompletedStatus==SRB_STATUS_TIMEOUT && D.Stage==UfsStopped && !D.DmaQuarantined);
  /* If HCE will not stop after the link transition, report failure and keep
   * the attached DMA transport quarantined for the adapter lifetime. */
  Reset(); Ready(); Before=Completes; ShutdownToHibernate8();
  Reg[0x20/4]|=1U<<10; Reg[0x98/4]=0; Reg[0x22c8/4]=0x30000000; HceStuck=TRUE;
  UfsTimer(&D);
  CHECK(Completes==Before && D.StopRequested && D.StopAttempts==1);
  for(I=0;I<99;I++) UfsTimer(&D);
  CHECK(Completes==Before+1 && CompletedStatus==SRB_STATUS_ERROR && D.DmaQuarantined &&
        D.Transport.Attached && D.Transport.Faulted && D.Stage==UfsStopped && D.StopAttempts==100 && !Stalls);
  Before=Completes; UfsTimer(&D); CHECK(Completes==Before && !Initialize());
  /* A device write error does not copy DMA data into the write source. */
  Reset(); Ready(); ReadRequest(); S.Cdb[0]=0x2a; S.SrbFlags=SRB_FLAGS_DATA_OUT;
  CHECK(UfsStartIo(&D,&S)); Finish(FALSE); Dma[MTK_UFS_RESPONSE_OFFSET+7]=2;
  CHECK(Interrupt() && CompletedStatus==SRB_STATUS_ERROR && !CompletedBytes && Data[0]==0x7a);
  CheckPowerEndpoint(); CheckPowerPrepareFlush();
  return Failure;
}
