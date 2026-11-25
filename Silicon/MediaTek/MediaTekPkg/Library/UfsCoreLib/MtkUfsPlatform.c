/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/MtkUfsPlatform.h>

#define PLATFORM_SIGNATURE  0x55465031U
#define REG_HCS             0x30U
#define REG_HCE             0x34U
#define REG_IE              0x24U
#define REG_UTRLDBR         0x58U
#define REG_UTMRLDBR        0x78U
#define REG_AHIT            0x18U
#define REG_XOUFS           0x140U
#define REG_REFCLK          0x144U
#define REG_OPT             0x160U
#define REG_IP_VERSION      0x2240U
#define REG_DEBUG_SEL       0x22C0U
#define REG_UFS_PROBE       0x22C8U
#define REG_DEBUG_SEL_B0    0x22D0U
#define REG_DEBUG_SEL_B1    0x22D4U
#define REG_DEBUG_SEL_B2    0x22D8U
#define REG_DEBUG_SEL_B3    0x22DCU
#define REG_1048_OPTION     0x2308U
#define DME_SAVEPOWER       0xD0A6U
#define DME_DEBUGCLOCK      0xD0A1U
#define DME_POWERDOWN       0xD0A8U
#define DME_LINK_TUNING     0xD09EU
#define POLL_STEP_US        100U
#define UFS_LINK_UP         2U
#define UFS_LINK_HIBERN8    3U
#define UFS_HCE_BASE        1U
#define UFS_HIB_ENTER       8U
#define UFS_HIB_EXIT        13U

static int Valid(const MTK_UFS_PLATFORM *P)
{
  return P != NULL && P->Signature == PLATFORM_SIGNATURE;
}

static int Fits(uint32_t Length, uint32_t Offset)
{
  return (Offset & 3U) == 0 && Offset <= Length && Length - Offset >= 4U;
}

static MTK_UFS_PLATFORM_STATUS Read(MTK_UFS_PLATFORM *P, uint32_t Offset, uint32_t *Value)
{
  if (!Fits(P->Config.ControllerLength, Offset)) return MtkUfsPlatformUnsupported;
  return P->Io.Read32(P->Io.Context, MtkUfsControllerBank, Offset, Value);
}

static MTK_UFS_PLATFORM_STATUS Write(MTK_UFS_PLATFORM *P, MTK_UFS_MMIO_BANK Bank, uint32_t Offset, uint32_t Value)
{
  uint32_t Length = Bank == MtkUfsControllerBank ? P->Config.ControllerLength : P->Config.ResetLength;
  if (!Fits(Length, Offset)) return MtkUfsPlatformUnsupported;
  return P->Io.Write32(P->Io.Context, Bank, Offset, Value);
}

static MTK_UFS_PLATFORM_STATUS Modify(MTK_UFS_PLATFORM *P, uint32_t Offset, uint32_t Clear, uint32_t Set)
{
  uint32_t Value;
  MTK_UFS_PLATFORM_STATUS Status = Read(P, Offset, &Value);
  if (Status != MtkUfsPlatformOk) return Status;
  if (Value == UINT32_MAX) return MtkUfsPlatformIoError;
  return Write(P, MtkUfsControllerBank, Offset, (Value & ~Clear) | Set);
}

static MTK_UFS_PLATFORM_STATUS CallSmc(MTK_UFS_PLATFORM *P, uint32_t Command, uint32_t Value, uint32_t Stage, int *Completed)
{
  uint64_t Args[8] = { MTK_UFS_SMC_FUNCTION, Command, Value, Stage, 0, 0, 0, 0 };
  MTK_UFS_PLATFORM_STATUS Status;
  Args[4] = P->Config.ControllerId;
  if (Completed != NULL) *Completed = 0;
  P->LastSmcResult = 0;
  Status = P->Io.Smc(P->Io.Context, Args, &P->LastSmcResult);
  if (Status != MtkUfsPlatformOk) return Status;
  if (Completed != NULL) *Completed = 1;
  /* T70's secure handler omits ref-clock notifications; the HCI ACK remains authoritative. */
  if (P->LastSmcResult == 0 || (Command == 8U && P->LastSmcResult == -1)) return MtkUfsPlatformOk;
  return MtkUfsPlatformIoError;
}

static MTK_UFS_PLATFORM_STATUS Smc(MTK_UFS_PLATFORM *P, uint32_t Command, uint32_t Value, uint32_t Stage)
{
  return CallSmc(P, Command, Value, Stage, NULL);
}

static MTK_UFS_PLATFORM_STATUS Poll(MTK_UFS_PLATFORM *P, uint32_t Offset, uint32_t Mask, uint32_t Expected, uint32_t Timeout)
{
  uint32_t Elapsed = 0, Value;
  MTK_UFS_PLATFORM_STATUS Status;
  for (;;) {
    Status = Read(P, Offset, &Value);
    if (Status != MtkUfsPlatformOk) return Status;
    if (Value == UINT32_MAX) return MtkUfsPlatformIoError;
    if ((Value & Mask) == Expected) return MtkUfsPlatformOk;
    if (Elapsed == Timeout) return MtkUfsPlatformTimeout;
    P->Io.DelayUs(P->Io.Context, POLL_STEP_US);
    Elapsed += POLL_STEP_US;
  }
}

static MTK_UFS_PLATFORM_STATUS Fault(MTK_UFS_PLATFORM *P, MTK_UFS_PLATFORM_STATUS Status)
{
  P->State = MtkUfsPlatformFault;
  return Status;
}

static void Recovery(MTK_UFS_PLATFORM *P, MTK_UFS_PLATFORM_STATUS Status)
{
  if (P->RecoveryStatus == MtkUfsPlatformOk && Status != MtkUfsPlatformOk) P->RecoveryStatus = Status;
}

static MTK_UFS_PLATFORM_STATUS Quiescent(MTK_UFS_PLATFORM *P, int RequireStopped)
{
  uint32_t Value;
  MTK_UFS_PLATFORM_STATUS Status;
  const uint32_t Offsets[] = { REG_UTRLDBR, REG_UTMRLDBR, REG_HCE };
  size_t I;
  for (I = 0; I < sizeof(Offsets) / sizeof(Offsets[0]); I++) {
    Status = Read(P, Offsets[I], &Value);
    if (Status != MtkUfsPlatformOk) return Status;
    if (Value == UINT32_MAX) return MtkUfsPlatformIoError;
    if ((I < 2 && Value != 0) || (I == 2 && RequireStopped && (Value & 1U))) return MtkUfsPlatformBusy;
  }
  return MtkUfsPlatformOk;
}

MTK_UFS_PLATFORM_STATUS MtkUfsPlatformInitialize(MTK_UFS_PLATFORM *P, const MTK_UFS_PLATFORM_IO *Io, const MTK_UFS_PLATFORM_CONFIG *Config)
{
  size_t I, J;
  if (P == NULL || Io == NULL || Config == NULL || Io->Read32 == NULL || Io->Write32 == NULL ||
      Io->DelayUs == NULL || Io->Smc == NULL || Io->Dme == NULL || Io->Hibernate == NULL ||
      !Fits(Config->ControllerLength, REG_IP_VERSION) ||
      Config->RefClockGatingWaitUs > 10000U || Config->RefClockUngatingWaitUs > 10000U ||
      Config->AllowSramRetention > 1U) return MtkUfsPlatformInvalid;
  for (I = 0; I < 3; I++) {
    const MTK_UFS_RESET_LINE *R = &Config->Reset[I];
    if (!Fits(Config->ResetLength, R->AssertOffset) || !Fits(Config->ResetLength, R->DeassertOffset) ||
        R->AssertOffset == R->DeassertOffset || R->Mask == 0 || (R->Mask & (R->Mask - 1U))) return MtkUfsPlatformInvalid;
    for (J = 0; J < I; J++) {
      if ((R->AssertOffset == Config->Reset[J].AssertOffset && R->Mask == Config->Reset[J].Mask) ||
          (R->DeassertOffset == Config->Reset[J].DeassertOffset && R->Mask == Config->Reset[J].Mask)) return MtkUfsPlatformInvalid;
    }
  }
  P->Io = *Io;
  P->Config = *Config;
  P->State = MtkUfsPlatformIdle;
  P->RefClock = MtkUfsRefClockUnknown;
  P->SramResume = MtkUfsSramNoRestore;
  P->RecoveryStatus = MtkUfsPlatformOk;
  P->LastSmcResult = 0;
  P->IpVersion = 0;
  P->Signature = PLATFORM_SIGNATURE;
  return MtkUfsPlatformOk;
}

static MTK_UFS_PLATFORM_STATUS RefClockTransition(MTK_UFS_PLATFORM *P, int Enable, int RequireStopped)
{
  MTK_UFS_PLATFORM_STATUS Status, Cleanup;
  uint32_t Expected;
  Status = Quiescent(P, RequireStopped);
  if (Status != MtkUfsPlatformOk) return Status;
  P->RefClock = MtkUfsRefClockUnknown;
  Status = Smc(P, 8U, (uint32_t)Enable, 0U);
  if (Status != MtkUfsPlatformOk) return Status;
  if (!Enable) P->Io.DelayUs(P->Io.Context, P->Config.RefClockGatingWaitUs);
  Status = Write(P, MtkUfsControllerBank, REG_REFCLK, (uint32_t)Enable);
  Expected = Enable ? 3U : 0U;
  if (Status == MtkUfsPlatformOk) Status = Poll(P, REG_REFCLK, 3U, Expected, MTK_UFS_REFCLK_TIMEOUT_US);
  if (Status == MtkUfsPlatformOk) {
    if (Enable) P->Io.DelayUs(P->Io.Context, P->Config.RefClockUngatingWaitUs);
    Status = Smc(P, 8U, (uint32_t)Enable, 1U);
    if (Status == MtkUfsPlatformOk) {
      P->RefClock = Enable ? MtkUfsRefClockOn : MtkUfsRefClockOff;
      return Status;
    }
  }
  /* The failed request must not be advertised as an enabled clock. For an ON
   * failure cancel its request and balance the secure firmware notification. */
  if (Enable) {
    Cleanup = Write(P, MtkUfsControllerBank, REG_REFCLK, 0U);
    Recovery(P, Cleanup);
    if (Cleanup == MtkUfsPlatformOk) {
      Cleanup = Poll(P, REG_REFCLK, 3U, 0U, MTK_UFS_REFCLK_TIMEOUT_US);
      Recovery(P, Cleanup);
    }
    Recovery(P, Smc(P, 8U, 0U, 1U));
    if (P->RecoveryStatus == MtkUfsPlatformOk) P->RefClock = MtkUfsRefClockOff;
  } else {
    Recovery(P, Smc(P, 8U, 0U, 1U));
  }
  return Status;
}

MTK_UFS_PLATFORM_STATUS MtkUfsPlatformSetRefClock(MTK_UFS_PLATFORM *P, int Enable)
{
  MTK_UFS_PLATFORM_STATUS Status;
  if (!Valid(P) || (Enable != 0 && Enable != 1)) return MtkUfsPlatformInvalid;
  if (P->State != MtkUfsPlatformIdle) return MtkUfsPlatformWrongState;
  Status = RefClockTransition(P, Enable, 1);
  return Status == MtkUfsPlatformOk ? Status : Fault(P, Status);
}

MTK_UFS_PLATFORM_STATUS MtkUfsPlatformResetDevice(MTK_UFS_PLATFORM *P)
{
  MTK_UFS_PLATFORM_STATUS Status, Release;
  if (!Valid(P)) return MtkUfsPlatformInvalid;
  if (P->State != MtkUfsPlatformIdle) return MtkUfsPlatformWrongState;
  Status = Quiescent(P, 0);
  if (Status != MtkUfsPlatformOk) return Status;
  Status = Smc(P, 2U, 0U, 0U);
  if (Status != MtkUfsPlatformOk) {
    P->Io.DelayUs(P->Io.Context, 10U);
    Recovery(P, Smc(P, 2U, 1U, 0U));
    P->Io.DelayUs(P->Io.Context, 10000U);
    return Fault(P, Status);
  }
  Status = Write(P, MtkUfsControllerBank, REG_HCE, 0U);
  if (Status == MtkUfsPlatformOk) Status = Poll(P, REG_HCE, 1U, 0U, MTK_UFS_HOST_TIMEOUT_US);
  P->Io.DelayUs(P->Io.Context, 10U);
  /* Release reset even when stopping HCE failed, and retain both errors. */
  Release = Smc(P, 2U, 1U, 0U);
  P->Io.DelayUs(P->Io.Context, 10000U);
  if (Status != MtkUfsPlatformOk) {
    Recovery(P, Release);
    return Fault(P, Status);
  }
  return Release == MtkUfsPlatformOk ? Release : Fault(P, Release);
}

static MTK_UFS_PLATFORM_STATUS HostReset(MTK_UFS_PLATFORM *P)
{
  static const unsigned Order[] = { 0U, 2U, 1U };
  unsigned I, Asserted = 0;
  MTK_UFS_PLATFORM_STATUS Status = MtkUfsPlatformOk, Release;
  for (I = 0; I < 3; I++) {
    const MTK_UFS_RESET_LINE *R = &P->Config.Reset[Order[I]];
    Status = Write(P, MtkUfsResetBank, R->AssertOffset, R->Mask);
    /* A failed bus write can have reached the device: attempt its release too. */
    Asserted++;
    if (Status != MtkUfsPlatformOk) break;
  }
  P->Io.DelayUs(P->Io.Context, 100U);
  while (Asserted != 0) {
    const MTK_UFS_RESET_LINE *R = &P->Config.Reset[Order[--Asserted]];
    Release = Write(P, MtkUfsResetBank, R->DeassertOffset, R->Mask);
    Recovery(P, Release);
    if (Status == MtkUfsPlatformOk) Status = Release;
  }
  return Status;
}

MTK_UFS_PLATFORM_STATUS MtkUfsPlatformPreHce(MTK_UFS_PLATFORM *P)
{
  uint32_t Raw;
  MTK_UFS_PLATFORM_STATUS Status;
  if (!Valid(P)) return MtkUfsPlatformInvalid;
  if (P->State != MtkUfsPlatformIdle || P->RefClock != MtkUfsRefClockOn) return MtkUfsPlatformWrongState;
  Status = Quiescent(P, 1);
  if (Status != MtkUfsPlatformOk) return Status;
  Status = Read(P, REG_IP_VERSION, &Raw);
  if (Status != MtkUfsPlatformOk) return Fault(P, Status);
  if (Raw == 0 || Raw == UINT32_MAX) return Fault(P, MtkUfsPlatformIoError);
  P->IpVersion = ((Raw | 0x01000000U) >> 24) == 1U ? (Raw & 0x00FFFFFFU) | 0x10000000U : Raw;
  /* This revision uses a register outside the T70Pro 0x2300 DT resource. */
  if (P->IpVersion == 0x10480000U && !Fits(P->Config.ControllerLength, REG_1048_OPTION)) return Fault(P, MtkUfsPlatformUnsupported);
  Status = HostReset(P);
  if (Status != MtkUfsPlatformOk) return Fault(P, Status);
  Status = Write(P, MtkUfsControllerBank, REG_AHIT, 0U);
  if (Status == MtkUfsPlatformOk) Status = Modify(P, REG_XOUFS, 0, 0x80U);
  if (Status == MtkUfsPlatformOk && P->IpVersion > 0x1044FFFFU && P->IpVersion != 0x20160706U) {
    Status = Modify(P, REG_OPT, 0x007FFF00U, 0x00453000U);
    if (Status == MtkUfsPlatformOk && P->IpVersion > 0x1045FFFFU) Status = Modify(P, REG_OPT, 0x10000000U, 0x02000000U);
    if (Status == MtkUfsPlatformOk && P->IpVersion == 0x10480000U) Status = Modify(P, REG_1048_OPTION, 0, 1U);
  }
  if (Status != MtkUfsPlatformOk) return Fault(P, Status);
  P->State = MtkUfsPlatformHostPrepared;
  return MtkUfsPlatformOk;
}

static MTK_UFS_PLATFORM_STATUS DmeSet(MTK_UFS_PLATFORM *P, uint16_t Attribute, uint32_t Value);
static MTK_UFS_PLATFORM_STATUS WaitLinkState(MTK_UFS_PLATFORM *P, uint32_t Expected, uint32_t TimeoutUs);
static MTK_UFS_PLATFORM_STATUS VerifyRefClockOff(MTK_UFS_PLATFORM *P);

MTK_UFS_PLATFORM_STATUS MtkUfsPlatformRestartStopped(MTK_UFS_PLATFORM *P)
{
  static const uint32_t Stopped[] = { REG_HCE, 0x60U, 0x80U, REG_UTRLDBR,
    REG_UTMRLDBR, 0x50U, 0x54U, 0x70U, 0x74U, 0x24U };
  MTK_UFS_PLATFORM_IO Io;
  MTK_UFS_PLATFORM_CONFIG Config;
  MTK_UFS_PLATFORM_STATUS Status;
  uint32_t Value;
  size_t I;
  if (!Valid(P)) return MtkUfsPlatformInvalid;
  /* A controller reset cannot restore SRAM that a sleep transaction powered off. */
  if (P->State == MtkUfsPlatformIdle || P->State == MtkUfsPlatformLinkSuspended ||
      P->RefClock != MtkUfsRefClockOff || P->SramResume != MtkUfsSramNoRestore) {
    return MtkUfsPlatformWrongState;
  }
  for (I = 0; I < sizeof(Stopped) / sizeof(Stopped[0]); I++) {
    Status = Read(P, Stopped[I], &Value);
    if (Status != MtkUfsPlatformOk) return Status;
    if (Value == UINT32_MAX) return MtkUfsPlatformIoError;
    if (Value != 0) return MtkUfsPlatformBusy;
  }
  Status = VerifyRefClockOff(P);
  if (Status != MtkUfsPlatformOk) return Status;
  /* The caller has retired every old request and still owns powered MMIO.
   * Start a new reset transaction without dropping the board's retained VEMC
   * enable state or exclusive gate ownership. */
  Io = P->Io;
  Config = P->Config;
  Status = MtkUfsPlatformInitialize(P, &Io, &Config);
  if (Status == MtkUfsPlatformOk) Status = MtkUfsPlatformResetDevice(P);
  if (Status == MtkUfsPlatformOk) Status = MtkUfsPlatformSetRefClock(P, 1);
  if (Status == MtkUfsPlatformOk) Status = MtkUfsPlatformPreHce(P);
  return Status == MtkUfsPlatformOk ? Status : Fault(P, Status);
}

static MTK_UFS_PLATFORM_STATUS VerifyStoppedRoots(MTK_UFS_PLATFORM *P)
{
  static const uint32_t Stopped[] = { REG_HCE, 0x60U, 0x80U, REG_UTRLDBR,
    REG_UTMRLDBR, 0x50U, 0x54U, 0x70U, 0x74U, REG_IE };
  MTK_UFS_PLATFORM_STATUS Status;
  uint32_t Value;
  size_t I;

  for (I = 0; I < sizeof(Stopped) / sizeof(Stopped[0]); I++) {
    Status = Read(P, Stopped[I], &Value);
    if (Status != MtkUfsPlatformOk) return Status;
    if (Value == UINT32_MAX) return MtkUfsPlatformIoError;
    if (Value != 0) return MtkUfsPlatformBusy;
  }
  return MtkUfsPlatformOk;
}

static MTK_UFS_PLATFORM_STATUS VerifyRefClockOff(MTK_UFS_PLATFORM *P)
{
  uint32_t Value;
  MTK_UFS_PLATFORM_STATUS Status = Read(P, REG_REFCLK, &Value);
  if (Status != MtkUfsPlatformOk) return Status;
  if (Value == UINT32_MAX) return MtkUfsPlatformIoError;
  return (Value & 3U) == 0 ? MtkUfsPlatformOk : MtkUfsPlatformBusy;
}

MTK_UFS_PLATFORM_STATUS MtkUfsPlatformPrepareStop(MTK_UFS_PLATFORM *P)
{
  MTK_UFS_PLATFORM_STATUS Status, RecoveryStatus = MtkUfsPlatformOk;
  uint32_t Hce;
  int HibernateMayBeActive = 0;
  int PowerdownMayBeSet = 0;

  if (!Valid(P)) return MtkUfsPlatformInvalid;
  if (P->State != MtkUfsPlatformLinkReady || P->RefClock != MtkUfsRefClockOn ||
      P->SramResume != MtkUfsSramNoRestore) return MtkUfsPlatformWrongState;

  Status = Quiescent(P, 0);
  if (Status != MtkUfsPlatformOk) return Status;
  Status = Read(P, REG_HCE, &Hce);
  if (Status != MtkUfsPlatformOk) return Status;
  if (Hce == UINT32_MAX) return MtkUfsPlatformIoError;
  if ((Hce & 1U) == 0) return MtkUfsPlatformWrongState;

  /* The consumer has already completed device-WLUN POWERDOWN. Keep the host
   * clock and refclock available until HCE/list-root stop is proven. */
  HibernateMayBeActive = 1;
  Status = P->Io.Hibernate(P->Io.Context, 1);
  if (Status == MtkUfsPlatformOk) Status = WaitLinkState(P, UFS_LINK_HIBERN8, 15000U);
  if (Status == MtkUfsPlatformOk) {
    PowerdownMayBeSet = 1;
    Status = DmeSet(P, DME_POWERDOWN, 1U);
  }
  if (Status == MtkUfsPlatformOk) {
    P->State = MtkUfsPlatformStopPrepared;
    return MtkUfsPlatformOk;
  }

  /* If a UIC write failed after reaching the device, unwind link power while
   * the HCE and all clocks are still available. Preserve Fault if recovery is
   * incomplete so a later stop cannot claim success. */
  if (PowerdownMayBeSet) {
    RecoveryStatus = DmeSet(P, DME_POWERDOWN, 0U);
  }
  if (HibernateMayBeActive) {
    MTK_UFS_PLATFORM_STATUS ExitStatus = P->Io.Hibernate(P->Io.Context, 0);
    if (RecoveryStatus == MtkUfsPlatformOk && ExitStatus != MtkUfsPlatformOk) RecoveryStatus = ExitStatus;
  }
  if (HibernateMayBeActive) {
    MTK_UFS_PLATFORM_STATUS LinkStatus = WaitLinkState(P, UFS_LINK_UP, 100000U);
    if (RecoveryStatus == MtkUfsPlatformOk && LinkStatus != MtkUfsPlatformOk) RecoveryStatus = LinkStatus;
  }
  Recovery(P, RecoveryStatus);
  if (RecoveryStatus == MtkUfsPlatformOk) return Status;
  return Fault(P, Status);
}

MTK_UFS_PLATFORM_STATUS MtkUfsPlatformHostStopped(MTK_UFS_PLATFORM *P)
{
  MTK_UFS_PLATFORM_STATUS Status;

  if (!Valid(P)) return MtkUfsPlatformInvalid;
  if (P->SramResume != MtkUfsSramNoRestore) return MtkUfsPlatformWrongState;
  if (P->State == MtkUfsPlatformStopped) {
    Status = VerifyStoppedRoots(P);
    if (Status != MtkUfsPlatformOk) return Status;
    if (P->RefClock != MtkUfsRefClockOff) return MtkUfsPlatformIoError;
    return VerifyRefClockOff(P);
  }
  if (P->State != MtkUfsPlatformStopPrepared && P->State != MtkUfsPlatformHceStopped) {
    return MtkUfsPlatformWrongState;
  }

  Status = VerifyStoppedRoots(P);
  if (Status != MtkUfsPlatformOk) return Status;
  /* Save the independent HCE/root proof before refclock work, so a failed
   * clock transition can be retried without repeating HCE shutdown. */
  P->State = MtkUfsPlatformHceStopped;
  if (P->RefClock != MtkUfsRefClockOff) {
    Status = RefClockTransition(P, 0, 1);
    if (Status != MtkUfsPlatformOk) return Status;
  }
  if (P->RefClock != MtkUfsRefClockOff) return MtkUfsPlatformIoError;
  Status = VerifyStoppedRoots(P);
  if (Status != MtkUfsPlatformOk) return Status;
  Status = VerifyRefClockOff(P);
  if (Status != MtkUfsPlatformOk) return Status;
  P->State = MtkUfsPlatformStopped;
  return MtkUfsPlatformOk;
}

MTK_UFS_PLATFORM_STATUS MtkUfsPlatformPostHce(MTK_UFS_PLATFORM *P)
{
  MTK_UFS_PLATFORM_STATUS Status;
  uint32_t Value;
  if (!Valid(P)) return MtkUfsPlatformInvalid;
  if (P->State != MtkUfsPlatformHostPrepared) return MtkUfsPlatformWrongState;
  P->Io.DelayUs(P->Io.Context, MTK_UFS_HCE_DELAY_US);
  Status = Poll(P, REG_HCE, 1U, 1U, MTK_UFS_HOST_TIMEOUT_US);
  if (Status == MtkUfsPlatformOk) Status = Poll(P, REG_HCS, 8U, 8U, MTK_UFS_HOST_TIMEOUT_US);
  if (Status == MtkUfsPlatformOk && P->IpVersion == 0x10480000U) {
    Status = Read(P, REG_1048_OPTION, &Value);
    if (Status == MtkUfsPlatformOk && !(Value & 2U)) Status = MtkUfsPlatformIoError;
  }
  if (Status != MtkUfsPlatformOk) return Fault(P, Status);
  P->State = MtkUfsPlatformHostEnabled;
  return MtkUfsPlatformOk;
}

static MTK_UFS_PLATFORM_STATUS DmeSet(MTK_UFS_PLATFORM *P, uint16_t Attribute, uint32_t Value)
{
  return P->Io.Dme(P->Io.Context, 0, Attribute, &Value);
}

static MTK_UFS_PLATFORM_STATUS DmeModify(MTK_UFS_PLATFORM *P, uint16_t Attribute, uint32_t Clear, uint32_t Set)
{
  uint32_t Value;
  MTK_UFS_PLATFORM_STATUS Status = P->Io.Dme(P->Io.Context, 1, Attribute, &Value);
  if (Status != MtkUfsPlatformOk) return Status;
  return DmeSet(P, Attribute, (Value & ~Clear) | Set);
}

MTK_UFS_PLATFORM_STATUS MtkUfsPlatformPreLink(MTK_UFS_PLATFORM *P)
{
  MTK_UFS_PLATFORM_STATUS Status;
  if (!Valid(P)) return MtkUfsPlatformInvalid;
  if (P->State != MtkUfsPlatformHostEnabled || P->RefClock != MtkUfsRefClockOn) return MtkUfsPlatformWrongState;
  Status = DmeSet(P, DME_POWERDOWN, 0U);
  if (Status == MtkUfsPlatformOk) Status = DmeSet(P, 0x155EU, 0U);
  if (Status == MtkUfsPlatformOk) Status = DmeModify(P, DME_SAVEPOWER, 0x40U, 0U);
  if (Status == MtkUfsPlatformOk && P->IpVersion == 0x10450000U) Status = DmeModify(P, DME_LINK_TUNING, 0U, 0x10U);
  if (Status != MtkUfsPlatformOk) return Fault(P, Status);
  P->State = MtkUfsPlatformLinkPrepared;
  return MtkUfsPlatformOk;
}

MTK_UFS_PLATFORM_STATUS MtkUfsPlatformPostLink(MTK_UFS_PLATFORM *P)
{
  MTK_UFS_PLATFORM_STATUS Status;
  uint32_t Value;
  if (!Valid(P)) return MtkUfsPlatformInvalid;
  if (P->State != MtkUfsPlatformLinkPrepared) return MtkUfsPlatformWrongState;
  Status = Read(P, REG_HCS, &Value);
  if (Status == MtkUfsPlatformOk && (Value == UINT32_MAX || (Value & 9U) != 9U)) Status = MtkUfsPlatformIoError;
  if (Status == MtkUfsPlatformOk && P->IpVersion >= 0x10450100U) Status = DmeModify(P, DME_LINK_TUNING, 0U, 0x100U);
  if (Status == MtkUfsPlatformOk) Status = DmeModify(P, DME_SAVEPOWER, 0U, 0x0DU);
  if (Status == MtkUfsPlatformOk) Status = DmeModify(P, DME_DEBUGCLOCK, 0x20U, 0U);
  if (Status != MtkUfsPlatformOk) return Fault(P, Status);
  P->State = MtkUfsPlatformLinkReady;
  return MtkUfsPlatformOk;
}


static MTK_UFS_PLATFORM_STATUS SelectLinkProbe(MTK_UFS_PLATFORM *P)
{
  MTK_UFS_PLATFORM_STATUS Status;
  if (((P->IpVersion >> 16) & 0xFFU) >= 0x36U) {
    Status = Write(P, MtkUfsControllerBank, REG_DEBUG_SEL, 0x00820820U);
    if (Status == MtkUfsPlatformOk) Status = Write(P, MtkUfsControllerBank, REG_DEBUG_SEL_B0, 0U);
    if (Status == MtkUfsPlatformOk) Status = Write(P, MtkUfsControllerBank, REG_DEBUG_SEL_B1, 0x55555555U);
    if (Status == MtkUfsPlatformOk) Status = Write(P, MtkUfsControllerBank, REG_DEBUG_SEL_B2, 0xAAAAAAAAU);
    if (Status == MtkUfsPlatformOk) Status = Write(P, MtkUfsControllerBank, REG_DEBUG_SEL_B3, UINT32_MAX);
    return Status;
  }
  return Write(P, MtkUfsControllerBank, REG_DEBUG_SEL, 0x20U);
}

static MTK_UFS_PLATFORM_STATUS WaitLinkState(MTK_UFS_PLATFORM *P, uint32_t State, uint32_t TimeoutUs)
{
  MTK_UFS_PLATFORM_STATUS Status;
  uint32_t Elapsed, Value;
  for (Elapsed = 0; ; Elapsed += POLL_STEP_US) {
    Status = SelectLinkProbe(P);
    if (Status != MtkUfsPlatformOk) return Status;
    Status = Read(P, REG_UFS_PROBE, &Value);
    if (Status != MtkUfsPlatformOk) return Status;
    if (Value == UINT32_MAX) return MtkUfsPlatformIoError;
    if ((Value >> 28) == State) return MtkUfsPlatformOk;
    if (Elapsed >= TimeoutUs) return MtkUfsPlatformTimeout;
    P->Io.DelayUs(P->Io.Context, POLL_STEP_US);
  }
}

static MTK_UFS_PLATFORM_STATUS WaitLinkIdle(MTK_UFS_PLATFORM *P, uint32_t TimeoutUs)
{
  MTK_UFS_PLATFORM_STATUS Status;
  uint32_t Elapsed = 0, Value, State;
  int SawHibern8Transition = 0;
  P->Io.DelayUs(P->Io.Context, 10U);
  Elapsed = 10U;
  for (;;) {
    Status = SelectLinkProbe(P);
    if (Status != MtkUfsPlatformOk) return Status;
    Status = Read(P, REG_UFS_PROBE, &Value);
    if (Status != MtkUfsPlatformOk) return Status;
    if (Value == UINT32_MAX) return MtkUfsPlatformIoError;
    State = Value & 0x1FU;
    if (State >= UFS_HIB_ENTER && State <= UFS_HIB_EXIT) {
      SawHibern8Transition = 1;
      if (Elapsed >= TimeoutUs) return MtkUfsPlatformTimeout;
      P->Io.DelayUs(P->Io.Context, 50U);
      Elapsed += 50U;
      continue;
    }
    if (!SawHibern8Transition || State == UFS_HCE_BASE) return MtkUfsPlatformOk;
    if (Elapsed >= TimeoutUs) return MtkUfsPlatformTimeout;
    P->Io.DelayUs(P->Io.Context, 50U);
    Elapsed += 50U;
  }
}

