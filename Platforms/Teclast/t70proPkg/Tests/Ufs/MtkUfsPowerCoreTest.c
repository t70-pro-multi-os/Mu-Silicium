/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Library/MtkUfsPowerCore.h>

static unsigned Checks;
#define CHECK(X) do { \
  Checks++; \
  if (!(X)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #X); \
    fprintf(stderr, "core state=%u op=%u phase=%u root=%u gate=%u wacs=%u uic=%u last=%u; time=%llu reads=%u writes=%u smc=%u\n", \
            Core.State, Core.Operation, Core.Phase, Core.RootIndex, Core.GateIndex, \
            Core.WacsStage, Core.UicPending, Core.LastError, \
            (unsigned long long)M.TimeUs, M.ReadCalls, M.WriteCalls, M.SmcCalls); \
    abort(); \
  } \
} while (0)

#define REG_WORDS        (0x2400U / 4U)
#define TRACE_CAPACITY   12000U
#define WACS_CMD         0x880U
#define WACS_RDATA       0x8A8U
#define WACS_SWINF_RDATA 0x894U
#define WACS_VLDCLR      0x8A4U
#define VEMC_ENABLE_REG  0x1B1CU
#define VEMC_VOLT_REG    0x1E38U
#define GATE_STATUS      0x0ACU
#define GATE_ENABLE_CLR  0x0A8U
#define GATE_DISABLE_SET 0x0A4U
#define GATE_MASK        ((1U << 28) | (1U << 13) | (1U << 12) | (1U << 11) | (1U << 29))
#define UIC_COMPLETE     (1U << 10)
#define MAX_STEPS        5000U

typedef struct {
  char Kind;
  uint32_t Bank;
  uint32_t Offset;
  uint32_t Value;
  uint64_t TimeUs;
} EVENT;

typedef struct {
  uint32_t Reg[4][REG_WORDS];
  uint16_t Pmic[0x8000];
  EVENT Trace[TRACE_CAPACITY];
  unsigned TraceCount;
  unsigned ReadCalls;
  unsigned WriteCalls;
  unsigned SmcCalls;
  unsigned LockCalls;
  unsigned UnlockCalls;
  unsigned CommandWrites;
  unsigned ValidClearWrites;
  unsigned UicWrites;
  unsigned GateWrites;
  unsigned HciReadsWithGateOff;
  unsigned FailedReadCount;
  unsigned BusyReadCount;
  unsigned FailedWriteCount;
  uint32_t FailedReadBank;
  uint32_t FailedReadOffset;
  int FailReadOnce;
  int BusyReadOnce;
  uint32_t BusyReadBank;
  uint32_t BusyReadOffset;
  uint32_t FailedWriteBank;
  uint32_t FailedWriteOffset;
  uint32_t FailedWriteValue;
  int FailWriteOnce;
  int FailWriteAfterEffect;
  uint32_t DropWriteBank;
  uint32_t DropWriteOffset;
  uint32_t DropWriteValue;
  unsigned DropWriteCount;
  int RejectLock;
  int Locked;
  int WacsFsm;
  int WacsDelayValid;
  int WacsAccepted;
  int HoldUic;
  int HoldUicReady;
  int HoldH8Probe;
  int UicSetArg3ReturnZero;
  int UicClearBusyAfterEffectOnce;
  unsigned UicClearBusyAtCommand;
  unsigned UicClearBusyCount;
  int AdvanceTime;
  int64_t SmcRawResult;
  unsigned FailSmcAt;
  uint64_t TimeUs;
  uint32_t ActivePmicAddress;
  uint32_t UfsProbeValue;
} MODEL;

static MODEL M;
static MTK_UFS_POWER_CORE Core;
static MTK_UFS_POWER_CORE_IO Io;

static const uint32_t RootOffsets[MTK_UFS_POWER_ROOT_COUNT] = {
  0x034U, 0x060U, 0x080U, 0x058U, 0x078U,
  0x050U, 0x054U, 0x070U, 0x074U, 0x024U
};
static const uint32_t GateBits[MTK_UFS_POWER_GATE_COUNT] = {
  1U << 28, 1U << 13, 1U << 12, 1U << 11, 1U << 29
};

static EVENT *
LogEvent(char Kind, uint32_t Bank, uint32_t Offset, uint32_t Value)
{
  EVENT *E;
  CHECK(M.TraceCount < TRACE_CAPACITY);
  E = &M.Trace[M.TraceCount++];
  E->Kind = Kind;
  E->Bank = Bank;
  E->Offset = Offset;
  E->Value = Value;
  E->TimeUs = M.TimeUs;
  return E;
}

static int
AnyGateOff(void)
{
  return (M.Reg[MtkUfsPowerBankInfra][GATE_STATUS / 4U] & GATE_MASK) != 0U;
}

static int
AllGatesOn(void)
{
  return !AnyGateOff();
}

static MTK_UFS_POWER_IO_STATUS
ModelRead32(void *Context, MTK_UFS_POWER_BANK Bank, uint32_t Offset, uint32_t *Value)
{
  uint32_t V;
  CHECK(Context == &M && Value != NULL);
  CHECK((unsigned)Bank < 4U && (Offset & 3U) == 0U && Offset < REG_WORDS * 4U);
  M.ReadCalls++;
  if (Bank == MtkUfsPowerBankHci && !AllGatesOn()) {
    M.HciReadsWithGateOff++;
    return MtkUfsPowerIoFailed;
  }
  if (M.FailReadOnce && Bank == M.FailedReadBank && Offset == M.FailedReadOffset) {
    M.FailReadOnce = 0;
    M.FailedReadCount++;
    LogEvent('E', Bank, Offset, 0U);
    return MtkUfsPowerIoFailed;
  }
  if (M.BusyReadOnce && Bank == M.BusyReadBank && Offset == M.BusyReadOffset) {
    M.BusyReadOnce = 0;
    M.BusyReadCount++;
    LogEvent('B', Bank, Offset, 0U);
    return MtkUfsPowerIoBusy;
  }
  if (Bank == MtkUfsPowerBankPwrap && Offset == WACS_RDATA) {
    V = ((uint32_t)M.WacsFsm & 7U) << 1;
  } else if (Bank == MtkUfsPowerBankPwrap && Offset == WACS_SWINF_RDATA) {
    V = M.Pmic[M.ActivePmicAddress & 0x7FFFU];
  } else if (Bank == MtkUfsPowerBankHci && Offset == 0x22C8U) {
    V = M.UfsProbeValue;
  } else if (Bank == MtkUfsPowerBankHci && Offset == 0x030U &&
             M.HoldUicReady && Core.Phase == MtkUfsPowerPhasePrepareHibernate) {
    V = M.Reg[Bank][Offset / 4U] & ~(1U << 3);
  } else {
    V = M.Reg[Bank][Offset / 4U];
  }
  *Value = V;
  LogEvent('R', Bank, Offset, V);
  return MtkUfsPowerIoOk;
}

static MTK_UFS_POWER_IO_STATUS
ModelWrite32(void *Context, MTK_UFS_POWER_BANK Bank, uint32_t Offset, uint32_t Value)
{
  int Fail;
  int Drop;
  int ReturnBusy = 0;
  CHECK(Context == &M);
  CHECK((unsigned)Bank < 4U && (Offset & 3U) == 0U && Offset < REG_WORDS * 4U);
  M.WriteCalls++;
  Fail = M.FailWriteOnce && Bank == M.FailedWriteBank && Offset == M.FailedWriteOffset && Value == M.FailedWriteValue;
  Drop = M.DropWriteCount != 0U && Bank == M.DropWriteBank && Offset == M.DropWriteOffset && Value == M.DropWriteValue;
  if (Drop) M.DropWriteCount--;
  if (Fail && !M.FailWriteAfterEffect) {
    M.FailWriteOnce = 0;
    M.FailedWriteCount++;
    LogEvent('E', Bank, Offset, Value);
    return MtkUfsPowerIoFailed;
  }
  if (Bank == MtkUfsPowerBankHci && !AllGatesOn()) {
    M.HciReadsWithGateOff++;
    return MtkUfsPowerIoFailed;
  }
  LogEvent(Drop ? 'D' : 'W', Bank, Offset, Value);
  if (!Drop) M.Reg[Bank][Offset / 4U] = Value;
  if (!Drop && Bank == MtkUfsPowerBankPwrap && Offset == WACS_CMD) {
    M.CommandWrites++;
    M.ActivePmicAddress = Value & 0x7FFFU;
    M.WacsAccepted = 1;
    M.WacsFsm = M.WacsDelayValid ? 0 : 6;
  } else if (!Drop && Bank == MtkUfsPowerBankPwrap && Offset == WACS_VLDCLR) {
    if (Value == 1U) {
      M.ValidClearWrites++;
      M.WacsFsm = 0;
    }
  } else if (!Drop && Bank == MtkUfsPowerBankInfra && Offset == GATE_DISABLE_SET) {
    M.GateWrites++;
    M.Reg[Bank][GATE_STATUS / 4U] |= Value;
  } else if (!Drop && Bank == MtkUfsPowerBankInfra && Offset == GATE_ENABLE_CLR) {
    M.GateWrites++;
    M.Reg[Bank][GATE_STATUS / 4U] &= ~Value;
  } else if (!Drop && Bank == MtkUfsPowerBankHci && Offset == 0x090U) {
    M.UicWrites++;
    if (M.HoldUic) M.Reg[Bank][0x020U / 4U] &= ~UIC_COMPLETE;
    else M.Reg[Bank][0x020U / 4U] |= UIC_COMPLETE;
    if ((Value & 0xFFU) == 1U) {
      M.Reg[Bank][0x09CU / 4U] = 0xA5A500F0U;
    } else if ((Value & 0xFFU) == 2U && M.UicSetArg3ReturnZero) {
      M.Reg[Bank][0x09CU / 4U] = 0U;
    }
    if ((Value & 0xFFU) == 0x17U && !M.HoldH8Probe) {
      M.UfsProbeValue = (3U << 28) | 1U;
    }
  } else if (!Drop && Bank == MtkUfsPowerBankHci && Offset == 0x020U) {
    M.Reg[Bank][Offset / 4U] &= ~Value; /* UCCS is write-one-to-clear. */
    if (M.UicClearBusyAfterEffectOnce && M.UicWrites == M.UicClearBusyAtCommand) {
      M.UicClearBusyAfterEffectOnce = 0;
      M.UicClearBusyCount++;
      ReturnBusy = 1;
    }
  } else if (!Drop && Bank == MtkUfsPowerBankHci && Offset == 0x144U) {
    M.Reg[Bank][Offset / 4U] = Value ? 3U : 0U;
  }
  if (Fail) {
    M.FailWriteOnce = 0;
    M.FailedWriteCount++;
    return MtkUfsPowerIoFailed;
  }
  if (ReturnBusy) return MtkUfsPowerIoBusy;
  return MtkUfsPowerIoOk;
}

static int
ModelTryLock(void *Context)
{
  CHECK(Context == &M);
  M.LockCalls++;
  if (M.RejectLock || M.Locked) return 0;
  M.Locked = 1;
  return 1;
}

static void
ModelUnlock(void *Context)
{
  CHECK(Context == &M && M.Locked);
  M.UnlockCalls++;
  M.Locked = 0;
}

static MTK_UFS_POWER_IO_STATUS
ModelSmc(void *Context, uint32_t Command, uint32_t Value, uint32_t Stage, int64_t *RawResult)
{
  CHECK(Context == &M && RawResult != NULL);
  M.SmcCalls++;
  LogEvent('S', 0U, Command, (Value & 0xFFFFU) | (Stage << 16));
  CHECK(Command == 2U || Command == 8U);
  CHECK((Command == 2U && Value <= 1U && Stage == 0U) ||
        (Command == 8U && Value <= 1U && Stage <= 1U));
  *RawResult = (M.FailSmcAt != 0U && M.SmcCalls == M.FailSmcAt) ? -3 : M.SmcRawResult;
  return MtkUfsPowerIoOk;
}

