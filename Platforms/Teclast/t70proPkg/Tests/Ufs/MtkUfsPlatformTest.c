/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Library/MtkUfsPlatform.h>

static unsigned Checks;
#define CHECK(X) do { Checks++; if (!(X)) { fprintf(stderr, "%d: %s\n", __LINE__, #X); abort(); } } while (0)

typedef struct { char Kind; unsigned Bank; uint32_t Offset; uint64_t Value; uint64_t Args[8]; } EVENT;
typedef struct {
  uint32_t Reg[0x2400 / 4];
  uint32_t Attr[0x10000];
  EVENT Trace[4096];
  unsigned TraceCount, Calls, FailAt, FailSmcAt, SmcCalls, RefPolls, HcePolls;
  uint32_t RejectSmcCommand;
  unsigned SmcTransportFailAt;
  int64_t SmcResult, RejectSmcResult;
  uint64_t Time;
  int AckOn, AckOff, StuckHce;
  unsigned HibernateCalls, HibernateFailAt;
  int HoldHibernateState;
} MODEL;
static MODEL M;
static MTK_UFS_PLATFORM P;
static const MTK_UFS_PLATFORM_CONFIG Config = {
  0x2300, 0x1000, 0, 0, 32,
  { { 0x130, 0x134, 1U << 15 }, { 0x140, 0x144, 1U << 7 }, { 0x150, 0x154, 1U << 21 } }, 0
};
static MTK_UFS_PLATFORM_STATUS Step(void) { return ++M.Calls == M.FailAt ? MtkUfsPlatformIoError : MtkUfsPlatformOk; }
static EVENT *Log(char Kind, unsigned Bank, uint32_t Offset, uint64_t Value) {
  EVENT *E;
  CHECK(M.TraceCount < sizeof(M.Trace) / sizeof(M.Trace[0]));
  E = &M.Trace[M.TraceCount++]; memset(E, 0, sizeof(*E));
  E->Kind = Kind; E->Bank = Bank; E->Offset = Offset; E->Value = Value; return E;
}
static MTK_UFS_PLATFORM_STATUS Read(void *C, MTK_UFS_MMIO_BANK Bank, uint32_t Offset, uint32_t *Value) {
  MTK_UFS_PLATFORM_STATUS S;
  CHECK(C == &M && Bank == MtkUfsControllerBank && (Offset & 3U) == 0 && Offset < P.Config.ControllerLength);
  S = Step(); if (S) return S;
  if (Offset == 0x144) M.RefPolls++;
  if (Offset == 0x34) M.HcePolls++;
  *Value = M.Reg[Offset / 4]; return MtkUfsPlatformOk;
}
static MTK_UFS_PLATFORM_STATUS Write(void *C, MTK_UFS_MMIO_BANK Bank, uint32_t Offset, uint32_t Value) {
  MTK_UFS_PLATFORM_STATUS S;
  CHECK(C == &M && (Offset & 3U) == 0);
  CHECK(Offset < (Bank == MtkUfsControllerBank ? P.Config.ControllerLength : P.Config.ResetLength));
  Log('W', Bank, Offset, Value);
  S = Step(); if (S) return S;
  if (Bank == MtkUfsControllerBank) {
    if (Offset == 0x144) M.Reg[Offset / 4] = Value ? (M.AckOn ? 3 : 1) : (M.AckOff ? 0 : 2);
    else if (Offset != 0x34 || !M.StuckHce) M.Reg[Offset / 4] = Value;
  }
  return MtkUfsPlatformOk;
}
static void Delay(void *C, uint32_t Us) { CHECK(C == &M); M.Time += Us; if (Us != 100) Log('T', 0, Us, 0); }
static MTK_UFS_PLATFORM_STATUS Smc(void *C, const uint64_t Args[8], int64_t *Result) {
  MTK_UFS_PLATFORM_STATUS S;
  EVENT *E;
  CHECK(C == &M);
  CHECK(Args[0] == MTK_UFS_SMC_FUNCTION && Args[4] == P.Config.ControllerId);
  CHECK(Args[5] == 0 && Args[6] == 0 && Args[7] == 0);
  E = Log('S', 0, 0, 0); memcpy(E->Args, Args, sizeof(E->Args));
  S = Step(); M.SmcCalls++;
  if (M.SmcCalls == M.SmcTransportFailAt) S = MtkUfsPlatformIoError;
  *Result = M.SmcCalls == M.FailSmcAt ? M.SmcResult :
    (M.RejectSmcCommand != 0 && Args[1] == M.RejectSmcCommand ? M.RejectSmcResult : 0);
  return S;
}
static MTK_UFS_PLATFORM_STATUS Dme(void *C, int Get, uint16_t Attr, uint32_t *Value) {
  MTK_UFS_PLATFORM_STATUS S;
  CHECK(C == &M);
  Log(Get ? 'G' : 'D', 0, Attr, Get ? 0 : *Value);
  S = Step(); if (S) return S;
  if (Get) *Value = M.Attr[Attr]; else M.Attr[Attr] = *Value;
  return MtkUfsPlatformOk;
}
static MTK_UFS_PLATFORM_STATUS Hibernate(void *C, int Enter) {
  CHECK(C == &M && (Enter == 0 || Enter == 1));
  Log('H', 0, 0, (uint64_t)Enter);
  M.HibernateCalls++;
  if (M.HibernateCalls == M.HibernateFailAt) return MtkUfsPlatformIoError;
  if (!M.HoldHibernateState) M.Reg[0x22C8 / 4] = (Enter ? 3U : 2U) << 28 | 1U;
  return MtkUfsPlatformOk;
}
static const MTK_UFS_PLATFORM_IO Io = { &M, Read, Write, Delay, Smc, Dme, Hibernate };
static void Reset(uint32_t Version) {
  memset(&M, 0, sizeof(M)); memset(&P, 0, sizeof(P));
  M.Reg[0x2240 / 4] = Version; M.Reg[0x30 / 4] = 9;
  M.AckOn = M.AckOff = 1;
  M.Reg[0x22C8 / 4] = (2U << 28) | 1U;
  M.Attr[0xD0A6] = 0xA5A500F0; M.Attr[0xD0A1] = 0x1234567F; M.Attr[0xD09E] = 0xABC00001;
  M.Reg[0x140 / 4] = 0x40001; M.Reg[0x160 / 4] = 0x980000A5;
  CHECK(MtkUfsPlatformInitialize(&P, &Io, &Config) == MtkUfsPlatformOk);
}
static void RefOn(void) { CHECK(MtkUfsPlatformSetRefClock(&P, 1) == MtkUfsPlatformOk); }
static void Host(void) {
  RefOn(); CHECK(MtkUfsPlatformPreHce(&P) == MtkUfsPlatformOk);
  M.Reg[0x34 / 4] = 1;
  CHECK(MtkUfsPlatformPostHce(&P) == MtkUfsPlatformOk);
}
static void Full(void) {
  CHECK(MtkUfsPlatformResetDevice(&P) == MtkUfsPlatformOk);
  Host(); CHECK(MtkUfsPlatformPreLink(&P) == MtkUfsPlatformOk);
  CHECK(MtkUfsPlatformPostLink(&P) == MtkUfsPlatformOk);
  CHECK(P.State == MtkUfsPlatformLinkReady);
}
static void ConfirmRefClockOff(void) {
  uint32_t Value = UINT32_MAX;
  CHECK(P.Io.Read32(P.Io.Context, MtkUfsControllerBank, 0x144U, &Value) == MtkUfsPlatformOk);
  CHECK(Value != UINT32_MAX && (Value & 3U) == 0U);
  P.RefClock = MtkUfsRefClockOff;
}
static void TestTraceAndVersions(void) {
  static const uint32_t Versions[] = { 0x10380000, 0x20160706, 0x10440000, 0x10450000, 0x10450100, 0x10460000, 0x00450000, 0x01450000, 0x10480000 };
  size_t I;
  for (I = 0; I < sizeof(Versions) / sizeof(Versions[0]); I++) {
    uint32_t V = Versions[I]; unsigned J, R = 0;
    uint32_t Offsets[] = { 0x130, 0x150, 0x140, 0x144, 0x154, 0x134 };
    uint32_t Masks[] = { 0x8000, 0x200000, 0x80, 0x80, 0x200000, 0x8000 };
    Reset(V);
    if (V == 0x10480000) { P.Config.ControllerLength = 0x2400; M.Reg[0x2308 / 4] = 2; }
    Full();
    CHECK(P.RefClock == MtkUfsRefClockOn && P.RecoveryStatus == MtkUfsPlatformOk);
    CHECK(M.Time == 10 + 10000 + 32 + 100 + 600);
    CHECK(M.Trace[0].Kind == 'S' && M.Trace[0].Args[1] == 2 && M.Trace[0].Args[2] == 0);
    CHECK(M.Trace[1].Kind == 'W' && M.Trace[1].Offset == 0x34 && M.Trace[1].Value == 0);
    CHECK(M.Trace[3].Kind == 'S' && M.Trace[3].Args[1] == 2 && M.Trace[3].Args[2] == 1);
    for (J = 0; J < M.TraceCount; J++) if (M.Trace[J].Kind == 'W' && M.Trace[J].Bank == MtkUfsResetBank) {
      CHECK(R < 6 && M.Trace[J].Offset == Offsets[R] && M.Trace[J].Value == Masks[R]); R++;
    }
    CHECK(R == 6);
    CHECK(M.Reg[0x140 / 4] == 0x40081 && M.Reg[0x18 / 4] == 0);
    CHECK(M.Attr[0xD0A6] == ((0xA5A500F0 & ~0x40U) | 0xDU));
    CHECK(M.Attr[0xD0A1] == (0x1234567F & ~0x20U));
    if (V == 0x00450000 || V == 0x01450000) V = 0x10450000;
    CHECK(P.IpVersion == V);
    CHECK(M.Attr[0xD09E] == (0xABC00001U | (V == 0x10450000 ? 0x10 : V >= 0x10450100 ? 0x100 : 0)));
    CHECK(M.Reg[0x160 / 4] == ((V <= 0x1044FFFF || V == 0x20160706) ? 0x980000A5U :
      V <= 0x1045FFFF ? 0x984530A5U : 0x8A4530A5U));
  }
  Reset(0x10480000); RefOn(); M.TraceCount = 0;
  CHECK(MtkUfsPlatformPreHce(&P) == MtkUfsPlatformUnsupported && M.TraceCount == 0);
}
static void TestFailures(void) {
  unsigned Count, At, I;
  Reset(0x10450000); Full(); Count = M.Calls;
  for (At = 1; At <= Count; At++) {
    MTK_UFS_PLATFORM_STATUS S;
    Reset(0x10450000); M.FailAt = At;
    S = MtkUfsPlatformResetDevice(&P);
    if (S == MtkUfsPlatformOk) S = MtkUfsPlatformSetRefClock(&P, 1);
    if (S == MtkUfsPlatformOk) S = MtkUfsPlatformPreHce(&P);
    if (S == MtkUfsPlatformOk) { M.Reg[0x34 / 4] = 1; S = MtkUfsPlatformPostHce(&P); }
    if (S == MtkUfsPlatformOk) S = MtkUfsPlatformPreLink(&P);
    if (S == MtkUfsPlatformOk) S = MtkUfsPlatformPostLink(&P);
    CHECK(S == MtkUfsPlatformIoError && P.State != MtkUfsPlatformLinkReady);
    /* A reset assertion failure still receives a matching release attempt. */
    for (I = 0; I < M.TraceCount; I++) if (M.Trace[I].Kind == 'W' && M.Trace[I].Bank == MtkUfsResetBank &&
        (M.Trace[I].Offset == 0x130 || M.Trace[I].Offset == 0x140 || M.Trace[I].Offset == 0x150)) {
      unsigned J; int Released = 0;
      for (J = I + 1; J < M.TraceCount; J++) if (M.Trace[J].Kind == 'W' && M.Trace[J].Bank == MtkUfsResetBank &&
          M.Trace[J].Offset == M.Trace[I].Offset + 4 && M.Trace[J].Value == M.Trace[I].Value) Released = 1;
      CHECK(Released);
    }
  }
  for (At = 1; At <= 4; At++) {
    int Results[] = { -1, -3, 1, 7 };
    for (I = 0; I < 4; I++) {
      MTK_UFS_PLATFORM_STATUS S;
      Reset(0x10450000); M.FailSmcAt = At; M.SmcResult = Results[I];
      S = MtkUfsPlatformResetDevice(&P);
      if (S == MtkUfsPlatformOk) S = MtkUfsPlatformSetRefClock(&P, 1);
      if (At >= 3 && Results[I] == -1) {
        CHECK(S == MtkUfsPlatformOk && P.State == MtkUfsPlatformIdle && P.RefClock == MtkUfsRefClockOn);
      } else {
        CHECK(S == MtkUfsPlatformIoError && P.State == MtkUfsPlatformFault);
      }
    }
  }
}

static void TestSmcReturnContracts(void) {
  static const int64_t OtherResults[] = { -3, 1, 7 };
  static const int64_t NonzeroResults[] = { -1, -3, 1, 7 };
  unsigned Enable, Stage, At, I;

  for (Enable = 0; Enable <= 1; Enable++) {
    for (Stage = 0; Stage <= 1; Stage++) {
      unsigned Start, FirstSmc = 0, SecondSmc = 0, Seen = 0, J;
      Reset(0x10450000);
      if (!Enable) RefOn();
      Start = M.TraceCount;
      M.FailSmcAt = M.SmcCalls + (Stage ? 2 : 1);
      M.SmcResult = -1;
      CHECK(MtkUfsPlatformSetRefClock(&P, (int)Enable) == MtkUfsPlatformOk);
      CHECK(P.RefClock == (Enable ? MtkUfsRefClockOn : MtkUfsRefClockOff));
      CHECK(M.Reg[0x144 / 4] == (Enable ? 3U : 0U));
      for (J = Start; J < M.TraceCount; J++) if (M.Trace[J].Kind == 'S') {
        if (Seen++ == 0) FirstSmc = J; else if (Seen == 2) SecondSmc = J;
      }
      CHECK(Seen == 2);
      CHECK(M.Trace[FirstSmc].Args[1] == 8 && M.Trace[FirstSmc].Args[2] == Enable &&
            M.Trace[FirstSmc].Args[3] == 0);
      CHECK(M.Trace[SecondSmc].Args[1] == 8 && M.Trace[SecondSmc].Args[2] == Enable &&
            M.Trace[SecondSmc].Args[3] == 1);
      CHECK(P.LastSmcResult == (Stage ? -1 : 0));
    }
  }

  for (At = 1; At <= 2; At++) {
    for (I = 0; I < sizeof(OtherResults) / sizeof(OtherResults[0]); I++) {
      Reset(0x10450000);
      M.FailSmcAt = At;
      M.SmcResult = OtherResults[I];
      CHECK(MtkUfsPlatformSetRefClock(&P, 1) == MtkUfsPlatformIoError);
      CHECK(P.State == MtkUfsPlatformFault);
    }
  }

  /* Callback/transport failure wins even if the callback captured -1. */
  Reset(0x10450000);
  M.FailAt = 4;
  M.FailSmcAt = 1;
  M.SmcResult = -1;
  CHECK(MtkUfsPlatformSetRefClock(&P, 1) == MtkUfsPlatformIoError);
  CHECK(P.State == MtkUfsPlatformFault && P.LastSmcResult == -1);

  for (At = 1; At <= 2; At++) {
    Reset(0x10450000);
    M.FailSmcAt = At;
    M.SmcResult = -1;
    CHECK(MtkUfsPlatformResetDevice(&P) == MtkUfsPlatformIoError);
    CHECK(P.State == MtkUfsPlatformFault);
  }

  for (I = 0; I < sizeof(NonzeroResults) / sizeof(NonzeroResults[0]); I++) {
    unsigned J, HibernateExits = 0, SawSramOff = 0, SawSramOn = 0;
    Reset(0x10450000);
    Full();
    M.RejectSmcCommand = 32;
    M.RejectSmcResult = NonzeroResults[I];
    CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformIoError);
    CHECK(P.RefClock == MtkUfsRefClockOn && P.LastSmcResult == NonzeroResults[I]);
    if (NonzeroResults[I] == -1) {
      CHECK(P.State == MtkUfsPlatformLinkReady && P.RecoveryStatus == MtkUfsPlatformOk);
      CHECK(M.Attr[0xD0A8] == 0 && (M.Reg[0x22C8 / 4] >> 28) == 2);
    } else {
      CHECK(P.State == MtkUfsPlatformFault && M.Attr[0xD0A8] == 1);
    }
    for (J = 0; J < M.TraceCount; J++) {
      if (M.Trace[J].Kind == 'H' && M.Trace[J].Value == 0) HibernateExits++;
      if (M.Trace[J].Kind == 'S' && M.Trace[J].Args[1] == 32 && M.Trace[J].Args[2] == 0) SawSramOff = 1;
      if (M.Trace[J].Kind == 'S' && M.Trace[J].Args[1] == 32 && M.Trace[J].Args[2] == 1) SawSramOn = 1;
    }
    CHECK(SawSramOff);
    if (NonzeroResults[I] == -1) CHECK(HibernateExits == 1 && !SawSramOn);
    else CHECK(HibernateExits == 0 && SawSramOn);
  }

  // A failed callback plus -1 leaves SRAM state uncertain: still attempt
  // SRAM-on and preserve failure if the same mandatory service is unsupported.
  Reset(0x10450000); Full();
  M.RejectSmcCommand = 32; M.RejectSmcResult = -1;
  M.SmcTransportFailAt = M.SmcCalls + 1;
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformIoError);
  CHECK(P.State == MtkUfsPlatformFault && M.Attr[0xD0A8] == 1);
  CHECK(M.SmcCalls == M.SmcTransportFailAt + 1);
  CHECK(M.Trace[M.TraceCount-1].Kind == 'S' && M.Trace[M.TraceCount-1].Args[2] == 1);

  // Before SRAM-off was attempted, recovery must not introduce an unsupported
  // SRAM-on operation that prevents a successful Hibern8/DME restoration.
  Reset(0x10450000); Full();
  M.RejectSmcCommand = 32; M.RejectSmcResult = -1; M.HibernateFailAt = 1;
  At = M.SmcCalls;
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformIoError);
  CHECK(P.State == MtkUfsPlatformLinkReady && P.RecoveryStatus == MtkUfsPlatformOk);
  CHECK(M.Attr[0xD0A8] == 0 && M.HibernateCalls == 2 && M.SmcCalls == At);

  Reset(0x10450000); Full();
  M.RejectSmcCommand = 32; M.RejectSmcResult = -1; M.HibernateFailAt = 2;
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformIoError);
  CHECK(P.State == MtkUfsPlatformFault && P.RecoveryStatus == MtkUfsPlatformIoError);

  Reset(0x10450000);
  Full();
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformOk);
  M.RejectSmcCommand = 32;
  M.RejectSmcResult = -1;
  CHECK(MtkUfsPlatformResume(&P) == MtkUfsPlatformIoError);
  CHECK(P.State == MtkUfsPlatformFault && P.RefClock == MtkUfsRefClockOn);
  CHECK(M.Attr[0xD0A8] == 1 && (M.Reg[0x22C8 / 4] >> 28) == 3);
}

