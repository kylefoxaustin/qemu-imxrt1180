/*
 * NXP i.MX RT1180 LPSPI — Low-Power SPI, controller (master) mode.
 *
 * Register-accurate against the MIMXRT1189 CMSIS PERI_LPSPI.h.  Drives a real
 * QEMU SSIBus so SPI device models (flash, sensors) respond.  Transfers run
 * synchronously on the TDR write (QEMU SSI is synchronous): TDF stays set and
 * the received word is available in the rx FIFO (RDF) immediately.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/ssi/imxrt1180_lpspi.h"
#include "migration/vmstate.h"

/* Register offsets. */
#define LPSPI_VERID 0x00
#define LPSPI_PARAM 0x04
#define LPSPI_CR    0x10
#define LPSPI_SR    0x14
#define LPSPI_IER   0x18
#define LPSPI_DER   0x1C
#define LPSPI_CFGR0 0x20
#define LPSPI_CFGR1 0x24
#define LPSPI_DMR0  0x30
#define LPSPI_DMR1  0x34
#define LPSPI_CCR   0x40
#define LPSPI_CCR1  0x44
#define LPSPI_FCR   0x58
#define LPSPI_FSR   0x5C
#define LPSPI_TCR   0x60
#define LPSPI_TDR   0x64
#define LPSPI_RSR   0x70
#define LPSPI_RDR   0x74
#define LPSPI_RDROR 0x78

/* CR bits. */
#define CR_MEN 0x1
#define CR_RST 0x2
#define CR_RTF 0x100
#define CR_RRF 0x200

/* SR bits. */
#define SR_TDF 0x1
#define SR_RDF 0x2
#define SR_WCF 0x100
#define SR_FCF 0x200
#define SR_TCF 0x400
#define SR_MBF 0x01000000
#define SR_STICKY_MASK 0x00003F00u   /* WCF/FCF/TCF/TEF/REF/DMF (W1C) */
#define SR_INT_MASK    0x00003F03u

/* TCR fields. */
#define TCR_FRAMESZ_MASK 0xFFF
#define TCR_TXMSK 0x40000
#define TCR_RXMSK 0x80000
#define TCR_CONT  0x200000
#define TCR_PCS_SHIFT 24
#define TCR_PCS_MASK  0x3

#define RSR_RXEMPTY 0x2

static uint32_t lpspi_sr(IMXRT1180LPSPIState *s)
{
    uint32_t v = s->sr_sticky;
    if (s->cr & CR_MEN) {
        v |= SR_TDF;                 /* tx FIFO always has room */
    }
    if (s->rx_count) {
        v |= SR_RDF;
    }
    return v;
}

static void lpspi_update_irq(IMXRT1180LPSPIState *s)
{
    qemu_set_irq(s->irq, (lpspi_sr(s) & s->ier & SR_INT_MASK) != 0);
}

static void lpspi_set_cs(IMXRT1180LPSPIState *s, int pcs, bool select)
{
    /* CS lines are active-low by default; assert = drive 0. */
    if (pcs >= 0 && pcs < IMXRT1180_LPSPI_NUMCS) {
        qemu_set_irq(s->cs_lines[pcs], select ? 0 : 1);
    }
}

/* Shift one TDR frame out to the SSI slave and capture the received word. */
static void lpspi_transfer(IMXRT1180LPSPIState *s, uint32_t tx)
{
    unsigned bits = (s->tcr & TCR_FRAMESZ_MASK) + 1;
    unsigned nbytes = bits < 8 ? 1 : (bits + 7) / 8;
    int pcs = (s->tcr >> TCR_PCS_SHIFT) & TCR_PCS_MASK;
    uint32_t rx = 0;

    if (s->cs_active != pcs) {
        if (s->cs_active >= 0) {
            lpspi_set_cs(s, s->cs_active, false);
        }
        lpspi_set_cs(s, pcs, true);
        s->cs_active = pcs;
    }

    for (int b = nbytes - 1; b >= 0; b--) {         /* MSB byte first */
        uint8_t out = (s->tcr & TCR_TXMSK) ? 0xFF : (tx >> (b * 8)) & 0xFF;
        uint8_t in = ssi_transfer(s->bus, out);
        rx = (rx << 8) | in;
    }

    if (!(s->tcr & TCR_RXMSK)) {                     /* capture unless masked */
        if (s->rx_count < IMXRT1180_LPSPI_FIFO) {
            s->rx_fifo[(s->rx_head + s->rx_count) % IMXRT1180_LPSPI_FIFO] = rx;
            s->rx_count++;
        }
    }

    s->sr_sticky |= SR_WCF | SR_FCF;
    if (!(s->tcr & TCR_CONT)) {                      /* frame ends -> drop CS */
        lpspi_set_cs(s, pcs, false);
        s->cs_active = -1;
        s->sr_sticky |= SR_TCF;
    }
    lpspi_update_irq(s);
}

