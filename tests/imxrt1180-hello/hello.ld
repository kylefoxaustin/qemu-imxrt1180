/*
 * Link the semihosting hello into the i.MX RT1180 CM33 FlexSPI NOR XIP window
 * (0x28000000).  The reset vector table lands first so init-svtor finds it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
ENTRY(reset_handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x28000000, LENGTH = 8M      /* FlexSPI1 NOR (XIP) */
    DTCM  (rwx) : ORIGIN = 0x20000000, LENGTH = 128K    /* system TCM         */
}

SECTIONS
{
    .vectors 0x28000000 : { KEEP(*(.vectors)) } > FLASH
    .text : { *(.text*) *(.rodata*) } > FLASH
    .data : { *(.data*) } > DTCM
    .bss  : { *(.bss*) *(COMMON) } > DTCM
}
