// SPDX-License-Identifier: BSD-2-Clause-Patent
// Runs in DRAM, checks the live preloader, then returns to its command loop.
#ifndef PRELOADER_FILE
#error Pass the unchanged shipping preloader through PRELOADER_FILE.
#endif
.syntax unified
.arch armv7-a
.arm
.text
.global _start
_start:
  push {r4-r12, lr}
  ldr r0, =0x401ffff4
  cmp r4, r0
  bne probe_return
  cmp r5, #12
  bne probe_return
  ldr r0, =0x002091cb
  cmp lr, r0
  bne probe_return
  // USB READ32 excludes RAM. Check these code windows from the CPU instead.
  adr r6, fingerprints
  mov r8, #4
verify_window:
  ldmia r6!, {r0, r1}
verify_word:
  ldr r2, [r0], #4
  ldr r3, [r6], #4
  cmp r2, r3
  bne probe_return
  subs r1, r1, #1
  bne verify_word
  subs r8, r8, #1
  bne verify_window
  ldr r7, =0x002f35a8
  ldr r7, [r7]
  bl check_pointer
  ldr r7, [r7, #8]
  bl check_pointer
  ldr r7, [r7]
  tst r7, #1
  bne callback_bounds
  tst r7, #3
  bne probe_return_loop
callback_bounds:
  bic r0, r7, #1
  ldr r1, =0x00200f10
  cmp r0, r1
  blo probe_return_loop
  ldr r1, =0x00200f10 + 367036
  cmp r0, r1
  bhs probe_return_loop
  adr r6, record
#ifdef T70_GUARD_AUDIT
  mov r0, #3
#else
  mov r0, #2
#endif
  str r0, [r6, #8]
  adr r0, _start
  str r0, [r6, #12]
  mrs r0, cpsr
  str r0, [r6, #16]
  mrc p15, 0, r0, c1, c0, 0
  str r0, [r6, #20]
  mrc p15, 0, r0, c0, c0, 5
  str r0, [r6, #24]
  str r4, [r6, #28]
  str r5, [r6, #32]
  // Preserve the actual preloader return PC saved at entry, not a helper's LR.
  ldr r0, [sp, #36]
  str r0, [r6, #36]
#ifdef T70_GUARD_AUDIT
  // Read only the exact extra guard windows used by the RAM bridge. Keep the
  // original four trusted windows above as a prerequisite for any USB call.
  add r0, sp, #40
  str r0, [r6, #56]
  ldr r0, [sp, #0x19c]
  str r0, [r6, #60]
  ldr r3, [r6, #20]
  ldr r2, =0x1005
  ands r3, r3, r2
  beq audit_mode
  mov r0, #1
  mov r1, #0
  mov r2, #0
  bl audit_failure
audit_mode:
  ldr r3, [r6, #16]
  and r3, r3, #0x3f
  cmp r3, #0x13
  beq audit_windows
  mov r0, #2
  mov r1, #0
  mov r2, #0x13
  bl audit_failure
audit_windows:
  adr r8, bridge_fingerprints
  mov r9, #1
  mov r10, #0
  mov r11, #0
audit_window:
  ldmia r8!, {r0, r1}
  mov r4, #0
audit_word:
  ldr r2, [r8], #4
  ldr r3, [r0]
  add r11, r11, #1
  cmp r2, r3
  beq audit_next_word
  mov r4, #1
  ldr r12, [r6, #40]
  cmp r12, #0
  bne audit_next_word
  mov r12, #3
  str r12, [r6, #40]
  str r0, [r6, #44]
  str r2, [r6, #48]
  str r3, [r6, #52]
audit_next_word:
  add r0, r0, #4
  subs r1, r1, #1
  bne audit_word
  cmp r4, #0
  orreq r10, r10, r9
  lsl r9, r9, #1
  cmp r9, #0x200
  bne audit_window
  str r10, [r6, #64]
  str r11, [r6, #68]
  ldr r3, [r6, #60]
  ldr r2, =0x224e5d
  cmp r3, r2
  beq audit_send
  mov r0, #4
  ldr r1, [r6, #56]
  add r1, r1, #0x174
  bl audit_failure
audit_send:
  mov r0, r6
  mov r1, #80
#else
  mov r0, r6
  mov r1, #40
#endif
  blx r7
  dsb sy
probe_return_loop:
  pop {r4-r12, lr}
  // The original noreturn jump tail replaced r5 with the DA argument size.
  // Restore the parser's stack buffer before executing another USB command.
  add r5, sp, #0x10
  bx lr
probe_return:
#ifdef T70_GUARD_AUDIT
  b probe_return_loop
// Record only the first failed guard. r6/r7 and the live code stay unchanged.
audit_failure:
  ldr r12, [r6, #40]
  cmp r12, #0
  streq r0, [r6, #40]
  streq r1, [r6, #44]
  streq r2, [r6, #48]
  streq r3, [r6, #52]
  bx lr
#else
  pop {r4-r12, pc}
#endif
// A rejected pointer unwinds the probe's saved registers without dereferencing it.
check_pointer:
  tst r7, #3
  bne probe_return_loop
  ldr r0, =0x00200000
  cmp r7, r0
  blo probe_return_loop
  ldr r0, =0x002ffff4
  cmp r7, r0
  bxls lr
  ldr r0, =0x40000000
  cmp r7, r0
  blo probe_return_loop
  ldr r0, =0x4ffffff4
  cmp r7, r0
  bhi probe_return_loop
  bx lr
.balign 4
record:
  .ascii "T70RAM32"
#ifdef T70_GUARD_AUDIT
  .space 72
bridge_fingerprints:
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
#else
  .space 32
#endif
fingerprints:
  .word 0x00201170, 3
  .incbin PRELOADER_FILE, 0x260, 12
  .word 0x00208f24, 19
  .incbin PRELOADER_FILE, 0x8014, 76
  .word 0x00209178, 21
  .incbin PRELOADER_FILE, 0x8268, 84
  .word 0x0020949c, 16
  .incbin PRELOADER_FILE, 0x858c, 64
.ltorg
