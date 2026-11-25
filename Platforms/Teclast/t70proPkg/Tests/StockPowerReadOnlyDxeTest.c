// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "MediaTekTest.h"
#include <Protocol/I2cMaster.h>
#include <Protocol/StockPowerReadOnly.h>
#include "../../../../Silicon/MediaTek/MediaTekPkg/Drivers/StockPowerReadOnlyDxe/StockPowerReadOnlyDxe.c"

EFI_GUID gEfiI2cMasterProtocolGuid = EFI_I2C_MASTER_PROTOCOL_GUID;
EFI_GUID gStockPowerReadOnlyProtocolGuid = STOCK_POWER_READ_ONLY_PROTOCOL_GUID;
EFI_GUID gStockPowerBoardReadyProtocolGuid = STOCK_POWER_BOARD_READY_PROTOCOL_GUID;

typedef struct {
  SP_U8 ScRegisters[256];
  SP_U8 HusbRegisters[256];
  UINTN TraceCount;
  UINTN TraceAddress[32];
  SP_U8 TraceRegister[32];
  SP_U8 TraceLength[32];
  SP_U8 FailEnabled;
  SP_U8 FailRegister;
} FAKE_I2C;

static FAKE_I2C mBus;
static EFI_HANDLE mControllerHandle = (EFI_HANDLE)(UINTN)0x1234;
static STOCK_POWER_BOARD_READY_PROTOCOL mBoardReady;
static EFI_I2C_MASTER_PROTOCOL mFakeMaster;

