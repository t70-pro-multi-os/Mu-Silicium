// SPDX-License-Identifier: GPL-2.0
/* Compile-only vermagic/modversion probe. Never loaded by these tools. */
#include <linux/init.h>
#include <linux/module.h>
static int __init t70_gki_abi_probe_init(void) { return 0; }
static void __exit t70_gki_abi_probe_exit(void) { }
module_init(t70_gki_abi_probe_init);
module_exit(t70_gki_abi_probe_exit);
MODULE_LICENSE("GPL");
