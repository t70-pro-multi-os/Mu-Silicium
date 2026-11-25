/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <PrmModule.h>
#include <Protocol/PrmConfig.h>
#include <Protocol/MtkPmicRtcRuntime.h>
#include <Library/MtkRtcCalendar.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/MtkUfsPowerCore.h>
#include <Library/MtkPmicBroker.h>
#include <Library/SynchronizationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define RUNTIME_BANK_COUNT 4U
#define UFS_SMC_FID        0xC2000276U

typedef struct {
  UINT64 Count;
  PRM_RUNTIME_MMIO_RANGE Range[RUNTIME_BANK_COUNT];
} UFS_RUNTIME_RANGES;
typedef struct {
  UINTN Base[RUNTIME_BANK_COUNT];
} UFS_RUNTIME_IO_CONTEXT;

STATIC CONST UINT64 mPhysicalBases[RUNTIME_BANK_COUNT] = {
  0x11270000U, 0x10001000U, 0x10000000U, 0x10026000U
};
/* PRM maps whole pages; controller accesses stay within its stock 0x2300
 * resource; the remaining page padding is never accessed. */
STATIC CONST UINT32 mMappedLengths[RUNTIME_BANK_COUNT] = {
  0x3000U, 0x1000U, 0x1000U, 0x1000U
};
STATIC CONST UINT32 mAccessLengths[RUNTIME_BANK_COUNT] = {
  0x2300U, 0x1000U, 0x1000U, 0x1000U
};
STATIC CONST EFI_GUID mHandlerGuid = MTK_UFS_PRM_HANDLER_GUID;
STATIC CONST EFI_GUID mModuleGuid = EFI_CALLER_ID_GUID;
STATIC CONST EFI_GUID mPmicHandlerGuid = MTK_PMIC_PRM_HANDLER_GUID;
STATIC MTK_PMIC_BROKER mPmicBroker;
STATIC MTK_UFS_POWER_CORE mPowerCore;
STATIC volatile UINT32 mPowerLock;
STATIC EFI_HANDLE mConfigHandle;
STATIC PRM_CONTEXT_BUFFER mContextBuffers[2];
STATIC UFS_RUNTIME_RANGES mRuntimeRanges;
STATIC PRM_CONFIG_PROTOCOL mConfiguration;

STATIC BOOLEAN PointerSpanValid(UINTN Pointer, UINTN Bytes)
{
  return Pointer != 0 && Bytes != 0 && Pointer <= MAX_UINTN - (Bytes - 1U);
}

STATIC BOOLEAN SpansOverlap(UINTN Left, UINTN LeftBytes, UINTN Right, UINTN RightBytes)
{
  return Left <= Right + RightBytes - 1U && Right <= Left + LeftBytes - 1U;
}

/* The OS supplies converted virtual addresses. Check the entire fixed table
 * before any register access; never use the physical base as a fallback. */
