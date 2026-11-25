/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef MTK_DSI_HOST_H
#define MTK_DSI_HOST_H
#include <Library/MtkDsiCore.h>
#define MTK_DSI_HOST_REVISION 1U
typedef struct _MTK_DSI_HOST_PROTOCOL MTK_DSI_HOST_PROTOCOL;
typedef EFI_STATUS (EFIAPI *MTK_DSI_ACQUIRE)(
  MTK_DSI_HOST_PROTOCOL *This, EFI_HANDLE Agent, UINT64 *Token);
typedef EFI_STATUS (EFIAPI *MTK_DSI_CONFIGURE)(
  MTK_DSI_HOST_PROTOCOL *This, UINT64 Token, CONST MTK_DSI_OP7_PROFILE *Profile);
typedef EFI_STATUS (EFIAPI *MTK_DSI_TRANSITION)(
  MTK_DSI_HOST_PROTOCOL *This, UINT64 Token);
typedef EFI_STATUS (EFIAPI *MTK_DSI_QUERY)(
  MTK_DSI_HOST_PROTOCOL *This, MTK_DSI_CORE_STATE *State);
struct _MTK_DSI_HOST_PROTOCOL {
  UINT32 Revision;
  MTK_DSI_ACQUIRE Acquire;
  MTK_DSI_CONFIGURE Configure;
  MTK_DSI_TRANSITION StartVideo;
  MTK_DSI_TRANSITION Suspend;
  MTK_DSI_TRANSITION Resume;
  MTK_DSI_TRANSITION Release;
  MTK_DSI_QUERY QueryState;
};
/* Publication is passive. Acquire serializes this provider's clients but does
 * not prepare the panel or acquire domain/clock/plane ownership for the caller.
 * The caller must quiesce touch, prepare the panel, retain DISP/DSI clocks and
 * program the display route/plane before StartVideo, then enable the panel.
 * Configure accepts only a stopped/idle host and the exact along_cyx profile.
 * Suspend stops video, waits at most one second, saves context and stops D-PHY.
 * It does not switch off the panel or DISP domain. Resume restores D-PHY and
 * leaves video stopped; StartVideo must follow only after route/plane restore.
 * Release succeeds only before Configure or after successful Suspend. It
 * preserves saved context for the next owner. Failures with possible writes
 * quarantine the host and retain its token; no blind release/replay is allowed.
 * Every hardware operation validates the current runtime DT, GCD mappings,
 * DISP power/bus protection and required running clocks. It never powers up a
 * dead domain. Calls require TPL <= CALLBACK and active boot services.
 * No OS runtime/PRM provider, cold panel controller or visible frame is implied.
 */
extern EFI_GUID gMtkDsiHostProtocolGuid;
#endif
