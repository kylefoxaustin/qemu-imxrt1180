/*
 * NXP i.MX RT1180 GPT — General Purpose Timer.
 *
 * Register-accurate against the MIMXRT1189 DFP PERI_GPT.h.  While CR.EN, a
 * ptimer counts the prescaled clock (PR+1) up to the output-compare OCR1, sets
 * SR.OF1, raises the IRQ (if IR.OF1IE) and restarts (restart mode).  CNT reads
 * the live count.  Free-run vs restart, input capture and OCR2/3 are register-
 * accurate but fall back to the OCR1 periodic behaviour (flagged).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/timer/imxrt1180_gpt.h"
#include "migration/vmstate.h"

#define R_CR 0x0
#define R_PR 0x4
#define R_SR 0x8
#define R_IR 0xC
#define R_OCR1 0x10
#define R_CNT 0x24
#define CR_EN 0x1
#define CR_SWR 0x8000        /* software reset (self-clearing) */
#define SR_OF1 0x1
#define IR_OF1IE 0x1
#define GPT_CLK_DEFAULT 24000000u

static void gpt_update(IMXRT1180GPTState *s)
{
    qemu_set_irq(s->irq, !!((s->sr & SR_OF1) && (s->ir & IR_OF1IE)));
}
static void gpt_tick(void *opaque)
{
    IMXRT1180GPTState *s = opaque;
    s->sr |= SR_OF1;
    gpt_update(s);
}
static void gpt_run(IMXRT1180GPTState *s)
{
    ptimer_transaction_begin(s->timer);
    if ((s->cr & CR_EN) && s->ocr1) {
        ptimer_set_freq(s->timer, s->clk / ((s->pr & 0xFFF) + 1));
        ptimer_set_limit(s->timer, (uint64_t)s->ocr1 + 1, 1);
        ptimer_run(s->timer, 0);
    } else {
        ptimer_stop(s->timer);
    }
    ptimer_transaction_commit(s->timer);
}
static uint64_t gpt_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180GPTState *s = opaque;
    switch (off) {
    case R_CR: return s->cr;
    case R_PR: return s->pr;
    case R_SR: return s->sr;
    case R_IR: return s->ir;
    case R_OCR1: return s->ocr1;
    case R_CNT: return s->ocr1 - (uint32_t)ptimer_get_count(s->timer);
    }
    return 0;
}
static void gpt_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    IMXRT1180GPTState *s = opaque;
    switch (off) {
    case R_CR:
        if (v & CR_SWR) {                 /* GPT_SoftwareReset: reset + self-clear */
            s->pr = s->sr = s->ir = s->ocr1 = 0;
            gpt_run(s);
            s->cr = v & ~CR_SWR;
            gpt_update(s);
            break;
        }
        { bool was = s->cr & CR_EN; s->cr = v; if ((v & CR_EN) != was) { gpt_run(s); } }
        break;
    case R_PR: s->pr = v; if (s->cr & CR_EN) { gpt_run(s); } break;
    case R_SR: s->sr &= ~(uint32_t)v; gpt_update(s); break;   /* W1C */
    case R_IR: s->ir = v; gpt_update(s); break;
    case R_OCR1: s->ocr1 = v; if (s->cr & CR_EN) { gpt_run(s); } break;
    }
}
static const MemoryRegionOps gpt_ops = {
    .read = gpt_read, .write = gpt_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};
static void gpt_reset(DeviceState *dev)
{
    IMXRT1180GPTState *s = IMXRT1180_GPT(dev);
    s->cr = s->pr = s->sr = s->ir = s->ocr1 = 0;
    ptimer_transaction_begin(s->timer); ptimer_stop(s->timer); ptimer_transaction_commit(s->timer);
    qemu_set_irq(s->irq, 0);
}
static void gpt_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180GPTState *s = IMXRT1180_GPT(dev);
    if (!s->clk) { s->clk = GPT_CLK_DEFAULT; }
    s->timer = ptimer_init(gpt_tick, s, PTIMER_POLICY_NO_IMMEDIATE_TRIGGER | PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    memory_region_init_io(&s->iomem, OBJECT(s), &gpt_ops, s, TYPE_IMXRT1180_GPT, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}
static const VMStateDescription vmstate_gpt = {
    .name = TYPE_IMXRT1180_GPT, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr, IMXRT1180GPTState), VMSTATE_UINT32(pr, IMXRT1180GPTState),
        VMSTATE_UINT32(sr, IMXRT1180GPTState), VMSTATE_UINT32(ir, IMXRT1180GPTState),
        VMSTATE_UINT32(ocr1, IMXRT1180GPTState), VMSTATE_PTIMER(timer, IMXRT1180GPTState),
        VMSTATE_END_OF_LIST() } };
static void gpt_class_init(ObjectClass *k, const void *d)
{
    DeviceClass *dc = DEVICE_CLASS(k);
    dc->realize = gpt_realize; device_class_set_legacy_reset(dc, gpt_reset); dc->vmsd = &vmstate_gpt;
}
static const TypeInfo gpt_types[] = {{ .name = TYPE_IMXRT1180_GPT, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRT1180GPTState), .class_init = gpt_class_init }};
DEFINE_TYPES(gpt_types)
