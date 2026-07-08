/*
 * NXP i.MX RT1180 LPTMR — Low-Power Timer.
 *
 * Register-accurate against the MIMXRT1189 DFP PERI_LPTMR.h.  In time-counter
 * mode: while CSR.TEN, a ptimer counts the prescaled clock up to CMR, sets
 * CSR.TCF, raises the IRQ (if CSR.TIE) and reloads.  CNR reads the live count.
 * Pulse-counter / glitch-filter modes fall back to periodic (flagged).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/timer/imxrt1180_lptmr.h"
#include "migration/vmstate.h"

#define R_CSR 0x0
#define R_PSR 0x4
#define R_CMR 0x8
#define R_CNR 0xC
#define CSR_TEN 0x1
#define CSR_TIE 0x40
#define CSR_TCF 0x80
#define LPTMR_CLK_DEFAULT 24000000u

static void lptmr_update(IMXRT1180LPTMRState *s)
{
    qemu_set_irq(s->irq, !!((s->csr & CSR_TCF) && (s->csr & CSR_TIE)));
}
static void lptmr_tick(void *opaque)
{
    IMXRT1180LPTMRState *s = opaque;
    s->csr |= CSR_TCF;
    lptmr_update(s);
}
static void lptmr_run(IMXRT1180LPTMRState *s)
{
    ptimer_transaction_begin(s->timer);
    if (s->csr & CSR_TEN) {
        unsigned div = 1u << ((s->psr & 0x78) >> 3);       /* PSR.PRESCALE */
        if (!(s->psr & 0x4)) {                             /* PBYP: 0 = use prescaler */
            div = div ? div : 1;
        } else {
            div = 1;
        }
        ptimer_set_freq(s->timer, s->clk / div);
        ptimer_set_limit(s->timer, (uint64_t)(s->cmr & 0xFFFF) + 1, 1);
        ptimer_run(s->timer, 0);
    } else {
        ptimer_stop(s->timer);
    }
    ptimer_transaction_commit(s->timer);
}
static uint64_t lptmr_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180LPTMRState *s = opaque;
    switch (off) {
    case R_CSR: return s->csr;
    case R_PSR: return s->psr;
    case R_CMR: return s->cmr;
    case R_CNR: return (s->cmr & 0xFFFF) - (uint16_t)ptimer_get_count(s->timer);
    }
    return 0;
}
static void lptmr_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    IMXRT1180LPTMRState *s = opaque;
    switch (off) {
    case R_CSR: {
        bool was = s->csr & CSR_TEN;
        if (v & CSR_TCF) { s->csr &= ~CSR_TCF; }           /* W1C */
        s->csr = (s->csr & CSR_TCF) | (v & ~CSR_TCF);
        if ((s->csr & CSR_TEN) != was) { lptmr_run(s); }
        lptmr_update(s);
        break;
    }
    case R_PSR: s->psr = v; break;
    case R_CMR: s->cmr = v; if (s->csr & CSR_TEN) { lptmr_run(s); } break;
    }
}
static const MemoryRegionOps lptmr_ops = {
    .read = lptmr_read, .write = lptmr_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};
static void lptmr_reset(DeviceState *dev)
{
    IMXRT1180LPTMRState *s = IMXRT1180_LPTMR(dev);
    s->csr = s->psr = s->cmr = 0;
    ptimer_transaction_begin(s->timer); ptimer_stop(s->timer); ptimer_transaction_commit(s->timer);
    qemu_set_irq(s->irq, 0);
}
static void lptmr_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180LPTMRState *s = IMXRT1180_LPTMR(dev);
    if (!s->clk) { s->clk = LPTMR_CLK_DEFAULT; }
    s->timer = ptimer_init(lptmr_tick, s, PTIMER_POLICY_NO_IMMEDIATE_TRIGGER | PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
    memory_region_init_io(&s->iomem, OBJECT(s), &lptmr_ops, s, TYPE_IMXRT1180_LPTMR, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}
static const VMStateDescription vmstate_lptmr = {
    .name = TYPE_IMXRT1180_LPTMR, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(csr, IMXRT1180LPTMRState), VMSTATE_UINT32(psr, IMXRT1180LPTMRState),
        VMSTATE_UINT32(cmr, IMXRT1180LPTMRState), VMSTATE_PTIMER(timer, IMXRT1180LPTMRState),
        VMSTATE_END_OF_LIST() } };
static void lptmr_class_init(ObjectClass *k, const void *d)
{
    DeviceClass *dc = DEVICE_CLASS(k);
    dc->realize = lptmr_realize; device_class_set_legacy_reset(dc, lptmr_reset); dc->vmsd = &vmstate_lptmr;
}
static const TypeInfo lptmr_types[] = {{ .name = TYPE_IMXRT1180_LPTMR, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMXRT1180LPTMRState), .class_init = lptmr_class_init }};
DEFINE_TYPES(lptmr_types)
