// SPDX-License-Identifier: BSD-2-Clause-Patent
// Resume the original boot chain; change only volatile, fingerprinted code.
#ifndef PRELOADER_FILE
#error PRELOADER_FILE is required.
#endif
#ifndef BL2_PATCH_FILE
#error BL2_PATCH_FILE is required.
#endif
.syntax unified
.arch armv7-a
.arm
.text
.global _start, bl2_entry_patch, stage_end
_start:
  push {r4-r12, lr}
  ldr r0, =0x401ffff4
  cmp r4, r0
  bne return_loop
  cmp r5, #12
  bne return_loop
  ldr r0, =0x2091cb
  cmp lr, r0
  bne return_loop
  mrc p15, 0, r0, c1, c0, 0
  ldr r1, =0x1005             // Require MMU, D-cache and I-cache off here.
  tst r0, r1
  bne return_loop
  mrs r0, cpsr
  and r0, r0, #0x3f
  cmp r0, #0x13
  bne return_loop
  ldr r6, =fingerprints
  mov r8, #10
verify_window:
  ldmia r6!, {r0,r1}
verify_word:
  ldr r2, [r0], #4
  ldr r3, [r6], #4
  cmp r2, r3
  bne return_loop
  subs r1, r1, #1
  bne verify_word
  subs r8, r8, #1
  bne verify_window
  ldr r0, [sp, #0x19c]       // Saved USB loop caller LR, through the real frame.
  ldr r1, =0x224e5d
  cmp r0, r1
  bne return_loop
  ldr r7, =0x2f35a8
  ldr r7, [r7]
  bl check_pointer
  ldr r7, [r7, #8]
  bl check_pointer
  ldr r8, [r7, #4]          // Shipping USB receive callback.
  ldr r7, [r7]              // Shipping USB transmit callback.
  bl check_callback
  mov r9, r7
  mov r7, r8
  bl check_callback
  mov r7, r9
  // Send the marker before changing original code. The host must receive it
  // and send the exact resume token before USB shutdown can lose the packet.
  ldr r6, =record
#ifdef T70_RETAINED_PROBE
  mov r0, #3
#else
  mov r0, #3
#endif
  str r0, [r6, #8]
  ldr r0, =_start
  str r0, [r6, #12]
  mrs r0, cpsr
  str r0, [r6, #16]
  mrc p15, 0, r0, c1, c0, 0
  str r0, [r6, #20]
  mrc p15, 0, r0, c0, c0, 5
  str r0, [r6, #24]
  str r4, [r6, #28]
  str r5, [r6, #32]
  ldr r0, [sp, #36]
  str r0, [r6, #36]
#ifdef T70_RETAINED_PROBE
  b retained_probe
#else
  // Mirror the shipping name lookup without calling its unbounded strlen.
  // Only the selected firmware allocation's addresses and match flags leave RAM.
  ldr r9, =0x2f35ac
  ldr r9, [r9]
  tst r9, #3
  bne allocation_done
  ldr r0, =0x200000
  cmp r9, r0
  blo allocation_done
  ldr r0, =0x27e000          // Complete 0x81900-byte shipping allocation table.
  cmp r9, r0
  bls allocation_table
  ldr r0, =0x40000000
  cmp r9, r0
  blo allocation_done
  ldr r0, =0xfff7e000
  cmp r9, r0
  bhi allocation_done
allocation_table:
  mov r0, #1
  str r0, [r6, #40]
  ldr r0, =0x7cd00
  add r9, r9, r0
  mov r10, #128
allocation_next:
  ldmia r9, {r0,r1}
  orrs r0, r0, r1
  beq allocation_skip
  add r11, r9, #0x14
  ldr r2, =bl2_allocation_name
  mov r3, #15
allocation_name:
  ldrb r0, [r11], #1
  ldrb r1, [r2], #1
  cmp r0, r1
  bne allocation_skip
  subs r3, r3, #1
  bne allocation_name
  mov r0, #2
  str r0, [r6, #40]
  str r9, [r6, #60]
  ldmia r9, {r0,r1,r2}
  str r0, [r6, #44]
  str r1, [r6, #48]
  str r2, [r6, #52]
  cmp r1, #0
  bne allocation_done
  ldr r3, =645992
  cmp r2, r3
  blo allocation_done
  ubfx r1, r0, #0, #12
  cmp r1, #0
  bne allocation_done
  ldr r1, =0x40000000
  cmp r0, r1
  blo allocation_done
  ldr r1, =0xfff00000
  cmp r0, r1
  bhi allocation_done
  mov r9, r0
  mov r0, #3
  str r0, [r6, #40]
  ldr r11, =expected_bl2_prefix
  mov r10, #4
allocation_prefix:
  ldr r0, [r9], #4
  ldr r1, [r11], #4
  cmp r0, r1
  bne allocation_done
  subs r10, r10, #1
  bne allocation_prefix
  mov r0, #4
  str r0, [r6, #40]
  mov r0, #1
  str r0, [r6, #56]
  b allocation_done
allocation_skip:
  add r9, r9, #0x98
  subs r10, r10, #1
  bne allocation_next
allocation_done:
  mov r0, r6
  mov r1, #64
  blx r7
  ldr r0, =resume_token
  mov r1, #8
  mov r2, #0                // Same blocking receive mode as the stock parser.
  blx r8
  ldr r6, =mutation_record
  str r0, [r6, #32]        // Callback status, not an assumed byte-count contract.
  // The stock blocking receive path consumes bytes, not a required zero return.
  ldr r0, =resume_token
  ldmia r0, {r1,r2}
  ldr r3, =0x47303754       // T70G
  cmp r1, r3
  bne return_loop
  ldr r3, =0x2132334f       // O32!
  cmp r2, r3
  bne return_loop
  mov r0, #1
  str r0, [r6, #36]        // Exact host resume token was received.
  // The physical diagnostic showed BL2 can already be present at this point.
  // Patch that verified RAM image now; keep RVBAR as a later-load fallback.
  ldr r6, =record
  ldr r0, [r6, #40]
  cmp r0, #4
  bne arm_later_load
  ldr r0, [r6, #44]
  bl patch_bl2_now
arm_later_load:
  // All guards and the host receipt finish before any original code/frame edit.
  ldr r0, =rvbar_hook
  ldr r1, =0x230f44
  ldmia r0, {r2,r3,r6}
  stmia r1, {r2,r3,r6}
  dsb sy
  isb
  ldr r0, =rvbar_hook
  ldmia r0, {r2,r3,r6}
  ldmia r1, {r9,r10,r11}
  cmp r2, r9
  cmpeq r3, r10
  cmpeq r6, r11
  moveq r2, #1
  movne r2, #0
  ldr r6, =mutation_record
  str r2, [r6, #28]
  mov r0, r6
  mov r1, #40
  blx r7
  // Let the host drain the readback before the shipping USB shutdown. This is
  // a receipt only; the exact GO token already authorized the RAM edits above.
  ldr r0, =final_receipt
  mov r1, #8
  mov r2, #0
  blx r8
#endif
resume_stock_chain:
  // Unwind the actual parser and USB-listener frames via their stock epilogues.
  ldr r0, =0x224f2f
  str r0, [sp, #0x19c]
  pop {r4-r12, lr}
  ldr r0, =0x2093d1
  bx r0
return_loop:
  pop {r4-r12, lr}
  add r5, sp, #0x10
  bx lr
#ifdef T70_RETAINED_PROBE
// Read only our two fixed boot records. Unrecognized RAM is never exported.
// The full normal-bridge entry guards above also protect the frame unwind.
retained_probe:
  ldr r9, =0x43f7ffe0
  ldmia r9, {r0-r3}
  ldr r10, =0x57443754
  cmp r0, r10
  cmpeq r1, #1
  ldreq r10, =0xa8bbc8ab
  cmpeq r2, r10
  mvneq r10, #1
  cmpeq r3, r10
  bne retained_handoff
  ldr r0, [r9, #24]
  ldr r1, [r9, #28]
  cmp r1, #0
  bne retained_handoff
  cmp r0, #1
  blo retained_handoff
  cmp r0, #3
  bhi retained_handoff
  mov r0, #1
  str r0, [r6, #40]
  add r10, r6, #48
  mov r11, #8
retained_trial_copy:
  ldr r0, [r9], #4
  str r0, [r10], #4
  subs r11, r11, #1
  bne retained_trial_copy
retained_handoff:
  ldr r9, =0x43f7f000
  ldmia r9, {r0,r1}
  ldr r10, =0x48303754
  cmp r0, r10
  cmpeq r1, #1
  bne retained_send
  ldr r0, [r9, #24]
  ldr r1, [r9, #28]
  cmp r0, #0x40000000
  cmpeq r1, #0
  bne retained_send
  ldr r0, [r9, #56]
  ldr r1, [r9, #60]
  cmp r1, #0
  bne retained_send
  cmp r0, #1
  blo retained_send
  cmp r0, #4
  bhi retained_send
  mov r1, #1
  str r1, [r6, #44]
  add r10, r6, #80
  cmp r0, #4
  moveq r11, #24
  movne r11, #16
retained_handoff_copy:
  ldr r0, [r9], #4
  str r0, [r10], #4
  subs r11, r11, #1
  bne retained_handoff_copy
  // Only a recognized own handoff permits the adjacent GOP record read.
  ldr r9, =0x43f7f060
  ldmia r9, {r0,r1}
  ldr r10, =0x47503754
  cmp r0, r10
  cmpeq r1, #1
  bne retained_sec_snapshot
  ldr r0, [r9, #8]
  cmp r0, #1
  blo retained_sec_snapshot
  cmp r0, #8
  bhi retained_sec_snapshot
  ldr r0, [r9, #12]
  cmp r0, #0
  beq retained_sec_snapshot
  add r10, r6, #176
  mov r11, #8
retained_gop_copy:
  ldr r0, [r9], #4
  str r0, [r10], #4
  subs r11, r11, #1
  bne retained_gop_copy
  mov r0, #1
  str r0, [r6, #216]
  // Export only our recognized, fixed-size display register snapshot.
  ldr r9, =0x43f7f800
  ldmia r9, {r0,r1}
  ldr r10, =0x4c443754
  cmp r0, r10
  bne retained_send
  cmp r1, #7
  beq retained_bds_launch_snapshot
  cmp r1, #6
  beq retained_bds_snapshot
  cmp r1, #3
  beq retained_overlap_snapshot
  cmp r1, #2
  beq retained_dsi_snapshot
  cmp r1, #1
  bne retained_send
  ldr r0, [r9, #8]
  ldr r1, =0x14005000
  cmp r0, r1
  moveq r2, #4
  beq retained_snapshot_layer
  add r1, r1, #0x1000
  cmp r0, r1
  bne retained_send
  mov r2, #2
retained_snapshot_layer:
  ldr r0, [r9, #12]
  cmp r0, r2
  bhs retained_send
  mov r1, #1
  lsl r1, r1, r0
  ldr r2, [r9, #28]
  cmp r1, r2
  bne retained_send
  b retained_snapshot_ready
retained_dsi_snapshot:
  // Version 2 contains only twelve known DSI controls and two zero words.
  ldr r0, [r9, #8]
  ldr r1, =0x14013000
  cmp r0, r1
  bne retained_send
  ldr r0, [r9, #12]
  ldr r1, [r9, #64]
  ldr r2, [r9, #68]
  orr r0, r0, r1
  orrs r0, r0, r2
  bne retained_send
  b retained_snapshot_ready
retained_overlap_snapshot:
  ldr r0, [r9, #8]
  ldr r1, =0x43f7f080
  cmp r0, r1
  bne retained_send
  ldr r0, [r9, #12]
  ldr r1, [r9, #56]
  ldr r2, [r9, #60]
  orr r0, r0, r1
  orr r0, r0, r2
  ldr r1, [r9, #64]
  ldr r2, [r9, #68]
  orr r0, r0, r1
  orrs r0, r0, r2
  bne retained_send
  b retained_snapshot_ready
retained_bds_launch_snapshot:
  // v7 adds only a bounded launch phase and a compact EFI status.
  ldr r0, [r9, #64]
  cmp r0, #8
  bhi retained_send
  ldr r0, [r9, #68]
  cmp r0, #0x20000
  bhs retained_send
  b retained_bds_common
retained_bds_snapshot:
  ldr r0, [r9, #64]
  ldr r1, [r9, #68]
  orrs r0, r0, r1
  bne retained_send
retained_bds_common:
  ldr r0, [r6, #184]
  cmp r0, #8
  bne retained_send
  ldr r0, [r6, #192]
  ldr r1, [r6, #196]
  orrs r0, r0, r1
  bne retained_send
  // Board BDS progress: fixed protocol counts/booleans, never input contents.
  ldr r0, [r9, #8]
  ldr r1, =0x43f7f060
  cmp r0, r1
  bne retained_send
  ldr r0, [r9, #16]
  cmp r0, #1
  blo retained_send
  cmp r0, #11
  bhi retained_send
  ldr r0, [r9, #12]
  cmp r0, #0
  bne retained_send
  b retained_snapshot_ready
retained_sec_snapshot:
  // SEC owns this fixed record only after the completed C-entry handoff.
  ldr r0, [r6, #136]
  cmp r0, #3
  blo retained_send
  ldr r9, =0x43f7f800
  ldmia r9, {r0,r1}
  ldr r10, =0x4c443754
  cmp r0, r10
  bne retained_send
  cmp r1, #5
  beq retained_sec_exception
  cmp r1, #4
  bne retained_send
  ldr r0, [r9, #8]
  ldr r1, =0x43f7f060
  cmp r0, r1
  bne retained_send
  ldr r0, [r9, #12]
  cmp r0, #0
  bne retained_send
  ldr r0, [r9, #16]
  cmp r0, #11
  bhi retained_send
  ldr r0, [r9, #20]
  cmp r0, #128
  bhs retained_send
  ldr r0, [r9, #48]
  cmp r0, #4
  bhi retained_send
  add r10, r9, #52
  mov r11, #5
retained_sec_tail:
  ldr r0, [r10], #4
  cmp r0, #0
  bne retained_send
  subs r11, r11, #1
  bne retained_sec_tail
  b retained_snapshot_ready
retained_sec_exception:
  ldr r0, [r9, #8]
  ldr r1, =0x43f7f060
  cmp r0, r1
  bne retained_send
  ldr r0, [r9, #12]
  cmp r0, #0
  bne retained_send
  ldr r0, [r9, #16]
  cmp r0, #4
  cmpne r0, #8
  bne retained_send
  ldr r0, [r9, #20]
  cmp r0, #3
  bhi retained_send
  ldr r0, [r9, #64]
  ldr r1, [r9, #68]
  orrs r0, r0, r1
  bne retained_send
retained_snapshot_ready:
  add r10, r6, #224
  mov r11, #18
retained_snapshot_copy:
  ldr r0, [r9], #4
  str r0, [r10], #4
  subs r11, r11, #1
  bne retained_snapshot_copy
  ldr r9, =0x43f7f800
  mov r0, #0
  str r0, [r9]
  str r0, [r9, #4]
  dsb sy
  ldmia r9, {r0,r1}
  orrs r0, r0, r1
  moveq r0, #1
  movne r0, #2
  str r0, [r6, #220]
retained_send:
  // Remove only a recognized old trial cookie, after copying its bounded
  // fields. The next trial cannot inherit a positive result from this boot.
  ldr r0, [r6, #40]
  cmp r0, #1
  bne retained_clear_handoff
  ldr r9, =0x43f7ffe0
  mov r0, #0
  str r0, [r9]
  str r0, [r9, #4]
  str r0, [r9, #8]
  str r0, [r9, #12]
  dsb sy
  ldmia r9, {r0-r3}
  orrs r0, r0, r1
  orrs r0, r0, r2
  orrs r0, r0, r3
  moveq r0, #1
  movne r0, #2
  str r0, [r6, #208]       // 1: old cookie cleared; 2: clear did not match.
retained_clear_handoff:
  ldr r0, [r6, #44]
  cmp r0, #1
  bne retained_clear_gop
  ldr r9, =0x43f7f000
  mov r0, #0
  str r0, [r9]
  str r0, [r9, #4]
  dsb sy
  ldmia r9, {r0,r1}
  orrs r0, r0, r1
  moveq r0, #1
  movne r0, #2
  str r0, [r6, #212]
retained_clear_gop:
  ldr r0, [r6, #216]
  cmp r0, #1
  bne retained_transmit
  ldr r9, =0x43f7f060
  mov r0, #0
  str r0, [r9]
  str r0, [r9, #4]
  dsb sy
  ldmia r9, {r0,r1}
  orrs r0, r0, r1
  moveq r0, #1
  movne r0, #2
  str r0, [r6, #216]
retained_transmit:
  mov r0, r6
  mov r1, #296
  blx r7
  ldr r0, =final_receipt
  mov r1, #8
  mov r2, #0
  blx r8
  ldr r0, =final_receipt
  ldmia r0, {r1,r2}
  ldr r3, =0x52303754       // T70R
  cmp r1, r3
  ldreq r3, =0x21214e55     // UN!!
  cmpeq r2, r3
  bne return_loop
  b resume_stock_chain
#endif
check_callback:
  tst r7, #1
  bne callback_bounds
  tst r7, #3
  bne return_loop
callback_bounds:
  bic r0, r7, #1
  ldr r1, =0x200f10
  cmp r0, r1
  blo return_loop
  ldr r1, =0x200f10 + 367036
  cmp r0, r1
  bhs return_loop
  bx lr
check_pointer:
  tst r7, #3
  bne return_loop
  ldr r0, =0x200000
  cmp r7, r0
  blo return_loop
  ldr r0, =0x2ffff4
  cmp r7, r0
  bxls lr
  ldr r0, =0x40000000
  cmp r7, r0
  blo return_loop
  ldr r0, =0x4ffffff4
  cmp r7, r0
  bhi return_loop
  bx lr

// Original preloader calls this after loading BL2_EXT, immediately before RVBAR.
bl2_entry_patch:
  push {r0-r12, lr}
  mov r12, #0
  push {r12}                // Callback must retain the original RVBAR write.
  b patch_bl2_common
patch_bl2_now:
  push {r0-r12, lr}
  mov r12, #1
  push {r12}                // Direct patch performs no RVBAR register write.
patch_bl2_common:
  ldr r12, =mutation_record
  mov r0, #1
  str r0, [r12, #12]
  mrc p15, 0, r0, c1, c0, 0
  tst r0, #1
  bne original_rvbar_write   // Physical RAM edits require MMU off.
  // Restore the original preloader instruction window even on a BL2 mismatch.
  ldr r0, =original_rvbar
  ldr r1, =0x230f44
  ldmia r0, {r2,r3,r4}
  stmia r1, {r2,r3,r4}
  ldr r4, [sp, #4]
  mov r0, #2
  str r0, [r12, #12]
  // RVBAR receives the actual BL2 allocation selected by the shipping loader.
  // Require a complete, page-aligned DRAM range before checking image bytes.
  ubfx r0, r4, #0, #12
  cmp r0, #0
  bne flush_preloader_code
  ldr r0, =0x40000000
  cmp r4, r0
  blo flush_preloader_code
  ldr r0, =0xfff00000
  cmp r4, r0
  bhi flush_preloader_code
  mov r0, #3
  str r0, [r12, #12]
  ldr r6, =expected_bl2_prefix
  mov r5, r4
  mov r8, #4
check_bl2_prefix:
  ldr r0, [r5], #4
  ldr r1, [r6], #4
  cmp r0, r1
  bne flush_preloader_code
  subs r8, r8, #1
  bne check_bl2_prefix
  mov r0, #4
  str r0, [r12, #12]
  ldr r5, =0x7850
  add r5, r4, r5
  ldr r0, [r5]
  str r0, [r12, #20]       // Selected shipping instruction only.
  ldr r1, =0x94000349
  cmp r0, r1
  bne flush_preloader_code
  mov r0, #5
  str r0, [r12, #12]
  add r5, r4, #0x400
  mov r8, #0x100
check_bl2_padding:
  ldr r0, [r5], #4
  cmp r0, #0
  bne bl2_padding_mismatch
  subs r8, r8, #1
  bne check_bl2_padding
  mov r0, #6
  str r0, [r12, #12]
  ldr r6, =bl2_payload
  add r5, r4, #0x400
  ldr r8, =(bl2_payload_end-bl2_payload)/4
copy_bl2_payload:
  ldr r0, [r6], #4
  str r0, [r5], #4
  subs r8, r8, #1
  bne copy_bl2_payload
  ldr r5, =0x7850
  add r5, r4, r5
  ldr r0, =BL2_HOOK_BRANCH
  str r0, [r5]
  add r0, r4, #0x400
  ldr r1, =(bl2_payload_end-bl2_payload)
  add r1, r0, r1
  bl clean_data_range
  ldr r0, =0x7850
  add r0, r4, r0
  add r1, r0, #4
  bl clean_data_range
  ldr r6, =bl2_payload
  add r5, r4, #0x400
  ldr r8, =(bl2_payload_end-bl2_payload)/4
verify_bl2_payload:
  ldr r0, [r6], #4
  ldr r1, [r5], #4
  cmp r0, r1
  bne bl2_readback_mismatch
  subs r8, r8, #1
  bne verify_bl2_payload
  ldr r5, =0x7850
  ldr r0, [r4, r5]
  ldr r1, =BL2_HOOK_BRANCH
  cmp r0, r1
  bne bl2_readback_mismatch
  mov r0, #7
  str r0, [r12, #12]
  mov r0, #1
  str r0, [r12, #24]
  b flush_preloader_code
bl2_padding_mismatch:
  sub r0, r5, r4
  sub r0, r0, #4
  str r0, [r12, #16]       // Offset only; never export the occupied word.
  b flush_preloader_code
bl2_readback_mismatch:
  mov r0, #8
  str r0, [r12, #12]
flush_preloader_code:
  ldr r0, =0x230f44
  add r1, r0, #12
  bl clean_data_range
  dsb sy
  mov r0, #0
  mcr p15, 0, r0, c7, c5, 0
original_rvbar_write:
  dsb sy
  isb
  pop {r0}
  cmp r0, #0
  bne direct_patch_return
  pop {r0-r12, lr}
  ldr r1, =0x0c53c900
  str r0, [r1]               // Exactly the original RVBAR write and value.
  bx lr
direct_patch_return:
  pop {r0-r12, pc}
clean_data_range:
  mrc p15, 0, r2, c0, c0, 1
  ubfx r2, r2, #16, #4
  mov r3, #4
  lsl r2, r3, r2
  sub r3, r2, #1
  bic r0, r0, r3
1:
  mcr p15, 0, r0, c7, c10, 1
  add r0, r0, r2
  cmp r0, r1
  blo 1b
  bx lr
.balign 4
rvbar_hook:
  .hword 0x4b01, 0x4718, 0xbf00, 0xbf00
  .word bl2_entry_patch
original_rvbar:
  .incbin PRELOADER_FILE, 0x230f44-0x200f10, 12
expected_bl2_prefix:
  .word BL2_PREFIX0, BL2_PREFIX1, BL2_PREFIX2, BL2_PREFIX3
record:
#ifdef T70_RETAINED_PROBE
  .ascii "T70DIA32"
  .space 288
#else
  .ascii "T70STG32"
  .space 56
#endif
bl2_allocation_name:
  .asciz "system_bl2-ext"
.balign 4
resume_token:
  .space 8
final_receipt:
  .space 8
mutation_record:
  .ascii "T70ARM32"
  .word 1
  .space 28
fingerprints:
  .word 0x201170, 3
  .incbin PRELOADER_FILE, 0x201170-0x200f10, 12
  .word 0x209178, 21
  .incbin PRELOADER_FILE, 0x209178-0x200f10, 84
  .word 0x208e44, 5
  .incbin PRELOADER_FILE, 0x208e44-0x200f10, 20
  .word 0x2093d0, 3
  .incbin PRELOADER_FILE, 0x2093d0-0x200f10, 12
  .word 0x224cd4, 3
  .incbin PRELOADER_FILE, 0x224cd4-0x200f10, 12
  .word 0x224e58, 4
  .incbin PRELOADER_FILE, 0x224e58-0x200f10, 16
  .word 0x224f2c, 5
  .incbin PRELOADER_FILE, 0x224f2c-0x200f10, 20
  .word 0x230f44, 3
  .incbin PRELOADER_FILE, 0x230f44-0x200f10, 12
  .word 0x236904, 4
  .incbin PRELOADER_FILE, 0x236904-0x200f10, 16
  .word 0x229600, 40
  .incbin PRELOADER_FILE, 0x229600-0x200f10, 160
.balign 4
bl2_payload:
  .incbin BL2_PATCH_FILE
bl2_payload_end:
.ltorg
stage_end:
