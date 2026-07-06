/*
 * NXP i.MX RT1180 LPADC — 12/16-bit SAR ADC.
 *
 * The motor-control analog front-end.  Register-accurate against the MIMXRT1189
 * DFP PERI_ADC.h, with a working conversion engine:
 *
 *   - CTRL: soft reset + FIFO resets self-clear; CAL_REQ reports calibration
 *     complete immediately (STAT.CAL_RDY + GCR.RDY), so LPADC_DoAutoCalibration
 *     returns.
 *   - A trigger — SWTRIG bit t, or a rising edge on hardware trigger input t (to
 *     be routed from an eFlexPWM edge through the XBAR) — runs the command chain
 *     selected by TCTRL[t].TCMD (following each command's CMDH.NEXT), pushing one
 *     tagged result (VALID | trigger-source | loop | data) per command into a
 *     result FIFO.
 *   - Reading RESFIFO[n] pops the oldest result; FCTRL[n].FCOUNT and STAT.RDYn
 *     track the fill level against the watermark and raise the IRQ (IE.FWMIEn).
 *
 * FIDELITY NOTE — there is no analog front-end and no motor plant, so a
 * conversion cannot measure a real voltage/current.  Each result is a fixed
 * mid-scale placeholder (flagged once via LOG_UNIMP), i.e. the reading of an
 * un-energised sensor — NOT a fabricated current.  A virtual-motor plant that
 * turns PWM duty + rotor angle into real phase-current samples is flagged future
 * work; until then a FOC loop closes on ~zero measured current, consistent with
 * "no plant".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/misc/imxrt1180_adc.h"
#include "migration/vmstate.h"

/* Register offsets. */
#define R_VERID     0x00
#define R_PARAM     0x04
#define R_CTRL      0x10
#define R_STAT      0x14
#define R_IE        0x18
#define R_DE        0x1C
#define R_CFG       0x20
#define R_SWTRIG    0x34
#define R_TSTAT     0x38
#define R_TCTRL0    0xA0    /* [8], step 4  */
#define R_FCTRL0    0xE0    /* [2], step 4  */
#define R_GCC0      0xF0    /* [2], step 4  */
#define R_GCR0      0xF8    /* [2], step 4  */
#define R_CMD0      0x100   /* CMDL/CMDH [15], step 8 */
#define R_RESFIFO0  0x300   /* [2], step 4  */

/* CTRL bits. */
#define CTRL_ADCEN     0x00000001
#define CTRL_RST       0x00000002
#define CTRL_CAL_REQ   0x00000008
#define CTRL_RSTFIFO0  0x00000100
#define CTRL_RSTFIFO1  0x00000200
/* STAT bits. */
#define STAT_RDY0      0x00000001
#define STAT_FOF0      0x00000002
#define STAT_RDY1      0x00000004
#define STAT_FOF1      0x00000008
#define STAT_TCOMP_INT 0x00000200
#define STAT_CAL_RDY   0x00000400
#define STAT_WFLAGS    (STAT_FOF0 | STAT_FOF1 | STAT_TCOMP_INT) /* W1C */
/* IE bits. */
#define IE_FWMIE0      0x00000001
#define IE_FWMIE1      0x00000004
/* TCTRL / FCTRL / CMD / RESFIFO fields. */
#define TCTRL_HTEN     0x00000001
#define TCTRL_TCMD_SHIFT 24
#define TCTRL_TCMD_MASK  0x0F000000
#define FCTRL_FWMARK_SHIFT 16
#define FCTRL_FWMARK_MASK  0x000F0000
#define CMDH_LOOP_SHIFT  16
#define CMDH_LOOP_MASK   0x000F0000
#define CMDH_NEXT_SHIFT  24
#define CMDH_NEXT_MASK   0x0F000000
#define RESFIFO_VALID    0x80000000
#define RESFIFO_TSRC_SHIFT 16
#define RESFIFO_LOOPCNT_SHIFT 20
#define GCR_RDY          0x01000000

/* Un-energised mid-scale placeholder (no analog front-end / no plant). */
#define ADC_RESULT_PLACEHOLDER 0x8000

#define REG(s, off) ((s)->regs[(off) / 4])

static uint32_t fctrl_fwmark(IMXRT1180ADCState *s, int f)
{
    return (REG(s, R_FCTRL0 + 4 * f) & FCTRL_FWMARK_MASK) >> FCTRL_FWMARK_SHIFT;
}

static void adc_update_irq(IMXRT1180ADCState *s)
{
    uint32_t stat = 0;
    if (s->fifo[0].count > fctrl_fwmark(s, 0)) {
        stat |= STAT_RDY0;
    }
    if (s->fifo[1].count > fctrl_fwmark(s, 1)) {
        stat |= STAT_RDY1;
    }
    /* Keep the sticky/W1C flags, refresh the level-driven RDY bits. */
    REG(s, R_STAT) = (REG(s, R_STAT) & ~(STAT_RDY0 | STAT_RDY1)) | stat;

    uint32_t ie = REG(s, R_IE);
    bool active = ((stat & STAT_RDY0) && (ie & IE_FWMIE0)) ||
                  ((stat & STAT_RDY1) && (ie & IE_FWMIE1));
    qemu_set_irq(s->irq, active);
}

static void adc_fifo_push(IMXRT1180ADCState *s, int f, uint32_t entry)
{
    IMXRT1180ADCFifo *fi = &s->fifo[f];
    if (fi->count >= IMXRT1180_ADC_FIFO_DEPTH) {
        REG(s, R_STAT) |= (f == 0) ? STAT_FOF0 : STAT_FOF1;   /* overflow */
        return;
    }
    fi->data[(fi->head + fi->count) % IMXRT1180_ADC_FIFO_DEPTH] = entry;
    fi->count++;
}

static uint32_t adc_fifo_pop(IMXRT1180ADCState *s, int f)
{
    IMXRT1180ADCFifo *fi = &s->fifo[f];
    if (fi->count == 0) {
        return 0;                       /* VALID clear -> empty */
    }
    uint32_t v = fi->data[fi->head];
    fi->head = (fi->head + 1) % IMXRT1180_ADC_FIFO_DEPTH;
    fi->count--;
    return v;
}

/* Run the command chain for trigger source t. */
static void adc_run_trigger(IMXRT1180ADCState *s, int t)
{
    if (!(REG(s, R_CTRL) & CTRL_ADCEN)) {
        return;
    }
    uint32_t tctrl = REG(s, R_TCTRL0 + 4 * t);
    unsigned cmd = (tctrl & TCTRL_TCMD_MASK) >> TCTRL_TCMD_SHIFT;   /* 1-based */
    int guard = 0;

    while (cmd != 0 && cmd <= 15 && guard++ < 32) {
        uint32_t cmdh = REG(s, R_CMD0 + 8 * (cmd - 1) + 4);
        unsigned loops = ((cmdh & CMDH_LOOP_MASK) >> CMDH_LOOP_SHIFT) + 1;

        unsigned ch = REG(s, R_CMD0 + 8 * (cmd - 1)) & 0x1F;   /* CMDL.ADCH */
        uint16_t sample = s->channel_input[ch];                /* plant, or 0x8000 */
        if (sample == ADC_RESULT_PLACEHOLDER && !s->no_afe_logged) {
            s->no_afe_logged = true;
            qemu_log_mask(LOG_UNIMP, "%s: no plant drives channel %u -- result is "
                "a fixed mid-scale placeholder (flagged)\n", __func__, ch);
        }
        for (unsigned l = 0; l < loops; l++) {
            uint32_t entry = RESFIFO_VALID |
                             ((uint32_t)t << RESFIFO_TSRC_SHIFT) |
                             ((uint32_t)l << RESFIFO_LOOPCNT_SHIFT) |
                             sample;
            adc_fifo_push(s, 0, entry);   /* single-ended results -> FIFO0 */
        }
        cmd = (cmdh & CMDH_NEXT_MASK) >> CMDH_NEXT_SHIFT;   /* 0 = end of chain */
    }
    REG(s, R_STAT) |= STAT_TCOMP_INT;
    adc_update_irq(s);
}

static void adc_hw_trigger(void *opaque, int line, int level)
{
    IMXRT1180ADCState *s = IMXRT1180_ADC(opaque);

    if (level && line < IMXRT1180_ADC_NTRIG &&
        (REG(s, R_TCTRL0 + 4 * line) & TCTRL_HTEN)) {
        adc_run_trigger(s, line);        /* rising edge from the XBAR */
    }
}

static uint64_t imxrt1180_adc_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180ADCState *s = IMXRT1180_ADC(opaque);

    if (offset + 4 > IMXRT1180_ADC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    if (offset == R_VERID) {
        return 0x02090000;               /* LPADC v2.9 */
    }
    if (offset >= R_RESFIFO0 && offset < R_RESFIFO0 + 4 * IMXRT1180_ADC_NFIFO) {
        int f = (offset - R_RESFIFO0) / 4;
        uint32_t v = adc_fifo_pop(s, f);
        adc_update_irq(s);
        return v;
    }
    if (offset >= R_FCTRL0 && offset < R_FCTRL0 + 4 * IMXRT1180_ADC_NFIFO) {
        int f = (offset - R_FCTRL0) / 4;
        /* FCOUNT (low 5 bits) reflects the live fill level. */
        return (REG(s, offset) & ~0x1Fu) | s->fifo[f].count;
    }
    return REG(s, offset);
}

