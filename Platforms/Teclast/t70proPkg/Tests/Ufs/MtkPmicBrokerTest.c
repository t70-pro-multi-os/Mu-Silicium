/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host-side tests for the production MT6789 PMIC WACS broker core. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Library/MtkPmicBroker.h>

_Static_assert(sizeof(MTK_PMIC_BROKER_REQUEST) == 64, "wire request must stay 64 bytes");
_Static_assert(offsetof(MTK_PMIC_BROKER_REQUEST, Size) == 0, "Size offset");
_Static_assert(offsetof(MTK_PMIC_BROKER_REQUEST, Generation) == 16, "Generation offset");
_Static_assert(offsetof(MTK_PMIC_BROKER_REQUEST, Result) == 24, "Result offset");
_Static_assert(offsetof(MTK_PMIC_BROKER_REQUEST, Reserved) == 40, "Reserved offset");

#define WACS_CMD       0x880U
#define WACS_WDATA     0x884U
#define WACS_RDATA     0x894U
#define WACS_VLDCLR    0x8A4U
#define WACS_STATUS    0x8A8U
#define TRACE_CAP      512U
#define MAX_STEPS      128U

static unsigned Checks;
#define CHECK(X) do { \
  Checks++; \
  if (!(X)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #X); \
    fprintf(stderr, "broker state=%u phase=%u op=%u generation=%llu address=%x value=%x result=%u; io reads=%u writes=%u cmds=%u clears=%u\n", \
            Core.State, Core.Phase, Core.Operation, \
            (unsigned long long)Core.Generation, Core.Address, Core.Value, Core.FinalResult, \
            Model.ReadCalls, Model.WriteCalls, Model.CommandEffects, Model.ClearEffects); \
    abort(); \
  } \
} while (0)

typedef struct {
  char Kind;
  uint32_t Offset;
  uint32_t Value;
  uint64_t TimeUs;
} TRACE_EVENT;

typedef struct {
  uint16_t Pmic[0x8000U];
  TRACE_EVENT Trace[TRACE_CAP];
  unsigned TraceCount;
  unsigned ReadCalls, WriteCalls, UnexpectedReads, UnexpectedWrites;
  unsigned CommandAttempts, CommandEffects, ReadCommandEffects, WriteCommandEffects;
  unsigned WdataAttempts, WdataEffects, ClearAttempts, ClearEffects;
  unsigned StatusReads;
  uint32_t Address, WriteData, Fsm;
  uint32_t IgnoreWriteAddress;
  unsigned DelayValidPolls, ValidPollsRemaining;
  int HoldReadValid;
  uint64_t TimeUs, TimeIncrement;
  uint32_t FailReadOffset, FailWriteOffset;
  uint32_t FailWriteValue;
  int FailWriteValueMatch;
  int FailReadOnce, FailWriteOnce, FailAfterEffect;
} MODEL;

static MODEL Model;
static MTK_PMIC_BROKER Core;
static MTK_PMIC_BROKER_IO Io;

static void
Trace(char Kind, uint32_t Offset, uint32_t Value)
{
  if (Model.TraceCount < TRACE_CAP) {
    TRACE_EVENT *Event = &Model.Trace[Model.TraceCount++];
    Event->Kind = Kind;
    Event->Offset = Offset;
    Event->Value = Value;
    Event->TimeUs = Model.TimeUs;
  }
}

static MTK_PMIC_BROKER_IO_STATUS
ModelRead32(void *Context, uint32_t Offset, uint32_t *Value)
{
  if (Context != &Model || Value == NULL) return MtkPmicBrokerIoFailed;
  Model.ReadCalls++;
  if (Model.FailReadOnce && Offset == Model.FailReadOffset) {
    Model.FailReadOnce = 0;
    Trace('e', Offset, 0);
    return MtkPmicBrokerIoFailed;
  }
  if (Offset == 0U) {
    *Value = 1U; /* PWRAP INIT_DONE2 */
  } else if (Offset == WACS_STATUS) {
    Model.StatusReads++;
    if (Model.ValidPollsRemaining != 0U) {
      Model.ValidPollsRemaining--;
      if (Model.ValidPollsRemaining == 0U && !Model.HoldReadValid) Model.Fsm = 6U;
    }
    *Value = Model.Fsm << 1;
  } else if (Offset == WACS_RDATA) {
    *Value = Model.Address < 0x8000U ? Model.Pmic[Model.Address] : UINT32_MAX;
  } else {
    Model.UnexpectedReads++;
    Trace('?', Offset, 0);
    return MtkPmicBrokerIoFailed;
  }
  Trace('r', Offset, *Value);
  return MtkPmicBrokerIoOk;
}

static void
ApplyWrite(uint32_t Offset, uint32_t Value)
{
  if (Offset == WACS_CMD) {
    Model.Address = Value & 0xFFFFU;
    Model.CommandEffects++;
    if ((Value & 0x20000000U) != 0U) {
      Model.WriteCommandEffects++;
      if (Model.Address==0x91CU)
        Model.Pmic[Model.Address]&=(uint16_t)~Model.WriteData;
      else if (Model.Address!=Model.IgnoreWriteAddress) {
        if (Model.Address==0x1726U)
          Model.Pmic[0x1724U]|=(uint16_t)Model.WriteData;
        else if (Model.Address==0x1728U)
          Model.Pmic[0x1724U]&=(uint16_t)~Model.WriteData;
        else Model.Pmic[Model.Address] = (uint16_t)Model.WriteData;
      }
      Model.Fsm = 0U;
    } else {
      Model.ReadCommandEffects++;
      if (Model.HoldReadValid) {
        Model.Fsm = 1U;
        Model.ValidPollsRemaining = 0U;
      } else if (Model.DelayValidPolls != 0U) {
        Model.Fsm = 1U;
        Model.ValidPollsRemaining = Model.DelayValidPolls;
      } else {
        Model.Fsm = 6U;
      }
    }
  } else if (Offset == WACS_WDATA) {
    Model.WdataEffects++;
    Model.WriteData = Value;
  } else if (Offset == WACS_VLDCLR && Value == 1U) {
    Model.ClearEffects++;
    Model.Fsm = 0U;
  }
}

static MTK_PMIC_BROKER_IO_STATUS
ModelWrite32(void *Context, uint32_t Offset, uint32_t Value)
{
  int Fail;
  if (Context != &Model) return MtkPmicBrokerIoFailed;
  CHECK(MtkPmicBrokerWacsWriteAllowed(&Core,Offset,Value));
  Model.WriteCalls++;
  Trace('w', Offset, Value);
  if (Offset == WACS_CMD) Model.CommandAttempts++;
  else if (Offset == WACS_WDATA) Model.WdataAttempts++;
  else if (Offset == WACS_VLDCLR) Model.ClearAttempts++;
  else {
    Model.UnexpectedWrites++;
    return MtkPmicBrokerIoFailed;
  }

  Fail = Model.FailWriteOnce && Offset == Model.FailWriteOffset &&
         (!Model.FailWriteValueMatch || Value == Model.FailWriteValue);
  if (!Fail || Model.FailAfterEffect) ApplyWrite(Offset, Value);
  if (Fail) {
    Model.FailWriteOnce = 0;
    return MtkPmicBrokerIoFailed;
  }
  return MtkPmicBrokerIoOk;
}

static uint64_t
ModelNowUs(void *Context)
{
  uint64_t Now;
  if (Context != &Model) return 0;
  Now = Model.TimeUs;
  Model.TimeUs += Model.TimeIncrement;
  return Now;
}

static void
ResetHarness(void)
{
  memset(&Model, 0, sizeof(Model));
  Model.TimeIncrement = 1U;
  MtkPmicBrokerInitialize(&Core);
  Io.Context = &Model;
  Io.Read32 = ModelRead32;
  Io.Write32 = ModelWrite32;
  Io.NowUs = ModelNowUs;
}

static MTK_PMIC_BROKER_RESULT
SendVersion(uint16_t Version, MTK_PMIC_BROKER_OPERATION Operation,
            uint64_t Generation, uint32_t Address, uint32_t Value,
            int OtherOwned, MTK_PMIC_BROKER_REQUEST *Response)
{
  MTK_PMIC_BROKER_REQUEST Request;
  MTK_PMIC_BROKER_RESULT Result;
  memset(&Request, 0, sizeof(Request));
  Request.Size = sizeof(Request);
  Request.Version = Version;
  Request.Operation = (uint16_t)Operation;
  Request.Generation = Generation;
  Request.Address = Address;
  Request.Value = Value;
  Result = MtkPmicBrokerHandle(&Core, &Io, OtherOwned, &Request);
  if (Response != NULL) *Response = Request;
  return Result;
}

static MTK_PMIC_BROKER_RESULT
Send(MTK_PMIC_BROKER_OPERATION Operation, uint64_t Generation,
     uint32_t Address, uint32_t Value, int OtherOwned,
     MTK_PMIC_BROKER_REQUEST *Response)
{
  return SendVersion(MTK_PMIC_BROKER_VERSION,Operation,Generation,Address,Value,
                     OtherOwned,Response);
}

static MTK_PMIC_BROKER_RESULT
SendRaw(MTK_PMIC_BROKER_REQUEST *Request, int OtherOwned)
{
  return MtkPmicBrokerHandle(&Core, &Io, OtherOwned, Request);
}

static MTK_PMIC_BROKER_RESULT
BeginRead(uint32_t Address, uint64_t *Generation)
{
  MTK_PMIC_BROKER_REQUEST Response;
  MTK_PMIC_BROKER_RESULT Result = Send(MtkPmicBrokerBeginRead, 0, Address, 0, 0, &Response);
  if (Generation != NULL) *Generation = Response.Generation;
  return Result;
}

static MTK_PMIC_BROKER_RESULT
BeginWrite(uint32_t Address, uint32_t Value, uint64_t *Generation)
{
  MTK_PMIC_BROKER_REQUEST Response;
  MTK_PMIC_BROKER_RESULT Result = Send(MtkPmicBrokerBeginWrite, 0, Address, Value, 0, &Response);
  if (Generation != NULL) *Generation = Response.Generation;
  return Result;
}

static MTK_PMIC_BROKER_RESULT
BeginSnapshot(uint64_t Generation, uint32_t Address, uint32_t Value,
              int OtherOwned, MTK_PMIC_BROKER_REQUEST *Response)
{
  return Send(MtkPmicBrokerBeginKeySnapshot, Generation, Address, Value,
              OtherOwned, Response);
}

static MTK_PMIC_BROKER_RESULT
Step(uint64_t Generation, int OtherOwned, MTK_PMIC_BROKER_REQUEST *Response)
{
  return Send(MtkPmicBrokerStep, Generation, 0, 0, OtherOwned, Response);
}

static MTK_PMIC_BROKER_RESULT
StepVersion(uint16_t Version, uint64_t Generation, int OtherOwned,
            MTK_PMIC_BROKER_REQUEST *Response)
{
  return SendVersion(Version,MtkPmicBrokerStep,Generation,0U,0U,OtherOwned,Response);
}

static MTK_PMIC_BROKER_RESULT
Ack(uint64_t Generation)
{
  return Send(MtkPmicBrokerAck, Generation, 0, 0, 0, NULL);
}

