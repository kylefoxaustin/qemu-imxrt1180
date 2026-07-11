/*
 * NXP i.MX RT1180 FlexSPI controller (serial NOR flash).
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register offsets, bit positions, LUT opcodes and the LUT unlock key are all
 * taken from the MIMXRT1189 CMSIS header (devices/RT/RT1180/periph/
 * PERI_FLEXSPI.h) and the MCUXpresso fsl_flexspi driver -- not guessed.
 *
 * Two command paths reach the attached SPI-NOR (hw/block/m25p80) over an SSI
 * bus:
 *
 *  - IP commands.  Firmware loads a sequence into the LUT (each seqid is four
 *    32-bit words = eight 16-bit instructions: opcode[15:10], pads[9:8],
 *    operand[7:0]), writes IPCR0 (flash address) and IPCR1 (data size +
 *    sequence id), then triggers IPCMD[TRG].  We interpret the sequence
 *    (CMD/ADDR/MODE/DUMMY/READ/WRITE/STOP), shifting bytes to the flash with
 *    chip-select asserted, filling RFDR on reads and draining TFDR on writes,
 *    and raise INTR[IPCMDDONE].
 *
 *  - AHB reads.  The memory-mapped XIP window issues a normal 03h read for the
 *    accessed offset, so memory-mapped reads return real flash content.
 *
 * The XIP window is READ-ONLY, exactly as on silicon: a CPU store into flash
 * address space does not modify flash.  (Backing that window with plain RAM --
 * which lets guest stores just land -- is a false-green generator: it makes a
 * firmware that programs flash appear to work while hiding whatever the write
 * path actually does.  Erase/program physics -- erase-before-write, bits only
 * 1->0, program-without-erase fails -- come from m25p80, not from us.)
 *
 * PADS ARE NOT MODELLED.  QEMU's SSI bus shifts whole bytes, so 1/2/4/8-pad
 * (SDR) sequences all become the same byte stream; that is fine for NOR command
 * sequences, whose semantics do not depend on lane count.  DDR opcodes are not
 * implemented and are reported (LOG_UNIMP) rather than silently treated as SDR.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/ssi/imxrt1180_flexspi.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/* --- Register offsets (MIMXRT1189 PERI_FLEXSPI.h) ------------------------- */
#define FSPI_MCR0       0x000
#define FSPI_INTEN      0x010
#define FSPI_INTR       0x014
#define FSPI_LUTKEY     0x018
#define FSPI_LUTCR      0x01c
#define FSPI_IPCR0      0x0a0
#define FSPI_IPCR1      0x0a4
#define FSPI_IPCMD      0x0b0
#define FSPI_IPRXFCR    0x0b8
#define FSPI_IPTXFCR    0x0bc
#define FSPI_STS0       0x0e0
#define FSPI_IPRXFSTS   0x0f0
#define FSPI_IPTXFSTS   0x0f4
#define FSPI_RFDR       0x100   /* 32 words */
#define FSPI_TFDR       0x180   /* 32 words */
#define FSPI_LUT        0x200   /* 128 words -> 32 sequences */
#define FSPI_LUT_END    0x400

/* --- Bit positions -------------------------------------------------------- */
#define MCR0_SWRESET        (1u << 0)

#define INTR_IPCMDDONE      (1u << 0)
#define INTR_IPCMDGE        (1u << 1)   /* IP command grant error */
#define INTR_IPCMDERR       (1u << 3)
#define INTR_IPRXWA         (1u << 5)   /* RX FIFO watermark available */
#define INTR_IPTXWE         (1u << 6)   /* TX FIFO watermark empty     */

#define LUTKEY_VAL          0x5af05af0u
#define LUTCR_LOCK          (1u << 0)
#define LUTCR_UNLOCK        (1u << 1)

#define IPCR1_IDATSZ_MASK   0x0000ffffu
#define IPCR1_ISEQID_SHIFT  16
#define IPCR1_ISEQID_MASK   0x1fu       /* 5 bits: 32 sequences */
#define IPCR1_ISEQNUM_SHIFT 24
#define IPCR1_ISEQNUM_MASK  0x7u

#define IPCMD_TRG           (1u << 0)

#define IPRXFCR_CLRIPRXF    (1u << 0)
#define IPRXFCR_RXWMRK_MASK 0x7cu
#define IPRXFCR_RXWMRK_SHIFT 2

#define IPTXFCR_CLRIPTXF    (1u << 0)

#define STS0_SEQIDLE        (1u << 0)
#define STS0_ARBIDLE        (1u << 1)

#define FSTS_FILL_MASK      0xffu       /* fill level, in 8-byte entries */

/* --- LUT instruction opcodes (fsl_flexspi.h flexspi_command_t) ------------ */
#define LUT_STOP        0x00
#define LUT_CMD_SDR     0x01
#define LUT_RADDR_SDR   0x02
#define LUT_CADDR_SDR   0x03
#define LUT_MODE1_SDR   0x04
#define LUT_MODE2_SDR   0x05
#define LUT_MODE4_SDR   0x06
#define LUT_MODE8_SDR   0x07
#define LUT_WRITE_SDR   0x08
#define LUT_READ_SDR    0x09
#define LUT_DUMMY_SDR   0x0c
#define LUT_JUMP_ON_CS  0x1f

#define FLASH_CMD_READ  0x03            /* AHB (XIP) reads use a plain 03h */

static void flexspi_update_irq(IMXRT1180FlexSPIState *s)
{
    uint32_t active = s->regs[FSPI_INTR >> 2] & s->regs[FSPI_INTEN >> 2];

    qemu_set_irq(s->irq, !!active);
}

/* Fill level in 8-byte entries, as IPRXFSTS/IPTXFSTS report it. The FlexSPI
 * FIFOs are 64-bit wide, so a partial entry still counts: fsl_flexspi's small-
 * read path spins on "size > FILL * 8", which would never satisfy a 4-byte
 * read if we truncated instead of rounding up. */
static uint32_t flexspi_fill_entries(Fifo8 *f)
{
    uint32_t entries = (fifo8_num_used(f) + 7) / 8;

    return MIN(entries, FSTS_FILL_MASK);
}

/*
 * Execute one LUT sequence against the flash.
 *
 * Commands run inline (to completion) on the IPCMD[TRG] write.  fsl_flexspi's
 * write path pre-fills the IP TX FIFO *before* triggering (it only streams the
 * remainder afterwards via the TX watermark), and a page program is at most 256
 * bytes against a 1 KiB TX FIFO, so in practice the whole payload is already
 * queued when we run.  If it is not -- a payload larger than the FIFO, which
 * would require true streaming -- we must NOT pad with zeros and silently
 * program the wrong bytes: we report IPCMDERR and log, so the failure is loud.
 */