static uint64_t
ModelNowUs(void *Context)
{
  CHECK(Context == &M);
  if (M.AdvanceTime) M.TimeUs += 100U;
  return M.TimeUs;
}

static void
ResetModel(void)
{
  memset(&M, 0, sizeof(M));
  memset(&Core, 0, sizeof(Core));
  memset(&Io, 0, sizeof(Io));
  /* PMIC wrapper reports WACS2 initialized before any regulator read. */
  M.Reg[MtkUfsPowerBankPwrap][0U] = 1U;
  M.Pmic[VEMC_ENABLE_REG] = 1U;
  M.Pmic[VEMC_VOLT_REG] = 3U << 8;
  M.Reg[MtkUfsPowerBankHci][0x030U / 4U] = 0xFU;
  M.Reg[MtkUfsPowerBankHci][0x2240U / 4U] = 0x10450000U;
  M.Reg[MtkUfsPowerBankHci][0x034U / 4U] = 1U;
  M.Reg[MtkUfsPowerBankHci][0x144U / 4U] = 3U;
  M.UfsProbeValue = (2U << 28) | 1U;
  M.AdvanceTime = 1;
  Io.Context = &M;
  Io.TryLock = ModelTryLock;
  Io.Unlock = ModelUnlock;
  Io.Read32 = ModelRead32;
  Io.Write32 = ModelWrite32;
  Io.Smc = ModelSmc;
  Io.NowUs = ModelNowUs;
  MtkUfsPowerCoreInitialize(&Core);
}

static void
InitRequest(MTK_UFS_POWER_REQUEST *R, MTK_UFS_POWER_OPERATION Operation, uint64_t Generation, uint32_t Flags)
{
  memset(R, 0, sizeof(*R));
  R->Size = MTK_UFS_POWER_REQUEST_BYTES;
  R->Version = MTK_UFS_POWER_VERSION;
  R->Operation = (uint16_t)Operation;
  R->Generation = Generation;
  R->Flags = Flags;
}

static MTK_UFS_POWER_RESULT
Step(MTK_UFS_POWER_REQUEST *R)
{
  R->State = 0U;
  R->Result = 0U;
  R->Capabilities = 0U;
  if (R->Operation == MtkUfsPowerQuery) R->Generation = 0U;
  return MtkUfsPowerCoreHandle(&Core, &Io, R);
}

static MTK_UFS_POWER_RESULT
Drive(MTK_UFS_POWER_REQUEST *R)
{
  unsigned I;
  MTK_UFS_POWER_RESULT S = MtkUfsPowerInProgress;
  for (I = 0; I < MAX_STEPS; I++) {
    S = Step(R);
    if (S != MtkUfsPowerInProgress) return S;
  }
  CHECK(0 && "power operation exceeded bounded host-step budget");
  return S;
}

static MTK_UFS_POWER_RESULT
StartQuery(MTK_UFS_POWER_REQUEST *R)
{
  InitRequest(R, MtkUfsPowerQuery, 0U, 0U);
  return Drive(R);
}

static MTK_UFS_POWER_RESULT
StartPrepare(MTK_UFS_POWER_REQUEST *R)
{
  InitRequest(R, MtkUfsPowerPrepareStop, Core.Generation, MTK_UFS_POWER_DEVICE_DOWN_ACKED);
  return Drive(R);
}

static MTK_UFS_POWER_RESULT
StartRelease(MTK_UFS_POWER_REQUEST *R)
{
  InitRequest(R, MtkUfsPowerRelease, Core.Generation, 0U);
  return Drive(R);
}

static MTK_UFS_POWER_RESULT
StartRestore(MTK_UFS_POWER_REQUEST *R)
{
  InitRequest(R, MtkUfsPowerRestore, Core.Generation, 0U);
  return Drive(R);
}

static void
SetRoots(uint32_t Value)
{
  unsigned I;
  for (I = 0; I < MTK_UFS_POWER_ROOT_COUNT; I++) {
    M.Reg[MtkUfsPowerBankHci][RootOffsets[I] / 4U] = Value;
  }
}

static void
PrepareAndStopHost(MTK_UFS_POWER_REQUEST *Query, MTK_UFS_POWER_REQUEST *Prepare)
{
  CHECK(StartQuery(Query) == MtkUfsPowerSuccess);
  CHECK(Core.State == MtkUfsPowerActive && Core.Generation != 0U);
  CHECK(StartPrepare(Prepare) == MtkUfsPowerSuccess);
  CHECK(Core.State == MtkUfsPowerStopPrepared);
  /* Model the separately owned upper transport having stopped its controller. */
  SetRoots(0U);
}

static unsigned
FindWrite(unsigned Start, uint32_t Bank, uint32_t Offset, uint32_t Value)
{
  unsigned I;
  for (I = Start; I < M.TraceCount; I++) {
    if (M.Trace[I].Kind == 'W' && M.Trace[I].Bank == Bank &&
        M.Trace[I].Offset == Offset && M.Trace[I].Value == Value) return I;
  }
  return TRACE_CAPACITY;
}

static unsigned
FindEvent(unsigned Start, char Kind, uint32_t Bank, uint32_t Offset, uint32_t Value)
{
  unsigned I;
  for (I = Start; I < M.TraceCount; I++) {
    if (M.Trace[I].Kind == Kind && M.Trace[I].Bank == Bank &&
        M.Trace[I].Offset == Offset && M.Trace[I].Value == Value) return I;
  }
  return TRACE_CAPACITY;
}

typedef struct {
  uint32_t Opcode;
  uint32_t Arg1;
  uint32_t Arg3;
} UIC_EXPECTATION;

static void
CheckRestoreUicSequence(unsigned Start)
{
  UIC_EXPECTATION Expected[13];
  uint32_t Arg1 = UINT32_MAX;
  uint32_t Arg3 = UINT32_MAX;
  unsigned I;
  unsigned CommandIndex = 0U;
  unsigned ExpectedCount = 0U;

  Expected[ExpectedCount++] = (UIC_EXPECTATION){ 0x02U, 0xD0A80000U, 0x00000000U }; /* DME_POWERDOWN clear */
  Expected[ExpectedCount++] = (UIC_EXPECTATION){ 0x02U, 0x155E0000U, 0x00000000U }; /* PCS_CPORT */
  Expected[ExpectedCount++] = (UIC_EXPECTATION){ 0x01U, 0xD0A60000U, 0x00000000U }; /* SAVEPOWER read */
  Expected[ExpectedCount++] = (UIC_EXPECTATION){ 0x02U, 0xD0A60000U, 0xA5A500B0U }; /* clear bit 6 */
  if (Core.IpVersion == 0x10450000U) {
    Expected[ExpectedCount++] = (UIC_EXPECTATION){ 0x01U, 0xD09E0000U, 0x00000000U }; /* pre-link tuning read */
    Expected[ExpectedCount++] = (UIC_EXPECTATION){ 0x02U, 0xD09E0000U, 0xA5A500F0U }; /* bit 4 already set */
  }
  Expected[ExpectedCount++] = (UIC_EXPECTATION){ 0x16U, 0x00000000U, 0x00000000U }; /* LINK_STARTUP */
  if (Core.IpVersion >= 0x10450100U) {
    Expected[ExpectedCount++] = (UIC_EXPECTATION){ 0x01U, 0xD09E0000U, 0x00000000U }; /* post-link tuning read */
    Expected[ExpectedCount++] = (UIC_EXPECTATION){ 0x02U, 0xD09E0000U, 0xA5A501F0U }; /* set bit 8 */
  }
  Expected[ExpectedCount++] = (UIC_EXPECTATION){ 0x01U, 0xD0A60000U, 0x00000000U }; /* SAVEPOWER read */
  Expected[ExpectedCount++] = (UIC_EXPECTATION){ 0x02U, 0xD0A60000U, 0xA5A500FDU }; /* set low power bits */
  Expected[ExpectedCount++] = (UIC_EXPECTATION){ 0x01U, 0xD0A10000U, 0x00000000U }; /* DEBUGCLOCK read */
  Expected[ExpectedCount++] = (UIC_EXPECTATION){ 0x02U, 0xD0A10000U, 0xA5A500D0U }; /* clear bit 5 */

  for (I = Start; I < M.TraceCount; I++) {
    if (M.Trace[I].Kind != 'W' || M.Trace[I].Bank != MtkUfsPowerBankHci) continue;
    if (M.Trace[I].Offset == 0x094U) {
      Arg1 = M.Trace[I].Value;
    } else if (M.Trace[I].Offset == 0x09CU) {
      Arg3 = M.Trace[I].Value;
    } else if (M.Trace[I].Offset == 0x090U) {
      if (CommandIndex >= ExpectedCount) {
        fprintf(stderr, "unexpected UIC command #%u: opcode=%#x arg1=%#x arg3=%#x\n",
                CommandIndex, M.Trace[I].Value, Arg1, Arg3);
        CHECK(CommandIndex < ExpectedCount);
      }
      if (M.Trace[I].Value != Expected[CommandIndex].Opcode ||
          Arg1 != Expected[CommandIndex].Arg1 || Arg3 != Expected[CommandIndex].Arg3) {
        fprintf(stderr, "UIC command #%u: expected (%#x,%#x,%#x), got (%#x,%#x,%#x)\n",
                CommandIndex, Expected[CommandIndex].Opcode, Expected[CommandIndex].Arg1,
                Expected[CommandIndex].Arg3, M.Trace[I].Value, Arg1, Arg3);
      }
      CHECK(M.Trace[I].Value == Expected[CommandIndex].Opcode &&
            Arg1 == Expected[CommandIndex].Arg1 && Arg3 == Expected[CommandIndex].Arg3);
      CommandIndex++;
      Arg1 = UINT32_MAX;
      Arg3 = UINT32_MAX;
    }
  }
  if (CommandIndex != ExpectedCount) {
    fprintf(stderr, "UIC command count: expected %u, got %u\n", ExpectedCount, CommandIndex);
  }
  CHECK(CommandIndex == ExpectedCount);
}