static MTK_UFS_PLATFORM_STATUS ResumeLink(MTK_UFS_PLATFORM *P, MTK_UFS_SRAM_RESUME SramResume)
{
  MTK_UFS_PLATFORM_STATUS Status;
  int Completed;
  if (P->RefClock != MtkUfsRefClockOn) {
    Status = RefClockTransition(P, 1, 0);
    if (Status != MtkUfsPlatformOk) return Status;
  }
  Status = MtkUfsPlatformOk;
  if (SramResume != MtkUfsSramNoRestore) {
    Status = CallSmc(P, 32U, 1U, 0U, &Completed);
    if (Completed && P->LastSmcResult == -1 && P->Config.AllowSramRetention &&
        SramResume == MtkUfsSramRetained) Status = MtkUfsPlatformOk;
  }
  if (Status == MtkUfsPlatformOk) Status = DmeSet(P, DME_POWERDOWN, 0U);
  if (Status == MtkUfsPlatformOk) Status = P->Io.Hibernate(P->Io.Context, 0);
  if (Status == MtkUfsPlatformOk) Status = WaitLinkIdle(P, 5000U);
  if (Status == MtkUfsPlatformOk) Status = WaitLinkState(P, UFS_LINK_UP, 100000U);
  return Status;
}

MTK_UFS_PLATFORM_STATUS MtkUfsPlatformSuspend(MTK_UFS_PLATFORM *P)
{
  MTK_UFS_PLATFORM_STATUS Status, RecoveryStatus;
  uint32_t Hce;
  MTK_UFS_SRAM_RESUME SramResume = MtkUfsSramNoRestore;
  int Completed;
  if (!Valid(P)) return MtkUfsPlatformInvalid;
  if (P->State != MtkUfsPlatformLinkReady || P->RefClock != MtkUfsRefClockOn) return MtkUfsPlatformWrongState;
  Status = Quiescent(P, 0);
  if (Status != MtkUfsPlatformOk) return Status;
  Status = Read(P, REG_HCE, &Hce);
  if (Status != MtkUfsPlatformOk) return Fault(P, Status);
  if (Hce == UINT32_MAX) return Fault(P, MtkUfsPlatformIoError);
  if ((Hce & 1U) == 0) return MtkUfsPlatformWrongState;

  P->SramResume = MtkUfsSramNoRestore;

  /* Match the UFS core suspend order: enter Hibern8 before UniPro LPM,
   * SRAM power control, refclock gating and the caller's host-clock gating. */
  Status = P->Io.Hibernate(P->Io.Context, 1);
  if (Status == MtkUfsPlatformOk) Status = WaitLinkState(P, UFS_LINK_HIBERN8, 15000U);
  if (Status == MtkUfsPlatformOk) Status = DmeSet(P, DME_POWERDOWN, 1U);
  if (Status == MtkUfsPlatformOk) {
    SramResume = MtkUfsSramPowerOnRequired;
    Status = CallSmc(P, 32U, 0U, 0U, &Completed);
    /* Only a completed, exact T70 unsupported reply proves no SRAM change.
     * Keep the strict default; the board may explicitly retain VCC/domain power. */
    if (Completed && P->LastSmcResult == -1) {
      SramResume = P->Config.AllowSramRetention ? MtkUfsSramRetained : MtkUfsSramNoRestore;
      if (P->Config.AllowSramRetention) Status = MtkUfsPlatformOk;
    }
    P->SramResume = SramResume;
  }
  if (Status == MtkUfsPlatformOk) Status = RefClockTransition(P, 0, 0);
  if (Status == MtkUfsPlatformOk) {
    P->State = MtkUfsPlatformLinkSuspended;
    return MtkUfsPlatformOk;
  }

  RecoveryStatus = ResumeLink(P, SramResume);
  Recovery(P, RecoveryStatus);
  if (RecoveryStatus == MtkUfsPlatformOk) {
    P->SramResume = MtkUfsSramNoRestore;
    P->State = MtkUfsPlatformLinkReady;
  }
  else Fault(P, Status);
  return Status;
}

MTK_UFS_PLATFORM_STATUS MtkUfsPlatformResume(MTK_UFS_PLATFORM *P)
{
  MTK_UFS_PLATFORM_STATUS Status;
  uint32_t Hce;
  if (!Valid(P)) return MtkUfsPlatformInvalid;
  if (P->State != MtkUfsPlatformLinkSuspended || P->RefClock != MtkUfsRefClockOff) return MtkUfsPlatformWrongState;
  /* This path retains the host. Lost HCE or new I/O requires a different,
   * fully owned restart, not a successful-looking Hibern8 exit. */
  Status = Quiescent(P, 0);
  if (Status != MtkUfsPlatformOk) return Fault(P, Status);
  Status = Read(P, REG_HCE, &Hce);
  if (Status != MtkUfsPlatformOk) return Fault(P, Status);
  if (Hce == UINT32_MAX || (Hce & 1U) == 0) return Fault(P, MtkUfsPlatformIoError);
  Status = ResumeLink(P, P->SramResume);
  if (Status != MtkUfsPlatformOk) return Fault(P, Status);
  P->SramResume = MtkUfsSramNoRestore;
  P->State = MtkUfsPlatformLinkReady;
  return MtkUfsPlatformOk;
}
