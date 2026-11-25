// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <PiDxe.h>
#include <Protocol/I2cMaster.h>
#include <Protocol/StockPowerReadOnly.h>
#include <Library/StockPowerCore.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

typedef struct {
  EFI_I2C_MASTER_PROTOCOL *Master;
  UINTN Address;
  EFI_STATUS LastStatus;
} STOCK_POWER_READ_CONTEXT;

typedef struct {
  UINTN OperationCount;
  EFI_I2C_OPERATION Operation[2];
} STOCK_POWER_I2C_REQUEST;

STATIC EFI_HANDLE mServiceHandle;
STATIC STOCK_POWER_READ_ONLY_PROTOCOL mReadOnlyProtocol;

STATIC SP_POWER_STATUS
ReadRegisters(
  VOID *Context,
  SP_U8 FirstRegister,
  SP_U8 Length,
  SP_U8 *Bytes
  )
{
  STOCK_POWER_READ_CONTEXT *ReadContext;
  STOCK_POWER_I2C_REQUEST Request;
  SP_U8 RegisterPointer;

  ReadContext = (STOCK_POWER_READ_CONTEXT *)Context;
  if ((ReadContext == NULL) || (ReadContext->Master == NULL) ||
      (ReadContext->Master->StartRequest == NULL) || (Bytes == NULL) ||
      (Length == 0) || (Length > 2) ||
      (((UINTN)FirstRegister + Length) > 0x100U)) {
    return SpPowerInvalid;
  }

  RegisterPointer = FirstRegister;
  Request.OperationCount = 2;
  Request.Operation[0].Flags = 0;
  Request.Operation[0].LengthInBytes = 1;
  Request.Operation[0].Buffer = &RegisterPointer;
  Request.Operation[1].Flags = I2C_FLAG_READ;
  Request.Operation[1].LengthInBytes = Length;
  Request.Operation[1].Buffer = Bytes;

  /*
   * EFI_I2C_REQUEST_PACKET defines a repeated START between operations.
   * The first operation sends only a register pointer; no register value is
   * sent. StartRequest is synchronous because Event is NULL.
   */
  ReadContext->LastStatus = ReadContext->Master->StartRequest(
    ReadContext->Master,
    ReadContext->Address,
    (EFI_I2C_REQUEST_PACKET *)&Request,
    NULL,
    NULL
    );
  return EFI_ERROR(ReadContext->LastStatus) ? SpPowerIoError : SpPowerOk;
}

STATIC EFI_STATUS
MapCoreStatus(
  SP_POWER_STATUS Status,
  const STOCK_POWER_READ_CONTEXT *ReadContext
  )
{
  switch (Status) {
    case SpPowerOk:
      return EFI_SUCCESS;
    case SpPowerInvalid:
      return EFI_INVALID_PARAMETER;
    case SpPowerUnsupported:
      return EFI_UNSUPPORTED;
    case SpPowerIoError:
      if ((ReadContext != NULL) && EFI_ERROR(ReadContext->LastStatus)) {
        return ReadContext->LastStatus;
      }
      return EFI_DEVICE_ERROR;
    default:
      return EFI_DEVICE_ERROR;
  }
}

