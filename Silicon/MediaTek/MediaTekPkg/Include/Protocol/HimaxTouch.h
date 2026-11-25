// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef HIMAX_TOUCH_H
#define HIMAX_TOUCH_H
#include <Protocol/MtkGpio.h>
/* Board producer must attest running normal-report firmware and exclusive
 * bus/pin ownership, and supply the verified IRQ readiness gate. The DXE reads
 * and checks the HX83132 ID itself before exposing AbsolutePointer. */
#define HIMAX_TOUCH_REVISION 3
#define HIMAX_TOUCH_FIRMWARE_RUNNING 2
#define HIMAX_TOUCH_EXCLUSIVE 4
#define HIMAX_TOUCH_REQUIRED_FLAGS (HIMAX_TOUCH_FIRMWARE_RUNNING|HIMAX_TOUCH_EXCLUSIVE)
// State comes from the input driver after it has claimed the I2C master.
#define HIMAX_TOUCH_STOPPED 0
#define HIMAX_TOUCH_STARTING 1
#define HIMAX_TOUCH_REPORT_VALID 2
#define HIMAX_TOUCH_REPORT_INVALID 3
#define HIMAX_TOUCH_READY 4
typedef struct {
  UINT32 Revision, Flags;
  UINT32 Address, Slots, XMax, YMax;
  VOID *Context;
  BOOLEAN (EFIAPI *Ready)(VOID *Context);
  BOOLEAN (EFIAPI *Pending)(VOID *Context);
  VOID (EFIAPI *State)(VOID *Context, UINT32 State);
  /* Recheck board resources and the GPIO152 snapshot while the caller owns
   * this exact lease. Do not acquire a second, temporary lease. */
  BOOLEAN (EFIAPI *ValidateGpioLease)(VOID *Context,
                                     MTK_GPIO_PROTOCOL *Gpio,
                                     UINT64 Token);
} HIMAX_TOUCH_BOARD;
/* Host polling/transfer barrier for the display controller sharing GPIO152.
 * Quiesce cancels polling, invalidates the OS handoff and releases the reset
 * pin. It keeps the I2C driver claim; it does not issue sense-off or power off.
 * Token is returned before a fallible cancellation/release, including errors.
 * Retry Quiesce with the same Agent to finish; do not reset pins until success.
 * Resume requires the token, live board resources, matching chip ID and
 * running firmware. A pending report must pass its checksum. It never resets,
 * programs firmware or blindly replays sense-on after power loss. EFI_NOT_READY means the caller may poll
 * Resume again with the same token. Failed resume retains stopped ownership.
 * This is a boot-services protocol; TPL <= CALLBACK. Stop is denied while a
 * token is held. Publication itself performs no touch I/O. */
#define HIMAX_TOUCH_CONTROL_REVISION 2U
typedef struct _HIMAX_TOUCH_CONTROL HIMAX_TOUCH_CONTROL;
typedef EFI_STATUS (EFIAPI *HIMAX_TOUCH_QUIESCE)(
  HIMAX_TOUCH_CONTROL *This, EFI_HANDLE Agent, UINT64 *Token);
typedef EFI_STATUS (EFIAPI *HIMAX_TOUCH_RESUME)(
  HIMAX_TOUCH_CONTROL *This, UINT64 Token);
struct _HIMAX_TOUCH_CONTROL {
  UINT32 Revision;
  HIMAX_TOUCH_QUIESCE Quiesce;
  HIMAX_TOUCH_RESUME Resume;
};
extern EFI_GUID gHimaxTouchControlProtocolGuid;
extern EFI_GUID gHimaxTouchBoardProtocolGuid;
#endif