static void flexspi_run_seq(IMXRT1180FlexSPIState *s, int seqid)
{
    uint32_t addr = s->regs[FSPI_IPCR0 >> 2];
    uint32_t ipcr1 = s->regs[FSPI_IPCR1 >> 2];
    uint32_t datasz = ipcr1 & IPCR1_IDATSZ_MASK;
    bool err = false;
    int i;

    qemu_set_irq(s->cs[0], 0);          /* assert chip-select */

    for (i = 0; i < 8; i++) {
        uint32_t word = s->regs[(FSPI_LUT + seqid * 16 + (i / 2) * 4) >> 2];
        uint16_t instr = (i & 1) ? (word >> 16) : (word & 0xffff);
        uint8_t opcode = (instr >> 10) & 0x3f;
        uint8_t operand = instr & 0xff;
        uint32_t n;
        int b;

        switch (opcode) {
        case LUT_STOP:
        case LUT_JUMP_ON_CS:
            i = 8;                      /* end of sequence */
            break;

        case LUT_CMD_SDR:
            ssi_transfer(s->bus, operand);
            break;

        case LUT_RADDR_SDR:
        case LUT_CADDR_SDR:
            /* operand = address width in bits, MSB first */
            for (b = operand / 8 - 1; b >= 0; b--) {
                ssi_transfer(s->bus, (addr >> (b * 8)) & 0xff);
            }
            break;

        case LUT_MODE1_SDR:
        case LUT_MODE2_SDR:
        case LUT_MODE4_SDR:
        case LUT_MODE8_SDR:
            ssi_transfer(s->bus, operand);
            break;

        case LUT_DUMMY_SDR:
            /* operand = dummy cycles; the SSI bus shifts whole bytes. */
            for (n = 0; n < (operand + 7u) / 8u; n++) {
                ssi_transfer(s->bus, 0);
            }
            break;

        case LUT_READ_SDR:
            for (n = 0; n < datasz; n++) {
                uint8_t rx = ssi_transfer(s->bus, 0);

                if (fifo8_is_full(&s->rx)) {
                    qemu_log_mask(LOG_GUEST_ERROR, "%s: IP RX FIFO overflow "
                                  "(datasz %u > %u) - read truncated\n",
                                  __func__, datasz,
                                  IMXRT1180_FLEXSPI_FIFO_BYTES);
                    err = true;
                    break;
                }
                fifo8_push(&s->rx, rx);
            }
            break;

        case LUT_WRITE_SDR:
            for (n = 0; n < datasz; n++) {
                if (fifo8_is_empty(&s->tx)) {
                    /*
                     * Firmware asked us to program more bytes than it queued.
                     * Padding with zeros here would silently write the wrong
                     * data to flash -- fail loudly instead.
                     */
                    qemu_log_mask(LOG_GUEST_ERROR, "%s: IP TX FIFO underrun "
                                  "at byte %u of %u - refusing to program "
                                  "padding (streaming writes larger than the "
                                  "%u-byte FIFO are not modelled)\n",
                                  __func__, n, datasz,
                                  IMXRT1180_FLEXSPI_FIFO_BYTES);
                    err = true;
                    break;
                }
                ssi_transfer(s->bus, fifo8_pop(&s->tx));
            }
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: LUT opcode 0x%02x (seq %d, slot %d) "
                          "not implemented\n", __func__, opcode, seqid, i);
            err = true;
            break;
        }
    }

    qemu_set_irq(s->cs[0], 1);          /* deassert chip-select */

    s->regs[FSPI_INTR >> 2] |= INTR_IPCMDDONE | (err ? INTR_IPCMDERR : 0);
    flexspi_update_irq(s);
}

static uint64_t flexspi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180FlexSPIState *s = opaque;

    /* IP RX FIFO data: any word in the RFDR aperture pops 4 bytes. */
    if (offset >= FSPI_RFDR && offset < FSPI_RFDR + 0x80) {
        uint32_t word = 0;
        int b;

        for (b = 0; b < 4; b++) {       /* RFDR packs 4 bytes, LSB first */
            if (!fifo8_is_empty(&s->rx)) {
                word |= (uint32_t)fifo8_pop(&s->rx) << (b * 8);
            }
        }
        return word;
    }

    switch (offset) {
    case FSPI_STS0:
        /* Commands complete inline, so the controller is always idle. */
        return STS0_SEQIDLE | STS0_ARBIDLE;

    case FSPI_IPRXFSTS:
        return flexspi_fill_entries(&s->rx);

    case FSPI_IPTXFSTS:
        return flexspi_fill_entries(&s->tx);

    case FSPI_INTR: {
        /*
         * Commands complete inline, so the IP FIFOs are never a bottleneck:
         * TX always has room (IPTXWE), and RX reports watermark-available once
         * it holds at least a watermark's worth.  Without these the fsl_flexspi
         * per-chunk polls never retire.
         */
        uint32_t rxwmrk =
            ((s->regs[FSPI_IPRXFCR >> 2] & IPRXFCR_RXWMRK_MASK)
             >> IPRXFCR_RXWMRK_SHIFT) + 1;
        uint32_t intr = s->regs[FSPI_INTR >> 2] | INTR_IPTXWE;

        if (fifo8_num_used(&s->rx) >= rxwmrk * 8) {
            intr |= INTR_IPRXWA;
        }
        return intr;
    }

    default:
        if ((offset >> 2) >= IMXRT1180_FLEXSPI_NUM_REGS) {
            return 0;
        }
        return s->regs[offset >> 2];
    }
}