static uint64_t imxrt1180_lpspi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180LPSPIState *s = opaque;

    switch (offset) {
    case LPSPI_VERID:
        return 0x02000004;
    case LPSPI_PARAM:
        return 0x00040404;           /* PCSNUM=4, TXFIFO=RXFIFO=2^4 */
    case LPSPI_CR:
        return s->cr;
    case LPSPI_SR:
        return lpspi_sr(s);
    case LPSPI_IER:
        return s->ier;
    case LPSPI_DER:
        return s->der;
    case LPSPI_CFGR0:
    case LPSPI_CFGR1:
        return s->cfgr[(offset - LPSPI_CFGR0) / 4];
    case LPSPI_DMR0:
    case LPSPI_DMR1:
        return s->dmr[(offset - LPSPI_DMR0) / 4];
    case LPSPI_CCR:
        return s->ccr[0];
    case LPSPI_CCR1:
        return s->ccr[1];
    case LPSPI_FCR:
        return s->fcr;
    case LPSPI_FSR:
        return (uint32_t)s->rx_count << 16;   /* RXCOUNT; TXCOUNT = 0 */
    case LPSPI_TCR:
        return s->tcr;
    case LPSPI_RSR:
        return s->rx_count ? 0 : RSR_RXEMPTY;
    case LPSPI_RDR:
    case LPSPI_RDROR: {
        if (s->rx_count == 0) {
            return 0;
        }
        uint32_t d = s->rx_fifo[s->rx_head];
        if (offset == LPSPI_RDR) {
            s->rx_head = (s->rx_head + 1) % IMXRT1180_LPSPI_FIFO;
            s->rx_count--;
            lpspi_update_irq(s);
        }
        return d;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @0x%02x\n",
                      __func__, (unsigned)offset);
        return 0;
    }
}

static void imxrt1180_lpspi_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    IMXRT1180LPSPIState *s = opaque;
    uint32_t v = value;

    switch (offset) {
    case LPSPI_CR:
        s->cr = v & (CR_MEN | CR_RST | 0x8 | 0x4);
        if (v & CR_RST) {
            s->sr_sticky = 0;
            s->rx_head = s->rx_count = 0;
        }
        if (v & CR_RRF) {
            s->rx_head = s->rx_count = 0;
        }
        lpspi_update_irq(s);
        break;
    case LPSPI_SR:
        s->sr_sticky &= ~(v & SR_STICKY_MASK);
        lpspi_update_irq(s);
        break;
    case LPSPI_IER:
        s->ier = v;
        lpspi_update_irq(s);
        break;
    case LPSPI_DER:
        s->der = v;
        break;
    case LPSPI_CFGR0:
    case LPSPI_CFGR1:
        s->cfgr[(offset - LPSPI_CFGR0) / 4] = v;
        break;
    case LPSPI_DMR0:
    case LPSPI_DMR1:
        s->dmr[(offset - LPSPI_DMR0) / 4] = v;
        break;
    case LPSPI_CCR:
        s->ccr[0] = v;
        break;
    case LPSPI_CCR1:
        s->ccr[1] = v;
        break;
    case LPSPI_FCR:
        s->fcr = v;
        break;
    case LPSPI_TCR:
        s->tcr = v;
        break;
    case LPSPI_TDR:
        if (s->cr & CR_MEN) {
            lpspi_transfer(s, v);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @0x%02x=0x%08x\n",
                      __func__, (unsigned)offset, v);
        break;
    }
}

static const MemoryRegionOps imxrt1180_lpspi_ops = {
    .read = imxrt1180_lpspi_read,
    .write = imxrt1180_lpspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_lpspi_reset(DeviceState *dev)
{
    IMXRT1180LPSPIState *s = IMXRT1180_LPSPI(dev);

    s->cr = s->sr_sticky = s->ier = s->der = s->fcr = s->tcr = 0;
    memset(s->cfgr, 0, sizeof(s->cfgr));
    memset(s->dmr, 0, sizeof(s->dmr));
    memset(s->ccr, 0, sizeof(s->ccr));
    s->rx_head = s->rx_count = 0;
    s->cs_active = -1;
    for (int i = 0; i < IMXRT1180_LPSPI_NUMCS; i++) {
        qemu_set_irq(s->cs_lines[i], 1);   /* deassert (active-low) */
    }
    qemu_set_irq(s->irq, 0);
}

static void imxrt1180_lpspi_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180LPSPIState *s = IMXRT1180_LPSPI(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_lpspi_ops, s,
                          TYPE_IMXRT1180_LPSPI, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    for (int i = 0; i < IMXRT1180_LPSPI_NUMCS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->cs_lines[i]);
    }
    s->bus = ssi_create_bus(dev, "spi");
    s->cs_active = -1;
}

static const VMStateDescription vmstate_imxrt1180_lpspi = {
    .name = TYPE_IMXRT1180_LPSPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr, IMXRT1180LPSPIState),
        VMSTATE_UINT32(sr_sticky, IMXRT1180LPSPIState),
        VMSTATE_UINT32(ier, IMXRT1180LPSPIState),
        VMSTATE_UINT32(der, IMXRT1180LPSPIState),
        VMSTATE_UINT32_ARRAY(cfgr, IMXRT1180LPSPIState, 2),
        VMSTATE_UINT32_ARRAY(dmr, IMXRT1180LPSPIState, 2),
        VMSTATE_UINT32_ARRAY(ccr, IMXRT1180LPSPIState, 2),
        VMSTATE_UINT32(fcr, IMXRT1180LPSPIState),
        VMSTATE_UINT32(tcr, IMXRT1180LPSPIState),
        VMSTATE_INT32(cs_active, IMXRT1180LPSPIState),
        VMSTATE_UINT32_ARRAY(rx_fifo, IMXRT1180LPSPIState, IMXRT1180_LPSPI_FIFO),
        VMSTATE_UINT8(rx_head, IMXRT1180LPSPIState),
        VMSTATE_UINT8(rx_count, IMXRT1180LPSPIState),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_lpspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_lpspi_realize;
    device_class_set_legacy_reset(dc, imxrt1180_lpspi_reset);
    dc->vmsd = &vmstate_imxrt1180_lpspi;
}

static const TypeInfo imxrt1180_lpspi_types[] = {
    {
        .name          = TYPE_IMXRT1180_LPSPI,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180LPSPIState),
        .class_init    = imxrt1180_lpspi_class_init,
    },
};

DEFINE_TYPES(imxrt1180_lpspi_types)