static void
CheckRestoreResetAndHceTrace(unsigned Start)
{
  unsigned ResetAssert;
  unsigned HceOff;
  unsigned ResetRelease;
  unsigned RefOnPre;
  unsigned RefOnWrite;
  unsigned RefOnAck;
  unsigned RefOnPost;
  unsigned HciSet;
  unsigned CryptoSet;
  unsigned UniProSet;
  unsigned UniProClear;
  unsigned CryptoClear;
  unsigned HciClear;
  unsigned HceOn;
  unsigned HceReady;
  unsigned I;

  ResetAssert = FindEvent(Start, 'S', 0U, 2U, 0U);
  HceOff = FindWrite(ResetAssert + 1U, MtkUfsPowerBankHci, 0x034U, 0U);
  ResetRelease = FindEvent(HceOff + 1U, 'S', 0U, 2U, 1U);
  RefOnPre = FindEvent(ResetRelease + 1U, 'S', 0U, 8U, 1U);
  RefOnWrite = FindWrite(RefOnPre + 1U, MtkUfsPowerBankHci, 0x144U, 1U);
  RefOnAck = FindEvent(RefOnWrite + 1U, 'R', MtkUfsPowerBankHci, 0x144U, 3U);
  RefOnPost = FindEvent(RefOnAck + 1U, 'S', 0U, 8U, 0x10001U);
  HciSet = FindWrite(RefOnPost + 1U, MtkUfsPowerBankInfra, 0x130U, 1U << 15);
  CryptoSet = FindWrite(HciSet + 1U, MtkUfsPowerBankInfra, 0x150U, 1U << 21);
  UniProSet = FindWrite(CryptoSet + 1U, MtkUfsPowerBankInfra, 0x140U, 1U << 7);
  UniProClear = FindWrite(UniProSet + 1U, MtkUfsPowerBankInfra, 0x144U, 1U << 7);
  CryptoClear = FindWrite(UniProClear + 1U, MtkUfsPowerBankInfra, 0x154U, 1U << 21);
  HciClear = FindWrite(CryptoClear + 1U, MtkUfsPowerBankInfra, 0x134U, 1U << 15);
  HceOn = FindWrite(HciClear + 1U, MtkUfsPowerBankHci, 0x034U, 1U);
  HceReady = FindEvent(HceOn + 1U, 'R', MtkUfsPowerBankHci, 0x034U, 1U);

  CHECK(ResetAssert < HceOff && HceOff < ResetRelease &&
        ResetRelease < RefOnPre && RefOnPre < RefOnWrite &&
        RefOnWrite < RefOnAck && RefOnAck < RefOnPost && RefOnPost < HciSet &&
        ResetRelease < HciSet && HciSet < CryptoSet && CryptoSet < UniProSet &&
        UniProSet < UniProClear && UniProClear < CryptoClear &&
        CryptoClear < HciClear && HciClear < HceOn && HceOn < HceReady);
  CHECK(FindEvent(Start, 'R', MtkUfsPowerBankHci, 0x144U, 0U) < ResetAssert);
  for (I = 0; I < MTK_UFS_POWER_ROOT_COUNT; I++) {
    CHECK(FindEvent(Start, 'R', MtkUfsPowerBankHci, RootOffsets[I], 0U) < ResetAssert);
  }
  CHECK(M.Trace[RefOnPost].TimeUs >= M.Trace[RefOnAck].TimeUs + 32U);
  CHECK(M.Trace[ResetRelease].TimeUs >= M.Trace[HceOff].TimeUs + 10U);
  CHECK(M.Trace[HciSet].TimeUs >= M.Trace[ResetRelease].TimeUs + 10000U);
  CHECK(M.Trace[UniProClear].TimeUs >= M.Trace[UniProSet].TimeUs + 100U);
  CHECK(M.Trace[HceReady].TimeUs >= M.Trace[HceOn].TimeUs + 600U);
}

static void
TestMalformedAndLocking(void)
{
  MTK_UFS_POWER_REQUEST R;
  MTK_UFS_POWER_CORE CoreBefore;
  unsigned LockCallsBefore;
  unsigned UnlocksBefore;
  unsigned I;
  ResetModel();
  for (I = 0; I < 9U; I++) {
    InitRequest(&R, MtkUfsPowerQuery, 0U, 0U);
    switch (I) {
      case 0: R.Size--; break;
      case 1: R.Version++; break;
      case 2: R.Operation = 0xFFFFU; break;
      case 3: R.Flags = 1U; break;
      case 4: R.State = 1U; break;
      case 5: R.Result = 1U; break;
      case 6: R.Capabilities = 1U; break;
      case 7: R.Reserved[0] = 1U; break;
      default: R.Generation = 1U; break;
    }
    CoreBefore = Core;
    CHECK(MtkUfsPowerCoreHandle(&Core, &Io, &R) == MtkUfsPowerInvalid);
    CHECK(R.Result == MtkUfsPowerInvalid);
    CHECK(memcmp(&Core, &CoreBefore, sizeof(Core)) == 0);
    CHECK(M.ReadCalls == 0U && M.WriteCalls == 0U && M.SmcCalls == 0U);
  }
  for (I = 0; I < 4U; I++) {
    InitRequest(&R, MtkUfsPowerQuery, 0U, 0U);
    R.Reserved[I] = 1U;
    CHECK(MtkUfsPowerCoreHandle(&Core, &Io, &R) == MtkUfsPowerInvalid);
    CHECK(M.ReadCalls == 0U && M.WriteCalls == 0U && M.SmcCalls == 0U);
  }
  InitRequest(&R, MtkUfsPowerQuery, 0U, 0U);
  LockCallsBefore = M.LockCalls;
  UnlocksBefore = M.UnlockCalls;
  M.RejectLock = 1;
  CHECK(MtkUfsPowerCoreHandle(&Core, &Io, &R) == MtkUfsPowerBusy);
  CHECK(M.ReadCalls == 0U && M.WriteCalls == 0U &&
        M.LockCalls == LockCallsBefore + 1U && M.UnlockCalls == UnlocksBefore);
  M.RejectLock = 0;
  InitRequest(&R, MtkUfsPowerRelease, Core.Generation, 0U);
  CHECK(MtkUfsPowerCoreHandle(&Core, &Io, &R) == MtkUfsPowerWrongState);
  CHECK(M.Locked == 0 && M.LockCalls == M.UnlockCalls + 1U);
}

static void
TestQueryAndWacsOwnership(void)
{
  MTK_UFS_POWER_REQUEST R;
  unsigned BeforeHciWrites;
  unsigned BeforeInfraWrites;
  unsigned BeforeSmc;
  unsigned I;
  ResetModel();
  CHECK(StartQuery(&R) == MtkUfsPowerSuccess);
  CHECK(Core.State == MtkUfsPowerActive && Core.Generation != 0U);
  CHECK((R.Capabilities & (MTK_UFS_POWER_CAP_RETAINED_VEMC | MTK_UFS_POWER_CAP_LEAF_GATES)) ==
        (MTK_UFS_POWER_CAP_RETAINED_VEMC | MTK_UFS_POWER_CAP_LEAF_GATES));
  CHECK(M.Pmic[VEMC_ENABLE_REG] == 1U && ((M.Pmic[VEMC_VOLT_REG] >> 8) & 7U) == 3U);
  CHECK(M.CommandWrites == 2U && M.ValidClearWrites == 2U);

  ResetModel();
  M.FailedWriteBank = MtkUfsPowerBankPwrap;
  M.FailedWriteOffset = WACS_CMD;
  M.FailedWriteValue = VEMC_ENABLE_REG;
  M.FailWriteOnce = 1;
  M.FailWriteAfterEffect = 1;
  InitRequest(&R, MtkUfsPowerQuery, 0U, 0U);
  CHECK(Drive(&R) == MtkUfsPowerIoError);
  CHECK(M.CommandWrites == 1U);
  CHECK(Drive(&R) == MtkUfsPowerSuccess);
  CHECK(M.CommandWrites == 2U && M.ValidClearWrites == 2U);
  BeforeHciWrites = 0U; BeforeInfraWrites = 0U;
  for (I = 0; I < M.TraceCount; I++) {
    if (M.Trace[I].Kind == 'W' && M.Trace[I].Bank == MtkUfsPowerBankHci) BeforeHciWrites++;
    if (M.Trace[I].Kind == 'W' && M.Trace[I].Bank == MtkUfsPowerBankInfra) BeforeInfraWrites++;
  }
  BeforeSmc = M.SmcCalls;
  CHECK(BeforeHciWrites == 0U && BeforeInfraWrites == 0U && BeforeSmc == 0U);
  CHECK(M.Reg[MtkUfsPowerBankTop][0x0D0U / 4U] == 0U && AllGatesOn());
  for (I = 0; I < M.TraceCount; I++) {
    CHECK(!(M.Trace[I].Kind == 'W' && M.Trace[I].Bank == MtkUfsPowerBankTop));
  }

  ResetModel();
  M.WacsFsm = 6; /* Unowned stale valid response must never be cleared. */
  InitRequest(&R, MtkUfsPowerQuery, 0U, 0U);
  CHECK(MtkUfsPowerCoreHandle(&Core, &Io, &R) == MtkUfsPowerBusy);
  CHECK(M.CommandWrites == 0U && M.ValidClearWrites == 0U && M.SmcCalls == 0U);

  ResetModel();
  M.WacsDelayValid = 1;
  InitRequest(&R, MtkUfsPowerQuery, 0U, 0U);
  CHECK(MtkUfsPowerCoreHandle(&Core, &Io, &R) == MtkUfsPowerInProgress);
  CHECK(M.CommandWrites == 1U);
  M.WacsDelayValid = 0;
  M.WacsFsm = 1;
  for (I = 0; I < 3U; I++) {
    (void)Step(&R);
    CHECK(M.CommandWrites == 1U); /* Accepted WACS read is polled, never resent. */
  }
  M.WacsFsm = 6;
  CHECK(Drive(&R) == MtkUfsPowerSuccess);
  CHECK(M.CommandWrites == 2U && M.ValidClearWrites == 2U);
}

static void
TestVoltageAndGenerationValidation(void)
{
  static const uint16_t BadSelectors[] = { 0U, 1U, 4U, 6U, 7U };
  MTK_UFS_POWER_REQUEST R;
  unsigned I;
  for (I = 0; I < sizeof(BadSelectors) / sizeof(BadSelectors[0]); I++) {
    ResetModel();
    M.Pmic[VEMC_VOLT_REG] = (uint16_t)(BadSelectors[I] << 8);
    InitRequest(&R, MtkUfsPowerQuery, 0U, 0U);
    CHECK(Drive(&R) != MtkUfsPowerSuccess);
    CHECK(Core.State != MtkUfsPowerActive && AllGatesOn());
    CHECK(M.SmcCalls == 0U && M.GateWrites == 0U && M.Pmic[VEMC_ENABLE_REG] == 1U);
  }
  ResetModel();
  CHECK(StartQuery(&R) == MtkUfsPowerSuccess);
  InitRequest(&R, MtkUfsPowerPrepareStop, Core.Generation + 1U, MTK_UFS_POWER_DEVICE_DOWN_ACKED);
  CHECK(MtkUfsPowerCoreHandle(&Core, &Io, &R) == MtkUfsPowerStaleGeneration);
  CHECK(M.UicWrites == 0U && M.SmcCalls == 0U && Core.State == MtkUfsPowerActive);
  InitRequest(&R, MtkUfsPowerPrepareStop, Core.Generation, 0U);
  CHECK(MtkUfsPowerCoreHandle(&Core, &Io, &R) == MtkUfsPowerInvalid);
  CHECK(M.UicWrites == 0U && Core.State == MtkUfsPowerActive);

  {
    uint64_t OldGeneration = Core.Generation;
    InitRequest(&R, MtkUfsPowerPrepareStop, OldGeneration, MTK_UFS_POWER_DEVICE_DOWN_ACKED);
    CHECK(Drive(&R) == MtkUfsPowerSuccess);
    CHECK(Core.State == MtkUfsPowerStopPrepared && M.UicWrites == 2U);
    InitRequest(&R, MtkUfsPowerPrepareStop, OldGeneration, MTK_UFS_POWER_DEVICE_DOWN_ACKED);
    CHECK(MtkUfsPowerCoreHandle(&Core, &Io, &R) == MtkUfsPowerStaleGeneration);
    CHECK(Core.State == MtkUfsPowerStopPrepared && M.UicWrites == 2U);
  }
}

static void
TestQueryGateAdmission(void)
{
  unsigned Gate;
  for (Gate = 0; Gate < MTK_UFS_POWER_GATE_COUNT; Gate++) {
    MTK_UFS_POWER_REQUEST R;
    ResetModel();
    M.Reg[MtkUfsPowerBankInfra][GATE_STATUS / 4U] = GateBits[Gate];
    InitRequest(&R, MtkUfsPowerQuery, 0U, 0U);
    CHECK(Drive(&R) != MtkUfsPowerSuccess);
    CHECK(M.HciReadsWithGateOff == 0U);
    CHECK(Core.State != MtkUfsPowerActive && M.GateWrites == 0U && M.SmcCalls == 0U);
    CHECK(M.Pmic[VEMC_ENABLE_REG] == 1U);
  }
  {
    static const uint32_t InvalidParent[] = { 1U << 31, 7U << 24 };
    unsigned I;
    for (I = 0; I < sizeof(InvalidParent) / sizeof(InvalidParent[0]); I++) {
      MTK_UFS_POWER_REQUEST R;
      ResetModel();
      M.Reg[MtkUfsPowerBankTop][0x0D0U / 4U] = InvalidParent[I];
      InitRequest(&R, MtkUfsPowerQuery, 0U, 0U);
      CHECK(Drive(&R) == MtkUfsPowerWrongState);
      CHECK(M.HciReadsWithGateOff == 0U && M.GateWrites == 0U && M.SmcCalls == 0U);
      CHECK(AllGatesOn() && M.Pmic[VEMC_ENABLE_REG] == 1U);
    }
  }
}

