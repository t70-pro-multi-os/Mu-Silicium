// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Library/MtkHusb311TypecCore.h>

#define TEST_MAX_UPDATES 32U

static unsigned Checks;

#define CHECK(Expression) do { \
  ++Checks; \
  if (!(Expression)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #Expression); \
    abort(); \
  } \
} while (0)

typedef struct {
  MTK_HUSB311_U8 Registers[256];
  MTK_HUSB311_TYPEC_GUARDS Guards;
  MTK_HUSB311_TYPEC_STATUS GuardStatus;
  MTK_HUSB311_U8 UpdateRegister[TEST_MAX_UPDATES];
  MTK_HUSB311_U8 UpdateMask[TEST_MAX_UPDATES];
  MTK_HUSB311_U8 UpdateValue[TEST_MAX_UPDATES];
  unsigned UpdateCount;
  MTK_HUSB311_U32 NowMs;
  MTK_HUSB311_U32 AttachAfterMs;
  MTK_HUSB311_U32 CcLookingUntilMs[2];
  MTK_HUSB311_U32 StaleOpenCcAfterMs;
  MTK_HUSB311_U32 TxAlertAfterMs;
  MTK_HUSB311_U32 DelayCalls;
  MTK_HUSB311_U32 FailDelayNumber;
  MTK_HUSB311_U32 ChangeRoleOnDelayNumber;
  MTK_HUSB311_U32 RoleReadCount;
  MTK_HUSB311_U32 MutateRoleReadNumber;
  MTK_HUSB311_U32 FailReadMatchNumber;
  MTK_HUSB311_U32 FailReadMatchCount;
  MTK_HUSB311_U8 FailReadEnabled;
  MTK_HUSB311_U8 FailReadTriggered;
  MTK_HUSB311_U8 FailReadRegister;
  MTK_HUSB311_U8 MutateRoleReadMask;
  MTK_HUSB311_U8 MutateRoleReadValue;
  MTK_HUSB311_U8 ChangeRoleOnDelayMask;
  MTK_HUSB311_U8 ChangeRoleOnDelayValue;
  MTK_HUSB311_U8 ToggleTxAlertEnabled;
  MTK_HUSB311_U8 ToggleTxAlertState;
  MTK_HUSB311_U32 ToggleTxAlertAfterMs;
  MTK_HUSB311_U8 AttachPin;
  MTK_HUSB311_U8 TxAlert;
  unsigned FailUpdateNumber;
  unsigned IgnoreUpdateNumber;
  unsigned IgnoreOpenWrites;
  unsigned PartialOpenWrites;
  unsigned DropCombinedSourcePulls;
  unsigned IgnoreSourceWrites;
  unsigned VbusOnUpdateNumber;
  unsigned VbusOnDelay;
  MTK_HUSB311_U8 ApplyFailedUpdate;
  MTK_HUSB311_U8 VendorAfterAck;
  MTK_HUSB311_U8 AlertAfterAck;
  MTK_HUSB311_U8 VbusOnAfterAck;
} FAKE_TCPC;

static MTK_HUSB311_TYPEC_STATUS
FakeReadRegisters(
  void *Context,
  MTK_HUSB311_U8 FirstRegister,
  MTK_HUSB311_U8 Length,
  MTK_HUSB311_U8 *Bytes
  )
{
  FAKE_TCPC *Fake;
  unsigned Index;
  MTK_HUSB311_U8 CcStatus;

  Fake = (FAKE_TCPC *)Context;
  CHECK(Fake != 0 && Bytes != 0 && Length != 0U);
  CHECK(((unsigned)FirstRegister + Length) <= sizeof(Fake->Registers));
  if ((unsigned)MTK_HUSB311_TYPEC_REG_ROLE_CONTROL >=
      (unsigned)FirstRegister &&
      (unsigned)MTK_HUSB311_TYPEC_REG_ROLE_CONTROL <
        ((unsigned)FirstRegister + (unsigned)Length)) {
    ++Fake->RoleReadCount;
    if (Fake->MutateRoleReadNumber == Fake->RoleReadCount) {
      Fake->Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] =
        (MTK_HUSB311_U8)(
          (Fake->Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] &
           (MTK_HUSB311_U8)~Fake->MutateRoleReadMask) |
          (Fake->MutateRoleReadValue & Fake->MutateRoleReadMask));
    }
  }
  if (Fake->FailReadEnabled && !Fake->FailReadTriggered &&
      ((unsigned)Fake->FailReadRegister >= (unsigned)FirstRegister) &&
      ((unsigned)Fake->FailReadRegister <
       ((unsigned)FirstRegister + (unsigned)Length))) {
    ++Fake->FailReadMatchCount;
    if ((Fake->FailReadMatchNumber == 0U) ||
        (Fake->FailReadMatchCount == Fake->FailReadMatchNumber)) {
      Fake->FailReadTriggered = 1U;
      return MtkHusb311TypecIoError;
    }
  }
  if ((FirstRegister == MTK_HUSB311_TYPEC_REG_CC_STATUS) && (Length == 1U)) {
    unsigned Open = (Fake->Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] &
        MTK_HUSB311_TYPEC_ROLE_CC_MASK) == MTK_HUSB311_TYPEC_ROLE_OPEN_FIELDS;
    if (Fake->NowMs < Fake->CcLookingUntilMs[Open]) {
      CcStatus = 0x20U;
    } else if (Open) {
      CcStatus = ((Fake->StaleOpenCcAfterMs != 0U) &&
                  (Fake->NowMs >= Fake->StaleOpenCcAfterMs)) ?
                 MTK_HUSB311_TYPEC_CC_RD : 0U;
    } else if ((Fake->AttachPin != MTK_HUSB311_TYPEC_CC_PIN_NONE) &&
               (Fake->NowMs >= Fake->AttachAfterMs)) {
      CcStatus = (Fake->AttachPin == MTK_HUSB311_TYPEC_CC_PIN_1) ?
        MTK_HUSB311_TYPEC_CC_RD :
        (MTK_HUSB311_U8)(MTK_HUSB311_TYPEC_CC_RD << 2);
    } else {
      CcStatus = 0U;
    }
    Bytes[0] = CcStatus;
    return MtkHusb311TypecSuccess;
  }
  for (Index = 0U; Index < Length; ++Index) {
    Bytes[Index] = Fake->Registers[(unsigned)FirstRegister + Index];
  }
  return MtkHusb311TypecSuccess;
}