static void TestTimeoutsAndGuards(void) {
  MTK_UFS_PLATFORM_CONFIG C;
  unsigned Count;
  Reset(0x10450000); M.AckOn = 0;
  CHECK(MtkUfsPlatformSetRefClock(&P, 1) == MtkUfsPlatformTimeout);
  CHECK(P.State == MtkUfsPlatformFault && P.RefClock == MtkUfsRefClockOff && M.Time == 3000 && M.RefPolls == 32);
  Reset(0x10450000); RefOn(); M.Reg[0x34 / 4] = 1; M.StuckHce = 1;
  CHECK(MtkUfsPlatformResetDevice(&P) == MtkUfsPlatformTimeout);
  CHECK(M.Time == 32 + MTK_UFS_HOST_TIMEOUT_US + 10 + 10000 && M.Trace[M.TraceCount - 2].Args[2] == 1);
  Reset(0x10450000); RefOn(); CHECK(MtkUfsPlatformPreHce(&P) == MtkUfsPlatformOk);
  CHECK(MtkUfsPlatformPostHce(&P) == MtkUfsPlatformTimeout);
  CHECK(M.Time == 32 + 100 + 600 + MTK_UFS_HOST_TIMEOUT_US);
  Reset(0x10450000); M.Reg[0x58 / 4] = 1; Count = M.TraceCount;
  CHECK(MtkUfsPlatformResetDevice(&P) == MtkUfsPlatformBusy);
  CHECK(MtkUfsPlatformSetRefClock(&P, 1) == MtkUfsPlatformBusy);
  CHECK(M.TraceCount == Count);
  Reset(0x10450000); M.Reg[0x34 / 4] = 1;
  CHECK(MtkUfsPlatformSetRefClock(&P, 1) == MtkUfsPlatformBusy && M.TraceCount == 0);
  Reset(0x10450000); CHECK(MtkUfsPlatformPreHce(&P) == MtkUfsPlatformWrongState);
  CHECK(MtkUfsPlatformPostHce(&P) == MtkUfsPlatformWrongState);
  CHECK(MtkUfsPlatformPreLink(&P) == MtkUfsPlatformWrongState);
  CHECK(MtkUfsPlatformPostLink(&P) == MtkUfsPlatformWrongState && M.TraceCount == 0);
  Host(); Count = M.TraceCount;
  CHECK(MtkUfsPlatformSetRefClock(&P, 0) == MtkUfsPlatformWrongState && M.TraceCount == Count);
  CHECK(MtkUfsPlatformResetDevice(&P) == MtkUfsPlatformWrongState);
  CHECK(MtkUfsPlatformPreLink(&P) == MtkUfsPlatformOk); M.Reg[0x30 / 4] = 8;
  CHECK(MtkUfsPlatformPostLink(&P) == MtkUfsPlatformIoError);
  Reset(0x10450000); C = Config; C.Reset[1] = C.Reset[0];
  CHECK(MtkUfsPlatformInitialize(&P, &Io, &C) == MtkUfsPlatformInvalid);
  C = Config; C.ControllerLength = 0x2243;
  CHECK(MtkUfsPlatformInitialize(&P, &Io, &C) == MtkUfsPlatformInvalid);
  C = Config; C.Reset[0].Mask = 3;
  CHECK(MtkUfsPlatformInitialize(&P, &Io, &C) == MtkUfsPlatformInvalid);
  Reset(0); RefOn(); CHECK(MtkUfsPlatformPreHce(&P) == MtkUfsPlatformIoError);
  Reset(UINT32_MAX); RefOn(); CHECK(MtkUfsPlatformPreHce(&P) == MtkUfsPlatformIoError);
  Reset(0x10450000); P.Config.ControllerId = 2; RefOn();
  CHECK(M.Trace[0].Args[4] == 2 && M.Trace[3].Args[4] == 2);
  CHECK(MtkUfsPlatformSetRefClock(&P, 0) == MtkUfsPlatformOk && P.RefClock == MtkUfsRefClockOff);
  CHECK(MtkUfsPlatformSetRefClock(NULL, 0) == MtkUfsPlatformInvalid);
}