static void
TestMutationAndQuerySerialization(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_REQUEST ConcurrentQuery;
  unsigned Commands;
  unsigned Uic;
  ResetModel();
  CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
  InitRequest(&Prepare, MtkUfsPowerPrepareStop, Core.Generation, MTK_UFS_POWER_DEVICE_DOWN_ACKED);
  CHECK(Step(&Prepare) == MtkUfsPowerInProgress);
  CHECK(Core.Operation == MtkUfsPowerPrepareStop);
  Commands = M.CommandWrites;
  Uic = M.UicWrites;
  InitRequest(&ConcurrentQuery, MtkUfsPowerQuery, 0U, 0U);
  CHECK(MtkUfsPowerCoreHandle(&Core, &Io, &ConcurrentQuery) == MtkUfsPowerBusy);
  CHECK(Core.Operation == MtkUfsPowerPrepareStop && !M.Locked);
  CHECK(M.CommandWrites == Commands && M.UicWrites == Uic);
}

static void
TestHibern8MustBeObservedBeforePowerdown(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  unsigned H8Command;
  unsigned ProbeRead = TRACE_CAPACITY;
  unsigned PowerdownArg = TRACE_CAPACITY;
  unsigned Sel;
  unsigned I;
  unsigned TimeoutProbeReads;
  unsigned SelectorCounts[5];
  unsigned TimeoutSelectorTail;
  static const uint32_t DebugOffsets[] = { 0x22C0U, 0x22D0U, 0x22D4U, 0x22D8U, 0x22DCU };
  static const uint32_t DebugValues[] = { 0x00820820U, 0U, 0x55555555U, 0xAAAAAAAAU, UINT32_MAX };

  ResetModel();
  CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
  InitRequest(&Prepare, MtkUfsPowerPrepareStop, Core.Generation, MTK_UFS_POWER_DEVICE_DOWN_ACKED);
  CHECK(Drive(&Prepare) == MtkUfsPowerSuccess);
  CHECK((M.UfsProbeValue >> 28) == 3U);
  H8Command = FindWrite(0U, MtkUfsPowerBankHci, 0x090U, 0x17U);
  CHECK(H8Command < M.TraceCount);
  for (I = H8Command + 1U; I < M.TraceCount; I++) {
    if (M.Trace[I].Kind == 'R' && M.Trace[I].Bank == MtkUfsPowerBankHci && M.Trace[I].Offset == 0x22C8U &&
        (M.Trace[I].Value >> 28) == 3U) { ProbeRead = I; break; }
  }
  for (I = 0; I < M.TraceCount; I++) {
    if (M.Trace[I].Kind == 'W' && M.Trace[I].Bank == MtkUfsPowerBankHci &&
        M.Trace[I].Offset == 0x094U && M.Trace[I].Value == (0xD0A8U << 16)) {
      PowerdownArg = I;
      break;
    }
  }
  CHECK(ProbeRead < PowerdownArg && PowerdownArg < M.TraceCount);
  for (Sel = 0; Sel < sizeof(DebugOffsets) / sizeof(DebugOffsets[0]); Sel++) {
    unsigned Index = FindWrite(H8Command + 1U, MtkUfsPowerBankHci, DebugOffsets[Sel], DebugValues[Sel]);
    CHECK(Index < ProbeRead);
    CHECK(FindWrite(Index + 1U, MtkUfsPowerBankHci, DebugOffsets[Sel], DebugValues[Sel]) == TRACE_CAPACITY);
  }

  ResetModel();
  CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
  M.HoldH8Probe = 1;
  InitRequest(&Prepare, MtkUfsPowerPrepareStop, Core.Generation, MTK_UFS_POWER_DEVICE_DOWN_ACKED);
  CHECK(Drive(&Prepare) == MtkUfsPowerTimeout);
  CHECK(Core.State != MtkUfsPowerStopPrepared && (M.UfsProbeValue >> 28) == 2U);
  memset(SelectorCounts, 0, sizeof(SelectorCounts));
  TimeoutProbeReads = 0U;
  TimeoutSelectorTail = 0U;
  H8Command = FindWrite(0U, MtkUfsPowerBankHci, 0x090U, 0x17U);
  CHECK(H8Command < M.TraceCount);
  {
    unsigned NextSelector = 0U;
    for (I = H8Command + 1U; I < M.TraceCount; I++) {
      if (M.Trace[I].Kind == 'W' && M.Trace[I].Bank == MtkUfsPowerBankHci) {
        for (Sel = 0; Sel < sizeof(DebugOffsets) / sizeof(DebugOffsets[0]); Sel++) {
          if (M.Trace[I].Offset == DebugOffsets[Sel]) {
            CHECK(Sel == NextSelector && M.Trace[I].Value == DebugValues[Sel]);
            SelectorCounts[Sel]++;
            NextSelector++;
            break;
          }
        }
        if (M.Trace[I].Offset == 0x22DCU) {
          CHECK(NextSelector == sizeof(DebugOffsets) / sizeof(DebugOffsets[0]));
        }
      }
      if (M.Trace[I].Kind == 'R' && M.Trace[I].Bank == MtkUfsPowerBankHci &&
          M.Trace[I].Offset == 0x22C8U) {
        CHECK(NextSelector == sizeof(DebugOffsets) / sizeof(DebugOffsets[0]));
        NextSelector = 0U;
        TimeoutProbeReads++;
      }
      CHECK(!(M.Trace[I].Kind == 'W' && M.Trace[I].Bank == MtkUfsPowerBankHci &&
              M.Trace[I].Offset == 0x094U && M.Trace[I].Value == (0xD0A8U << 16)));
    }
    /* Timeout may be reached in PrepareHibernateWait after the selectors
     * were programmed but before that call reads the probe. Earlier complete
     * probe samples must still consume a full selector sequence. */
    CHECK(NextSelector == 0U || NextSelector == sizeof(DebugOffsets) / sizeof(DebugOffsets[0]));
    TimeoutSelectorTail = NextSelector;
  }
  CHECK(TimeoutProbeReads > 1U && M.UicWrites == 1U);
  for (Sel = 0; Sel < sizeof(DebugOffsets) / sizeof(DebugOffsets[0]); Sel++) {
    CHECK(SelectorCounts[Sel] == TimeoutProbeReads +
          (TimeoutSelectorTail == sizeof(DebugOffsets) / sizeof(DebugOffsets[0]) ? 1U : 0U));
  }
  CHECK(AllGatesOn() && M.Pmic[VEMC_ENABLE_REG] == 1U);
}

static void
TestPrepareReleaseAndRestore(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_REQUEST Release;
  MTK_UFS_POWER_REQUEST Restore;
  uint16_t SavedVemc;
  uint16_t SavedVoltage;
  unsigned FirstOff;
  unsigned ReleaseEnd;
  unsigned RefPre;
  unsigned RefWrite;
  unsigned RefAck;
  unsigned RefPost;
  unsigned RestoreStart;
  unsigned RefOnPre;
  unsigned RefOnWrite;
  unsigned RefOnAck;
  unsigned RefOnPost;
  unsigned I;
  ResetModel();
  M.Reg[MtkUfsPowerBankInfra][GATE_STATUS / 4U] = 1U << 6;
  M.Reg[MtkUfsPowerBankTop][0x100U / 4U] = 0xA5C39E71U;
  PrepareAndStopHost(&Query, &Prepare);
  CHECK(M.UicWrites >= 2U);
  SavedVemc = M.Pmic[VEMC_ENABLE_REG];
  SavedVoltage = M.Pmic[VEMC_VOLT_REG];
  CHECK(StartRelease(&Release) == MtkUfsPowerSuccess);
  CHECK(Core.State == MtkUfsPowerReleased && M.GateWrites == MTK_UFS_POWER_GATE_COUNT);
  CHECK((M.Reg[MtkUfsPowerBankInfra][GATE_STATUS / 4U] & GATE_MASK) == GATE_MASK);
  CHECK(Core.GateOwnedMask == GATE_MASK);
  CHECK(M.Pmic[VEMC_ENABLE_REG] == SavedVemc && M.Pmic[VEMC_VOLT_REG] == SavedVoltage);
  RefPre = FindEvent(0U, 'S', 0U, 8U, 0U);
  RefWrite = FindEvent(RefPre + 1U, 'W', MtkUfsPowerBankHci, 0x144U, 0U);
  RefAck = FindEvent(RefWrite + 1U, 'R', MtkUfsPowerBankHci, 0x144U, 0U);
  RefPost = FindEvent(RefAck + 1U, 'S', 0U, 8U, 0x10000U);
  CHECK(RefPre < RefWrite && RefWrite < RefAck && RefAck < RefPost);
  CHECK(M.Trace[RefWrite].TimeUs >= M.Trace[RefPre].TimeUs + 100U);
  CHECK(M.Reg[MtkUfsPowerBankTop][0x100U / 4U] == 0xA5C39E71U && M.HciReadsWithGateOff == 0U);
  CHECK((M.Reg[MtkUfsPowerBankInfra][GATE_STATUS / 4U] & (1U << 6)) != 0U);
  FirstOff = TRACE_CAPACITY;
  for (I = 0; I < M.TraceCount; I++) {
    if (M.Trace[I].Kind == 'W' && M.Trace[I].Bank == MtkUfsPowerBankInfra &&
        M.Trace[I].Offset == GATE_DISABLE_SET) { FirstOff = I; break; }
  }
  CHECK(FirstOff < M.TraceCount);
  CHECK(RefPost < FirstOff);
  for (I = 0; I < MTK_UFS_POWER_ROOT_COUNT; I++) {
    unsigned J;
    int Seen = 0;
    for (J = 0; J < FirstOff; J++) {
      if (M.Trace[J].Kind == 'R' && M.Trace[J].Bank == MtkUfsPowerBankHci &&
          M.Trace[J].Offset == RootOffsets[I] && M.Trace[J].Value == 0U) Seen = 1;
    }
    CHECK(Seen);
  }
  ReleaseEnd = M.TraceCount;
  for (I = FirstOff + 1U; I < ReleaseEnd; I++) {
    CHECK(!(M.Trace[I].Kind == 'R' && M.Trace[I].Bank == MtkUfsPowerBankHci));
  }

  I = M.TraceCount;
  CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
  CHECK(Core.State == MtkUfsPowerReleased && M.HciReadsWithGateOff == 0U);
  for (; I < M.TraceCount; I++) {
    CHECK(!(M.Trace[I].Kind == 'R' && M.Trace[I].Bank == MtkUfsPowerBankHci));
  }

  RestoreStart = M.TraceCount;
  CHECK(StartRestore(&Restore) == MtkUfsPowerSuccess);
  RefOnPre = FindEvent(RestoreStart, 'S', 0U, 8U, 1U);
  RefOnWrite = FindEvent(RefOnPre + 1U, 'W', MtkUfsPowerBankHci, 0x144U, 1U);
  RefOnAck = FindEvent(RefOnWrite + 1U, 'R', MtkUfsPowerBankHci, 0x144U, 3U);
  RefOnPost = FindEvent(RefOnAck + 1U, 'S', 0U, 8U, 0x10001U);
  CHECK(RefOnPre < RefOnWrite && RefOnWrite < RefOnAck && RefOnAck < RefOnPost);
  CHECK(M.Trace[RefOnPre].TimeUs <= M.Trace[RefOnWrite].TimeUs);
  CHECK(M.Trace[RefOnPost].TimeUs >= M.Trace[RefOnAck].TimeUs + 32U);
  CHECK(Core.State == MtkUfsPowerActive && AllGatesOn());
  CHECK(M.HciReadsWithGateOff == 0U);
  CHECK(M.Pmic[VEMC_ENABLE_REG] == SavedVemc && M.Pmic[VEMC_VOLT_REG] == SavedVoltage);
  CheckRestoreUicSequence(RestoreStart);
  CheckRestoreResetAndHceTrace(RestoreStart);
  CHECK(M.Reg[MtkUfsPowerBankHci][0x034U / 4U] == 1U);
  CHECK(M.Reg[MtkUfsPowerBankTop][0x100U / 4U] == 0xA5C39E71U);
  CHECK((M.Reg[MtkUfsPowerBankInfra][GATE_STATUS / 4U] & (1U << 6)) != 0U);
}

