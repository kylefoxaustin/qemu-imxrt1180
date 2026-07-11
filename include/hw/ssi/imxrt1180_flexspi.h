/*
 * NXP i.MX RT1180 FlexSPI controller (serial NOR flash).
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives an attached SPI-NOR flash (hw/block/m25p80) over a QEMU SSI bus via
 * the two paths real firmware uses:
 *
 *  - IP commands: firmware loads a sequence into the LUT, sets IPCR0 (flash
 *    address) / IPCR1 (data size + sequence id), and triggers IPCMD[TRG].  We
 *    interpret the sequence and shift bytes to the flash with CS asserted.
 *  - AHB reads: the memory-mapped XIP window issues a read to the flash, so
 *    memory-mapped reads return real flash content.
 *
 * The XIP window is READ-ONLY, as on silicon: CPU stores into flash address
 * space do nothing.  Programming goes through the IP path, so erase/program
 * physics (erase-before-write, bits only 1->0) come from the m25p80 model
 * rather than being hand-rolled here.
 */
#ifndef HW_SSI_IMXRT1180_FLEXSPI_H
#define HW_SSI_IMXRT1180_FLEXSPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qemu/fifo8.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_FLEXSPI "imxrt1180-flexspi"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180FlexSPIState, IMXRT1180_FLEXSPI)

/*
 * Register window is 4 KiB; registers are backed to 0x400, which is where the
 * RT1180 LUT ends: FLEXSPI_LUT_COUNT is 128 words (0x200..0x400), i.e. 32
 * sequences.  (Note this differs from the i.MX 93 FlexSPI, whose LUT is 64
 * words -- verified against MIMXRT1189 PERI_FLEXSPI.h.)
 */
#define IMXRT1180_FLEXSPI_REG_SIZE  0x1000
#define IMXRT1180_FLEXSPI_NUM_REGS  (0x400 / 4)
#define IMXRT1180_FLEXSPI_NUM_CS    4

/*
 * IP FIFO depth.  Real silicon has a 128-entry (1 KiB) IP TX FIFO; we give the
 * model room to spare so that fsl_flexspi's "pre-fill then trigger" write path
 * always lands its whole payload before IPCMD is triggered (see the run_seq
 * comment in the .c).  Overflow is reported, never silently dropped.
 */
#define IMXRT1180_FLEXSPI_FIFO_BYTES 4096

struct IMXRT1180FlexSPIState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;     /* control registers                  */
    MemoryRegion ahb;       /* AHB-mapped flash (XIP) window       */
    qemu_irq irq;
    qemu_irq cs[IMXRT1180_FLEXSPI_NUM_CS];
    SSIBus *bus;

    uint32_t ahb_size;      /* size of the XIP window (flash size) */

    uint32_t regs[IMXRT1180_FLEXSPI_NUM_REGS];
    bool lut_unlocked;
    Fifo8 rx;               /* bytes read from flash (RFDR packs 4/word)   */
    Fifo8 tx;               /* bytes to program     (TFDR unpacks 4/word)  */
};

/*
 * Program firmware into the attached NOR -- "flash the board".  The board loader
 * uses this for image segments linked into the XIP window, so that -kernel of an
 * XIP image means what it does on real hardware: the content is IN the flash,
 * not merely visible in the memory-mapped view of it.
 */
void imxrt1180_flexspi_flash_program(IMXRT1180FlexSPIState *s, uint32_t off,
                                     const uint8_t *buf, uint32_t len);

#endif /* HW_SSI_IMXRT1180_FLEXSPI_H */