static void TestSuspendResume(void) {
  unsigned I;
  Reset(0x10450000); Full(); M.TraceCount = 0;
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformOk);
  CHECK(P.State == MtkUfsPlatformLinkSuspended && P.RefClock == MtkUfsRefClockOff);
  CHECK(M.Attr[0xD0A8] == 1 && M.Reg[0x144 / 4] == 0);
  CHECK(M.HibernateCalls == 1 && M.Trace[0].Kind == 'H' && M.Trace[0].Value == 1);
  for (I = 0; I < M.TraceCount; I++) {
    if (M.Trace[I].Kind == 'S' && M.Trace[I].Args[1] == 32 && M.Trace[I].Args[2] == 0) break;
  }
  CHECK(I < M.TraceCount && M.Trace[I + 1].Kind == 'S' && M.Trace[I + 1].Args[1] == 8 &&
        M.Trace[I + 1].Args[2] == 0 && M.Trace[I + 1].Args[3] == 0);
  M.TraceCount = 0;
  CHECK(MtkUfsPlatformResume(&P) == MtkUfsPlatformOk);
  CHECK(P.State == MtkUfsPlatformLinkReady && P.RefClock == MtkUfsRefClockOn);
  CHECK(M.Attr[0xD0A8] == 0 && M.Reg[0x144 / 4] == 3 && M.HibernateCalls == 2);
  CHECK(M.Trace[0].Kind == 'S' && M.Trace[0].Args[1] == 8 && M.Trace[0].Args[2] == 1);
  CHECK(M.Trace[M.TraceCount - 1].Kind == 'W' || M.Trace[M.TraceCount - 1].Kind == 'H');

  Reset(0x10450000); Full(); M.Reg[0x58 / 4] = 1; I = M.TraceCount;
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformBusy);
  CHECK(P.State == MtkUfsPlatformLinkReady && M.TraceCount == I && M.HibernateCalls == 0);

  Reset(0x10450000); Full(); M.HibernateFailAt = 1; M.TraceCount = 0;
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformIoError);
  CHECK(P.State == MtkUfsPlatformLinkReady && P.RefClock == MtkUfsRefClockOn);
  CHECK(M.Attr[0xD0A8] == 0 && M.HibernateCalls == 2);

  Reset(0x10450000); Full(); M.HoldHibernateState = 1; M.TraceCount = 0;
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformTimeout);
  CHECK(P.State == MtkUfsPlatformLinkReady && P.RefClock == MtkUfsRefClockOn && M.Time >= 15000);

  Reset(0x10450000); Full(); CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformOk);
  M.HibernateFailAt = M.HibernateCalls + 1;
  CHECK(MtkUfsPlatformResume(&P) == MtkUfsPlatformIoError && P.State == MtkUfsPlatformFault);
}


