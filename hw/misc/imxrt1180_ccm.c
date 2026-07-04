/*
 * NXP i.MX RT1180 CCM — Clock Controller Module.
 *
 * Register-backed (clock roots hold their written MUX/DIV), with two derived
 * behaviours the SDK clock code depends on:
 *
 *   - LPCG (low-power clock gate) STATUS0.ON mirrors DIRECT.ON, so the
 *     "enable clock then poll it on" loops (CLOCK_ControlGate) complete.
 *   - OBSERVE[].FREQUENCY_CURRENT reports a NOMINAL non-zero value, so
 *     CLOCK_GetFreqFromObs() (which spin-waits FREQUENCY_CURRENT != 0 and
 *     returns it * (CCM_OBS_DIV+1)) terminates and yields a usable clock.
 *
 * FIDELITY NOTE: this does NOT compute the real clock tree.  The OBSERVE
 * frequency is a nominal placeholder (enough for a driver to derive a sane
 * baud/divider); it is not the true measured Hz of the selected clock.  A
 * consumer that needs an exact frequency is getting an approximation — flagged
 * here rather than presented as measured silicon.
 *
 * Offsets/counts verified against the MIMXRT1189 CMSIS PERI_CCM.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/imxrt1180_ccm.h"
#include "migration/vmstate.h"

/* LPCG array: DIRECT @0x8000 step 0x40; STATUS0 @0x8020 step 0x40; 149 gates. */
#define CCM_LPCG_DIRECT_BASE   0x8000
#define CCM_LPCG_STATUS0_BASE  0x8020
#define CCM_LPCG_STEP          0x40
#define CCM_LPCG_COUNT         149
#define LPCG_ON                0x1u

/* OBSERVE array: FREQUENCY_CURRENT @0x4440 step 0x80; 2 slices. */
#define CCM_OBS_FREQ_BASE      0x4440
#define CCM_OBS_STEP           0x80
#define CCM_OBS_COUNT          2
/* CCM_OBS_DIV is 3 in the SDK; GetFreqFromObs returns FREQ_CURRENT*(3+1).
 * 6 MHz * 4 = 24 MHz nominal — non-zero and sane for a console baud divider. */
#define CCM_OBS_FREQ_NOMINAL   6000000u

static bool ccm_is_lpcg_status0(hwaddr off, unsigned *gate)
{
    if (off >= CCM_LPCG_STATUS0_BASE &&
        off <  CCM_LPCG_DIRECT_BASE + CCM_LPCG_COUNT * CCM_LPCG_STEP &&
        ((off - CCM_LPCG_STATUS0_BASE) % CCM_LPCG_STEP) == 0) {
        *gate = (off - CCM_LPCG_STATUS0_BASE) / CCM_LPCG_STEP;
        return true;
    }
    return false;
}

static bool ccm_is_obs_freq(hwaddr off)
{
    for (int n = 0; n < CCM_OBS_COUNT; n++) {
        if (off == CCM_OBS_FREQ_BASE + n * CCM_OBS_STEP) {
            return true;
        }
    }
    return false;
}

static uint64_t imxrt1180_ccm_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180CCMState *s = IMXRT1180_CCM(opaque);
    unsigned gate;

    if (offset + 4 > IMXRT1180_CCM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    if (ccm_is_lpcg_status0(offset, &gate)) {
        uint32_t direct = s->regs[(CCM_LPCG_DIRECT_BASE +
                                   gate * CCM_LPCG_STEP) / 4];
        return (direct & LPCG_ON) ? LPCG_ON : 0;   /* STATUS0.ON = DIRECT.ON */
    }
    if (ccm_is_obs_freq(offset)) {
        return CCM_OBS_FREQ_NOMINAL;               /* non-zero measured freq */
    }
    return s->regs[offset / 4];
}

static void imxrt1180_ccm_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    IMXRT1180CCMState *s = IMXRT1180_CCM(opaque);

    if (offset + 4 > IMXRT1180_CCM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imxrt1180_ccm_ops = {
    .read = imxrt1180_ccm_read,
    .write = imxrt1180_ccm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_ccm_reset(DeviceState *dev)
{
    IMXRT1180CCMState *s = IMXRT1180_CCM(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imxrt1180_ccm_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180CCMState *s = IMXRT1180_CCM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_ccm_ops, s,
                          TYPE_IMXRT1180_CCM, IMXRT1180_CCM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_imxrt1180_ccm = {
    .name = TYPE_IMXRT1180_CCM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180CCMState, IMXRT1180_CCM_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_ccm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_ccm_realize;
    device_class_set_legacy_reset(dc, imxrt1180_ccm_reset);
    dc->vmsd = &vmstate_imxrt1180_ccm;
}

static const TypeInfo imxrt1180_ccm_types[] = {
    {
        .name          = TYPE_IMXRT1180_CCM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180CCMState),
        .class_init    = imxrt1180_ccm_class_init,
    },
};

DEFINE_TYPES(imxrt1180_ccm_types)
