// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_RAM_BOOT_WATCHDOG_H
#define T70_RAM_BOOT_WATCHDOG_H

// Shared C/assembly constants; the final 32 bytes of the reserved handoff page.
#define T70_HANDOFF_BASE          0x43F7F000
#define T70_HANDOFF_MAGIC         0x0000000148303754
#define T70_TRIAL_RECORD          0x43F7FFE0
#define T70_TRIAL_MAGIC           0x0000000157443754
#define T70_TRIAL_INVERSE         0xFFFFFFFEA8BBC8AB
#define T70_WDT_BASE              0x10007000
#define T70_WDT_MODE_KEY          0x22000000
#define T70_WDT_RELOAD            0x1971
#define T70_WDT_LENGTH_30S        0xF008
#define T70_WDT_MODE_TEST_MASK    0x4D
#define T70_WDT_SINGLE_RESET      0x05

#endif
