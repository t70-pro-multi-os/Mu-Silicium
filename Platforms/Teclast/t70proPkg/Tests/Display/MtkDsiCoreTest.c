/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Library/MtkDsiCore.h>

static unsigned Checks;
#define CHECK(Expression) do { \
  Checks++; \
  if (!(Expression)) { \
    fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #Expression); \
    abort (); \
  } \
} while (0)

#include "MtkDsiFixture.h"

#define MAX_TRACE 768U
#define MAX_REGISTERS 64U

typedef enum {
  TraceRead = 1,
  TraceWrite,
  TraceDelay
} TRACE_KIND;

typedef struct {
  TRACE_KIND Kind;
  MTK_DSI_BANK Bank;
  uint32_t Offset;
  uint32_t Before;
  uint32_t Value;
} TRACE_ENTRY;

typedef struct {
  MTK_DSI_BANK Bank;
  uint32_t Offset;
  uint32_t Value;
  uint8_t Used;
} REGISTER_ENTRY;

typedef struct {
  MTK_DSI_CORE *Core;
  const MTK_DSI_OP7_PROFILE *Profile;
  TRACE_ENTRY Trace[MAX_TRACE];
  REGISTER_ENTRY Registers[MAX_REGISTERS];
  unsigned TraceCount;
  unsigned ReadCalls;
  unsigned WriteCalls;
  unsigned DelayCalls;
  unsigned FailReadAt;
  unsigned FailWriteAt;
  unsigned FailDelayAt;
  unsigned ExternalChangeReadAt;
  MTK_DSI_BANK ExternalChangeBank;
  uint32_t ExternalChangeOffset;
  uint32_t ExternalChangeValue;
  unsigned BusyReads;
  uint8_t ModelIdle;
  uint8_t BusyForever;
  uint8_t ApplyFailedWrite;
  uint8_t ReenterAtFirstWrite;
  uint8_t MutateProfileAtFirstWrite;
  MTK_DSI_STATUS ReentrantStatus;
} DSI_MOCK;

static void
Record (
  DSI_MOCK *Mock,
  TRACE_KIND Kind,
  MTK_DSI_BANK Bank,
  uint32_t Offset,
  uint32_t Before,
  uint32_t Value
  )
{
  CHECK (Mock->TraceCount < MAX_TRACE);
  Mock->Trace[Mock->TraceCount].Kind = Kind;
  Mock->Trace[Mock->TraceCount].Bank = Bank;
  Mock->Trace[Mock->TraceCount].Offset = Offset;
  Mock->Trace[Mock->TraceCount].Before = Before;
  Mock->Trace[Mock->TraceCount].Value = Value;
  Mock->TraceCount++;
}

static void
SetParam32 (
  uint32_t Offset,
  uint32_t Value
  )
{
  GetParams[Offset] = (MTK_DSI_U8)(Value & 0xFFU);
  GetParams[Offset + 1U] = (MTK_DSI_U8)((Value >> 8) & 0xFFU);
  GetParams[Offset + 2U] = (MTK_DSI_U8)((Value >> 16) & 0xFFU);
  GetParams[Offset + 3U] = (MTK_DSI_U8)((Value >> 24) & 0xFFU);
}

static REGISTER_ENTRY *
FindRegister (
  DSI_MOCK *Mock,
  MTK_DSI_BANK Bank,
  uint32_t Offset,
  int Create
  )
{
  unsigned Index;
  REGISTER_ENTRY *FreeEntry;

  FreeEntry = 0;
  for (Index = 0U; Index < MAX_REGISTERS; ++Index) {
    if (Mock->Registers[Index].Used == 0U) {
      if (FreeEntry == 0) {
        FreeEntry = &Mock->Registers[Index];
      }
      continue;
    }

    if ((Mock->Registers[Index].Bank == Bank) &&
        (Mock->Registers[Index].Offset == Offset)) {
      return &Mock->Registers[Index];
    }
  }

  if ((Create == 0) || (FreeEntry == 0)) {
    return 0;
  }

  FreeEntry->Used = 1U;
  FreeEntry->Bank = Bank;
  FreeEntry->Offset = Offset;
  FreeEntry->Value = 0U;
  return FreeEntry;
}

static uint32_t
ReadRegisterValue (
  DSI_MOCK *Mock,
  MTK_DSI_BANK Bank,
  uint32_t Offset
  )
{
  REGISTER_ENTRY *Entry;

  Entry = FindRegister (Mock, Bank, Offset, 0);
  return (Entry == 0) ? 0U : Entry->Value;
}

static MTK_DSI_STATUS
MockRead32 (
  void *Context,
  MTK_DSI_BANK Bank,
  MTK_DSI_U32 Offset,
  MTK_DSI_U32 *Value
  )
{
  DSI_MOCK *Mock;
  REGISTER_ENTRY *Entry;
  uint32_t Current;

  Mock = (DSI_MOCK *)Context;
  Mock->ReadCalls++;
  if (Mock->ReadCalls == Mock->ExternalChangeReadAt) {
    Entry = FindRegister (Mock, Mock->ExternalChangeBank,
                          Mock->ExternalChangeOffset, 1);
    CHECK (Entry != 0);
    Entry->Value = Mock->ExternalChangeValue;
  }

  if (Mock->ReadCalls == Mock->FailReadAt) {
    Record (Mock, TraceRead, Bank, Offset, 0U, 0U);
    return MtkDsiIoError;
  }

  Entry = FindRegister (Mock, Bank, Offset, 1);
  CHECK (Entry != 0);
  if (Mock->ModelIdle && Bank == MtkDsiBankDsi0 && Offset == 0x1300CU) {
    if (Mock->BusyReads || Mock->BusyForever) {
      Entry->Value |= 0x80000000U;
      if (Mock->BusyReads) Mock->BusyReads--;
    } else {
      Entry->Value &= 0x7FFFFFFFU;
    }
  }
  Current = Entry->Value;
  *Value = Current;
  Record (Mock, TraceRead, Bank, Offset, Current, Current);
  return MtkDsiSuccess;
}

static MTK_DSI_STATUS
MockWrite32 (
  void *Context,
  MTK_DSI_BANK Bank,
  MTK_DSI_U32 Offset,
  MTK_DSI_U32 Value
  )
{
  DSI_MOCK *Mock;
  REGISTER_ENTRY *Entry;
  uint32_t Before;
  uint8_t Fail;

  Mock = (DSI_MOCK *)Context;
  Mock->WriteCalls++;
  Entry = FindRegister (Mock, Bank, Offset, 1);
  CHECK (Entry != 0);
  Before = Entry->Value;
  Record (Mock, TraceWrite, Bank, Offset, Before, Value);

  if ((Mock->ReenterAtFirstWrite != 0U) && (Mock->WriteCalls == 1U)) {
    Mock->ReenterAtFirstWrite = 0U;
    Mock->ReentrantStatus = MtkDsiCoreRunAlongCyxOp7 (
                              Mock->Core,
                              Mock->Profile
                              );
  }

  if ((Mock->MutateProfileAtFirstWrite != 0U) &&
      (Mock->WriteCalls == 1U)) {
    Mock->MutateProfileAtFirstWrite = 0U;
    ((MTK_DSI_OP7_PROFILE *)Mock->Profile)->Mode = 1U;
    ((MTK_DSI_OP7_PROFILE *)Mock->Profile)->Initial.Reg08 = 0xDEADBEEFU;
  }

  Fail = (uint8_t)(Mock->WriteCalls == Mock->FailWriteAt);
  if ((Fail == 0U) || (Mock->ApplyFailedWrite != 0U)) {
    Entry->Value = Value;
  }

  return (Fail != 0U) ? MtkDsiIoError : MtkDsiSuccess;
}

static MTK_DSI_STATUS
MockDelayMicroseconds (
  void *Context,
  MTK_DSI_U32 Microseconds
  )
{
  DSI_MOCK *Mock;

  Mock = (DSI_MOCK *)Context;
  Mock->DelayCalls++;
  Record (Mock, TraceDelay, MtkDsiBankDsi0, 0U, 0U, Microseconds);
  return (Mock->DelayCalls == Mock->FailDelayAt)
           ? MtkDsiDelayError
           : MtkDsiSuccess;
}

static void
SeedRegister (
  DSI_MOCK *Mock,
  MTK_DSI_BANK Bank,
  uint32_t Offset,
  uint32_t Value
  )
{
  REGISTER_ENTRY *Entry;

  Entry = FindRegister (Mock, Bank, Offset, 1);
  CHECK (Entry != 0);
  Entry->Value = Value;
}

