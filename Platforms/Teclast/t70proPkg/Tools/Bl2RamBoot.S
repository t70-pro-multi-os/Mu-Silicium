// SPDX-License-Identifier: BSD-2-Clause-Patent
// Run only after the shipping BL2_EXT loader has finished verifying/loading LK.
#ifndef LK_PATCH_FILE
#error LK_PATCH_FILE must name the tested LK RAM payload.
#endif
.text
.balign 4
.global bl2_patch_lk, bl2_patch_end
bl2_patch_lk:
  ldr x16, [sp, #8]           // Original loader's mapped LK buffer.
  stp x29, x30, [sp, #-16]!
  mov w17, wzr              // Diagnostic progress is kept in caller-saved state.
  // The original loader produces this mapped pointer from its allocation
  // descriptor through vmm_alloc_physical. This is a virtual address, so its
  // numeric lower bound is not the physical DRAM base. Validate the mapped
  // kernel region, alignment, and the shipping image bytes before any edit.
  tst x16, #0xfff
  b.ne bl2_return
  ldr x0, minimum_lk_address
  cmp x16, x0
  b.lo bl2_return
  ldr x0, maximum_lk_address
  cmp x16, x0
  b.hi bl2_return
  ldp x0, x1, [x16]
  adr x2, expected_lk_prefix
  ldp x3, x4, [x2]
  cmp x0, x3
  b.ne bl2_return
  cmp x1, x4
  b.ne bl2_return
  ldr w2, version_offset
  add x0, x16, x2
  adr x1, expected_version
  mov w2, #(expected_version_end-expected_version)
check_lk_version:
  ldrb w3, [x0], #1
  ldrb w4, [x1], #1
  cmp w3, w4
  b.ne bl2_return
  subs w2, w2, #1
  b.ne check_lk_version
  mov w17, #1
  mov w2, #0x8358
  ldr w0, [x16, x2]
  ldr w1, original_register_call
  cmp w0, w1
  b.ne bl2_return
  mov w17, #2
  mov w2, #0xb5e4
  movk w2, #5, lsl #16
  ldr w0, [x16, x2]
  ldr w1, original_source_load
  cmp w0, w1
  b.ne bl2_return
  mov w17, #3
  add x0, x16, #0x400
  mov w1, #0x100
check_lk_padding:
  ldr w2, [x0], #4
  cbnz w2, bl2_return
  subs w1, w1, #1
  b.ne check_lk_padding
  mov w17, #4
  adr x0, lk_payload
  add x1, x16, #0x400
  mov w2, #(lk_payload_end-lk_payload)/4
copy_lk_payload:
  ldr w3, [x0], #4
  str w3, [x1], #4
  subs w2, w2, #1
  b.ne copy_lk_payload
  mov w2, #0x8358
  ldr w0, replacement_register_call
  str w0, [x16, x2]
  mov w2, #0xb5e4
  movk w2, #5, lsl #16
  ldr w0, replacement_source_branch
  str w0, [x16, x2]
  add x0, x16, #0x400
  mov w1, #(lk_payload_end-lk_payload)
  bl sync_code
  mov w0, #0x8358
  add x0, x16, x0
  mov w1, #4
  bl sync_code
  mov w0, #0xb5e4
  movk w0, #5, lsl #16
  add x0, x16, x0
  mov w1, #4
  bl sync_code
  mov w17, #5
bl2_return:
  // Alter only the fingerprinted version's four trailing bytes in RAM.
  // The shipping fastboot version publisher makes this progress observable.
  cbz w17, bl2_restore
  ldr w2, version_tail_offset
  add x0, x16, x2
  mov w1, #0x4252            // RB0 + ASCII stage number, little endian.
  movk w1, #0x3030, lsl #16
  add w1, w1, w17, lsl #24
  str w1, [x0]
  mov w1, #4
  bl sync_code
bl2_restore:
  ldp x29, x30, [sp], #16
  b bl2_original_clock        // Preserve its return value and original caller LR.
sync_code:
  mrs x2, ctr_el0
  add x3, x0, x1
  ubfx x4, x2, #16, #4
  mov x6, #4
  lsl x4, x6, x4
  sub x7, x4, #1
  bic x5, x0, x7
1:
  dc cvac, x5
  add x5, x5, x4
  cmp x5, x3
  b.lo 1b
  dsb sy
  and x4, x2, #15
  lsl x4, x6, x4
  sub x7, x4, #1
  bic x5, x0, x7
2:
  ic ivau, x5
  add x5, x5, x4
  cmp x5, x3
  b.lo 2b
  dsb sy
  isb
  ret
.balign 8
minimum_lk_address:
  .quad 0xffff000000000000
maximum_lk_address:
  .quad 0xfffffffffff00000
expected_lk_prefix:
  .word LK_PREFIX0, LK_PREFIX1, LK_PREFIX2, LK_PREFIX3
original_register_call:
  .word LK_REGISTER_ORIGINAL
original_source_load:
  .word 0xf940051b
replacement_register_call:
  .word LK_REGISTER_REPLACEMENT
replacement_source_branch:
  .word LK_SOURCE_REPLACEMENT
version_offset:
  .word LK_VERSION_OFFSET
version_tail_offset:
  .word LK_VERSION_TAIL_OFFSET
expected_version:
  .asciz "11ee062dcfa_202607030435"
expected_version_end:
.balign 4
lk_payload:
  .incbin LK_PATCH_FILE
lk_payload_end:
.balign 4
bl2_patch_end:
