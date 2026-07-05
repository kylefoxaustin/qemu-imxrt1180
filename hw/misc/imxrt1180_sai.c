/*
 * NXP MCX N SAI (Serial Audio Interface / I2S) — bring-up model.
 *
 * Models the SAI transmit and receive control/status registers (CMSIS
 * I2S_Type) so firmware audio init never hangs:
 *
 *   - TCSR/RCSR software-reset (SR) and FIFO-reset (FR) bits are momentary in
 *     real hardware; here they self-clear so the "set SR, wait for SR clear"
 *     init step terminates immediately.
 *   - The transmit FIFO always reads as having space (FWF warning flag set,
 *     FEF empty flag set), and the receive FIFO reads as empty (RCSR FWF/FEF
 *     reflect "no data, space available") so polling loops resolve.
 *   - The transmit/receive enable bits (TE/RE) read back exactly as written so
 *     "enable then confirm" sequences pass.
 *   - Status flags FRF/FWF/FEF/SEF/WSF are write-1-to-clear.
 *
 * The transmit/receive data and FIFO words are otherwise permissively backed.
 * Offsets and bit masks come from the MCXN947 CMSIS header (I2S_Type).  VERID
 * and PARAM are read-only constants; the VERID value is best-effort for this
 * SAI revision (firmware does not gate on it).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/imxrt1180_sai.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS I2S_Type). */
#define SAI_VERID   0x00    /* RO */
#define SAI_PARAM   0x04    /* RO */
#define SAI_TCSR    0x08    /* Transmit Control/Status */
#define SAI_TCR1    0x0C
#define SAI_TCR2    0x10
#define SAI_TCR3    0x14
#define SAI_TCR4    0x18
#define SAI_TCR5    0x1C
#define SAI_TDR0    0x20    /* TDR[2] @0x20..0x24, WO */
#define SAI_TFR0    0x40    /* TFR[2] @0x40..0x44, RO */
#define SAI_TMR     0x60
#define SAI_RCSR    0x88    /* Receive Control/Status */
#define SAI_RCR1    0x8C
#define SAI_RCR2    0x90
#define SAI_RCR3    0x94
#define SAI_RCR4    0x98
#define SAI_RCR5    0x9C
#define SAI_RDR0    0xA0    /* RDR[2] @0xA0..0xA4, RO */
#define SAI_RFR0    0xC0    /* RFR[2] @0xC0..0xC4, RO */
#define SAI_RMR     0xE0
#define SAI_MCR     0x100

/* TCSR/RCSR bit masks (shared layout for the two CSR registers). */
#define CSR_FRF     (1u << 16)  /* FIFO request flag        */
#define CSR_FWF     (1u << 17)  /* FIFO warning flag        */
#define CSR_FEF     (1u << 18)  /* FIFO error (underrun/overrun) flag */
#define CSR_SEF     (1u << 19)  /* sync error flag          */
#define CSR_WSF     (1u << 20)  /* word start flag          */
#define CSR_SR      (1u << 24)  /* software reset           */
#define CSR_FR      (1u << 25)  /* FIFO reset               */
#define CSR_BCE     (1u << 28)  /* bit clock enable         */
#define CSR_DBGE    (1u << 29)
#define CSR_STOPE   (1u << 30)
#define CSR_EN      (1u << 31)  /* TE for TCSR / RE for RCSR */

/* W1C flag bits within TCSR/RCSR. */
#define CSR_FLAGS_W1C  (CSR_FEF | CSR_SEF | CSR_WSF)

/*
 * Interrupt-enable bits sit 8 below their status flag (FRIE@8 enables FRF@16,
 * FWIE@9 enables FWF@17, FEIE@10 enables FEF@18, ...).  An interrupt is
 * requested when any (flag & matching-enable) is set.
 */
#define CSR_IE_TO_FLAG_SHIFT  8
#define CSR_STICKY_FLAGS  (CSR_FEF | CSR_SEF | CSR_WSF)

/* Best-effort constants (firmware does not gate boot on these). */
#define SAI_VERID_VALUE  0x03010000u   /* major=3, minor=1 (best-effort) */
#define SAI_PARAM_VALUE  0x00050302u   /* FIFO=32, channels=2 (best-effort) */

