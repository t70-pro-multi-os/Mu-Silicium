// SPDX-License-Identifier: BSD-2-Clause-Patent
/* Execute the production BatteryD0Exit callback as ARM64 code with WDF mocks. */
#include <ntddk.h>
#include <wdf.h>
#include <string.h>
#include "BatteryCore.h"
#include "../MtkMali/PmicPrm.h"

static MALI_RESULT BatteryTestPmicSnapshot(MTK_PMIC_BATTERY_SNAPSHOT *Snapshot);
static ULONGLONG BatteryTestQueryInterruptTime(void);
static VOID BatteryTestCoreSuspend(MTK_BATTERY_CORE *Core);
static LONG BatteryTestInterlockedExchange(volatile LONG *Target,LONG Value);
static LONG BatteryTestInterlockedCompareExchange(volatile LONG *Target,LONG Value,LONG Compare);
#define BATTERYCLASS
#define MtkMaliPmicPrmReadBatterySnapshot BatteryTestPmicSnapshot
#define MtkBatteryCoreSuspend BatteryTestCoreSuspend
#undef KeQueryInterruptTime
#define KeQueryInterruptTime BatteryTestQueryInterruptTime
#undef InterlockedExchange
#undef InterlockedCompareExchange
#define InterlockedExchange BatteryTestInterlockedExchange
#define InterlockedCompareExchange BatteryTestInterlockedCompareExchange
#include "Driver.c"
#undef MtkBatteryCoreSuspend
#undef InterlockedExchange

__declspec(dllexport) unsigned MtkBatteryAssertionCount;
static unsigned Failure,LockDepth,TimerStops,WorkFlushes,Suspends,PmicCalls,ClassNotifications;
static ULONGLONG TestTime=100000000U;
static ULONG AcState;
static NTSTATUS AcStatus;
static MTK_PMIC_BATTERY_SNAPSHOT TestSnapshot;
static MTK_BATTERY_DEVICE Device;
static WDFFUNC WdfTable[WdfFunctionTableNumEntries];
const WDFFUNC *WdfFunctions=WdfTable;
PWDF_DRIVER_GLOBALS WdfDriverGlobals;
BOOLEAN WdfClientVersionHigherThanFramework;
ULONG WdfStructureCount;
WDF_STRUCT_INFO WdfStructures;
#define TEST_DEVICE ((WDFDEVICE)(ULONG_PTR)0x1001)
#define TEST_STATE_LOCK ((WDFWAITLOCK)(ULONG_PTR)0x1002)
#define TEST_CLASS_LOCK ((WDFWAITLOCK)(ULONG_PTR)0x1005)
#define TEST_TIMER ((WDFTIMER)(ULONG_PTR)0x1003)
#define TEST_WORK ((WDFWORKITEM)(ULONG_PTR)0x1004)
#define CHECK(X) do { MtkBatteryAssertionCount++; if(!(X)&&!Failure)Failure=__LINE__; } while(0)