static void
TestRootProofBeforeRelease(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_REQUEST Release;
  unsigned Root;
  for (Root = 0; Root < MTK_UFS_POWER_ROOT_COUNT; Root++) {
    unsigned I;
    unsigned GateWritesBefore;
    unsigned SmcBefore;
    ResetModel();
    PrepareAndStopHost(&Query, &Prepare);
    M.Reg[MtkUfsPowerBankHci][RootOffsets[Root] / 4U] = 0x80000000U;
    GateWritesBefore = M.GateWrites;
    SmcBefore = M.SmcCalls;
    InitRequest(&Release, MtkUfsPowerRelease, Core.Generation, 0U);
    CHECK(Drive(&Release) == MtkUfsPowerBusy);
    CHECK(Core.State == MtkUfsPowerStopPrepared);
    CHECK(M.GateWrites == GateWritesBefore && M.SmcCalls == SmcBefore);
    CHECK(AllGatesOn() && M.Pmic[VEMC_ENABLE_REG] == 1U);
    for (I = 0; I < M.TraceCount; I++) {
      CHECK(!(M.Trace[I].Kind == 'W' && M.Trace[I].Bank == MtkUfsPowerBankInfra &&
              (M.Trace[I].Offset == GATE_DISABLE_SET || M.Trace[I].Offset == GATE_ENABLE_CLR)));
    }
  }
}

static void
TestRootReadErrorsBeforeRelease(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_REQUEST Release;
  unsigned Root;
  for (Root = 0; Root < MTK_UFS_POWER_ROOT_COUNT; Root++) {
    ResetModel();
    PrepareAndStopHost(&Query, &Prepare);
    M.FailedReadBank = MtkUfsPowerBankHci;
    M.FailedReadOffset = RootOffsets[Root];
    M.FailReadOnce = 1;
    InitRequest(&Release, MtkUfsPowerRelease, Core.Generation, 0U);
    CHECK(Drive(&Release) == MtkUfsPowerIoError);
    CHECK(M.FailedReadCount == 1U && Core.State == MtkUfsPowerStopPrepared);
    CHECK(Core.Operation == MtkUfsPowerRelease && Core.Phase == MtkUfsPowerPhaseReleaseRoots &&
          Core.RootIndex == Root + 2U && Core.GateOwnedMask == GATE_MASK);
    CHECK(M.GateWrites == 0U && M.SmcCalls == 0U && AllGatesOn());
    CHECK(M.Pmic[VEMC_ENABLE_REG] == 1U);
  }
}

static void
TestReleaseRejectsLostTopParent(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_REQUEST Release;
  ResetModel();
  PrepareAndStopHost(&Query, &Prepare);
  M.Reg[MtkUfsPowerBankTop][0x0D0U / 4U] = 1U << 31;
  InitRequest(&Release, MtkUfsPowerRelease, Core.Generation, 0U);
  CHECK(Drive(&Release) == MtkUfsPowerWrongState);
  CHECK(M.GateWrites == 0U && M.SmcCalls == 0U && AllGatesOn());
  CHECK(Core.State == MtkUfsPowerStopPrepared && M.Pmic[VEMC_ENABLE_REG] == 1U);
}

static void
TestReleaseRetryAndUnknownSmc(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_REQUEST Release;
  unsigned I;
  ResetModel();
  PrepareAndStopHost(&Query, &Prepare);
  M.FailedWriteBank = MtkUfsPowerBankInfra;
  M.FailedWriteOffset = GATE_DISABLE_SET;
  M.FailedWriteValue = GateBits[2];
  M.FailWriteOnce = 1;
  M.FailWriteAfterEffect = 1;
  InitRequest(&Release, MtkUfsPowerRelease, Core.Generation, 0U);
  CHECK(Drive(&Release) != MtkUfsPowerSuccess);
  CHECK(M.FailedWriteCount == 1U && M.HciReadsWithGateOff == 0U);
  CHECK((M.Reg[MtkUfsPowerBankInfra][GATE_STATUS / 4U] & GateBits[2]) != 0U);
  CHECK((Core.GateOwnedMask & GATE_MASK) != 0U);
  CHECK(M.Pmic[VEMC_ENABLE_REG] == 1U);
  I = M.ReadCalls;
  CHECK(Drive(&Release) == MtkUfsPowerSuccess);
  CHECK(M.HciReadsWithGateOff == 0U && M.ReadCalls >= I);
  CHECK(Core.State == MtkUfsPowerReleased &&
        (M.Reg[MtkUfsPowerBankInfra][GATE_STATUS / 4U] & GATE_MASK) == GATE_MASK);

  ResetModel();
  PrepareAndStopHost(&Query, &Prepare);
  M.SmcRawResult = -3; /* Only zero and exact -1 are accepted by this platform. */
  InitRequest(&Release, MtkUfsPowerRelease, Core.Generation, 0U);
  CHECK(Drive(&Release) == MtkUfsPowerIoError);
  CHECK(M.SmcCalls == 1U && M.GateWrites == 0U && AllGatesOn());
  CHECK(Core.GateOwnedMask == GATE_MASK && M.Pmic[VEMC_ENABLE_REG] == 1U);
  CHECK(Core.State == MtkUfsPowerFault && Core.LastError == MtkUfsPowerIoError);
  I = M.WriteCalls;
  CHECK(Drive(&Release) == MtkUfsPowerWrongState && M.SmcCalls == 1U);
  CHECK(M.WriteCalls == I && M.GateWrites == 0U && AllGatesOn());
  CHECK(Core.State == MtkUfsPowerFault && Core.LastError == MtkUfsPowerIoError);

  ResetModel();
  PrepareAndStopHost(&Query, &Prepare);
  M.SmcRawResult = -1;
  InitRequest(&Release, MtkUfsPowerRelease, Core.Generation, 0U);
  CHECK(Drive(&Release) == MtkUfsPowerSuccess);
  CHECK(M.SmcCalls == 2U && Core.State == MtkUfsPowerReleased);
}

static void
TestDroppedGateWriteReadback(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_REQUEST Release;
  unsigned I;
  ResetModel();
  PrepareAndStopHost(&Query, &Prepare);
  M.DropWriteBank = MtkUfsPowerBankInfra;
  M.DropWriteOffset = GATE_DISABLE_SET;
  M.DropWriteValue = GateBits[4];
  M.DropWriteCount = 1U;
  InitRequest(&Release, MtkUfsPowerRelease, Core.Generation, 0U);
  CHECK(Drive(&Release) == MtkUfsPowerSuccess);
  CHECK(Core.State == MtkUfsPowerReleased && M.HciReadsWithGateOff == 0U);
  CHECK((M.Reg[MtkUfsPowerBankInfra][GATE_STATUS / 4U] & GATE_MASK) == GATE_MASK);
  for (I = 0; I < M.TraceCount; I++) {
    if (M.Trace[I].Kind == 'D') {
      CHECK(M.Trace[I].Bank == MtkUfsPowerBankInfra && M.Trace[I].Offset == GATE_DISABLE_SET);
      break;
    }
  }
  CHECK(I < M.TraceCount);
}

static void
TestRestoreSupplyAndGateOrdering(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_REQUEST Release;
  MTK_UFS_POWER_REQUEST Restore;
  unsigned GateWrites;
  unsigned HciReads;
  unsigned I;
  ResetModel();
  PrepareAndStopHost(&Query, &Prepare);
  CHECK(StartRelease(&Release) == MtkUfsPowerSuccess);
  GateWrites = M.GateWrites;
  HciReads = M.HciReadsWithGateOff;
  M.Pmic[VEMC_ENABLE_REG] = 0U;
  InitRequest(&Restore, MtkUfsPowerRestore, Core.Generation, 0U);
  CHECK(Drive(&Restore) != MtkUfsPowerSuccess);
  CHECK(Core.State == MtkUfsPowerRestorePreparing && M.GateWrites == GateWrites);
  CHECK(M.HciReadsWithGateOff == HciReads && (M.Reg[MtkUfsPowerBankInfra][GATE_STATUS / 4U] & GATE_MASK) == GATE_MASK);
  M.Pmic[VEMC_ENABLE_REG] = 1U;
  M.Pmic[VEMC_VOLT_REG] = 3U << 8;
  CHECK(Drive(&Restore) == MtkUfsPowerSuccess);
  CHECK(Core.State == MtkUfsPowerActive && AllGatesOn() && M.HciReadsWithGateOff == 0U);
  for (I = 0; I < M.TraceCount; I++) {
    if (M.Trace[I].Kind == 'R' && M.Trace[I].Bank == MtkUfsPowerBankHci) {
      CHECK(AllGatesOn());
    }
  }
}

static void
TestRestoreNeedsTenRootStopProof(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_REQUEST Restore;
  unsigned I;
  ResetModel();
  CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
  CHECK(StartPrepare(&Prepare) == MtkUfsPowerSuccess);
  CHECK(Core.State == MtkUfsPowerStopPrepared && M.Reg[MtkUfsPowerBankHci][0x034U / 4U] == 1U);
  InitRequest(&Restore, MtkUfsPowerRestore, Core.Generation, 0U);
  CHECK(Drive(&Restore) == MtkUfsPowerBusy);
  CHECK(Core.State == MtkUfsPowerRestorePreparing && M.SmcCalls == 0U && M.GateWrites == 0U);
  CHECK(AllGatesOn() && M.Reg[MtkUfsPowerBankHci][0x034U / 4U] == 1U);
  for (I = 0; I < M.TraceCount; I++) {
    CHECK(!(M.Trace[I].Kind == 'W' && M.Trace[I].Bank == MtkUfsPowerBankInfra &&
            (M.Trace[I].Offset == 0x130U || M.Trace[I].Offset == 0x134U ||
             M.Trace[I].Offset == 0x140U || M.Trace[I].Offset == 0x144U ||
             M.Trace[I].Offset == 0x150U || M.Trace[I].Offset == 0x154U)));
  }
  SetRoots(0U);
  CHECK(Drive(&Restore) == MtkUfsPowerSuccess);
  CHECK(Core.State == MtkUfsPowerActive && AllGatesOn());
}

static void
TestUicCommandNotResent(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_REQUEST R;
  unsigned I;
  ResetModel();
  CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
  M.HoldUic = 1;
  InitRequest(&R, MtkUfsPowerPrepareStop, Core.Generation, MTK_UFS_POWER_DEVICE_DOWN_ACKED);
  for (I = 0; I < MAX_STEPS; I++) {
    MTK_UFS_POWER_RESULT S = Step(&R);
    if (M.UicWrites != 0U) {
      CHECK(S == MtkUfsPowerInProgress || S == MtkUfsPowerBusy);
      break;
    }
    CHECK(S == MtkUfsPowerInProgress);
  }
  CHECK(M.UicWrites == 1U);
  for (I = 0; I < 8U; I++) {
    MTK_UFS_POWER_RESULT S = Step(&R);
    CHECK(S == MtkUfsPowerInProgress || S == MtkUfsPowerBusy);
    CHECK(M.UicWrites == 1U);
  }
  M.HoldUic = 0;
  M.Reg[MtkUfsPowerBankHci][0x020U / 4U] |= UIC_COMPLETE;
  CHECK(Drive(&R) == MtkUfsPowerSuccess);
  CHECK(Core.State == MtkUfsPowerStopPrepared && M.UicWrites == 2U);
  (void)Prepare;
}

