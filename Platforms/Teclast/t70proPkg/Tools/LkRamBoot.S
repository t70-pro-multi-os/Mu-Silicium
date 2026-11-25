// SPDX-License-Identifier: BSD-2-Clause-Patent
// G3D1 shipping LK only. Installed into verified padding in RAM, never storage.
.text
.balign 4
.global register_ram_boot, ram_boot, ram_image_source_hook, patch_end
register_ram_boot:
  stp x29, x30, [sp, #-16]!
  mov x29, sp
  bl lk_register               // Keep the original continue registration.
  adr x0, boot_name
  adr x1, ram_boot
  mov w2, #1                 // Supported under the shipping security-control gate.
  mov w3, #1                 // Dispatcher also requires raw unlocked state 3.
  bl lk_register
  ldp x29, x30, [sp], #16
  ret

ram_boot:
  stp x29, x30, [sp, #-64]!
  stp x20, x19, [sp, #16]
  stp x22, x21, [sp, #32]
  mov x29, sp
  ldrb w8, [x0]               // LK matches prefixes; reject boot with arguments.
  cbnz w8, boot_fail
  add x0, sp, #48
  str wzr, [x0]
  bl lk_get_lock_state        // Real seccfg getter: status + raw state output.
  cbnz w0, boot_fail
  ldr w8, [sp, #48]
  cmp w8, #3                  // Only the shipping unlocked state is accepted.
  b.ne boot_fail
  adrp x19, download_buffer
  add x19, x19, :lo12:download_buffer
  ldr x20, [x19]
  cbz x20, boot_fail
  ldr w21, [x19, #64]         // download_size at c8178, buffer at c8138.
  tbnz w21, #31, boot_fail
  cmp w21, #4096
  b.lo boot_fail
  ldr w8, [x19, #8]          // Current allocation's maximum download length.
  cmp w21, w8
  b.hi boot_fail
  ldr x8, [x20]
  ldr x9, android_magic
  cmp x8, x9
  b.ne boot_fail
  ldr w8, [x20, #40]
  cmp w8, #4
  b.ne boot_fail
  ldr w8, [x20, #20]
  cmp w8, #1584
  b.ne boot_fail
  ldr w8, [x20, #12]
  cbnz w8, boot_fail          // This route is for the no-ramdisk EDK2 image.
  ldr w8, [x20, #8]
  cmp w8, #64
  b.lo boot_fail
  add x8, x8, #4095
  and x8, x8, #0xfffffffffffff000
  add x8, x8, #4096
  ldr w9, [x20, #1580]
  add x8, x8, x9
  cmp x8, x21
  b.hi boot_fail
  mov x0, x21
  bl lk_malloc
  cbz x0, boot_fail
  mov x22, x0
  mov x1, x20
  mov x2, x21
  bl lk_memcpy
  // Own a separate heap copy before the fastboot transport stops or is reused.
  str x22, [x19]
  str w21, [x19, #8]          // Keep a failed boot's later download in bounds.
  orr w21, w21, #0x80000000
  add x8, x19, #64
  stlr w21, [x8]
  ldp x22, x21, [sp, #32]
  ldp x20, x19, [sp, #16]
  ldp x29, x30, [sp], #64
  b lk_continue              // Original OKAY, transport stop and boot event.
boot_fail:
  adr x0, failure_text
  bl lk_fail
  ldp x22, x21, [sp, #32]
  ldp x20, x19, [sp, #16]
  ldp x29, x30, [sp], #64
  ret

ram_image_source_hook:
  adrp x9, download_buffer
  add x9, x9, :lo12:download_buffer
  add x12, x9, #64
  ldar w10, [x12]
  tbz w10, #31, source_original
  // Preserve vendor_boot/init_boot and boot-prefixed partition names exactly.
  ldr w11, [x1]
  ldr w13, boot_name
  cmp w11, w13
  b.ne source_original
  ldrb w11, [x1, #4]
  cbnz w11, source_original
  ldr x11, [x9]
  cbz x11, source_original
  and w10, w10, #0x7fffffff
  stp x11, x10, [x8, #8]     // Change pointer AND length before header parsing.
  stlr w10, [x12]             // Consume the request once; ordinary continue stays stock.
source_original:
  ldr x27, [x8, #8]          // Displaced shipping instruction.
  b lk_source_resume
.balign 8
android_magic:
  .ascii "ANDROID!"
boot_name:
  .asciz "boot"
failure_text:
  .asciz "RAM boot: unlocked, valid EDK2 image required"
.balign 4
patch_end:
