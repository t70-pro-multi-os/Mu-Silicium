/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <ntddk.h>
#include <ntddscsi.h>
#include <storport.h>
#include <Library/MtkUfsTransport.h>
#include <Library/MtkUfsCore.h>
#include <Library/MtkUfsPower.h>
/* Initial-D0, legacy UFSHCI transport. No PCI/SMC guesses, clock/power/reset,
 * RPMB, boot LUs, firmware updates, OS hibernation or crash-dump resume support.
 * LU2 is presented as the sole Windows disk (BTL 0:0:0). No installation is authorized. */
#define UFS_MMIO 0x11270000ULL
#define UFS_MMIO_BYTES 0x2300U
#define UFS_HANDOFF 0x43f7f0b0ULL
#define UFS_TICK_US 10000U
#define UFS_INIT_TIMEOUT_100NS 10000000ULL
#define UFS_MAX_TIMEOUT_100NS 300000000ULL
#define UFS_HIBERN8_TIMEOUT_100NS 150000ULL
#define UFS_HIBERN8_POLL_US 1000U
#define UFS_STOP_RETRY_US 1000U
#define UFS_STOP_RETRY_MAX 100U
#define UFS_DPC_WORK_START 0x00000001U
#define UFS_DPC_WORK_INTERRUPT 0x00000002U
#define UFS_DPC_WORK_TIMER 0x00000004U
#define UFS_DPC_WORK_STOP 0x00000008U
#define UFS_DPC_WORK_RESTART 0x00000010U
#define UFS_DPC_WORK_INITIALIZE 0x00000020U
#define UFS_DPC_WORK_POWER 0x00000040U
#define UFS_POWER_TIMEOUT_100NS 50000000ULL
typedef struct {
  SRB_IO_CONTROL Header;
  UCHAR Request[MTK_UFS_POWER_REQUEST_BYTES];
} UFS_POWER_WIRE_BUFFER;
C_ASSERT(sizeof(SRB_IO_CONTROL)==28);
C_ASSERT(sizeof(UFS_POWER_WIRE_BUFFER)==92);
C_ASSERT(FIELD_OFFSET(UFS_POWER_WIRE_BUFFER,Request)==28);

typedef enum {
  UfsPowerOpNone,
  UfsPowerOpPrepareDevice,
  UfsPowerOpStopHost,
  UfsPowerOpRestartHost
} UFS_POWER_OP;
typedef enum {
  UfsPowerPhaseIdle,
  UfsPowerPhaseWaitDrain,
  UfsPowerPhaseReadBkops,
  UfsPowerPhaseDisableBkops,
  UfsPowerPhaseVerifyBkops,
  UfsPowerPhaseFlush,
  UfsPowerPhasePowerDown,
  UfsPowerPhaseStopHost,
  UfsPowerPhaseStopPoll,
  UfsPowerPhaseRestartNop,
  UfsPowerPhaseRestartActive,
  UfsPowerPhaseRestartLiveActive,
  UfsPowerPhaseRestartCapacity
} UFS_POWER_PHASE;

typedef enum { UfsStopped, UfsStarting, UfsNop, UfsCapacity, UfsReady } UFS_STAGE;
typedef enum { UfsShutdownNone, UfsShutdownCache, UfsShutdownPowerDown, UfsShutdownHibernate8 } UFS_SHUTDOWN_PHASE;
typedef enum { UfsStopNone, UfsStopDisableHce, UfsStopDetachLists } UFS_STOP_MODE;
typedef struct {
  STOR_DPC Dpc;
  PUCHAR Registers;
  PVOID Allocation;
  ULONGLONG Physical;
  volatile UCHAR *Dma;
  MTK_UFS_TRANSPORT Transport;
  PSCSI_REQUEST_BLOCK Pending;
  ULONGLONG Started, Timeout, PerformanceFrequency;
  UFS_STAGE Stage;
  BOOLEAN TimerArmed, DmaQuarantined, InitializationAttempted, Restartable;
  UCHAR CapacityAttempts;
  BOOLEAN PendingWrite, PendingShutdown;
  UFS_SHUTDOWN_PHASE ShutdownPhase;
  BOOLEAN DpcInitialized, DpcScheduled, DpcRunning, TimerExpired;
  BOOLEAN StopRequested, StopRetryPending, StopPermanent, StopLinkDownSent;
  BOOLEAN IrqMaskUnverified;
  BOOLEAN StopRestartable, StopCompletePending;
  BOOLEAN RestartPending;
  UFS_STOP_MODE StopMode;
  UCHAR StopStatus;
  ULONG WorkFlags, LatchedInterrupts, StopAttempts;
  ULONG LastIs, LastIe, LastHce, LastRun, LastDoor;
  ULONG LastUtrlBaseLow, LastUtrlBaseHigh, LastUtmrlBaseLow, LastUtmrlBaseHigh;
  BOOLEAN PendingCommand;
  UCHAR PendingCdb[16];
  ULONG PendingCdbLength, PendingBytes;
  ULONGLONG MiniPowerGeneration;
  ULONGLONG MiniPowerOperationGeneration;
  ULONG MiniPowerState, MiniPowerResult;
  ULONG MiniPowerCapabilities, MiniPowerFlags;
  UFS_POWER_OP MiniPowerOperation, MiniPowerLastOperation;
  UFS_POWER_PHASE MiniPowerPhase;
  BOOLEAN MiniPowerActive, MiniPowerAdmissionClosed;
  ULONGLONG MiniPowerStarted;
  ULONG MiniPowerPolls;
} UFS_DEVICE;
HW_FIND_ADAPTER UfsFind;
HW_INITIALIZE UfsInitialize;
HW_STARTIO UfsStartIo;
HW_INTERRUPT UfsInterrupt;
HW_TIMER UfsTimer;
HW_RESET_BUS UfsReset;
HW_ADAPTER_CONTROL UfsControl;
DRIVER_INITIALIZE DriverEntry;
HW_PASSIVE_INITIALIZE_ROUTINE UfsPassiveInitialize;
HW_DPC_ROUTINE UfsDpc;

