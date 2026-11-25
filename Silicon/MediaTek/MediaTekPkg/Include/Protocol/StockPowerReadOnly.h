// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef STOCK_POWER_READ_ONLY_H
#define STOCK_POWER_READ_ONLY_H

#include <Uefi.h>
#include <Library/StockPowerCore.h>

#define STOCK_POWER_READ_ONLY_PROTOCOL_GUID \
  { 0x1fd8c634, 0x721b, 0x4e3b, { 0x9a, 0x74, 0x1b, 0x87, 0x2f, 0xd2, 0x05, 0x41 } }
#define STOCK_POWER_BOARD_READY_PROTOCOL_GUID \
  { 0x65b97cc0, 0x5e5e, 0x49ae, { 0x8b, 0x30, 0xb5, 0x7c, 0x3c, 0x91, 0xa0, 0x6e } }

#define STOCK_POWER_READ_ONLY_BOOST_REVISION 2U
#define STOCK_POWER_READ_ONLY_CHARGE_STATE_REVISION 3U
#define STOCK_POWER_READ_ONLY_PROTOCOL_REVISION STOCK_POWER_READ_ONLY_CHARGE_STATE_REVISION
#define STOCK_POWER_BOARD_READY_SIGNATURE SIGNATURE_32('S', 'P', 'B', 'R')
#define STOCK_POWER_BOARD_READY_REVISION  1U
#define STOCK_POWER_BOARD_READY_ALLOW_STATUS_READ  0x00000001U

// Verified 7-bit target addresses carried by the private board-ready marker.
#define STOCK_POWER_SC8989X_ADDRESS  0x6AU
#define STOCK_POWER_HUSB311_ADDRESS  0x4EU

extern EFI_GUID gStockPowerReadOnlyProtocolGuid;
extern EFI_GUID gStockPowerBoardReadyProtocolGuid;

/*
 * A trusted platform provider installs this private marker only after the
 * controller and these board targets are ready. Consumers never enumerate or
 * scan I2C addresses. The allow flag authorizes register-pointer/read
 * transactions only; it does not claim device reads are side-effect free.
 */
typedef struct {
  UINT32 Signature;
  UINT32 Revision;
  UINT32 Flags;
  EFI_HANDLE I2cMasterHandle;
  UINTN Sc8989xAddress;
  UINTN Husb311Address;
} STOCK_POWER_BOARD_READY_PROTOCOL;

typedef struct _STOCK_POWER_READ_ONLY_PROTOCOL STOCK_POWER_READ_ONLY_PROTOCOL;

struct _STOCK_POWER_READ_ONLY_PROTOCOL {
  UINT32 Revision;

  EFI_STATUS (EFIAPI *GetSc8989xIdentity)(
    STOCK_POWER_READ_ONLY_PROTOCOL *This,
    SP_SC8989X_IDENTITY *Identity
    );

  EFI_STATUS (EFIAPI *GetSc8989xStatus)(
    STOCK_POWER_READ_ONLY_PROTOCOL *This,
    SP_SC8989X_STATUS *Status
    );

  EFI_STATUS (EFIAPI *DecodeSc8989xAdc)(
    STOCK_POWER_READ_ONLY_PROTOCOL *This,
    SP_U8 Channel,
    SP_U32 RawValue,
    SP_U32 *ScaledValue,
    SP_ADC_UNIT *Unit
    );

  EFI_STATUS (EFIAPI *GetHusb311Identity)(
    STOCK_POWER_READ_ONLY_PROTOCOL *This,
    SP_HUSB311_IDENTITY *Identity
    );

  EFI_STATUS (EFIAPI *GetHusb311PowerStatus)(
    STOCK_POWER_READ_ONLY_PROTOCOL *This,
    SP_HUSB311_POWER_STATUS *Status
    );

  EFI_STATUS (EFIAPI *GetHusb311CcStatus)(
    STOCK_POWER_READ_ONLY_PROTOCOL *This,
    SP_HUSB311_CC_STATUS *Status
    );

  EFI_STATUS (EFIAPI *GetHusb311Alerts)(
    STOCK_POWER_READ_ONLY_PROTOCOL *This,
    SP_HUSB311_ALERT_STATUS *Status
    );

  EFI_STATUS (EFIAPI *GetHusb311FaultStatus)(
    STOCK_POWER_READ_ONLY_PROTOCOL *This,
    SP_U8 *RawFaultStatus
    );

  EFI_STATUS (EFIAPI *GetSc8989xBoostState)(
    STOCK_POWER_READ_ONLY_PROTOCOL *This,
    SP_SC8989X_BOOST_STATE *State
    );

  EFI_STATUS (EFIAPI *GetSc8989xChargeState)(
    STOCK_POWER_READ_ONLY_PROTOCOL *This,
    SP_SC8989X_CHARGE_STATE *State
    );
};

EFI_STATUS
EFIAPI
StockPowerReadOnlyEntryPoint(
  EFI_HANDLE ImageHandle,
  EFI_SYSTEM_TABLE *SystemTable
  );

#endif
