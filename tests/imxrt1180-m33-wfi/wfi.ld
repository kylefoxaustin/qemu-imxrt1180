/*
 * Link the CM33 WFI probe into the code TCM (0x0FFE0000), vector table first so
 * the machine's VTOR seeding finds it. TCM — NOT the FlexSPI XIP window — so the
 * image runs on both QEMU and a second implementation without the permissive
 * XIP map or a boot-ROM FlexSPI setup.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
ENTRY(reset_handler)

MEMORY
{
    ITCM (rx) : ORIGIN = 0x0FFE0000, LENGTH = 128K   /* CM33 code TCM */
    DTCM (rw) : ORIGIN = 0x20000000, LENGTH = 128K   /* CM33 system TCM */
}

SECTIONS
{
    .vectors 0x0FFE0000 : { KEEP(*(.vectors)) } > ITCM
    .text : { *(.text*) *(.rodata*) } > ITCM
    .data : { *(.data*) } > DTCM
    .bss  : { *(.bss*) *(COMMON) } > DTCM
}
