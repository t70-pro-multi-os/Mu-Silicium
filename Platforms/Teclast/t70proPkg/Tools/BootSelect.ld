/* SPDX-License-Identifier: BSD-2-Clause-Patent */
OUTPUT_ARCH(aarch64)
ENTRY(_head)
PROVIDE(_image_base = 0);
PROVIDE(_stack_bytes = 4096);
SECTIONS {
  . = _image_base;
  .text : { KEEP(*(.text.boot)) *(.text .text.*) *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .stack : { . = ALIGN(4096); . += _stack_bytes - 1; BYTE(0); }
  _stack_end = .;
  _image_bytes = . - _head;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(_head == _image_base, "Image header moved")
ASSERT(_copy >= _stock_end, "Copy code overlaps Android destination")
ASSERT(_image_bytes < 0x03F7F000, "Capsule overlaps reserved handoff page")
