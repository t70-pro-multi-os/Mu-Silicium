// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_USB_TYPEC_DIAGNOSTICS_H
#define T70_USB_TYPEC_DIAGNOSTICS_H

#include <Uefi.h>
#include <Library/MtkHusb311TypecCore.h>

#define T70_USB_TYPEC_DIAGNOSTICS_GUID \
  { 0xfa918184, 0x5b01, 0x4a1f, { 0xa6, 0xb6, 0x7d, 0xc8, 0xeb, 0xa3, 0x15, 0x29 } }

typedef EFI_STATUS (EFIAPI *T70_USB_TYPEC_READ)(
  MTK_HUSB311_TYPEC_DIAGNOSTIC *Failure,
  UINT8                       *GuardStep
  );

typedef struct {
  T70_USB_TYPEC_READ Read;
} T70_USB_TYPEC_DIAGNOSTICS;

#endif