static void ResetRetained(uint32_t Version) {
  MTK_UFS_PLATFORM_CONFIG C = Config;
  Reset(Version);
  C.AllowSramRetention = 1;
  CHECK(MtkUfsPlatformInitialize(&P, &Io, &C) == MtkUfsPlatformOk);
}
static unsigned SramCalls(unsigned Start, unsigned On) {
  unsigned I, Count = 0;
  for (I = Start; I < M.TraceCount; I++)
    if (M.Trace[I].Kind == 'S' && M.Trace[I].Args[1] == 32 && M.Trace[I].Args[2] == On) Count++;
  return Count;
}
static void TestRetainedSram(void) {
  static const uint32_t Versions[] = { 0x10380000, 0x20160706, 0x10440000, 0x10450000, 0x10450100, 0x10460000 };
  static const int64_t Errors[] = { -3, 1, 7, INT64_MIN, INT64_MAX };
  unsigned I, Start, Before, Calls, At;
  MTK_UFS_PLATFORM_CONFIG C = Config;
  C.AllowSramRetention = 2;
  Reset(0x10450000);
  CHECK(MtkUfsPlatformInitialize(&P, &Io, &C) == MtkUfsPlatformInvalid && M.TraceCount == 0);

  for (I = 0; I < sizeof(Versions) / sizeof(Versions[0]); I++) {
    ResetRetained(Versions[I]); Full();
    M.RejectSmcCommand = 32; M.RejectSmcResult = -1; Start = M.TraceCount;
    CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformOk);
    CHECK(P.State == MtkUfsPlatformLinkSuspended && P.RefClock == MtkUfsRefClockOff &&
          P.SramResume == MtkUfsSramRetained);
    CHECK(M.Attr[0xD0A8] == 1 && (M.Reg[0x22C8 / 4] >> 28) == 3 && M.Reg[0x34 / 4] == 1);
    CHECK(SramCalls(Start, 0) == 1 && SramCalls(Start, 1) == 0);
    Before = M.TraceCount;
    CHECK(MtkUfsPlatformResume(&P) == MtkUfsPlatformOk);
    CHECK(P.State == MtkUfsPlatformLinkReady && P.RefClock == MtkUfsRefClockOn &&
          P.SramResume == MtkUfsSramNoRestore);
    CHECK(M.Attr[0xD0A8] == 0 && (M.Reg[0x22C8 / 4] >> 28) == 2 && M.Reg[0x34 / 4] == 1);
    CHECK(SramCalls(Before, 0) == 0 && SramCalls(Before, 1) == 1);
  }

  /* A supported SRAM-off cannot be paired with unsupported SRAM-on, even
   * with the retention opt-in. No DME/Hibern8 exit may hide that failure. */
  ResetRetained(0x10450000); Full();
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformOk && P.SramResume == MtkUfsSramPowerOnRequired);
  M.RejectSmcCommand = 32; M.RejectSmcResult = -1; Start = M.HibernateCalls;
  CHECK(MtkUfsPlatformResume(&P) == MtkUfsPlatformIoError && P.State == MtkUfsPlatformFault);
  CHECK(M.HibernateCalls == Start && M.Attr[0xD0A8] == 1 && P.SramResume == MtkUfsSramPowerOnRequired);

  for (I = 0; I < sizeof(Errors) / sizeof(Errors[0]); I++) {
    ResetRetained(0x10450000); Full();
    M.RejectSmcCommand = 32; M.RejectSmcResult = Errors[I];
    CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformIoError);
    CHECK(P.State == MtkUfsPlatformFault && P.SramResume == MtkUfsSramPowerOnRequired);
    CHECK(M.Attr[0xD0A8] == 1 && M.HibernateCalls == 1);
    ResetRetained(0x10450000); Full();
    M.RejectSmcCommand = 32; M.RejectSmcResult = -1;
    CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformOk);
    M.RejectSmcResult = Errors[I]; Start = M.HibernateCalls;
    CHECK(MtkUfsPlatformResume(&P) == MtkUfsPlatformIoError && P.State == MtkUfsPlatformFault);
    CHECK(M.HibernateCalls == Start && M.Attr[0xD0A8] == 1 && P.SramResume == MtkUfsSramRetained);
  }

  /* Capturing -1 in a failed callback is not an unsupported-service proof. */
  ResetRetained(0x10450000); Full();
  M.RejectSmcCommand = 32; M.RejectSmcResult = -1;
  M.SmcTransportFailAt = M.SmcCalls + 1; Start = M.TraceCount;
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformIoError && P.State == MtkUfsPlatformFault);
  CHECK(P.SramResume == MtkUfsSramPowerOnRequired && SramCalls(Start, 1) == 1 && M.Attr[0xD0A8] == 1);
  ResetRetained(0x10450000); Full();
  M.RejectSmcCommand = 32; M.RejectSmcResult = -1;
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformOk);
  M.SmcTransportFailAt = M.SmcCalls + 3; /* refclk PRE, POST, then SRAM-on */
  CHECK(MtkUfsPlatformResume(&P) == MtkUfsPlatformIoError && P.State == MtkUfsPlatformFault);
  CHECK(P.SramResume == MtkUfsSramRetained && M.Attr[0xD0A8] == 1 && M.HibernateCalls == 1);

  /* Refclock failure restores the retained link with a matching cmd32-on. */
  ResetRetained(0x10450000); Full();
  M.RejectSmcCommand = 32; M.RejectSmcResult = -1; M.AckOff = 0; Start = M.TraceCount;
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformTimeout && P.State == MtkUfsPlatformLinkReady);
  CHECK(P.SramResume == MtkUfsSramNoRestore && P.RefClock == MtkUfsRefClockOn && M.Attr[0xD0A8] == 0);
  CHECK(P.RecoveryStatus == MtkUfsPlatformOk && SramCalls(Start, 0) == 1 && SramCalls(Start, 1) == 1);

  /* New requests or lost host state cannot be repaired by this retained path. */
  for (I = 0; I < 4; I++) {
    ResetRetained(0x10450000); Full();
    M.RejectSmcCommand = 32; M.RejectSmcResult = -1;
    CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformOk);
    if (I < 2) M.Reg[0x34 / 4] = I ? UINT32_MAX : 0;
    else M.Reg[(I == 2 ? 0x58 : 0x78) / 4] = 1;
    Start = M.TraceCount; Before = M.SmcCalls;
    CHECK(MtkUfsPlatformResume(&P) != MtkUfsPlatformOk && P.State == MtkUfsPlatformFault);
    CHECK(M.TraceCount == Start && M.SmcCalls == Before && M.Attr[0xD0A8] == 1 && M.HibernateCalls == 1);
    CHECK(MtkUfsPlatformResume(&P) == MtkUfsPlatformWrongState && M.TraceCount == Start);
  }
  ResetRetained(0x10450000); M.RejectSmcCommand = 2; M.RejectSmcResult = -1;
  CHECK(MtkUfsPlatformResetDevice(&P) == MtkUfsPlatformIoError && P.State == MtkUfsPlatformFault);

  /* Every production MMIO/DME/SMC step in both retained directions fails
   * closed and retains a recoverable ready state or an explicit fault. */
  ResetRetained(0x10450000); Full();
  M.RejectSmcCommand = 32; M.RejectSmcResult = -1; Before = M.Calls;
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformOk);
  Calls = M.Calls - Before;
  for (At = 1; At <= Calls; At++) {
    ResetRetained(0x10450000); Full();
    M.RejectSmcCommand = 32; M.RejectSmcResult = -1; M.FailAt = M.Calls + At;
    CHECK(MtkUfsPlatformSuspend(&P) != MtkUfsPlatformOk);
    CHECK(P.State == MtkUfsPlatformLinkReady || P.State == MtkUfsPlatformFault);
    if (P.State == MtkUfsPlatformLinkReady)
      CHECK(P.RefClock == MtkUfsRefClockOn && P.SramResume == MtkUfsSramNoRestore &&
            M.Attr[0xD0A8] == 0 && (M.Reg[0x22C8 / 4] >> 28) == 2);
  }
  ResetRetained(0x10450000); Full();
  M.RejectSmcCommand = 32; M.RejectSmcResult = -1;
  CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformOk); Before = M.Calls;
  CHECK(MtkUfsPlatformResume(&P) == MtkUfsPlatformOk); Calls = M.Calls - Before;
  for (At = 1; At <= Calls; At++) {
    ResetRetained(0x10450000); Full();
    M.RejectSmcCommand = 32; M.RejectSmcResult = -1;
    CHECK(MtkUfsPlatformSuspend(&P) == MtkUfsPlatformOk); M.FailAt = M.Calls + At;
    CHECK(MtkUfsPlatformResume(&P) != MtkUfsPlatformOk && P.State == MtkUfsPlatformFault);
  }
}

