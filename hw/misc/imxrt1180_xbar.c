/*
 * NXP i.MX RT1180 XBAR1 — inter-peripheral signal crossbar.
 *
 * Register-accurate against the MIMXRT1189 DFP PERI_XBAR_NUM_OUT221.h: the SEL[]
 * registers are 16-bit, each holding two 8-bit output-source selects (output N
 * is driven by the input whose index is in SEL[N/2] byte (N & 1)).  When an
 * input signal changes level, every output selecting it follows.
 *
 * This carries an eFlexPWM trigger edge to the LPADC hardware-trigger inputs so
 * the ADC samples synchronously with the PWM period (the FOC current-sense path:
 * XBAR_SetSignalsConnection(FlexpwmPwmOutTrig, Adc12HwTrig)).
 *
 * FIDELITY NOTE — the CTRL[] registers (per-output edge detection, DMA/interrupt
 * generation) are register-accurate storage but not behaviourally modelled: an
 * input level is propagated straight through to the selected outputs, so a
 * source pulse yields one output pulse (enough for edge-triggered consumers like
 * the ADC).  Edge/DMA modes are flagged, not faked.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/misc/imxrt1180_xbar.h"
#include "migration/vmstate.h"

#define XBAR_SEL_LAST  0xDC     /* SEL[110] is the last select register */

/* Input index driving output N (SEL[N/2] byte N&1). */
static unsigned xbar_source(IMXRT1180XBARState *s, unsigned out)
{
    uint16_t sel = s->regs[out / 2];       /* SEL registers start at offset 0 */
    return (out & 1) ? (sel >> 8) & 0xFF : sel & 0xFF;
}

static void xbar_input(void *opaque, int line, int level)
{
    IMXRT1180XBARState *s = IMXRT1180_XBAR(opaque);

    if (line < 0 || line >= IMXRT1180_XBAR_NIN) {
        return;
    }
    s->in_level[line] = level;
    for (unsigned o = 0; o < IMXRT1180_XBAR_NOUT; o++) {
        if (xbar_source(s, o) == (unsigned)line) {
            qemu_set_irq(s->out[o], level);
        }
    }
}

static uint64_t imxrt1180_xbar_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180XBARState *s = IMXRT1180_XBAR(opaque);

    if (offset + 2 > IMXRT1180_XBAR_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
    return s->regs[offset / 2];
}

static void imxrt1180_xbar_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    IMXRT1180XBARState *s = IMXRT1180_XBAR(opaque);

    if (offset + 2 > IMXRT1180_XBAR_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }
    s->regs[offset / 2] = (uint16_t)value;

    /* A SEL write re-routes its two outputs: refresh them to the new source. */
    if (offset <= XBAR_SEL_LAST) {
        unsigned base = (offset / 2) * 2;
        for (unsigned o = base; o < base + 2 && o < IMXRT1180_XBAR_NOUT; o++) {
            qemu_set_irq(s->out[o], s->in_level[xbar_source(s, o)]);
        }
    }
}

static const MemoryRegionOps imxrt1180_xbar_ops = {
    .read = imxrt1180_xbar_read,
    .write = imxrt1180_xbar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
    .impl.min_access_size = 2,
    .impl.max_access_size = 2,
};

static void imxrt1180_xbar_reset(DeviceState *dev)
{
    IMXRT1180XBARState *s = IMXRT1180_XBAR(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->in_level, 0, sizeof(s->in_level));
    for (unsigned o = 0; o < IMXRT1180_XBAR_NOUT; o++) {
        qemu_set_irq(s->out[o], 0);
    }
}

static void imxrt1180_xbar_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180XBARState *s = IMXRT1180_XBAR(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_xbar_ops, s,
                          TYPE_IMXRT1180_XBAR, IMXRT1180_XBAR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    qdev_init_gpio_out_named(dev, s->out, "xbar-out", IMXRT1180_XBAR_NOUT);
    qdev_init_gpio_in_named(dev, xbar_input, "xbar-in", IMXRT1180_XBAR_NIN);
}

/* Migration: re-drive every output from the restored routing + input levels */
static int vmstate_imxrt1180_xbar_post_load(void *opaque, int version_id)
{
    IMXRT1180XBARState *s = opaque;
    for (unsigned o = 0; o < IMXRT1180_XBAR_NOUT; o++) {
        qemu_set_irq(s->out[o], s->in_level[xbar_source(s, o)]);
    }
    return 0;
}

static const VMStateDescription vmstate_imxrt1180_xbar = {
    .name = TYPE_IMXRT1180_XBAR,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = vmstate_imxrt1180_xbar_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, IMXRT1180XBARState, IMXRT1180_XBAR_SIZE / 2),
        VMSTATE_BOOL_ARRAY(in_level, IMXRT1180XBARState, IMXRT1180_XBAR_NIN),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_xbar_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_xbar_realize;
    device_class_set_legacy_reset(dc, imxrt1180_xbar_reset);
    dc->vmsd = &vmstate_imxrt1180_xbar;
}

static const TypeInfo imxrt1180_xbar_types[] = {
    {
        .name          = TYPE_IMXRT1180_XBAR,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180XBARState),
        .class_init    = imxrt1180_xbar_class_init,
    },
};

DEFINE_TYPES(imxrt1180_xbar_types)
