// SPDX-License-Identifier: BSD-2-Clause-Patent
// Linux Image entry for the stock G3D1 LK/preloader reservation layout.
// No stack is used. Interactive RAM boot disables automatic watchdog resets.
#include "Library/PlatformSecLib/RamBootWatchdog.h"
#ifndef T70_RAM_TRIAL
#define T70_RAM_TRIAL 0
#endif
.section .text
.global _Head
_Head:
  b _Start
  .long 0
  .quad 0                    // Linux text_offset
  .quad _ImageEnd - _Head    // Linux image_size, including the FD payload
  .quad 0xA                 // Little endian, 4 KiB pages, flexible placement
  .quad 0, 0, 0
  .ascii "ARM\x64"
  .long 0
_Start:
  adr x8, _Head
  mov x9, #0x40000000
  cmp x8, x9
  b.ne _Park
  mrs x10, CurrentEL
  cmp x10, #4
  b.eq 1f
  cmp x10, #8
  b.ne _Park
  mrs x11, sctlr_el2
  b 2f
1:
  mrs x11, sctlr_el1
2:
  tbnz x11, #0, _Park        // Require MMU disabled
  tbnz x11, #2, _Park        // Require D-cache disabled
#if T70_RAM_TRIAL
  // Interactive RAM boot: disable the inherited watchdog before validating
  // the FDT. Preserve unrelated mode bits; never program a timeout or reload.
  ldr x16, =T70_WDT_BASE
  ldr w17, [x16]
  ldr w18, =0x00FFFFFE
  and w17, w17, w18
  ldr w18, =T70_WDT_MODE_KEY
  orr w17, w17, w18
  str w17, [x16]
  dsb sy
  ldr w17, [x16]
  tbnz w17, #0, _Park
  ldr x16, =T70_TRIAL_RECORD
  ldr x17, =T70_TRIAL_MAGIC
  ldr x18, =T70_TRIAL_INVERSE
  stp x17, x18, [x16]
  stp xzr, xzr, [x16, #16]
  mov x17, #1               // Interactive RAM boot accepted, watchdog disabled
  str x17, [x16, #24]
  dsb sy
#endif
  tst x0, #7
  b.ne _Park
  ldr x12, =0x47C80000
  cmp x0, x12
  b.lo _Park
  ldr x13, =0x48080000
  cmp x0, x13
  b.hs _Park
  add x14, x0, #40
  cmp x14, x13
  b.hi _Park
  ldr w14, [x0]
  ldr w15, =0xEDFE0DD0
  cmp w14, w15
  b.ne _Park
  ldr w14, [x0, #4]
  rev w14, w14
  cmp w14, #40
  b.lo _Park
  mov w15, #0x200000
  cmp w14, w15
  b.hi _Park
  add x15, x0, x14
  cmp x15, x13
  b.hi _Park

  msr daifset, #0xF
#if !T70_RAM_TRIAL
  // A normal boot cannot inherit a trial cookie from retained DRAM.
  ldr x16, =T70_TRIAL_RECORD
  stp xzr, xzr, [x16]
  stp xzr, xzr, [x16, #16]
#endif
  ldr x12, =0x43F7F000
  ldr x13, =0x0000000148303754 // "T70H", handoff format version 1
  stp x13, x0, [x12]
  stp x14, x8, [x12, #16]
  mrs x13, mpidr_el1
  stp x10, x13, [x12, #32]
  mov x13, #1
  stp x11, x13, [x12, #48]
  // Discard any old GOP progress record before executing this FD.
  stp xzr, xzr, [x12, #0x60]
  stp xzr, xzr, [x12, #0x70]
  add x13, x12, #0x800
  str xzr, [x13] // Discard the old bounded display-register snapshot.
  // No peripheral is declared ready by a previous boot or uninitialized RAM.
  stp xzr, xzr, [x12, #0x80]
  stp xzr, xzr, [x12, #0x90]
  stp xzr, xzr, [x12, #0xA0]
  stp xzr, xzr, [x12, #0xB0]
  stp xzr, xzr, [x12, #0xC0]
  stp xzr, xzr, [x12, #0xD0]
  stp xzr, xzr, [x12, #0xE0]
  stp xzr, xzr, [x12, #0xF0]
  stp xzr, xzr, [x12, #0x100]
  stp xzr, xzr, [x12, #0x110]
  stp xzr, xzr, [x12, #0x120]
  stp xzr, xzr, [x12, #0x130]
  str xzr, [x12, #0x140] // I2C1 handoff rate and charger IRQ readiness
  // CCCI map-completion marker and CCCI handoff, separate from MUSB.
  stp xzr, xzr, [x12, #0x150]
  stp xzr, xzr, [x12, #0x160]
  stp xzr, xzr, [x12, #0x170]
  stp xzr, xzr, [x12, #0x180]
  stp xzr, xzr, [x12, #0x190]
  stp xzr, xzr, [x12, #0x1A0]
  stp xzr, xzr, [x12, #0x1B0]

  adr x1, _Payload
  ldr x2, =0x44000000
  mov x3, #0x400000
3:
  ldp x4, x5, [x1], #16
  stp x4, x5, [x2], #16
  subs x3, x3, #16
  b.ne 3b

  // Clean copied code to PoU, then invalidate the corresponding I-cache lines.
  mrs x6, ctr_el0
  ubfx x7, x6, #16, #4
  mov x8, #4
  lsl x7, x8, x7
  ldr x1, =0x44000000
  ldr x2, =0x44400000
4:
  dc cvau, x1
  add x1, x1, x7
  cmp x1, x2
  b.lo 4b
  dsb sy
  and x6, x6, #15
  lsl x7, x8, x6
  ldr x1, =0x44000000
5:
  ic ivau, x1
  add x1, x1, x7
  cmp x1, x2
  b.lo 5b
  dsb sy
  isb
  mov x13, #2
  str x13, [x12, #56]
  ldr x1, =0x44000000
  br x1                     // x0 remains the original bootloader FDT pointer
_Park:
  wfe
  b _Park
.ltorg
.balign 4096
_Payload:
.incbin FD_PATH
_ImageEnd:
