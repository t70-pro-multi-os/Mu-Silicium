// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_PMIC_RTC_RUNTIME_PROTOCOL_H
#define MTK_PMIC_RTC_RUNTIME_PROTOCOL_H

#include <Uefi.h>

// The library validates and converts the PWRAP mapping before calling
// into the same runtime image that owns the PRM WACS2 lock.
typedef EFI_STATUS (EFIAPI *MTK_PMIC_RTC_ACCESS)(
  IN UINTN PwrapBase,
  IN BOOLEAN SetTime,
  IN OUT EFI_TIME *Time
  );

typedef struct {
  MTK_PMIC_RTC_ACCESS Access;
} MTK_PMIC_RTC_RUNTIME_PROTOCOL;

extern EFI_GUID gMtkPmicRtcRuntimeProtocolGuid;

#endif
