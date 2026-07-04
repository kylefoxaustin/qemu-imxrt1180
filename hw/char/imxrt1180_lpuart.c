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

#define FIFO_RXEMPT 0x00400000u
#define FIFO_TXEMPT 0x00800000u

/* Reset values reported by the SDK's LPUART_GetInstance()/VERID probe. */
#define LPUART_VERID_VALUE  0x04010003u
#define LPUART_PARAM_VALUE  0x00000404u  /* TX/RX FIFO depth fields */

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
        r = s->rx_byte;
        if (offset == LPUART_DATA && s->rx_full) {
            s->rx_full = false;
            imxrt1180_lpuart_update_irq(s);
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
            s->ctrl = s->baud = s->fifo = s->water = 0;
            s->rx_full = false;
            imxrt1180_lpuart_update_irq(s);
        }
        break;
    case LPUART_PINCFG:
        s->pincfg = value;
        break;
    case LPUART_BAUD:
        s->baud = value;
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
        s->tosr = value;
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
    }
}

static void imxrt1180_lpuart_reset(DeviceState *dev)
{
    IMXRT1180LPUARTState *s = IMXRT1180_LPUART(dev);

    s->global = s->pincfg = s->baud = s->ctrl = 0;
    s->match = s->modir = s->fifo = s->water = 0;
    s->reir = s->teir = s->hdcr = s->tocr = s->tosr = 0;
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