static void imxrt1180_adc_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    IMXRT1180ADCState *s = IMXRT1180_ADC(opaque);
    uint32_t v = (uint32_t)value;

    if (offset + 4 > IMXRT1180_ADC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }

    switch (offset) {
    case R_CTRL:
        if (v & CTRL_RST) {                       /* soft reset */
            for (int f = 0; f < IMXRT1180_ADC_NFIFO; f++) {
                s->fifo[f].head = s->fifo[f].count = 0;
            }
            REG(s, R_STAT) = 0;
        }
        if (v & CTRL_RSTFIFO0) {
            s->fifo[0].head = s->fifo[0].count = 0;
        }
        if (v & CTRL_RSTFIFO1) {
            s->fifo[1].head = s->fifo[1].count = 0;
        }
        if (v & CTRL_CAL_REQ) {                   /* calibration completes now */
            REG(s, R_STAT) |= STAT_CAL_RDY;
            REG(s, R_GCR0)     = GCR_RDY | 0x10000;   /* unity gain, ready */
            REG(s, R_GCR0 + 4) = GCR_RDY | 0x10000;
        }
        /* Store CTRL with the self-clearing bits masked off. */
        REG(s, R_CTRL) = v & ~(CTRL_RST | CTRL_RSTFIFO0 | CTRL_RSTFIFO1 |
                               CTRL_CAL_REQ);
        adc_update_irq(s);
        return;
    case R_STAT:
        REG(s, R_STAT) &= ~(v & STAT_WFLAGS);     /* W1C */
        adc_update_irq(s);
        return;
    case R_SWTRIG:
        for (int t = 0; t < IMXRT1180_ADC_NTRIG; t++) {
            if (v & (1u << t)) {
                adc_run_trigger(s, t);
            }
        }
        return;                                    /* SWTRIG self-clears */
    case R_IE:
        REG(s, R_IE) = v;
        adc_update_irq(s);
        return;
    default:
        REG(s, offset) = v;
        return;
    }
}

static const MemoryRegionOps imxrt1180_adc_ops = {
    .read = imxrt1180_adc_read,
    .write = imxrt1180_adc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_adc_reset(DeviceState *dev)
{
    IMXRT1180ADCState *s = IMXRT1180_ADC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    for (int f = 0; f < IMXRT1180_ADC_NFIFO; f++) {
        s->fifo[f].head = s->fifo[f].count = 0;
    }
    for (int c = 0; c < 32; c++) {
        s->channel_input[c] = ADC_RESULT_PLACEHOLDER;   /* neutral until a plant */
    }
    s->no_afe_logged = false;
    qemu_set_irq(s->irq, 0);
}

void imxrt1180_adc_set_channel_input(IMXRT1180ADCState *s, unsigned ch,
                                     uint16_t code)
{
    if (ch < 32) {
        s->channel_input[ch] = code;
    }
}

static void imxrt1180_adc_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180ADCState *s = IMXRT1180_ADC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_adc_ops, s,
                          TYPE_IMXRT1180_ADC, IMXRT1180_ADC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    /* Hardware trigger inputs (wired from the XBAR / eFlexPWM edges). */
    qdev_init_gpio_in_named(dev, adc_hw_trigger, "adc-trig",
                            IMXRT1180_ADC_NTRIG);
}

static const VMStateDescription vmstate_imxrt1180_adc_fifo = {
    .name = "imxrt1180-adc-fifo",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(data, IMXRT1180ADCFifo, IMXRT1180_ADC_FIFO_DEPTH),
        VMSTATE_UINT8(head, IMXRT1180ADCFifo),
        VMSTATE_UINT8(count, IMXRT1180ADCFifo),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_imxrt1180_adc = {
    .name = TYPE_IMXRT1180_ADC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180ADCState, IMXRT1180_ADC_SIZE / 4),
        VMSTATE_STRUCT_ARRAY(fifo, IMXRT1180ADCState, IMXRT1180_ADC_NFIFO, 1,
                             vmstate_imxrt1180_adc_fifo, IMXRT1180ADCFifo),
        VMSTATE_UINT16_ARRAY(channel_input, IMXRT1180ADCState, 32),
        VMSTATE_BOOL(no_afe_logged, IMXRT1180ADCState),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_adc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_adc_realize;
    device_class_set_legacy_reset(dc, imxrt1180_adc_reset);
    dc->vmsd = &vmstate_imxrt1180_adc;
}

static const TypeInfo imxrt1180_adc_types[] = {
    {
        .name          = TYPE_IMXRT1180_ADC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180ADCState),
        .class_init    = imxrt1180_adc_class_init,
    },
};

DEFINE_TYPES(imxrt1180_adc_types)
