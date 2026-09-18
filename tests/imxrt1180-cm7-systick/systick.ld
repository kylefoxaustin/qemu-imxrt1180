/*
 * cm7 SysTick probe -- links like a real SDK cm7 image: vectors + code in the
 * M7 local ITCM (0x0), stack in local DTCM (0x20000000). The machine
 * auto-detects a cm7 image (a PT_LOAD below the M33 code TCM 0x0FFE0000).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
ENTRY(m7_reset)
MEMORY {
  ITCM (rwx) : ORIGIN = 0x00000000, LENGTH = 256K
  DTCM (rw)  : ORIGIN = 0x20000000, LENGTH = 256K
}
SECTIONS {
  .vectors 0x00000000 : { KEEP(*(.vectors)) } > ITCM
  .text : { *(.text*) *(.rodata*) } > ITCM
  .data : { *(.data*) } > ITCM
  /DISCARD/ : { *(.bss*) *(COMMON) *(.ARM.*) *(.comment) }
}