static MTK_HUSB311_TYPEC_STATUS
FakeUpdateRegister(
  void *Context,
  MTK_HUSB311_U8 Register,
  MTK_HUSB311_U8 Mask,
  MTK_HUSB311_U8 Value
  )
{
  FAKE_TCPC *Fake;
  unsigned Index;

  Fake = (FAKE_TCPC *)Context;
  CHECK(Fake != 0);
  CHECK(Fake->UpdateCount < TEST_MAX_UPDATES);
  Index = Fake->UpdateCount++;
  Fake->UpdateRegister[Index] = Register;
  Fake->UpdateMask[Index] = Mask;
  Fake->UpdateValue[Index] = Value;
  if ((Fake->FailUpdateNumber == Fake->UpdateCount) &&
      !Fake->ApplyFailedUpdate) {
    return MtkHusb311TypecIoError;
  }
  if ((Fake->IgnoreUpdateNumber == Fake->UpdateCount) ||
      (Fake->IgnoreOpenWrites && (Register == 0x1aU) && (Value == 0x2fU))) {
    return MtkHusb311TypecSuccess;
  }
  if (Fake->PartialOpenWrites != 0U && Register == 0x1aU && Value == 0x2fU) {
    --Fake->PartialOpenWrites;
    Mask &= MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK;
  }
  if (Register == MTK_HUSB311_TYPEC_REG_ROLE_CONTROL &&
      Value == MTK_HUSB311_TYPEC_ROLE_SOURCE_RP_DFT) {
    if (Fake->IgnoreSourceWrites) {
      return MtkHusb311TypecSuccess;
    }
    const MTK_HUSB311_U8 Changed = (Fake->Registers[Register] ^ Value) & Mask;
    if (Fake->DropCombinedSourcePulls &&
        (Changed & MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK) &&
        (Changed & MTK_HUSB311_TYPEC_ROLE_CC_MASK)) {
      Mask &= MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK;
    }
  }
  if (Register == MTK_HUSB311_TYPEC_REG_VENDOR_ALERT) {
    CHECK(Mask == 2U && Value == 2U);
    Fake->Registers[Register] = (MTK_HUSB311_U8)(
      (Fake->Registers[Register] & (MTK_HUSB311_U8)~Value) | Fake->VendorAfterAck);
    Fake->Registers[MTK_HUSB311_TYPEC_REG_ALERT] |= Fake->AlertAfterAck;
    if (Fake->VbusOnAfterAck) {
      Fake->Registers[MTK_HUSB311_TYPEC_REG_POWER_STATUS] |= 4U;
      Fake->Registers[MTK_HUSB311_TYPEC_REG_VENDOR_STATUS] &= (MTK_HUSB311_U8)~2U;
      Fake->Guards.ScBoostEnabled = 1U;
      Fake->Guards.HusbVbusSafe0V = 0U;
    }
  } else {
    Fake->Registers[Register] = (MTK_HUSB311_U8)(
      (Fake->Registers[Register] & (MTK_HUSB311_U8)~Mask) | (Value & Mask));
  }
  if (Fake->VbusOnUpdateNumber == Fake->UpdateCount) {
    Fake->Guards.HusbVbusSafe0V = 0U;
    Fake->Registers[MTK_HUSB311_TYPEC_REG_POWER_STATUS] |= 4U;
    Fake->Registers[MTK_HUSB311_TYPEC_REG_VENDOR_STATUS] &= (MTK_HUSB311_U8)~2U;
  }
  if (Fake->FailUpdateNumber == Fake->UpdateCount) {
    return MtkHusb311TypecIoError;
  }
  return MtkHusb311TypecSuccess;
}

static MTK_HUSB311_TYPEC_STATUS
FakeDelayMs(
  void *Context,
  MTK_HUSB311_U32 Milliseconds
  )
{
  FAKE_TCPC *Fake;

  Fake = (FAKE_TCPC *)Context;
  CHECK(Fake != 0);
  ++Fake->DelayCalls;
  if (Fake->FailDelayNumber == Fake->DelayCalls) {
    return MtkHusb311TypecIoError;
  }
  Fake->NowMs += Milliseconds;
  if (Fake->VbusOnDelay == Fake->DelayCalls) {
    Fake->Guards.HusbVbusSafe0V = 0U;
    Fake->Registers[MTK_HUSB311_TYPEC_REG_POWER_STATUS] |= 4U;
    Fake->Registers[MTK_HUSB311_TYPEC_REG_VENDOR_STATUS] &= (MTK_HUSB311_U8)~2U;
  }
  if (Fake->ChangeRoleOnDelayNumber == Fake->DelayCalls) {
    Fake->Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] =
      (MTK_HUSB311_U8)(
        (Fake->Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] &
         (MTK_HUSB311_U8)~Fake->ChangeRoleOnDelayMask) |
        (Fake->ChangeRoleOnDelayValue & Fake->ChangeRoleOnDelayMask));
  }
  if (Fake->ToggleTxAlertEnabled &&
      (Fake->NowMs >= Fake->ToggleTxAlertAfterMs)) {
    Fake->ToggleTxAlertState = (MTK_HUSB311_U8)(
      Fake->ToggleTxAlertState == 0U ? 1U : 0U);
    Fake->Registers[MTK_HUSB311_TYPEC_REG_ALERT] =
      Fake->ToggleTxAlertState != 0U ? 0x10U : 0x20U;
  }
  if ((Fake->TxAlertAfterMs != 0U) &&
      (Fake->NowMs >= Fake->TxAlertAfterMs)) {
    Fake->Registers[MTK_HUSB311_TYPEC_REG_ALERT] =
      (MTK_HUSB311_U8)(Fake->Registers[MTK_HUSB311_TYPEC_REG_ALERT] |
                       Fake->TxAlert);
  }
  return MtkHusb311TypecSuccess;
}

static MTK_HUSB311_TYPEC_STATUS
FakeReadGuards(
  void *Context,
  MTK_HUSB311_TYPEC_GUARDS *Guards
  )
{
  FAKE_TCPC *Fake;

  Fake = (FAKE_TCPC *)Context;
  CHECK(Fake != 0 && Guards != 0);
  if (Fake->GuardStatus != MtkHusb311TypecSuccess) return Fake->GuardStatus;
  *Guards = Fake->Guards;
  return MtkHusb311TypecSuccess;
}

static void
InitializeFake(
  FAKE_TCPC *Fake,
  MTK_HUSB311_TYPEC_CORE *Core
  )
{
  MTK_HUSB311_TYPEC_IO Io;

  memset(Fake, 0, sizeof(*Fake));
  memset(Core, 0, sizeof(*Core));
  Fake->Registers[0x00U] = 0x99U;
  Fake->Registers[0x01U] = 0x2EU;
  Fake->Registers[0x02U] = 0x11U;
  Fake->Registers[0x03U] = 0x03U;
  Fake->Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] = 0x52U;
  Fake->Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] = 0x07U;
  Fake->Registers[MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER] = 0x06U;
  Fake->Registers[MTK_HUSB311_TYPEC_REG_POWER_STATUS] = 0x00U;
  Fake->Registers[MTK_HUSB311_TYPEC_REG_VENDOR_STATUS] = 0x02U;
  Fake->Guards.PowerBusLeaseHeld = 1U;
  Fake->Guards.TcpcInitialized = 1U;
  Fake->Guards.CcDetectionEnabled = 1U;
  Fake->Guards.HusbVbusSafe0V = 1U;
  Fake->Guards.MusbControllerIdle = 1U;
  Io.Context = Fake;
  Io.ReadRegisters = FakeReadRegisters;
  Io.UpdateRegister = FakeUpdateRegister;
  Io.DelayMs = FakeDelayMs;
  Io.ReadGuards = FakeReadGuards;
  CHECK(MtkHusb311TypecInitialize(Core, &Io) == MtkHusb311TypecSuccess);
}

static void
CheckUpdate(
  const FAKE_TCPC *Fake,
  unsigned Index,
  MTK_HUSB311_U8 Register
  )
{
  CHECK(Index < Fake->UpdateCount);
  CHECK(Fake->UpdateRegister[Index] == Register);
}

static void
CheckAcquireAndRelease(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;
  MTK_HUSB311_TOKEN Stale;

  InitializeFake(&Fake, &Core);
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_1;
  Fake.AttachAfterMs = 100U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecSuccess);
  CHECK(Token != 0U && MtkHusb311TypecTokenActive(&Core, Token));
  CHECK(Fake.NowMs >= (MTK_HUSB311_TYPEC_TX_QUIESCENCE_MS +
                       MTK_HUSB311_TYPEC_ATTACH_DEBOUNCE_MS));
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x05U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER] == 0x0FU);
  CHECK(Core.AttachedCcPin == MTK_HUSB311_TYPEC_CC_PIN_1);
  CHECK(Core.TxResultBaseline == 0U);
  CheckUpdate(&Fake, 0U, MTK_HUSB311_TYPEC_REG_RX_DETECT);
  CheckUpdate(&Fake, 1U, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL);
  CheckUpdate(&Fake, 2U, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL);
  CheckUpdate(&Fake, 3U, MTK_HUSB311_TYPEC_REG_ROLE_CONTROL);
  CheckUpdate(&Fake, 4U, MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER);

  Fake.Guards.ScBoostEnabled = 1U;
  Fake.Guards.HusbVbusSafe0V = 0U;
  Fake.Guards.MusbDevCtl = MTK_HUSB311_TYPEC_MUSB_SESSION_MASK;
  Fake.Guards.MusbControllerIdle = 0U;
  Fake.Registers[MTK_HUSB311_TYPEC_REG_POWER_STATUS] = 0x04U;
  CHECK(MtkHusb311TypecValidateSource(&Core, Token) ==
        MtkHusb311TypecSuccess);
  Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] |=
    MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK;
  CHECK(MtkHusb311TypecValidateSource(&Core, Token) ==
        MtkHusb311TypecConflict);
  Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] &=
    (MTK_HUSB311_U8)~MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK;
  Fake.Registers[MTK_HUSB311_TYPEC_REG_ALERT] = 0x20U;
  CHECK(MtkHusb311TypecValidateSource(&Core, Token) ==
        MtkHusb311TypecConflict);
  Fake.Registers[MTK_HUSB311_TYPEC_REG_ALERT] = 0U;

  Fake.Guards.ScBoostEnabled = 0U;
  Fake.Guards.HusbVbusSafe0V = 1U;
  Fake.Guards.MusbDevCtl = 0U;
  Fake.Guards.MusbControllerIdle = 1U;
  Fake.Registers[MTK_HUSB311_TYPEC_REG_POWER_STATUS] = 0x00U;
  Stale = Token + 1U;
  CHECK(MtkHusb311TypecReleaseSource(&Core, Stale) ==
        MtkHusb311TypecStaleToken);
  CHECK(MtkHusb311TypecReleaseSource(&Core, Token) ==
        MtkHusb311TypecSuccess);
  CHECK(!MtkHusb311TypecTokenActive(&Core, Token));
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER] == 0x06U);
  CHECK(Fake.UpdateRegister[5U] == MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER);
  CHECK(Fake.UpdateRegister[6U] == MTK_HUSB311_TYPEC_REG_ROLE_CONTROL);
  CHECK(Fake.UpdateRegister[7U] == MTK_HUSB311_TYPEC_REG_ROLE_CONTROL);
  CHECK(Fake.UpdateRegister[8U] == MTK_HUSB311_TYPEC_REG_RX_DETECT);
}

static void
CheckRejectsUnsafeInputs(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;
  unsigned Case;

  for (Case = 0U; Case < 6U; ++Case) {
    InitializeFake(&Fake, &Core);
    if (Case == 0U) Fake.Guards.PowerBusLeaseHeld = 0U;
    if (Case == 1U) Fake.Guards.TcpcInitialized = 0U;
    if (Case == 2U) Fake.Guards.CcDetectionEnabled = 0U;
    if (Case == 3U) Fake.Guards.ScBoostEnabled = 1U;
    if (Case == 4U) Fake.Guards.MusbDevCtl =
                      MTK_HUSB311_TYPEC_MUSB_SESSION_MASK;
    if (Case == 5U) Fake.Guards.MusbControllerIdle = 0U;
    CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
          MtkHusb311TypecNotReady);
    CHECK(Fake.UpdateCount == 0U);
    CHECK(Core.State == MtkHusb311TypecStateIdle);
  }

  for (Case = 0U; Case < 8U; ++Case) {
    InitializeFake(&Fake, &Core);
    if (Case == 0U) Fake.Guards.HusbVbusSafe0V = 0U;
    if (Case == 1U) Fake.Registers[0x00U] = 0U;
    if (Case == 2U) Fake.Registers[MTK_HUSB311_TYPEC_REG_TCPC_CONTROL] =
                      MTK_HUSB311_TYPEC_TCPC_BIST_MASK;
    if (Case == 3U) Fake.Registers[MTK_HUSB311_TYPEC_REG_POWER_CONTROL] =
                      MTK_HUSB311_TYPEC_POWER_VCONN_MASK;
    if (Case == 4U) Fake.Registers[MTK_HUSB311_TYPEC_REG_FAULT_STATUS] = 1U;
    if (Case == 5U) Fake.Registers[MTK_HUSB311_TYPEC_REG_ALERT] = 0x04U;
    if (Case == 6U) Fake.Registers[MTK_HUSB311_TYPEC_REG_VENDOR_ALERT] = 1U;
    if (Case == 7U) Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] =
                      0xD2U;
    CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) !=
          MtkHusb311TypecSuccess);
    CHECK(Fake.UpdateCount == 0U);
    CHECK(Core.State == MtkHusb311TypecStateIdle);
  }
}

static void
CheckTxCompletionQuarantineAndRecovery(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;

  InitializeFake(&Fake, &Core);
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_1;
  Fake.AttachAfterMs = 100U;
  Fake.TxAlertAfterMs = 10U;
  Fake.TxAlert = 0x10U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecNotReady);
  CHECK(Token == 0U);
  CHECK(Core.State == MtkHusb311TypecStateIdle);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);
}

static void
CheckNoAttachAndReverseCleanup(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;

  InitializeFake(&Fake, &Core);
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecTimeout);
  CHECK(Token == 0U);
  CHECK(Core.State == MtkHusb311TypecStateIdle);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);
  CHECK(Fake.UpdateRegister[0U] == MTK_HUSB311_TYPEC_REG_RX_DETECT);
  CHECK(Fake.UpdateRegister[1U] == MTK_HUSB311_TYPEC_REG_ROLE_CONTROL);
  CHECK(Fake.UpdateRegister[2U] == MTK_HUSB311_TYPEC_REG_ROLE_CONTROL);
  CHECK(Fake.UpdateRegister[3U] == MTK_HUSB311_TYPEC_REG_ROLE_CONTROL);
  CHECK(Fake.UpdateRegister[4U] == MTK_HUSB311_TYPEC_REG_ROLE_CONTROL);
  CHECK(Fake.UpdateRegister[5U] == MTK_HUSB311_TYPEC_REG_ROLE_CONTROL);
  CHECK(Fake.UpdateRegister[6U] == MTK_HUSB311_TYPEC_REG_RX_DETECT);
}

static void
CheckWriteFailureSideEffects(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;

  InitializeFake(&Fake, &Core);
  Fake.FailUpdateNumber = 1U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecIoError);
  CHECK(Core.State == MtkHusb311TypecStateIdle);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);

  InitializeFake(&Fake, &Core);
  Fake.FailUpdateNumber = 1U;
  Fake.ApplyFailedUpdate = 1U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecIoError);
  CHECK(Core.State == MtkHusb311TypecStateIdle);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);

  InitializeFake(&Fake, &Core);
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_2;
  Fake.AttachAfterMs = 100U;
  Fake.FailUpdateNumber = 5U;
  Fake.ApplyFailedUpdate = 1U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecIoError);
  CHECK(Core.State == MtkHusb311TypecStateIdle);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER] == 0x06U);
}

static void
CheckReadAndDelayFailures(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;

  InitializeFake(&Fake, &Core);
  Fake.FailReadEnabled = 1U;
  Fake.FailReadRegister = MTK_HUSB311_TYPEC_REG_CC_STATUS;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecIoError);
  CHECK(Fake.FailReadTriggered != 0U);
  CHECK(Core.State == MtkHusb311TypecStateIdle);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);

  InitializeFake(&Fake, &Core);
  Fake.FailDelayNumber = 1U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecIoError);
  CHECK(Fake.DelayCalls > 1U);
  CHECK(Core.State == MtkHusb311TypecStateIdle);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);
}