static MTK_PMIC_BROKER_RESULT
AckVersion(uint16_t Version, uint64_t Generation)
{
  return SendVersion(Version,MtkPmicBrokerAck,Generation,0U,0U,0,NULL);
}

static MTK_PMIC_BROKER_RESULT
DriveVersionToTerminal(uint16_t Version, uint64_t Generation,
                       MTK_PMIC_BROKER_REQUEST *Last)
{
  unsigned Index;
  MTK_PMIC_BROKER_RESULT Result=MtkPmicBrokerResultInProgress;
  for (Index=0;Index<MAX_STEPS;Index++) {
    if (Core.State==MtkPmicBrokerStateDone || Core.State==MtkPmicBrokerStateFault)
      return (MTK_PMIC_BROKER_RESULT)Core.FinalResult;
    Result=StepVersion(Version,Generation,0,Last);
    if (Result!=MtkPmicBrokerResultInProgress) return Result;
  }
  return MtkPmicBrokerResultTimeout;
}

static MTK_PMIC_BROKER_RESULT
DriveToTerminal(uint64_t Generation, MTK_PMIC_BROKER_REQUEST *Last)
{
  unsigned Index;
  MTK_PMIC_BROKER_RESULT Result = MtkPmicBrokerResultInProgress;
  for (Index = 0; Index < MAX_STEPS; Index++) {
    if (Core.State == MtkPmicBrokerStateDone || Core.State == MtkPmicBrokerStateFault)
      return (MTK_PMIC_BROKER_RESULT)Core.FinalResult;
    Result = Step(Generation, 0, Last);
    if (Result != MtkPmicBrokerResultInProgress) return Result;
  }
  return MtkPmicBrokerResultTimeout;
}

static unsigned
GetReadCommandAddresses(uint32_t *Addresses, unsigned Capacity)
{
  unsigned Count = 0;
  for (unsigned Index = 0; Index < Model.TraceCount; Index++) {
    TRACE_EVENT *Event = &Model.Trace[Index];
    if (Event->Kind != 'w' || Event->Offset != WACS_CMD ||
        (Event->Value & 0x20000000U) != 0U)
      continue;
    if (Count < Capacity) Addresses[Count] = Event->Value;
    Count++;
  }
  return Count;
}

static void
TestWireAndInvalidRequests(void)
{
  MTK_PMIC_BROKER_REQUEST Request;
  uint32_t Reads, Writes;
  ResetHarness();
  CHECK(sizeof(Request) == 64U);
  CHECK(offsetof(MTK_PMIC_BROKER_REQUEST, Size) == 0U);
  CHECK(offsetof(MTK_PMIC_BROKER_REQUEST, Version) == 4U);
  CHECK(offsetof(MTK_PMIC_BROKER_REQUEST, Flags) == 8U);
  CHECK(offsetof(MTK_PMIC_BROKER_REQUEST, State) == 12U);
  CHECK(offsetof(MTK_PMIC_BROKER_REQUEST, Generation) == 16U);
  CHECK(offsetof(MTK_PMIC_BROKER_REQUEST, Result) == 24U);
  CHECK(offsetof(MTK_PMIC_BROKER_REQUEST, Address) == 28U);
  CHECK(offsetof(MTK_PMIC_BROKER_REQUEST, Value) == 32U);
  CHECK(offsetof(MTK_PMIC_BROKER_REQUEST, Reserved0) == 36U);
  CHECK(offsetof(MTK_PMIC_BROKER_REQUEST, Reserved) == 40U);

  Reads = Model.ReadCalls;
  Writes = Model.WriteCalls;
  memset(&Request, 0, sizeof(Request));
  Request.Size = sizeof(Request);
  Request.Version = MTK_PMIC_BROKER_VERSION;
  Request.Operation = MtkPmicBrokerBeginRead;
  Request.Address = 0x1B1CU; /* shared VEMC is never exposed */
  CHECK(SendRaw(&Request, 0) == MtkPmicBrokerResultInvalid);
  CHECK(Model.ReadCalls == Reads && Model.WriteCalls == Writes);

  memset(&Request, 0, sizeof(Request));
  Request.Size = sizeof(Request) - 1U;
  Request.Version = MTK_PMIC_BROKER_VERSION;
  Request.Operation = MtkPmicBrokerBeginRead;
  Request.Address = 0x1508U;
  CHECK(SendRaw(&Request, 0) == MtkPmicBrokerResultInvalid);

  memset(&Request, 0, sizeof(Request));
  Request.Size = sizeof(Request);
  Request.Version = MTK_PMIC_BROKER_VERSION_CONN_RAILS + 1U;
  Request.Operation = MtkPmicBrokerBeginRead;
  Request.Address = 0x1508U;
  CHECK(SendRaw(&Request, 0) == MtkPmicBrokerResultInvalid);

  memset(&Request, 0, sizeof(Request));
  Request.Size = sizeof(Request);
  Request.Version = MTK_PMIC_BROKER_VERSION;
  Request.Operation = MtkPmicBrokerBeginRead;
  Request.Address = 0x1508U;
  Request.Flags = 1U;
  CHECK(SendRaw(&Request, 0) == MtkPmicBrokerResultInvalid);

  memset(&Request, 0, sizeof(Request));
  Request.Size = sizeof(Request);
  Request.Version = MTK_PMIC_BROKER_VERSION;
  Request.Operation = MtkPmicBrokerBeginRead;
  Request.Address = 0x1508U;
  Request.Reserved[2] = 1U;
  CHECK(SendRaw(&Request, 0) == MtkPmicBrokerResultInvalid);

  memset(&Request, 0, sizeof(Request));
  Request.Size = sizeof(Request);
  Request.Version = MTK_PMIC_BROKER_VERSION;
  Request.Operation = MtkPmicBrokerBeginRead;
  Request.Address = 0x1508U;
  Request.State = MtkPmicBrokerStatePending;
  CHECK(SendRaw(&Request, 0) == MtkPmicBrokerResultInvalid);

  memset(&Request, 0, sizeof(Request));
  Request.Size = sizeof(Request);
  Request.Version = MTK_PMIC_BROKER_VERSION;
  Request.Operation = MtkPmicBrokerBeginRead;
  Request.Address = 0x1508U;
  Request.Result = MtkPmicBrokerResultInProgress;
  CHECK(SendRaw(&Request, 0) == MtkPmicBrokerResultInvalid);

  memset(&Request, 0, sizeof(Request));
  Request.Size = sizeof(Request);
  Request.Version = MTK_PMIC_BROKER_VERSION;
  Request.Operation = 99U;
  CHECK(SendRaw(&Request, 0) == MtkPmicBrokerResultInvalid);

  CHECK(Send(MtkPmicBrokerBeginRead, 0, 0x1520U, 1U, 0, NULL) ==
        MtkPmicBrokerResultInvalid); /* begin-read input value must be zero */
  CHECK(Send(MtkPmicBrokerBeginWrite, 0, 0x28U, 0U, 0, NULL) ==
        MtkPmicBrokerResultInvalid); /* TOPSTATUS must never be writable */
  CHECK(Send(MtkPmicBrokerBeginWrite, 0, 0x28U, 2U, 0, NULL) ==
        MtkPmicBrokerResultInvalid);
  CHECK(Send(MtkPmicBrokerBeginRead, 0, 0x26U, 0U, 0, NULL) ==
        MtkPmicBrokerResultInvalid); /* no neighboring register exposure */
  CHECK(Send(MtkPmicBrokerBeginRead, 0, 0x2AU, 0U, 0, NULL) ==
        MtkPmicBrokerResultInvalid);
  CHECK(Send(MtkPmicBrokerBeginRead, 0, 0x19EU, 0U, 0, NULL) ==
        MtkPmicBrokerResultInvalid); /* key summary is snapshot-only */
  CHECK(Send(MtkPmicBrokerBeginRead, 0, 0x91CU, 0U, 0, NULL) ==
        MtkPmicBrokerResultInvalid); /* key status is snapshot-only */
  CHECK(Send(MtkPmicBrokerBeginWrite, 0, 0x910U, 1U, 0, NULL) ==
        MtkPmicBrokerResultInvalid); /* key IRQ enable is not brokered */
  CHECK(Send(MtkPmicBrokerBeginWrite, 0, 0x1520U, 1U, 0, NULL) ==
        MtkPmicBrokerResultInvalid); /* DA/status registers are read-only */
  CHECK(Send(MtkPmicBrokerBeginWrite, 0, 0x1B1CU, 1U, 0, NULL) ==
        MtkPmicBrokerResultInvalid); /* shared VEMC cannot be written */
  CHECK(Send(MtkPmicBrokerBeginWrite, 0, 0x1508U, 0x10000U, 0, NULL) ==
        MtkPmicBrokerResultInvalid);
  CHECK(Send(MtkPmicBrokerQuery, 1U, 0, 0, 0, NULL) ==
        MtkPmicBrokerResultInvalid);
  CHECK(Send(MtkPmicBrokerStep, 1U, 1U, 0, 0, NULL) ==
        MtkPmicBrokerResultInvalid);
  CHECK(Send(MtkPmicBrokerBeginRead, 0, 0x1510U, 0, 0, NULL) ==
        MtkPmicBrokerResultInvalid);
  CHECK(MtkPmicBrokerHandle(NULL, &Io, 0, &Request) == MtkPmicBrokerResultInvalid);
  CHECK(Model.ReadCalls == Reads && Model.WriteCalls == Writes);
  CHECK(Core.State == MtkPmicBrokerStateIdle);
  CHECK(!MtkPmicBrokerBlocksUfs(&Core));
}

static void
TestKeySnapshotOrderResultAndMetadataAck(void)
{
  MTK_PMIC_BROKER_REQUEST Response;
  uint32_t Addresses[4] = {0};
  uint64_t Generation;
  unsigned ReadCalls, WriteCalls;
  MTK_PMIC_BROKER_RESULT Result = MtkPmicBrokerResultInProgress;

  ResetHarness();
  Model.Pmic[0x19EU] = 0x0004U; /* group 2 is present */
  Model.Pmic[0x91CU] = 0xA55AU;
  Model.Pmic[0x28U] = 0x5A01U;
  CHECK(BeginSnapshot(0, 0, 0, 0, &Response) ==
        MtkPmicBrokerResultInProgress);
  Generation = Response.Generation;
  CHECK(Generation == 1U);
  CHECK(Response.State == MtkPmicBrokerStatePending);
  CHECK(Response.Address == 0x91CU && Response.Value == 0U);
  CHECK(Core.Address == 0x91CU && Core.Value == 0U);
  for (unsigned Index = 0; Index < MAX_STEPS &&
       Core.State == MtkPmicBrokerStatePending; Index++) {
    Result = Step(Generation, 0, &Response);
    if (Core.State == MtkPmicBrokerStatePending) {
      CHECK(Result == MtkPmicBrokerResultInProgress);
      CHECK(Response.Result == MtkPmicBrokerResultInProgress);
      CHECK(Response.Generation == Generation && Response.Address == 0x91CU &&
            Response.Value == 0U);
    }
  }
  CHECK(Result == MtkPmicBrokerResultSuccess);
  CHECK(Response.State == MtkPmicBrokerStateDone);
  CHECK(Response.Generation == Generation);
  CHECK(Response.Address == 0x91CU);
  CHECK(Response.Value == 0xA55A5A01U);
  CHECK(Core.Address == 0x91CU && Core.Value == 0xA55A5A01U);
  CHECK(Core.Operation == MtkPmicBrokerBeginKeySnapshot);
  CHECK(GetReadCommandAddresses(Addresses, 4U) == 3U);
  CHECK(Addresses[0] == 0x19EU && Addresses[1] == 0x91CU &&
        Addresses[2] == 0x28U);
  CHECK(Model.ReadCommandEffects == 3U);
  CHECK(Model.WriteCommandEffects == 0U && Model.WdataEffects == 0U);
  CHECK(Model.UnexpectedReads == 0U && Model.UnexpectedWrites == 0U);

  ReadCalls = Model.ReadCalls;
  WriteCalls = Model.WriteCalls;
  CHECK(Step(Generation, 0, &Response) == MtkPmicBrokerResultSuccess);
  CHECK(Response.Generation == Generation && Response.Address == 0x91CU &&
        Response.Value == 0xA55A5A01U);
  CHECK(Send(MtkPmicBrokerQuery, 0, 0, 0, 0, &Response) ==
        MtkPmicBrokerResultSuccess);
  CHECK(Response.State == MtkPmicBrokerStateDone &&
        Response.Generation == Generation && Response.Address == 0x91CU &&
        Response.Value == 0xA55A5A01U);
  CHECK(Model.ReadCalls == ReadCalls && Model.WriteCalls == WriteCalls);

  CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess);
  CHECK(Model.ReadCalls == ReadCalls && Model.WriteCalls == WriteCalls);
  CHECK(Model.WriteCommandEffects == 0U && Model.WdataEffects == 0U);
}

static void
TestKeySnapshotSkipsAbsentGroup(void)
{
  MTK_PMIC_BROKER_REQUEST Response;
  uint32_t Addresses[3] = {0};
  uint64_t Generation;

  ResetHarness();
  Model.Pmic[0x19EU] = 0x0001U; /* group 2 bit is clear */
  Model.Pmic[0x91CU] = 0xDEADU; /* must not be read */
  Model.Pmic[0x28U] = 0x1234U;
  CHECK(BeginSnapshot(0, 0, 0, 0, &Response) ==
        MtkPmicBrokerResultInProgress);
  Generation = Response.Generation;
  CHECK(Response.Address == 0x91CU && Response.Value == 0U);
  CHECK(DriveToTerminal(Generation, &Response) == MtkPmicBrokerResultSuccess);
  CHECK(Response.Address == 0x91CU && Response.Value == 0x00001234U);
  CHECK(Core.SnapshotGroupStatus == 0U);
  CHECK(GetReadCommandAddresses(Addresses, 3U) == 2U);
  CHECK(Addresses[0] == 0x19EU && Addresses[1] == 0x28U);
  CHECK(Model.ReadCommandEffects == 2U && Model.ClearEffects == 2U);
  CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess);
}

static void
TestKeySnapshotInvalidInputsAndBusyOwnership(void)
{
  MTK_PMIC_BROKER_REQUEST Response;
  uint64_t Generation;
  unsigned Reads, Writes;

  ResetHarness();
  Reads = Model.ReadCalls;
  Writes = Model.WriteCalls;
  CHECK(BeginSnapshot(1U, 0, 0, 0, NULL) == MtkPmicBrokerResultInvalid);
  CHECK(BeginSnapshot(0, 0x19EU, 0, 0, NULL) == MtkPmicBrokerResultInvalid);
  CHECK(BeginSnapshot(0, 0, 1U, 0, NULL) == MtkPmicBrokerResultInvalid);
  CHECK(Core.State == MtkPmicBrokerStateIdle);
  CHECK(Core.Generation == 0U && Model.ReadCalls == Reads &&
        Model.WriteCalls == Writes);

  CHECK(BeginSnapshot(0, 0, 0, 1, NULL) == MtkPmicBrokerResultBusy);
  CHECK(Core.State == MtkPmicBrokerStateIdle);
  CHECK(Model.ReadCalls == Reads && Model.WriteCalls == Writes);

  Model.Pmic[0x19EU] = 4U;
  Model.Pmic[0x91CU] = 0xFFFFU;
  Model.Pmic[0x28U] = 0xBEEFU;
  CHECK(BeginSnapshot(0, 0, 0, 0, &Response) ==
        MtkPmicBrokerResultInProgress);
  Generation = Response.Generation;
  Reads = Model.ReadCalls;
  Writes = Model.WriteCalls;
  CHECK(Step(Generation, 1, &Response) == MtkPmicBrokerResultBusy);
  CHECK(Response.State == MtkPmicBrokerStatePending &&
        Response.Address == 0x91CU && Response.Value == 0U);
  CHECK(Core.State == MtkPmicBrokerStatePending && Core.SnapshotStage == 0U);
  CHECK(Model.ReadCalls == Reads && Model.WriteCalls == Writes);

  Model.Fsm = 6U; /* an unowned stale response is not cleared */
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultBusy);
  CHECK(Model.ReadCommandEffects == 0U && Model.ClearEffects == 0U);
  CHECK(Core.State == MtkPmicBrokerStatePending && Core.SnapshotStage == 0U);
  Model.Fsm = 0U;
  CHECK(DriveToTerminal(Generation, &Response) == MtkPmicBrokerResultSuccess);
  CHECK(Response.Value == 0xFFFFBEEFU);
  CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess);
}

static void
TestKeySnapshotTimeoutAndFaultPreservation(void)
{
  MTK_PMIC_BROKER_REQUEST Response;
  uint64_t Generation;
  unsigned Reads, Writes, Commands, Clears;

  for (int AfterEffect = 0; AfterEffect <= 1; AfterEffect++) {
    ResetHarness();
    CHECK(BeginSnapshot(0, 0, 0, 0, &Response) ==
          MtkPmicBrokerResultInProgress);
    Generation = Response.Generation;
    Model.FailWriteOffset = WACS_CMD;
    Model.FailWriteOnce = 1;
    Model.FailAfterEffect = AfterEffect;
    CHECK(Step(Generation, 0, &Response) == MtkPmicBrokerResultIoError);
    CHECK(Core.State == MtkPmicBrokerStateFault);
    CHECK(Core.Address == 0x91CU && Core.Value == 0U &&
          Core.SnapshotStage == 0U);
    CHECK(Response.State == MtkPmicBrokerStateFault &&
          Response.Address == 0x91CU && Response.Value == 0U);
    CHECK(MtkPmicBrokerBlocksUfs(&Core));
    Commands = Model.CommandAttempts;
    Reads = Model.ReadCalls;
    Writes = Model.WriteCalls;
    CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultIoError);
    CHECK(Model.CommandAttempts == Commands && Model.ReadCalls == Reads &&
          Model.WriteCalls == Writes);
  }

  ResetHarness();
  CHECK(BeginSnapshot(0, 0, 0, 0, &Response) ==
        MtkPmicBrokerResultInProgress);
  Generation = Response.Generation;
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultInProgress);
  CHECK(Core.Phase == 2U && Core.SnapshotStage == 0U); /* PhaseReadWait */
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultInProgress);
  CHECK(Core.Phase == 3U && Core.SnapshotStage == 0U); /* PhaseReadClear */
  Model.FailWriteOffset = WACS_VLDCLR;
  Model.FailWriteOnce = 1;
  Model.FailAfterEffect = 1; /* clear may take effect, so it cannot be retried */
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultIoError);
  CHECK(Core.State == MtkPmicBrokerStateFault && Core.SnapshotStage == 0U);
  CHECK(Core.Address == 0x91CU && Core.Value == 0U);
  Commands = Model.CommandAttempts;
  Clears = Model.ClearAttempts;
  Reads = Model.ReadCalls;
  Writes = Model.WriteCalls;
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultIoError);
  CHECK(Model.CommandAttempts == Commands && Model.ClearAttempts == Clears &&
        Model.ReadCalls == Reads && Model.WriteCalls == Writes);

  ResetHarness();
  Model.HoldReadValid = 1;
  CHECK(BeginSnapshot(0, 0, 0, 0, &Response) ==
        MtkPmicBrokerResultInProgress);
  Generation = Response.Generation;
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultInProgress);
  CHECK(Model.ReadCommandEffects == 1U && Model.ClearEffects == 0U);
  Model.TimeIncrement = 6000U;
  for (int Index = 0; Index < 5 && Core.State != MtkPmicBrokerStateFault; Index++)
    (void)Step(Generation, 0, NULL);
  CHECK(Core.State == MtkPmicBrokerStateFault);
  CHECK(Core.FinalResult == MtkPmicBrokerResultTimeout);
  CHECK(Core.Address == 0x91CU && Core.Value == 0U &&
        Core.SnapshotStage == 0U);
  CHECK(MtkPmicBrokerBlocksUfs(&Core));
  Commands = Model.CommandAttempts;
  Reads = Model.ReadCalls;
  Writes = Model.WriteCalls;
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultTimeout);
  CHECK(Model.CommandAttempts == Commands && Model.ReadCalls == Reads &&
        Model.WriteCalls == Writes);
  CHECK(Model.ReadCommandEffects == 1U && Model.ClearEffects == 0U);
}

static void
TestReadAllowlistAndAck(void)
{
  static const uint32_t Addresses[] = {
    0x28U, 0x1508U, 0x1520U, 0x1526U, 0x1BC8U, 0x1BE4U, 0x1BE2U, 0x1BF4U
  };
  unsigned Index;
  for (Index = 0; Index < sizeof(Addresses) / sizeof(Addresses[0]); Index++) {
    MTK_PMIC_BROKER_REQUEST Response;
    uint64_t Generation;
    uint16_t Expected = (uint16_t)(0x2100U + Index);
    ResetHarness();
    Model.Pmic[Addresses[Index]] = Expected;
    CHECK(BeginRead(Addresses[Index], &Generation) == MtkPmicBrokerResultInProgress);
    CHECK(Generation == 1U);
    CHECK(Core.State == MtkPmicBrokerStatePending);
    CHECK(MtkPmicBrokerBlocksUfs(&Core));
    CHECK(DriveToTerminal(Generation, &Response) == MtkPmicBrokerResultSuccess);
    CHECK(Core.State == MtkPmicBrokerStateDone);
    CHECK(Response.State == MtkPmicBrokerStateDone);
    CHECK(Response.Generation == Generation);
    CHECK(Response.Address == Addresses[Index]);
    CHECK(Response.Value == Expected);
    CHECK(!MtkPmicBrokerBlocksUfs(&Core));
    CHECK(Model.ReadCommandEffects == 1U);
    CHECK(Model.WriteCommandEffects == 0U);
    CHECK(Model.ClearEffects == 1U);
    CHECK(Model.UnexpectedReads == 0U && Model.UnexpectedWrites == 0U);
    CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess);
    CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess); /* idempotent last ack */
    CHECK(Core.State == MtkPmicBrokerStateIdle);
    CHECK(!MtkPmicBrokerBlocksUfs(&Core));
  }

  ResetHarness();
  CHECK(Send(MtkPmicBrokerBeginRead, 0, 0x1E38U, 0, 0, NULL) ==
        MtkPmicBrokerResultInvalid); /* shared VEMC voltage is not in the broker */
  CHECK(Model.ReadCalls == 0U && Model.WriteCalls == 0U);
}

