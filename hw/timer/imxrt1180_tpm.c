/*
 * NXP i.MX RT1180 TPM — Timer/PWM Module.
 *
 * Register-accurate against the MIMXRT1189 DFP PERI_TPM.h.  While SC.CMOD != 0,
 * a ptimer counts the prescaled clock (2^SC.PS) up to MOD, sets SC.TOF, raises
 * the IRQ (if SC.TOIE) and wraps.  CNT reads the live count.  The PWM channel
 * outputs and input-capture are register-accurate but not driven (flagged) --
 * this covers the timer/overflow use.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/timer/imxrt1180_tpm.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"

#define R_VERID 0x0
#define R_PARAM 0x4
#define R_SC  0x10
#define R_CNT 0x14
#define R_MOD 0x18
#define R_STATUS 0x1C
#define SC_PS 0x7
#define SC_CMOD 0x18
#define SC_TOIE 0x40
#define SC_TOF 0x80

static void tpm_update(IMXRT1180TPMState *s)
{
    qemu_set_irq(s->irq, !!((s->sc & SC_TOF) && (s->sc & SC_TOIE)));
}
static void tpm_tick(void *opaque)
{
    IMXRT1180TPMState *s = opaque;
    s->sc |= SC_TOF;
    tpm_update(s);
}
static void tpm_run(IMXRT1180TPMState *s)
{
    ptimer_transaction_begin(s->timer);
    if ((s->sc & SC_CMOD) && (s->mod & 0xFFFF)) {
        uint32_t hz = imxrt1180_ccm_periph_hz(s->ccm, s->clk_root,
                                              "imxrt1180-tpm");
        if (hz) {
            ptimer_set_freq(s->timer, hz / (1u << (s->sc & SC_PS)));
            ptimer_set_limit(s->timer, (uint64_t)(s->mod & 0xFFFF) + 1, 1);
            ptimer_run(s->timer, 0);
        } else {
            ptimer_stop(s->timer);      /* no clock => no ticks.  Not 24 MHz. */
        }
    } else {
        ptimer_stop(s->timer);
    }
    ptimer_transaction_commit(s->timer);
}
static uint64_t tpm_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180TPMState *s = opaque;
    switch (off) {
    case R_VERID: return 0x02000000;
    case R_PARAM: return 0x00060404;              /* 6 channels, FIFO fields */
    case R_SC: return s->sc;
    case R_MOD: return s->mod;
    case R_STATUS: return s->status;
    /*
     * A STOPPED COUNTER READS ZERO.  This synthesised MOD - ptimer_get_count()
     * unconditionally; with the counter halted ptimer_get_count() is 0, so CNT read
     * back MOD itself -- 0xFFFF out of reset, where silicon reads 0.  (The same
     * shape as eFlexPWM's CNT, which read 1.)  While CMOD == 0 the TPM counter does
     * not run, and a counter that is not running is at zero.
     */
    case R_CNT:
        if (!(s->sc & SC_CMOD)) {
            return 0;
        }
        return (s->mod & 0xFFFF) - (uint16_t)ptimer_get_count(s->timer);
    }
    /* CONTROLS[] channel regs + others: register-backed so read-back matches
     * (TPM_SetupPwm polls `while (cnv != base->CONTROLS[].CnV)`). */
    if (off + 4 <= sizeof(s->regs)) {
        return s->regs[off / 4];
    }
    return 0;
}
static void tpm_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    IMXRT1180TPMState *s = opaque;
    switch (off) {
    case R_SC: {
        uint32_t old = s->sc;
        if (v & SC_TOF) { old &= ~SC_TOF; }                 /* TOF W1C */
        s->sc = (old & SC_TOF) | (v & ~SC_TOF);
        tpm_run(s); tpm_update(s);
        break;
    }
    case R_CNT: tpm_run(s); break;                          /* any write resets count */
    case R_MOD: s->mod = v; if (s->sc & SC_CMOD) { tpm_run(s); } break;
    case R_STATUS: s->status &= ~(uint32_t)v; break;        /* W1C */
    default:
        if (off + 4 <= sizeof(s->regs)) { s->regs[off / 4] = v; }
        break;
    }
}
static const MemoryRegionOps tpm_ops = {
    .read = tpm_read, .write = tpm_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};
static void tpm_reset(DeviceState *dev)
{
    IMXRT1180TPMState *s = IMXRT1180_TPM(dev);
    s->sc = s->status = 0;
    /*
     * MOD resets to 0xFFFF -- the free-running full-range modulo.  We reset it to 0,
     * which makes the period 1 tick and the counter effectively DEAD AT RESET until
     * the guest writes MOD.  (91emulator hit the identical thing in their TPM.)
     */
    s->mod = 0x0000FFFF;
    memset(s->regs, 0, sizeof(s->regs));
    ptimer_transaction_begin(s->timer); ptimer_stop(s->timer); ptimer_transaction_commit(s->timer);
    qemu_set_irq(s->irq, 0);
}
static void tpm_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180TPMState *s = IMXRT1180_TPM(dev);
    s->timer = ptimer_init(tpm_tick, s, PTIMER_POLICY_NO_IMMEDIATE_TRIGGER | PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    memory_region_init_io(&s->iomem, OBJECT(s), &tpm_ops, s, TYPE_IMXRT1180_TPM, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}
/* Migration: re-assert the IRQ line from restored state (outputs aren't migrated). */
static int vmstate_tpm_post_load(void *opaque, int version_id)
{
    tpm_update(opaque);
    return 0;
}

static const VMStateDescription vmstate_tpm = {
    .name = TYPE_IMXRT1180_TPM, .version_id = 1, .minimum_version_id = 1,
    .post_load = vmstate_tpm_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sc, IMXRT1180TPMState), VMSTATE_UINT32(mod, IMXRT1180TPMState),
        VMSTATE_UINT32(status, IMXRT1180TPMState),
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180TPMState, 0x100/4),
        VMSTATE_PTIMER(timer, IMXRT1180TPMState),
        VMSTATE_END_OF_LIST() } };
/*
 * NO "clk" PROPERTY, AND NO DEFAULT.  This block used to carry a hardcoded
 * frequency behind `if (!clk) clk = DEFAULT;` -- and the SoC never set it, so the
 * FALLBACK WAS THE CLOCK, silently, at the wrong rate.  The frequency now comes
 * from CLOCK_ROOT[clk-root] in the CCM, read at the point of use.
 */
static const Property tpm_properties[] = {
    DEFINE_PROP_LINK("ccm", IMXRT1180TPMState, ccm,
                     TYPE_IMXRT1180_CCM, IMXRT1180CCMState *),
    DEFINE_PROP_UINT32("clk-root", IMXRT1180TPMState, clk_root, 0),
};

static void tpm_class_init(ObjectClass *k, const void *d)
{
    DeviceClass *dc = DEVICE_CLASS(k);
    dc->realize = tpm_realize; device_class_set_legacy_reset(dc, tpm_reset); dc->vmsd = &vmstate_tpm;
    device_class_set_props(dc, tpm_properties);
}
static const TypeInfo tpm_types[] = {{ .name = TYPE_IMXRT1180_TPM, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRT1180TPMState), .class_init = tpm_class_init }};
DEFINE_TYPES(tpm_types)