static PVOID BatteryTestContext(PWDF_DRIVER_GLOBALS Globals,WDFOBJECT Object,PCWDF_OBJECT_CONTEXT_TYPE_INFO Info) {
  UNREFERENCED_PARAMETER(Globals);UNREFERENCED_PARAMETER(Info);CHECK(Object==TEST_DEVICE);return &Device;
}
static VOID BatteryTestLockAcquire(PWDF_DRIVER_GLOBALS Globals,WDFWAITLOCK Lock,PLONGLONG Timeout) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Lock==TEST_STATE_LOCK&&Timeout==NULL&&LockDepth==0);LockDepth++;
}
static VOID BatteryTestLockRelease(PWDF_DRIVER_GLOBALS Globals,WDFWAITLOCK Lock) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Lock==TEST_STATE_LOCK&&LockDepth==1);LockDepth--;
}
static BOOLEAN BatteryTestTimerStop(PWDF_DRIVER_GLOBALS Globals,WDFTIMER Timer,BOOLEAN Wait) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Timer==TEST_TIMER&&Wait&&LockDepth==0&&Device.Active==0);TimerStops++;return TRUE;
}
static VOID BatteryTestWorkFlush(PWDF_DRIVER_GLOBALS Globals,WDFWORKITEM Work) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Work==TEST_WORK&&LockDepth==0&&Device.Active==0&&Device.WorkQueued==1);
  WorkFlushes++;Device.WorkQueued=0;
}
static WDFOBJECT BatteryTestWorkParent(PWDF_DRIVER_GLOBALS Globals,WDFWORKITEM Work) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Work==TEST_WORK);return TEST_DEVICE;
}
static LONG BatteryTestInterlockedExchange(volatile LONG *Target,LONG Value) {
  LONG Old=*Target;*Target=Value;return Old;
}
static LONG BatteryTestInterlockedCompareExchange(volatile LONG *Target,LONG Value,LONG Compare) {
  LONG Old=*Target;if(Old==Compare)*Target=Value;return Old;
}
static WDFIOTARGET BatteryTestIoTarget(PWDF_DRIVER_GLOBALS Globals,WDFDEVICE Handle) {
  UNREFERENCED_PARAMETER(Globals);CHECK(Handle==TEST_DEVICE);
  return (WDFIOTARGET)(ULONG_PTR)0x1006;
}
static NTSTATUS BatteryTestEvalAcpi(PWDF_DRIVER_GLOBALS Globals,WDFIOTARGET Target,
    WDFREQUEST Request,ULONG Code,PWDF_MEMORY_DESCRIPTOR Input,PWDF_MEMORY_DESCRIPTOR Output,
    PWDF_REQUEST_SEND_OPTIONS Options,PULONG_PTR Bytes) {
  UNREFERENCED_PARAMETER(Globals);
  const ACPI_EVAL_INPUT_BUFFER *In=Input->u.BufferType.Buffer;
  ACPI_EVAL_OUTPUT_BUFFER *Out=Output->u.BufferType.Buffer;
  CHECK(Target==(WDFIOTARGET)(ULONG_PTR)0x1006 && !Request && Code==IOCTL_ACPI_EVAL_METHOD);
  CHECK(In->Signature==ACPI_EVAL_INPUT_BUFFER_SIGNATURE && !memcmp(In->MethodName,"ACST",4));
  CHECK(Options->Timeout==WDF_REL_TIMEOUT_IN_SEC(2) && LockDepth==1);
  if (AcStatus!=STATUS_SUCCESS) return AcStatus;
  Out->Signature=ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE;
  Out->Count=1;Out->Length=sizeof(*Out);
  Out->Argument[0].Type=ACPI_METHOD_ARGUMENT_INTEGER;
  Out->Argument[0].DataLength=sizeof(ULONG);
  Out->Argument[0].Argument=AcState;
  *Bytes=Out->Length;
  return STATUS_SUCCESS;
}
static MALI_RESULT BatteryTestPmicSnapshot(MTK_PMIC_BATTERY_SNAPSHOT *Snapshot) {
  PmicCalls++;if(!Snapshot)return MaliInvalid;*Snapshot=TestSnapshot;return MaliOk;
}
static ULONGLONG BatteryTestQueryInterruptTime(void) { return TestTime; }
NTSTATUS BatteryClassInitializeDevice(PBATTERY_MINIPORT_INFO Info,PVOID *Handle) {
  UNREFERENCED_PARAMETER(Info);UNREFERENCED_PARAMETER(Handle);return STATUS_NOT_SUPPORTED;
}
NTSTATUS BatteryClassUnload(PVOID Handle) { UNREFERENCED_PARAMETER(Handle);return STATUS_SUCCESS; }
NTSTATUS BatteryClassIoctl(PVOID Handle,PIRP Irp) { UNREFERENCED_PARAMETER(Handle);UNREFERENCED_PARAMETER(Irp);return STATUS_NOT_SUPPORTED; }
NTSTATUS BatteryClassSystemControl(PVOID Handle,PVOID WmiContext,PDEVICE_OBJECT DeviceObject,PIRP Irp,PVOID Disposition) {
  UNREFERENCED_PARAMETER(Handle);UNREFERENCED_PARAMETER(WmiContext);UNREFERENCED_PARAMETER(DeviceObject);UNREFERENCED_PARAMETER(Irp);UNREFERENCED_PARAMETER(Disposition);return STATUS_NOT_SUPPORTED;
}
NTSTATUS BatteryClassQueryWmiDataBlock(PVOID Handle,PDEVICE_OBJECT DeviceObject,PIRP Irp,ULONG GuidIndex,PULONG Lengths,ULONG BufferSize,PUCHAR Buffer) {
  UNREFERENCED_PARAMETER(Handle);UNREFERENCED_PARAMETER(DeviceObject);UNREFERENCED_PARAMETER(Irp);UNREFERENCED_PARAMETER(GuidIndex);UNREFERENCED_PARAMETER(Lengths);UNREFERENCED_PARAMETER(BufferSize);UNREFERENCED_PARAMETER(Buffer);return STATUS_NOT_SUPPORTED;
}
NTSTATUS BatteryClassStatusNotify(PVOID Handle) { UNREFERENCED_PARAMETER(Handle);ClassNotifications++;return STATUS_SUCCESS; }
static VOID BatteryTestCoreSuspend(MTK_BATTERY_CORE *Core) {
  CHECK(LockDepth==1);Suspends++;MtkBatteryCoreSuspend(Core);
}
int TestMain(void) {
  WdfTable[WdfObjectGetTypedContextWorkerTableIndex]=(WDFFUNC)BatteryTestContext;
  WdfTable[WdfWaitLockAcquireTableIndex]=(WDFFUNC)BatteryTestLockAcquire;
  WdfTable[WdfWaitLockReleaseTableIndex]=(WDFFUNC)BatteryTestLockRelease;
  WdfTable[WdfTimerStopTableIndex]=(WDFFUNC)BatteryTestTimerStop;
  WdfTable[WdfWorkItemFlushTableIndex]=(WDFFUNC)BatteryTestWorkFlush;
  WdfTable[WdfWorkItemGetParentObjectTableIndex]=(WDFFUNC)BatteryTestWorkParent;
  WdfTable[WdfDeviceGetIoTargetTableIndex]=(WDFFUNC)BatteryTestIoTarget;
  WdfTable[WdfIoTargetSendIoctlSynchronouslyTableIndex]=(WDFFUNC)BatteryTestEvalAcpi;

  RtlZeroMemory(&Device,sizeof(Device));MtkBatteryCoreInitialize(&Device.Core);
  Device.Device=TEST_DEVICE;Device.StateLock=TEST_STATE_LOCK;Device.ClassLock=TEST_CLASS_LOCK;
  Device.Active=1;TestTime=100000000U;
  TestSnapshot.BatteryVoltageMv=3900;TestSnapshot.BatteryCurrentStockUnits=10000;
  TestSnapshot.BatteryTemperatureDeciC=250;
  MTK_BATTERY_STATE Committed,Queried,After;MTK_BATTERY_CORE CoreBefore;
  ULONGLONG CacheBefore;
  CHECK(BatteryReadAndUpdate(&Device,TRUE,TRUE,&Committed)==STATUS_SUCCESS&&Device.StateValid);
  CoreBefore=Device.Core;CacheBefore=Device.CacheTime100ns;
  TestSnapshot.BatteryVoltageMv=3800;TestTime+=600000000U;
  CHECK(BatteryReadAndUpdate(&Device,TRUE,FALSE,&Queried)==STATUS_SUCCESS);
  CHECK(memcmp(&Device.Core,&CoreBefore,sizeof(CoreBefore))==0&&
        memcmp(&Device.State,&Committed,sizeof(Committed))==0&&Device.CacheTime100ns==CacheBefore);
  CHECK(BatteryReadAndUpdate(&Device,TRUE,TRUE,&After)==STATUS_SUCCESS&&
        Device.Core.LastSampleTime100ns==TestTime&&memcmp(&Device.State,&After,sizeof(After))==0&&
        PmicCalls==3&&ClassNotifications==0&&LockDepth==0);
  {
    BATTERY_INFORMATION Information={0};ULONG Returned=0;
    CHECK(BatteryQueryInformation(&Device,MTK_BATTERY_TAG,BatteryInformation,0,
        &Information,sizeof(Information),&Returned)==STATUS_SUCCESS);
    CHECK(Returned==sizeof(Information)&&Information.CycleCount==0);
  }

  BATTERY_STATUS Status={0};
  TestSnapshot.BatteryCurrentStockUnits=0;
  AcState=1;
  CHECK(BatteryReadAndUpdate(&Device,TRUE,TRUE,&After)==STATUS_SUCCESS);
  CHECK(BatteryQueryStatus(&Device,MTK_BATTERY_TAG,&Status)==STATUS_SUCCESS &&
        Status.PowerState==BATTERY_POWER_ON_LINE && Status.Rate==0);
  AcState=2;
  CHECK(BatteryReadAndUpdate(&Device,TRUE,TRUE,&After)==STATUS_SUCCESS &&
        After.PowerState==BATTERY_POWER_ON_LINE);
  AcStatus=STATUS_IO_TIMEOUT;
  CHECK(BatteryReadAndUpdate(&Device,TRUE,TRUE,&After)==STATUS_SUCCESS &&
        After.PowerState==BATTERY_POWER_ON_LINE);
  AcStatus=STATUS_SUCCESS;
  AcState=0;
  CHECK(BatteryReadAndUpdate(&Device,TRUE,TRUE,&After)==STATUS_SUCCESS &&
        After.PowerState==0);

  RtlZeroMemory(&Device,sizeof(Device));MtkBatteryCoreInitialize(&Device.Core);
  Device.Device=TEST_DEVICE;Device.StateLock=TEST_STATE_LOCK;Device.PollTimer=TEST_TIMER;
  Device.SampleWorkItem=TEST_WORK;Device.Active=1;Device.WorkQueued=1;Device.StateValid=TRUE;
  CHECK(BatteryD0Exit(TEST_DEVICE,WdfPowerDeviceD3)==STATUS_SUCCESS);
  CHECK(Device.Active==0&&Device.WorkQueued==0&&TimerStops==1&&WorkFlushes==1&&
        Suspends==1&&Device.Core.ReanchorOnNextSample&&LockDepth==0);
  return (int)Failure;
}