static void
CheckChangingTxResultHasBoundedQuarantine(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;

  InitializeFake(&Fake, &Core);
  Fake.ToggleTxAlertEnabled = 1U;
  Fake.ToggleTxAlertAfterMs = MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecQuarantined);
  CHECK(Token == 0U);
  CHECK(Core.State == MtkHusb311TypecStateQuarantined);
  CHECK(Core.LastError == MtkHusb311TypecTimeout);
  CHECK(Core.OwnedFields != 0U);
  CHECK(Fake.NowMs >= MTK_HUSB311_TYPEC_TX_QUIESCENCE_TIMEOUT_MS);
  CHECK(Fake.NowMs <= (MTK_HUSB311_TYPEC_TX_QUIESCENCE_TIMEOUT_MS +
                       2U * MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS));
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0U);
  CHECK((Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] &
         (MTK_HUSB311_TYPEC_ROLE_DRP_MASK |
          MTK_HUSB311_TYPEC_ROLE_CC_MASK)) ==
        MTK_HUSB311_TYPEC_ROLE_OPEN_FIELDS);
}

static void
CheckRoleDriftDuringOpenWait(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;

  InitializeFake(&Fake, &Core);
  Fake.ChangeRoleOnDelayNumber = 1U;
  Fake.ChangeRoleOnDelayMask = MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK;
  Fake.ChangeRoleOnDelayValue = MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecQuarantined);
  CHECK(Token == 0U);
  CHECK(Core.State == MtkHusb311TypecStateQuarantined);
  CHECK(Core.ConflictActive != 0U);
  CHECK((Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] &
         MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK) != 0U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0U);

  Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] &=
    (MTK_HUSB311_U8)~MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK;
  CHECK(MtkHusb311TypecRetryCleanup(&Core) == MtkHusb311TypecSuccess);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U);
}

static void
CheckRoleDriftAfterOpenWait(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;
  MTK_HUSB311_U8 ExpectedOpenRole;

  InitializeFake(&Fake, &Core);
  Fake.MutateRoleReadNumber = 24U;
  Fake.MutateRoleReadMask = MTK_HUSB311_TYPEC_ROLE_DRP_MASK;
  Fake.MutateRoleReadValue = MTK_HUSB311_TYPEC_ROLE_DRP_MASK;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecQuarantined);
  CHECK(Token == 0U);
  CHECK(Core.State == MtkHusb311TypecStateQuarantined);
  CHECK(Core.ConflictActive != 0U);
  CHECK(Fake.UpdateCount == 2U);
  CHECK((Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] &
         MTK_HUSB311_TYPEC_ROLE_DRP_MASK) != 0U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0U);

  ExpectedOpenRole = Core.ExpectedRoleControl;
  Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] = ExpectedOpenRole;
  CHECK(MtkHusb311TypecRetryCleanup(&Core) == MtkHusb311TypecSuccess);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U);
}

static void
CheckApplyUpdateReadbackAndOwnership(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;
  MTK_HUSB311_U8 ExpectedRole;
  unsigned BeforeUpdates;

  InitializeFake(&Fake, &Core);
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_1;
  Fake.AttachAfterMs = 100U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecSuccess);
  ExpectedRole = Core.ExpectedRoleControl;
  BeforeUpdates = Fake.UpdateCount;
  Fake.MutateRoleReadNumber = Fake.RoleReadCount + 3U;
  Fake.MutateRoleReadMask = MTK_HUSB311_TYPEC_ROLE_CC_MASK;
  Fake.MutateRoleReadValue = (MTK_HUSB311_U8)(ExpectedRole ^ 0x01U);
  CHECK(MtkHusb311TypecReleaseSource(&Core, Token) ==
        MtkHusb311TypecQuarantined);
  CHECK(Core.ConflictActive != 0U);
  CHECK(Core.ConflictWriteAttempted == 0U);
  CHECK(Core.ExpectedRoleControl == ExpectedRole);
  CHECK(Fake.UpdateCount == BeforeUpdates + 1U);
  CHECK(Fake.UpdateRegister[BeforeUpdates] ==
        MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] != ExpectedRole);

  Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] = ExpectedRole;
  CHECK(MtkHusb311TypecRetryCleanup(&Core) == MtkHusb311TypecSuccess);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);

  InitializeFake(&Fake, &Core);
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_1;
  Fake.AttachAfterMs = 100U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecSuccess);
  ExpectedRole = Core.ExpectedRoleControl;
  BeforeUpdates = Fake.UpdateCount;
  Fake.FailReadEnabled = 1U;
  Fake.FailReadRegister = MTK_HUSB311_TYPEC_REG_ROLE_CONTROL;
  Fake.FailReadMatchNumber = 3U;
  CHECK(MtkHusb311TypecReleaseSource(&Core, Token) ==
        MtkHusb311TypecQuarantined);
  CHECK(Fake.FailReadTriggered != 0U);
  CHECK(Core.ExpectedRoleControl == ExpectedRole);
  CHECK(Fake.UpdateCount == BeforeUpdates + 1U);
  CHECK(Fake.UpdateRegister[BeforeUpdates] ==
        MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER);
  CHECK(MtkHusb311TypecRetryCleanup(&Core) == MtkHusb311TypecSuccess);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);

  InitializeFake(&Fake, &Core);
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_2;
  Fake.AttachAfterMs = 100U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecSuccess);
  ExpectedRole = Core.ExpectedRoleControl;
  BeforeUpdates = Fake.UpdateCount;
  Fake.IgnoreUpdateNumber = BeforeUpdates + 1U;
  CHECK(MtkHusb311TypecReleaseSource(&Core, Token) ==
        MtkHusb311TypecQuarantined);
  CHECK(Core.State == MtkHusb311TypecStateQuarantined);
  CHECK((Core.OwnedFields & MTK_HUSB311_TYPEC_TOKEN_FIELD_HEADER) != 0U);
  CHECK(Core.ExpectedRoleControl == ExpectedRole);
  CHECK(Core.ExpectedMessageHeader == 0x0FU);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER] == 0x0FU);
  Fake.IgnoreUpdateNumber = 0U;
  CHECK(MtkHusb311TypecRetryCleanup(&Core) == MtkHusb311TypecSuccess);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER] == 0x06U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);

  InitializeFake(&Fake, &Core);
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_2;
  Fake.AttachAfterMs = 100U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecSuccess);
  BeforeUpdates = Fake.UpdateCount;
  Fake.FailUpdateNumber = BeforeUpdates + 1U;
  Fake.ApplyFailedUpdate = 1U;
  CHECK(MtkHusb311TypecReleaseSource(&Core, Token) ==
        MtkHusb311TypecIoError);
  CHECK(Core.State == MtkHusb311TypecStateIdle);
  CHECK(Core.OwnedFields == 0U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER] == 0x06U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U);
}

static void
CheckReservedRoleDriftStaysQuarantined(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;
  unsigned BeforeUpdates;

  InitializeFake(&Fake, &Core);
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_1;
  Fake.AttachAfterMs = 100U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecSuccess);
  BeforeUpdates = Fake.UpdateCount;
  Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] |=
    MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK;
  CHECK(MtkHusb311TypecReleaseSource(&Core, Token) ==
        MtkHusb311TypecQuarantined);
  CHECK(Core.State == MtkHusb311TypecStateQuarantined);
  CHECK(Core.ConflictActive != 0U);
  CHECK(Core.OwnedFields != 0U);
  CHECK(Fake.UpdateCount == BeforeUpdates);
  CHECK((Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] &
         MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK) != 0U);

  Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] &=
    (MTK_HUSB311_U8)~MTK_HUSB311_TYPEC_ROLE_RESERVED_MASK;
  CHECK(MtkHusb311TypecRetryCleanup(&Core) == MtkHusb311TypecSuccess);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);
}

static void
CheckDriftDoesNotGetOverwritten(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;
  unsigned BeforeDriftUpdates;

  InitializeFake(&Fake, &Core);
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_1;
  Fake.AttachAfterMs = 100U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
        MtkHusb311TypecSuccess);
  BeforeDriftUpdates = Fake.UpdateCount;
  Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] ^= 0x01U;
  CHECK(MtkHusb311TypecReleaseSource(&Core, Token) ==
        MtkHusb311TypecQuarantined);
  CHECK(Core.State == MtkHusb311TypecStateQuarantined);
  CHECK((Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] & 0x01U) == 0U);
  CHECK(Fake.UpdateCount == BeforeDriftUpdates + 1U);
  CHECK(Fake.UpdateRegister[BeforeDriftUpdates] ==
        MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER);

  Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] =
    Core.ExpectedRoleControl;
  CHECK(MtkHusb311TypecRetryCleanup(&Core) == MtkHusb311TypecSuccess);
  CHECK(Core.State == MtkHusb311TypecStateIdle);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U);
}

static void
CheckCleanupRetryAndBaselineTxAlert(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;
  static const MTK_HUSB311_U8 TxAlerts[] = { 0x10U, 0x20U, 0x40U };
  unsigned Index;

  for (Index = 0U; Index < sizeof(TxAlerts) / sizeof(TxAlerts[0]); ++Index) {
    InitializeFake(&Fake, &Core);
    Fake.Registers[MTK_HUSB311_TYPEC_REG_ALERT] = TxAlerts[Index];
    Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_2;
    Fake.AttachAfterMs = 100U;
    CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) ==
          MtkHusb311TypecSuccess);
    CHECK(Core.TxResultBaseline == TxAlerts[Index]);
    CHECK(Core.AttachedCcPin == MTK_HUSB311_TYPEC_CC_PIN_2);

    Fake.FailUpdateNumber = Fake.UpdateCount + 1U;
    CHECK(MtkHusb311TypecReleaseSource(&Core, Token) ==
          MtkHusb311TypecQuarantined);
    CHECK(Core.State == MtkHusb311TypecStateQuarantined);
    Fake.FailUpdateNumber = 0U;
    CHECK(MtkHusb311TypecRetryCleanup(&Core) == MtkHusb311TypecSuccess);
    CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U);
    CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U);
    CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER] == 0x06U);
  }
}


static void
CheckFirstFailureSurvivesCleanup(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;
  MTK_HUSB311_TYPEC_DIAGNOSTIC First;
  unsigned Case;

  /* One failed safety condition must be distinguishable without TCPC writes. */
  for (Case = 0U; Case < 7U; ++Case) {
    InitializeFake(&Fake, &Core);
    if (Case == 0U) Fake.Guards.PowerBusLeaseHeld = 0U;
    if (Case == 1U) Fake.Guards.TcpcInitialized = 0U;
    if (Case == 2U) Fake.Guards.CcDetectionEnabled = 0U;
    if (Case == 3U) Fake.Guards.ScBoostEnabled = 1U;
    if (Case == 4U) Fake.Guards.HusbVbusSafe0V = 0U;
    if (Case == 5U) Fake.Guards.MusbDevCtl = 1U;
    if (Case == 6U) Fake.Guards.MusbControllerIdle = 0U;
    CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) == MtkHusb311TypecNotReady);
    CHECK(Token == 0U && Fake.UpdateCount == 0U);
    CHECK(Core.FirstFailure.Step == 1U && Core.FirstFailure.Length == 0U);
    CHECK(Core.FirstFailure.Status == MtkHusb311TypecNotReady);
    CHECK(Core.FirstFailure.GuardFlags == (MTK_HUSB311_U8)(0x7fU & ~(1U << Case)));
  }
  InitializeFake(&Fake, &Core);
  Fake.GuardStatus = MtkHusb311TypecIoError;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) == MtkHusb311TypecIoError);
  CHECK(Core.FirstFailure.Step == 1U && Core.FirstFailure.GuardFlags == 0xffU &&
        Core.FirstFailure.Length == 0U && Fake.UpdateCount == 0U);

  /* ALERT is 16-bit and a later vendor read must not hide the rejected bit. */
  static const struct {
    MTK_HUSB311_U8 WriteRegister, WriteValue, Step, Register, Length;
    MTK_HUSB311_U16 Value;
  } Rejected[] = {
    { 0x00U, 0x98U, 2U, 0x00U, 2U, 0x2e98U },
    { 0x02U, 0x10U, 2U, 0x02U, 2U, 0x0310U },
    { 0x1eU, 0x04U, 3U, 0x1eU, 1U, 0x0004U },
    { 0x97U, 0x00U, 3U, 0x97U, 1U, 0x0000U },
    { 0x19U, 0x02U, 4U, 0x19U, 1U, 0x0002U },
    { 0x1cU, 0x01U, 4U, 0x1cU, 1U, 0x0001U },
    { 0x1fU, 0x01U, 5U, 0x1fU, 1U, 0x0001U },
    { 0x10U, 0x04U, 6U, 0x10U, 2U, 0x0004U },
    { 0x11U, 0x80U, 6U, 0x10U, 2U, 0x8000U },
    { 0x98U, 0x01U, 6U, 0x98U, 1U, 0x0001U },
    { 0x1aU, 0xd2U, 7U, 0x1aU, 1U, 0x00d2U }
  };
  for (Case = 0U; Case < sizeof(Rejected) / sizeof(Rejected[0]); ++Case) {
    InitializeFake(&Fake, &Core);
    Fake.Registers[Rejected[Case].WriteRegister] = Rejected[Case].WriteValue;
    CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) == MtkHusb311TypecNotReady);
    CHECK(Core.State == MtkHusb311TypecStateIdle && Core.OwnedFields == 0U &&
          Fake.UpdateCount == 0U);
    CHECK(Core.FirstFailure.Step == Rejected[Case].Step &&
          Core.FirstFailure.Register == Rejected[Case].Register &&
          Core.FirstFailure.Length == Rejected[Case].Length &&
          Core.FirstFailure.Value == Rejected[Case].Value &&
          Core.FirstFailure.ReadStatus == MtkHusb311TypecSuccess &&
          Core.FirstFailure.Status == MtkHusb311TypecNotReady);
    First = Core.FirstFailure;
    CHECK(MtkHusb311TypecRetryCleanup(&Core) == MtkHusb311TypecNotReady);
    CHECK(memcmp(&First, &Core.FirstFailure, sizeof(First)) == 0);
  }

  /* A bus read failure is not a valid register value, even after rollback. */
  static const struct { MTK_HUSB311_U8 Register, Step; } ReadFailures[] = {
    { 0x00U, 2U }, { 0x02U, 2U }, { 0x1eU, 3U }, { 0x97U, 3U },
    { 0x19U, 4U }, { 0x1cU, 4U }, { 0x1fU, 5U }, { 0x10U, 6U },
    { 0x98U, 6U }, { 0x1aU, 7U }, { 0x2fU, 7U }, { 0x2eU, 7U },
    { 0x1dU, 12U }
  };
  for (Case = 0U; Case < sizeof(ReadFailures) / sizeof(ReadFailures[0]); ++Case) {
    InitializeFake(&Fake, &Core);
    Fake.FailReadEnabled = 1U;
    Fake.FailReadRegister = ReadFailures[Case].Register;
    CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) == MtkHusb311TypecIoError);
    CHECK(Fake.FailReadTriggered && Core.State == MtkHusb311TypecStateIdle);
    CHECK(Core.FirstFailure.Step == ReadFailures[Case].Step &&
          Core.FirstFailure.Register == ReadFailures[Case].Register &&
          Core.FirstFailure.Value == 0xffffU &&
          Core.FirstFailure.ReadStatus == MtkHusb311TypecIoError &&
          Core.FirstFailure.Status == MtkHusb311TypecIoError);
  }

  /* Cleanup and later acquires must never replace the first physical cause. */
  InitializeFake(&Fake, &Core);
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) == MtkHusb311TypecTimeout);
  CHECK(Core.FirstFailure.Step == 12U && Core.FirstFailure.Status == MtkHusb311TypecTimeout);
  CHECK(Core.State == MtkHusb311TypecStateIdle && Core.OwnedFields == 0U);
  CHECK(Fake.Registers[0x1aU] == 0x52U && Fake.Registers[0x2fU] == 7U);
  First = Core.FirstFailure;
  Fake.Registers[0x98U] = 1U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) == MtkHusb311TypecNotReady);
  CHECK(memcmp(&First, &Core.FirstFailure, sizeof(First)) == 0);
  Fake.Registers[0x98U] = 0U;
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_1;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) == MtkHusb311TypecSuccess);
  CHECK(MtkHusb311TypecReleaseSource(&Core, Token) == MtkHusb311TypecSuccess);
  CHECK(memcmp(&First, &Core.FirstFailure, sizeof(First)) == 0);

  /* A cleanup timeout must not overwrite the original TX-change rejection. */
  InitializeFake(&Fake, &Core);
  Fake.ToggleTxAlertEnabled = 1U;
  Fake.ToggleTxAlertAfterMs = MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) == MtkHusb311TypecQuarantined);
  CHECK(Core.FirstFailure.Step == 10U && Core.FirstFailure.Status == MtkHusb311TypecNotReady);
  First = Core.FirstFailure;
  Fake.ToggleTxAlertEnabled = 0U;
  Fake.Registers[0x10U] = (MTK_HUSB311_U8)Core.TxResultBaseline;
  CHECK(MtkHusb311TypecRetryCleanup(&Core) == MtkHusb311TypecSuccess);
  CHECK(memcmp(&First, &Core.FirstFailure, sizeof(First)) == 0);

  InitializeFake(&Fake, &Core);
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_1;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) == MtkHusb311TypecSuccess);
  CHECK(Core.FirstFailure.Step == 0U);
  CHECK(MtkHusb311TypecReleaseSource(&Core, Token) == MtkHusb311TypecSuccess);
}

