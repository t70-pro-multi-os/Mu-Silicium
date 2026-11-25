// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_LK_BOOT_MODE_H
#define T70_LK_BOOT_MODE_H
// Original G100 LK SHA256 05e0155f...33b4: tag at 0xe6e8.
// At 0x103c0 a nonempty A/B suffix makes recovery use the ordinary boot kernel.
#define T70_ATAG_BOOT_SIZE 0x10
#define T70_ATAG_BOOT_MAGIC 0x41000802
#define T70_LK_NORMAL_MODE 0
#define T70_LK_RECOVERY_MODE 2
#define T70_LK_FACTORY_MODE 4
// Original LK keeps this mode when fastboot continue signals the Linux loader.
#define T70_LK_FASTBOOT_MODE 99
// T70 board keypad mapping, also read by KeypadDeviceLib.
#define T70_LK_KPD_BASE 0x10010000
#define T70_LK_KPD_KEYS 0x04
#define T70_LK_KPD_ENABLE 0x24
#define T70_LK_KPD_VOLUME_MASK 3
#define T70_ATAG_BOOT_TRAILER 2
#endif