static void
SeedRmwRegisters (
  DSI_MOCK *Mock,
  uint32_t Seed
  )
{
  SeedRegister (Mock, MtkDsiBankDsi0, 0x108U, Seed);
  SeedRegister (Mock, MtkDsiBankDsi0, 0x1A8U, Seed);
  SeedRegister (Mock, MtkDsiBankDsi0, 0x13000U, Seed);
  SeedRegister (Mock, MtkDsiBankDsi0, 0x13014U, Seed);
  SeedRegister (Mock, MtkDsiBankDsi0, 0x13104U, Seed);
  SeedRegister (Mock, MtkDsiBankDsi0, 0x13010U, Seed);
  SeedRegister (Mock, MtkDsiBankMipiTx, 0x328U, Seed);
  SeedRegister (Mock, MtkDsiBankMipiTx, 0x244U, Seed);
  SeedRegister (Mock, MtkDsiBankMipiTx, 0x444U, Seed);
  SeedRegister (Mock, MtkDsiBankMipiTx, 0x144U, Seed);
  SeedRegister (Mock, MtkDsiBankMipiTx, 0x544U, Seed);
  SeedRegister (Mock, MtkDsiBankMipiTx, 0x344U, Seed);
  SeedRegister (Mock, MtkDsiBankMipiTx, 0x28U, Seed);
  SeedRegister (Mock, MtkDsiBankMipiTx, 0x30U, Seed);
  SeedRegister (Mock, MtkDsiBankMipiTx, 0x34U, Seed);
}

static void
InitializeFixture (
  DSI_MOCK *Mock,
  MTK_DSI_CORE *Core,
  const MTK_DSI_OP7_PROFILE *Profile,
  uint32_t Seed
  )
{
  MTK_DSI_CORE_IO Io;

  *Mock = (DSI_MOCK){ 0 };
  *Core = (MTK_DSI_CORE){ 0 };
  Mock->Core = Core;
  Mock->Profile = Profile;
  SeedRmwRegisters (Mock, Seed);
  Io.Context = Mock;
  Io.Read32 = MockRead32;
  Io.Write32 = MockWrite32;
  Io.DelayMicroseconds = MockDelayMicroseconds;
  CHECK (MtkDsiCoreInitialize (Core, &Io) == MtkDsiSuccess);
}

static unsigned
FindEvent (
  const DSI_MOCK *Mock,
  TRACE_KIND Kind,
  MTK_DSI_BANK Bank,
  uint32_t Offset,
  unsigned Occurrence
  )
{
  unsigned Index;
  unsigned Seen;

  Seen = 0U;
  for (Index = 0U; Index < Mock->TraceCount; ++Index) {
    if ((Mock->Trace[Index].Kind == Kind) &&
        (Mock->Trace[Index].Bank == Bank) &&
        (Mock->Trace[Index].Offset == Offset)) {
      if (Seen == Occurrence) {
        return Index;
      }
      Seen++;
    }
  }

  return MAX_TRACE;
}

static void
CheckSuccessfulProfile (
  uint32_t Seed
  )
{
  DSI_MOCK Mock;
  MTK_DSI_CORE Core;
  MTK_DSI_OP7_PROFILE Profile;
  MTK_DSI_PORT_SHADOW Port;
  unsigned FirstResetClear;
  unsigned ResetSet;
  unsigned LastResetClear;
  unsigned DelayIndex;
  unsigned Index;
  uint32_t Expected30;

  Profile = SupportedProfile ();
  CHECK (MtkDsiCoreBuildAlongCyxConfigShadow (
           &Profile, &Profile.Initial, &Port) == MtkDsiSuccess);
  InitializeFixture (&Mock, &Core, &Profile, Seed);
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiSuccess);
  CHECK (Core.State == MtkDsiStateOp7Complete);
  CHECK (Core.ReadCalls == 20U);
  CHECK (Core.WriteAttempts == 48U);
  CHECK (Core.WritesAccepted == 48U);
  CHECK (Core.NextWriteIndex == 48U);
  CHECK (Core.DelayAttempts == 5U);
  CHECK (Mock.TraceCount == 73U);

  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x108U) ==
         (Seed | 0x00080000U));
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x1A8U) == (Seed | 1U));
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x328U) == (Seed | 1U));
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x40U) == 0x65432100U);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x44U) == 0x24232987U);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x48U) == 0x76543210U);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x4CU) == 8U);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x14U) == 0U);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x3CU) == 0x00FF12E0U);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x0CU) == 0x3FFF0080U);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x244U) == (Seed & ~1U));
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x444U) == (Seed & ~1U));
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x144U) == (Seed & ~1U));
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x544U) == (Seed & ~1U));
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x344U) == (Seed & ~1U));
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x28U) == ((Seed | 1U) & ~2U));
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x2CU) == 0x70C4EC4EU);
  Expected30 = (Seed & ~0x700U) | 0x110U;
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x30U) == Expected30);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x34U) == (Seed & ~2U));

  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13008U) == Port.Reg08);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13014U) ==
         (Port.Reg14 & ~0x00100000U));
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13018U) ==
         (Port.Reg18 & 0xFFFFFFC3U));
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x1301CU) == Port.Reg1C);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13020U) == Port.Reg20);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13024U) == Port.Reg24);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13028U) == Port.Reg28);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x1302CU) == Port.Reg2C);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13050U) == Port.Reg50);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13054U) == Port.Reg54);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13058U) == Port.Reg58);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x1305CU) == Port.Reg5C);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13064U) == Port.Reg64);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13090U) == Port.Reg90);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13110U) == Port.Reg110);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13114U) == Port.Reg114);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13118U) == Port.Reg118);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x1311CU) == Port.Reg11C);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13200U) == Port.Reg200);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13000U) == (Seed & ~4U));
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13104U) == (Seed & ~1U));
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13010U) == (Seed & ~1U));

  FirstResetClear = FindEvent (&Mock, TraceWrite, MtkDsiBankDsi0, 0x13000U, 0U);
  ResetSet = FindEvent (&Mock, TraceWrite, MtkDsiBankDsi0, 0x13000U, 1U);
  LastResetClear = FindEvent (&Mock, TraceWrite, MtkDsiBankDsi0, 0x13000U, 2U);
  CHECK (FirstResetClear < ResetSet && ResetSet < LastResetClear);
  DelayIndex = FindEvent (&Mock, TraceDelay, MtkDsiBankDsi0, 0U, 0U);
  CHECK (FindEvent (&Mock, TraceWrite, MtkDsiBankMipiTx, 0x0CU, 0U) < DelayIndex);
  CHECK (DelayIndex < FindEvent (&Mock, TraceWrite, MtkDsiBankMipiTx, 0x0CU, 1U));
  CHECK (FindEvent (&Mock, TraceWrite, MtkDsiBankMipiTx, 0x28U, 0U) <
         FindEvent (&Mock, TraceDelay, MtkDsiBankDsi0, 0U, 1U));
  CHECK (FindEvent (&Mock, TraceWrite, MtkDsiBankMipiTx, 0x30U, 0U) <
         FindEvent (&Mock, TraceDelay, MtkDsiBankDsi0, 0U, 2U));
  CHECK (FindEvent (&Mock, TraceDelay, MtkDsiBankDsi0, 0U, 2U) <
         FindEvent (&Mock, TraceWrite, MtkDsiBankMipiTx, 0x30U, 1U));
  CHECK (FindEvent (&Mock, TraceDelay, MtkDsiBankDsi0, 0U, 3U) < ResetSet);
  CHECK (ResetSet < FindEvent (&Mock, TraceDelay, MtkDsiBankDsi0, 0U, 4U));
  CHECK (FindEvent (&Mock, TraceDelay, MtkDsiBankDsi0, 0U, 4U) < LastResetClear);
  for (Index = 0U; Index < Mock.TraceCount; ++Index) {
    if (Mock.Trace[Index].Kind == TraceDelay) {
      CHECK (Mock.Trace[Index].Value == MTK_DSI_LK_DELAY_MICROSECONDS);
    }
  }

  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiWrongState);
  CHECK (Core.WriteAttempts == 48U && Mock.TraceCount == 73U);
  CHECK (MtkDsiCoreInitialize (&Core, &Core.Io) == MtkDsiWrongState);
}

static void
CheckProfileRejectsBeforeIo (
  void
  )
{
  DSI_MOCK Mock;
  MTK_DSI_CORE Core;
  MTK_DSI_OP7_PROFILE Profile;
  unsigned Index;

  Profile = SupportedProfile ();
  InitializeFixture (&Mock, &Core, &Profile, 0U);
  Profile.ModuleId++;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiInvalid);
  CHECK (Mock.TraceCount == 0U && Core.WriteAttempts == 0U);
  CHECK (Core.State == MtkDsiStateIdle);

  Profile = SupportedProfile ();
  SetParam32 (0x234U, 733U);
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiInvalid);
  CHECK (Mock.TraceCount == 0U);

  Profile = SupportedProfile ();
  Profile.Mode = 1U;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiInvalid);
  CHECK (Mock.TraceCount == 0U);

  Profile = SupportedProfile ();
  SetParam32 (0x240U, 0U);
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiInvalid);
  CHECK (Mock.TraceCount == 0U);

  Profile = SupportedProfile ();
  SetParam32 (0x624U, 1U);
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiInvalid);
  CHECK (Mock.TraceCount == 0U);

  Profile = SupportedProfile ();
  SetParam32 (0x6D8U, 1U);
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiInvalid);
  CHECK (Mock.TraceCount == 0U);

  for (Index = 0U; Index < 6U; ++Index) {
    Profile = SupportedProfile ();
    SetParam32 (0x628U + (Index * 4U), 1U);
    CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiInvalid);
    CHECK (Mock.TraceCount == 0U);
  }

  Profile = SupportedProfile ();
  Profile.ConfigWidth++;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiInvalid);
  CHECK (Mock.TraceCount == 0U);
}

static void
CheckConfigShadowBuilder (
  void
  )
{
  MTK_DSI_OP7_PROFILE Profile;
  MTK_DSI_PORT_SHADOW Output;
  MTK_DSI_PORT_SHADOW Before;
  MTK_DSI_STATUS Status;

  Profile = SupportedProfile ();
  memset (&Output, 0xA5, sizeof (Output));
  CHECK (MtkDsiCoreBuildAlongCyxConfigShadow (
           &Profile, &Profile.Initial, &Output) == MtkDsiSuccess);
  CHECK (Output.Reg08 == 0x10203040U);
  CHECK (Output.Reg14 == (0x12345678U | 0x00100000U));
  CHECK (Output.Reg18 == 0xA5A4053CU);
  CHECK (Output.Reg1C == 0x00031A40U);
  CHECK (Output.Reg20 == 8U && Output.Reg24 == 32U &&
         Output.Reg28 == 230U && Output.Reg2C == 1400U);
  CHECK (Output.Reg50 == 16U && Output.Reg54 == 88U &&
         Output.Reg58 == 108U && Output.Reg5C == 0U);
  CHECK (Output.Reg64 == 0x55667788U && Output.Reg90 == 0x3CU);
  CHECK (Output.Reg110 == 0x111C0E10U);
  CHECK (Output.Reg114 == 0x1B501840U);
  CHECK (Output.Reg118 == 0x103A0103U);
  CHECK (Output.Reg11C == 0x001A1C0CU);
  CHECK (Output.Reg200 == (0x21U | 33U));

  Before = Output;
  Profile.ConfigWidth++;
  Status = MtkDsiCoreBuildAlongCyxConfigShadow (
             &Profile, &Profile.Initial, &Output);
  CHECK (Status == MtkDsiInvalid);
  CHECK (memcmp (&Output, &Before, sizeof (Output)) == 0);

  Profile = SupportedProfile ();
  Profile.ConfigHeight++;
  Status = MtkDsiCoreBuildAlongCyxConfigShadow (
             &Profile, &Profile.Initial, &Output);
  CHECK (Status == MtkDsiInvalid);
  CHECK (memcmp (&Output, &Before, sizeof (Output)) == 0);

  Profile = SupportedProfile ();
  Status = MtkDsiCoreBuildAlongCyxConfigShadow (
             &Profile, 0, &Output);
  CHECK (Status == MtkDsiInvalid);
  CHECK (memcmp (&Output, &Before, sizeof (Output)) == 0);

  Profile = SupportedProfile ();
  Status = MtkDsiCoreBuildAlongCyxConfigShadow (
             &Profile, &Profile.Initial, 0);
  CHECK (Status == MtkDsiInvalid);

  Profile = SupportedProfile ();
  Profile.GetParamsSize--;
  Status = MtkDsiCoreBuildAlongCyxConfigShadow (
             &Profile, &Profile.Initial, &Output);
  CHECK (Status == MtkDsiInvalid);
  CHECK (memcmp (&Output, &Before, sizeof (Output)) == 0);

  Profile = SupportedProfile ();
  Profile.GetParams = 0;
  Status = MtkDsiCoreBuildAlongCyxConfigShadow (
             &Profile, &Profile.Initial, &Output);
  CHECK (Status == MtkDsiInvalid);
  CHECK (memcmp (&Output, &Before, sizeof (Output)) == 0);

  Profile = SupportedProfile ();
  SetParam32 (0x1B8U, 231U);
  Status = MtkDsiCoreBuildAlongCyxConfigShadow (
             &Profile, &Profile.Initial, &Output);
  CHECK (Status == MtkDsiInvalid);
  CHECK (memcmp (&Output, &Before, sizeof (Output)) == 0);
}

