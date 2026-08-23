/*
 * NXP i.MX RT1180 TMR — QuadTimer (4-channel 16-bit timer/counter).
 *
 * Register-accurate against the MIMXRT1189 DFP PERI_TMR.h.  Each channel is a
 * ptimer-backed 16-bit up-counter: while enabled (ENBL bit + CTRL.CM != 0) it
 * counts a prescaled clock (CTRL.PCS = internal /2^n for PCS 0..7), and on
 * reaching COMP1 it sets SCTRL.TCF (+ CSCTRL.TCF1), reloads from LOAD, and
 * raises the shared IRQ if SCTRL.TCFIE.  This covers the common periodic /
 * modulo-timer use (the FOC delay timer, the bubble demo's TMR5).
 *
 * FIDELITY NOTE — the non-internal count modes (external/secondary source,
 * quadrature decode, cascade, input capture, COMP2/alternating compare) are
 * register-accurate but fall back to the periodic COMP1 behaviour above; that
 * simplification is flagged, not faked.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/timer/imxrt1180_tmr.h"
#include "migration/vmstate.h"

/* Per-channel register offsets (channel stride 0x20). */
#define CH_STRIDE 0x20
#define R_COMP1   0x00
#define R_LOAD    0x06
#define R_CNTR    0x0A
#define R_CTRL    0x0C
#define R_SCTRL   0x0E
#define R_CSCTRL  0x14
#define R_ENBL    0x1E        /* module enable bits, in channel 0's block */

/* CTRL bits. */
#define CTRL_PCS_MASK   0x1E00
#define CTRL_PCS_SHIFT  9
#define CTRL_CM_MASK    0xE000
/* SCTRL bits. */
#define SCTRL_TCF       0x8000
#define SCTRL_TCFIE     0x4000
#define SCTRL_TOF       0x2000
#define SCTRL_TOFIE     0x1000
/* CSCTRL bits. */
#define CSCTRL_TCF1     0x0010      /* compare-1 flag                    */
#define CSCTRL_TCF2     0x0020      /* compare-2 flag                    */
#define CSCTRL_TCF1EN   0x0040      /* compare-1 interrupt enable        */
#define CSCTRL_TCF2EN   0x0080      /* compare-2 interrupt enable        */

static uint16_t *reg(IMXRT1180TMRState *s, unsigned ch, unsigned off)
{
    return &s->regs[(ch * CH_STRIDE + off) / 2];
}

static void tmr_update_irq(IMXRT1180TMRState *s)
{
    bool active = false;
    for (unsigned c = 0; c < IMXRT1180_TMR_NCHAN; c++) {
        uint16_t sc = *reg(s, c, R_SCTRL);
        uint16_t cs = *reg(s, c, R_CSCTRL);
        /*
         * Two independent compare-interrupt paths, and the QuadTimer driver picks
         * per use: the SCTRL timer-compare-flag (TCF/TCFIE) OR the CSCTRL compare-1/2
         * flags (TCF1/TCF1EN, TCF2/TCF2EN).  mc_pmsm's 1 ms slow loop enables the
         * IRQ through CSCTRL[TCF1EN] only -- checking SCTRL alone left it silent.
         */
        if (((sc & SCTRL_TCF)  && (sc & SCTRL_TCFIE)) ||
            ((sc & SCTRL_TOF)  && (sc & SCTRL_TOFIE)) ||
            ((cs & CSCTRL_TCF1) && (cs & CSCTRL_TCF1EN)) ||
            ((cs & CSCTRL_TCF2) && (cs & CSCTRL_TCF2EN))) {
            active = true;
        }
    }
    qemu_set_irq(s->irq, active);
}

static void tmr_ch_tick(void *opaque)
{
    IMXRT1180TMRChan *c = opaque;
    IMXRT1180TMRState *s = c->s;

    *reg(s, c->ch, R_SCTRL)  |= SCTRL_TCF;      /* compare flag */
    *reg(s, c->ch, R_CSCTRL) |= CSCTRL_TCF1;
    tmr_update_irq(s);
}

static void tmr_ch_update(IMXRT1180TMRState *s, unsigned ch)
{
    uint16_t ctrl = *reg(s, ch, R_CTRL);
    uint16_t enbl = *reg(s, 0, R_ENBL);
    bool running = (ctrl & CTRL_CM_MASK) && (enbl & (1u << ch));

    ptimer_transaction_begin(s->timer[ch]);
    if (running) {
        unsigned pcs = (ctrl & CTRL_PCS_MASK) >> CTRL_PCS_SHIFT;
        /*
         * PCS 0x8..0xF select the IP-bus clock divided by 2^(pcs-8) (0x8=/1 ..
         * 0xF=/128); 0x0..0x7 select external/secondary count sources we
         * approximate as the undivided bus clock.  mc_pmsm's slow loop uses
         * PCS=0xC (bus/16) -- reading it as /1 ran the 1 ms timer 16x fast.
         */
        unsigned div = pcs >= 8 ? (1u << (pcs - 8)) : 1;
        uint16_t comp1 = *reg(s, ch, R_COMP1);
        uint16_t load  = *reg(s, ch, R_LOAD);
        uint32_t period = (uint16_t)(comp1 - load) + 1u;   /* modulo count */
        uint32_t hz = imxrt1180_ccm_periph_hz(s->ccm, s->clk_root,
                                              "imxrt1180-tmr");
        if (hz) {
            ptimer_set_freq(s->timer[ch], hz / div);
            ptimer_set_limit(s->timer[ch], period, 1);
            ptimer_run(s->timer[ch], 0);             /* periodic */
        } else {
            ptimer_stop(s->timer[ch]);   /* no clock => no ticks.  Not 240 MHz. */
        }
    } else {
        ptimer_stop(s->timer[ch]);
    }
    ptimer_transaction_commit(s->timer[ch]);
}

