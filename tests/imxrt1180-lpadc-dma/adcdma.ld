ENTRY(reset_handler)
MEMORY { ITCM (rx) : ORIGIN = 0x0FFE0000, LENGTH = 128K
         DTCM (rw) : ORIGIN = 0x20000000, LENGTH = 128K }
SECTIONS {
  .vectors 0x0FFE0000 : { KEEP(*(.vectors)) } > ITCM
  .text : { *(.text*) *(.rodata*) } > ITCM
  .data : { *(.data*) } > DTCM
  .bss  : { *(.bss*) *(COMMON) } > DTCM
}