static EFI_STATUS EFIAPI FakeInstallProtocolInterface(
  EFI_HANDLE *Handle,
  EFI_GUID *Protocol,
  EFI_INTERFACE_TYPE InterfaceType,
  VOID *Interface
  )
{
  (VOID)Handle;
  (VOID)Protocol;
  (VOID)InterfaceType;
  (VOID)Interface;
  Installs++;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI FakeLocateProtocol(
  EFI_GUID *Protocol,
  VOID *Registration,
  VOID **Interface
  )
{
  (VOID)Registration;
  if (CompareMem(Protocol, &gStockPowerBoardReadyProtocolGuid, sizeof(EFI_GUID)) != 0) {
    return EFI_NOT_FOUND;
  }
  *Interface = &mBoardReady;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI FakeHandleProtocol(
  EFI_HANDLE Handle,
  EFI_GUID *Protocol,
  VOID **Interface
  )
{
  if ((Handle != mControllerHandle) ||
      (CompareMem(Protocol, &gEfiI2cMasterProtocolGuid, sizeof(EFI_GUID)) != 0)) {
    return EFI_NOT_FOUND;
  }
  *Interface = &mFakeMaster;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI FakeStartRequest(
  CONST EFI_I2C_MASTER_PROTOCOL *This,
  UINTN SlaveAddress,
  EFI_I2C_REQUEST_PACKET *Request,
  EFI_EVENT Event,
  EFI_STATUS *I2cStatus
  )
{
  SP_U8 Register;
  SP_U8 *Registers;
  EFI_I2C_OPERATION *WriteOperation;
  EFI_I2C_OPERATION *ReadOperation;
  UINTN Index;

  (VOID)Event;
  (VOID)I2cStatus;
  CHECK(This == &mFakeMaster);
  CHECK(Request != 0);
  CHECK(Request->OperationCount == 2U);
  CHECK(mBus.TraceCount < 32U);
  CHECK((SlaveAddress == STOCK_POWER_SC8989X_ADDRESS) ||
        (SlaveAddress == STOCK_POWER_HUSB311_ADDRESS));
  WriteOperation = &Request->Operation[0];
  ReadOperation = &Request->Operation[1];
  CHECK(WriteOperation->Flags == 0U);
  CHECK(WriteOperation->LengthInBytes == 1U);
  CHECK(WriteOperation->Buffer != 0);
  CHECK(ReadOperation->Flags == I2C_FLAG_READ);
  CHECK((ReadOperation->LengthInBytes == 1U) ||
        (ReadOperation->LengthInBytes == 2U));
  CHECK(ReadOperation->Buffer != 0);
  Register = WriteOperation->Buffer[0];

  mBus.TraceAddress[mBus.TraceCount] = SlaveAddress;
  mBus.TraceRegister[mBus.TraceCount] = Register;
  mBus.TraceLength[mBus.TraceCount] = (SP_U8)ReadOperation->LengthInBytes;
  mBus.TraceCount++;

  if (mBus.FailEnabled && (Register == mBus.FailRegister)) {
    return EFI_DEVICE_ERROR;
  }
  Registers = (SlaveAddress == STOCK_POWER_SC8989X_ADDRESS)
    ? mBus.ScRegisters : mBus.HusbRegisters;
  CHECK(((UINTN)Register + ReadOperation->LengthInBytes) <= 256U);
  for (Index = 0; Index < ReadOperation->LengthInBytes; ++Index) {
    ReadOperation->Buffer[Index] = Registers[(UINTN)Register + Index];
  }
  return EFI_SUCCESS;
}

static void
ResetBus(void)
{
  memset(&mBus, 0, sizeof(mBus));
}

static void
ExpectTransfer(UINTN Index, UINTN Address, SP_U8 Register, SP_U8 Length)
{
  CHECK(Index < mBus.TraceCount);
  CHECK(mBus.TraceAddress[Index] == Address);
  CHECK(mBus.TraceRegister[Index] == Register);
  CHECK(mBus.TraceLength[Index] == Length);
}

int
main(void)
{
  SP_SC8989X_STATUS ScStatus;
  SP_SC8989X_CHARGE_STATE Charge;
  SP_SC8989X_IDENTITY ScIdentity;
  SP_SC8989X_BOOST_STATE ScBoostState, ScBoostBefore;
  SP_HUSB311_IDENTITY HUsbIdentity;
  SP_HUSB311_ALERT_STATUS Alerts;
  SP_HUSB311_CC_STATUS CcStatus;
  SP_U8 Fault;
  SP_U32 Scaled;
  SP_ADC_UNIT Unit;
  EFI_STATUS Status;

  memset(&Services, 0, sizeof(Services));
  Services.InstallProtocolInterface = FakeInstallProtocolInterface;
  Services.LocateProtocol = FakeLocateProtocol;
  Services.HandleProtocol = FakeHandleProtocol;
  mFakeMaster.StartRequest = FakeStartRequest;
  mBoardReady.Signature = STOCK_POWER_BOARD_READY_SIGNATURE;
  mBoardReady.Revision = STOCK_POWER_BOARD_READY_REVISION;
  mBoardReady.Flags = STOCK_POWER_BOARD_READY_ALLOW_STATUS_READ;
  mBoardReady.I2cMasterHandle = mControllerHandle;
  mBoardReady.Sc8989xAddress = STOCK_POWER_SC8989X_ADDRESS;
  mBoardReady.Husb311Address = STOCK_POWER_HUSB311_ADDRESS;

  Status = StockPowerReadOnlyEntryPoint(0, 0);
  CHECK(Status == EFI_SUCCESS);
  CHECK(Installs == 1U);
  CHECK(mReadOnlyProtocol.Revision == STOCK_POWER_READ_ONLY_PROTOCOL_REVISION);
  CHECK(mReadOnlyProtocol.GetSc8989xBoostState != NULL);

  ResetBus();
  mBus.ScRegisters[SP_SC8989X_REG_PART_ID] = (SP_U8)(2U << 3);
  mBus.ScRegisters[SP_SC8989X_REG_STATUS_B] = (SP_U8)((2U << 3) | (7U << 5));
  mBus.ScRegisters[SP_SC8989X_REG_STATUS_C] = 0x08U;
  mBus.ScRegisters[SP_SC8989X_REG_BOOST_CONTROL] = SP_SC8989X_BOOST_ENABLE_MASK;
  CHECK(mReadOnlyProtocol.GetSc8989xIdentity(&mReadOnlyProtocol, &ScIdentity) == EFI_SUCCESS);
  CHECK(ScIdentity.StockDriverRecognizesCode == 1U);
  CHECK(mReadOnlyProtocol.GetSc8989xStatus(&mReadOnlyProtocol, &ScStatus) == EFI_SUCCESS);
  CHECK(ScStatus.ChargeStatus == SpPowerSupplyDischarging);
  CHECK(mReadOnlyProtocol.GetSc8989xBoostState(&mReadOnlyProtocol, &ScBoostState) == EFI_SUCCESS);
  CHECK(ScBoostState.PartCode == 2U && ScBoostState.BoostEnabled == 1U);
  CHECK(mBus.TraceCount == 6U);
  ExpectTransfer(0, STOCK_POWER_SC8989X_ADDRESS, 0x14U, 1);
  ExpectTransfer(1, STOCK_POWER_SC8989X_ADDRESS, 0x14U, 1);
  ExpectTransfer(2, STOCK_POWER_SC8989X_ADDRESS, 0x0BU, 1);
  ExpectTransfer(3, STOCK_POWER_SC8989X_ADDRESS, 0x0CU, 1);
  ExpectTransfer(4, STOCK_POWER_SC8989X_ADDRESS, 0x14U, 1);
  ExpectTransfer(5, STOCK_POWER_SC8989X_ADDRESS, 0x03U, 1);

  ResetBus();
  mBus.ScRegisters[SP_SC8989X_REG_PART_ID] = (SP_U8)(2U << 3);
  mBus.ScRegisters[SP_SC8989X_REG_STATUS_B] = (SP_U8)((3U << 5) | (2U << 3) | (1U << 2));
  CHECK(mReadOnlyProtocol.GetSc8989xChargeState(&mReadOnlyProtocol, &Charge) == EFI_SUCCESS);
  CHECK(Charge.ChargeStatus == SpPowerSupplyCharging && Charge.PowerGood && Charge.VbusStateCode == 3U);
  CHECK(mBus.TraceCount == 2U);
  ExpectTransfer(0, STOCK_POWER_SC8989X_ADDRESS, SP_SC8989X_REG_PART_ID, 1);
  ExpectTransfer(1, STOCK_POWER_SC8989X_ADDRESS, SP_SC8989X_REG_STATUS_B, 1);
  mBus.FailEnabled = 1U;
  mBus.FailRegister = SP_SC8989X_REG_STATUS_B;
  CHECK(mReadOnlyProtocol.GetSc8989xChargeState(&mReadOnlyProtocol, &Charge) == EFI_DEVICE_ERROR);
  CHECK(Charge.ChargeStatus == SpPowerSupplyCharging && Charge.PowerGood && Charge.VbusStateCode == 3U);

  ResetBus();
  memset(&ScBoostState, 0xA5, sizeof(ScBoostState));
  ScBoostBefore = ScBoostState;
  mBus.ScRegisters[SP_SC8989X_REG_PART_ID] = (SP_U8)(1U << 3);
  CHECK(mReadOnlyProtocol.GetSc8989xBoostState(&mReadOnlyProtocol, &ScBoostState) == EFI_UNSUPPORTED);
  CHECK(mBus.TraceCount == 1U);
  ExpectTransfer(0, STOCK_POWER_SC8989X_ADDRESS, SP_SC8989X_REG_PART_ID, 1U);
  CHECK(memcmp(&ScBoostState, &ScBoostBefore, sizeof(ScBoostState)) == 0);

  ResetBus();
  memset(&ScBoostState, 0xA5, sizeof(ScBoostState));
  ScBoostBefore = ScBoostState;
  mBus.ScRegisters[SP_SC8989X_REG_PART_ID] = (SP_U8)(2U << 3);
  mBus.FailEnabled = 1U;
  mBus.FailRegister = SP_SC8989X_REG_BOOST_CONTROL;
  CHECK(mReadOnlyProtocol.GetSc8989xBoostState(&mReadOnlyProtocol, &ScBoostState) == EFI_DEVICE_ERROR);
  CHECK(mBus.TraceCount == 2U);
  ExpectTransfer(0, STOCK_POWER_SC8989X_ADDRESS, SP_SC8989X_REG_PART_ID, 1U);
  ExpectTransfer(1, STOCK_POWER_SC8989X_ADDRESS, SP_SC8989X_REG_BOOST_CONTROL, 1U);
  CHECK(memcmp(&ScBoostState, &ScBoostBefore, sizeof(ScBoostState)) == 0);

  ResetBus();
  mBus.HusbRegisters[SP_HUSB311_REG_VENDOR_ID] = 0x99;
  mBus.HusbRegisters[SP_HUSB311_REG_VENDOR_ID + 1U] = 0x2E;
  mBus.HusbRegisters[SP_HUSB311_REG_DEVICE_ID] = 0x11;
  mBus.HusbRegisters[SP_HUSB311_REG_DEVICE_ID + 1U] = 0x03;
  mBus.HusbRegisters[SP_HUSB311_REG_CC_STATUS] = 0x1DU;
  mBus.HusbRegisters[SP_HUSB311_REG_ROLE_CONTROL] = 0x40U;
  mBus.HusbRegisters[SP_HUSB311_REG_ALERT] = 0x34U;
  mBus.HusbRegisters[SP_HUSB311_REG_ALERT + 1U] = 0x12U;
  mBus.HusbRegisters[SP_HUSB311_REG_VENDOR_ALERT] = 0x56U;
  mBus.HusbRegisters[SP_HUSB311_REG_ALERT_MASK] = 0x78U;
  mBus.HusbRegisters[SP_HUSB311_REG_ALERT_MASK + 1U] = 0x9AU;
  mBus.HusbRegisters[SP_HUSB311_REG_VENDOR_MASK] = 0xBCU;
  mBus.HusbRegisters[SP_HUSB311_REG_FAULT_STATUS] = 0xA5U;
  CHECK(mReadOnlyProtocol.GetHusb311Identity(&mReadOnlyProtocol, &HUsbIdentity) == EFI_SUCCESS);
  CHECK(HUsbIdentity.VendorId == 0x2E99U && HUsbIdentity.DeviceId == 0x0311U);
  CHECK(mReadOnlyProtocol.GetHusb311CcStatus(&mReadOnlyProtocol, &CcStatus) == EFI_SUCCESS);
  CHECK(CcStatus.DriverCc1Value == 5U && CcStatus.DriverCc2Value == 7U);
  CHECK(mReadOnlyProtocol.GetHusb311Alerts(&mReadOnlyProtocol, &Alerts) == EFI_SUCCESS);
  CHECK(Alerts.AlertStatus == 0x00561234U && Alerts.AlertMask == 0x00BC9A78U);
  CHECK(mReadOnlyProtocol.GetHusb311FaultStatus(&mReadOnlyProtocol, &Fault) == EFI_SUCCESS);
  CHECK(Fault == 0xA5U);
  CHECK(mBus.TraceCount == 15U);
  ExpectTransfer(0, STOCK_POWER_HUSB311_ADDRESS, 0x00U, 2);
  ExpectTransfer(1, STOCK_POWER_HUSB311_ADDRESS, 0x02U, 2);
  ExpectTransfer(2, STOCK_POWER_HUSB311_ADDRESS, 0x00U, 2);
  ExpectTransfer(3, STOCK_POWER_HUSB311_ADDRESS, 0x02U, 2);
  ExpectTransfer(4, STOCK_POWER_HUSB311_ADDRESS, 0x1DU, 1);
  ExpectTransfer(5, STOCK_POWER_HUSB311_ADDRESS, 0x1AU, 1);
  ExpectTransfer(6, STOCK_POWER_HUSB311_ADDRESS, 0x00U, 2);
  ExpectTransfer(7, STOCK_POWER_HUSB311_ADDRESS, 0x02U, 2);
  ExpectTransfer(8, STOCK_POWER_HUSB311_ADDRESS, 0x10U, 2);
  ExpectTransfer(9, STOCK_POWER_HUSB311_ADDRESS, 0x98U, 1);
  ExpectTransfer(10, STOCK_POWER_HUSB311_ADDRESS, 0x12U, 2);
  ExpectTransfer(11, STOCK_POWER_HUSB311_ADDRESS, 0x99U, 1);
  ExpectTransfer(12, STOCK_POWER_HUSB311_ADDRESS, 0x00U, 2);
  ExpectTransfer(13, STOCK_POWER_HUSB311_ADDRESS, 0x02U, 2);
  ExpectTransfer(14, STOCK_POWER_HUSB311_ADDRESS, 0x1FU, 1);

  ResetBus();
  mBoardReady.Flags = 0;
  CHECK(mReadOnlyProtocol.GetSc8989xChargeState(&mReadOnlyProtocol, &Charge) == EFI_ACCESS_DENIED);
  CHECK(mReadOnlyProtocol.GetSc8989xIdentity(&mReadOnlyProtocol, &ScIdentity) == EFI_ACCESS_DENIED);
  CHECK(mReadOnlyProtocol.GetSc8989xBoostState(&mReadOnlyProtocol, &ScBoostState) == EFI_ACCESS_DENIED);
  CHECK(mBus.TraceCount == 0U);
  mBoardReady.Flags = STOCK_POWER_BOARD_READY_ALLOW_STATUS_READ;
  mBoardReady.Husb311Address = 0x4DU;
  CHECK(mReadOnlyProtocol.GetHusb311Identity(&mReadOnlyProtocol, &HUsbIdentity) == EFI_UNSUPPORTED);
  CHECK(mBus.TraceCount == 0U);
  mBoardReady.Husb311Address = STOCK_POWER_HUSB311_ADDRESS;

  CHECK(mReadOnlyProtocol.DecodeSc8989xAdc(&mReadOnlyProtocol, 2, 123U, &Scaled, &Unit) == EFI_SUCCESS);
  CHECK(Scaled == 123000U && Unit == SpAdcMicroVolts);

  printf("Stock power DXE path: %u checks passed (board-ready gate, I2C pointer/repeated-start shape, IDs, status)\n", Checks);
  return 0;
}
