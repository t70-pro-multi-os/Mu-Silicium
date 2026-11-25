/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The stock ksysfs translation unit sees these as forward declarations.
 * Enabling KEXEC_CORE adds includes that expose their definitions without
 * changing their layout. Keep the original DWARF type expansion in this
 * translation unit only. See check-ksysfs-kabi.py for independent validation.
 */
#include <linux/android_kabi.h>
ANDROID_KABI_DECLONLY(module);
ANDROID_KABI_DECLONLY(compat_robust_list_head);
ANDROID_KABI_DECLONLY(iov_iter);
