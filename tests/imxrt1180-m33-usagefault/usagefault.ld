/*
 * Link the CM33 UsageFault probe into the FlexSPI NOR XIP window (0x28000000),
 * vector table first so the machine's VTOR seeding finds it -- same scheme as
 * tests/imxrt1180-hello.  No external memory / VTOR-header harness needed.
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
