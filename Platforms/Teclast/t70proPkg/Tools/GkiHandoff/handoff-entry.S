// SPDX-License-Identifier: BSD-2-Clause-Patent
// Legacy ARM64 kexec enters purgatory with no usable DTB argument.
// The kernel has already relocated every segment and disabled MMU/D-cache.
// Do not touch EL state, MMIO, stacks, watchdogs or firmware reservations.
.section .text,"ax"
.global t70_kexec_entry
t70_kexec_entry:
  ldr x0, .Ldtb
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  ldr x16, .Limage
  br x16
.balign 8
.Ldtb:
  .quad 0x47C80000
.Limage:
  .quad 0x40000000
