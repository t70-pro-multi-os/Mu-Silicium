// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <AsmMacroLib.h>
ASM_FUNC (PlatformAssemblyInitialize)
  ret

// Keep only own SEC exception metadata; the normal handler still waits for
// the already-armed trial watchdog. No stack or arbitrary memory is read.
ASM_FUNC (T70ProRecordSecException)
  movz x3, #0xf000
  movk x3, #0x43f7, lsl #16
  ldr x4, [x3]
  movz x5, #0x3754
  movk x5, #0x4830, lsl #16
  movk x5, #1, lsl #32
  cmp x4, x5
  b.ne 9f
  ldr x4, [x3, #0x38]
  cmp x4, #3
  b.lo 9f
  cmp x4, #4
  b.hi 9f
  mrs x2, CurrentEL
  cmp x2, #4
  b.eq 1f
  cmp x2, #8
  b.ne 9f
  mrs x4, esr_el2
  mrs x5, far_el2
  mrs x6, sctlr_el2
  mrs x7, spsr_el2
  b 2f
1:
  mrs x4, esr_el1
  mrs x5, far_el1
  mrs x6, sctlr_el1
  mrs x7, spsr_el1
2:
  add x3, x3, #0x800
  str xzr, [x3]
  movz w8, #0xf060
  movk w8, #0x43f7, lsl #16
  stp w8, wzr, [x3, #8]
  stp w2, w0, [x3, #16]
  stp x4, x1, [x3, #24]
  stp x5, x6, [x3, #40]
  str x7, [x3, #56]
  str xzr, [x3, #64]
  dsb sy
  movz x4, #0x3754
  movk x4, #0x4c44, lsl #16
  movk x4, #5, lsl #32
  str x4, [x3]
  // An eight-byte stride covers every cache line containing our 72 bytes.
  mov x4, #9
3:
  dc cvac, x3
  add x3, x3, #8
  subs x4, x4, #1
  b.ne 3b
  dsb sy
9:
  ret
