// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#include <Library/StockPowerCore.h>

typedef struct {
  SP_U8 Registers[256];
  SP_U8 TraceRegister[16];
  SP_U8 TraceLength[16];
  unsigned TraceCount;
  SP_U8 FailRegister;
  SP_U8 FailEnabled;
} FAKE_BUS;

static SP_POWER_STATUS
FakeRead(
  void *Context,
  SP_U8 FirstRegister,
  SP_U8 Length,
  SP_U8 *Bytes
  )
{
  FAKE_BUS *Bus = (FAKE_BUS *)Context;

  CHECK(Bus != 0);
  CHECK(Bytes != 0);
  CHECK((Length == 1) || (Length == 2));
  CHECK(Bus->TraceCount < 16);
  Bus->TraceRegister[Bus->TraceCount] = FirstRegister;
  Bus->TraceLength[Bus->TraceCount] = Length;
  Bus->TraceCount++;
  if (Bus->FailEnabled && (FirstRegister == Bus->FailRegister)) {
    return SpPowerIoError;
  }
  CHECK(((unsigned)FirstRegister + Length) <= sizeof(Bus->Registers));
  Bytes[0] = Bus->Registers[FirstRegister];
  if (Length == 2) {
    Bytes[1] = Bus->Registers[(unsigned)FirstRegister + 1U];
  }
  return SpPowerOk;
}

static void
ResetBus(FAKE_BUS *Bus)
{
  memset(Bus, 0, sizeof(*Bus));
}

static void
ExpectTrace(const FAKE_BUS *Bus, unsigned Index, SP_U8 Register, SP_U8 Length)
{
  CHECK(Index < Bus->TraceCount);
  CHECK(Bus->TraceRegister[Index] == Register);
  CHECK(Bus->TraceLength[Index] == Length);
}

static void
CheckScStatusCode(SP_U8 ChargeCode, SP_U8 VbusCode, SP_POWER_SUPPLY_STATUS Expected)
{
  FAKE_BUS Bus;
  SP_SC8989X_STATUS Status;

  ResetBus(&Bus);
  memset(&Status, 0xA5, sizeof(Status));
  Bus.Registers[SP_SC8989X_REG_PART_ID] = (SP_U8)(2U << 3);
  Bus.Registers[SP_SC8989X_REG_STATUS_B] =
    (SP_U8)((ChargeCode << 3) | (VbusCode << 5));
  Bus.Registers[SP_SC8989X_REG_STATUS_C] = 0x08U;
  CHECK(SpReadSc8989xStatus(FakeRead, &Bus, &Status) == SpPowerOk);
  CHECK(Status.ChargeStateCode == ChargeCode);
  CHECK(Status.VbusStateCode == VbusCode);
  CHECK(Status.ChargeStatus == Expected);
  CHECK(Status.StatusB == (SP_U8)((ChargeCode << 3) | (VbusCode << 5)));
  CHECK(Status.StatusC == 0x08U);
  CHECK(Status.Reg0CBit3 == 1U);
  CHECK(Bus.TraceCount == 3U);
  ExpectTrace(&Bus, 0, 0x14U, 1);
  ExpectTrace(&Bus, 1, 0x0BU, 1);
  ExpectTrace(&Bus, 2, 0x0CU, 1);
}

int
main(void)
{
  static const SP_U8 AcceptedCodes[] = {2, 4, 7};
  static const SP_U8 RejectedCodes[] = {0, 1, 3, 5, 6};
  static const SP_POWER_SUPPLY_STATUS ChargeMap[] = {
    SpPowerSupplyNotCharging, SpPowerSupplyCharging, SpPowerSupplyCharging,
    SpPowerSupplyFull, SpPowerSupplyUnknown, SpPowerSupplyUnknown,
    SpPowerSupplyUnknown, SpPowerSupplyDischarging
  };
  FAKE_BUS Bus;
  SP_SC8989X_IDENTITY ScIdentity;
  SP_SC8989X_STATUS ScStatus, ScBefore;
  SP_SC8989X_BOOST_STATE BoostState, BoostBefore;
  SP_HUSB311_IDENTITY HUsbIdentity;
  SP_HUSB311_POWER_STATUS PowerStatus;
  SP_HUSB311_CC_STATUS CcStatus;
  SP_HUSB311_ALERT_STATUS Alerts, AlertsBefore;
  SP_U8 Fault, FaultBefore;
  SP_U32 Scaled, ScaledBefore;
  SP_ADC_UNIT Unit, UnitBefore;
  unsigned I;

  ResetBus(&Bus);
  for (I = 0; I < sizeof(AcceptedCodes) / sizeof(AcceptedCodes[0]); ++I) {
    Bus.Registers[SP_SC8989X_REG_PART_ID] = (SP_U8)(AcceptedCodes[I] << 3);
    CHECK(SpReadSc8989xIdentity(FakeRead, &Bus, &ScIdentity) == SpPowerOk);
    CHECK(ScIdentity.PartCode == AcceptedCodes[I]);
    CHECK(ScIdentity.StockDriverRecognizesCode == 1U);
  }
  for (I = 0; I < sizeof(RejectedCodes) / sizeof(RejectedCodes[0]); ++I) {
    Bus.Registers[SP_SC8989X_REG_PART_ID] = (SP_U8)(RejectedCodes[I] << 3);
    CHECK(SpReadSc8989xIdentity(FakeRead, &Bus, &ScIdentity) == SpPowerOk);
    CHECK(ScIdentity.PartCode == RejectedCodes[I]);
    CHECK(ScIdentity.StockDriverRecognizesCode == 0U);
  }

  for (I = 0; I < 256U; ++I) {
    ResetBus(&Bus);
    memset(&BoostState, 0xA5, sizeof(BoostState));
    Bus.Registers[SP_SC8989X_REG_PART_ID] = (SP_U8)(2U << 3);
    Bus.Registers[SP_SC8989X_REG_BOOST_CONTROL] = (SP_U8)I;
    CHECK(SpReadSc8989xBoostState(FakeRead, &Bus, &BoostState) == SpPowerOk);
    CHECK(BoostState.PartCode == 2U);
    CHECK(BoostState.BoostEnabled == (SP_U8)((I & SP_SC8989X_BOOST_ENABLE_MASK) != 0U));
    CHECK(Bus.TraceCount == 2U);
    ExpectTrace(&Bus, 0, SP_SC8989X_REG_PART_ID, 1U);
    ExpectTrace(&Bus, 1, SP_SC8989X_REG_BOOST_CONTROL, 1U);
  }

  ResetBus(&Bus);
  memset(&BoostState, 0xA5, sizeof(BoostState));
  BoostBefore = BoostState;
  Bus.Registers[SP_SC8989X_REG_PART_ID] = (SP_U8)(1U << 3);
  CHECK(SpReadSc8989xBoostState(FakeRead, &Bus, &BoostState) == SpPowerUnsupported);
  CHECK(Bus.TraceCount == 1U);
  ExpectTrace(&Bus, 0, SP_SC8989X_REG_PART_ID, 1U);
  CHECK(memcmp(&BoostState, &BoostBefore, sizeof(BoostState)) == 0);

  ResetBus(&Bus);
  memset(&BoostState, 0xA5, sizeof(BoostState));
  BoostBefore = BoostState;
  Bus.FailEnabled = 1U;
  Bus.FailRegister = SP_SC8989X_REG_PART_ID;
  CHECK(SpReadSc8989xBoostState(FakeRead, &Bus, &BoostState) == SpPowerIoError);
  CHECK(Bus.TraceCount == 1U);
  CHECK(memcmp(&BoostState, &BoostBefore, sizeof(BoostState)) == 0);

  ResetBus(&Bus);
  memset(&BoostState, 0xA5, sizeof(BoostState));
  BoostBefore = BoostState;
  Bus.Registers[SP_SC8989X_REG_PART_ID] = (SP_U8)(2U << 3);
  Bus.FailEnabled = 1U;
  Bus.FailRegister = SP_SC8989X_REG_BOOST_CONTROL;
  CHECK(SpReadSc8989xBoostState(FakeRead, &Bus, &BoostState) == SpPowerIoError);
  CHECK(Bus.TraceCount == 2U);
  ExpectTrace(&Bus, 0, SP_SC8989X_REG_PART_ID, 1U);
  ExpectTrace(&Bus, 1, SP_SC8989X_REG_BOOST_CONTROL, 1U);
  CHECK(memcmp(&BoostState, &BoostBefore, sizeof(BoostState)) == 0);

  for (I = 0; I < 4; ++I) {
    unsigned Vbus;
    for (Vbus = 0; Vbus < 8; ++Vbus) {
      SP_POWER_SUPPLY_STATUS Expected = (Vbus == 7U)
        ? SpPowerSupplyDischarging : ChargeMap[I];
      CheckScStatusCode((SP_U8)I, (SP_U8)Vbus, Expected);
    }
  }

  ResetBus(&Bus);
  memset(&ScStatus, 0xA5, sizeof(ScStatus));
  ScBefore = ScStatus;
  Bus.Registers[SP_SC8989X_REG_PART_ID] = (SP_U8)(1U << 3);
  CHECK(SpReadSc8989xStatus(FakeRead, &Bus, &ScStatus) == SpPowerUnsupported);
  CHECK(Bus.TraceCount == 1U);
  ExpectTrace(&Bus, 0, 0x14U, 1);
  CHECK(memcmp(&ScStatus, &ScBefore, sizeof(ScStatus)) == 0);

  ResetBus(&Bus);
  memset(&ScStatus, 0xA5, sizeof(ScStatus));
  ScBefore = ScStatus;
  Bus.Registers[SP_SC8989X_REG_PART_ID] = (SP_U8)(2U << 3);
  Bus.FailEnabled = 1;
  Bus.FailRegister = SP_SC8989X_REG_STATUS_B;
  CHECK(SpReadSc8989xStatus(FakeRead, &Bus, &ScStatus) == SpPowerIoError);
  CHECK(Bus.TraceCount == 2U);
  CHECK(memcmp(&ScStatus, &ScBefore, sizeof(ScStatus)) == 0);

  ResetBus(&Bus);
  memset(&ScStatus, 0xA5, sizeof(ScStatus));
  ScBefore = ScStatus;
  Bus.Registers[SP_SC8989X_REG_PART_ID] = (SP_U8)(2U << 3);
  Bus.FailEnabled = 1;
  Bus.FailRegister = SP_SC8989X_REG_STATUS_C;
  CHECK(SpReadSc8989xStatus(FakeRead, &Bus, &ScStatus) == SpPowerIoError);
  CHECK(Bus.TraceCount == 3U);
  CHECK(memcmp(&ScStatus, &ScBefore, sizeof(ScStatus)) == 0);

  CHECK(SpReadSc8989xStatus(0, &Bus, &ScStatus) == SpPowerInvalid);
  CHECK(SpReadSc8989xStatus(FakeRead, &Bus, 0) == SpPowerInvalid);
  CHECK(SpReadSc8989xIdentity(FakeRead, &Bus, 0) == SpPowerInvalid);
  CHECK(SpReadSc8989xBoostState(0, &Bus, &BoostState) == SpPowerInvalid);
  CHECK(SpReadSc8989xBoostState(FakeRead, &Bus, 0) == SpPowerInvalid);

  CHECK(SpSc8989xDecodeAdc(2, 123, &Scaled, &Unit) == SpPowerOk);
  CHECK(Scaled == 123000U && Unit == SpAdcMicroVolts);
  CHECK(SpSc8989xDecodeAdc(4, 45, &Scaled, &Unit) == SpPowerOk);
  CHECK(Scaled == 45000U && Unit == SpAdcMicroAmps);
  CHECK(SpSc8989xDecodeAdc(3, 4567, &Scaled, &Unit) == SpPowerOk);
  CHECK(Scaled == 4567U && Unit == SpAdcMicroAmps);
  Scaled = 0x12345678U;
  Unit = SpAdcMicroVolts;
  ScaledBefore = Scaled;
  UnitBefore = Unit;
  CHECK(SpSc8989xDecodeAdc(2, 0xFFFFFFFFU, &Scaled, &Unit) == SpPowerInvalid);
  CHECK(Scaled == ScaledBefore && Unit == UnitBefore);
  CHECK(SpSc8989xDecodeAdc(1, 1, &Scaled, &Unit) == SpPowerUnsupported);
  CHECK(SpSc8989xDecodeAdc(2, 1, 0, &Unit) == SpPowerInvalid);
  CHECK(SpSc8989xDecodeAdc(2, 1, &Scaled, 0) == SpPowerInvalid);

  ResetBus(&Bus);
  Bus.Registers[SP_HUSB311_REG_VENDOR_ID] = 0x99;
  Bus.Registers[SP_HUSB311_REG_VENDOR_ID + 1U] = 0x2E;
  Bus.Registers[SP_HUSB311_REG_DEVICE_ID] = 0x11;
  Bus.Registers[SP_HUSB311_REG_DEVICE_ID + 1U] = 0x03;
  CHECK(SpReadHusb311Identity(FakeRead, &Bus, &HUsbIdentity) == SpPowerOk);
  CHECK(HUsbIdentity.VendorId == 0x2E99U);
  CHECK(HUsbIdentity.DeviceId == 0x0311U);
  CHECK(HUsbIdentity.StockDriverRecognizesId == 1U);
  CHECK(Bus.TraceCount == 2U);
  ExpectTrace(&Bus, 0, 0x00U, 2);
  ExpectTrace(&Bus, 1, 0x02U, 2);
  Bus.Registers[SP_HUSB311_REG_VENDOR_ID] = 0;
  CHECK(SpReadHusb311Identity(FakeRead, &Bus, &HUsbIdentity) == SpPowerOk);
  CHECK(HUsbIdentity.StockDriverRecognizesId == 0U);

  ResetBus(&Bus);
  Bus.Registers[SP_HUSB311_REG_POWER_STATUS] = 0xFCU;
  Bus.Registers[SP_HUSB311_REG_VENDOR_STATUS] = 0xFFU;
  CHECK(SpReadHusb311PowerStatus(FakeRead, &Bus, &PowerStatus) == SpPowerOk);
  CHECK(PowerStatus.PowerStatus == 0xFCU && PowerStatus.VbusPresent == 1U);
  CHECK(PowerStatus.VendorStatusReg97 == 0xFFU && PowerStatus.Reg97Bit1 == 1U);
  CHECK(Bus.TraceCount == 2U);
  ExpectTrace(&Bus, 0, 0x1EU, 1);
  ExpectTrace(&Bus, 1, 0x97U, 1);

  CHECK(SpHusb311VbusSafe0V(0x00U, 0x00U) == 0);
  CHECK(SpHusb311VbusSafe0V(0x04U, 0x00U) == 0);
  CHECK(SpHusb311VbusSafe0V(0x00U, 0x02U) == 1);
  CHECK(SpHusb311VbusSafe0V(0x04U, 0x02U) == 0);
  CHECK(SpHusb311VbusSafe0V(0xFBU, 0xFDU) == 0);
  CHECK(SpHusb311VbusSafe0V(0xFFU, 0xFDU) == 0);
  CHECK(SpHusb311VbusSafe0V(0xFBU, 0xFFU) == 1);

  ResetBus(&Bus);
  Bus.Registers[SP_HUSB311_REG_CC_STATUS] = 0x20U | 0x0FU;
  CHECK(SpReadHusb311CcStatus(FakeRead, &Bus, &CcStatus) == SpPowerOk);
  CHECK(CcStatus.CcStatusBit5 == 1U);
  CHECK(CcStatus.DriverCc1Value == 0x0FU && CcStatus.DriverCc2Value == 0x0FU);
  CHECK(CcStatus.RoleControlWasRead == 0U);
  CHECK(Bus.TraceCount == 1U);
  ExpectTrace(&Bus, 0, 0x1DU, 1);

  ResetBus(&Bus);
  Bus.Registers[SP_HUSB311_REG_CC_STATUS] = 0x1DU;
  Bus.Registers[SP_HUSB311_REG_ROLE_CONTROL] = 0x40U;
  CHECK(SpReadHusb311CcStatus(FakeRead, &Bus, &CcStatus) == SpPowerOk);
  CHECK(CcStatus.Cc1Field == 1U && CcStatus.Cc2Field == 3U);
  CHECK(CcStatus.DriverCc1Value == 5U && CcStatus.DriverCc2Value == 7U);
  CHECK(CcStatus.CcAugmentationCode == 1U);
  CHECK(CcStatus.RoleControlWasRead == 1U && CcStatus.RoleControl == 0x40U);
  CHECK(Bus.TraceCount == 2U);
  ExpectTrace(&Bus, 0, 0x1DU, 1);
  ExpectTrace(&Bus, 1, 0x1AU, 1);

  ResetBus(&Bus);
  memset(&CcStatus, 0xA5, sizeof(CcStatus));
  {
    SP_HUSB311_CC_STATUS Before = CcStatus;
    Bus.Registers[SP_HUSB311_REG_CC_STATUS] = 0;
    Bus.FailEnabled = 1;
    Bus.FailRegister = SP_HUSB311_REG_ROLE_CONTROL;
    CHECK(SpReadHusb311CcStatus(FakeRead, &Bus, &CcStatus) == SpPowerIoError);
    CHECK(memcmp(&CcStatus, &Before, sizeof(CcStatus)) == 0);
  }

  ResetBus(&Bus);
  Bus.Registers[SP_HUSB311_REG_ALERT] = 0x34;
  Bus.Registers[SP_HUSB311_REG_ALERT + 1U] = 0x12;
  Bus.Registers[SP_HUSB311_REG_VENDOR_ALERT] = 0x56;
  Bus.Registers[SP_HUSB311_REG_ALERT_MASK] = 0x78;
  Bus.Registers[SP_HUSB311_REG_ALERT_MASK + 1U] = 0x9A;
  Bus.Registers[SP_HUSB311_REG_VENDOR_MASK] = 0xBC;
  CHECK(SpReadHusb311Alerts(FakeRead, &Bus, &Alerts) == SpPowerOk);
  CHECK(Alerts.AlertStatus == 0x00561234U);
  CHECK(Alerts.AlertMask == 0x00BC9A78U);
  CHECK(Bus.TraceCount == 4U);
  ExpectTrace(&Bus, 0, 0x10U, 2);
  ExpectTrace(&Bus, 1, 0x98U, 1);
  ExpectTrace(&Bus, 2, 0x12U, 2);
  ExpectTrace(&Bus, 3, 0x99U, 1);

  for (I = 0; I < 4; ++I) {
    ResetBus(&Bus);
    memset(&Alerts, 0xA5, sizeof(Alerts));
    AlertsBefore = Alerts;
    Bus.FailEnabled = 1;
    Bus.FailRegister = (SP_U8[]){0x10U, 0x98U, 0x12U, 0x99U}[I];
    CHECK(SpReadHusb311Alerts(FakeRead, &Bus, &Alerts) == SpPowerIoError);
    CHECK(memcmp(&Alerts, &AlertsBefore, sizeof(Alerts)) == 0);
    CHECK(Bus.TraceCount == I + 1U);
  }

  ResetBus(&Bus);
  Bus.Registers[SP_HUSB311_REG_FAULT_STATUS] = 0xA5U;
  CHECK(SpReadHusb311FaultStatus(FakeRead, &Bus, &Fault) == SpPowerOk);
  CHECK(Fault == 0xA5U);
  ExpectTrace(&Bus, 0, 0x1FU, 1);

  ResetBus(&Bus);
  Fault = 0x5AU;
  FaultBefore = Fault;
  Bus.FailEnabled = 1;
  Bus.FailRegister = SP_HUSB311_REG_FAULT_STATUS;
  CHECK(SpReadHusb311FaultStatus(FakeRead, &Bus, &Fault) == SpPowerIoError);
  CHECK(Fault == FaultBefore);

  printf("Stock power core: %u checks passed (read order, IDs, status, ADC scaling, alert/fault errors)\n", Checks);
  return 0;
}
