// SPDX-License-Identifier: BSD-2-Clause-Patent
// Original-LK boot-mode selection, or the earlier offline RAM-request prototype.
#include "BootSelect.h"
.section .text.boot,"ax"
.global _head, _copy, _stock, _stock_end, _uefi, _uefi_end, _park
_head:
  b _entry
  .long 0
  .quad 0
  .quad _image_bytes
  .quad 0xA
  .quad 0,0,0
  .ascii "ARM\x64"
  .long 0
_entry:
  msr daifset,#0xf
  adr x19,_head
  mov x20,x0
  // UEFI requires the original LK placement.
  mov x8,#0x40000000
  cmp x19,x8
#ifdef T70_USE_LK_BOOT_MODE
  b.ne _park
#else
  b.ne _android
#endif
  mrs x8,CurrentEL
  cmp x8,#4
  b.eq 1f
  cmp x8,#8
  b.ne _park
  mrs x8,sctlr_el2
  b 2f
1:
  mrs x8,sctlr_el1
2:
  tbnz x8,#0,_park
  tbnz x8,#2,_park
  adrp x8,_stack_end
  add x8,x8,:lo12:_stack_end
  mov sp,x8
#ifdef T70_USE_LK_BOOT_MODE
  bl select_from_lk_boot_mode
  cbz w0,_android
  cmp w0,#1
  b.eq _select_uefi
  b _park
#else
  bl reserve_request_page
  cbz w0,_android
  ldr x8,=T70_REQUEST_ADDR
  ldp x9,x10,[x8]
  ldp x11,x12,[x8,#16]
  // Consume even invalid records; UEFI cannot inherit a persistent boot loop.
  stp xzr,xzr,[x8]
  stp xzr,xzr,[x8,#16]
  dsb sy
  ldr x13,=T70_REQUEST_MAGIC
  cmp x9,x13
  b.ne _android
  ldr x13,=T70_REQUEST_INVERSE
  cmp x10,x13
  b.ne _android
  ldr x13,=T70_REQUEST_IMAGE_ID
  cmp x11,x13
  b.ne _android
  mvn x13,x13
  cmp x12,x13
  b.ne _android
#endif
_select_uefi:
  adrp x1,_uefi
  add x1,x1,:lo12:_uefi
  ldr x2,=(_uefi_end-_uefi)
  b _copy
_android:
  adrp x1,_stock
  add x1,x1,:lo12:_stock
  ldr x2,=(_stock_end-_stock)
  b _copy
_park:
  wfe
  b _park
.ltorg
.balign 4096
_stock:
.incbin T70_STOCK_FILE
_stock_end:
_uefi:
.incbin T70_UEFI_FILE
_uefi_end:
.balign 16
// This code is above both destination image extents, including Android BSS.
_copy:
  mov x3,x19
  mov x4,x2
3:
  ldp x5,x6,[x1],#16
  stp x5,x6,[x3],#16
  subs x4,x4,#16
  b.ne 3b
  mrs x5,ctr_el0
  ubfx x6,x5,#16,#4
  mov x7,#4
  lsl x6,x7,x6
  mov x3,x19
  add x4,x19,x2
4:
  dc cvac,x3
  add x3,x3,x6
  cmp x3,x4
  b.lo 4b
  dsb sy
  and x5,x5,#15
  lsl x6,x7,x5
  mov x3,x19
5:
  ic ivau,x3
  add x3,x3,x6
  cmp x3,x4
  b.lo 5b
  dsb sy
  isb
  mov x0,x20
  mov x1,xzr
  mov x2,xzr
  mov x3,xzr
  br x19