static void
CheckVbusEventAck(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;
  unsigned Case;

  InitializeFake(&Fake, &Core);
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_1;
  Fake.Registers[0x98] = 2U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) == MtkHusb311TypecSuccess);
  CHECK(Token != 0U && Fake.Registers[0x98] == 0U && Core.FirstFailure.Step == 0U);
  CheckUpdate(&Fake, 0U, 0x98U);
  CHECK(Fake.UpdateMask[0] == 2U && Fake.UpdateValue[0] == 2U);
  /* Active-source validation may see an old event; it must not require safe0V. */
  Fake.Guards.ScBoostEnabled = 1U; Fake.Guards.HusbVbusSafe0V = 0U;
  Fake.Guards.MusbDevCtl = 1U; Fake.Guards.MusbControllerIdle = 0U;
  Fake.Registers[0x1e] = 4U; Fake.Registers[0x97] = 0U;
  Fake.Registers[0x98] = 2U;
  CHECK(MtkHusb311TypecValidateSource(&Core, Token) == MtkHusb311TypecSuccess);
  CHECK(Fake.Registers[0x98] == 0U);
  Fake.Guards.ScBoostEnabled = 0U; Fake.Guards.HusbVbusSafe0V = 1U;
  Fake.Guards.MusbDevCtl = 0U; Fake.Guards.MusbControllerIdle = 1U;
  Fake.Registers[0x1e] = 0U; Fake.Registers[0x97] = 2U;
  CHECK(MtkHusb311TypecReleaseSource(&Core, Token) == MtkHusb311TypecSuccess);

  for (Case = 0U; Case < 3U; ++Case) {
    InitializeFake(&Fake, &Core); Fake.Registers[0x98] = Case == 0U ? 1U : Case == 1U ? 3U : 0x80U;
    CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) == MtkHusb311TypecNotReady);
    CHECK(Token == 0U && Fake.UpdateCount == 0U);
  }
  InitializeFake(&Fake, &Core); Fake.Registers[0x98] = 2U; Fake.Registers[0x10] = 4U;
  CHECK(MtkHusb311TypecAcquireSource(&Core, &Token) == MtkHusb311TypecNotReady);
  CHECK(Fake.UpdateCount == 0U && Core.FirstFailure.Register == 0x10U);

  for (Case = 0U; Case < 7U; ++Case) {
    InitializeFake(&Fake, &Core); Fake.AttachPin = 1U; Fake.Registers[0x98] = 2U;
    if (Case == 0U) Fake.IgnoreUpdateNumber = 1U;
    if (Case == 1U) Fake.FailUpdateNumber = 1U;
    if (Case == 2U) {
      Fake.FailReadEnabled = 1U; Fake.FailReadRegister = 0x98U; Fake.FailReadMatchNumber = 2U;
    }
    if (Case == 3U) Fake.VendorAfterAck = 1U;
    if (Case == 4U) Fake.AlertAfterAck = 4U;
    if (Case == 5U) Fake.AlertAfterAck = 0x10U;
    if (Case == 6U) Fake.VbusOnAfterAck = 1U;
    MTK_HUSB311_TYPEC_STATUS Result = MtkHusb311TypecAcquireSource(&Core, &Token);
    if (Result != ((Case == 1U || Case == 2U) ? MtkHusb311TypecIoError : MtkHusb311TypecNotReady))
      fprintf(stderr, "VBUS ack case=%u status=%u step=%u owned=%u conflict=%u updates=%u\n",
              Case, Result, Core.FirstFailure.Step, Core.OwnedFields, Core.ConflictActive, Fake.UpdateCount);
    CHECK(Result == ((Case == 1U || Case == 2U) ? MtkHusb311TypecIoError : MtkHusb311TypecNotReady));
    CHECK(Token == 0U && Fake.UpdateCount == 1U && Core.OwnedFields == 0U &&
          Core.State == MtkHusb311TypecStateIdle);
    CheckUpdate(&Fake, 0U, 0x98U);
    if (Case == 0U || Case == 3U) CHECK(Core.FirstFailure.Register == 0x98U);
    if (Case == 4U || Case == 5U) CHECK(Core.FirstFailure.Register == 0x10U);
    if (Case != 6U) CHECK(Core.FirstFailure.Step == 6U);
  }
}

static void
CheckTimerDrivenProbeDoesNotDelay(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;
  MTK_HUSB311_TYPEC_STATUS Status;
  unsigned Ticks;
  unsigned Limit = (MTK_HUSB311_TYPEC_ATTACH_TIMEOUT_MS +
                    MTK_HUSB311_TYPEC_TX_QUIESCENCE_TIMEOUT_MS) /
                   MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS + 32U;

  InitializeFake(&Fake, &Core);
  CHECK(MtkHusb311TypecBeginSourceProbe(&Core) == MtkHusb311TypecPending);
  CHECK(Fake.DelayCalls == 0U);
  Token = 0U;
  for (Ticks = 0U; Ticks < Limit; ++Ticks) {
    Fake.NowMs += MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS;
    Status = MtkHusb311TypecPollSourceProbe(&Core, &Token);
    CHECK(Fake.DelayCalls == 0U);
    if (Status != MtkHusb311TypecPending) break;
  }
  CHECK(Ticks < Limit && Status == MtkHusb311TypecTimeout && Token == 0U &&
        Core.State == MtkHusb311TypecStateIdle && Core.OwnedFields == 0U);
  CHECK(Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U &&
        Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U &&
        Fake.Registers[MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER] == 0x06U);

  InitializeFake(&Fake, &Core);
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_1;
  Fake.AttachAfterMs = 40U;
  CHECK(MtkHusb311TypecBeginSourceProbe(&Core) == MtkHusb311TypecPending);
  CHECK(Fake.DelayCalls == 0U);
  Token = 0U;
  for (Ticks = 0U; Ticks < Limit; ++Ticks) {
    Fake.NowMs += MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS;
    Status = MtkHusb311TypecPollSourceProbe(&Core, &Token);
    CHECK(Fake.DelayCalls == 0U);
    if (Status != MtkHusb311TypecPending) break;
  }
  CHECK(Ticks < Limit && Status == MtkHusb311TypecSuccess && Token != 0U &&
        Core.AttachedCcPin == MTK_HUSB311_TYPEC_CC_PIN_1);
  Status = MtkHusb311TypecStepCleanup(&Core, Token);
  for (Ticks = 0U; Status == MtkHusb311TypecPending && Ticks < Limit; ++Ticks) {
    Fake.NowMs += MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS;
    Status = MtkHusb311TypecStepCleanup(&Core, Token);
    CHECK(Fake.DelayCalls == 0U);
  }
  CHECK(Ticks < Limit && Status == MtkHusb311TypecSuccess &&
        Core.State == MtkHusb311TypecStateIdle && Core.OwnedFields == 0U);
  CHECK(Fake.DelayCalls == 0U &&
        Fake.Registers[MTK_HUSB311_TYPEC_REG_ROLE_CONTROL] == 0x52U &&
        Fake.Registers[MTK_HUSB311_TYPEC_REG_RX_DETECT] == 0x07U &&
        Fake.Registers[MTK_HUSB311_TYPEC_REG_MESSAGE_HEADER] == 0x06U);
}

