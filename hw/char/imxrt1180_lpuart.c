/*
 * NXP i.MX RT1180 LPUART (console model)
 *
 * Standard NXP LPUART register block: TX is synchronous (always ready), RX is a
 * single-entry holding register fed by the chardev backend.  Offsets and bit
 * positions VERIFIED against the MIMXRT1189 CMSIS PERI_LPUART.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/char/imxrt1180_lpuart.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"  /* DEFINE_PROP_CHR */
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* --- LPUART register offsets ---------------------------------------------- */
#define LPUART_VERID    0x00  /* RO */
#define LPUART_PARAM    0x04  /* RO */
#define LPUART_GLOBAL   0x08
#define LPUART_PINCFG   0x0C
#define LPUART_BAUD     0x10
#define LPUART_STAT     0x14
#define LPUART_CTRL     0x18
#define LPUART_DATA     0x1C
#define LPUART_MATCH    0x20
#define LPUART_MODIR    0x24
#define LPUART_FIFO     0x28
#define LPUART_WATER    0x2C
#define LPUART_DATARO   0x30  /* RO */
#define LPUART_REIR     0x48
#define LPUART_TEIR     0x4C
#define LPUART_HDCR     0x50
#define LPUART_TOCR     0x58
#define LPUART_TOSR     0x5C
#define LPUART_TIMEOUT0 0x60
#define LPUART_TIMEOUT3 0x6C

/* --- Register bits (CMSIS-verified) --------------------------------------- */
#define GLOBAL_RST  0x00000002u

#define STAT_OR     0x00080000u
#define STAT_IDLE   0x00100000u
#define STAT_RDRF   0x00200000u
#define STAT_TC     0x00400000u
#define STAT_TDRE   0x00800000u

#define CTRL_RE     0x00040000u
#define CTRL_TE     0x00080000u
#define CTRL_RIE    0x00200000u
#define CTRL_TCIE   0x00400000u
#define CTRL_TIE    0x00800000u

#define DATA_RXEMPT 0x00001000u  /* PERI_LPUART.h DATA_RXEMPT_MASK */
#define FIFO_RXEMPT 0x00400000u
#define FIFO_TXEMPT 0x00800000u

/* Reset values reported by the SDK's LPUART_GetInstance()/VERID probe. */
#define LPUART_VERID_VALUE  0x04010003u
#define LPUART_PARAM_VALUE  0x00000404u  /* TX/RX FIFO depth fields */

/* BAUD DMA-enable bits — PERI_LPUART.h (RT1180): TDMAE=23, RDMAE=21. */
#define BAUD_RDMAE  0x00200000u
#define BAUD_TDMAE  0x00800000u

/*
 * Drive the two eDMA request lines.
 *
 * TX: this model transmits synchronously, so TDRE is permanently set -- the TX
 * request is asserted whenever the guest enables it. That is honest: our TX FIFO
 * genuinely never fills, so a real "wait for TDRE" would be a wait on a condition
 * that is always true.
 * RX: asserted only while a byte is actually in the holding register (RDRF). A
 * DMA channel armed on LPUART RX therefore moves EXACTLY as many bytes as arrive
 * on the wire -- if the line were hardwired asserted, the channel would happily
 * copy the same stale byte CITER times and the test would still "pass".
 */
static void imxrt1180_lpuart_update_dma(IMXRT1180LPUARTState *s)
{
    qemu_set_irq(s->dma_tx_req, !!(s->baud & BAUD_TDMAE));
    qemu_set_irq(s->dma_rx_req, (s->baud & BAUD_RDMAE) && s->rx_full);
}

/*
 * Interrupt condition: TX data-register-empty and transmit-complete are always
 * asserted in this model (writes are synchronous), so TIE/TCIE assert
 * immediately; RIE asserts while the rx holding register is full.
 */
static void imxrt1180_lpuart_update_irq(IMXRT1180LPUARTState *s)
{
    bool tx = s->ctrl & (CTRL_TIE | CTRL_TCIE);
    bool rx = (s->ctrl & CTRL_RIE) && s->rx_full;

    qemu_set_irq(s->irq, tx || rx);
}

static uint64_t imxrt1180_lpuart_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180LPUARTState *s = IMXRT1180_LPUART(opaque);
    uint32_t r = 0;

    switch (offset) {
    case LPUART_VERID:
        r = LPUART_VERID_VALUE;
        break;
    case LPUART_PARAM:
        r = LPUART_PARAM_VALUE;
        break;
    case LPUART_GLOBAL:
        r = s->global;
        break;
    case LPUART_PINCFG:
        r = s->pincfg;
        break;
    case LPUART_BAUD:
        r = s->baud;
        break;
    case LPUART_STAT:
        /* TX always ready; RDRF reflects the 1-byte rx holding register. */
        r = STAT_TDRE | STAT_TC;
        if (s->rx_full) {
            r |= STAT_RDRF;
        }
        break;
    case LPUART_CTRL:
        r = s->ctrl;
        break;
    case LPUART_DATA:
    case LPUART_DATARO:
        /*
         * RXEMPT (bit 12) -- WITHOUT IT, AN EMPTY RECEIVER HANDS OUT A PHANTOM NUL.
         *
         * This returned the raw byte and nothing else, so with no character received
         * DATA read 0x0000: RXEMPT CLEAR, data 0x00.  To a driver that is not "the
         * receiver is empty" -- IT IS THE BYTE 0x00, AND IT IS VALID.  The RM resets
         * DATA to 0x1000 for exactly this reason.
         *
         *   AN UNMODELLED BIT IS NOT A FREE BIT.  IT STILL ANSWERS, AND ZERO IS AN
         *   ANSWER.  (91emulator and mcxn947qemu found the same thing in LPI2C's
         *   MRDR alias the same day: "I was telling the guest the receive FIFO HAS
         *   DATA when nothing had been received.")
         */
        r = s->rx_byte | (s->rx_full ? 0 : DATA_RXEMPT);
        if (offset == LPUART_DATA && s->rx_full) {
            s->rx_full = false;
            imxrt1180_lpuart_update_irq(s);
            imxrt1180_lpuart_update_dma(s);
            /* Holding register free again — tell the chardev to resume input,
             * or a continuous RX stream stalls after one byte. */
            qemu_chr_fe_accept_input(&s->chr);
        }
        break;
    case LPUART_MATCH:
        r = s->match;
        break;
    case LPUART_MODIR:
        r = s->modir;
        break;
    case LPUART_FIFO:
        r = s->fifo | FIFO_TXEMPT;
        if (!s->rx_full) {
            r |= FIFO_RXEMPT;
        }
        break;
    case LPUART_WATER:
        r = s->water;
        break;
    case LPUART_REIR:
        r = s->reir;
        break;
    case LPUART_TEIR:
        r = s->teir;
        break;
    case LPUART_HDCR:
        r = s->hdcr;
        break;
    case LPUART_TOCR:
        r = s->tocr;
        break;
    case LPUART_TOSR:
        r = s->tosr;
        break;
    case LPUART_TIMEOUT0 ... LPUART_TIMEOUT3:
        r = s->timeout[(offset - LPUART_TIMEOUT0) >> 2];
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled read @0x%03" HWADDR_PRIx "\n",
                      __func__, offset);
        break;
    }
    return r;
}

static void imxrt1180_lpuart_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IMXRT1180LPUARTState *s = IMXRT1180_LPUART(opaque);
    uint8_t ch;

    switch (offset) {
    case LPUART_GLOBAL:
        s->global = value;
        if (value & GLOBAL_RST) {
            /* A software reset restores the RESET VALUES, not zero. */
            s->ctrl = s->water = 0;
            s->baud = 0x0F000004;
            s->fifo = 0x00C00033;
            s->rx_full = false;
            imxrt1180_lpuart_update_irq(s);
            imxrt1180_lpuart_update_dma(s);   /* BAUD cleared => requests drop */
        }
        break;
    case LPUART_PINCFG:
        s->pincfg = value;
        break;
    case LPUART_BAUD:
        s->baud = value;
        imxrt1180_lpuart_update_dma(s);
        break;
    case LPUART_STAT:
        /* STAT is computed on read; W1C flag writes have no standalone state. */
        break;
    case LPUART_CTRL:
        s->ctrl = value;
        imxrt1180_lpuart_update_irq(s);
        break;
    case LPUART_DATA:
        ch = value & 0xFF;
        if (s->ctrl & CTRL_TE) {          /* honour TE: only transmit if enabled */
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
        }
        imxrt1180_lpuart_update_irq(s);
        break;
    case LPUART_MATCH:
        s->match = value;
        break;
    case LPUART_MODIR:
        s->modir = value;
        break;
    case LPUART_FIFO:
        s->fifo = value;
        break;
    case LPUART_WATER:
        s->water = value;
        break;
    case LPUART_REIR:
        s->reir = value;
        break;
    case LPUART_TEIR:
        s->teir = value;
        break;
    case LPUART_HDCR:
        s->hdcr = value;
        break;
    case LPUART_TOCR:
        s->tocr = value;
        break;
    case LPUART_TOSR:
        /*
         * ================= TOSR IS W1C, AND I NEARLY SHIPPED A HANG =================
         *
         * The four timeout flags reset SET (TOSR = 0xF) and are WRITE-1-TO-CLEAR.
         * This was a PLAIN STORE.  With TOSR seeded to 0xF -- which I did today, to
         * make the reset-value gate green -- a driver clearing flag 0 writes 0x1, a
         * plain store SETS it, the driver polls for it to drop, AND SPINS FOREVER.
         *
         * Before the seed, TOSR read 0 and the flag was already clear, so the driver
         * never entered that loop.  I TOOK A BENIGN ZERO AND TURNED IT INTO A HANG,
         * AND THE GATE CALLED IT AN IMPROVEMENT.
         *
         *   ⭐ SEED CONFIG.  *IMPLEMENT* STATUS.
         *      A reset value is not a number you return -- it is the state a WORKING
         *      REGISTER STARTS IN.  If the register has behaviour, the reset value is
         *      only half the fix, and shipping the other half is not optional.
         *      (91emulator, 2026-07-13, who caught this on their own TOSR and warned
         *      the fleet hours after I had already done it.)
         */
        s->tosr &= ~(uint32_t)value;
        break;
    case LPUART_TIMEOUT0 ... LPUART_TIMEOUT3:
        s->timeout[(offset - LPUART_TIMEOUT0) >> 2] = value;
        break;
    case LPUART_VERID:
    case LPUART_PARAM:
    case LPUART_DATARO:
        /* read-only */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled write @0x%03" HWADDR_PRIx
                      " = 0x%08x\n", __func__, offset, (uint32_t)value);
        break;
    }
}

static const MemoryRegionOps imxrt1180_lpuart_ops = {
    .read = imxrt1180_lpuart_read,
    .write = imxrt1180_lpuart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * Accept 1/2/4-byte access.  A driver moving data over eDMA bursts the DATA
     * register one byte at a time; an over-strict 4-byte-only window would make
     * the memory core SILENTLY DROP those byte writes (fleet lesson: the
     * i.MX95 LPSPI-over-eDMA silent-drop).  impl.min=1 routes each byte to the
     * handler; the data register holds its value in the low byte.
     */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static int imxrt1180_lpuart_can_rx(void *opaque)
{
    IMXRT1180LPUARTState *s = IMXRT1180_LPUART(opaque);
    return (s->ctrl & CTRL_RE) && !s->rx_full;
}

static void imxrt1180_lpuart_rx(void *opaque, const uint8_t *buf, int size)
{
    IMXRT1180LPUARTState *s = IMXRT1180_LPUART(opaque);

    if (size > 0) {
        s->rx_byte = buf[0];
        s->rx_full = true;
        imxrt1180_lpuart_update_irq(s);
        imxrt1180_lpuart_update_dma(s);
    }
}

static void imxrt1180_lpuart_reset(DeviceState *dev)
{
    IMXRT1180LPUARTState *s = IMXRT1180_LPUART(dev);

    s->global = s->pincfg = s->ctrl = 0;
    s->match = s->modir = s->water = 0;
    s->reir = s->teir = s->hdcr = s->tocr = 0;

    /*
     * RESET VALUES from the RM's cold-POR column (tests/imxrt1180-reset-values).
     * A memset-to-zero is a CLAIM ABOUT EVERY BIT, and firmware read-modify-writes
     * all three of these -- BAUD in particular: LPUART_SetBaudRate() reads BAUD,
     * masks in OSR/SBR and writes it back, so a zeroed BAUD launders our lie into
     * the guest's own configuration.
     *   BAUD = 0x0F000004  (OSR = 15, SBR = 4)
     *   FIFO = 0x00C00033  (TX/RX FIFO size fields + the empty flags)
     *   TOSR = 0x0000000F
     */
    s->baud = 0x0F000004;
    s->fifo = 0x00C00033;
    s->tosr = 0x0000000F;
    s->timeout[0] = s->timeout[1] = s->timeout[2] = s->timeout[3] = 0;
    s->rx_byte = 0;
    s->rx_full = false;
}

static void imxrt1180_lpuart_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180LPUARTState *s = IMXRT1180_LPUART(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_lpuart_ops, s,
                          TYPE_IMXRT1180_LPUART, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(dev, &s->dma_tx_req, "dma-tx-req", 1);
    qdev_init_gpio_out_named(dev, &s->dma_rx_req, "dma-rx-req", 1);

    qemu_chr_fe_set_handlers(&s->chr, imxrt1180_lpuart_can_rx,
                             imxrt1180_lpuart_rx, NULL, NULL, s, NULL, true);
}

static const VMStateDescription vmstate_imxrt1180_lpuart = {
    .name = TYPE_IMXRT1180_LPUART,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(global, IMXRT1180LPUARTState),
        VMSTATE_UINT32(pincfg, IMXRT1180LPUARTState),
        VMSTATE_UINT32(baud, IMXRT1180LPUARTState),
        VMSTATE_UINT32(ctrl, IMXRT1180LPUARTState),
        VMSTATE_UINT32(match, IMXRT1180LPUARTState),
        VMSTATE_UINT32(modir, IMXRT1180LPUARTState),
        VMSTATE_UINT32(fifo, IMXRT1180LPUARTState),
        VMSTATE_UINT32(water, IMXRT1180LPUARTState),
        VMSTATE_UINT32(reir, IMXRT1180LPUARTState),
        VMSTATE_UINT32(teir, IMXRT1180LPUARTState),
        VMSTATE_UINT32(hdcr, IMXRT1180LPUARTState),
        VMSTATE_UINT32(tocr, IMXRT1180LPUARTState),
        VMSTATE_UINT32(tosr, IMXRT1180LPUARTState),
        VMSTATE_UINT32_ARRAY(timeout, IMXRT1180LPUARTState, 4),
        VMSTATE_UINT8(rx_byte, IMXRT1180LPUARTState),
        VMSTATE_BOOL(rx_full, IMXRT1180LPUARTState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imxrt1180_lpuart_properties[] = {
    DEFINE_PROP_CHR("chardev", IMXRT1180LPUARTState, chr),
};

static void imxrt1180_lpuart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_lpuart_realize;
    device_class_set_legacy_reset(dc, imxrt1180_lpuart_reset);
    dc->vmsd = &vmstate_imxrt1180_lpuart;
    device_class_set_props(dc, imxrt1180_lpuart_properties);
}

static const TypeInfo imxrt1180_lpuart_types[] = {
    {
        .name          = TYPE_IMXRT1180_LPUART,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180LPUARTState),
        .class_init    = imxrt1180_lpuart_class_init,
    },
};

DEFINE_TYPES(imxrt1180_lpuart_types)
