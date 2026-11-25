/* SPDX-License-Identifier: BSD-2-Clause-Patent */
OUTPUT_ARCH(aarch64)
ENTRY(bl2_patch_lk)
SECTIONS {
  . = 0x400;
  .text : { *(.text) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(bl2_patch_end <= 0x800, "BL2 RAM bridge exceeds the verified alignment gap")
bl2_original_clock = 0x8574;
