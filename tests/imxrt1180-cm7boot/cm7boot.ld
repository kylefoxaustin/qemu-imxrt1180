/*
 * Standalone Cortex-M7 boot test — links like a real SDK cm7 image: vector
 * table + code in the M7's LOCAL ITCM (0x0), stack in its LOCAL DTCM
 * (0x20000000).  The machine auto-detects a cm7 image (a PT_LOAD below the M33
 * code TCM 0x0FFE0000) and boots the M7 with its per-core view.
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