static void
TestOtherOwnerAndUnknownResponse(void)
{
  uint64_t Generation;
  unsigned Reads, Writes;

  ResetHarness();
  CHECK(Send(MtkPmicBrokerBeginRead, 0, 0x1508U, 0, 1, NULL) ==
        MtkPmicBrokerResultBusy);
  CHECK(Model.ReadCalls == 0U && Model.WriteCalls == 0U);
  CHECK(Core.State == MtkPmicBrokerStateIdle);

  CHECK(BeginRead(0x1508U, &Generation) == MtkPmicBrokerResultInProgress);
  Reads = Model.ReadCalls;
  Writes = Model.WriteCalls;
  CHECK(Step(Generation, 1, NULL) == MtkPmicBrokerResultBusy);
  CHECK(Model.ReadCalls == Reads && Model.WriteCalls == Writes);
  CHECK(Core.State == MtkPmicBrokerStatePending);
  CHECK(MtkPmicBrokerBlocksUfs(&Core));
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultInProgress);
  CHECK(Model.CommandEffects == 1U);

  ResetHarness();
  Model.Fsm = 6U; /* response already valid, but no broker owner can claim it */
  CHECK(BeginRead(0x1520U, &Generation) == MtkPmicBrokerResultInProgress);
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultBusy);
  CHECK(Model.CommandEffects == 0U && Model.ClearEffects == 0U);
  CHECK(Core.State == MtkPmicBrokerStatePending);
  Model.Fsm = 0U;
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultInProgress);
  CHECK(Model.ReadCommandEffects == 1U);
  CHECK(DriveToTerminal(Generation, NULL) == MtkPmicBrokerResultSuccess);
  CHECK(Model.ClearEffects == 1U);
  CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess);
}

static void
TestDelayedReadValid(void)
{
  uint64_t Generation;
  unsigned Index;
  ResetHarness();
  Model.Pmic[0x1520U] = 0xA55AU;
  Model.DelayValidPolls = 3U;
  CHECK(BeginRead(0x1520U, &Generation) == MtkPmicBrokerResultInProgress);
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultInProgress);
  CHECK(Model.ReadCommandEffects == 1U);
  for (Index = 0; Index < 2U; Index++) {
    CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultInProgress);
    CHECK(Model.ClearEffects == 0U);
    CHECK(Model.ReadCommandEffects == 1U);
  }
  CHECK(DriveToTerminal(Generation, NULL) == MtkPmicBrokerResultSuccess);
  CHECK(Core.Value == 0xA55AU);
  CHECK(Model.ReadCommandEffects == 1U && Model.ClearEffects == 1U);
  CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess);
}

static void
TestWriteMasksAndReadback(void)
{
  static const struct {
    uint32_t Address, Before, Desired;
  } Cases[] = {
    {0x1508U, 0x00A0U, 0x00A1U},
    {0x1526U, 0xA500U, 0xA523U},
    {0x1BC8U, 0x0040U, 0x0041U},
    {0x1BF4U, 0xB000U, 0xB011U}
  };
  unsigned Index, EventIndex;
  for (Index = 0; Index < sizeof(Cases) / sizeof(Cases[0]); Index++) {
    uint64_t Generation;
    ResetHarness();
    Model.Pmic[Cases[Index].Address] = (uint16_t)Cases[Index].Before;
    CHECK(BeginWrite(Cases[Index].Address, Cases[Index].Desired, &Generation) ==
          MtkPmicBrokerResultInProgress);
    CHECK(DriveToTerminal(Generation, NULL) == MtkPmicBrokerResultSuccess);
    CHECK(Core.State == MtkPmicBrokerStateDone);
    CHECK(Model.Pmic[Cases[Index].Address] == (uint16_t)Cases[Index].Desired);
    CHECK(Model.ReadCommandEffects == 2U); /* pre-read and post-write readback */
    CHECK(Model.WriteCommandEffects == 1U);
    CHECK(Model.WdataEffects == 1U);
    CHECK(Model.ClearEffects == 2U);
    CHECK(!MtkPmicBrokerBlocksUfs(&Core));
    CHECK(Model.UnexpectedReads == 0U && Model.UnexpectedWrites == 0U);

    EventIndex = 0;
    while (EventIndex < Model.TraceCount && Model.Trace[EventIndex].Kind != 'w') EventIndex++;
    CHECK(EventIndex < Model.TraceCount);
    CHECK(Model.Trace[EventIndex].Offset == WACS_CMD &&
          Model.Trace[EventIndex].Value == Cases[Index].Address);
    EventIndex++;
    while (EventIndex < Model.TraceCount &&
           !(Model.Trace[EventIndex].Kind == 'w' && Model.Trace[EventIndex].Offset == WACS_WDATA))
      EventIndex++;
    CHECK(EventIndex < Model.TraceCount);
    CHECK(Model.Trace[EventIndex].Value == Cases[Index].Desired);
    EventIndex++;
    while (EventIndex < Model.TraceCount &&
           !(Model.Trace[EventIndex].Kind == 'w' && Model.Trace[EventIndex].Offset == WACS_CMD &&
             Model.Trace[EventIndex].Value == (0x20000000U | Cases[Index].Address)))
      EventIndex++;
    CHECK(EventIndex < Model.TraceCount);
    CHECK(DriveToTerminal(Generation, NULL) == MtkPmicBrokerResultSuccess);
    CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess);
  }

  ResetHarness();
  Model.Pmic[0x1508U] = 0U;
  {
    uint64_t Generation;
    CHECK(BeginWrite(0x1508U, 2U, &Generation) == MtkPmicBrokerResultInProgress);
    CHECK(DriveToTerminal(Generation, NULL) == MtkPmicBrokerResultInvalid);
    CHECK(Core.State == MtkPmicBrokerStateDone);
    CHECK(Model.ReadCommandEffects == 1U && Model.ClearEffects == 1U);
    CHECK(Model.WdataEffects == 0U && Model.WriteCommandEffects == 0U);
    CHECK(!MtkPmicBrokerBlocksUfs(&Core));
    CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess);
  }

  ResetHarness();
  Model.Pmic[0x1526U] = 0U;
  {
    uint64_t Generation;
    CHECK(BeginWrite(0x1526U, 0x80U, &Generation) == MtkPmicBrokerResultInProgress);
    CHECK(DriveToTerminal(Generation, NULL) == MtkPmicBrokerResultInvalid);
    CHECK(Model.ReadCommandEffects == 1U && Model.ClearEffects == 1U);
    CHECK(Model.WdataEffects == 0U && Model.WriteCommandEffects == 0U);
    CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess);
  }

  ResetHarness();
  Model.Pmic[0x1BF4U] = 0x7F55U;
  {
    uint64_t Generation;
    CHECK(BeginWrite(0x1BF4U, 0x7F55U, &Generation) == MtkPmicBrokerResultInProgress);
    CHECK(DriveToTerminal(Generation, NULL) == MtkPmicBrokerResultSuccess);
    CHECK(Model.WdataEffects == 0U && Model.WriteCommandEffects == 0U);
    CHECK(Model.ReadCommandEffects == 1U && Model.ClearEffects == 1U);
    CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess);
  }
}

static void
TestAckAndGenerationRules(void)
{
  MTK_PMIC_BROKER_REQUEST Response;
  uint64_t Generation, NextGeneration;
  unsigned Reads, Writes;
  ResetHarness();
  Model.Pmic[0x1508U] = 1U;
  CHECK(BeginRead(0x1508U, &Generation) == MtkPmicBrokerResultInProgress);
  Reads = Model.ReadCalls;
  Writes = Model.WriteCalls;
  CHECK(Step(Generation + 1U, 0, NULL) == MtkPmicBrokerResultStaleGeneration);
  CHECK(Ack(Generation + 1U) == MtkPmicBrokerResultStaleGeneration);
  CHECK(Model.ReadCalls == Reads && Model.WriteCalls == Writes);
  CHECK(DriveToTerminal(Generation, &Response) == MtkPmicBrokerResultSuccess);
  Reads = Model.ReadCalls;
  Writes = Model.WriteCalls;
  CHECK(Send(MtkPmicBrokerBeginRead, 0, 0x1520U, 0, 0, NULL) == MtkPmicBrokerResultBusy);
  CHECK(Model.ReadCalls == Reads && Model.WriteCalls == Writes);
  CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess);
  CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess);
  CHECK(Model.ReadCalls == Reads && Model.WriteCalls == Writes);
  CHECK(Core.State == MtkPmicBrokerStateIdle);
  CHECK(BeginRead(0x1520U, &NextGeneration) == MtkPmicBrokerResultInProgress);
  CHECK(NextGeneration == Generation + 1U);

  ResetHarness();
  Core.Generation = UINT64_MAX;
  CHECK(BeginRead(0x1520U, &Generation) == MtkPmicBrokerResultExhausted);
  CHECK(Core.State == MtkPmicBrokerStateIdle);
  CHECK(Core.Generation == UINT64_MAX);
  CHECK(Model.ReadCalls == 0U && Model.WriteCalls == 0U);
}

static void
TestNoReplayAfterCommandOrClearFailure(void)
{
  uint64_t Generation;
  unsigned Attempts, Clears, Reads, Writes;

  ResetHarness();
  CHECK(BeginRead(0x1520U, &Generation) == MtkPmicBrokerResultInProgress);
  Model.FailWriteOffset = WACS_CMD;
  Model.FailWriteOnce = 1;
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultIoError);
  CHECK(Core.State == MtkPmicBrokerStateFault);
  CHECK(MtkPmicBrokerBlocksUfs(&Core));
  CHECK(Model.CommandAttempts == 1U && Model.CommandEffects == 0U);
  Attempts = Model.CommandAttempts;
  Reads = Model.ReadCalls;
  Writes = Model.WriteCalls;
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultIoError);
  CHECK(Model.CommandAttempts == Attempts && Model.ReadCalls == Reads && Model.WriteCalls == Writes);

  ResetHarness();
  CHECK(BeginRead(0x1520U, &Generation) == MtkPmicBrokerResultInProgress);
  Model.FailWriteOffset = WACS_CMD;
  Model.FailWriteOnce = 1;
  Model.FailAfterEffect = 1;
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultIoError);
  CHECK(Core.State == MtkPmicBrokerStateFault);
  CHECK(Model.CommandAttempts == 1U && Model.CommandEffects == 1U);
  CHECK(Model.ClearEffects == 0U);
  Attempts = Model.CommandAttempts;
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultIoError);
  CHECK(Model.CommandAttempts == Attempts && Model.ClearEffects == 0U);

  for (int After = 0; After <= 1; After++) {
    ResetHarness();
    CHECK(BeginRead(0x1520U, &Generation) == MtkPmicBrokerResultInProgress);
    CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultInProgress); /* accepted read */
    CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultInProgress); /* capture response */
    Model.FailWriteOffset = WACS_VLDCLR;
    Model.FailWriteOnce = 1;
    Model.FailAfterEffect = After;
    CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultIoError);
    CHECK(Core.State == MtkPmicBrokerStateFault);
    CHECK(MtkPmicBrokerBlocksUfs(&Core));
    CHECK(Model.ClearAttempts == 1U);
    Clears = Model.ClearAttempts;
    Reads = Model.ReadCalls;
    Writes = Model.WriteCalls;
    CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultIoError);
    CHECK(Model.ClearAttempts == Clears && Model.ReadCalls == Reads && Model.WriteCalls == Writes);
  }

  ResetHarness();
  CHECK(BeginRead(0x1508U, &Generation) == MtkPmicBrokerResultInProgress);
  Model.HoldReadValid = 1;
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultInProgress);
  CHECK(Model.CommandEffects == 1U);
  Model.TimeIncrement = 6000U;
  for (int Index = 0; Index < 4 && Core.State != MtkPmicBrokerStateFault; Index++)
    (void)Step(Generation, 0, NULL);
  CHECK(Core.State == MtkPmicBrokerStateFault);
  CHECK(Core.FinalResult == MtkPmicBrokerResultTimeout);
  CHECK(MtkPmicBrokerBlocksUfs(&Core));
  CHECK(Model.CommandEffects == 1U && Model.ClearEffects == 0U);
}

static void
TestBlocksUfsFailsClosedForCorruptJournal(void)
{
  ResetHarness();
  CHECK(!MtkPmicBrokerBlocksUfs(&Core)); /* normal Idle */

  Core.State = UINT32_MAX; /* valid signature, unknown state */
  CHECK(MtkPmicBrokerBlocksUfs(&Core));

  ResetHarness();
  Core.State = MtkPmicBrokerStateIdle;
  Core.Phase = 1U; /* Idle with a retained phase journal */
  CHECK(MtkPmicBrokerBlocksUfs(&Core));

  ResetHarness();
  Core.State = MtkPmicBrokerStateDone;
  Core.Phase = 1U; /* Done with a non-idle phase journal */
  CHECK(MtkPmicBrokerBlocksUfs(&Core));

  ResetHarness();
  Core.Signature ^= 1U;
  CHECK(MtkPmicBrokerBlocksUfs(&Core));

  ResetHarness();
  CHECK(!MtkPmicBrokerBlocksUfs(&Core)); /* normal Idle restored */
  Core.State = MtkPmicBrokerStateDone;
  Core.Phase = 0U;
  CHECK(!MtkPmicBrokerBlocksUfs(&Core)); /* normal Done after hardware-idle proof */

  ResetHarness();
  Core.Phase = 1U;
  Core.Address = 0xABCDU;
  Core.Value = 0x1234U;
  CHECK(BeginRead(0x1508U, NULL) == MtkPmicBrokerResultIoError);
  CHECK(Core.State == MtkPmicBrokerStateFault);
  CHECK(Core.Phase == 1U);
  CHECK(Core.Address == 0xABCDU && Core.Value == 0x1234U);
  CHECK(MtkPmicBrokerBlocksUfs(&Core));
  CHECK(Model.ReadCalls == 0U && Model.WriteCalls == 0U);
  {
    MTK_PMIC_BROKER_REQUEST Response;
    CHECK(Send(MtkPmicBrokerQuery, 0, 0, 0, 0, &Response) ==
          MtkPmicBrokerResultIoError);
    CHECK(Response.State == MtkPmicBrokerStateFault);
    CHECK(Response.Result == MtkPmicBrokerResultIoError);
    CHECK(Response.Address == 0xABCDU && Response.Value == 0x1234U);
    CHECK(Model.ReadCalls == 0U && Model.WriteCalls == 0U);
  }
}

static void
TestUfsOwnerAndFaultBlocking(void)
{
  uint64_t Generation;
  ResetHarness();
  CHECK(!MtkPmicBrokerBlocksUfs(&Core));
  CHECK(BeginRead(0x1520U, &Generation) == MtkPmicBrokerResultInProgress);
  CHECK(MtkPmicBrokerBlocksUfs(&Core));
  CHECK(DriveToTerminal(Generation, NULL) == MtkPmicBrokerResultSuccess);
  CHECK(!MtkPmicBrokerBlocksUfs(&Core)); /* Done means channel idle, despite pending Ack */
  CHECK(Ack(Generation) == MtkPmicBrokerResultSuccess);

  ResetHarness();
  CHECK(BeginRead(0x1520U, &Generation) == MtkPmicBrokerResultInProgress);
  Model.FailWriteOffset = WACS_CMD;
  Model.FailWriteOnce = 1;
  CHECK(Step(Generation, 0, NULL) == MtkPmicBrokerResultIoError);
  CHECK(Core.State == MtkPmicBrokerStateFault);
  CHECK(MtkPmicBrokerBlocksUfs(&Core));
}


static uint64_t V2Job(MTK_PMIC_BROKER_OPERATION Op,uint64_t Token,
                      MTK_PMIC_BROKER_RESULT Expected) {
  MTK_PMIC_BROKER_REQUEST R;
  MTK_PMIC_BROKER_RESULT Result=SendVersion(2U,Op,Token,0,0,0,&R);
  uint64_t Job=R.Generation;
  CHECK(Result==MtkPmicBrokerResultInProgress || Result==Expected);
  CHECK(Job>Token && Job!=0U);
  if (Result==MtkPmicBrokerResultInProgress)
    Result=DriveVersionToTerminal(2U,Job,&R);
  CHECK(Result==Expected && Core.State==MtkPmicBrokerStateDone);
  CHECK(AckVersion(2U,Job)==MtkPmicBrokerResultSuccess);
  CHECK(Core.State==MtkPmicBrokerStateIdle && Core.WriteCallbackActive==0U);
  return Job;
}
static void V2CapabilityNoEffects(void) {
  MTK_PMIC_BROKER_REQUEST R;
  MTK_PMIC_BROKER Before=Core;
  unsigned Reads=Model.ReadCalls,Writes=Model.WriteCalls;
  CHECK(SendVersion(2U,MtkPmicBrokerQuery,0,0,0,0,&R)==MtkPmicBrokerResultSuccess);
  CHECK(R.Flags==3U && R.State==0U && R.Generation==0U && R.Address==0U && R.Value==0U);
  CHECK(memcmp(&Before,&Core,sizeof(Core))==0);
  CHECK(Reads==Model.ReadCalls && Writes==Model.WriteCalls);
}
static void TestV2LeaseCaptureAndRestore(void) {
  for (uint32_t Borrowed=0;Borrowed<6U;Borrowed++) {
    if (Borrowed==2U || Borrowed==3U) continue;
    ResetHarness();Model.Pmic[0x910]=0xA0U|Borrowed;
    V2CapabilityNoEffects();
    uint64_t Lease=V2Job(MtkPmicBrokerAcquireKeyLease,0,MtkPmicBrokerResultSuccess);
    CHECK(Core.KeyLeaseToken==Lease && Model.Pmic[0x910]==0xA5U);
    CHECK(Core.KeyLeaseOwnedMask==((~Borrowed)&5U));
    CHECK(Core.KeyLeaseBaselineBits==Borrowed);
    Model.Pmic[0x19e]=4U;Model.Pmic[0x91c]=1U;Model.Pmic[0x28]=2U;
    uint64_t Snap=V2Job(MtkPmicBrokerBeginKeySnapshot,Lease,MtkPmicBrokerResultSuccess);
    CHECK(Core.KeySnapshotValid && Core.KeySnapshotToken==Snap &&
          Core.KeySnapshotLeaseToken==Lease && Core.KeySnapshotStatus==1U &&
          Core.KeySnapshotTopStatus==2U && Model.Pmic[0x91c]==1U);
    V2CapabilityNoEffects();
    unsigned Reads=Model.ReadCalls,Writes=Model.WriteCalls;
    CHECK(SendVersion(2U,MtkPmicBrokerReleaseKeyLease,Lease,0,0,0,NULL)==MtkPmicBrokerResultWrongState);
    CHECK(SendVersion(2U,MtkPmicBrokerAckKeySnapshot,Snap+1U,0,0,0,NULL)==MtkPmicBrokerResultStaleGeneration);
    CHECK(Reads==Model.ReadCalls && Writes==Model.WriteCalls && Core.KeySnapshotToken==Snap);
    /* A v1 snapshot and a GPU read do not replace the retained event. */
    uint64_t G;
    MTK_PMIC_BROKER_REQUEST Legacy;
    CHECK(BeginSnapshot(0,0,0,0,&Legacy)==MtkPmicBrokerResultInProgress);G=Legacy.Generation;
    CHECK(DriveToTerminal(G,NULL)==MtkPmicBrokerResultSuccess && Ack(G)==MtkPmicBrokerResultSuccess);
    CHECK(Core.KeySnapshotToken==Snap && Core.KeySnapshotStatus==1U);
    CHECK(BeginRead(0x1520U,&G)==MtkPmicBrokerResultInProgress);
    CHECK(DriveToTerminal(G,NULL)==MtkPmicBrokerResultSuccess && Ack(G)==MtkPmicBrokerResultSuccess);
    CHECK(Core.KeySnapshotToken==Snap && Core.KeySnapshotStatus==1U);
    /* Fresh release bit4 was not part of capture1: W1C must leave it set. */
    Model.Pmic[0x91c]=5U;
    unsigned Commands=Model.WriteCommandEffects;
    uint64_t AckJob=V2Job(MtkPmicBrokerAckKeySnapshot,Snap,MtkPmicBrokerResultSuccess);
    CHECK(Model.Pmic[0x91c]==4U && Model.WriteCommandEffects==Commands+1U);
    CHECK(Core.KeySnapshotValid==0U && Core.KeyLeaseToken==Lease);
    CHECK(Model.WriteData==1U);
    Reads=Model.ReadCalls;Writes=Model.WriteCalls;
    MTK_PMIC_BROKER_REQUEST R;
    CHECK(SendVersion(2U,MtkPmicBrokerAckKeySnapshot,Snap,0,0,0,&R)==MtkPmicBrokerResultSuccess);
    CHECK(R.State==0U && R.Generation==AckJob && Reads==Model.ReadCalls && Writes==Model.WriteCalls);
    Model.Pmic[0x910]|=0x200U;
    uint64_t Release=V2Job(MtkPmicBrokerReleaseKeyLease,Lease,MtkPmicBrokerResultSuccess);
    CHECK(Model.Pmic[0x910]==(0x2A0U|Borrowed) && Core.KeyLeaseToken==0U);
    Reads=Model.ReadCalls;Writes=Model.WriteCalls;
    CHECK(SendVersion(2U,MtkPmicBrokerReleaseKeyLease,Lease,0,0,0,&R)==MtkPmicBrokerResultSuccess);
    CHECK(R.Generation==Release && R.State==0U && Reads==Model.ReadCalls && Writes==Model.WriteCalls);
    V2CapabilityNoEffects();
  }
}
static void TestV2SiblingRetryAndProtocolGuards(void) {
  ResetHarness();
  uint64_t Lease=V2Job(MtkPmicBrokerAcquireKeyLease,0,MtkPmicBrokerResultSuccess);
  Model.Pmic[0x19e]=4U;Model.Pmic[0x91c]=7U;Model.Pmic[0x28]=2U;
  uint64_t Snap=V2Job(MtkPmicBrokerBeginKeySnapshot,Lease,MtkPmicBrokerResultSuccess);
  unsigned Writes=Model.WriteCommandEffects;
  V2Job(MtkPmicBrokerAckKeySnapshot,Snap,MtkPmicBrokerResultSiblingPending);
  CHECK(Model.WriteCommandEffects==Writes && Model.Pmic[0x91c]==7U &&
        Core.KeySnapshotValid && Core.KeySnapshotToken==Snap && Core.KeySnapshotStatus==7U);
  /* A non-key upper bit is also owned by another consumer. */
  Model.Pmic[0x91c]=0x8005U;
  V2Job(MtkPmicBrokerAckKeySnapshot,Snap,MtkPmicBrokerResultSiblingPending);
  CHECK(Model.WriteCommandEffects==Writes && Core.KeySnapshotValid);
  Model.Pmic[0x91c]=5U;
  V2Job(MtkPmicBrokerAckKeySnapshot,Snap,MtkPmicBrokerResultSuccess);
  CHECK(Model.Pmic[0x91c]==0U && Model.WriteData==5U);
  CHECK(!Core.KeySnapshotValid);
  V2Job(MtkPmicBrokerReleaseKeyLease,Lease,MtkPmicBrokerResultSuccess);
  ResetHarness();
  unsigned Reads=Model.ReadCalls;Writes=Model.WriteCalls;
  CHECK(SendVersion(1U,MtkPmicBrokerAcquireKeyLease,0,0,0,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(2U,MtkPmicBrokerBeginRead,0,0x910U,0,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(2U,MtkPmicBrokerBeginWrite,0,0x91cU,5U,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(2U,MtkPmicBrokerAcquireKeyLease,1U,0,0,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(2U,MtkPmicBrokerAcquireKeyLease,0,0x910U,5U,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(Reads==Model.ReadCalls && Writes==Model.WriteCalls);
  CHECK(!MtkPmicBrokerWacsWriteAllowed(&Core,WACS_CMD,0x2000091cU));
  CHECK(!MtkPmicBrokerWacsWriteAllowed(&Core,WACS_WDATA,5U));
}
static void TestV2AcceptedFailureAndCleanupReserve(void) {
  for (unsigned Which=0;Which<2U;Which++) {
    ResetHarness();
    uint64_t Lease=V2Job(MtkPmicBrokerAcquireKeyLease,0,MtkPmicBrokerResultSuccess);
    uint64_t Token=Lease;
    MTK_PMIC_BROKER_OPERATION Op=MtkPmicBrokerReleaseKeyLease;
    if (Which==0U) {
      Model.Pmic[0x19e]=4U;Model.Pmic[0x91c]=5U;Model.Pmic[0x28]=2U;
      Token=V2Job(MtkPmicBrokerBeginKeySnapshot,Lease,MtkPmicBrokerResultSuccess);
      Op=MtkPmicBrokerAckKeySnapshot;
    }
    Model.FailWriteOffset=WACS_CMD;Model.FailWriteValueMatch=1;
    Model.FailWriteValue=Which?0x20000910U:0x2000091cU;
    Model.FailWriteOnce=1;Model.FailAfterEffect=1;
    MTK_PMIC_BROKER_REQUEST R;
    CHECK(SendVersion(2U,Op,Token,0,0,0,&R)==MtkPmicBrokerResultInProgress);
    CHECK(DriveVersionToTerminal(2U,R.Generation,NULL)==MtkPmicBrokerResultIoError);
    CHECK(Core.State==MtkPmicBrokerStateFault && Core.KeyLeaseToken==Lease);
    CHECK(Which || (Core.KeySnapshotValid && Core.KeyAckWriteMayHaveEffect));
    unsigned Reads=Model.ReadCalls,Writes=Model.WriteCalls;
    CHECK(StepVersion(2U,R.Generation,0,NULL)==MtkPmicBrokerResultIoError);
    CHECK(AckVersion(2U,R.Generation)==MtkPmicBrokerResultWrongState);
    CHECK(Reads==Model.ReadCalls && Writes==Model.WriteCalls && MtkPmicBrokerBlocksUfs(&Core));
    V2CapabilityNoEffects();
  }
  ResetHarness();
  uint64_t Lease=V2Job(MtkPmicBrokerAcquireKeyLease,0,MtkPmicBrokerResultSuccess);
  Model.Pmic[0x19e]=4U;Model.Pmic[0x91c]=5U;Model.Pmic[0x28]=2U;
  uint64_t Snap=V2Job(MtkPmicBrokerBeginKeySnapshot,Lease,MtkPmicBrokerResultSuccess);
  Core.Generation=UINT64_MAX-2U;
  CHECK(SendVersion(1U,MtkPmicBrokerBeginRead,0,0x1520U,0,0,NULL)==MtkPmicBrokerResultExhausted);
  V2Job(MtkPmicBrokerAckKeySnapshot,Snap,MtkPmicBrokerResultSuccess);
  CHECK(Core.Generation==UINT64_MAX-1U);
  V2Job(MtkPmicBrokerReleaseKeyLease,Lease,MtkPmicBrokerResultSuccess);
  CHECK(Core.Generation==UINT64_MAX && !Core.KeyLeaseToken && !Core.KeySnapshotValid);
  V2CapabilityNoEffects();
}

static const uint32_t SdVersion=MTK_PMIC_BROKER_VERSION_SD_POWER;

static uint64_t
SdPowerJob(MTK_PMIC_BROKER_OPERATION Operation,uint64_t Lease,MTK_PMIC_BROKER_RESULT Expected)
{
  MTK_PMIC_BROKER_REQUEST Request;
  CHECK(SendVersion(SdVersion,Operation,Lease,0,0,0,&Request)==MtkPmicBrokerResultInProgress);
  uint64_t Job=Request.Generation;
  CHECK(Job!=0 && Core.State==MtkPmicBrokerStatePending && MtkPmicBrokerBlocksUfs(&Core));
  CHECK(StepVersion(SdVersion,Job,1,NULL)==MtkPmicBrokerResultBusy);
  CHECK(StepVersion(2U,Job,0,NULL)==MtkPmicBrokerResultWrongState);
  CHECK(DriveVersionToTerminal(SdVersion,Job,&Request)==Expected);
  if (Core.State==MtkPmicBrokerStateDone) {
    unsigned Reads=Model.ReadCalls,Writes=Model.WriteCalls;
    CHECK(!MtkPmicBrokerBlocksUfs(&Core));
    CHECK(StepVersion(SdVersion,Job,0,NULL)==Expected);
    CHECK(AckVersion(SdVersion,Job)==MtkPmicBrokerResultSuccess);
    CHECK(Reads==Model.ReadCalls && Writes==Model.WriteCalls);
  }
  return Job;
}

static void
CheckSdWrites(const uint32_t *Addresses,const uint16_t *Values,unsigned Count)
{
  unsigned Index=0;
  uint32_t Value=0;
  uint64_t VmchDisabled=0;
  for (unsigned I=0;I<Model.TraceCount;++I) {
    const TRACE_EVENT *Event=&Model.Trace[I];
    if (Event->Kind!='w') continue;
    if (Event->Offset==WACS_WDATA) Value=Event->Value;
    if (Event->Offset!=WACS_CMD || (Event->Value&0x20000000U)==0U) continue;
    CHECK(Index<Count && Event->Value==(0x20000000U|Addresses[Index]) && Value==Values[Index]);
    if (Addresses[Index]==0x1CD8U && (Value&1U)==0U) VmchDisabled=Event->TimeUs;
    if (Addresses[Index]==0x1D70U && (Value&1U)==0U && VmchDisabled)
      CHECK(Event->TimeUs-VmchDisabled>=1500U);
    ++Index;
  }
  CHECK(Index==Count);
}

static void
TestSdPowerLease(void)
{
  MTK_PMIC_BROKER_REQUEST Request;
  ResetHarness();
  Model.TimeIncrement=100U;
  CHECK(SendVersion(SdVersion,MtkPmicBrokerQuery,0,0,0,1,&Request)==MtkPmicBrokerResultSuccess);
  CHECK(Request.Flags==MTK_PMIC_BROKER_CAP_SD_POWER_LEASE && !Request.Generation);
  CHECK(SendVersion(2U,MtkPmicBrokerAcquireSdPower,0,0,0,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(SdVersion,MtkPmicBrokerAcquireSdPower,1U,0,0,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(SdVersion,MtkPmicBrokerAcquireSdPower,0,0,0,1,NULL)==MtkPmicBrokerResultBusy);
  CHECK(SendVersion(SdVersion,MtkPmicBrokerBeginWrite,0,0x1CD8U,1U,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(Send(MtkPmicBrokerBeginWrite,0,0x1CD8U,1U,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(Model.ReadCalls==0 && Model.WriteCalls==0);

  Model.Pmic[0x1e48]=0xA200U; Model.Pmic[0x1e4c]=0xB400U;
  Model.Pmic[0x1d70]=0x84U; Model.Pmic[0x1cd8]=0x20U; Model.Pmic[0x1cc4]=0x40U;
  uint64_t Lease=SdPowerJob(MtkPmicBrokerAcquireSdPower,0,MtkPmicBrokerResultSuccess);
  CHECK(Core.SdPowerToken==Lease);
  static const uint32_t EnableAddress[]={0x1e48,0x1e4c,0x1d70,0x1cd8,0x1d70,0x1cc4};
  static const uint16_t EnableValue[]={0xA500,0xBD00,0x80,0x21,0x81,0x41};
  CheckSdWrites(EnableAddress,EnableValue,6);
  CHECK(SendVersion(SdVersion,MtkPmicBrokerAcquireSdPower,0,0,0,0,NULL)==MtkPmicBrokerResultBusy);
  CHECK(SendVersion(SdVersion,MtkPmicBrokerReleaseSdPower,Lease+1,0,0,0,NULL)==MtkPmicBrokerResultStaleGeneration);
  Model.Pmic[0x1e48]^=0x1000U;
  Model.Pmic[0x1d70]^=0x10U;
  Model.TraceCount=0;
  uint64_t Restore=SdPowerJob(MtkPmicBrokerReleaseSdPower,Lease,MtkPmicBrokerResultSuccess);
  static const uint32_t RestoreAddress[]={0x1cc4,0x1cd8,0x1d70,0x1e4c,0x1e48};
  static const uint16_t RestoreValue[]={0x40,0x20,0x94,0xB400,0xB200};
  CheckSdWrites(RestoreAddress,RestoreValue,5);
  unsigned Reads=Model.ReadCalls,Writes=Model.WriteCalls;
  CHECK(SendVersion(SdVersion,MtkPmicBrokerReleaseSdPower,Lease,0,0,0,&Request)==MtkPmicBrokerResultSuccess);
  CHECK(Request.Generation==Restore && !Core.SdPowerToken);
  CHECK(Reads==Model.ReadCalls && Writes==Model.WriteCalls);
}

static void
TestSdPowerInheritedAndFailures(void)
{
  for (unsigned Case=0;Case<4;++Case) {
    ResetHarness();
    Model.TimeIncrement=100U;
    Model.Pmic[0x1e48]=Case==1?0x200U:0x500U;
    Model.Pmic[0x1d70]=Case==2?5U:1U;
    Model.Pmic[0x1cd8]=1U;
    Model.Pmic[0x1e4c]=Case==3?0x400U:0xD00U;
    Model.Pmic[0x1cc4]=1U;
    uint64_t Lease=SdPowerJob(MtkPmicBrokerAcquireSdPower,0,
                              Case==0?MtkPmicBrokerResultSuccess:MtkPmicBrokerResultWrongState);
    if (Case==0) SdPowerJob(MtkPmicBrokerReleaseSdPower,Lease,MtkPmicBrokerResultSuccess);
    CHECK(Model.WriteCommandEffects==0 && Core.SdPowerToken==0 && !Core.SdPowerFault);
  }

  ResetHarness();
  Model.TimeIncrement=100U;
  uint64_t Lease=SdPowerJob(MtkPmicBrokerAcquireSdPower,0,MtkPmicBrokerResultSuccess);
  Model.Pmic[0x1e4c]=0x400U;
  unsigned Writes=Model.WriteCommandEffects;
  SdPowerJob(MtkPmicBrokerReleaseSdPower,Lease,MtkPmicBrokerResultIoError);
  CHECK(Core.SdPowerFault && Model.WriteCommandEffects==Writes);
  CHECK(SendVersion(SdVersion,MtkPmicBrokerReleaseSdPower,Lease,0,0,0,NULL)==MtkPmicBrokerResultWrongState);

  ResetHarness();
  Model.TimeIncrement=100U;
  Model.IgnoreWriteAddress=0x1cc4U;
  SdPowerJob(MtkPmicBrokerAcquireSdPower,0,MtkPmicBrokerResultIoError);
  CHECK(Core.SdPowerFault && !Core.SdPowerToken && !MtkPmicBrokerBlocksUfs(&Core));

  ResetHarness();
  Model.TimeIncrement=100U;
  Model.FailWriteOffset=WACS_CMD;
  Model.FailWriteValueMatch=1;
  Model.FailWriteValue=0x20001cd8U;
  Model.FailWriteOnce=1;
  Model.FailAfterEffect=1;
  uint64_t Job=SdPowerJob(MtkPmicBrokerAcquireSdPower,0,MtkPmicBrokerResultIoError);
  Writes=Model.WriteCommandEffects;
  CHECK(Model.Pmic[0x1cd8]==1U && Model.Pmic[0x1cc4]==0U);
  CHECK(Core.State==MtkPmicBrokerStateFault && MtkPmicBrokerBlocksUfs(&Core));
  CHECK(StepVersion(SdVersion,Job,0,NULL)==MtkPmicBrokerResultIoError);
  CHECK(AckVersion(SdVersion,Job)==MtkPmicBrokerResultWrongState);
  CHECK(Writes==Model.WriteCommandEffects && !Core.SdPowerToken);
}

static void
TestSdPowerGenerationReserve(void)
{
  ResetHarness();
  Model.TimeIncrement=100U;
  uint64_t Key=V2Job(MtkPmicBrokerAcquireKeyLease,0,MtkPmicBrokerResultSuccess);
  uint64_t Capture=V2Job(MtkPmicBrokerBeginKeySnapshot,Key,MtkPmicBrokerResultSuccess);
  Core.Generation=UINT64_MAX-4U;
  uint64_t Sd=SdPowerJob(MtkPmicBrokerAcquireSdPower,0,MtkPmicBrokerResultSuccess);
  CHECK(Sd==UINT64_MAX-3U);
  CHECK(Send(MtkPmicBrokerBeginRead,0,0x1520U,0,0,NULL)==MtkPmicBrokerResultExhausted);
  V2Job(MtkPmicBrokerAckKeySnapshot,Capture,MtkPmicBrokerResultSuccess);
  V2Job(MtkPmicBrokerReleaseKeyLease,Key,MtkPmicBrokerResultSuccess);
  CHECK(SdPowerJob(MtkPmicBrokerReleaseSdPower,Sd,MtkPmicBrokerResultSuccess)==UINT64_MAX);
}

static const uint16_t ConnVersion=MTK_PMIC_BROKER_VERSION_CONN_RAILS;

static uint64_t
AcquireConnLease(void)
{
  MTK_PMIC_BROKER_REQUEST Reply;
  unsigned Writes=Model.WriteCalls, Reads=Model.ReadCalls;
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerAcquireConnRails,0,0,0,0,&Reply)==MtkPmicBrokerResultSuccess);
  CHECK(Reply.State==MtkPmicBrokerStateDone && Reply.Generation && !Reply.Address && !Reply.Value);
  CHECK(AckVersion(ConnVersion,Reply.Generation)==MtkPmicBrokerResultSuccess);
  CHECK(!MtkPmicBrokerBlocksUfs(&Core) && Model.WriteCalls==Writes && Model.ReadCalls==Reads);
  return Reply.Generation;
}

static MTK_PMIC_BROKER_REQUEST
ConnJob(MTK_PMIC_BROKER_OPERATION Operation,uint64_t Lease,uint32_t Address,uint32_t Value,
        MTK_PMIC_BROKER_RESULT Expected)
{
  MTK_PMIC_BROKER_REQUEST Reply;
  CHECK(SendVersion(ConnVersion,Operation,Lease,Address,Value,0,&Reply)==MtkPmicBrokerResultInProgress);
  uint64_t Job=Reply.Generation;
  CHECK(Job>Lease && Reply.State==MtkPmicBrokerStatePending && MtkPmicBrokerBlocksUfs(&Core));
  CHECK(DriveVersionToTerminal(ConnVersion,Job,&Reply)==Expected);
  CHECK(Reply.Address==Address && Reply.Generation==Job && Reply.State==MtkPmicBrokerStateDone);
  CHECK(!MtkPmicBrokerBlocksUfs(&Core));
  CHECK(AckVersion(ConnVersion,Job)==MtkPmicBrokerResultSuccess);
  return Reply;
}

static void
TestConnLease(void)
{
  MTK_PMIC_BROKER_REQUEST Reply;
  ResetHarness();
  Model.TimeIncrement=100U;
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerQuery,0,0,0,0,&Reply)==MtkPmicBrokerResultSuccess);
  CHECK(Reply.Flags==MTK_PMIC_BROKER_CAP_CONN_RAIL_LEASE && !Reply.Generation);
  CHECK(SendVersion(3U,MtkPmicBrokerAcquireConnRails,0,0,0,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerBeginConnRead,1,0x1C58U,0,0,NULL)==MtkPmicBrokerResultWrongState);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerAcquireConnRails,0,0,0,1,NULL)==MtkPmicBrokerResultBusy);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerAcquireConnRails,0,0,1,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(!Model.WriteCalls && !Model.ReadCalls);
  uint64_t Lease=AcquireConnLease();
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerAcquireConnRails,0,0,0,0,NULL)==MtkPmicBrokerResultBusy);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerBeginConnRead,Lease+1,0x1C58U,0,0,NULL)==MtkPmicBrokerResultStaleGeneration);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerBeginConnRead,Lease,0x1726U,0,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerBeginConnRead,Lease,0x1CD8U,0,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerBeginConnWrite,Lease,0x1D2AU,1,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerBeginConnWrite,Lease,0x1724U,0x10U,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerBeginConnWrite,Lease,0x1726U,0x11U,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(1U,MtkPmicBrokerBeginWrite,0,0x1E8CU,0xC00U,0,NULL)==MtkPmicBrokerResultInvalid);
  CHECK(!Model.WriteCalls && !Model.ReadCalls);
  CHECK(MtkPmicBrokerConnAddressAllowed(0x1728U) && MtkPmicBrokerConnAddressAllowed(0x1D2AU));
  CHECK(!MtkPmicBrokerConnAddressAllowed(0x1CD8U) && !MtkPmicBrokerConnAddressAllowed(0x1C59U));

  Model.Pmic[0x1E8CU]=0xB352U;
  Model.Pmic[0x1D2AU]=0x1001U;
  Reply=ConnJob(MtkPmicBrokerBeginConnRead,Lease,0x1D2AU,0,MtkPmicBrokerResultSuccess);
  CHECK(Reply.Value==0x1001U && !Model.WriteCommandEffects);
  ConnJob(MtkPmicBrokerBeginConnWrite,Lease,0x1E8CU,0xBC51U,MtkPmicBrokerResultInvalid);
  ConnJob(MtkPmicBrokerBeginConnWrite,Lease,0x1E8CU,0xCC50U,MtkPmicBrokerResultInvalid);
  CHECK(Model.Pmic[0x1E8CU]==0xB352U && !Model.WriteCommandEffects && !Core.ConnDirty);
  ConnJob(MtkPmicBrokerBeginConnWrite,Lease,0x1E8CU,0xBC50U,MtkPmicBrokerResultSuccess);
  CHECK(Model.Pmic[0x1E8CU]==0xBC50U && Model.WriteCommandEffects==1U && Core.ConnDirty);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerReleaseConnRails,Lease,0,0,0,NULL)==MtkPmicBrokerResultWrongState);

  /* Key IRQ lease and SD rail jobs still work while CONN owns its fields. */
  uint64_t Key=V2Job(MtkPmicBrokerAcquireKeyLease,0,MtkPmicBrokerResultSuccess);
  uint64_t Sd=SdPowerJob(MtkPmicBrokerAcquireSdPower,0,MtkPmicBrokerResultSuccess);
  CHECK(Core.SdPowerToken==Sd && Core.KeyLeaseToken==Key && Core.ConnLeaseToken==Lease);
  V2Job(MtkPmicBrokerReleaseKeyLease,Key,MtkPmicBrokerResultSuccess);
  SdPowerJob(MtkPmicBrokerReleaseSdPower,Sd,MtkPmicBrokerResultSuccess);
  CHECK(Model.Pmic[0x1E8CU]==0xBC50U && Model.Pmic[0x1D2AU]==0x1001U);

  Model.IgnoreWriteAddress=0x1E8CU;
  ConnJob(MtkPmicBrokerBeginConnWrite,Lease,0x1E8CU,0xB352U,MtkPmicBrokerResultIoError);
  CHECK(Core.ConnDirty && Core.ConnLeaseToken==Lease);
  Model.IgnoreWriteAddress=0;
  ConnJob(MtkPmicBrokerBeginConnWrite,Lease,0x1E8CU,0xB352U,MtkPmicBrokerResultSuccess);
  CHECK(!Core.ConnDirty);
  unsigned Writes=Model.WriteCalls;
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerReleaseConnRails,Lease,0,0,1,&Reply)==MtkPmicBrokerResultSuccess);
  CHECK(!Reply.State && Reply.Generation==Lease && !Reply.Address && !Reply.Value && !Core.ConnLeaseToken);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerReleaseConnRails,Lease,0,0,0,NULL)==MtkPmicBrokerResultSuccess);
  CHECK(Model.WriteCalls==Writes && !MtkPmicBrokerBlocksUfs(&Core));
  uint64_t Next=AcquireConnLease();
  CHECK(Next>Lease && !Core.ConnReleasedToken);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerReleaseConnRails,Lease,0,0,0,NULL)==MtkPmicBrokerResultStaleGeneration);
  CHECK(Core.ConnLeaseToken==Next);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerReleaseConnRails,Next,0,0,0,NULL)==MtkPmicBrokerResultSuccess);
}

static void
TestConnVs2VoteAndFault(void)
{
  ResetHarness();
  uint64_t Lease=AcquireConnLease();
  Model.Pmic[0x1724U]=0x25U;
  ConnJob(MtkPmicBrokerBeginConnWrite,Lease,0x1726U,0x10U,MtkPmicBrokerResultSuccess);
  CHECK(Model.Pmic[0x1724U]==0x35U && Core.ConnDirty);
  ConnJob(MtkPmicBrokerBeginConnWrite,Lease,0x1728U,0x10U,MtkPmicBrokerResultSuccess);
  CHECK(Model.Pmic[0x1724U]==0x25U && !Core.ConnDirty);
  uint32_t Reads[4];
  CHECK(GetReadCommandAddresses(Reads,4)==4);
  for (unsigned Index=0;Index<4;++Index) CHECK(Reads[Index]==0x1724U);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerReleaseConnRails,Lease,0,0,0,NULL)==MtkPmicBrokerResultSuccess);
  Lease=AcquireConnLease();
  Model.Pmic[0x1724U]|=0x10U;
  ConnJob(MtkPmicBrokerBeginConnWrite,Lease,0x1728U,0x10U,MtkPmicBrokerResultInvalid);
  CHECK(Model.Pmic[0x1724U]==0x35U && !Core.ConnDirty);

  MTK_PMIC_BROKER_REQUEST Reply;
  Model.FailWriteOnce=1;
  Model.FailWriteOffset=WACS_CMD;
  Model.FailWriteValue=0x20001C58U;
  Model.FailWriteValueMatch=1;
  Model.FailAfterEffect=1;
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerBeginConnWrite,Lease,0x1C58U,1,0,&Reply)==MtkPmicBrokerResultInProgress);
  uint64_t Job=Reply.Generation;
  CHECK(StepVersion(1U,Job,0,NULL)==MtkPmicBrokerResultWrongState);
  CHECK(DriveVersionToTerminal(ConnVersion,Job,&Reply)==MtkPmicBrokerResultIoError);
  CHECK(Reply.State==MtkPmicBrokerStateFault && Core.ConnDirty && MtkPmicBrokerBlocksUfs(&Core));
  CHECK(Model.Pmic[0x1C58U]==1U);
  unsigned Writes=Model.WriteCalls;
  CHECK(StepVersion(ConnVersion,Job,0,NULL)==MtkPmicBrokerResultIoError);
  CHECK(AckVersion(ConnVersion,Job)==MtkPmicBrokerResultWrongState);
  CHECK(SendVersion(ConnVersion,MtkPmicBrokerReleaseConnRails,Lease,0,0,0,NULL)==MtkPmicBrokerResultBusy);
  CHECK(Model.WriteCalls==Writes && Core.ConnLeaseToken==Lease);
}

static const uint16_t GaugeVersion=MTK_PMIC_BROKER_VERSION_GAUGE;

static uint64_t
AcquireGaugeLease(void)
{
  MTK_PMIC_BROKER_REQUEST Reply;
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerAcquireGaugeLease,0,0,0,0,&Reply)==
        MtkPmicBrokerResultSuccess);
  CHECK(Reply.State==MtkPmicBrokerStateDone && Reply.Generation!=0U && !Reply.Address && !Reply.Value);
  uint64_t Lease=Reply.Generation;
  CHECK(AckVersion(GaugeVersion,Lease)==MtkPmicBrokerResultSuccess);
  CHECK(Core.GaugeLeaseToken==Lease && MtkPmicBrokerBlocksUfs(&Core));
  return Lease;
}

static MTK_PMIC_BROKER_RESULT
GaugeJob(MTK_PMIC_BROKER_OPERATION Operation,uint64_t Lease,uint32_t Address,
         uint32_t Value,MTK_PMIC_BROKER_RESULT Expected,MTK_PMIC_BROKER_REQUEST *Final)
{
  MTK_PMIC_BROKER_REQUEST Reply;
  CHECK(SendVersion(GaugeVersion,Operation,Lease,Address,Value,0,&Reply)==
        MtkPmicBrokerResultInProgress);
  uint64_t Job=Reply.Generation;
  MTK_PMIC_BROKER_RESULT Result=DriveVersionToTerminal(GaugeVersion,Job,&Reply);
  CHECK(Result==Expected);
  if (Core.State==MtkPmicBrokerStateDone)
    CHECK(AckVersion(GaugeVersion,Job)==MtkPmicBrokerResultSuccess);
  if (Final) *Final=Reply;
  return Result;
}

static void
TestGaugeLeaseAndRegisterScope(void)
{
  MTK_PMIC_BROKER_REQUEST Reply;
  ResetHarness();
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerQuery,0,0,0,0,&Reply)==
        MtkPmicBrokerResultSuccess);
  CHECK(Reply.Flags==(MTK_PMIC_BROKER_CAP_CONN_RAIL_LEASE|MTK_PMIC_BROKER_CAP_GAUGE_LEASE));
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerAcquireGaugeLease,0,0,0,1,NULL)==
        MtkPmicBrokerResultBusy);
  CHECK(!Core.GaugeLeaseToken);

  uint64_t Lease=AcquireGaugeLease();
  CHECK(SendVersion(1U,MtkPmicBrokerBeginRead,0,0x1520U,0,0,NULL)==MtkPmicBrokerResultBusy);
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerBeginGaugeRead,Lease+1U,0x0D8AU,0,0,NULL)==
        MtkPmicBrokerResultStaleGeneration);
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerBeginGaugeRead,Lease,0x91CU,0,0,NULL)==
        MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerBeginGaugeWrite,Lease,0x0D8AU,1,0,NULL)==
        MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerBeginGaugeWrite,Lease,0x0D0AU,2,0,NULL)==
        MtkPmicBrokerResultInvalid);

  Model.Pmic[0x0D0AU]=0U;
  GaugeJob(MtkPmicBrokerBeginGaugeRead,Lease,0x0D0AU,0,
           MtkPmicBrokerResultSuccess,&Reply);
  CHECK(Reply.Value==0U);
  Model.Pmic[0x0D8AU]=0xA55AU;
  GaugeJob(MtkPmicBrokerBeginGaugeRead,Lease,0x0D8AU,0,
           MtkPmicBrokerResultSuccess,&Reply);
  CHECK(Reply.Value==0xA55AU);

  GaugeJob(MtkPmicBrokerBeginGaugeWrite,Lease,0x0D0AU,1U,
           MtkPmicBrokerResultSuccess,&Reply);
  CHECK(Model.Pmic[0x0D0AU]==1U);
  Model.Pmic[0x0D0AU]=0x8001U;
  GaugeJob(MtkPmicBrokerBeginGaugeWrite,Lease,0x0D0AU,0x8008U,
           MtkPmicBrokerResultSuccess,&Reply);
  CHECK(Model.Pmic[0x0D0AU]==0x8008U);
  Model.Pmic[0x0D0AU]=8U;
  GaugeJob(MtkPmicBrokerBeginGaugeWrite,Lease,0x0D0AU,0U,
           MtkPmicBrokerResultSuccess,&Reply);
  CHECK(Model.Pmic[0x0D0AU]==0U);

  static const struct {uint32_t Output,Request,Raw;} Adc[]={
    {0x1088U,1U,0x9234U}, {0x108EU,8U,0x85ABU}, {0x1090U,16U,0x89CDU}
  };
  for (uint32_t Index=0;Index<sizeof(Adc)/sizeof(Adc[0]);++Index) {
    GaugeJob(MtkPmicBrokerBeginGaugeWrite,Lease,0x1108U,Adc[Index].Request,
             MtkPmicBrokerResultSuccess,&Reply);
    CHECK(Model.Pmic[0x1108U]==Adc[Index].Request && Reply.Value==Adc[Index].Request);
    Model.Pmic[Adc[Index].Output]=Adc[Index].Raw;
    GaugeJob(MtkPmicBrokerBeginGaugeRead,Lease,Adc[Index].Output,0,
             MtkPmicBrokerResultSuccess,&Reply);
    CHECK(Reply.Value==Adc[Index].Raw);
  }
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerBeginGaugeRead,Lease,0x108AU,0,0,NULL)==
        MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerBeginGaugeRead,Lease,0x1092U,0,0,NULL)==
        MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerBeginGaugeWrite,Lease,0x1108U,0U,0,NULL)==
        MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerBeginGaugeWrite,Lease,0x1108U,2U,0,NULL)==
        MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerBeginGaugeWrite,Lease,0x1108U,9U,0,NULL)==
        MtkPmicBrokerResultInvalid);
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerBeginGaugeWrite,Lease,0x1108U,0x8001U,0,NULL)==
        MtkPmicBrokerResultInvalid);

  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerReleaseGaugeLease,Lease+1U,0,0,0,NULL)==
        MtkPmicBrokerResultStaleGeneration);
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerReleaseGaugeLease,Lease,0,0,0,&Reply)==
        MtkPmicBrokerResultSuccess);
  CHECK(!Core.GaugeLeaseToken && !MtkPmicBrokerBlocksUfs(&Core));
  CHECK(SendVersion(GaugeVersion,MtkPmicBrokerReleaseGaugeLease,Lease,0,0,0,&Reply)==
        MtkPmicBrokerResultSuccess);
  CHECK(SendVersion(1U,MtkPmicBrokerBeginRead,0,0x0D8AU,0,0,NULL)==
        MtkPmicBrokerResultInvalid);
}

int
main(void)
{
  TestGaugeLeaseAndRegisterScope();
  TestConnLease();
  TestConnVs2VoteAndFault();
  TestSdPowerLease();
  TestSdPowerInheritedAndFailures();
  TestSdPowerGenerationReserve();
  TestV2LeaseCaptureAndRestore();
  TestV2SiblingRetryAndProtocolGuards();
  TestV2AcceptedFailureAndCleanupReserve();
  TestWireAndInvalidRequests();
  TestKeySnapshotOrderResultAndMetadataAck();
  TestKeySnapshotSkipsAbsentGroup();
  TestKeySnapshotInvalidInputsAndBusyOwnership();
  TestKeySnapshotTimeoutAndFaultPreservation();
  TestReadAllowlistAndAck();
  TestOtherOwnerAndUnknownResponse();
  TestDelayedReadValid();
  TestWriteMasksAndReadback();
  TestAckAndGenerationRules();
  TestNoReplayAfterCommandOrClearFailure();
  TestBlocksUfsFailsClosedForCorruptJournal();
  TestUfsOwnerAndFaultBlocking();
  printf("MtkPmicBrokerTest: %u checks; journal=%zu bytes\n", Checks, sizeof(Core));
  return 0;
}