static void imxrt1180_sai_update_irq(IMXRT1180SAIState *s)
{
    uint32_t tcsr = s->regs[SAI_TCSR >> 2];
    uint32_t rcsr = s->regs[SAI_RCSR >> 2];
    /*
     * The transmit FIFO always reports space, so FRF/FWF are effectively
     * asserted whenever the transmitter is enabled (TE).  Plus any sticky
     * error flags that the guest has not cleared.  The receive FIFO is empty,
     * so only its sticky flags can interrupt.
     */
    uint32_t tflags = ((tcsr & CSR_EN) ? (CSR_FRF | CSR_FWF) : 0) |
                      (tcsr & CSR_STICKY_FLAGS);
    uint32_t rflags = rcsr & CSR_STICKY_FLAGS;
    bool tx = (((tflags >> 16) & 0x1Fu) & ((tcsr >> CSR_IE_TO_FLAG_SHIFT) & 0x1Fu)) != 0;
    bool rx = (((rflags >> 16) & 0x1Fu) & ((rcsr >> CSR_IE_TO_FLAG_SHIFT) & 0x1Fu)) != 0;

    qemu_set_irq(s->irq, tx || rx);
}

static uint64_t imxrt1180_sai_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180SAIState *s = IMXRT1180_SAI(opaque);
    uint32_t v = (off < IMXRT1180_SAI_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case SAI_VERID:
        return SAI_VERID_VALUE;
    case SAI_PARAM:
        return SAI_PARAM_VALUE;
    case SAI_TCSR:
        /*
         * Soft-reset bits are momentary: never read back as set.  Always
         * advertise FIFO space (FRF/FWF) and a clean-empty FIFO (FEF clear).
         */
        v &= ~(CSR_SR | CSR_FR);
        v |= CSR_FRF | CSR_FWF;
        v &= ~CSR_FEF;
        return v;
    case SAI_RCSR:
        /*
         * Receive side: report no soft-reset in progress and an empty RX FIFO
         * (no data ready, no overrun) so receive polling resolves cleanly.
         */
        v &= ~(CSR_SR | CSR_FR);
        v &= ~(CSR_FRF | CSR_FWF | CSR_FEF);
        return v;
    case SAI_TFR0:
    case SAI_TFR0 + 4:
        /* Transmit FIFO read/write pointers equal: FIFO empty (space free). */
        return 0;
    case SAI_RFR0:
    case SAI_RFR0 + 4:
        /* Receive FIFO read/write pointers equal: FIFO empty (no data). */
        return 0;
    case SAI_RDR0:
    case SAI_RDR0 + 4:
        return 0;   /* no received data */
    default:
        return v;
    }
}

static void imxrt1180_sai_write(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    IMXRT1180SAIState *s = IMXRT1180_SAI(opaque);
    uint32_t val = value;

    if (off >= IMXRT1180_SAI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case SAI_VERID:
    case SAI_PARAM:
        return;   /* read-only */
    case SAI_TFR0:
    case SAI_TFR0 + 4:
    case SAI_RFR0:
    case SAI_RFR0 + 4:
    case SAI_RDR0:
    case SAI_RDR0 + 4:
        return;   /* read-only FIFO/data registers */
    case SAI_TCSR:
    case SAI_RCSR: {
        uint32_t cur = s->regs[off >> 2];
        /* Status flag bits are write-1-to-clear; clear those the guest set. */
        cur &= ~(val & CSR_FLAGS_W1C);
        /* Control bits (including TE/RE) latch from the write. */
        cur = (cur & CSR_FLAGS_W1C) | (val & ~CSR_FLAGS_W1C);
        /* Soft-reset bits self-clear immediately. */
        cur &= ~(CSR_SR | CSR_FR);
        s->regs[off >> 2] = cur;
        imxrt1180_sai_update_irq(s);
        return;
    }
    case SAI_TDR0:
    case SAI_TDR0 + 4:
        /* Transmit data is accepted and discarded (no audio sink modelled). */
        return;
    default:
        s->regs[off >> 2] = val;
        return;
    }
}

static const MemoryRegionOps imxrt1180_sai_ops = {
    .read = imxrt1180_sai_read,
    .write = imxrt1180_sai_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void imxrt1180_sai_reset(DeviceState *dev)
{
    IMXRT1180SAIState *s = IMXRT1180_SAI(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imxrt1180_sai_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180SAIState *s = IMXRT1180_SAI(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_sai_ops, s,
                          TYPE_IMXRT1180_SAI, IMXRT1180_SAI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_imxrt1180_sai = {
    .name = TYPE_IMXRT1180_SAI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180SAIState, IMXRT1180_SAI_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_sai_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_sai_realize;
    device_class_set_legacy_reset(dc, imxrt1180_sai_reset);
    dc->vmsd = &vmstate_imxrt1180_sai;
}

static const TypeInfo imxrt1180_sai_types[] = {
    {
        .name          = TYPE_IMXRT1180_SAI,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180SAIState),
        .class_init    = imxrt1180_sai_class_init,
    },
};

DEFINE_TYPES(imxrt1180_sai_types)
