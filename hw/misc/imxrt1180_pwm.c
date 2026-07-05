/*
 * NXP i.MX RT1180 eFlexPWM — enhanced FlexPWM (motor-control PWM).
 *
 * The RT1180's headline motor-control block.  Each PWM module (PWM1..4) has four
 * submodules; each is a 16-bit up-counter that runs from INIT to VAL1, reloads,
 * and drives a complementary PWMA/PWMB output pair whose edges are set by
 * VAL2..VAL5.  This model is register-accurate against the MIMXRT1189 CMSIS
 * PERI_PWM.h and reproduces the behaviour a field-oriented-control (FOC) loop
 * depends on:
 *
 *   - INIT and VAL0..VAL5 are double-buffered: writes land in a shadow and are
 *     committed to the live registers on MCTRL.LDOK (matching PWM_SetPwmLdok),
 *     so a control loop can update all six compare values atomically.
 *   - MCTRL.RUN[sm] starts/stops each submodule's counter (a QEMU ptimer at the
 *     submodule reload rate = modulo / (pwm_clk / prescaler)).
 *   - Each reload sets STS.RF and, if INTEN.RIE, raises the submodule IRQ — the
 *     periodic interrupt that clocks the FOC current loop.  STS is W1C.
 *   - The PWMA duty cycle is computed from the live compare values and kept in
 *     `duty[]` (per-mille) as the model's honest output.
 *
 * FIDELITY NOTE — there is no motor/plant behind the outputs: the duty cycle is
 * computed and observable, but nothing consumes it and no quadrature encoder
 * (EQDC) responds to it, so a closed FOC loop will not spin a virtual rotor.
 * The ADC-sync output triggers (TCTRL) are stored but not yet routed through the
 * XBAR to the ADC.  Both are flagged future work (a virtual-motor plant + XBAR),
 * not silently faked.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/imxrt1180_pwm.h"
#include "migration/vmstate.h"

/* Submodule register offsets (within a 0x60 stride). */
#define SM_STRIDE   0x60
#define R_CNT       0x00        /* Counter (RO)                */
#define R_INIT      0x02        /* Initial count (buffered)    */
#define R_CTRL2     0x04
#define R_CTRL      0x06
#define R_VAL0      0x0A        /* Value 0..5 (buffered)       */
#define R_VAL1      0x0E
#define R_VAL2      0x12
#define R_VAL3      0x16
#define R_VAL4      0x1A
#define R_VAL5      0x1E
#define R_STS       0x24
#define R_INTEN     0x26
#define R_DMAEN     0x28
#define R_TCTRL     0x2A

/* Module-level register offsets. */
#define R_OUTEN     0x180
#define R_MCTRL     0x188
#define R_FSTS      0x18E

/* CTRL bits. */
#define CTRL_LDMOD      0x0004
#define CTRL_PRSC_MASK  0x0070
#define CTRL_PRSC_SHIFT 4
/* STS / INTEN bits. */
#define STS_CMPF        0x003F
#define STS_RF          0x1000
#define INTEN_CMPIE     0x003F
#define INTEN_RIE       0x1000
/* MCTRL bits. */
#define MCTRL_LDOK      0x000F
#define MCTRL_CLDOK     0x00F0
#define MCTRL_RUN       0x0F00
#define MCTRL_RUN_SHIFT 8

/* VAL0..VAL5 live at these submodule offsets. */
static const unsigned val_off[6] = {
    R_VAL0, R_VAL1, R_VAL2, R_VAL3, R_VAL4, R_VAL5
};

#define PWM_CLK_DEFAULT 200000000u   /* nominal fast-peripheral clock (Hz) */

static uint16_t *smreg(IMXRT1180PWMState *s, unsigned sm, unsigned off)
{
    return &s->regs[(sm * SM_STRIDE + off) / 2];
}

static uint16_t pwm_modulo(IMXRT1180PWMState *s, unsigned sm)
{
    uint16_t init = *smreg(s, sm, R_INIT);
    uint16_t val1 = *smreg(s, sm, R_VAL1);
    return (uint16_t)(val1 - init) + 1u;   /* period in counter ticks */
}

static void pwm_compute_duty(IMXRT1180PWMState *s, unsigned sm)
{
    uint16_t modulo = pwm_modulo(s, sm);
    uint16_t on = (uint16_t)(*smreg(s, sm, R_VAL3) - *smreg(s, sm, R_VAL2));
    s->duty[sm] = modulo ? (uint16_t)((uint32_t)on * 1000u / modulo) : 0;
}

/* Commit the shadow (buffered) INIT/VAL registers for one submodule. */
static void pwm_commit(IMXRT1180PWMState *s, unsigned sm)
{
    *smreg(s, sm, R_INIT) = s->buf_init[sm];
    for (int i = 0; i < 6; i++) {
        *smreg(s, sm, val_off[i]) = s->buf_val[sm][i];
    }
    pwm_compute_duty(s, sm);
}

static void pwm_update_irq(IMXRT1180PWMState *s, unsigned sm)
{
    uint16_t sts = *smreg(s, sm, R_STS);
    uint16_t inten = *smreg(s, sm, R_INTEN);
    bool active = ((sts & STS_RF) && (inten & INTEN_RIE)) ||
                  (sts & inten & STS_CMPF);
    qemu_set_irq(s->irq_sm[sm], active);
}

static void pwm_sm_start(IMXRT1180PWMState *s, unsigned sm)
{
    uint16_t modulo = pwm_modulo(s, sm);
    uint16_t ctrl = *smreg(s, sm, R_CTRL);
    uint32_t prescale = 1u << ((ctrl & CTRL_PRSC_MASK) >> CTRL_PRSC_SHIFT);

    if (modulo == 0) {
        return;
    }
    ptimer_transaction_begin(s->timer[sm]);
    ptimer_set_freq(s->timer[sm], s->pwm_clk / prescale);
    ptimer_set_limit(s->timer[sm], modulo, 1);
    ptimer_run(s->timer[sm], 0);           /* periodic reload */
    ptimer_transaction_commit(s->timer[sm]);
}

static void pwm_sm_stop(IMXRT1180PWMState *s, unsigned sm)
{
    ptimer_transaction_begin(s->timer[sm]);
    ptimer_stop(s->timer[sm]);
    ptimer_transaction_commit(s->timer[sm]);
}

/* ptimer expiry = submodule counter reload. */
static void pwm_sm_tick(void *opaque)
{
    IMXRT1180PWMSub *sub = opaque;
    IMXRT1180PWMState *s = sub->pwm;
    unsigned sm = sub->idx;

    *smreg(s, sm, R_STS) |= STS_RF;        /* reload flag */
    pwm_compute_duty(s, sm);
    pwm_update_irq(s, sm);
}

static uint64_t imxrt1180_pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180PWMState *s = IMXRT1180_PWM(opaque);

    if (offset + 2 > IMXRT1180_PWM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    /* Submodule CNT: report the live up-counter position. */
    if (offset < IMXRT1180_PWM_NSM * SM_STRIDE &&
        (offset % SM_STRIDE) == R_CNT) {
        unsigned sm = offset / SM_STRIDE;
        uint16_t init = *smreg(s, sm, R_INIT);
        uint16_t modulo = pwm_modulo(s, sm);
        uint16_t down = (uint16_t)ptimer_get_count(s->timer[sm]);
        return (uint16_t)(init + (modulo - down));
    }
    return s->regs[offset / 2];
}

static void imxrt1180_pwm_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    IMXRT1180PWMState *s = IMXRT1180_PWM(opaque);
    uint16_t v = (uint16_t)value;

    if (offset + 2 > IMXRT1180_PWM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }

    /* Submodule register writes. */
    if (offset < IMXRT1180_PWM_NSM * SM_STRIDE) {
        unsigned sm  = offset / SM_STRIDE;
        unsigned reg = offset % SM_STRIDE;

        if (reg == R_INIT) {
            s->buf_init[sm] = v;             /* buffered until LDOK */
            return;
        }
        for (int i = 0; i < 6; i++) {
            if (reg == val_off[i]) {
                s->buf_val[sm][i] = v;       /* buffered until LDOK */
                return;
            }
        }
        if (reg == R_STS) {
            *smreg(s, sm, R_STS) &= ~v;      /* W1C */
            pwm_update_irq(s, sm);
            return;
        }
        if (reg == R_CNT) {
            return;                          /* CNT is read-only */
        }
        s->regs[offset / 2] = v;
        if (reg == R_INTEN) {
            pwm_update_irq(s, sm);
        }
        return;
    }

    /* Module-level registers. */
    if (offset == R_MCTRL) {
        uint16_t run_old = s->regs[R_MCTRL / 2] & MCTRL_RUN;

        /* CLDOK clears LDOK request bits (no commit). */
        uint16_t cldok = (v & MCTRL_CLDOK) >> 4;
        /* LDOK commits the buffered registers for the selected submodules. */
        uint16_t ldok = v & MCTRL_LDOK;
        uint16_t run_new = v & MCTRL_RUN;

        for (unsigned sm = 0; sm < IMXRT1180_PWM_NSM; sm++) {
            if (ldok & (1u << sm) && !(cldok & (1u << sm))) {
                pwm_commit(s, sm);
                if (run_new & (1u << (sm + MCTRL_RUN_SHIFT))) {
                    pwm_sm_start(s, sm);     /* live update of a running SM */
                }
            }
        }
        /* Store MCTRL with LDOK auto-cleared (load completes immediately). */
        s->regs[R_MCTRL / 2] = (v & ~MCTRL_LDOK);

        for (unsigned sm = 0; sm < IMXRT1180_PWM_NSM; sm++) {
            uint16_t bit = 1u << (sm + MCTRL_RUN_SHIFT);
            if ((run_new & bit) && !(run_old & bit)) {
                pwm_sm_start(s, sm);
                if (s->duty[sm] == 0) {
                    pwm_compute_duty(s, sm);
                }
                qemu_log_mask(LOG_UNIMP, "%s: SM%u running (duty %u%%); no motor "
                    "plant/encoder responds to the outputs (flagged)\n",
                    __func__, sm, s->duty[sm] / 10);
            } else if (!(run_new & bit) && (run_old & bit)) {
                pwm_sm_stop(s, sm);
            }
        }
        return;
    }
    s->regs[offset / 2] = v;
}

static const MemoryRegionOps imxrt1180_pwm_ops = {
    .read = imxrt1180_pwm_read,
    .write = imxrt1180_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
    .impl.min_access_size = 2,
    .impl.max_access_size = 2,
};

static void imxrt1180_pwm_reset(DeviceState *dev)
{
    IMXRT1180PWMState *s = IMXRT1180_PWM(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->buf_init, 0, sizeof(s->buf_init));
    memset(s->buf_val, 0, sizeof(s->buf_val));
    memset(s->duty, 0, sizeof(s->duty));
    for (unsigned sm = 0; sm < IMXRT1180_PWM_NSM; sm++) {
        ptimer_transaction_begin(s->timer[sm]);
        ptimer_stop(s->timer[sm]);
        ptimer_transaction_commit(s->timer[sm]);
        qemu_set_irq(s->irq_sm[sm], 0);
    }
    qemu_set_irq(s->irq_fault, 0);
}

static void imxrt1180_pwm_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180PWMState *s = IMXRT1180_PWM(dev);

    if (s->pwm_clk == 0) {
        s->pwm_clk = PWM_CLK_DEFAULT;
    }
    for (unsigned sm = 0; sm < IMXRT1180_PWM_NSM; sm++) {
        s->sub[sm].pwm = s;
        s->sub[sm].idx = sm;
        s->timer[sm] = ptimer_init(pwm_sm_tick, &s->sub[sm],
                                   PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                                   PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_pwm_ops, s,
                          TYPE_IMXRT1180_PWM, IMXRT1180_PWM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (unsigned sm = 0; sm < IMXRT1180_PWM_NSM; sm++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq_sm[sm]);
    }
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq_fault);
}

static const VMStateDescription vmstate_imxrt1180_pwm = {
    .name = TYPE_IMXRT1180_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, IMXRT1180PWMState, IMXRT1180_PWM_SIZE / 2),
        VMSTATE_UINT16_ARRAY(buf_init, IMXRT1180PWMState, IMXRT1180_PWM_NSM),
        VMSTATE_UINT16_2DARRAY(buf_val, IMXRT1180PWMState, IMXRT1180_PWM_NSM, 6),
        VMSTATE_UINT16_ARRAY(duty, IMXRT1180PWMState, IMXRT1180_PWM_NSM),
        VMSTATE_PTIMER_ARRAY(timer, IMXRT1180PWMState, IMXRT1180_PWM_NSM),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imxrt1180_pwm_properties[] = {
    DEFINE_PROP_UINT32("pwm-clk", IMXRT1180PWMState, pwm_clk, 0),
};

static void imxrt1180_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_pwm_realize;
    device_class_set_legacy_reset(dc, imxrt1180_pwm_reset);
    dc->vmsd = &vmstate_imxrt1180_pwm;
    device_class_set_props(dc, imxrt1180_pwm_properties);
}

static const TypeInfo imxrt1180_pwm_types[] = {
    {
        .name          = TYPE_IMXRT1180_PWM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180PWMState),
        .class_init    = imxrt1180_pwm_class_init,
    },
};

DEFINE_TYPES(imxrt1180_pwm_types)