static void
CheckFailuresAndQuarantine (
  void
  )
{
  DSI_MOCK Mock;
  MTK_DSI_CORE Core;
  MTK_DSI_OP7_PROFILE Profile;
  unsigned TraceAtFailure;
  unsigned WritesAtFailure;

  Profile = SupportedProfile ();
  InitializeFixture (&Mock, &Core, &Profile, 0U);
  Mock.FailReadAt = 1U;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiIoError);
  CHECK (Core.State == MtkDsiStateIdle);
  CHECK (Core.WriteAttempts == 0U);
  Mock.FailReadAt = 0U;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiSuccess);

  Profile = SupportedProfile ();
  InitializeFixture (&Mock, &Core, &Profile, 0xA5A5A5A5U);
  Mock.FailReadAt = 2U;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiIoError);
  CHECK (Core.State == MtkDsiStateQuarantined);
  CHECK (Core.WriteAttempts == 1U && Core.WritesAccepted == 1U);
  TraceAtFailure = Mock.TraceCount;
  WritesAtFailure = Mock.WriteCalls;
  Mock.FailReadAt = 0U;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiQuarantined);
  CHECK (Mock.TraceCount == TraceAtFailure && Mock.WriteCalls == WritesAtFailure);
  CHECK (MtkDsiCoreInitialize (&Core, &Core.Io) == MtkDsiWrongState);

  Profile = SupportedProfile ();
  InitializeFixture (&Mock, &Core, &Profile, 0U);
  Mock.ExternalChangeReadAt = 4U;
  Mock.ExternalChangeBank = MtkDsiBankMipiTx;
  Mock.ExternalChangeOffset = 0x244U;
  Mock.ExternalChangeValue = 0x12345679U;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiSuccess);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x244U) == 0x12345678U);

  Profile = SupportedProfile ();
  InitializeFixture (&Mock, &Core, &Profile, 0U);
  Mock.FailWriteAt = 1U;
  Mock.ApplyFailedWrite = 1U;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiIoError);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x108U) == 0x00080000U);
  CHECK (Core.State == MtkDsiStateQuarantined);
  TraceAtFailure = Mock.TraceCount;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiQuarantined);
  CHECK (Mock.TraceCount == TraceAtFailure);

  Profile = SupportedProfile ();
  InitializeFixture (&Mock, &Core, &Profile, 0U);
  Mock.FailWriteAt = 1U;
  Mock.ApplyFailedWrite = 0U;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiIoError);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x108U) == 0U);
  CHECK (Core.State == MtkDsiStateQuarantined);
  TraceAtFailure = Mock.TraceCount;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiQuarantined);
  CHECK (Mock.TraceCount == TraceAtFailure);

  Profile = SupportedProfile ();
  InitializeFixture (&Mock, &Core, &Profile, 0xA5A5A5A5U);
  Mock.FailWriteAt = 43U; /* DSI reset bit2 set; callback may have applied it. */
  Mock.ApplyFailedWrite = 1U;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiIoError);
  CHECK (Core.State == MtkDsiStateQuarantined);
  CHECK ((ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13000U) & 4U) != 0U);
  CHECK (Core.WriteAttempts == 43U && Core.WritesAccepted == 42U);
  TraceAtFailure = Mock.TraceCount;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiQuarantined);
  CHECK (Mock.TraceCount == TraceAtFailure);

  Profile = SupportedProfile ();
  InitializeFixture (&Mock, &Core, &Profile, 0U);
  Mock.FailDelayAt = 1U;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiDelayError);
  CHECK (Core.State == MtkDsiStateQuarantined);
  CHECK (Core.WritesAccepted == 10U && Core.DelayAttempts == 1U);
  TraceAtFailure = Mock.TraceCount;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiQuarantined);
  CHECK (Mock.TraceCount == TraceAtFailure);
}

static void
CheckReentryAndProfileSnapshot (
  void
  )
{
  DSI_MOCK Mock;
  MTK_DSI_CORE Core;
  MTK_DSI_OP7_PROFILE Profile;

  Profile = SupportedProfile ();
  InitializeFixture (&Mock, &Core, &Profile, 0U);
  Mock.ReenterAtFirstWrite = 1U;
  Mock.MutateProfileAtFirstWrite = 1U;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, &Profile) == MtkDsiSuccess);
  CHECK (Mock.ReentrantStatus == MtkDsiBusy);
  CHECK (Core.State == MtkDsiStateOp7Complete);
  CHECK (Core.WriteAttempts == 48U && Mock.TraceCount == 73U);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x2CU) == 0x70C4EC4EU);
  CHECK (ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13008U) == 0x10203040U);
}