static uint32_t UfsRead(void *Context,uint32_t Offset) {
  UFS_DEVICE *D=Context;
  return StorPortReadRegisterUlong(D,(PULONG)(D->Registers+Offset));
}
static void UfsWrite(void *Context,uint32_t Offset,uint32_t Value) {
  UFS_DEVICE *D=Context;
  StorPortWriteRegisterUlong(D,(PULONG)(D->Registers+Offset),Value);
}
static void UfsFence(void *Context) { UNREFERENCED_PARAMETER(Context); KeMemoryBarrier(); }
static void UfsStall(void *Context,uint32_t Us) { UNREFERENCED_PARAMETER(Context); StorPortStallExecution(Us); }
static ULONGLONG UfsNow(UFS_DEVICE *D) {
  LARGE_INTEGER Counter;
  if(!D || !D->PerformanceFrequency ||
     StorPortQueryPerformanceCounter(D,NULL,&Counter)!=STOR_STATUS_SUCCESS || Counter.QuadPart<0)
    return MAXULONGLONG;
  return (ULONGLONG)Counter.QuadPart;
}
static BOOLEAN UfsTimedOut(UFS_DEVICE *D,ULONGLONG Started,ULONGLONG Timeout100ns) {
  ULONGLONG Now,Seconds,Remainder,Limit,Frequency;
  if(Started==MAXULONGLONG || !D || !(Frequency=D->PerformanceFrequency)) return TRUE;
  Now=UfsNow(D);
  if(Now==MAXULONGLONG || Now<Started) return TRUE;
  Seconds=Timeout100ns/10000000ULL; Remainder=Timeout100ns%10000000ULL;
  if((Seconds && Frequency>MAXULONGLONG/Seconds) ||
     (Remainder && Frequency>MAXULONGLONG/Remainder)) return TRUE;
  Limit=Seconds*Frequency;
  Remainder=(Remainder*Frequency)/10000000ULL;
  if(Remainder>MAXULONGLONG-Limit) return TRUE;
  Limit+=Remainder;
  return (BOOLEAN)(Now-Started>=Limit);
}
static void UfsLockInterrupt(UFS_DEVICE *D,PSTOR_LOCK_HANDLE H) {
  StorPortAcquireSpinLock(D,InterruptLock,NULL,H);
}
static void UfsUnlockInterrupt(UFS_DEVICE *D,PSTOR_LOCK_HANDLE H) {
  StorPortReleaseSpinLock(D,H);
}
static void UfsQueueDpcLocked(UFS_DEVICE *D,ULONG Work) {
  D->WorkFlags|=Work;
  if(D->DpcInitialized && !D->DpcScheduled && !D->DpcRunning) {
    D->DpcScheduled=TRUE;
    /* FALSE means this DPC object is already queued, not a queue failure. */
    (void)StorPortIssueDpc(D,&D->Dpc,NULL,NULL);
  }
}
static void UfsArmLocked(UFS_DEVICE *D) {
  if(!D->TimerArmed &&
     ((D->Stage!=UfsStopped && D->Transport.Active) || D->StopRetryPending ||
      D->MiniPowerActive ||
      (D->PendingShutdown && D->ShutdownPhase==UfsShutdownHibernate8))) {
    D->TimerArmed=TRUE;
    StorPortNotification(RequestTimerCall,D,UfsTimer,
      (D->StopRetryPending || (D->PendingShutdown && D->ShutdownPhase==UfsShutdownHibernate8) ||
       D->Transport.StopInProgress)?
      UFS_STOP_RETRY_US:UFS_TICK_US);
  }
}
static void UfsCancelTimerLocked(UFS_DEVICE *D) {
  if(D->TimerArmed) {
    D->TimerArmed=FALSE;
    StorPortNotification(RequestTimerCall,D,UfsTimer,0);
  }
}
static void UfsComplete(UFS_DEVICE *D,PSCSI_REQUEST_BLOCK S,UCHAR Status,ULONG Bytes) {
  S->SrbStatus=Status;
  // Storport resubmits a busy SRB with the original transfer parameters.
  if(Status!=SRB_STATUS_BUSY) S->DataTransferLength=Bytes;
  StorPortNotification(RequestComplete,D,S);
}
static void UfsSense(UFS_DEVICE *D,PSCSI_REQUEST_BLOCK S,UCHAR Key,UCHAR Asc) {
  UCHAR Sense[18]={0x70,0,0,0,0,0,0,10}; ULONG N;
  Sense[2]=Key; Sense[12]=Asc; S->ScsiStatus=2;
  N=S->SenseInfoBufferLength<sizeof(Sense)?S->SenseInfoBufferLength:(ULONG)sizeof(Sense);
  if(N && S->SenseInfoBuffer) {
    RtlCopyMemory(S->SenseInfoBuffer,Sense,N);
    S->SenseInfoBufferLength=(UCHAR)N;
    UfsComplete(D,S,SRB_STATUS_ERROR|SRB_STATUS_AUTOSENSE_VALID,0);
  } else UfsComplete(D,S,SRB_STATUS_ERROR,0);
}
static void UfsRequestStopLocked(UFS_DEVICE *D,UCHAR Status,UFS_STOP_MODE Mode);
static void UfsPowerService(UFS_DEVICE *D);
static BOOLEAN UfsQueueInitialWorkLocked(PVOID Extension,PVOID Context) {
  UFS_DEVICE *D=Extension;
  UNREFERENCED_PARAMETER(Context);
  if(!D || !D->DpcInitialized || D->DmaQuarantined || D->Stage!=UfsStarting ||
     D->StopRequested || D->DpcRunning || D->DpcScheduled) return FALSE;
  UfsQueueDpcLocked(D,UFS_DPC_WORK_INITIALIZE);
  return TRUE;
}
static BOOLEAN UfsMarkInitFailedLocked(PVOID Extension,PVOID Context) {
  UFS_DEVICE *D=Extension;
  UNREFERENCED_PARAMETER(Context);
  if(D && D->Stage==UfsStarting && !D->Transport.Attached) D->Stage=UfsStopped;
  return TRUE;
}
static BOOLEAN UfsCompletePending(UFS_DEVICE *D,PSCSI_REQUEST_BLOCK S,UCHAR Status,ULONG Bytes) {
  STOR_LOCK_HANDLE Lock;
  UfsLockInterrupt(D,&Lock);
  if((D->LatchedInterrupts&MTK_UFS_ERROR_INTERRUPTS) || D->IrqMaskUnverified) {
    UfsRequestStopLocked(D,SRB_STATUS_ERROR,UfsStopDisableHce);
    UfsUnlockInterrupt(D,&Lock);
    return FALSE;
  }
  if(D->Pending!=S || D->StopRequested) {
    UfsUnlockInterrupt(D,&Lock);
    return FALSE;
  }
  D->Pending=NULL; D->PendingCommand=FALSE; D->PendingWrite=FALSE;
  D->PendingShutdown=FALSE; D->ShutdownPhase=UfsShutdownNone;
  UfsUnlockInterrupt(D,&Lock);
  UfsComplete(D,S,Status,Bytes);
  return TRUE;
}
static void UfsRequestStopLocked(UFS_DEVICE *D,UCHAR Status,UFS_STOP_MODE Mode) {
  if(!D->StopRequested) {
    D->StopRequested=TRUE; D->StopMode=Mode; D->StopStatus=Status;
    D->StopAttempts=0; D->StopPermanent=FALSE; D->StopRetryPending=TRUE;
    D->Stage=UfsStopped; D->Restartable=FALSE; D->DmaQuarantined=TRUE;
    D->PendingCommand=FALSE; D->PendingShutdown=FALSE; D->ShutdownPhase=UfsShutdownNone;
    D->StopCompletePending=(BOOLEAN)(D->Pending!=NULL);
    UfsCancelTimerLocked(D);
  } else if(Status!=SRB_STATUS_SUCCESS) {
    D->StopStatus=Status;
  }
  if(!D->DpcRunning) UfsQueueDpcLocked(D,UFS_DPC_WORK_STOP);
}
static void UfsStop(UFS_DEVICE *D,UCHAR Status) {
  STOR_LOCK_HANDLE Lock;
  UfsLockInterrupt(D,&Lock);
  UfsRequestStopLocked(D,Status,UfsStopDisableHce);
  UfsUnlockInterrupt(D,&Lock);
}
static void UfsShutdownStopAndComplete(UFS_DEVICE *D,PSCSI_REQUEST_BLOCK S,UCHAR Status) {
  if(D->Pending!=S) return;
  UfsStop(D,Status);
}
static BOOLEAN UfsDumpRequested(PCHAR Arguments) {
  static const CHAR Pattern[]="dump=1";
  PCHAR P;
  if(!Arguments) return FALSE;
  for(P=Arguments;*P;P++) {
    const CHAR *A=P,*B=Pattern;
    while(*A && *B && *A==*B) { A++; B++; }
    if(!*B) return TRUE;
  }
  return FALSE;
}
ULONG UfsFind(PVOID Extension,PVOID Context,PVOID BusInfo,PCHAR Arguments,
  PPORT_CONFIGURATION_INFORMATION Config,PBOOLEAN Again) {
  UFS_DEVICE *D=Extension;
  PACCESS_RANGE Ranges;
  volatile ULONG *Handoff=NULL;
  STOR_PHYSICAL_ADDRESS Address,Physical,AlignedPhysical;
  ULONG I,RegIndex=MAXULONG,HandIndex=MAXULONG,Contiguous,Offset;
  UNREFERENCED_PARAMETER(Context); UNREFERENCED_PARAMETER(BusInfo);
  if(!D || !Again) return SP_RETURN_BAD_CONFIG;
  *Again=FALSE;
  if(UfsDumpRequested(Arguments)) return SP_RETURN_BAD_CONFIG;
  if(!Config || Config->Length<sizeof(*Config) || Config->NumberOfAccessRanges!=2 ||
     !Config->AccessRanges || Config->InterruptMode!=LevelSensitive ||
     (!Config->BusInterruptLevel && !Config->BusInterruptVector)) return SP_RETURN_BAD_CONFIG;
  Ranges=*Config->AccessRanges;
  for(I=0;I<2;I++) {
    if(!Ranges[I].RangeInMemory) return SP_RETURN_BAD_CONFIG;
    if((ULONGLONG)Ranges[I].RangeStart.QuadPart==UFS_MMIO && Ranges[I].RangeLength==UFS_MMIO_BYTES) RegIndex=I;
    else if((ULONGLONG)Ranges[I].RangeStart.QuadPart==UFS_HANDOFF && Ranges[I].RangeLength==16) HandIndex=I;
    else return SP_RETURN_BAD_CONFIG;
  }
  if(RegIndex==MAXULONG || HandIndex==MAXULONG) return SP_RETURN_BAD_CONFIG;
  RtlZeroMemory(D,sizeof(*D));
  Address.QuadPart=UFS_HANDOFF;
  Handoff=StorPortGetDeviceBase(D,Config->AdapterInterfaceType,Config->SystemIoBusNumber,Address,16,FALSE);
  if(!Handoff) return SP_RETURN_ERROR;
  if(StorPortReadRegisterUlong(D,(PULONG)Handoff)!=1 ||
     StorPortReadRegisterUlong(D,(PULONG)(Handoff+1))!=(ULONG)UFS_MMIO ||
     StorPortReadRegisterUlong(D,(PULONG)(Handoff+2))!=MTK_UFS_USER_LUN ||
     StorPortReadRegisterUlong(D,(PULONG)(Handoff+3))!=1 ||
     StorPortReadRegisterUlong(D,(PULONG)Handoff)!=1) {
    StorPortFreeDeviceBase(D,(PVOID)Handoff); return SP_RETURN_NOT_FOUND;
  }
  StorPortFreeDeviceBase(D,(PVOID)Handoff);
  Address.QuadPart=UFS_MMIO;
  D->Registers=StorPortGetDeviceBase(D,Config->AdapterInterfaceType,Config->SystemIoBusNumber,Address,UFS_MMIO_BYTES,FALSE);
  if(!D->Registers) return SP_RETURN_ERROR;
  Config->NumberOfBuses=1; Config->MaximumNumberOfTargets=1; Config->MaximumNumberOfLogicalUnits=1;
  Config->InitiatorBusId[0]=0xff;
  Config->MaximumTransferLength=MTK_UFS_DATA_BYTES; Config->NumberOfPhysicalBreaks=33;
  Config->ScatterGather=TRUE; Config->Master=TRUE;
  Config->MapBuffers=STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE;
  Config->NeedPhysicalAddresses=TRUE; Config->TaggedQueuing=FALSE;
  Config->AutoRequestSense=TRUE; Config->MultipleRequestPerLu=FALSE;
  Config->SynchronizationModel=StorSynchronizeFullDuplex; Config->VirtualDevice=FALSE;
  Config->Dma64BitAddresses=SCSI_DMA64_MINIPORT_FULL64BIT_SUPPORTED; Config->DmaAddressWidth=36;
  Config->MaxNumberOfIO=1; Config->MaxIOsPerLun=1; Config->InitialLunQueueDepth=1;
  Config->SrbType=SRB_TYPE_SCSI_REQUEST_BLOCK; Config->AddressType=STORAGE_ADDRESS_TYPE_BTL8;
  D->Allocation=StorPortGetUncachedExtension(D,Config,MTK_UFS_DMA_BYTES+1023);
  if(!D->Allocation) goto Fail;
  Contiguous=MTK_UFS_DMA_BYTES+1023;
  Physical=StorPortGetPhysicalAddress(D,NULL,D->Allocation,&Contiguous);
  Offset=(ULONG)((0-(ULONGLONG)Physical.QuadPart)&1023U);
  if(Contiguous<MTK_UFS_DMA_BYTES+Offset || (ULONGLONG)Physical.QuadPart>MAXULONGLONG-Offset) goto Fail;
  D->Dma=(volatile UCHAR *)D->Allocation+Offset;
  Contiguous=MTK_UFS_DMA_BYTES;
  AlignedPhysical=StorPortGetPhysicalAddress(D,NULL,(PVOID)D->Dma,&Contiguous);
  if(Contiguous<MTK_UFS_DMA_BYTES || AlignedPhysical.QuadPart!=Physical.QuadPart+Offset ||
     ((ULONGLONG)AlignedPhysical.QuadPart&1023U) || ((ULONG_PTR)D->Dma&7U) ||
     (ULONGLONG)AlignedPhysical.QuadPart>=0x1000000000ULL ||
     MTK_UFS_DMA_BYTES>0x1000000000ULL-(ULONGLONG)AlignedPhysical.QuadPart) goto Fail;
  D->Physical=(ULONGLONG)AlignedPhysical.QuadPart;
  return SP_RETURN_FOUND;
Fail:
  if(D->Transport.Attached) UfsStop(D,SRB_STATUS_ERROR);
  if(!D->DmaQuarantined) { StorPortFreeDeviceBase(D,D->Registers); D->Registers=NULL; }
  return SP_RETURN_ERROR;
}
BOOLEAN UfsInitialize(PVOID Extension) {
  UFS_DEVICE *D=Extension;
  LARGE_INTEGER Frequency,Counter;
  BOOLEAN Enabled;
  if(!D || !D->Registers || !D->Dma || D->DmaQuarantined || D->InitializationAttempted) return FALSE;
  if(StorPortQueryPerformanceCounter(D,&Frequency,&Counter)!=STOR_STATUS_SUCCESS ||
     Frequency.QuadPart<=0 || Counter.QuadPart<0) return FALSE;
  D->PerformanceFrequency=(ULONGLONG)Frequency.QuadPart;
  D->InitializationAttempted=TRUE; D->TimerArmed=FALSE; D->Stage=UfsStarting;
  Enabled=StorPortEnablePassiveInitialization(D,UfsPassiveInitialize);
  if(!Enabled) D->Stage=UfsStopped;
  return Enabled;
}
static BOOLEAN UfsStartTransport(UFS_DEVICE *D,BOOLEAN RestartStopped) {
  MTK_UFS_TRANSPORT_IO Io={D,UfsRead,UfsWrite,UfsFence,UfsStall};
  MTK_UFS_TRANSPORT Fresh={0},*Transport=&D->Transport;
  STOR_LOCK_HANDLE Lock;
  BOOLEAN Enabled;
  MTK_UFS_IO_STATUS Status;
  if(RestartStopped) {
    /* Only the generation-checked private resume may replace a completely
     * stopped transport. Keep the fault latch for generic/error restarts. */
    if(!D->MiniPowerActive || D->MiniPowerOperation!=UfsPowerOpRestartHost ||
       D->MiniPowerPhase!=UfsPowerPhaseRestartNop ||
       D->MiniPowerState!=MtkUfsMiniPowerHostRestarting ||
       D->MiniPowerLastOperation!=UfsPowerOpStopHost ||
       D->MiniPowerFlags!=MTK_UFS_POWER_DEVICE_DOWN_ACKED ||
       !D->MiniPowerAdmissionClosed || D->Stage!=UfsStopped || D->DmaQuarantined ||
       D->Transport.Attached || D->Transport.Active || D->Transport.StopInProgress ||
       D->Transport.UicPending || D->Transport.Hibernate8CommandDone) return FALSE;
    Transport=&Fresh;
  }
  /* Attach validates PRM's fresh live-HCE/idle-list contract before writes.
   * If a write partially acquires ownership, publish it even on failure so
   * the existing stop/quarantine path retains the DMA allocation. */
  Status=MtkUfsTransportAttach(Transport,&Io,D->Dma,MTK_UFS_DMA_BYTES,D->Physical);
  if(RestartStopped && Fresh.Attached) D->Transport=Fresh;
  if(Status!=MtkUfsIoSuccess) {
    D->Stage=UfsStopped;
    if(D->Transport.Attached) UfsStop(D,SRB_STATUS_ERROR);
    return FALSE;
  }
  if(MtkUfsTransportNop(&D->Transport)!=MtkUfsIoSuccess) { UfsStop(D,SRB_STATUS_ERROR); return FALSE; }
  D->Stage=UfsNop; D->Started=UfsNow(D); D->Timeout=UFS_INIT_TIMEOUT_100NS;
  UfsLockInterrupt(D,&Lock);
  UfsWrite(D,0x24,MTK_UFS_INTERRUPTS); UfsFence(D);
  Enabled=(BOOLEAN)(UfsRead(D,0x24)==MTK_UFS_INTERRUPTS);
  UfsUnlockInterrupt(D,&Lock);
  if(!Enabled) { UfsStop(D,SRB_STATUS_ERROR); return FALSE; }
  return TRUE;
}
static ULONG UfsMiniResult(ULONG Result) {
  return Result<=MtkUfsPowerBusy?Result:MtkUfsPowerIoError;
}
static BOOLEAN UfsPowerInputZero(const MTK_UFS_POWER_REQUEST *R) {
  ULONG I;
  if(R->Flags || R->State || R->Result || R->Capabilities) return FALSE;
  for(I=0;I<RTL_NUMBER_OF(R->Reserved);I++) if(R->Reserved[I]) return FALSE;
  return TRUE;
}
static void UfsMiniportControlRequest(UFS_DEVICE *D,PSCSI_REQUEST_BLOCK S) {
  PSRB_IO_CONTROL C;
  MTK_UFS_POWER_REQUEST *R;
  MTK_UFS_POWER_REQUEST LocalRequest;
  ULONG Result=MtkUfsPowerInvalid;
  ULONG Bytes=sizeof(SRB_IO_CONTROL)+sizeof(MTK_UFS_POWER_REQUEST);
  ULONG I;
  ULONGLONG InputGeneration;
  USHORT InputOperation;
  UFS_POWER_OP Op;
  BOOLEAN LiveResume=FALSE;
  if(!S->DataBuffer || S->DataTransferLength!=Bytes) {
    S->SrbStatus=SRB_STATUS_INVALID_REQUEST; S->DataTransferLength=0;
    StorPortNotification(RequestComplete,D,S); return;
  }
  C=(PSRB_IO_CONTROL)S->DataBuffer; R=&LocalRequest;
  RtlCopyMemory(R,(PUCHAR)S->DataBuffer+sizeof(*C),sizeof(*R));
  if(C->HeaderLength!=sizeof(*C) || C->ControlCode!=MTK_UFS_POWER_SRB_CONTROL_CODE ||
     C->ReturnCode || C->Length!=sizeof(*R) || R->Size!=sizeof(*R) || R->Version!=MTK_UFS_POWER_VERSION ||
     !UfsPowerInputZero(R)) {
    C->ReturnCode=MtkUfsPowerInvalid; S->SrbStatus=SRB_STATUS_INVALID_REQUEST;
    S->DataTransferLength=Bytes; StorPortNotification(RequestComplete,D,S); return;
  }
  for(I=0;I<sizeof(C->Signature);I++) {
    static const UCHAR Signature[8]=MTK_UFS_POWER_SRB_SIGNATURE;
    if(C->Signature[I]!=Signature[I]) break;
  }
  if(I!=sizeof(C->Signature)) {
    C->ReturnCode=MtkUfsPowerInvalid; S->SrbStatus=SRB_STATUS_INVALID_REQUEST;
    S->DataTransferLength=Bytes; StorPortNotification(RequestComplete,D,S); return;
  }
  InputGeneration=R->Generation; InputOperation=R->Operation;
  Op=InputOperation==MtkUfsMiniPowerPrepareDevice?UfsPowerOpPrepareDevice:
     InputOperation==MtkUfsMiniPowerStopHost?UfsPowerOpStopHost:
     InputOperation==MtkUfsMiniPowerRestartHost?UfsPowerOpRestartHost:UfsPowerOpNone;
  LiveResume=(BOOLEAN)(Op==UfsPowerOpRestartHost &&
    D->MiniPowerState==MtkUfsMiniPowerDevicePrepared &&
    D->MiniPowerFlags==MTK_UFS_POWER_DEVICE_DOWN_ACKED && D->Stage==UfsReady &&
    D->Transport.Attached && !D->Transport.Active && !D->Transport.UicPending &&
    !D->Transport.Hibernate8CommandDone && !D->Transport.StopInProgress);
  if(InputOperation==MtkUfsMiniPowerQuery) {
    Result=InputGeneration!=0?MtkUfsPowerInvalid:
      (D->MiniPowerState==0?MtkUfsPowerBusy:MtkUfsPowerSuccess);
  } else if(Op==UfsPowerOpNone) {
    Result=MtkUfsPowerUnsupported;
  } else if(D->MiniPowerActive) {
    Result=(Op==D->MiniPowerOperation && InputGeneration==D->MiniPowerOperationGeneration)?
      MtkUfsPowerInProgress:
      (InputGeneration!=D->MiniPowerGeneration?MtkUfsPowerStaleGeneration:MtkUfsPowerBusy);
  } else if(Op==D->MiniPowerLastOperation && InputGeneration==D->MiniPowerOperationGeneration) {
    Result=D->MiniPowerResult;
  } else if(InputGeneration!=D->MiniPowerGeneration) {
    Result=MtkUfsPowerStaleGeneration;
  } else if(D->MiniPowerGeneration==MAXULONGLONG) {
    Result=MtkUfsPowerBusy;
  } else if((Op==UfsPowerOpPrepareDevice && D->MiniPowerState!=MtkUfsMiniPowerActive) ||
            (Op==UfsPowerOpStopHost && D->MiniPowerState!=MtkUfsMiniPowerDevicePrepared) ||
            (Op==UfsPowerOpRestartHost && D->MiniPowerState!=MtkUfsMiniPowerHostStopped && !LiveResume)) {
    Result=MtkUfsPowerWrongState;
  } else if((Op==UfsPowerOpPrepareDevice && (D->Stage!=UfsReady || !D->Transport.Attached)) ||
            (Op==UfsPowerOpStopHost && (D->Stage!=UfsReady || !D->Transport.Attached ||
              D->Transport.Active || !(D->MiniPowerFlags&MTK_UFS_POWER_DEVICE_DOWN_ACKED)))) {
    Result=MtkUfsPowerWrongState;
  } else if(Op==UfsPowerOpRestartHost &&
            ((!LiveResume && (D->Stage!=UfsStopped || D->Transport.Attached)) ||
             (LiveResume && (D->Stage!=UfsReady || !D->Transport.Attached || D->Transport.Active ||
              D->Transport.UicPending || D->Transport.Hibernate8CommandDone ||
              D->Transport.StopInProgress || D->DmaQuarantined)))) {
    Result=MtkUfsPowerWrongState;
  } else if(!D->Registers || !D->Dma || D->DmaQuarantined) {
    Result=MtkUfsPowerIoError;
  } else {
    D->MiniPowerGeneration++;
    D->MiniPowerOperationGeneration=D->MiniPowerGeneration;
    D->MiniPowerOperation=Op; D->MiniPowerActive=TRUE;
    D->MiniPowerStarted=UfsNow(D); D->MiniPowerResult=MtkUfsPowerInProgress;
    D->MiniPowerPhase=(Op==UfsPowerOpPrepareDevice)?UfsPowerPhaseWaitDrain:
      (Op==UfsPowerOpStopHost)?UfsPowerPhaseStopHost:
      (LiveResume?UfsPowerPhaseRestartLiveActive:UfsPowerPhaseRestartNop);
    D->MiniPowerState=(Op==UfsPowerOpPrepareDevice)?MtkUfsMiniPowerDevicePreparing:
      (Op==UfsPowerOpStopHost)?MtkUfsMiniPowerHostStopping:MtkUfsMiniPowerHostRestarting;
    D->MiniPowerAdmissionClosed=TRUE;
    if(Op!=UfsPowerOpPrepareDevice && !LiveResume) D->Stage=UfsStopped;
    if(Op==UfsPowerOpPrepareDevice) D->MiniPowerFlags=0;
    Result=MtkUfsPowerInProgress;
    UfsQueueDpcLocked(D,UFS_DPC_WORK_POWER);
  }
  R->Flags=D->MiniPowerFlags; R->State=D->MiniPowerState;
  R->Generation=D->MiniPowerGeneration; R->Result=UfsMiniResult(Result);
  R->Capabilities=D->MiniPowerCapabilities;
  RtlCopyMemory((PUCHAR)S->DataBuffer+sizeof(*C),R,sizeof(*R));
  C->ReturnCode=R->Result; S->SrbStatus=SRB_STATUS_SUCCESS; S->DataTransferLength=Bytes;
  StorPortNotification(RequestComplete,D,S);
}
BOOLEAN UfsPassiveInitialize(PVOID Extension) {
  UFS_DEVICE *D=Extension;
  if(!D || !D->Registers || !D->Dma || D->DmaQuarantined || D->DpcInitialized || D->Stage!=UfsStarting) return FALSE;
  StorPortInitializeDpc(D,&D->Dpc,UfsDpc);
  D->DpcInitialized=TRUE;
  if(!StorPortSynchronizeAccess(D,UfsQueueInitialWorkLocked,NULL)) {
    (void)StorPortSynchronizeAccess(D,UfsMarkInitFailedLocked,NULL);
    return FALSE;
  }
  return TRUE;
}
static BOOLEAN UfsCapacityRetryable(const MTK_UFS_IO_RESULT *Result) {
  if(Result->ScsiStatus!=2 || Result->SenseLength<14 ||
     (Result->Sense[0]&0x7eU)!=0x70) return FALSE;
  return (BOOLEAN)((Result->Sense[2]&15U)==6U ||
    ((Result->Sense[2]&15U)==2U && Result->Sense[12]==4U));
}

