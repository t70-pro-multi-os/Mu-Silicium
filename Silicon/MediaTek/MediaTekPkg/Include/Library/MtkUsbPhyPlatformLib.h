// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MTK_USB_PHY_PLATFORM_LIB_H
#define MTK_USB_PHY_PLATFORM_LIB_H

#include <Uefi.h>
#include <Protocol/MtkClock.h>
#include <Library/MtkUsbPhyCore.h>

/*
 * Injected read-only NVMEM access for the DT-selected TPHY calibration cell.
 * The provider must be verified by the caller. Raw calibration values must not
 * be logged, exported, or read during host-side analysis.
 */
typedef MTK_USB_PHY_STATUS
(*MTK_USB_PHY_PLATFORM_NVMEM_READ) (
  IN VOID *Context,
  IN CONST CHAR8 *Name,
  IN UINT32 Mask,
  OUT UINT32 *RawValue
  );

/*
 * Validate the T70 DT, mapped PHY region, and fixed reference clock, then
 * return callbacks/configuration for MtkUsbPhyCore. NULL NVMEM selects the
 * stock /chosen/atag,devinfo provider, restricted to the verified runtime boot
 * FDT copy. Missing or malformed calibration stops before clock/GCD/PHY access. Bind does not initialize the core, start hardware, select role,
 * publish readiness, or configure Type-C/VBUS.
 */
EFI_STATUS
MtkUsbPhyPlatformBind (
  IN CONST VOID *Fdt,
  IN UINTN FdtBytes,
  IN MTK_CLOCK_PROTOCOL *Clock,
  IN MTK_USB_PHY_PLATFORM_NVMEM_READ ReadNvmem,
  IN VOID *NvmemContext,
  OUT MTK_USB_PHY_IO *Io,
  OUT MTK_USB_PHY_CONFIG *Config,
  OUT MTK_USB_PHY_HOST_TUNING *HostTuning
  );

/*
 * Clear the provider context only after successful MtkUsbPhyCoreExit. This
 * function never changes hardware and refuses to discard owned clock state.
 */
EFI_STATUS
MtkUsbPhyPlatformUnbind (
  VOID
  );

#endif