static void
TestFrozenClockBoundsUicPolling(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  ResetModel();
  CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
  M.AdvanceTime = 0;
  M.HoldUic = 1;
  InitRequest(&Prepare, MtkUfsPowerPrepareStop, Core.Generation, MTK_UFS_POWER_DEVICE_DOWN_ACKED);
  CHECK(Drive(&Prepare) == MtkUfsPowerTimeout);
  CHECK(M.UicWrites == 1U && Core.UicPollCount != 0U);
  CHECK(Core.State != MtkUfsPowerStopPrepared);
  CHECK(AllGatesOn() && M.Pmic[VEMC_ENABLE_REG] == 1U);
}

static void
TestFrozenClockBoundsWacsPolling(void)
{
  MTK_UFS_POWER_REQUEST Query;
  ResetModel();
  M.AdvanceTime = 0;
  M.WacsDelayValid = 1;
  InitRequest(&Query, MtkUfsPowerQuery, 0U, 0U);
  CHECK(Step(&Query) == MtkUfsPowerInProgress);
  CHECK(M.CommandWrites == 1U);
  M.WacsFsm = 1;
  CHECK(Drive(&Query) == MtkUfsPowerTimeout);
  CHECK(M.CommandWrites == 1U && M.ValidClearWrites == 0U);
  CHECK(Core.State != MtkUfsPowerActive && M.GateWrites == 0U && M.SmcCalls == 0U);
  CHECK(M.Pmic[VEMC_ENABLE_REG] == 1U);
}

static unsigned
CountWritesToBank(unsigned Start, uint32_t Bank)
{
  unsigned I;
  unsigned Count = 0U;
  for (I = Start; I < M.TraceCount; I++) {
    if ((M.Trace[I].Kind == 'W' || M.Trace[I].Kind == 'D' || M.Trace[I].Kind == 'E') &&
        M.Trace[I].Bank == Bank) Count++;
  }
  return Count;
}

static void
TestStopPreparedQueryAcrossHostStop(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  uint64_t StopGeneration;
  uint32_t OwnedMask;
  uint16_t VemcEnable;
  uint16_t VemcVoltage;
  unsigned TraceStart;
  unsigned Root;

  /* Before the upper HCI driver stops, StopPrepared is represented by a
   * live HCE with idle doorbells and the previously observed Hibern8 state. */
  ResetModel();
  CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
  CHECK(StartPrepare(&Prepare) == MtkUfsPowerSuccess);
  CHECK(Core.State == MtkUfsPowerStopPrepared &&
        M.Reg[MtkUfsPowerBankHci][0x034U / 4U] == 1U &&
        M.Reg[MtkUfsPowerBankHci][0x058U / 4U] == 0U &&
        M.Reg[MtkUfsPowerBankHci][0x078U / 4U] == 0U &&
        (M.Reg[MtkUfsPowerBankHci][0x144U / 4U] & 3U) == 3U &&
        (M.UfsProbeValue >> 28) == 3U);
  StopGeneration = Core.Generation;
  OwnedMask = Core.GateOwnedMask;
  VemcEnable = M.Pmic[VEMC_ENABLE_REG];
  VemcVoltage = M.Pmic[VEMC_VOLT_REG];
  TraceStart = M.TraceCount;
  InitRequest(&Query, MtkUfsPowerQuery, 0U, 0U);
  CHECK(Drive(&Query) == MtkUfsPowerSuccess);
  CHECK(Query.State == MtkUfsPowerStopPrepared &&
        Query.Generation == StopGeneration && Query.Result == MtkUfsPowerSuccess);
  CHECK(Core.State == MtkUfsPowerStopPrepared && Core.Generation == StopGeneration &&
        Core.GateOwnedMask == OwnedMask && Core.Phase == MtkUfsPowerPhaseIdle);
  CHECK(CountWritesToBank(TraceStart, MtkUfsPowerBankHci) == 0U &&
        CountWritesToBank(TraceStart, MtkUfsPowerBankInfra) == 0U &&
        CountWritesToBank(TraceStart, MtkUfsPowerBankTop) == 0U &&
        M.SmcCalls == 0U);
  CHECK(M.Pmic[VEMC_ENABLE_REG] == VemcEnable && M.Pmic[VEMC_VOLT_REG] == VemcVoltage);

  /* After the pass-through owner stops the controller, require the full
   * ten-register zero proof while retaining exactly the same generation. */
  ResetModel();
  CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
  CHECK(StartPrepare(&Prepare) == MtkUfsPowerSuccess);
  SetRoots(0U);
  CHECK(Core.State == MtkUfsPowerStopPrepared &&
        M.Reg[MtkUfsPowerBankHci][0x034U / 4U] == 0U);
  StopGeneration = Core.Generation;
  OwnedMask = Core.GateOwnedMask;
  VemcEnable = M.Pmic[VEMC_ENABLE_REG];
  VemcVoltage = M.Pmic[VEMC_VOLT_REG];
  TraceStart = M.TraceCount;
  InitRequest(&Query, MtkUfsPowerQuery, 0U, 0U);
  CHECK(Drive(&Query) == MtkUfsPowerSuccess);
  CHECK(Query.State == MtkUfsPowerStopPrepared && Query.Generation == StopGeneration);
  CHECK(Core.State == MtkUfsPowerStopPrepared && Core.Generation == StopGeneration &&
        Core.GateOwnedMask == OwnedMask && Core.Phase == MtkUfsPowerPhaseIdle);
  CHECK(CountWritesToBank(TraceStart, MtkUfsPowerBankHci) == 0U &&
        CountWritesToBank(TraceStart, MtkUfsPowerBankInfra) == 0U &&
        CountWritesToBank(TraceStart, MtkUfsPowerBankTop) == 0U &&
        M.SmcCalls == 0U);
  CHECK(M.Pmic[VEMC_ENABLE_REG] == VemcEnable && M.Pmic[VEMC_VOLT_REG] == VemcVoltage);

  for (Root = 1U; Root < MTK_UFS_POWER_ROOT_COUNT; Root++) {
    M.Reg[MtkUfsPowerBankHci][RootOffsets[Root] / 4U] = 0x80000000U;
    TraceStart = M.TraceCount;
    InitRequest(&Query, MtkUfsPowerQuery, 0U, 0U);
    CHECK(Drive(&Query) != MtkUfsPowerSuccess);
    CHECK(Core.State == MtkUfsPowerStopPrepared && Core.Generation == StopGeneration &&
          Core.GateOwnedMask == OwnedMask);
    CHECK(CountWritesToBank(TraceStart, MtkUfsPowerBankHci) == 0U &&
          CountWritesToBank(TraceStart, MtkUfsPowerBankInfra) == 0U &&
          CountWritesToBank(TraceStart, MtkUfsPowerBankTop) == 0U &&
          M.SmcCalls == 0U && M.UicWrites == 2U);
    M.Reg[MtkUfsPowerBankHci][RootOffsets[Root] / 4U] = 0U;
    InitRequest(&Query, MtkUfsPowerQuery, 0U, 0U);
    CHECK(Drive(&Query) == MtkUfsPowerSuccess);
    CHECK(Query.State == MtkUfsPowerStopPrepared && Query.Generation == StopGeneration);
  }
}

static unsigned
CountSmcEvent(unsigned Start, uint32_t Command, uint32_t EncodedValue)
{
  unsigned I;
  unsigned Count = 0U;
  for (I = Start; I < M.TraceCount; I++) {
    if (M.Trace[I].Kind == 'S' && M.Trace[I].Bank == 0U &&
        M.Trace[I].Offset == Command && M.Trace[I].Value == EncodedValue) Count++;
  }
  return Count;
}

static void
AdvanceRestoreToPhase(
  MTK_UFS_POWER_REQUEST *Restore,
  MTK_UFS_POWER_PHASE Target
  )
{
  unsigned I;
  MTK_UFS_POWER_RESULT Status;
  for (I = 0; I < MAX_STEPS && Core.Phase != (uint32_t)Target; I++) {
    Status = Step(Restore);
    CHECK(Status == MtkUfsPowerInProgress || Status == MtkUfsPowerBusy);
  }
  CHECK(Core.Phase == (uint32_t)Target);
}

static void
AdvanceReleaseToPhase(
  MTK_UFS_POWER_REQUEST *Release,
  MTK_UFS_POWER_PHASE Target
  )
{
  unsigned I;
  MTK_UFS_POWER_RESULT Status;
  for (I = 0; I < MAX_STEPS && Core.Phase != (uint32_t)Target; I++) {
    Status = Step(Release);
    CHECK(Status == MtkUfsPowerInProgress || Status == MtkUfsPowerBusy);
  }
  CHECK(Core.Phase == (uint32_t)Target);
}

static void
CheckRestoreBusyDoesNotChangeJournal(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_REQUEST Release;
  MTK_UFS_POWER_REQUEST Restore;
  MTK_UFS_POWER_REQUEST Concurrent;
  MTK_UFS_POWER_CORE SavedCore;
  unsigned SavedReads;
  unsigned SavedWrites;
  unsigned SavedSmc;
  unsigned SavedGates;
  /* Observe the exact before/after phases around the one-shot refclock SMCs. */
  uint32_t TargetPhases[] = {
    MtkUfsPowerPhaseRestoreRefSmcPre,
    MtkUfsPowerPhaseRestoreRefSmcPost
  };
  unsigned I;

  for (I = 0; I < sizeof(TargetPhases) / sizeof(TargetPhases[0]); I++) {
    uint32_t Target = TargetPhases[I];
    ResetModel();
    PrepareAndStopHost(&Query, &Prepare);
    CHECK(StartRelease(&Release) == MtkUfsPowerSuccess);
    InitRequest(&Restore, MtkUfsPowerRestore, Core.Generation, 0U);
    AdvanceRestoreToPhase(&Restore, (MTK_UFS_POWER_PHASE)Target);

    SavedCore = Core;
    SavedReads = M.ReadCalls;
    SavedWrites = M.WriteCalls;
    SavedSmc = M.SmcCalls;
    SavedGates = M.GateWrites;
    {
      unsigned LocksBefore = M.LockCalls;
      unsigned UnlocksBefore = M.UnlockCalls;
      InitRequest(&Concurrent, MtkUfsPowerQuery, 0U, 0U);
      CHECK(MtkUfsPowerCoreHandle(&Core, &Io, &Concurrent) == MtkUfsPowerBusy);
      CHECK(M.LockCalls == LocksBefore + 1U && M.UnlockCalls == UnlocksBefore + 1U);
    }
    CHECK(memcmp(&Core, &SavedCore, sizeof(Core)) == 0);
    CHECK(M.ReadCalls == SavedReads && M.WriteCalls == SavedWrites &&
          M.SmcCalls == SavedSmc && M.GateWrites == SavedGates);

    if (Target == MtkUfsPowerPhaseRestoreRefSmcPre) {
      unsigned Before = M.SmcCalls;
      CHECK(Core.RootIndex == 0U);
      CHECK(Step(&Restore) == MtkUfsPowerInProgress);
      CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefWrite && Core.RootIndex == 1U);
      CHECK(M.SmcCalls == Before + 1U);
      CHECK(CountSmcEvent(0U, 8U, 1U) == 1U); /* refclock on, pre */
      CHECK(Step(&Restore) == MtkUfsPowerInProgress);
      CHECK(M.SmcCalls == Before + 1U);
      CHECK(CountSmcEvent(0U, 8U, 1U) == 1U);
    } else {
      unsigned Before = M.SmcCalls;
      CHECK(Step(&Restore) == MtkUfsPowerInProgress);
      CHECK(Core.Phase == MtkUfsPowerPhaseRestorePreHce);
      CHECK(M.SmcCalls == Before + 1U);
      CHECK(CountSmcEvent(0U, 8U, 0x10001U) == 1U); /* refclock on, post */
      CHECK(Step(&Restore) == MtkUfsPowerInProgress);
      CHECK(M.SmcCalls == Before + 1U); /* reset assert must precede refclock-on */
      CHECK(CountSmcEvent(0U, 8U, 0x10001U) == 1U);
      CHECK(Drive(&Restore) == MtkUfsPowerSuccess);
      CHECK(CountSmcEvent(0U, 8U, 1U) == 1U);
      CHECK(CountSmcEvent(0U, 8U, 0x10001U) == 1U);
      CHECK(Core.State == MtkUfsPowerActive && AllGatesOn());
    }
  }
}

