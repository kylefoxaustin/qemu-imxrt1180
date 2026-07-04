/*
 * NXP i.MX RT1180 ANADIG (analog clock: OSC + PLL + PMU) — clock-ready model.
 *
 * The SDK CLOCK_Init enables the 24 MHz OSC and each PLL, then polls their
 * "stable/locked" status bits.  In emulation clock lock is instantaneous, so
 * this model is register-backed (reads return what was written) and forces the
 * OSC-stable / PLL-stable status bits SET on read for the known status
 * registers — the standard QEMU clock-controller approach (cf. the i.MX / MCX
 * SCG "clocks ready" stubs).  It does NOT compute real clock frequencies.
 *
 * Register offsets and STABLE-bit masks VERIFIED against the MIMXRT1189 CMSIS
 * (PERI_ANADIG_OSC.h / PERI_ANADIG_PLL.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/imxrt1180_anadig.h"
#include "migration/vmstate.h"

/*
 * Status bits are derived from the enable/gate state so that firmware sees
 * INSTANT lock: a block reads "stable" once enabled and "not stable" while
 * gated/powered-down.  This matters because the SDK PFD reconfigure sequence
 * gates a PFD and *waits for its stable bit to CLEAR* before rewriting it —
 * permanently forcing the bit set would hang that loop.
 */
#define OSC_24M_STABLE   0x40000000u   /* OSC_24M_CTRL @0x4320, bit 30       */
#define PLL_STABLE       0x20000000u   /* *_PLL_CTRL   bit 29                */

/* Per-PFD (n=0..3, 8 bits each): STABLE = 0x40<<(n*8), CLKGATE = 0x80<<(n*8). */
static uint32_t anadig_pfd_status(uint32_t v)
{
    for (int n = 0; n < 4; n++) {
        uint32_t gate   = 0x80u << (n * 8);
        uint32_t stable = 0x40u << (n * 8);
        if (v & gate) {
            v &= ~stable;          /* gated -> not stable */
        } else {
            v |= stable;           /* enabled -> stable (instant lock) */
        }
    }
    return v;
}

static uint32_t imxrt1180_anadig_status(hwaddr offset, uint32_t v)
{
    switch (offset) {
    case 0x4320:                   /* OSC_24M_CTRL: 24M OSC always stable */
        return v | OSC_24M_STABLE;
    case 0x4000:                   /* ARM_PLL_CTRL   */
    case 0x4010:                   /* SYS_PLL3_CTRL  */
    case 0x4040:                   /* SYS_PLL2_CTRL  */
    case 0x4100:                   /* SYS_PLL1_CTRL  */
    case 0x4200:                   /* PLL_AUDIO_CTRL */
        /* Report locked unconditionally: firmware waits for STABLE=1 (often
         * before it (re)asserts POWERUP), assuming the boot ROM already brought
         * the PLL up.  Instant lock. */
        return v | PLL_STABLE;
    default:                       /* PFD regs handled in read (relock state) */
        return v;
    }
}

/* Which PFD-register relock bit an offset maps to (-1 if not a PFD reg). */
static int anadig_pfd_index(hwaddr offset)
{
    switch (offset) {
    case 0x4030: return 0;         /* SYS_PLL3_PFD */
    case 0x4070: return 1;         /* SYS_PLL2_PFD */
    default:     return -1;
    }
}

#define PFD_STABLE_ALL 0x40404040u /* PFD0..3 stable bits in a PFD register */

static uint64_t imxrt1180_anadig_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180AnadigState *s = IMXRT1180_ANADIG(opaque);
    int pfd;

    if (offset + 4 > IMXRT1180_ANADIG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    pfd = anadig_pfd_index(offset);
    if (pfd >= 0) {
        uint32_t v = s->regs[offset / 4];
        if (s->pfd_relock & (1u << pfd)) {
            /* One relock-transient read: report the PFDs not-yet-stable so the
             * SDK's "wait for the stable bit to change" reconfigure loop exits. */
            s->pfd_relock &= ~(1u << pfd);
            return v & ~PFD_STABLE_ALL;
        }
        return anadig_pfd_status(v);
    }
    return imxrt1180_anadig_status(offset, s->regs[offset / 4]);
}

static void imxrt1180_anadig_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IMXRT1180AnadigState *s = IMXRT1180_ANADIG(opaque);
    int pfd;

    if (offset + 4 > IMXRT1180_ANADIG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }
    s->regs[offset / 4] = value;

    /* A write to a PFD register kicks a relock -> next read reports transient. */
    pfd = anadig_pfd_index(offset);
    if (pfd >= 0) {
        s->pfd_relock |= (1u << pfd);
    }
}

static const MemoryRegionOps imxrt1180_anadig_ops = {
    .read = imxrt1180_anadig_read,
    .write = imxrt1180_anadig_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_anadig_reset(DeviceState *dev)
{
    IMXRT1180AnadigState *s = IMXRT1180_ANADIG(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->pfd_relock = 0;
}

static void imxrt1180_anadig_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180AnadigState *s = IMXRT1180_ANADIG(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_anadig_ops, s,
                          TYPE_IMXRT1180_ANADIG, IMXRT1180_ANADIG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_imxrt1180_anadig = {
    .name = TYPE_IMXRT1180_ANADIG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMXRT1180AnadigState,
                             IMXRT1180_ANADIG_SIZE / 4),
        VMSTATE_UINT8(pfd_relock, IMXRT1180AnadigState),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_anadig_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_anadig_realize;
    device_class_set_legacy_reset(dc, imxrt1180_anadig_reset);
    dc->vmsd = &vmstate_imxrt1180_anadig;
}

static const TypeInfo imxrt1180_anadig_types[] = {
    {
        .name          = TYPE_IMXRT1180_ANADIG,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180AnadigState),
        .class_init    = imxrt1180_anadig_class_init,
    },
};

DEFINE_TYPES(imxrt1180_anadig_types)