STATIC EFI_STATUS
GetReadContext(
  UINTN ExpectedAddress,
  STOCK_POWER_READ_CONTEXT *ReadContext
  )
{
  STOCK_POWER_BOARD_READY_PROTOCOL *BoardReady;
  EFI_I2C_MASTER_PROTOCOL *Master;
  EFI_STATUS Status;

  if (ReadContext == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->LocateProtocol(
    &gStockPowerBoardReadyProtocolGuid,
    NULL,
    (VOID **)&BoardReady
    );
  if (EFI_ERROR(Status) || (BoardReady == NULL)) {
    return EFI_NOT_READY;
  }
  if ((BoardReady->Signature != STOCK_POWER_BOARD_READY_SIGNATURE) ||
      (BoardReady->Revision != STOCK_POWER_BOARD_READY_REVISION)) {
    return EFI_NOT_READY;
  }
  if (BoardReady->Flags != STOCK_POWER_BOARD_READY_ALLOW_STATUS_READ) {
    return EFI_ACCESS_DENIED;
  }
  if ((BoardReady->I2cMasterHandle == NULL) ||
      (BoardReady->Sc8989xAddress != STOCK_POWER_SC8989X_ADDRESS) ||
      (BoardReady->Husb311Address != STOCK_POWER_HUSB311_ADDRESS) ||
      ((ExpectedAddress != STOCK_POWER_SC8989X_ADDRESS) &&
       (ExpectedAddress != STOCK_POWER_HUSB311_ADDRESS))) {
    return EFI_UNSUPPORTED;
  }

  Status = gBS->HandleProtocol(
    BoardReady->I2cMasterHandle,
    &gEfiI2cMasterProtocolGuid,
    (VOID **)&Master
    );
  if (EFI_ERROR(Status) || (Master == NULL) || (Master->StartRequest == NULL)) {
    return EFI_NOT_READY;
  }

  ReadContext->Master = Master;
  ReadContext->Address = ExpectedAddress;
  ReadContext->LastStatus = EFI_NOT_READY;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ValidateHusb311Identity(
  STOCK_POWER_READ_CONTEXT *ReadContext
  )
{
  SP_HUSB311_IDENTITY Identity;
  SP_POWER_STATUS Status;

  Status = SpReadHusb311Identity(ReadRegisters, ReadContext, &Identity);
  if (Status != SpPowerOk) {
    return MapCoreStatus(Status, ReadContext);
  }
  return Identity.StockDriverRecognizesId ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
EFIAPI
GetSc8989xIdentity(
  STOCK_POWER_READ_ONLY_PROTOCOL *This,
  SP_SC8989X_IDENTITY *Identity
  )
{
  STOCK_POWER_READ_CONTEXT Context;
  SP_POWER_STATUS Status;
  EFI_STATUS EfiStatus;

  if ((This != &mReadOnlyProtocol) || (Identity == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  EfiStatus = GetReadContext(STOCK_POWER_SC8989X_ADDRESS, &Context);
  if (EFI_ERROR(EfiStatus)) {
    return EfiStatus;
  }
  Status = SpReadSc8989xIdentity(ReadRegisters, &Context, Identity);
  return MapCoreStatus(Status, &Context);
}

STATIC EFI_STATUS
EFIAPI
GetSc8989xStatus(
  STOCK_POWER_READ_ONLY_PROTOCOL *This,
  SP_SC8989X_STATUS *StatusOut
  )
{
  STOCK_POWER_READ_CONTEXT Context;
  SP_POWER_STATUS Status;
  EFI_STATUS EfiStatus;

  if ((This != &mReadOnlyProtocol) || (StatusOut == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  EfiStatus = GetReadContext(STOCK_POWER_SC8989X_ADDRESS, &Context);
  if (EFI_ERROR(EfiStatus)) {
    return EfiStatus;
  }
  Status = SpReadSc8989xStatus(ReadRegisters, &Context, StatusOut);
  return MapCoreStatus(Status, &Context);
}

STATIC EFI_STATUS
EFIAPI
GetSc8989xChargeState(
  STOCK_POWER_READ_ONLY_PROTOCOL *This,
  SP_SC8989X_CHARGE_STATE *State
  )
{
  STOCK_POWER_READ_CONTEXT Context;
  EFI_STATUS Status;

  if ((This != &mReadOnlyProtocol) || (State == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = GetReadContext(STOCK_POWER_SC8989X_ADDRESS, &Context);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  return MapCoreStatus(SpReadSc8989xChargeState(ReadRegisters, &Context, State), &Context);
}

STATIC EFI_STATUS
EFIAPI
GetSc8989xBoostState(
  STOCK_POWER_READ_ONLY_PROTOCOL *This,
  SP_SC8989X_BOOST_STATE *State
  )
{
  STOCK_POWER_READ_CONTEXT Context;
  SP_POWER_STATUS Status;
  EFI_STATUS EfiStatus;

  if ((This != &mReadOnlyProtocol) || (State == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  EfiStatus = GetReadContext(STOCK_POWER_SC8989X_ADDRESS, &Context);
  if (EFI_ERROR(EfiStatus)) {
    return EfiStatus;
  }
  Status = SpReadSc8989xBoostState(ReadRegisters, &Context, State);
  return MapCoreStatus(Status, &Context);
}

STATIC EFI_STATUS
EFIAPI
DecodeSc8989xAdc(
  STOCK_POWER_READ_ONLY_PROTOCOL *This,
  SP_U8 Channel,
  SP_U32 RawValue,
  SP_U32 *ScaledValue,
  SP_ADC_UNIT *Unit
  )
{
  if (This != &mReadOnlyProtocol) {
    return EFI_INVALID_PARAMETER;
  }
  return MapCoreStatus(SpSc8989xDecodeAdc(Channel, RawValue, ScaledValue, Unit), NULL);
}

STATIC EFI_STATUS
EFIAPI
GetHusb311Identity(
  STOCK_POWER_READ_ONLY_PROTOCOL *This,
  SP_HUSB311_IDENTITY *Identity
  )
{
  STOCK_POWER_READ_CONTEXT Context;
  SP_POWER_STATUS Status;
  EFI_STATUS EfiStatus;

  if ((This != &mReadOnlyProtocol) || (Identity == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  EfiStatus = GetReadContext(STOCK_POWER_HUSB311_ADDRESS, &Context);
  if (EFI_ERROR(EfiStatus)) {
    return EfiStatus;
  }
  Status = SpReadHusb311Identity(ReadRegisters, &Context, Identity);
  return MapCoreStatus(Status, &Context);
}

STATIC EFI_STATUS
EFIAPI
GetHusb311PowerStatus(
  STOCK_POWER_READ_ONLY_PROTOCOL *This,
  SP_HUSB311_POWER_STATUS *StatusOut
  )
{
  STOCK_POWER_READ_CONTEXT Context;
  SP_POWER_STATUS Status;
  EFI_STATUS EfiStatus;

  if ((This != &mReadOnlyProtocol) || (StatusOut == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  EfiStatus = GetReadContext(STOCK_POWER_HUSB311_ADDRESS, &Context);
  if (EFI_ERROR(EfiStatus)) {
    return EfiStatus;
  }
  EfiStatus = ValidateHusb311Identity(&Context);
  if (EFI_ERROR(EfiStatus)) {
    return EfiStatus;
  }
  Status = SpReadHusb311PowerStatus(ReadRegisters, &Context, StatusOut);
  return MapCoreStatus(Status, &Context);
}

STATIC EFI_STATUS
EFIAPI
GetHusb311CcStatus(
  STOCK_POWER_READ_ONLY_PROTOCOL *This,
  SP_HUSB311_CC_STATUS *StatusOut
  )
{
  STOCK_POWER_READ_CONTEXT Context;
  SP_POWER_STATUS Status;
  EFI_STATUS EfiStatus;

  if ((This != &mReadOnlyProtocol) || (StatusOut == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  EfiStatus = GetReadContext(STOCK_POWER_HUSB311_ADDRESS, &Context);
  if (EFI_ERROR(EfiStatus)) {
    return EfiStatus;
  }
  EfiStatus = ValidateHusb311Identity(&Context);
  if (EFI_ERROR(EfiStatus)) {
    return EfiStatus;
  }
  Status = SpReadHusb311CcStatus(ReadRegisters, &Context, StatusOut);
  return MapCoreStatus(Status, &Context);
}

STATIC EFI_STATUS
EFIAPI
GetHusb311Alerts(
  STOCK_POWER_READ_ONLY_PROTOCOL *This,
  SP_HUSB311_ALERT_STATUS *StatusOut
  )
{
  STOCK_POWER_READ_CONTEXT Context;
  SP_POWER_STATUS Status;
  EFI_STATUS EfiStatus;

  if ((This != &mReadOnlyProtocol) || (StatusOut == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  EfiStatus = GetReadContext(STOCK_POWER_HUSB311_ADDRESS, &Context);
  if (EFI_ERROR(EfiStatus)) {
    return EfiStatus;
  }
  EfiStatus = ValidateHusb311Identity(&Context);
  if (EFI_ERROR(EfiStatus)) {
    return EfiStatus;
  }
  Status = SpReadHusb311Alerts(ReadRegisters, &Context, StatusOut);
  return MapCoreStatus(Status, &Context);
}

STATIC EFI_STATUS
EFIAPI
GetHusb311FaultStatus(
  STOCK_POWER_READ_ONLY_PROTOCOL *This,
  SP_U8 *RawFaultStatus
  )
{
  STOCK_POWER_READ_CONTEXT Context;
  SP_POWER_STATUS Status;
  EFI_STATUS EfiStatus;

  if ((This != &mReadOnlyProtocol) || (RawFaultStatus == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  EfiStatus = GetReadContext(STOCK_POWER_HUSB311_ADDRESS, &Context);
  if (EFI_ERROR(EfiStatus)) {
    return EfiStatus;
  }
  EfiStatus = ValidateHusb311Identity(&Context);
  if (EFI_ERROR(EfiStatus)) {
    return EfiStatus;
  }
  Status = SpReadHusb311FaultStatus(ReadRegisters, &Context, RawFaultStatus);
  return MapCoreStatus(Status, &Context);
}

STATIC STOCK_POWER_READ_ONLY_PROTOCOL mReadOnlyProtocol = {
  STOCK_POWER_READ_ONLY_PROTOCOL_REVISION,
  GetSc8989xIdentity,
  GetSc8989xStatus,
  DecodeSc8989xAdc,
  GetHusb311Identity,
  GetHusb311PowerStatus,
  GetHusb311CcStatus,
  GetHusb311Alerts,
  GetHusb311FaultStatus,
  GetSc8989xBoostState,
  GetSc8989xChargeState
};

EFI_STATUS
EFIAPI
StockPowerReadOnlyEntryPoint(
  EFI_HANDLE ImageHandle,
  EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;

  (VOID)ImageHandle;
  (VOID)SystemTable;
  Status = gBS->InstallProtocolInterface(
    &mServiceHandle,
    &gStockPowerReadOnlyProtocolGuid,
    EFI_NATIVE_INTERFACE,
    &mReadOnlyProtocol
    );
  return Status;
}