STATIC BOOLEAN BuildRuntimeContext(
  CONST PRM_CONTEXT_BUFFER *ContextBuffer,
  CONST VOID *ParameterBuffer,
  CONST EFI_GUID *HandlerGuid,
  UFS_RUNTIME_IO_CONTEXT *IoContext
  )
{
  PRM_CONTEXT_BUFFER Context;
  UFS_RUNTIME_RANGES Ranges;
  UINTN Index, Other, Base, End, OtherEnd;
  UINT64 Count;
  if (!PointerSpanValid((UINTN)ContextBuffer, sizeof(Context)) ||
      !PointerSpanValid((UINTN)ParameterBuffer, sizeof(MTK_UFS_POWER_REQUEST)) ||
      IoContext == NULL ||
      SpansOverlap((UINTN)ParameterBuffer, sizeof(MTK_UFS_POWER_REQUEST),
                   (UINTN)ContextBuffer, sizeof(Context))) return FALSE;
  CopyMem(&Context, ContextBuffer, sizeof(Context));
  if (Context.Signature != PRM_CONTEXT_BUFFER_SIGNATURE ||
      Context.Version != PRM_CONTEXT_BUFFER_INTERFACE_VERSION ||
      Context.Reserved != 0 || !CompareGuid(&Context.HandlerGuid, HandlerGuid) ||
      Context.StaticDataBuffer != NULL ||
      !PointerSpanValid((UINTN)Context.RuntimeMmioRanges, sizeof(Ranges)) ||
      SpansOverlap((UINTN)ParameterBuffer, sizeof(MTK_UFS_POWER_REQUEST),
                   (UINTN)Context.RuntimeMmioRanges, sizeof(Ranges)) ||
      SpansOverlap((UINTN)ContextBuffer, sizeof(Context),
                   (UINTN)Context.RuntimeMmioRanges, sizeof(Ranges))) return FALSE;
  CopyMem(&Count, Context.RuntimeMmioRanges, sizeof(Count));
  if (Count != RUNTIME_BANK_COUNT) return FALSE;
  CopyMem(&Ranges, Context.RuntimeMmioRanges, sizeof(Ranges));
  for (Index = 0; Index < RUNTIME_BANK_COUNT; Index++) {
    CONST PRM_RUNTIME_MMIO_RANGE *Range = &Ranges.Range[Index];
    Base = (UINTN)Range->VirtualBaseAddress;
    if (Range->PhysicalBaseAddress != mPhysicalBases[Index] ||
        Range->Length != mMappedLengths[Index] || (Base & 0xFFFU) != 0 ||
        !PointerSpanValid(Base, Range->Length)) return FALSE;
    IoContext->Base[Index] = Base;
    End = Base + Range->Length - 1U;
    /* A request must never turn the final response copy into a register write,
     * nor overwrite the context/range metadata used by later invocations. */
    if (SpansOverlap(Base, Range->Length, (UINTN)ParameterBuffer, sizeof(MTK_UFS_POWER_REQUEST)) ||
        SpansOverlap(Base, Range->Length, (UINTN)ContextBuffer, sizeof(Context)) ||
        SpansOverlap(Base, Range->Length, (UINTN)Context.RuntimeMmioRanges, sizeof(Ranges))) return FALSE;
    for (Other = 0; Other < Index; Other++) {
      OtherEnd = IoContext->Base[Other] + mMappedLengths[Other] - 1U;
      if (Base <= OtherEnd && IoContext->Base[Other] <= End) return FALSE;
    }
  }
  return TRUE;
}

// WACS2 transactions from UEFI time services share mPowerLock with PRM.
typedef enum { RtcWacsIdle, RtcWacsRead, RtcWacsWrite, RtcWacsDrain } RTC_WACS_STATE;
STATIC RTC_WACS_STATE mRtcWacs;
STATIC BOOLEAN mRuntimeRtcWriteFault;

STATIC UINT32 RtcWacsState(UINTN Base)
{
  UINT32 Status=MmioRead32(Base+0x8A8U);
  return Status==MAX_UINT32 ? MAX_UINT32 : (Status>>1)&7U;
}

STATIC EFI_STATUS RtcWacsWait(UINTN Base,UINT32 Expected)
{
  UINT32 Count,State;
  for (Count=0;Count<1000U;Count++) {
    State=RtcWacsState(Base);
    if (State==Expected) return EFI_SUCCESS;
    if (State==MAX_UINT32) return EFI_DEVICE_ERROR;
    MicroSecondDelay(10U);
  }
  return EFI_TIMEOUT;
}

STATIC VOID RtcWacsWrite32(UINTN Base,UINTN Offset,UINT32 Value)
{
  MmioWrite32(Base+Offset,Value);
  MemoryFence();
}

// A timed-out read can leave an owned response in WACS2. Retire it
// before either PRM handler or another RTC call starts a request.
STATIC BOOLEAN RtcWacsRetire(UINTN Base)
{
  UINT32 State;
  if (mRtcWacs==RtcWacsIdle) return TRUE;
  State=RtcWacsState(Base);
  if (mRtcWacs==RtcWacsRead) {
    if (State!=6U) return FALSE;
    mRtcWacs=RtcWacsDrain;
    RtcWacsWrite32(Base,0x8A4U,1U);
    State=RtcWacsState(Base);
  }
  if (State!=0U) return FALSE;
  mRtcWacs=RtcWacsIdle;
  return TRUE;
}