static void flexspi_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    IMXRT1180FlexSPIState *s = opaque;

    /* IP TX FIFO data: any word in the TFDR aperture pushes 4 bytes. */
    if (offset >= FSPI_TFDR && offset < FSPI_TFDR + 0x80) {
        int b;

        for (b = 0; b < 4; b++) {       /* TFDR unpacks 4 bytes, LSB first */
            if (fifo8_is_full(&s->tx)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: IP TX FIFO overflow - byte dropped\n",
                              __func__);
                break;
            }
            fifo8_push(&s->tx, (value >> (b * 8)) & 0xff);
        }
        return;
    }

    if (offset >= FSPI_LUT && offset < FSPI_LUT_END) {
        if (s->lut_unlocked) {
            s->regs[offset >> 2] = value;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: LUT write @0x%" HWADDR_PRIx " while locked\n",
                          __func__, offset);
        }
        return;
    }

    switch (offset) {
    case FSPI_MCR0:
        if (value & MCR0_SWRESET) {
            fifo8_reset(&s->rx);
            fifo8_reset(&s->tx);
            value &= ~MCR0_SWRESET;     /* self-clearing */
        }
        s->regs[FSPI_MCR0 >> 2] = value;
        break;

    case FSPI_LUTCR:
        /* Unlock needs the key in LUTKEY first; LUTCR=2 unlock, =1 lock. */
        if ((value & LUTCR_UNLOCK) &&
            s->regs[FSPI_LUTKEY >> 2] == LUTKEY_VAL) {
            s->lut_unlocked = true;
        } else if (value & LUTCR_LOCK) {
            s->lut_unlocked = false;
        }
        s->regs[FSPI_LUTCR >> 2] = value;
        break;

    case FSPI_INTR:
        s->regs[FSPI_INTR >> 2] &= ~(uint32_t)value;    /* write-1-to-clear */
        flexspi_update_irq(s);
        break;

    case FSPI_IPRXFCR:
        if (value & IPRXFCR_CLRIPRXF) {
            fifo8_reset(&s->rx);
        }
        /* Keep RXWMRK: the driver reads it back to size its FIFO polls. */
        s->regs[FSPI_IPRXFCR >> 2] = value & ~IPRXFCR_CLRIPRXF;
        break;

    case FSPI_IPTXFCR:
        if (value & IPTXFCR_CLRIPTXF) {
            fifo8_reset(&s->tx);
        }
        s->regs[FSPI_IPTXFCR >> 2] = value & ~IPTXFCR_CLRIPTXF;
        break;

    case FSPI_IPCMD:
        if (value & IPCMD_TRG) {
            uint32_t ipcr1 = s->regs[FSPI_IPCR1 >> 2];
            int seqid = (ipcr1 >> IPCR1_ISEQID_SHIFT) & IPCR1_ISEQID_MASK;
            uint32_t seqnum =
                (ipcr1 >> IPCR1_ISEQNUM_SHIFT) & IPCR1_ISEQNUM_MASK;

            if (seqnum != 0) {
                qemu_log_mask(LOG_UNIMP, "%s: IPCR1[ISEQNUM]=%u (multi-sequence "
                              "IP command) not modelled; running seq %d only\n",
                              __func__, seqnum, seqid);
            }
            flexspi_run_seq(s, seqid);
        }
        break;

    default:
        if ((offset >> 2) < IMXRT1180_FLEXSPI_NUM_REGS) {
            s->regs[offset >> 2] = value;
        }
        break;
    }
}