static void
ResetTrace (DSI_MOCK *Mock)
{
  Mock->TraceCount = 0U;
  Mock->ReadCalls = 0U;
  Mock->WriteCalls = 0U;
  Mock->DelayCalls = 0U;
  Mock->FailReadAt = 0U;
  Mock->FailWriteAt = 0U;
  Mock->FailDelayAt = 0U;
  Mock->ModelIdle = 1U;
}

static void
PrepareTransition (
  DSI_MOCK *Mock, MTK_DSI_CORE *Core,
  const MTK_DSI_OP7_PROFILE *Profile, unsigned Operation
  )
{
  InitializeFixture (Mock, Core, Profile, 0xA5A5A5A5U);
  Mock->ModelIdle = 1U;
  CHECK (MtkDsiCoreRunAlongCyxOp7 (Core, Profile) == MtkDsiSuccess);
  if (Operation > 0U) CHECK (MtkDsiCoreStartVideo (Core) == MtkDsiSuccess);
  if (Operation > 1U) CHECK (MtkDsiCoreSuspend (Core, 1U) == MtkDsiSuccess);
  ResetTrace (Mock);
}

static MTK_DSI_STATUS
RunTransition (MTK_DSI_CORE *Core, unsigned Operation)
{
  if (Operation == 0U) return MtkDsiCoreStartVideo (Core);
  if (Operation == 1U) return MtkDsiCoreSuspend (Core, 4U);
  return MtkDsiCoreResume (Core);
}

static void
CheckLinkLifecycle (void)
{
  MTK_DSI_OP7_PROFILE Profile;
  MTK_DSI_CORE Core;
  DSI_MOCK Mock;
  MTK_DSI_PORT_SHADOW Saved;
  unsigned Count;
  unsigned Cycle;

  Profile = SupportedProfile ();
  PrepareTransition (&Mock, &Core, &Profile, 0U);
  CHECK (MtkDsiCoreResume (&Core) == MtkDsiWrongState);
  CHECK (MtkDsiCoreSuspend (&Core, 1U) == MtkDsiWrongState);
  CHECK (Mock.TraceCount == 0U);
  for (Cycle = 0U; Cycle < 3U; ++Cycle) {
    CHECK (MtkDsiCoreStartVideo (&Core) == MtkDsiSuccess);
    CHECK (Core.State == MtkDsiStateStreaming && Core.Busy == 0U);
    CHECK ((ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13000U) & 1U) == 1U);
    CHECK ((ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13014U) & 3U) == 1U);
    CHECK ((ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13018U) & 0x1F0FFU) == 0x3CU);
    Count = Mock.TraceCount;
    CHECK (MtkDsiCoreStartVideo (&Core) == MtkDsiWrongState);
    CHECK (MtkDsiCoreSuspend (&Core, 0U) == MtkDsiInvalid);
    CHECK (MtkDsiCoreSuspend (&Core, 1001U) == MtkDsiInvalid);
    CHECK (Count == Mock.TraceCount);
    ResetTrace (&Mock);
    Mock.BusyReads = 3U;
    CHECK (MtkDsiCoreSuspend (&Core, 4U) == MtkDsiSuccess);
    CHECK (Mock.DelayCalls == 5U && Mock.BusyReads == 0U);
    CHECK (Core.State == MtkDsiStateSuspended && Core.Busy == 0U);
    CHECK ((Core.SavedPort.Reg14 & 3U) == 0U);
    Saved = Core.SavedPort;
    CHECK ((ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x30U) & 0x10U) == 0U);
    CHECK ((ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x28U) & 3U) == 2U);
    Count = Mock.TraceCount;
    CHECK (MtkDsiCoreSuspend (&Core, 1U) == MtkDsiWrongState);
    CHECK (MtkDsiCoreStartVideo (&Core) == MtkDsiWrongState);
    CHECK (Mock.TraceCount == Count);
    ResetTrace (&Mock);
    CHECK (MtkDsiCoreResume (&Core) == MtkDsiSuccess);
    CHECK (Core.State == MtkDsiStateOp7Complete && Core.Busy == 0U);
    CHECK (memcmp (&Saved, &Core.SavedPort, sizeof (Saved)) == 0);
    CHECK ((ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x130A0U) & 0xFFFFU) == 179U);
    CHECK ((ReadRegisterValue (&Mock, MtkDsiBankDsi0, 0x13014U) & 0x100003U) == 0U);
    CHECK ((ReadRegisterValue (&Mock, MtkDsiBankMipiTx, 0x30U) & 0x10U) != 0U);
    ResetTrace (&Mock);
  }
  PrepareTransition (&Mock, &Core, &Profile, 1U);
  Mock.BusyForever = 1U;
  CHECK (MtkDsiCoreSuspend (&Core, 3U) == MtkDsiTimeout);
  CHECK (Core.State == MtkDsiStateQuarantined && Core.Busy == 0U);
  CHECK (Mock.WriteCalls == 1U);
  CHECK (Mock.ReadCalls == 5U && Mock.DelayCalls == 2U);
  Count = Mock.TraceCount;
  CHECK (MtkDsiCoreResume (&Core) == MtkDsiQuarantined);
  CHECK (MtkDsiCoreStartVideo (&Core) == MtkDsiQuarantined);
  CHECK (MtkDsiCoreSuspend (&Core, 1U) == MtkDsiQuarantined);
  CHECK (Mock.TraceCount == Count);
}