static void
TestRestoreRefclockBusyAndSmcCompletion(void)
{
  CheckRestoreBusyDoesNotChangeJournal();
}

static void
TestRestoreFromStopPreparedSequence(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_REQUEST Restore;
  unsigned TraceStart;
  unsigned OffPre;
  unsigned OffWrite;
  unsigned OffAck;
  unsigned OffPost;
  unsigned ResetAssert;
  unsigned HceOff;
  unsigned ResetRelease;
  unsigned OnPre;
  unsigned OnWrite;
  unsigned OnAck;
  unsigned OnPost;
  unsigned HciSet;
  unsigned I;

  ResetModel();
  CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
  CHECK(StartPrepare(&Prepare) == MtkUfsPowerSuccess);
  SetRoots(0U);
  CHECK(M.Reg[MtkUfsPowerBankHci][0x144U / 4U] == 3U);
  TraceStart = M.TraceCount;
  CHECK(StartRestore(&Restore) == MtkUfsPowerSuccess);

  OffPre = FindEvent(TraceStart, 'S', 0U, 8U, 0U);
  OffWrite = FindWrite(OffPre + 1U, MtkUfsPowerBankHci, 0x144U, 0U);
  OffAck = FindEvent(OffWrite + 1U, 'R', MtkUfsPowerBankHci, 0x144U, 0U);
  OffPost = FindEvent(OffAck + 1U, 'S', 0U, 8U, 0x10000U);
  ResetAssert = FindEvent(OffPost + 1U, 'S', 0U, 2U, 0U);
  HceOff = FindWrite(ResetAssert + 1U, MtkUfsPowerBankHci, 0x034U, 0U);
  ResetRelease = FindEvent(HceOff + 1U, 'S', 0U, 2U, 1U);
  OnPre = FindEvent(ResetRelease + 1U, 'S', 0U, 8U, 1U);
  OnWrite = FindWrite(OnPre + 1U, MtkUfsPowerBankHci, 0x144U, 1U);
  OnAck = FindEvent(OnWrite + 1U, 'R', MtkUfsPowerBankHci, 0x144U, 3U);
  OnPost = FindEvent(OnAck + 1U, 'S', 0U, 8U, 0x10001U);
  HciSet = FindWrite(OnPost + 1U, MtkUfsPowerBankInfra, 0x130U, 1U << 15);

  CHECK(OffPre < OffWrite && OffWrite < OffAck && OffAck < OffPost &&
        OffPost < ResetAssert && ResetAssert < HceOff && HceOff < ResetRelease &&
        ResetRelease < OnPre && OnPre < OnWrite && OnWrite < OnAck &&
        OnAck < OnPost && OnPost < HciSet);
  for (I = 0; I < MTK_UFS_POWER_ROOT_COUNT; I++) {
    CHECK(FindEvent(TraceStart, 'R', MtkUfsPowerBankHci, RootOffsets[I], 0U) < OffPre);
  }
  CHECK(M.Trace[OffWrite].TimeUs >= M.Trace[OffPre].TimeUs + 100U);
  CHECK(M.Trace[OnPost].TimeUs >= M.Trace[OnAck].TimeUs + 32U);
  CHECK(M.Trace[ResetRelease].TimeUs >= M.Trace[HceOff].TimeUs + 10U);
  CHECK(M.Trace[HciSet].TimeUs >= M.Trace[ResetRelease].TimeUs + 10000U);
  CHECK(Core.State == MtkUfsPowerActive && AllGatesOn());
}

static void
TestUicSetOutputDoesNotReplacePendingInput(void)
{
  static const unsigned BusyCommandDeltas[] = { 4U, 9U };
  static const uint32_t BusyInputArg3[] = { 0xA5A500B0U, 0xA5A500FDU };
  unsigned Scenario;
  for (Scenario = 0; Scenario < sizeof(BusyCommandDeltas) / sizeof(BusyCommandDeltas[0]); Scenario++) {
    MTK_UFS_POWER_REQUEST Query;
    MTK_UFS_POWER_REQUEST Prepare;
    MTK_UFS_POWER_REQUEST Release;
    MTK_UFS_POWER_REQUEST Restore;
    unsigned TraceStart;
    ResetModel();
    PrepareAndStopHost(&Query, &Prepare);
    CHECK(StartRelease(&Release) == MtkUfsPowerSuccess);
    M.UicSetArg3ReturnZero = 1;
    M.UicClearBusyAfterEffectOnce = 1;
    M.UicClearBusyAtCommand = M.UicWrites + BusyCommandDeltas[Scenario];
    TraceStart = M.TraceCount;
    CHECK(StartRestore(&Restore) == MtkUfsPowerIoError);
    CHECK(Core.State == MtkUfsPowerRestorePreparing && Core.Operation == MtkUfsPowerRestore &&
          Core.UicPending == 2U && Core.UicCommand == 2U && Core.UicArg1 == 0xD0A60000U &&
          Core.UicInputArg3 == BusyInputArg3[Scenario] && Core.UicResultArg3 == 0U);
    {
      unsigned WritesAtBusy = M.UicWrites;
      CHECK(Step(&Restore) == MtkUfsPowerInProgress);
      CHECK(Core.UicPending == 0U && M.UicWrites == WritesAtBusy);
      CHECK(Drive(&Restore) == MtkUfsPowerSuccess);
    }
    CHECK(Core.State == MtkUfsPowerActive && AllGatesOn());
    CHECK(M.UicClearBusyCount == 1U && !M.UicClearBusyAfterEffectOnce);
    CHECK(M.UicWrites == 13U); /* two prepare commands plus 11 for IP 0x10450000 */
    CheckRestoreUicSequence(TraceStart);
  }
}

static void
TestPrepareCheckRetainsOwnedWacsResponse(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_CORE SavedCore;
  unsigned CommandsBefore;
  unsigned ClearsBefore;
  unsigned FailureKind;
  for (FailureKind = 0; FailureKind < 2U; FailureKind++) {
    MTK_UFS_POWER_RESULT Expected = FailureKind == 0U ? MtkUfsPowerIoError : MtkUfsPowerBusy;
    ResetModel();
    CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
    CommandsBefore = M.CommandWrites;
    ClearsBefore = M.ValidClearWrites;
    InitRequest(&Prepare, MtkUfsPowerPrepareStop, Core.Generation, MTK_UFS_POWER_DEVICE_DOWN_ACKED);
    CHECK(Step(&Prepare) == MtkUfsPowerInProgress);
    CHECK(Core.State == MtkUfsPowerStopPreparing &&
          Core.Phase == MtkUfsPowerPhasePrepareCheck && Core.RootIndex == 0U &&
          Core.WacsStage == 1U && M.CommandWrites == CommandsBefore + 1U);

    /* Fail the first observation after this exact WACS read was accepted. The
     * prepare journal must retain it and consume its response on retry. */
    if (FailureKind == 0U) {
      M.FailedReadBank = MtkUfsPowerBankPwrap;
      M.FailedReadOffset = WACS_RDATA;
      M.FailReadOnce = 1;
    } else {
      M.BusyReadBank = MtkUfsPowerBankPwrap;
      M.BusyReadOffset = WACS_RDATA;
      M.BusyReadOnce = 1;
    }
    CHECK(Step(&Prepare) == Expected);
    CHECK((FailureKind == 0U ? M.FailedReadCount : M.BusyReadCount) == 1U &&
          Core.State == MtkUfsPowerStopPreparing &&
          Core.Operation == MtkUfsPowerPrepareStop &&
          Core.Phase == MtkUfsPowerPhasePrepareCheck && Core.RootIndex == 0U &&
          Core.WacsStage == 1U);
    CHECK(M.CommandWrites == CommandsBefore + 1U && M.ValidClearWrites == ClearsBefore);

    /* A fresh Query cannot replace/reset an in-flight WACS owner. */
    SavedCore = Core;
    InitRequest(&Query, MtkUfsPowerQuery, 0U, 0U);
    CHECK(Step(&Query) == MtkUfsPowerBusy);
    CHECK(memcmp(&Core, &SavedCore, sizeof(Core)) == 0);
    CHECK(M.CommandWrites == CommandsBefore + 1U && M.ValidClearWrites == ClearsBefore);

    CHECK(Drive(&Prepare) == MtkUfsPowerSuccess);
    CHECK(Core.State == MtkUfsPowerStopPrepared && Core.WacsStage == 0U);
    CHECK(M.CommandWrites == CommandsBefore + 2U && M.ValidClearWrites == ClearsBefore + 2U);
    CHECK(M.UicWrites == 2U && M.Pmic[VEMC_ENABLE_REG] == 1U);
  }
}

static void
AdvancePrepareToHibernatePhase(MTK_UFS_POWER_REQUEST *Prepare)
{
  unsigned I;
  for (I = 0; I < MAX_STEPS && Core.Phase != MtkUfsPowerPhasePrepareHibernate; I++) {
    CHECK(Step(Prepare) == MtkUfsPowerInProgress);
  }
  CHECK(Core.Phase == MtkUfsPowerPhasePrepareHibernate);
}

static void
TestUicPreSubmitReadinessJournal(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  unsigned TraceStart;
  unsigned I;
  MTK_UFS_POWER_RESULT Status;

  /* Stale UCCS is not ours to clear; retain the pre-submit journal and budget. */
  ResetModel();
  CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
  InitRequest(&Prepare, MtkUfsPowerPrepareStop, Core.Generation, MTK_UFS_POWER_DEVICE_DOWN_ACKED);
  AdvancePrepareToHibernatePhase(&Prepare);
  M.Reg[MtkUfsPowerBankHci][0x020U / 4U] = UIC_COMPLETE;
  TraceStart = M.TraceCount;
  for (I = 0; I < 3U; I++) {
    unsigned PreviousPolls = Core.UicPollCount;
    CHECK(Step(&Prepare) == MtkUfsPowerBusy);
    CHECK(Core.Phase == MtkUfsPowerPhasePrepareHibernate &&
          Core.UicPending == 4U && Core.UicCommand == 0x17U);
    CHECK(Core.UicPollCount >= PreviousPolls && M.UicWrites == 0U);
  }
  CHECK(M.Reg[MtkUfsPowerBankHci][0x020U / 4U] == UIC_COMPLETE);
  CHECK(CountWritesToBank(TraceStart, MtkUfsPowerBankHci) == 0U &&
        CountWritesToBank(TraceStart, MtkUfsPowerBankInfra) == 0U);

  /* UCRDY can remain low after Stop checks; the bounded pre-submit wait must
   * time out without writing ARGs/CMD or clearing another owner's UCCS. */
  ResetModel();
  CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
  M.HoldUicReady = 1;
  InitRequest(&Prepare, MtkUfsPowerPrepareStop, Core.Generation, MTK_UFS_POWER_DEVICE_DOWN_ACKED);
  AdvancePrepareToHibernatePhase(&Prepare);
  M.AdvanceTime = 0;
  TraceStart = M.TraceCount;
  CHECK(Step(&Prepare) == MtkUfsPowerInProgress);
  CHECK(Core.UicPending == 4U && M.UicWrites == 0U);
  Status = MtkUfsPowerInProgress;
  for (I = 0; I < MAX_STEPS && Status == MtkUfsPowerInProgress; I++) {
    Status = Step(&Prepare);
  }
  CHECK(Status == MtkUfsPowerTimeout && Core.State == MtkUfsPowerFault);
  CHECK(Core.UicPending == 4U && Core.UicPollCount == 4097U && M.UicWrites == 0U);
  CHECK(CountWritesToBank(TraceStart, MtkUfsPowerBankHci) == 0U &&
        CountWritesToBank(TraceStart, MtkUfsPowerBankInfra) == 0U &&
        M.SmcCalls == 0U && M.GateWrites == 0U);
}