static const MemoryRegionOps flexspi_ops = {
    .read = flexspi_read,
    .write = flexspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* --- AHB-mapped flash (XIP) window ---------------------------------------- */

static uint64_t flexspi_ahb_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180FlexSPIState *s = opaque;
    uint64_t val = 0;
    unsigned b;

    qemu_set_irq(s->cs[0], 0);
    ssi_transfer(s->bus, FLASH_CMD_READ);
    for (b = 3; b-- > 0; ) {            /* 24-bit address, MSB first */
        ssi_transfer(s->bus, (offset >> (b * 8)) & 0xff);
    }
    for (b = 0; b < size; b++) {
        val |= (uint64_t)ssi_transfer(s->bus, 0) << (b * 8);
    }
    qemu_set_irq(s->cs[0], 1);

    return val;
}

static void flexspi_ahb_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    /*
     * The XIP window is read-only on silicon -- a CPU store into flash address
     * space does not modify flash.  Programming goes through the IP command
     * path.  Log it: firmware storing here is a bug in the firmware (or a
     * .bss/.data section mislinked into flash), and quietly absorbing the write
     * would hide it.
     */
    qemu_log_mask(LOG_GUEST_ERROR,
                  "imxrt1180-flexspi: store to read-only XIP window "
                  "@0x%" HWADDR_PRIx " (value 0x%" PRIx64 ") ignored; flash is "
                  "programmed via the IP command path\n", offset, value);
}

static const MemoryRegionOps flexspi_ahb_ops = {
    .read = flexspi_ahb_read,
    .write = flexspi_ahb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * Instruction fetch and memcpy from the XIP window burst it with wide
     * accesses; capping narrower makes QEMU reject them as invalid (an external
     * abort) before the handler ever runs.
     */
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void flexspi_reset(DeviceState *dev)
{
    IMXRT1180FlexSPIState *s = IMXRT1180_FLEXSPI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->lut_unlocked = false;
    fifo8_reset(&s->rx);
    fifo8_reset(&s->tx);
}

static void flexspi_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180FlexSPIState *s = IMXRT1180_FLEXSPI(dev);

    s->bus = ssi_create_bus(dev, "spi");
    qdev_init_gpio_out_named(dev, s->cs, "cs", IMXRT1180_FLEXSPI_NUM_CS);
    fifo8_create(&s->rx, IMXRT1180_FLEXSPI_FIFO_BYTES);
    fifo8_create(&s->tx, IMXRT1180_FLEXSPI_FIFO_BYTES);

    memory_region_init_io(&s->iomem, OBJECT(dev), &flexspi_ops, s,
                          TYPE_IMXRT1180_FLEXSPI, IMXRT1180_FLEXSPI_REG_SIZE);
    memory_region_init_io(&s->ahb, OBJECT(dev), &flexspi_ahb_ops, s,
                          "imxrt1180-flexspi-ahb", s->ahb_size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->ahb);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const Property flexspi_properties[] = {
    DEFINE_PROP_UINT32("ahb-size", IMXRT1180FlexSPIState, ahb_size,
                       0x01000000),     /* 16 MiB: the EVK's FlexSPI1 A1 NOR */
};

static const VMStateDescription vmstate_flexspi = {
    .name = TYPE_IMXRT1180_FLEXSPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180FlexSPIState,
                             IMXRT1180_FLEXSPI_NUM_REGS),
        VMSTATE_BOOL(lut_unlocked, IMXRT1180FlexSPIState),
        VMSTATE_FIFO8(rx, IMXRT1180FlexSPIState),
        VMSTATE_FIFO8(tx, IMXRT1180FlexSPIState),
        VMSTATE_END_OF_LIST()
    },
};

static void flexspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = flexspi_realize;
    dc->vmsd = &vmstate_flexspi;
    device_class_set_legacy_reset(dc, flexspi_reset);
    device_class_set_props(dc, flexspi_properties);
    dc->desc = "i.MX RT1180 FlexSPI controller";
}

static const TypeInfo flexspi_types[] = {
    {
        .name          = TYPE_IMXRT1180_FLEXSPI,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180FlexSPIState),
        .class_init    = flexspi_class_init,
    },
};

DEFINE_TYPES(flexspi_types)