static void
CheckBoundedSourceProbe(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;
  InitializeFake(&Fake, &Core);
  CHECK(MtkHusb311TypecProbeSource(&Core, &Token) == MtkHusb311TypecTimeout);
  CHECK(Fake.NowMs < 500U && Token == 0U && Core.OwnedFields == 0U &&
        Core.State == MtkHusb311TypecStateIdle);
  CHECK(Fake.Registers[0x1aU] == 0x52U && Fake.Registers[0x2fU] == 7U &&
        Fake.Registers[0x2eU] == 6U);
  for (unsigned Pin = 1U; Pin <= 2U; ++Pin) {
    InitializeFake(&Fake, &Core); Fake.AttachPin = (MTK_HUSB311_U8)Pin;
    CHECK(MtkHusb311TypecProbeSource(&Core, &Token) == MtkHusb311TypecSuccess);
    CHECK(Token != 0U && Core.AttachedCcPin == Pin &&
          Fake.NowMs >= MTK_HUSB311_TYPEC_ATTACH_DEBOUNCE_MS && Fake.NowMs < 500U);
    CHECK(MtkHusb311TypecReleaseSource(&Core, Token) == MtkHusb311TypecSuccess);
  }
  InitializeFake(&Fake, &Core); Fake.FailUpdateNumber = 1U;
  CHECK(MtkHusb311TypecProbeSource(&Core, &Token) != MtkHusb311TypecSuccess);
  CHECK(Token == 0U);
}

static void
CheckRetryWithStaleOpenCc(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;

  InitializeFake(&Fake, &Core);
  Fake.StaleOpenCcAfterMs = MTK_HUSB311_TYPEC_TX_QUIESCENCE_MS +
                            MTK_HUSB311_TYPEC_SAMPLE_PERIOD_MS;
  CHECK(MtkHusb311TypecProbeSource(&Core, &Token) == MtkHusb311TypecTimeout);
  CHECK(Core.FirstFailure.Step == 12U && Token == 0U &&
        Core.State == MtkHusb311TypecStateIdle && Core.OwnedFields == 0U);
  CHECK(Fake.Registers[0x1aU] == 0x52U && Fake.Registers[0x2fU] == 7U);
  for (unsigned Pin = 1U; Pin <= 2U; ++Pin) {
    Fake.AttachPin = (MTK_HUSB311_U8)Pin;
    CHECK(MtkHusb311TypecProbeSource(&Core, &Token) == MtkHusb311TypecSuccess);
    CHECK(Core.AttachedCcPin == Pin && Token != 0U);
    CHECK(MtkHusb311TypecReleaseSource(&Core, Token) == MtkHusb311TypecSuccess);
  }
}

static void
CheckOpenAfterDetach(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;
  unsigned Case;

  for (Case = 0U; Case < 6U; ++Case) {
    InitializeFake(&Fake, &Core);
    Fake.Registers[0x1aU] = 0x0aU;
    Fake.Registers[0x2fU] = 0U;
    Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_1;
    switch (Case) {
      case 0U: Fake.IgnoreUpdateNumber = 1U; break;
      case 1U: Fake.FailUpdateNumber = 1U; break;
      case 2U:
        Fake.IgnoreUpdateNumber = 1U;
        Fake.ChangeRoleOnDelayNumber = 1U;
        Fake.ChangeRoleOnDelayMask = 0x7fU;
        Fake.ChangeRoleOnDelayValue = 0x2fU;
        break;
      case 3U: Fake.IgnoreOpenWrites = 1U; break;
      case 4U:
        Fake.IgnoreUpdateNumber = 1U;
        Fake.VbusOnDelay = 1U;
        break;
      case 5U:
        Fake.FailUpdateNumber = 1U;
        Fake.ApplyFailedUpdate = 1U;
        break;
    }
    MTK_HUSB311_TYPEC_STATUS Status = MtkHusb311TypecProbeSource(&Core, &Token);
    CHECK(Fake.UpdateRegister[0] == 0x1aU && Fake.UpdateValue[0] == 0x2fU);
    if (Case < 3U) {
      unsigned OpenAttempts = Case == 2U ? 1U : 2U;
      CHECK(Status == MtkHusb311TypecSuccess && Token != 0U);
      CHECK(Core.Diagnostic.OpenAttempts == OpenAttempts);
      if (OpenAttempts == 2U) {
        CHECK(Fake.UpdateRegister[1] == 0x1aU && Fake.UpdateValue[1] == 0x2fU);
      }
      CHECK(Fake.UpdateRegister[OpenAttempts] == 0x1aU &&
            Fake.UpdateMask[OpenAttempts] == MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK &&
            Fake.UpdateValue[OpenAttempts] == 0U);
      CHECK(Fake.UpdateRegister[OpenAttempts + 1U] == 0x1aU &&
            Fake.UpdateValue[OpenAttempts + 1U] == 0x05U);
      CHECK(MtkHusb311TypecReleaseSource(&Core, Token) == MtkHusb311TypecSuccess);
    } else {
      unsigned Updates = Case == 3U ? 5U : (Case == 4U ? 1U : 2U);
      CHECK(Status == (Case == 4U ? MtkHusb311TypecNotReady : MtkHusb311TypecIoError));
      CHECK(Token == 0U && Fake.UpdateCount == Updates &&
            Core.State == MtkHusb311TypecStateIdle);
      CHECK(Core.FirstFailure.Step == 9U &&
            Core.FirstFailure.OpenAttempts == (Case == 3U ? 5U : 1U));
      CHECK(Core.FirstFailure.OpenWriteStatus ==
            (Case == 5U ? MtkHusb311TypecIoError : MtkHusb311TypecSuccess));
      for (unsigned I = 0U; I < Updates; ++I) {
        CHECK(Fake.UpdateRegister[I] == 0x1aU &&
              Fake.UpdateValue[I] == (Case == 5U && I == 1U ? 0x0aU : 0x2fU));
      }
    }
    CHECK(Fake.Registers[0x1aU] == 0x0aU && Core.OwnedFields == 0U);
  }
}