static void
CheckTransitionFailures (void)
{
  MTK_DSI_OP7_PROFILE Profile;
  MTK_DSI_CORE Core;
  DSI_MOCK Mock;
  unsigned Operation;
  unsigned Kind;
  unsigned Counts[3];
  unsigned At;
  unsigned TraceCount;

  Profile = SupportedProfile ();
  for (Operation = 0U; Operation < 3U; ++Operation) {
    PrepareTransition (&Mock, &Core, &Profile, Operation);
    CHECK (RunTransition (&Core, Operation) == MtkDsiSuccess);
    Counts[0] = Mock.ReadCalls;
    Counts[1] = Mock.WriteCalls;
    Counts[2] = Mock.DelayCalls;
    for (Kind = 0U; Kind < 3U; ++Kind) {
      for (At = 1U; At <= Counts[Kind]; ++At) {
        PrepareTransition (&Mock, &Core, &Profile, Operation);
        if (Kind == 0U) Mock.FailReadAt = At;
        if (Kind == 1U) {
          Mock.FailWriteAt = At;
          Mock.ApplyFailedWrite = (uint8_t)(At & 1U);
        }
        if (Kind == 2U) Mock.FailDelayAt = At;
        CHECK (RunTransition (&Core, Operation) ==
               (Kind == 2U ? MtkDsiDelayError : MtkDsiIoError));
        CHECK (Core.State == MtkDsiStateQuarantined && Core.Busy == 0U);
        TraceCount = Mock.TraceCount;
        CHECK (MtkDsiCoreResume (&Core) == MtkDsiQuarantined);
        CHECK (MtkDsiCoreStartVideo (&Core) == MtkDsiQuarantined);
        CHECK (MtkDsiCoreSuspend (&Core, 1U) == MtkDsiQuarantined);
        CHECK (TraceCount == Mock.TraceCount);
      }
    }
    PrepareTransition (&Mock, &Core, &Profile, Operation);
    Mock.ReenterAtFirstWrite = 1U;
    CHECK (RunTransition (&Core, Operation) == MtkDsiSuccess);
    CHECK (Mock.ReentrantStatus == MtkDsiBusy);
  }
  CHECK (MtkDsiCoreStartVideo (0) == MtkDsiInvalid);
  CHECK (MtkDsiCoreResume (0) == MtkDsiInvalid);
  CHECK (MtkDsiCoreSuspend (0, 1U) == MtkDsiInvalid);
}


int
main (
  void
  )
{
  DSI_MOCK Mock;
  MTK_DSI_CORE Core;
  MTK_DSI_CORE_IO Io;

  CHECK (MtkDsiCoreInitialize (0, 0) == MtkDsiInvalid);
  Core = (MTK_DSI_CORE){ 0 };
  Io.Context = 0;
  Io.Read32 = MockRead32;
  Io.Write32 = MockWrite32;
  Io.DelayMicroseconds = MockDelayMicroseconds;
  CHECK (MtkDsiCoreInitialize (&Core, &Io) == MtkDsiSuccess);
  CHECK (MtkDsiCoreRunAlongCyxOp7 (&Core, 0) == MtkDsiInvalid);

  CheckProfileRejectsBeforeIo ();
  CheckConfigShadowBuilder ();
  CheckSuccessfulProfile (0U);
  CheckSuccessfulProfile (0xA5A5A5A5U);
  CheckFailuresAndQuarantine ();
  CheckReentryAndProfileSnapshot ();
  CheckLinkLifecycle ();
  CheckTransitionFailures ();

  /* Silence accidental unused-state regressions without adding a fake path. */
  Mock = (DSI_MOCK){ 0 };
  CHECK (Mock.TraceCount == 0U);
  printf ("MtkDsiCoreTest: %u checks\n", Checks);
  return 0;
}
