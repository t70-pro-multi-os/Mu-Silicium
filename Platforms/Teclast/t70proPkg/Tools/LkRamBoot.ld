/* SPDX-License-Identifier: BSD-2-Clause-Patent */
OUTPUT_ARCH(aarch64)
ENTRY(register_ram_boot)
SECTIONS {
  . = 0x400;
  .text : { *(.text) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(patch_end <= 0x800, "RAM patch exceeds the verified LK alignment gap")
lk_register = 0x7750;
lk_get_lock_state = 0x84184;
lk_malloc = 0x6c68c;
lk_memcpy = 0x6d9cc;
lk_continue = 0x8d58;
lk_fail = 0x7acc;
download_buffer = 0xc8138;
lk_source_resume = 0x5b5e8;