static void
TestRestoreWaitBoundaries(void)
{
  MTK_UFS_POWER_REQUEST Query;
  MTK_UFS_POWER_REQUEST Prepare;
  MTK_UFS_POWER_REQUEST Release;
  MTK_UFS_POWER_REQUEST Restore;
  uint64_t StartUs;
  unsigned TraceStart;
  unsigned OffPreEvent;
  unsigned OffWriteEvent;
  unsigned I;

  /* The stock refclock-off path waits 100us after the secure pre-call and
   * before the MMIO gate write. Frozen time cannot advance that phase. */
  ResetModel();
  PrepareAndStopHost(&Query, &Prepare);
  InitRequest(&Release, MtkUfsPowerRelease, Core.Generation, 0U);
  AdvanceReleaseToPhase(&Release, MtkUfsPowerPhaseReleaseRefDelayPre);
  StartUs = Core.PhaseStartedUs;
  M.AdvanceTime = 0;
  M.TimeUs = StartUs + 99U;
  TraceStart = M.TraceCount;
  for (I = 0; I < 3U; I++) {
    CHECK(Step(&Release) == MtkUfsPowerInProgress);
    CHECK(Core.Phase == MtkUfsPowerPhaseReleaseRefDelayPre);
  }
  CHECK(CountSmcEvent(TraceStart, 8U, 0x10000U) == 0U);
  CHECK(FindEvent(TraceStart, 'W', MtkUfsPowerBankHci, 0x144U, 0U) == TRACE_CAPACITY);
  M.TimeUs = StartUs + 100U;
  CHECK(Step(&Release) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseReleaseRefWrite);
  CHECK(FindEvent(TraceStart, 'W', MtkUfsPowerBankHci, 0x144U, 0U) == TRACE_CAPACITY);
  CHECK(Step(&Release) == MtkUfsPowerInProgress);
  CHECK(FindEvent(TraceStart, 'W', MtkUfsPowerBankHci, 0x144U, 0U) < M.TraceCount);

  /* A direct StopPrepared restore also has to finish the stock refclock-off
   * pre/wait/write/ack/post sequence before asserting reset. */
  ResetModel();
  CHECK(StartQuery(&Query) == MtkUfsPowerSuccess);
  CHECK(StartPrepare(&Prepare) == MtkUfsPowerSuccess);
  SetRoots(0U);
  InitRequest(&Restore, MtkUfsPowerRestore, Core.Generation, 0U);
  TraceStart = M.TraceCount;
  AdvanceRestoreToPhase(&Restore, MtkUfsPowerPhaseRestoreRefOffDelayPre);
  StartUs = Core.PhaseStartedUs;
  M.AdvanceTime = 0;
  M.TimeUs = StartUs + 99U;
  for (I = 0; I < 3U; I++) {
    CHECK(Step(&Restore) == MtkUfsPowerInProgress);
    CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefOffDelayPre);
  }
  CHECK(FindEvent(TraceStart, 'S', 0U, 8U, 0U) < M.TraceCount);
  CHECK(FindWrite(TraceStart, MtkUfsPowerBankHci, 0x144U, 0U) == TRACE_CAPACITY);
  CHECK(FindEvent(TraceStart, 'S', 0U, 2U, 0U) == TRACE_CAPACITY);
  M.TimeUs = StartUs + 100U;
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefOffWrite);
  CHECK(FindWrite(TraceStart, MtkUfsPowerBankHci, 0x144U, 0U) == TRACE_CAPACITY);
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefOffAck);
  CHECK(FindWrite(TraceStart, MtkUfsPowerBankHci, 0x144U, 0U) < M.TraceCount);
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefOffSmcPost);
  CHECK(FindEvent(TraceStart, 'S', 0U, 2U, 0U) == TRACE_CAPACITY);
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreResetAssert);
  OffPreEvent = FindEvent(TraceStart, 'S', 0U, 8U, 0U);
  OffWriteEvent = FindWrite(TraceStart, MtkUfsPowerBankHci, 0x144U, 0U);
  CHECK(OffPreEvent < OffWriteEvent && OffWriteEvent < M.TraceCount);
  CHECK(M.Trace[OffWriteEvent].TimeUs >= M.Trace[OffPreEvent].TimeUs + 100U);
  CHECK(FindEvent(TraceStart, 'S', 0U, 8U, 0x10000U) < M.TraceCount);
  CHECK(FindEvent(TraceStart, 'S', 0U, 2U, 0U) == TRACE_CAPACITY);
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreResetRelease && Core.ResetIndex == 0U);
  CHECK(FindEvent(TraceStart, 'S', 0U, 2U, 0U) < M.TraceCount);

  /* Refclock-on needs 32us after ACK=3 before the post-SMC call. */
  ResetModel();
  PrepareAndStopHost(&Query, &Prepare);
  CHECK(StartRelease(&Release) == MtkUfsPowerSuccess);
  InitRequest(&Restore, MtkUfsPowerRestore, Core.Generation, 0U);
  AdvanceRestoreToPhase(&Restore, MtkUfsPowerPhaseRestoreRefDelayPost);
  StartUs = Core.PhaseStartedUs;
  TraceStart = M.TraceCount;
  M.AdvanceTime = 0;
  M.TimeUs = StartUs + 31U;
  for (I = 0; I < 3U; I++) {
    CHECK(Step(&Restore) == MtkUfsPowerInProgress);
    CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefDelayPost);
    CHECK(CountSmcEvent(TraceStart, 8U, 0x10001U) == 0U);
  }
  M.TimeUs = StartUs + 32U;
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefSmcPost);
  CHECK(CountSmcEvent(TraceStart, 8U, 0x10001U) == 0U);
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestorePreHce);
  CHECK(CountSmcEvent(TraceStart, 8U, 0x10001U) == 1U);

  /* Reset must remain asserted for 10us, then for 10ms before clock/reset
   * setup may proceed. The next stages also enforce 100us and 600us waits. */
  ResetModel();
  PrepareAndStopHost(&Query, &Prepare);
  CHECK(StartRelease(&Release) == MtkUfsPowerSuccess);
  InitRequest(&Restore, MtkUfsPowerRestore, Core.Generation, 0U);
  AdvanceRestoreToPhase(&Restore, MtkUfsPowerPhaseRestoreResetRelease);
  CHECK(Core.ResetIndex == 0U);
  CHECK(Step(&Restore) == MtkUfsPowerInProgress); /* write HCE=0, start 10us */
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreResetRelease && Core.ResetIndex == 1U);
  StartUs = Core.PhaseStartedUs;
  TraceStart = M.TraceCount;
  M.AdvanceTime = 0;
  M.TimeUs = StartUs + 9U;
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreResetRelease && Core.ResetIndex == 1U);
  CHECK(CountSmcEvent(TraceStart, 2U, 1U) == 0U);
  M.TimeUs = StartUs + 10U;
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.ResetIndex == 2U && CountSmcEvent(TraceStart, 2U, 1U) == 1U);

  StartUs = Core.PhaseStartedUs;
  M.TimeUs = StartUs + 9999U;
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreResetRelease && Core.ResetIndex == 2U);
  CHECK(FindWrite(TraceStart, MtkUfsPowerBankInfra, 0x130U, 1U << 15) == TRACE_CAPACITY);
  M.TimeUs = StartUs + 10000U;
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefOnCheck && Core.RootIndex == 0U);
  CHECK(FindWrite(TraceStart, MtkUfsPowerBankInfra, 0x130U, 1U << 15) == TRACE_CAPACITY);
  CHECK(CountSmcEvent(TraceStart, 8U, 1U) == 0U);
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefSmcPre);
  CHECK(CountSmcEvent(TraceStart, 8U, 1U) == 0U);
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefWrite && Core.RootIndex == 1U);
  CHECK(CountSmcEvent(TraceStart, 8U, 1U) == 1U);
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefAck);
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefDelayPost);
  StartUs = Core.PhaseStartedUs;
  M.TimeUs = StartUs + 31U;
  for (I = 0; I < 3U; I++) {
    CHECK(Step(&Restore) == MtkUfsPowerInProgress);
    CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefDelayPost);
    CHECK(CountSmcEvent(TraceStart, 8U, 0x10001U) == 0U);
  }
  M.TimeUs = StartUs + 32U;
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreRefSmcPost);
  CHECK(CountSmcEvent(TraceStart, 8U, 0x10001U) == 0U);
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestorePreHce && Core.RootIndex == 0U);
  CHECK(CountSmcEvent(TraceStart, 8U, 0x10001U) == 1U);

  while (Core.Phase == MtkUfsPowerPhaseRestorePreHce && Core.RootIndex < 4U) {
    CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  }
  CHECK(Core.Phase == MtkUfsPowerPhaseRestorePreHce && Core.RootIndex == 4U);
  CHECK(Step(&Restore) == MtkUfsPowerInProgress); /* begin reset 100us settle */
  CHECK(Core.RootIndex == 5U);
  StartUs = Core.PhaseStartedUs;
  M.TimeUs = StartUs + 99U;
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.RootIndex == 5U);
  CHECK(FindWrite(TraceStart, MtkUfsPowerBankInfra, 0x144U, 1U << 7) == TRACE_CAPACITY);
  M.TimeUs = StartUs + 100U;
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.RootIndex == 6U);
  CHECK(FindWrite(TraceStart, MtkUfsPowerBankInfra, 0x144U, 1U << 7) == TRACE_CAPACITY);
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(FindWrite(TraceStart, MtkUfsPowerBankInfra, 0x144U, 1U << 7) < M.TraceCount);

  while (Core.Phase != MtkUfsPowerPhaseRestoreHce) {
    CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  }
  CHECK(Core.RootIndex == 0U);
  TraceStart = M.TraceCount;
  CHECK(Step(&Restore) == MtkUfsPowerInProgress); /* HCE=1 write and 600us wait */
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreHce && Core.RootIndex == 1U);
  StartUs = Core.PhaseStartedUs;
  M.TimeUs = StartUs + 599U;
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestoreHce);
  CHECK(FindEvent(TraceStart, 'R', MtkUfsPowerBankHci, 0x034U, 1U) == TRACE_CAPACITY);
  M.TimeUs = StartUs + 600U;
  CHECK(Step(&Restore) == MtkUfsPowerInProgress);
  CHECK(Core.Phase == MtkUfsPowerPhaseRestorePostHce);
  CHECK(FindEvent(TraceStart, 'R', MtkUfsPowerBankHci, 0x034U, 1U) < M.TraceCount);
}

int
main(void)
{
  TestMalformedAndLocking();
  TestQueryAndWacsOwnership();
  TestVoltageAndGenerationValidation();
  TestQueryGateAdmission();
  TestMutationAndQuerySerialization();
  TestHibern8MustBeObservedBeforePowerdown();
  TestPrepareReleaseAndRestore();
  TestRootProofBeforeRelease();
  TestRootReadErrorsBeforeRelease();
  TestReleaseRejectsLostTopParent();
  TestReleaseRetryAndUnknownSmc();
  TestDroppedGateWriteReadback();
  TestRestoreSupplyAndGateOrdering();
  TestRestoreNeedsTenRootStopProof();
  TestUicCommandNotResent();
  TestFrozenClockBoundsUicPolling();
  TestFrozenClockBoundsWacsPolling();
  TestRestoreRefclockBusyAndSmcCompletion();
  TestRestoreFromStopPreparedSequence();
  TestUicSetOutputDoesNotReplacePendingInput();
  TestStopPreparedQueryAcrossHostStop();
  TestRestoreWaitBoundaries();
  TestPrepareCheckRetainsOwnedWacsResponse();
  TestUicPreSubmitReadinessJournal();
  printf("MtkUfsPowerCoreTest: %u checks\n", Checks);
  return 0;
}
