/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_I2C_EXCLUSIVE_H
#define MTK_I2C_EXCLUSIVE_H

#include <Uefi.h>
#include <Protocol/I2cMaster.h>

#define MTK_I2C_EXCLUSIVE_PROTOCOL_REVISION 2U

// Transfer is synchronous and uses the controller's polled path. A registered
// observer runs once at ExitBootServices before this controller is quiesced.
typedef struct _MTK_I2C_EXCLUSIVE_PROTOCOL MTK_I2C_EXCLUSIVE_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *MTK_I2C_EXCLUSIVE_ACQUIRE) (
  IN  MTK_I2C_EXCLUSIVE_PROTOCOL  *This,
  OUT UINT64                      *Token
  );

typedef
EFI_STATUS
(EFIAPI *MTK_I2C_EXCLUSIVE_TRANSFER) (
  IN  MTK_I2C_EXCLUSIVE_PROTOCOL  *This,
  IN  UINT64                      Token,
  IN  UINTN                       SlaveAddress,
  IN  EFI_I2C_REQUEST_PACKET      *Packet,
  OUT EFI_STATUS                  *I2cStatus OPTIONAL
  );

typedef
EFI_STATUS
(EFIAPI *MTK_I2C_EXCLUSIVE_RELEASE) (
  IN  MTK_I2C_EXCLUSIVE_PROTOCOL  *This,
  IN  UINT64                      Token
  );

typedef
EFI_STATUS
(EFIAPI *MTK_I2C_EXCLUSIVE_REGISTER_EXIT_OBSERVER) (
  IN  MTK_I2C_EXCLUSIVE_PROTOCOL  *This,
  IN  EFI_EVENT_NOTIFY            Observer,
  IN  VOID                        *Context
  );

typedef
EFI_STATUS
(EFIAPI *MTK_I2C_EXCLUSIVE_UNREGISTER_EXIT_OBSERVER) (
  IN  MTK_I2C_EXCLUSIVE_PROTOCOL  *This,
  IN  EFI_EVENT_NOTIFY            Observer,
  IN  VOID                        *Context
  );

typedef
EFI_STATUS
(EFIAPI *MTK_I2C_EXCLUSIVE_TRANSFER_TIMED) (
  IN MTK_I2C_EXCLUSIVE_PROTOCOL *This,
  IN UINT64                     Token,
  IN UINTN                      SlaveAddress,
  IN EFI_I2C_REQUEST_PACKET    *Packet,
  IN UINT32                     TimeoutUs
  );

// TransferTimed accepts one write/read transaction and bounds polling and
// fault recovery. Its timeout must accommodate the configured bus speed.
struct _MTK_I2C_EXCLUSIVE_PROTOCOL {
  UINT32                                      Revision;
  MTK_I2C_EXCLUSIVE_ACQUIRE                  Acquire;
  MTK_I2C_EXCLUSIVE_TRANSFER                 Transfer;
  MTK_I2C_EXCLUSIVE_RELEASE                  Release;
  MTK_I2C_EXCLUSIVE_REGISTER_EXIT_OBSERVER   RegisterExitObserver;
  MTK_I2C_EXCLUSIVE_UNREGISTER_EXIT_OBSERVER UnregisterExitObserver;
  MTK_I2C_EXCLUSIVE_TRANSFER_TIMED           TransferTimed;
};

extern EFI_GUID gMediaTekI2cExclusiveProtocolGuid;

#endif