STATIC BOOLEAN RtcCalendarRegister(UINT16 Address)
{
  return Address>=MTK_RTC_SECONDS && Address<MTK_RTC_SECONDS+2*MTK_RTC_FIELDS && !(Address&1U);
}

STATIC EFI_STATUS RtcReadRegister(VOID *Context,UINT16 Address,UINT16 *Value)
{
  UINTN Base=*(UINTN *)Context;
  EFI_STATUS Status;
  UINT16 Sample;
  if (Address!=MTK_RTC_BBPU && !RtcCalendarRegister(Address)) return EFI_INVALID_PARAMETER;
  if (RtcWacsState(Base)!=0U) return EFI_NOT_READY;
  mRtcWacs=RtcWacsRead;
  RtcWacsWrite32(Base,0x880U,Address);
  Status=RtcWacsWait(Base,6U);
  if (EFI_ERROR(Status)) return Status;
  Sample=(UINT16)MmioRead32(Base+0x894U);
  mRtcWacs=RtcWacsDrain;
  RtcWacsWrite32(Base,0x8A4U,1U);
  Status=RtcWacsWait(Base,0);
  if (EFI_ERROR(Status)) return Status;
  mRtcWacs=RtcWacsIdle;
  *Value=Sample;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS RtcWriteRegister(VOID *Context,UINT16 Address,UINT16 Value)
{
  UINTN Base=*(UINTN *)Context;
  EFI_STATUS Status;
  if (!RtcCalendarRegister(Address) && (Address!=MTK_RTC_TRIGGER || Value!=1U))
    return EFI_INVALID_PARAMETER;
  if (RtcWacsState(Base)!=0U) return EFI_NOT_READY;
  mRtcWacs=RtcWacsWrite;
  RtcWacsWrite32(Base,0x884U,Value);
  RtcWacsWrite32(Base,0x880U,0x20000000U|Address);
  Status=RtcWacsWait(Base,0);
  if (!EFI_ERROR(Status)) mRtcWacs=RtcWacsIdle;
  return Status;
}

STATIC EFI_STATUS EFIAPI RuntimeRtcAccess(UINTN Base,BOOLEAN SetTime,EFI_TIME *Time)
{
  MTK_RTC_IO Io={&Base,RtcReadRegister,RtcWriteRegister};
  EFI_STATUS Status;
  UINT16 Values[MTK_RTC_FIELDS];
  UINT32 Initial;
  if (!Time || !PointerSpanValid(Base,0x1000U) || (Base&0xFFFU)) return EFI_INVALID_PARAMETER;
  if (SetTime) {
    Status=MtkRtcEncodeCalendar(Time,Values);
    if (EFI_ERROR(Status)) return Status;
  }
  if (InterlockedCompareExchange32(&mPowerLock,0U,1U)!=0U) return EFI_NOT_READY;
  if (mPowerCore.WacsStage || MtkPmicBrokerBlocksUfs(&mPmicBroker) || !RtcWacsRetire(Base)) {
    Status=EFI_NOT_READY;
  } else if (mRuntimeRtcWriteFault) {
    Status=EFI_DEVICE_ERROR;
  } else {
    Initial=MmioRead32(Base);
    if (Initial==MAX_UINT32 || !(Initial&1U)) Status=EFI_NOT_READY;
    else if (SetTime) Status=MtkRtcWriteCalendar(&Io,Values,&mRuntimeRtcWriteFault);
    else Status=MtkRtcReadCalendar(&Io,Time);
  }
  (VOID)InterlockedCompareExchange32(&mPowerLock,1U,0U);
  return Status;
}

STATIC MTK_PMIC_RTC_RUNTIME_PROTOCOL mRtcAccess={RuntimeRtcAccess};

STATIC int RuntimeTryLock(void *Context)
{
  UFS_RUNTIME_IO_CONTEXT *IoContext=Context;
  /* PRM LockModule prevents module replacement; it does not serialize calls. */
  if (InterlockedCompareExchange32(&mPowerLock, 0U, 1U) != 0U) return 0;
  /* Keep the lease across PRM returns, including uncertain/faulted writes.
   * A completed cached GPU response needs no hardware and does not hold UFS. */
  if (MtkPmicBrokerBlocksUfs(&mPmicBroker) ||
      !RtcWacsRetire(IoContext->Base[MtkUfsPowerBankPwrap])) {
    (VOID)InterlockedCompareExchange32(&mPowerLock, 1U, 0U);
    return 0;
  }
  return 1;
}
STATIC void RuntimeUnlock(void *Context)
{
  (VOID)Context;
  (VOID)InterlockedCompareExchange32(&mPowerLock, 1U, 0U);
}
STATIC MTK_UFS_POWER_IO_STATUS RuntimeRead32(
  void *Context, MTK_UFS_POWER_BANK Bank, uint32_t Offset, uint32_t *Value
  )
{
  UFS_RUNTIME_IO_CONTEXT *IoContext = Context;
  if (IoContext == NULL || Value == NULL || (UINT32)Bank >= RUNTIME_BANK_COUNT ||
      (Offset & 3U) != 0 || Offset > mAccessLengths[Bank] - sizeof(UINT32)) return MtkUfsPowerIoFailed;
  if (Bank == MtkUfsPowerBankPwrap &&
      Offset != 0U && Offset != 0x8A8U && Offset != 0x894U) return MtkUfsPowerIoFailed;
  *Value = MmioRead32(IoContext->Base[Bank] + Offset);
  return MtkUfsPowerIoOk;
}
STATIC MTK_UFS_POWER_IO_STATUS RuntimeWrite32(
  void *Context, MTK_UFS_POWER_BANK Bank, uint32_t Offset, uint32_t Value
  )
{
  UFS_RUNTIME_IO_CONTEXT *IoContext = Context;
  /* TOP_UFS is a retained shared parent. No runtime client may alter it. */
  if (IoContext == NULL || (UINT32)Bank >= RUNTIME_BANK_COUNT ||
      Bank == MtkUfsPowerBankTop || (Offset & 3U) != 0 ||
      Offset > mAccessLengths[Bank] - sizeof(UINT32)) return MtkUfsPowerIoFailed;
  if (Bank == MtkUfsPowerBankInfra) {
    UINT32 Allowed = 0;
    if (Offset == 0xA4U || Offset == 0xA8U) Allowed = 0x30003800U;
    else if (Offset == 0x130U || Offset == 0x134U) Allowed = 1U << 15;
    else if (Offset == 0x140U || Offset == 0x144U) Allowed = 1U << 7;
    else if (Offset == 0x150U || Offset == 0x154U) Allowed = 1U << 21;
    if (Value == 0 || (Value & ~Allowed) != 0) return MtkUfsPowerIoFailed;
  }
  /* PWRAP access is limited to a read command and consuming our own response.
   * A PMIC data/enable/voltage write has no runtime wrapper path. */
  if (Bank == MtkUfsPowerBankPwrap &&
      !((Offset == 0x880U && (Value == 0x1B1CU || Value == 0x1E38U)) ||
        (Offset == 0x8A4U && Value == 1U))) return MtkUfsPowerIoFailed;
  MmioWrite32(IoContext->Base[Bank] + Offset, Value);
  MemoryFence();
  return MtkUfsPowerIoOk;
}
STATIC MTK_UFS_POWER_IO_STATUS RuntimeSmc(
  void *Context, uint32_t Command, uint32_t Value, uint32_t Stage, int64_t *RawResult
  )
{
  ARM_SMC_ARGS Args;
  (VOID)Context;
  if (RawResult == NULL || Value > 1U ||
      !((Command == 2U && Stage == 0U) || (Command == 8U && Stage <= 1U))) return MtkUfsPowerIoFailed;
  ZeroMem(&Args, sizeof(Args));
  Args.Arg0 = UFS_SMC_FID;
  Args.Arg1 = Command;
  Args.Arg2 = Value;
  Args.Arg3 = Stage;
  ArmCallSmc(&Args);
  *RawResult = (int64_t)Args.Arg0;
  return MtkUfsPowerIoOk;
}
STATIC uint64_t RuntimeNowUs(void *Context)
{
  (VOID)Context;
  return GetTimeInNanoSecond(GetPerformanceCounter()) / 1000U;
}

PRM_HANDLER_EXPORT(MtkUfsPrmHandler)
{
  UFS_RUNTIME_IO_CONTEXT Context;
  MTK_UFS_POWER_CORE_IO Io;
  MTK_UFS_POWER_REQUEST Request;
  if (!PointerSpanValid((UINTN)ParameterBuffer, sizeof(Request)) ||
      !BuildRuntimeContext(ContextBuffer, ParameterBuffer, &mHandlerGuid, &Context)) return EFI_INVALID_PARAMETER;
  CopyMem(&Request, ParameterBuffer, sizeof(Request));
  ZeroMem(&Io, sizeof(Io));
  Io.Context = &Context;
  Io.TryLock = RuntimeTryLock;
  Io.Unlock = RuntimeUnlock;
  Io.Read32 = RuntimeRead32;
  Io.Write32 = RuntimeWrite32;
  Io.Smc = RuntimeSmc;
  Io.NowUs = RuntimeNowUs;
  (VOID)MtkUfsPowerCoreHandle(&mPowerCore, &Io, &Request);
  CopyMem(ParameterBuffer, &Request, sizeof(Request));
  /* Endpoint results, including bounded progress, are in the wire buffer.
   * EFI status describes validation/dispatch of the handler itself. */
  return EFI_SUCCESS;
}

STATIC MTK_PMIC_BROKER_IO_STATUS RuntimePmicRead32(
  void *Context, uint32_t Offset, uint32_t *Value)
{
  return (MTK_PMIC_BROKER_IO_STATUS)RuntimeRead32(
    Context, MtkUfsPowerBankPwrap, Offset, Value);
}
STATIC BOOLEAN BrokerPmicAddressAllowed(UINT32 Address)
{
  /* The operation-specific WACS gate checks the executing lease and fields. */
  return Address == 0x28U || Address == 0x19EU || Address == 0x91CU ||
         Address == 0x910U ||
         Address == 0x1508U || Address == 0x1520U || Address == 0x1526U ||
         Address == 0x1BC8U || Address == 0x1BE4U || Address == 0x1BF4U ||
         Address == 0x1BE2U ||
         Address == 0x1E48U || Address == 0x1E4CU || Address == 0x1D70U ||
         Address == 0x1CD8U || Address == 0x1CC4U ||
         MtkPmicBrokerConnAddressAllowed(Address);
}
STATIC MTK_PMIC_BROKER_IO_STATUS RuntimePmicWrite32(
  void *Context, uint32_t Offset, uint32_t Value)
{
  UFS_RUNTIME_IO_CONTEXT *IoContext = Context;
  if (IoContext == NULL) return MtkPmicBrokerIoFailed;
  if (Offset == 0x880U) {
    UINT32 Address=Value&0xFFFFU;
    if ((Value&~0x2000FFFFU)!=0U || !BrokerPmicAddressAllowed(Address) ||
        !MtkPmicBrokerWacsWriteAllowed(&mPmicBroker,Offset,Value))
      return MtkPmicBrokerIoFailed;
  } else if (!MtkPmicBrokerWacsWriteAllowed(&mPmicBroker,Offset,Value)) {
    return MtkPmicBrokerIoFailed;
  }
  MmioWrite32(IoContext->Base[MtkUfsPowerBankPwrap] + Offset, Value);
  MemoryFence();
  return MtkPmicBrokerIoOk;
}
PRM_HANDLER_EXPORT(MtkPmicPrmHandler)
{
  UFS_RUNTIME_IO_CONTEXT Context;
  MTK_PMIC_BROKER_IO Io;
  MTK_PMIC_BROKER_REQUEST Request, Input;
  MTK_PMIC_BROKER_RESULT Result;
  if (!PointerSpanValid((UINTN)ParameterBuffer, sizeof(Request)) ||
      !BuildRuntimeContext(ContextBuffer, ParameterBuffer, &mPmicHandlerGuid, &Context))
    return EFI_INVALID_PARAMETER;
  CopyMem(&Request, ParameterBuffer, sizeof(Request));
  CopyMem(&Input, &Request, sizeof(Input));
  if (InterlockedCompareExchange32(&mPowerLock, 0U, 1U) != 0U) {
    /* No unlocked journal snapshot. Busy means the input was not executed. */
    Request.Result = MtkPmicBrokerResultBusy;
  } else if (!RtcWacsRetire(Context.Base[MtkUfsPowerBankPwrap])) {
    Request.Result = MtkPmicBrokerResultBusy;
    RuntimeUnlock(NULL);
  } else {
    Io.Context = &Context;
    Io.Read32 = RuntimePmicRead32;
    Io.Write32 = RuntimePmicWrite32;
    Io.NowUs = RuntimeNowUs;
    Result = MtkPmicBrokerHandle(&mPmicBroker, &Io, mPowerCore.WacsStage != 0U, &Request);
    if (Result == MtkPmicBrokerResultBusy) {
      CopyMem(&Request, &Input, sizeof(Request));
      Request.Result = MtkPmicBrokerResultBusy;
    }
    RuntimeUnlock(NULL);
  }
  CopyMem(ParameterBuffer, &Request, sizeof(Request));
  return EFI_SUCCESS;
}

PRM_MODULE_EXPORT(
  PRM_HANDLER_EXPORT_ENTRY(MTK_UFS_PRM_HANDLER_GUID, MtkUfsPrmHandler),
  PRM_HANDLER_EXPORT_ENTRY(MTK_PMIC_PRM_HANDLER_GUID, MtkPmicPrmHandler)
  );

EFI_STATUS EFIAPI MtkUfsPrmInitialize(
  EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;
  UINTN Index;
  (VOID)ImageHandle;
  (VOID)SystemTable;
  MtkUfsPowerCoreInitialize(&mPowerCore);
  MtkPmicBrokerInitialize(&mPmicBroker);
  mPowerLock = 0;
  ZeroMem(&mRuntimeRanges, sizeof(mRuntimeRanges));
  mRuntimeRanges.Count = RUNTIME_BANK_COUNT;
  for (Index = 0; Index < RUNTIME_BANK_COUNT; Index++) {
    mRuntimeRanges.Range[Index].PhysicalBaseAddress = mPhysicalBases[Index];
    mRuntimeRanges.Range[Index].Length = mMappedLengths[Index];
  }
  ZeroMem(mContextBuffers, sizeof(mContextBuffers));
  for (Index = 0; Index < 2U; Index++) {
    mContextBuffers[Index].Signature = PRM_CONTEXT_BUFFER_SIGNATURE;
    mContextBuffers[Index].Version = PRM_CONTEXT_BUFFER_INTERFACE_VERSION;
    CopyGuid(&mContextBuffers[Index].HandlerGuid, Index == 0U ? &mHandlerGuid : &mPmicHandlerGuid);
  }
  ZeroMem(&mConfiguration, sizeof(mConfiguration));
  CopyGuid(&mConfiguration.ModuleContextBuffers.ModuleGuid, &mModuleGuid);
  mConfiguration.ModuleContextBuffers.BufferCount = 2;
  mConfiguration.ModuleContextBuffers.Buffer = mContextBuffers;
  mConfiguration.ModuleContextBuffers.RuntimeMmioRanges =
    (PRM_RUNTIME_MMIO_RANGES *)&mRuntimeRanges;
  Status=gBS->InstallProtocolInterface(
    &mConfigHandle, &gPrmConfigProtocolGuid, EFI_NATIVE_INTERFACE, &mConfiguration);
  if (!EFI_ERROR(Status)) {
    // RTC is optional. Keep the installed PRM image resident if publishing
    // its time provider fails.
    (VOID)gBS->InstallProtocolInterface(
      &mConfigHandle, &gMtkPmicRtcRuntimeProtocolGuid, EFI_NATIVE_INTERFACE, &mRtcAccess);
  }
  return Status;
}