static uint64_t imxrt1180_tmr_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180TMRState *s = IMXRT1180_TMR(opaque);

    if (offset + 2 > IMXRT1180_TMR_SIZE) {
        return 0;
    }
    unsigned ch = offset / CH_STRIDE, off = offset % CH_STRIDE;
    if (off == R_CNTR) {
        /* Live counter: LOAD + elapsed = COMP1 - remaining. */
        uint16_t comp1 = *reg(s, ch, R_COMP1);
        uint16_t rem = (uint16_t)ptimer_get_count(s->timer[ch]);
        return (uint16_t)(comp1 - rem);
    }
    return s->regs[offset / 2];
}

static void imxrt1180_tmr_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    IMXRT1180TMRState *s = IMXRT1180_TMR(opaque);
    uint16_t v = (uint16_t)value;

    if (offset + 2 > IMXRT1180_TMR_SIZE) {
        return;
    }
    unsigned ch = offset / CH_STRIDE, off = offset % CH_STRIDE;

    if (off == R_SCTRL) {
        /* TCF/TOF are cleared by writing 0 to them (write-1 keeps). */
        uint16_t old = s->regs[offset / 2];
        uint16_t keep = old & (SCTRL_TCF | SCTRL_TOF) & v;
        s->regs[offset / 2] = (v & ~(SCTRL_TCF | SCTRL_TOF)) | keep;
        tmr_update_irq(s);
        return;
    }
    if (off == R_CSCTRL) {
        /* TCF1/TCF2 are hardware-set compare flags cleared by writing 0 (the
         * mc_pmsm ISR does `CSCTRL &= ~TCF1_MASK`); re-evaluate the level IRQ. */
        uint16_t old = s->regs[offset / 2];
        uint16_t keep = old & (CSCTRL_TCF1 | CSCTRL_TCF2) & v;
        s->regs[offset / 2] = (v & ~(CSCTRL_TCF1 | CSCTRL_TCF2)) | keep;
        tmr_update_irq(s);
        return;
    }
    s->regs[offset / 2] = v;
    if (off == R_CTRL || off == R_ENBL || off == R_COMP1 || off == R_LOAD) {
        if (off == R_ENBL) {
            for (unsigned c = 0; c < IMXRT1180_TMR_NCHAN; c++) {
                tmr_ch_update(s, c);
            }
        } else {
            tmr_ch_update(s, ch);
        }
    }
}

static const MemoryRegionOps imxrt1180_tmr_ops = {
    .read = imxrt1180_tmr_read,
    .write = imxrt1180_tmr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 2,
    .impl.min_access_size = 2,
    .impl.max_access_size = 2,
};

static void imxrt1180_tmr_reset(DeviceState *dev)
{
    IMXRT1180TMRState *s = IMXRT1180_TMR(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /*
     * ENBL resets to 0x0001 (RM 72.5.1.15: "Enables the timer channel -- default"),
     * i.e. channel 0 is enabled out of reset and starts counting as soon as
     * CTRL[CM] != 0.  mc_pmsm's InitTMR1 relies on this: it never writes ENBL, so a
     * zero reset left its 1 ms slow-loop timer permanently disabled.
     */
    *reg(s, 0, R_ENBL) = 0x0001;
    for (unsigned c = 0; c < IMXRT1180_TMR_NCHAN; c++) {
        ptimer_transaction_begin(s->timer[c]);
        ptimer_stop(s->timer[c]);
        ptimer_transaction_commit(s->timer[c]);
    }
    qemu_set_irq(s->irq, 0);
}

static void imxrt1180_tmr_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180TMRState *s = IMXRT1180_TMR(dev);

    for (unsigned c = 0; c < IMXRT1180_TMR_NCHAN; c++) {
        s->chan[c].s = s;
        s->chan[c].ch = c;
        s->timer[c] = ptimer_init(tmr_ch_tick, &s->chan[c],
                                  PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                                  PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_tmr_ops, s,
                          TYPE_IMXRT1180_TMR, IMXRT1180_TMR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_imxrt1180_tmr = {
    .name = TYPE_IMXRT1180_TMR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, IMXRT1180TMRState, IMXRT1180_TMR_SIZE / 2),
        VMSTATE_PTIMER_ARRAY(timer, IMXRT1180TMRState, IMXRT1180_TMR_NCHAN),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * NO "clk" PROPERTY, AND NO DEFAULT.  This block used to carry a hardcoded
 * frequency behind `if (!clk) clk = DEFAULT;` -- and the SoC never set it, so the
 * FALLBACK WAS THE CLOCK, silently, at the wrong rate.  The frequency now comes
 * from CLOCK_ROOT[clk-root] in the CCM, read at the point of use.
 */
static const Property imxrt1180_tmr_properties[] = {
    DEFINE_PROP_LINK("ccm", IMXRT1180TMRState, ccm,
                     TYPE_IMXRT1180_CCM, IMXRT1180CCMState *),
    DEFINE_PROP_UINT32("clk-root", IMXRT1180TMRState, clk_root, 0),
};

static void imxrt1180_tmr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_tmr_realize;
    device_class_set_legacy_reset(dc, imxrt1180_tmr_reset);
    dc->vmsd = &vmstate_imxrt1180_tmr;
    device_class_set_props(dc, imxrt1180_tmr_properties);
}

static const TypeInfo imxrt1180_tmr_types[] = {
    {
        .name          = TYPE_IMXRT1180_TMR,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180TMRState),
        .class_init    = imxrt1180_tmr_class_init,
    },
};

DEFINE_TYPES(imxrt1180_tmr_types)