static void TestRecoveryFailures(void) {
  unsigned CleanupWrite, CleanupSmc, At;
  Reset(0x10450000); M.AckOn = 0;
  CHECK(MtkUfsPlatformSetRefClock(&P, 1) == MtkUfsPlatformTimeout);
  CleanupWrite = 3 + 1 + 1 + 31 + 1;
  CleanupSmc = M.Calls;
  for (At = CleanupWrite; At <= CleanupSmc; At++) {
    Reset(0x10450000); M.AckOn = 0; M.FailAt = At;
    CHECK(MtkUfsPlatformSetRefClock(&P, 1) == MtkUfsPlatformTimeout);
    CHECK(P.State == MtkUfsPlatformFault && P.RefClock == MtkUfsRefClockUnknown);
    CHECK(P.RecoveryStatus == MtkUfsPlatformIoError);
  }
  Reset(0x10450000); M.AckOn = M.AckOff = 0;
  CHECK(MtkUfsPlatformSetRefClock(&P, 1) == MtkUfsPlatformTimeout);
  CHECK(P.RecoveryStatus == MtkUfsPlatformTimeout && P.RefClock == MtkUfsRefClockUnknown && M.Time == 6000);
  Reset(0x10450000); M.FailSmcAt = 3; M.SmcResult = -3; M.Reg[0x34 / 4] = 1; M.StuckHce = 1;
  /* The reset-high call is the second call here; record a separate failure
   * while preserving the original bounded HCE-stop timeout. */
  M.FailSmcAt = 2;
  CHECK(MtkUfsPlatformResetDevice(&P) == MtkUfsPlatformTimeout);
  CHECK(P.RecoveryStatus == MtkUfsPlatformIoError && P.State == MtkUfsPlatformFault);
  Reset(0x10450000); M.AckOff = 0; M.Reg[0x144 / 4] = 3;
  CHECK(MtkUfsPlatformSetRefClock(&P, 0) == MtkUfsPlatformTimeout);
  CHECK(P.RefClock == MtkUfsRefClockUnknown && M.Time == 3000);
}

static void TestStoppedRestart(void) {
  static const uint32_t Roots[] = { 0x34, 0x60, 0x80, 0x58, 0x78, 0x50, 0x54, 0x70, 0x74, 0x24 };
  static const MTK_UFS_PLATFORM_STATE States[] = {
    MtkUfsPlatformHostPrepared, MtkUfsPlatformHostEnabled, MtkUfsPlatformLinkPrepared,
    MtkUfsPlatformLinkReady, MtkUfsPlatformFault
  };
  MTK_UFS_PLATFORM Before;
  unsigned I, J, V, Calls, Start, At;
  MTK_UFS_PLATFORM_STATUS S;
  CHECK(MtkUfsPlatformRestartStopped(NULL) == MtkUfsPlatformInvalid);
  memset(&P, 0, sizeof(P));
  CHECK(MtkUfsPlatformRestartStopped(&P) == MtkUfsPlatformInvalid);

  Reset(0x10450000); Full(); M.Reg[0x34 / 4] = 0; M.Reg[0x144 / 4] = 0;
  ConfirmRefClockOff();
  P.Config.RefClockGatingWaitUs = 30; P.Config.AllowSramRetention = 1;
  Before = P; Start = M.Calls;
  CHECK(MtkUfsPlatformRestartStopped(&P) == MtkUfsPlatformOk);
  Calls = M.Calls - Start;
  CHECK(P.State == MtkUfsPlatformHostPrepared && P.RefClock == MtkUfsRefClockOn);
  CHECK(P.SramResume == MtkUfsSramNoRestore && P.RecoveryStatus == MtkUfsPlatformOk);
  CHECK(memcmp(&P.Config, &Before.Config, sizeof(P.Config)) == 0);
  CHECK(memcmp(&P.Io, &Before.Io, sizeof(P.Io)) == 0);
  CHECK(M.Reg[0x34 / 4] == 0);
  M.Reg[0x34 / 4] = 1;
  CHECK(MtkUfsPlatformPostHce(&P) == MtkUfsPlatformOk);
  CHECK(MtkUfsPlatformPreLink(&P) == MtkUfsPlatformOk);
  CHECK(MtkUfsPlatformPostLink(&P) == MtkUfsPlatformOk && P.State == MtkUfsPlatformLinkReady);

  /* A new driver may not recycle any old DMA root or active host, even if
   * only reserved HCE/RSR bits or a high address word remain nonzero. */
  for (I = 0; I < sizeof(States) / sizeof(States[0]); I++)
    for (J = 0; J < sizeof(Roots) / sizeof(Roots[0]); J++)
      for (V = 0; V < 3; V++) {
        Reset(0x10450000); Full(); M.Reg[0x34 / 4] = 0; M.Reg[0x144 / 4] = 0;
        P.State = States[I]; ConfirmRefClockOff();
        M.Reg[Roots[J] / 4] = V == 0 ? 1U : V == 1 ? 0x80000000U : UINT32_MAX;
        Before = P; Start = M.TraceCount;
        S = MtkUfsPlatformRestartStopped(&P);
        CHECK(S == (V == 2 ? MtkUfsPlatformIoError : MtkUfsPlatformBusy));
        CHECK(memcmp(&P, &Before, sizeof(P)) == 0 && M.TraceCount == Start);
      }

  for (I = 0; I < 4; I++) {
    Reset(0x10450000); Full(); M.Reg[0x34 / 4] = 0; M.Reg[0x144 / 4] = 0;
    ConfirmRefClockOff();
    if (I == 0) P.State = MtkUfsPlatformIdle;
    if (I == 1) P.State = MtkUfsPlatformLinkSuspended;
    if (I == 2) P.SramResume = MtkUfsSramPowerOnRequired;
    if (I == 3) P.SramResume = MtkUfsSramRetained;
    Before = P; Start = M.Calls;
    CHECK(MtkUfsPlatformRestartStopped(&P) == MtkUfsPlatformWrongState);
    CHECK(memcmp(&P, &Before, sizeof(P)) == 0 && M.Calls == Start);
  }

  /* Fail each actual preflight/reset/refclock/host-prepare operation. Failed
   * preflight is read-only; started resets remain Fault until another Stop. */
  for (At = 1; At <= Calls; At++) {
    Reset(0x10450000); Full(); M.Reg[0x34 / 4] = 0; M.Reg[0x144 / 4] = 0;
    ConfirmRefClockOff();
    Before = P; Start = M.Calls; M.FailAt = Start + At;
    CHECK(MtkUfsPlatformRestartStopped(&P) == MtkUfsPlatformIoError);
    if (At <= sizeof(Roots) / sizeof(Roots[0]) + 1U)
      CHECK(memcmp(&P, &Before, sizeof(P)) == 0);
    else CHECK(P.State == MtkUfsPlatformFault);
    M.FailAt = 0;
    for (J = 0; J < sizeof(Roots) / sizeof(Roots[0]); J++) M.Reg[Roots[J] / 4] = 0;
    M.Reg[0x144 / 4] = 0;
    ConfirmRefClockOff();
    CHECK(MtkUfsPlatformRestartStopped(&P) == MtkUfsPlatformOk);
    CHECK(P.State == MtkUfsPlatformHostPrepared);
  }

  /* The software marker and the raw request/ack bits must both say off.
   * A stale On/Unknown marker is rejected before any MMIO, even if the raw
   * register happens to read zero. */
  for (I = 0; I < 2; I++) {
    unsigned Trace, Smcs;
    Reset(0x10450000); Full(); M.Reg[0x34 / 4] = 0; M.Reg[0x144 / 4] = 0;
    P.RefClock = I == 0 ? MtkUfsRefClockOn : MtkUfsRefClockUnknown;
    Trace = M.TraceCount; Smcs = M.SmcCalls; Start = M.Calls;
    Before = P;
    S = MtkUfsPlatformRestartStopped(&P);
    CHECK(S == MtkUfsPlatformWrongState);
    CHECK(memcmp(&P, &Before, sizeof(P)) == 0 && M.TraceCount == Trace &&
          M.SmcCalls == Smcs && M.Calls == Start);
  }

  /* Once the software marker says off, stale hardware request/ack bits or an
   * unreadable register are caught by a fresh raw read before reset/SMC. */
  for (I = 0; I < 2; I++) {
    unsigned Trace, Smcs;
    Reset(0x10450000); Full(); M.Reg[0x34 / 4] = 0; M.Reg[0x144 / 4] = 0;
    ConfirmRefClockOff();
    M.Reg[0x144 / 4] = I == 0 ? 3U : UINT32_MAX;
    Trace = M.TraceCount; Smcs = M.SmcCalls; Start = M.Calls;
    Before = P;
    S = MtkUfsPlatformRestartStopped(&P);
    CHECK(S == (I == 0 ? MtkUfsPlatformBusy : MtkUfsPlatformIoError));
    CHECK(memcmp(&P, &Before, sizeof(P)) == 0 && M.TraceCount == Trace &&
          M.SmcCalls == Smcs && M.Calls > Start);
  }
}

static void TestFullStop(void) {
  static const uint32_t Roots[] = {
    0x34U, 0x60U, 0x80U, 0x58U, 0x78U, 0x50U, 0x54U, 0x70U, 0x74U, 0x24U
  };
  unsigned I, J, Start, Smcs, Hibernates, DmeSetAt;

  Reset(0x10450000);
  CHECK(MtkUfsPlatformPrepareStop(&P) == MtkUfsPlatformWrongState);
  CHECK(M.TraceCount == 0 && P.State == MtkUfsPlatformIdle);

  Reset(0x10450000);
  Full();
  M.TraceCount = 0;
  Smcs = M.SmcCalls;
  Hibernates = M.HibernateCalls;
  CHECK(MtkUfsPlatformPrepareStop(&P) == MtkUfsPlatformOk);
  CHECK(P.State == MtkUfsPlatformStopPrepared && P.RefClock == MtkUfsRefClockOn);
  CHECK(P.SramResume == MtkUfsSramNoRestore && M.Attr[0xD0A8] == 1U);
  CHECK(M.SmcCalls == Smcs && M.HibernateCalls == Hibernates + 1U);
  CHECK(M.Trace[0].Kind == 'H' && M.Trace[0].Value == 1U);
  for (I = 0; I < M.TraceCount; I++) CHECK(M.Trace[I].Kind != 'S');

  /* Each root is independently required to be exactly zero before the
   * refclock can be gated. Reserved bits and all-ones reads are not proof. */
  for (I = 0; I < sizeof(Roots) / sizeof(Roots[0]); I++) {
    for (J = 0; J < 3; J++) {
      uint32_t Seed = J == 0 ? 1U : J == 1 ? 0x80000000U : UINT32_MAX;
      MTK_UFS_PLATFORM_STATUS Expected = J == 2 ? MtkUfsPlatformIoError : MtkUfsPlatformBusy;
      Reset(0x10450000);
      Full();
      CHECK(MtkUfsPlatformPrepareStop(&P) == MtkUfsPlatformOk);
      M.Reg[0x34U / 4U] = 0U;
      M.Reg[Roots[I] / 4U] = Seed;
      Smcs = M.SmcCalls;
      Start = M.TraceCount;
      CHECK(MtkUfsPlatformHostStopped(&P) == Expected);
      CHECK(P.State == MtkUfsPlatformStopPrepared && P.RefClock == MtkUfsRefClockOn);
      CHECK(M.SmcCalls == Smcs && M.Reg[0x144U / 4U] == 3U);
      for (J = Start; J < M.TraceCount; J++) CHECK(M.Trace[J].Kind != 'S' && M.Trace[J].Kind != 'W');
    }
  }

  /* A failed root read leaves the stop prepared and performs no clock work. */
  for (I = 0; I < sizeof(Roots) / sizeof(Roots[0]); I++) {
    Reset(0x10450000);
    Full();
    CHECK(MtkUfsPlatformPrepareStop(&P) == MtkUfsPlatformOk);
    M.Reg[0x34U / 4U] = 0U;
    Start = M.Calls;
    Smcs = M.SmcCalls;
    M.FailAt = Start + I + 1U;
    CHECK(MtkUfsPlatformHostStopped(&P) == MtkUfsPlatformIoError);
    CHECK(P.State == MtkUfsPlatformStopPrepared && P.RefClock == MtkUfsRefClockOn);
    CHECK(M.SmcCalls == Smcs && M.Reg[0x144U / 4U] == 3U);
  }

  Reset(0x10450000);
  Full();
  CHECK(MtkUfsPlatformPrepareStop(&P) == MtkUfsPlatformOk);
  M.Reg[0x34U / 4U] = 0U;
  Smcs = M.SmcCalls;
  Start = M.TraceCount;
  CHECK(MtkUfsPlatformHostStopped(&P) == MtkUfsPlatformOk);
  CHECK(P.State == MtkUfsPlatformStopped && P.RefClock == MtkUfsRefClockOff);
  CHECK(M.Reg[0x144U / 4U] == 0U && M.SmcCalls == Smcs + 2U);
  for (I = Start; I < M.TraceCount; I++) {
    if (M.Trace[I].Kind == 'S') CHECK(M.Trace[I].Args[1] == 8U);
  }
  Smcs = M.SmcCalls;
  CHECK(MtkUfsPlatformHostStopped(&P) == MtkUfsPlatformOk);
  CHECK(M.SmcCalls == Smcs);

  /* If the second refclock notification fails after the hardware gate lands,
   * retain HCE-stopped progress and retry only while the roots remain zero. */
  Reset(0x10450000);
  Full();
  CHECK(MtkUfsPlatformPrepareStop(&P) == MtkUfsPlatformOk);
  M.Reg[0x34U / 4U] = 0U;
  M.FailSmcAt = M.SmcCalls + 2U;
  M.SmcResult = -3;
  CHECK(MtkUfsPlatformHostStopped(&P) == MtkUfsPlatformIoError);
  CHECK(P.State == MtkUfsPlatformHceStopped && P.RefClock != MtkUfsRefClockOff);
  CHECK(M.Reg[0x144U / 4U] == 0U);
  Smcs = M.SmcCalls;
  M.FailSmcAt = 0U;
  CHECK(MtkUfsPlatformHostStopped(&P) == MtkUfsPlatformOk);
  CHECK(P.State == MtkUfsPlatformStopped && P.RefClock == MtkUfsRefClockOff);
  CHECK(M.SmcCalls > Smcs);
  for (I = 0; I < M.TraceCount; I++) {
    if (M.Trace[I].Kind == 'S') CHECK(M.Trace[I].Args[1] != 32U);
  }

  /* A UIC powerdown write error is unwound while the HCE and refclock remain
   * available; it cannot be mistaken for a completed shutdown. */
  Reset(0x10450000);
  Full();
  DmeSetAt = M.Calls + 11U;
  M.FailAt = DmeSetAt;
  Hibernates = M.HibernateCalls;
  CHECK(MtkUfsPlatformPrepareStop(&P) == MtkUfsPlatformIoError);
  CHECK(P.State == MtkUfsPlatformLinkReady && P.RefClock == MtkUfsRefClockOn);
  CHECK(M.Attr[0xD0A8] == 0U && M.HibernateCalls == Hibernates + 2U);
  CHECK(M.SmcCalls == 4U);
}

int main(void) {
  TestTraceAndVersions(); TestFailures(); TestSmcReturnContracts(); TestTimeoutsAndGuards(); TestRecoveryFailures(); TestSuspendResume(); TestRetainedSram(); TestStoppedRestart(); TestFullStop();
  printf("UFS platform sequence: %u checks passed (fake MMIO/SMC/UIC; no device)\n", Checks);
  return 0;
}
