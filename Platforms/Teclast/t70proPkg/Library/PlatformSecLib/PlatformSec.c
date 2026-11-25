// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Library/IoLib.h>
#include <Library/PlatformSecLib.h>
#include "RamBootWatchdog.h"

VOID
PlatformInitialize (VOID)
{
  UINT32 Mode;
  UINT32 Length;

  // Only the interactive trial BootShim can request diagnostic recording. The normal shim clears
  // the whole record; require the normal handoff and completed copy as well.
  if ((MmioRead64 (T70_HANDOFF_BASE) == T70_HANDOFF_MAGIC) &&
      (MmioRead64 (T70_HANDOFF_BASE + 0x38) == 2) &&
      (MmioRead64 (T70_TRIAL_RECORD) == T70_TRIAL_MAGIC) &&
      (MmioRead64 (T70_TRIAL_RECORD + 8) == T70_TRIAL_INVERSE)) {
    Mode = MmioRead32 (T70_WDT_BASE);
    MmioWrite32 (T70_WDT_BASE, T70_WDT_MODE_KEY | (Mode & 0x00FFFFFEU));
    Mode = MmioRead32 (T70_WDT_BASE);
    Length = MmioRead32 (T70_WDT_BASE + 4);
    MmioWrite32 (T70_TRIAL_RECORD + 16, Mode);
    MmioWrite32 (T70_TRIAL_RECORD + 20, Length);
    MmioWrite64 (T70_TRIAL_RECORD + 24,
      (Mode & 1U) == 0U ? 2 : 3);
    // Keep interactive setup running until a user-requested restart.
  } else {
    // Match stock mtk_wdt_stop: disable bit 0 without overwriting the
    // inherited interrupt/reset configuration in the other MODE bits.
    Mode = MmioRead32 (T70_WDT_BASE);
    MmioWrite32 (T70_WDT_BASE, T70_WDT_MODE_KEY | (Mode & 0xFFFFFFFEU));
  }
  // BootShim's reserved handoff page: stage 3 means SEC C entry was reached.
  MmioWrite64 (0x43F7F038, 3);
}
