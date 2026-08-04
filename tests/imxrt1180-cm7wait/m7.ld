ENTRY(m7_reset)
MEMORY { TCM (rwx) : ORIGIN = 0x303C0000, LENGTH = 512K }
SECTIONS {
  .vectors 0x303C0000 : { KEEP(*(.vectors)) } > TCM
  .text : { *(.text*) *(.rodata*) } > TCM
  .data : { *(.data*) } > TCM
  .bss  : { *(.bss*) *(COMMON) } > TCM
}