static void
CheckPartialOpenAfterDetach(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;

  for (unsigned Pin = 1U; Pin <= 2U; ++Pin) {
    InitializeFake(&Fake, &Core);
    Fake.Registers[0x1aU] = 0x0aU;
    Fake.Registers[0x2fU] = 0U;
    Fake.AttachPin = (MTK_HUSB311_U8)Pin;
    Fake.PartialOpenWrites = 1U;
    CHECK(MtkHusb311TypecProbeSource(&Core, &Token) == MtkHusb311TypecSuccess);
    CHECK(Token != 0U && Core.AttachedCcPin == Pin);
    CHECK(Core.Diagnostic.OpenAttempts == 2U);
    CHECK(Fake.UpdateValue[0] == 0x2fU && Fake.UpdateValue[1] == 0x2fU &&
          Fake.UpdateMask[2] == MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK &&
          Fake.UpdateValue[2] == 0U && Fake.UpdateValue[3] == 0x05U);
    CHECK(MtkHusb311TypecReleaseSource(&Core, Token) == MtkHusb311TypecSuccess);
    CHECK(Fake.Registers[0x1aU] == 0x0aU && Core.OwnedFields == 0U);
  }

  InitializeFake(&Fake, &Core);
  Fake.Registers[0x1aU] = 0x0aU;
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_1;
  Fake.PartialOpenWrites = 5U;
  CHECK(MtkHusb311TypecProbeSource(&Core, &Token) == MtkHusb311TypecIoError);
  CHECK(Token == 0U && Core.State == MtkHusb311TypecStateIdle &&
        Core.OwnedFields == 0U);
  CHECK(Core.FirstFailure.Step == 9U && Core.FirstFailure.Register == 0x1aU &&
        Core.FirstFailure.Value == 0x2aU && Core.FirstFailure.OpenAttempts == 5U);
  CHECK(Fake.Registers[0x1aU] == 0x0aU && Fake.Registers[0x2fU] == 0x07U);
  CHECK(Fake.UpdateCount == 8U);
  for (unsigned Index = 1U; Index <= 5U; ++Index) {
    CHECK(Fake.UpdateRegister[Index] == 0x1aU && Fake.UpdateValue[Index] == 0x2fU);
  }
  CHECK(Fake.UpdateRegister[6] == 0x1aU &&
        Fake.UpdateMask[6] == MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK);
  CHECK(Fake.UpdateRegister[7] == 0x2fU && Fake.UpdateValue[7] == 0x07U);

  InitializeFake(&Fake, &Core);
  Fake.Registers[0x1aU] = 0x0aU;
  Fake.Registers[0x2fU] = 0U;
  Fake.AttachPin = MTK_HUSB311_TYPEC_CC_PIN_1;
  Fake.PartialOpenWrites = 1U;
  Fake.VbusOnDelay = 1U;
  CHECK(MtkHusb311TypecProbeSource(&Core, &Token) == MtkHusb311TypecQuarantined);
  CHECK(Token == 0U && Fake.UpdateCount == 1U && Core.ConflictActive == 0U);
  CHECK(Fake.Registers[0x1aU] == 0x2aU);
}

static void
CheckSourcePulledAfterRpSelection(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;

  for (unsigned Pin = 1U; Pin <= 2U; ++Pin) {
    InitializeFake(&Fake, &Core);
    Fake.Registers[0x1aU] = 0x0aU;
    Fake.Registers[0x2fU] = 0U;
    Fake.AttachPin = (MTK_HUSB311_U8)Pin;
    Fake.DropCombinedSourcePulls = 1U;
    CHECK(MtkHusb311TypecProbeSource(&Core, &Token) == MtkHusb311TypecSuccess);
    CHECK(Token != 0U && Core.AttachedCcPin == Pin);
    CHECK(Fake.UpdateValue[0] == 0x2fU &&
          Fake.UpdateMask[1] == MTK_HUSB311_TYPEC_ROLE_RP_LEVEL_MASK &&
          Fake.UpdateValue[1] == 0U && Fake.UpdateValue[2] == 0x05U);
    CHECK(MtkHusb311TypecReleaseSource(&Core, Token) == MtkHusb311TypecSuccess);
    CHECK(Fake.Registers[0x1aU] == 0x0aU && Core.OwnedFields == 0U);
  }

  InitializeFake(&Fake, &Core);
  Fake.Registers[0x1aU] = 0x0aU;
  Fake.Registers[0x2fU] = 0U;
  Fake.IgnoreSourceWrites = 1U;
  CHECK(MtkHusb311TypecProbeSource(&Core, &Token) == MtkHusb311TypecIoError);
  CHECK(Token == 0U && Core.State == MtkHusb311TypecStateIdle &&
        Core.OwnedFields == 0U);
  CHECK(Core.FirstFailure.Step == 11U && Core.FirstFailure.Register == 0x1aU &&
        Core.FirstFailure.Value == 0x0fU);
  CHECK(Fake.UpdateCount == 8U && Fake.Registers[0x1aU] == 0x0aU);

  InitializeFake(&Fake, &Core);
  Fake.Registers[0x1aU] = 0x0aU;
  Fake.Registers[0x2fU] = 0U;
  Fake.VbusOnUpdateNumber = 2U;
  CHECK(MtkHusb311TypecProbeSource(&Core, &Token) == MtkHusb311TypecQuarantined);
  CHECK(Token == 0U && Fake.UpdateCount == 2U && Fake.Registers[0x1aU] == 0x0fU);
}

static void
CheckCcSearchAfterDetach(void)
{
  FAKE_TCPC Fake;
  MTK_HUSB311_TYPEC_CORE Core;
  MTK_HUSB311_TOKEN Token;

  for (unsigned Pin = 1U; Pin <= 2U; ++Pin) {
    InitializeFake(&Fake, &Core);
    Fake.CcLookingUntilMs[1] = 20U;
    Fake.CcLookingUntilMs[0] = 260U;
    Fake.AttachPin = (MTK_HUSB311_U8)Pin;
    CHECK(MtkHusb311TypecProbeSource(&Core, &Token) == MtkHusb311TypecSuccess);
    CHECK(Core.AttachedCcPin == Pin && Token != 0U);
    CHECK(Fake.NowMs >= 260U + MTK_HUSB311_TYPEC_ATTACH_DEBOUNCE_MS);
    CHECK(MtkHusb311TypecReleaseSource(&Core, Token) == MtkHusb311TypecSuccess);
  }

  InitializeFake(&Fake, &Core);
  Fake.CcLookingUntilMs[0] = 1000U;
  CHECK(MtkHusb311TypecProbeSource(&Core, &Token) == MtkHusb311TypecTimeout);
  CHECK(Core.FirstFailure.Step == 12U && Core.FirstFailure.Value == 0x20U);
  CHECK(Token == 0U && Core.OwnedFields == 0U &&
        Core.State == MtkHusb311TypecStateIdle && Fake.NowMs < 500U);
}

int
main(void)
{
  CheckVbusEventAck();
  CheckRetryWithStaleOpenCc();
  CheckOpenAfterDetach();
  CheckPartialOpenAfterDetach();
  CheckSourcePulledAfterRpSelection();
  CheckCcSearchAfterDetach();
  CheckTimerDrivenProbeDoesNotDelay();
  CheckBoundedSourceProbe();
  CheckFirstFailureSurvivesCleanup();
  CheckAcquireAndRelease();
  CheckRejectsUnsafeInputs();
  CheckTxCompletionQuarantineAndRecovery();
  CheckNoAttachAndReverseCleanup();
  CheckWriteFailureSideEffects();
  CheckReadAndDelayFailures();
  CheckChangingTxResultHasBoundedQuarantine();
  CheckRoleDriftDuringOpenWait();
  CheckRoleDriftAfterOpenWait();
  CheckApplyUpdateReadbackAndOwnership();
  CheckReservedRoleDriftStaysQuarantined();
  CheckDriftDoesNotGetOverwritten();
  CheckCleanupRetryAndBaselineTxAlert();
  printf("HUSB311 Type-C core: %u checks passed (source/DFP attach, bounded TX quiet period, guarded rollback and quarantine)\n", Checks);
  return 0;
}