static void UfsService(UFS_DEVICE *D) {
  MTK_UFS_IO_RESULT Result;
  MTK_UFS_IO_STATUS Status;
  uint64_t Last;
  uint32_t Block;
  PSCSI_REQUEST_BLOCK S;
  UCHAR Cdb[16]={0x9e,0x10,0,0,0,0,0,0,0,0,0,0,0,32};
  if(D->MiniPowerActive && !D->Pending) return;
  if(D->Stage==UfsStopped) return;
  if(D->PendingShutdown && D->ShutdownPhase==UfsShutdownHibernate8) {
    S=D->Pending;
    if(UfsTimedOut(D,D->Started,D->Timeout)) {
      UfsShutdownStopAndComplete(D,S,SRB_STATUS_TIMEOUT);
      return;
    }
    Status=MtkUfsTransportPollHibernate8(&D->Transport);
    if(Status==MtkUfsIoPending) return;
    UfsShutdownStopAndComplete(D,S,Status==MtkUfsIoSuccess?SRB_STATUS_SUCCESS:SRB_STATUS_ERROR);
    return;
  }
  if(!D->Transport.Active) return;
  Status=MtkUfsTransportPoll(&D->Transport,&Result);
  if(Status==MtkUfsIoPending) {
    if(UfsTimedOut(D,D->Started,D->Timeout)) UfsStop(D,SRB_STATUS_TIMEOUT);
    return;
  }
  if(Status!=MtkUfsIoSuccess) { UfsStop(D,SRB_STATUS_ERROR); return; }
  if(D->Stage==UfsNop) {
    if(MtkUfsTransportScsi(&D->Transport,Cdb,16,32)!=MtkUfsIoSuccess) { UfsStop(D,SRB_STATUS_ERROR); return; }
    D->Stage=UfsCapacity; D->CapacityAttempts=1; D->Started=UfsNow(D); return;
  }
  if(D->Stage==UfsCapacity) {
    UCHAR Capacity[32]; ULONG I;
    if(UfsCapacityRetryable(&Result) && D->CapacityAttempts<3 &&
       !UfsTimedOut(D,D->Started,D->Timeout)) {
      D->CapacityAttempts++;
      if(MtkUfsTransportScsi(&D->Transport,Cdb,16,32)!=MtkUfsIoSuccess) UfsStop(D,SRB_STATUS_ERROR);
      return;
    }
    for(I=0;I<sizeof(Capacity);I++) Capacity[I]=D->Dma[MTK_UFS_DATA_OFFSET+I];
    if(Result.ScsiStatus || Result.Transferred!=32 ||
       MtkUfsDecodeReadCapacity16(Capacity,sizeof(Capacity),&Last,&Block)!=MtkUfsCoreSuccess ||
       Block!=MTK_UFS_BLOCK_BYTES || !MtkUfsCapacityMatches(Last,Block,MTK_UFS_USER_BYTES)) {
      UfsStop(D,SRB_STATUS_ERROR); return;
    }
    {
      STOR_LOCK_HANDLE Lock;
      UfsLockInterrupt(D,&Lock);
      D->Stage=UfsReady;
      if(D->MiniPowerGeneration==0) D->MiniPowerGeneration=1;
      D->MiniPowerState=MtkUfsMiniPowerActive;
      D->MiniPowerCapabilities=MTK_UFS_POWER_CAP_PROGRESS;
      D->MiniPowerFlags=0; D->MiniPowerAdmissionClosed=FALSE;
      UfsUnlockInterrupt(D,&Lock);
    }
    return;
  }
  S=D->Pending;
  if(!S) { UfsStop(D,SRB_STATUS_ERROR); return; }
  S->ScsiStatus=Result.ScsiStatus;
  if(Result.ScsiStatus) {
    ULONG N=Result.SenseLength;
    UCHAR SrbStatus=SRB_STATUS_ERROR;
    if(N>S->SenseInfoBufferLength) N=S->SenseInfoBufferLength;
    if(N && S->SenseInfoBuffer) {
      RtlCopyMemory(S->SenseInfoBuffer,Result.Sense,N); S->SenseInfoBufferLength=(UCHAR)N;
      SrbStatus|=SRB_STATUS_AUTOSENSE_VALID;
    }
    if(D->PendingShutdown) {
      UfsShutdownStopAndComplete(D,S,SrbStatus);
      return;
    }
    UfsCompletePending(D,S,SrbStatus,0); return;
  }
  if(Result.Transferred>S->DataTransferLength || (Result.Transferred && !S->DataBuffer)) {
    UfsStop(D,SRB_STATUS_ERROR); return;
  }
  if(D->PendingShutdown && D->ShutdownPhase==UfsShutdownCache) {
    if(Result.Transferred || MtkUfsTransportPowerDown(&D->Transport)!=MtkUfsIoSuccess) {
      UfsShutdownStopAndComplete(D,S,SRB_STATUS_ERROR);
      return;
    }
    D->ShutdownPhase=UfsShutdownPowerDown; D->Started=UfsNow(D);
    return;
  }
  if(D->PendingShutdown && D->ShutdownPhase==UfsShutdownPowerDown) {
    if(Result.Transferred || MtkUfsTransportBeginHibernate8(&D->Transport)!=MtkUfsIoSuccess) {
      UfsShutdownStopAndComplete(D,S,SRB_STATUS_ERROR);
      return;
    }
    D->ShutdownPhase=UfsShutdownHibernate8; D->Started=UfsNow(D);
    D->Timeout=UFS_HIBERN8_TIMEOUT_100NS;
    return;
  }
  if(Result.Transferred && !D->PendingWrite) {
    RtlCopyMemory(S->DataBuffer,(PVOID)(D->Dma+MTK_UFS_DATA_OFFSET),Result.Transferred);
  }
  if(D->PendingShutdown) {
    UfsShutdownStopAndComplete(D,S,SRB_STATUS_SUCCESS);
    return;
  }
  UfsCompletePending(D,S,Result.Transferred==S->DataTransferLength?SRB_STATUS_SUCCESS:SRB_STATUS_DATA_OVERRUN,Result.Transferred);
}
static void UfsRecordStopEvidence(UFS_DEVICE *D) {
  if(!D->Registers) {
    D->LastIs=D->LastIe=D->LastHce=D->LastRun=D->LastDoor=UINT32_MAX;
    D->LastUtrlBaseLow=D->LastUtrlBaseHigh=D->LastUtmrlBaseLow=D->LastUtmrlBaseHigh=UINT32_MAX;
    return;
  }
  D->LastIs=UfsRead(D,0x20); D->LastIe=UfsRead(D,0x24); D->LastHce=UfsRead(D,0x34);
  D->LastRun=UfsRead(D,0x60)|UfsRead(D,0x80); D->LastDoor=UfsRead(D,0x58)|UfsRead(D,0x78);
  D->LastUtrlBaseLow=UfsRead(D,0x50); D->LastUtrlBaseHigh=UfsRead(D,0x54);
  D->LastUtmrlBaseLow=UfsRead(D,0x70); D->LastUtmrlBaseHigh=UfsRead(D,0x74);
}
static void UfsNotifyLinkDownOnce(UFS_DEVICE *D) {
  if(!D->StopLinkDownSent) {
    D->StopLinkDownSent=TRUE;
    StorPortNotification(LinkDown,D);
  }
}
static void UfsStopStep(UFS_DEVICE *D) {
  MTK_UFS_IO_STATUS Status;
  PSCSI_REQUEST_BLOCK S=NULL;
  UCHAR Completion=SRB_STATUS_ERROR;
  STOR_LOCK_HANDLE Lock;
  if(!D->StopRequested || D->StopPermanent) return;
  UfsNotifyLinkDownOnce(D);
  if(!D->Transport.Attached) Status=MtkUfsIoSuccess;
  else if(D->Transport.StopInProgress) Status=MtkUfsTransportPollStop(&D->Transport);
  else Status=MtkUfsTransportBeginStop(&D->Transport);
  D->StopAttempts++;
  UfsRecordStopEvidence(D);
  if(Status==MtkUfsIoPending) {
    if(D->StopAttempts<UFS_STOP_RETRY_MAX) D->StopRetryPending=TRUE;
    else Status=MtkUfsIoUnsafeDma;
  }
  if(Status==MtkUfsIoSuccess) {
    UfsLockInterrupt(D,&Lock);
    D->DmaQuarantined=FALSE; D->StopRetryPending=FALSE; D->StopRequested=FALSE;
    D->StopPermanent=FALSE; D->StopMode=UfsStopNone; D->StopAttempts=0;
    UfsCancelTimerLocked(D);
    if(D->StopCompletePending) {
      S=D->Pending; D->Pending=NULL; D->StopCompletePending=FALSE;
      Completion=D->StopStatus;
    }
    UfsUnlockInterrupt(D,&Lock);
    if(S) UfsComplete(D,S,Completion,0);
    return;
  }
  if(Status==MtkUfsIoUnsafeDma || Status==MtkUfsIoError || Status==MtkUfsIoNotReady) {
    UfsLockInterrupt(D,&Lock);
    D->StopRetryPending=FALSE; D->StopPermanent=TRUE; D->DmaQuarantined=TRUE;
    Completion=(UCHAR)((D->StopStatus&SRB_STATUS_AUTOSENSE_VALID)|SRB_STATUS_ERROR);
    UfsCancelTimerLocked(D);
    if(D->StopCompletePending) {
      S=D->Pending; D->Pending=NULL; D->StopCompletePending=FALSE;
    }
    UfsUnlockInterrupt(D,&Lock);
    if(S) UfsComplete(D,S,Completion,0);
    return;
  }
  UfsLockInterrupt(D,&Lock);
  UfsArmLocked(D);
  UfsUnlockInterrupt(D,&Lock);
}
static void UfsSubmitPending(UFS_DEVICE *D) {
  PSCSI_REQUEST_BLOCK S=D->Pending;
  MTK_UFS_IO_STATUS Status;
  if(!D->PendingCommand) return;
  if(!S) { UfsStop(D,SRB_STATUS_ERROR); return; }
  if(D->PendingWrite)
    Status=MtkUfsTransportWriteScsi(&D->Transport,D->PendingCdb,D->PendingCdbLength,
                                    D->PendingBytes,S->DataBuffer);
  else Status=MtkUfsTransportScsi(&D->Transport,D->PendingCdb,D->PendingCdbLength,D->PendingBytes);
  if(Status!=MtkUfsIoSuccess) { UfsStop(D,SRB_STATUS_ERROR); return; }
  D->PendingCommand=FALSE; D->Started=UfsNow(D);
}
static void UfsPowerFinish(UFS_DEVICE *D,ULONG Result,ULONG State) {
  STOR_LOCK_HANDLE Lock;
  UfsLockInterrupt(D,&Lock);
  D->MiniPowerLastOperation=D->MiniPowerOperation;
  D->MiniPowerOperation=UfsPowerOpNone; D->MiniPowerActive=FALSE;
  D->MiniPowerPhase=UfsPowerPhaseIdle; D->MiniPowerResult=Result;
  D->MiniPowerState=State; D->MiniPowerOperationGeneration=D->MiniPowerGeneration;
  D->MiniPowerAdmissionClosed=(BOOLEAN)(State!=MtkUfsMiniPowerActive);
  if(State==MtkUfsMiniPowerDevicePrepared) D->MiniPowerFlags=MTK_UFS_POWER_DEVICE_DOWN_ACKED;
  else if(State==MtkUfsMiniPowerActive) D->MiniPowerFlags=0;
  if(State==MtkUfsMiniPowerFault) {
    D->Stage=UfsStopped; D->Restartable=FALSE; D->DmaQuarantined=TRUE;
  } else if(State==MtkUfsMiniPowerHostStopped) {
    D->Stage=UfsStopped; D->Restartable=FALSE; D->DmaQuarantined=FALSE;
  } else if(State==MtkUfsMiniPowerActive) {
    D->Stage=UfsReady; D->Restartable=FALSE; D->DmaQuarantined=FALSE;
  }
  UfsUnlockInterrupt(D,&Lock);
}
static void UfsPowerFail(UFS_DEVICE *D,ULONG Result) {
  BOOLEAN Stop=(BOOLEAN)D->Transport.Attached;
  UfsPowerFinish(D,Result,MtkUfsMiniPowerFault);
  if(Stop) UfsStop(D,Result==MtkUfsPowerTimeout?SRB_STATUS_TIMEOUT:SRB_STATUS_ERROR);
}
static BOOLEAN UfsPowerExpired(UFS_DEVICE *D) {
  if(!UfsTimedOut(D,D->MiniPowerStarted,UFS_POWER_TIMEOUT_100NS)) return FALSE;
  UfsPowerFail(D,MtkUfsPowerTimeout);
  return TRUE;
}
static BOOLEAN UfsPowerBeginScsi(UFS_DEVICE *D,MTK_UFS_IO_STATUS Status,UFS_POWER_PHASE Phase) {
  if(Status!=MtkUfsIoSuccess) { UfsPowerFail(D,MtkUfsPowerIoError); return FALSE; }
  if(D->MiniPowerPhase!=Phase) D->MiniPowerStarted=UfsNow(D);
  D->MiniPowerPhase=Phase; return TRUE;
}
static void UfsPowerService(UFS_DEVICE *D) {
  MTK_UFS_IO_RESULT Result;
  MTK_UFS_IO_STATUS Status;
  UCHAR Capacity[32];
  UCHAR ReadCapacityCdb[16]={0x9e,0x10,0,0,0,0,0,0,0,0,0,0,0,32};
  ULONGLONG Last;
  uint32_t Block;
  ULONG I;
  if(!D->MiniPowerActive) return;
  if(D->MiniPowerOperation==UfsPowerOpPrepareDevice) {
    if(D->MiniPowerPhase==UfsPowerPhaseWaitDrain) {
      if(UfsPowerExpired(D)) return;
      if(D->Pending) return;
      if(!D->Transport.Attached || D->Transport.Active || D->Transport.UicPending ||
         D->Transport.Hibernate8CommandDone || D->DmaQuarantined) {
        UfsPowerFail(D,MtkUfsPowerWrongState); return;
      }
      if(UfsPowerBeginScsi(D,MtkUfsTransportReadAutoBkops(&D->Transport),UfsPowerPhaseReadBkops))
        return;
      return;
    }
    if(UfsPowerExpired(D)) return;
    Status=MtkUfsTransportPoll(&D->Transport,&Result);
    if(Status==MtkUfsIoPending) return;
    if(Status!=MtkUfsIoSuccess || Result.ScsiStatus!=0) {
      UfsPowerFail(D,MtkUfsPowerIoError); return;
    }
    if(D->MiniPowerPhase==UfsPowerPhaseReadBkops) {
      if(!Result.QueryFlagValid || Result.QueryFlagValue>1) {
        UfsPowerFail(D,MtkUfsPowerIoError); return;
      }
      if(Result.QueryFlagValue) {
        (void)UfsPowerBeginScsi(D,MtkUfsTransportDisableAutoBkops(&D->Transport),UfsPowerPhaseDisableBkops);
      } else {
        static const UCHAR FlushCdb[10]={0x35};
        (void)UfsPowerBeginScsi(D,MtkUfsTransportWriteScsi(&D->Transport,FlushCdb,sizeof(FlushCdb),0,NULL),UfsPowerPhaseFlush);
      }
      return;
    }
    if(D->MiniPowerPhase==UfsPowerPhaseDisableBkops) {
      if(Result.Transferred) { UfsPowerFail(D,MtkUfsPowerIoError); return; }
      (void)UfsPowerBeginScsi(D,MtkUfsTransportReadAutoBkops(&D->Transport),UfsPowerPhaseVerifyBkops);
      return;
    }
    if(D->MiniPowerPhase==UfsPowerPhaseVerifyBkops) {
      if(!Result.QueryFlagValid || Result.QueryFlagValue!=0) {
        UfsPowerFail(D,MtkUfsPowerIoError); return;
      }
      {
        static const UCHAR FlushCdb[10]={0x35};
        (void)UfsPowerBeginScsi(D,MtkUfsTransportWriteScsi(&D->Transport,FlushCdb,sizeof(FlushCdb),0,NULL),UfsPowerPhaseFlush);
      }
      return;
    }
    if(D->MiniPowerPhase==UfsPowerPhaseFlush) {
      if(Result.Transferred) { UfsPowerFail(D,MtkUfsPowerIoError); return; }
      (void)UfsPowerBeginScsi(D,MtkUfsTransportPowerDown(&D->Transport),UfsPowerPhasePowerDown);
      return;
    }
    if(D->MiniPowerPhase==UfsPowerPhasePowerDown) {
      if(Result.Transferred) { UfsPowerFail(D,MtkUfsPowerIoError); return; }
      UfsPowerFinish(D,MtkUfsPowerSuccess,MtkUfsMiniPowerDevicePrepared);
      return;
    }
    UfsPowerFail(D,MtkUfsPowerIoError); return;
  }
  if(D->MiniPowerOperation==UfsPowerOpStopHost) {
    if(D->MiniPowerPhase==UfsPowerPhaseStopHost) {
      Status=MtkUfsTransportBeginStop(&D->Transport);
      if(Status==MtkUfsIoSuccess) {
        UfsPowerFinish(D,MtkUfsPowerSuccess,MtkUfsMiniPowerHostStopped); return;
      }
      if(Status==MtkUfsIoPending) { D->MiniPowerPhase=UfsPowerPhaseStopPoll; D->MiniPowerPolls=0; return; }
      UfsPowerFail(D,MtkUfsPowerIoError); return;
    }
    if(D->MiniPowerPhase==UfsPowerPhaseStopPoll) {
      Status=MtkUfsTransportPollStop(&D->Transport);
      if(Status==MtkUfsIoPending) {
        if(++D->MiniPowerPolls<UFS_STOP_RETRY_MAX) return;
        D->DmaQuarantined=TRUE; UfsPowerFail(D,MtkUfsPowerTimeout); return;
      }
      if(Status!=MtkUfsIoSuccess) {
        D->DmaQuarantined=TRUE; UfsPowerFail(D,MtkUfsPowerIoError); return;
      }
      UfsPowerFinish(D,MtkUfsPowerSuccess,MtkUfsMiniPowerHostStopped); return;
    }
    UfsPowerFail(D,MtkUfsPowerIoError); return;
  }
  if(D->MiniPowerOperation==UfsPowerOpRestartHost) {
    if(D->MiniPowerPhase==UfsPowerPhaseRestartNop && !D->Transport.Attached) {
      if(!UfsStartTransport(D,TRUE)) { UfsPowerFail(D,MtkUfsPowerIoError); return; }
      D->MiniPowerStarted=UfsNow(D); return;
    }
    if(UfsPowerExpired(D)) return;
    if(D->MiniPowerPhase==UfsPowerPhaseRestartLiveActive) {
      (void)UfsPowerBeginScsi(D,MtkUfsTransportPowerActive(&D->Transport),UfsPowerPhaseRestartActive);
      return;
    }
    Status=MtkUfsTransportPoll(&D->Transport,&Result);
    if(Status==MtkUfsIoPending) return;
    if(Status==MtkUfsIoSuccess && D->MiniPowerPhase==UfsPowerPhaseRestartCapacity &&
       D->CapacityAttempts<3 && UfsCapacityRetryable(&Result)) {
      D->CapacityAttempts++;
      (void)UfsPowerBeginScsi(D,MtkUfsTransportScsi(&D->Transport,ReadCapacityCdb,sizeof(ReadCapacityCdb),32),
                              UfsPowerPhaseRestartCapacity);
      return;
    }
    if(Status!=MtkUfsIoSuccess || Result.ScsiStatus!=0) {
      UfsPowerFail(D,MtkUfsPowerIoError); return;
    }
    if(D->MiniPowerPhase==UfsPowerPhaseRestartNop) {
      if(Result.Transferred) { UfsPowerFail(D,MtkUfsPowerIoError); return; }
      (void)UfsPowerBeginScsi(D,MtkUfsTransportPowerActive(&D->Transport),UfsPowerPhaseRestartActive);
      return;
    }
    if(D->MiniPowerPhase==UfsPowerPhaseRestartActive) {
      if(Result.Transferred) { UfsPowerFail(D,MtkUfsPowerIoError); return; }
      D->CapacityAttempts=1;
      (void)UfsPowerBeginScsi(D,MtkUfsTransportScsi(&D->Transport,ReadCapacityCdb,sizeof(ReadCapacityCdb),32),
                              UfsPowerPhaseRestartCapacity);
      return;
    }
    if(D->MiniPowerPhase==UfsPowerPhaseRestartCapacity) {
      for(I=0;I<sizeof(Capacity);I++) Capacity[I]=D->Dma[MTK_UFS_DATA_OFFSET+I];
      if(Result.Transferred!=sizeof(Capacity) ||
         MtkUfsDecodeReadCapacity16(Capacity,sizeof(Capacity),&Last,&Block)!=MtkUfsCoreSuccess ||
         Block!=MTK_UFS_BLOCK_BYTES || !MtkUfsCapacityMatches(Last,Block,MTK_UFS_USER_BYTES)) {
        UfsPowerFail(D,MtkUfsPowerIoError); return;
      }
      D->StopLinkDownSent=FALSE;
      UfsPowerFinish(D,MtkUfsPowerSuccess,MtkUfsMiniPowerActive); return;
    }
    UfsPowerFail(D,MtkUfsPowerIoError); return;
  }
  UfsPowerFail(D,MtkUfsPowerIoError);
}
static void UfsEnableInterruptsAndCheck(UFS_DEVICE *D) {
  STOR_LOCK_HANDLE Lock;
  ULONG Is,Ie;
  UfsLockInterrupt(D,&Lock);
  if(!D->StopRequested && !D->StopPermanent && D->Stage!=UfsStopped && D->Transport.Attached) {
    if((D->LatchedInterrupts&MTK_UFS_ERROR_INTERRUPTS) || D->IrqMaskUnverified) {
      UfsRequestStopLocked(D,SRB_STATUS_ERROR,UfsStopDisableHce);
    } else {
      UfsWrite(D,0x24,MTK_UFS_INTERRUPTS); UfsFence(D);
      Ie=UfsRead(D,0x24); Is=UfsRead(D,0x20);
      D->LastIe=Ie; D->LastIs=Is;
      if(Ie!=MTK_UFS_INTERRUPTS || Is==UINT32_MAX) {
        UfsRequestStopLocked(D,SRB_STATUS_ERROR,UfsStopDisableHce);
      } else if(Is&MTK_UFS_INTERRUPTS) {
        UfsWrite(D,0x24,0); UfsWrite(D,0x20,Is&MTK_UFS_INTERRUPTS); UfsFence(D);
        D->LatchedInterrupts|=Is&MTK_UFS_INTERRUPTS;
        D->IrqMaskUnverified=(BOOLEAN)(UfsRead(D,0x24)!=0 ||
           (UfsRead(D,0x20)&(Is&MTK_UFS_INTERRUPTS))!=0);
        UfsQueueDpcLocked(D,UFS_DPC_WORK_INTERRUPT);
      } else D->IrqMaskUnverified=FALSE;
    }
  }
  UfsUnlockInterrupt(D,&Lock);
}
static void UfsFinishDpc(UFS_DEVICE *D) {
  STOR_LOCK_HANDLE Lock;
  UfsEnableInterruptsAndCheck(D);
  UfsLockInterrupt(D,&Lock);
  UfsArmLocked(D);
  D->DpcRunning=FALSE;
  if(D->StopRequested && !D->StopPermanent && !D->Transport.StopInProgress && !D->DpcScheduled)
    UfsQueueDpcLocked(D,UFS_DPC_WORK_STOP);
  else if(D->WorkFlags && !D->DpcScheduled) UfsQueueDpcLocked(D,0);
  UfsUnlockInterrupt(D,&Lock);
}
VOID UfsDpc(PSTOR_DPC Dpc,PVOID Extension,PVOID SystemArgument1,PVOID SystemArgument2) {
  UFS_DEVICE *D=Extension;
  STOR_LOCK_HANDLE Lock;
  ULONG Work,Owned;
  BOOLEAN IrqFault;
  UNREFERENCED_PARAMETER(Dpc); UNREFERENCED_PARAMETER(SystemArgument1); UNREFERENCED_PARAMETER(SystemArgument2);
  if(!D || !D->Registers) return;
  UfsLockInterrupt(D,&Lock);
  D->DpcScheduled=FALSE; D->DpcRunning=TRUE;
  Work=D->WorkFlags; D->WorkFlags=0;
  Owned=D->LatchedInterrupts; D->LatchedInterrupts=0;
  IrqFault=D->IrqMaskUnverified;
  D->TimerExpired=FALSE;
  UfsUnlockInterrupt(D,&Lock);
  if(D->StopRequested) {
    if(D->MiniPowerActive) UfsPowerFinish(D,MtkUfsPowerIoError,MtkUfsMiniPowerFault);
    UfsStopStep(D); UfsFinishDpc(D); return;
  }
  if((Owned&MTK_UFS_ERROR_INTERRUPTS) || IrqFault) {
    if(D->MiniPowerActive) UfsPowerFinish(D,MtkUfsPowerIoError,MtkUfsMiniPowerFault);
    UfsStop(D,SRB_STATUS_ERROR); UfsStopStep(D); UfsFinishDpc(D); return;
  }
  if(Work&UFS_DPC_WORK_RESTART) {
    D->RestartPending=FALSE;
    if(!UfsStartTransport(D,FALSE)) {
      if(D->StopRequested) UfsStopStep(D);
      else UfsNotifyLinkDownOnce(D);
      UfsFinishDpc(D); return;
    }
    D->StopLinkDownSent=FALSE;
    Work|=UFS_DPC_WORK_INTERRUPT;
  }
  if(Work&UFS_DPC_WORK_INITIALIZE) {
    if(!UfsStartTransport(D,FALSE)) {
      if(D->StopRequested) UfsStopStep(D);
      else UfsNotifyLinkDownOnce(D);
      UfsFinishDpc(D); return;
    }
    D->StopLinkDownSent=FALSE;
    Work|=UFS_DPC_WORK_INTERRUPT;
  }
  if(Work&UFS_DPC_WORK_START) UfsSubmitPending(D);
  if(D->StopRequested) {
    UfsStopStep(D); UfsFinishDpc(D); return;
  }
  if((Work&(UFS_DPC_WORK_INTERRUPT|UFS_DPC_WORK_TIMER|UFS_DPC_WORK_POWER)) &&
     (D->Transport.Active || D->Transport.StopInProgress || D->MiniPowerActive ||
      (D->PendingShutdown && D->ShutdownPhase==UfsShutdownHibernate8))) {
    UfsService(D);
    if(D->MiniPowerActive && !D->Pending && !D->StopRequested) UfsPowerService(D);
  }
  if(D->StopRequested) UfsStopStep(D);
  UfsFinishDpc(D);
}
BOOLEAN UfsInterrupt(PVOID Extension) {
  UFS_DEVICE *D=Extension; ULONG Is,Owned,Ie;
  if(!D || !D->Registers) return FALSE;
  if(D->Stage==UfsStopped && !D->Transport.Attached && !D->StopRequested) return FALSE;
  Is=UfsRead(D,0x20);
  if(Is==UINT32_MAX) return FALSE;
  Owned=Is&MTK_UFS_INTERRUPTS;
  if(!Owned) return FALSE;
  Ie=UfsRead(D,0x24);
  D->LastIs=Is; D->LastIe=Ie;
  UfsWrite(D,0x24,0); UfsWrite(D,0x20,Owned); UfsFence(D);
  Ie=UfsRead(D,0x24); Is=UfsRead(D,0x20);
  D->LastIe=Ie; D->LastIs=Is;
  D->IrqMaskUnverified=(BOOLEAN)(Ie!=0 || Is==UINT32_MAX || (Is&Owned)!=0);
  D->LatchedInterrupts|=Owned;
  if(D->Stage!=UfsStopped && !D->StopRequested)
    UfsQueueDpcLocked(D,UFS_DPC_WORK_INTERRUPT);
  else if(D->StopRetryPending && !D->StopPermanent)
    UfsQueueDpcLocked(D,UFS_DPC_WORK_STOP);
  return TRUE;
}
static BOOLEAN UfsTimerLocked(PVOID Extension,PVOID Context) {
  UNREFERENCED_PARAMETER(Context);
  UFS_DEVICE *D=Extension;
  if(!D->TimerArmed) return TRUE;
  D->TimerArmed=FALSE; D->TimerExpired=TRUE;
  UfsQueueDpcLocked(D,UFS_DPC_WORK_TIMER);
  return TRUE;
}
VOID UfsTimer(PVOID Extension) {
  StorPortSynchronizeAccess(Extension,UfsTimerLocked,NULL);
}
static BOOLEAN UfsStartLocked(PVOID Extension,PVOID Request) {
  PSCSI_REQUEST_BLOCK S=Request;
  UFS_DEVICE *D=Extension; ULONG N; ULONGLONG Timeout;
  BOOLEAN Write,Flush;
  UCHAR FlushCdb[10]={0x35};
  const UCHAR *Cdb;
  ULONG CdbLength;
  if(!D || !S) return FALSE;
  if(S->Length<sizeof(*S) || S->PathId || S->TargetId || S->Lun) {
    UfsComplete(D,S,SRB_STATUS_NO_DEVICE,0); return TRUE;
  }
  if(S->Function==SRB_FUNCTION_IO_CONTROL) {
    UfsMiniportControlRequest(D,S); return TRUE;
  }
  if(D->Pending) { UfsComplete(D,S,SRB_STATUS_BUSY,0); return TRUE; }
  if(D->Stage!=UfsReady) {
    UfsComplete(D,S,D->Stage==UfsStopped?SRB_STATUS_NO_DEVICE:SRB_STATUS_BUSY,0); return TRUE;
  }
  if(D->MiniPowerAdmissionClosed) {
    UfsComplete(D,S,SRB_STATUS_BUSY,0); return TRUE;
  }
  Flush=(BOOLEAN)(S->Function==SRB_FUNCTION_FLUSH || S->Function==SRB_FUNCTION_SHUTDOWN);
  if((S->Function!=SRB_FUNCTION_EXECUTE_SCSI && !Flush) ||
     (!Flush && (!S->CdbLength || S->CdbLength>16)) ||
     S->DataTransferLength>MTK_UFS_DATA_BYTES ||
     (S->DataTransferLength && !S->DataBuffer) || (Flush && S->DataTransferLength)) {
    UfsSense(D,S,5,0x24); return TRUE;
  }
  Cdb=Flush?FlushCdb:S->Cdb; CdbLength=Flush?10:S->CdbLength;
  Write=(BOOLEAN)MtkUfsTransportWriteCdb(Cdb,CdbLength,S->DataTransferLength);
  if(((S->SrbFlags&SRB_FLAGS_DATA_OUT) && (!Write || (S->SrbFlags&SRB_FLAGS_DATA_IN))) ||
     (S->DataTransferLength && !(S->SrbFlags&(Write?SRB_FLAGS_DATA_OUT:SRB_FLAGS_DATA_IN)))) {
    UfsSense(D,S,5,0x24); return TRUE;
  }
  /* Present one public LU; neither boot LUs nor RPMB are enumerated. */
  if(!Flush && S->Cdb[0]==0xa0) {
    const UCHAR *C=S->Cdb;
    ULONG Allocation=((ULONG)C[6]<<24)|((ULONG)C[7]<<16)|((ULONG)C[8]<<8)|C[9];
    if(S->CdbLength!=12 || C[1] || C[2] || C[3] || C[4] || C[5] || C[10] || C[11] ||
       Allocation<16 || Allocation!=S->DataTransferLength) { UfsSense(D,S,5,0x24); return TRUE; }
    N=S->DataTransferLength<16?S->DataTransferLength:16;
    RtlZeroMemory(S->DataBuffer,N); ((PUCHAR)S->DataBuffer)[3]=8;
    S->ScsiStatus=0; UfsComplete(D,S,N==S->DataTransferLength?SRB_STATUS_SUCCESS:SRB_STATUS_DATA_OVERRUN,N); return TRUE;
  }
  if(!Write && !MtkUfsTransportReadCdb(Cdb,CdbLength,S->DataTransferLength)) { UfsSense(D,S,5,0x20); return TRUE; }
  S->SrbStatus=SRB_STATUS_PENDING; D->Pending=S;
  D->PendingWrite=Write; D->PendingShutdown=(BOOLEAN)(S->Function==SRB_FUNCTION_SHUTDOWN);
  D->ShutdownPhase=D->PendingShutdown?UfsShutdownCache:UfsShutdownNone;
  D->PendingCdbLength=CdbLength; D->PendingBytes=S->DataTransferLength;
  RtlZeroMemory(D->PendingCdb,sizeof(D->PendingCdb));
  RtlCopyMemory(D->PendingCdb,Cdb,CdbLength);
  D->PendingCommand=TRUE;
  Timeout=(ULONGLONG)S->TimeOutValue*10000000ULL;
  D->Timeout=(!Timeout||Timeout>UFS_MAX_TIMEOUT_100NS)?UFS_MAX_TIMEOUT_100NS:Timeout;
  D->Started=UfsNow(D); UfsQueueDpcLocked(D,UFS_DPC_WORK_START); return TRUE;
}
BOOLEAN UfsStartIo(PVOID Extension,PSCSI_REQUEST_BLOCK S) {
  return StorPortSynchronizeAccess(Extension,UfsStartLocked,S);
}
static BOOLEAN UfsResetLocked(PVOID Extension,PVOID Context) {
  ULONG PathId=(ULONG)(ULONG_PTR)Context;
  UFS_DEVICE *D=Extension;
  if(D && !PathId && D->MiniPowerActive) return FALSE;
  if(D && !PathId) UfsRequestStopLocked(D,SRB_STATUS_BUS_RESET,UfsStopDisableHce);
  /* No false recovery claim: a fresh firmware handoff is required after HCE off. */
  return FALSE;
}
BOOLEAN UfsReset(PVOID Extension,ULONG PathId) {
  return StorPortSynchronizeAccess(Extension,UfsResetLocked,(PVOID)(ULONG_PTR)PathId);
}
SCSI_ADAPTER_CONTROL_STATUS UfsControl(PVOID Extension,SCSI_ADAPTER_CONTROL_TYPE Type,PVOID Parameters) {
  UFS_DEVICE *D=Extension;
  if(Type==ScsiQuerySupportedControlTypes && Parameters) {
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST L=Parameters;
    ULONG I;
    for(I=0;I<L->MaxControlType;I++) L->SupportedTypeList[I]=FALSE;
    if(L->MaxControlType>ScsiStopAdapter) L->SupportedTypeList[ScsiStopAdapter]=TRUE;
    if(L->MaxControlType>ScsiRestartAdapter) L->SupportedTypeList[ScsiRestartAdapter]=TRUE;
    return ScsiAdapterControlSuccess;
  }
  /* Storport invokes Stop/Restart with InterruptLock already held. */
  if(Type==ScsiStopAdapter && D) {
    if(D->MiniPowerActive) return ScsiAdapterControlUnsuccessful;
    if(D->Stage==UfsStopped && !D->Transport.Attached && !D->DmaQuarantined) return ScsiAdapterControlSuccess;
    D->Restartable=FALSE;
    if(!D->Pending && !D->Transport.Active && !D->DpcRunning && !D->DpcScheduled && !D->WorkFlags &&
       MtkUfsTransportDetach(&D->Transport)==MtkUfsIoSuccess) {
      D->Stage=UfsStopped; UfsCancelTimerLocked(D);
      D->Restartable=TRUE; D->InitializationAttempted=FALSE;
      return ScsiAdapterControlSuccess;
    }
    UfsRequestStopLocked(D,SRB_STATUS_ABORTED,UfsStopDisableHce);
    return ScsiAdapterControlUnsuccessful;
  }
  if(Type==ScsiRestartAdapter && D && D->Restartable && !D->MiniPowerActive) {
    D->Restartable=FALSE; D->RestartPending=TRUE; D->Stage=UfsStarting;
    UfsQueueDpcLocked(D,UFS_DPC_WORK_RESTART);
    return ScsiAdapterControlSuccess;
  }
  if(Type==ScsiRestartAdapter && D && !D->MiniPowerActive &&
     D->MiniPowerState==MtkUfsMiniPowerHostStopped && D->MiniPowerAdmissionClosed &&
     D->Stage==UfsStopped && !D->Transport.Attached && !D->DmaQuarantined) {
    /* D0 handoff restored board clocks first; the upper filter owns the later
       private RestartHost request that powers the device back up. */
    return ScsiAdapterControlSuccess;
  }
  return ScsiAdapterControlUnsuccessful;
}
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,PUNICODE_STRING RegistryPath) {
  HW_INITIALIZATION_DATA H;
  if(!DriverObject || !RegistryPath) return STATUS_NOT_SUPPORTED;
  RtlZeroMemory(&H,sizeof(H));
  // The controller is enumerated by ACPI (HANA678B).
  H.HwInitializationDataSize=sizeof(H); H.AdapterInterfaceType=ACPIBus;
  H.HwFindAdapter=UfsFind; H.HwInitialize=UfsInitialize; H.HwStartIo=UfsStartIo;
  H.HwInterrupt=UfsInterrupt; H.HwResetBus=UfsReset; H.HwAdapterControl=UfsControl;
  H.DeviceExtensionSize=sizeof(UFS_DEVICE); H.SrbExtensionSize=16; H.NumberOfAccessRanges=2;
  H.MapBuffers=STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE; H.NeedPhysicalAddresses=TRUE;
  H.AutoRequestSense=TRUE; H.SrbTypeFlags=SRB_TYPE_FLAG_SCSI_REQUEST_BLOCK;
  H.AddressTypeFlags=ADDRESS_TYPE_FLAG_BTL8;
  H.FeatureSupport=STOR_FEATURE_ADAPTER_NOT_REQUIRE_IO_PORT;
  return (NTSTATUS)StorPortInitialize(DriverObject,RegistryPath,&H,NULL);
}
