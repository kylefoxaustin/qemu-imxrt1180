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

/* AUDIO_PLL fractional-PLL block (PLL_Type: CTRL0/SPREAD/NUMERATOR/DENOMINATOR,
 * each a 16-byte RW/SET/CLR/TOG group).  AUDIO_PLL_BASE 0x4448_4280 = ANADIG+0x4280. */
#define ANADIG_AUDIO_PLL_BASE 0x4280
#define ANADIG_AUDIO_PLL_END  0x42C0
#define PLL_STABLE       0x20000000u   /* *_PLL_CTRL   bit 29                */
#define PMU_BIAS_CTRL2_WB_EN 0x01000000u /* PMU_BIAS_CTRL2 @0x4610 bit 24    */
#define PMU_BIAS_CTRL2_WB_OK 0x04000000u /* PMU_BIAS_CTRL2 @0x4610 bit 26    */

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
    /*
     * TMPSNS temperature sensor (aliased into the ANADIG_TEMPSENSOR window).
     * The fsl_tempsensor driver derives the 25C calibration reference s_Ts25c
     * from TEMPSNS_OTP_TRIM_VALUE.TEMP_VAL (bits 21:10) and asserts the alarm
     * code it computes is >= 0 -- a zero trim makes that go negative.  Report a
     * plausible factory trim (code 1900), and make STATUS0 read conversion-done
     * (FINISH) with the same 1900 code so GetCurrentTemperature() resolves to
     * ~25 C (measured code == calibration code => exactly 25 C by construction).
     */
    case 0x4530:                   /* TEMPSNS_OTP_TRIM_VALUE: TEMP_VAL=1900 */
        return 1900u << 10;
    case 0x45D0:                   /* TMPSNS STATUS0 (+0x4580+0x50): FINISH|1900 */
        return 0x10000u | 1900u;
    case 0x4000:                   /* ARM_PLL_CTRL   */
    case 0x4010:                   /* SYS_PLL3_CTRL  */
    case 0x4040:                   /* SYS_PLL2_CTRL  */
    case 0x4100:                   /* SYS_PLL1_CTRL  */
    case 0x4200:                   /* PLL_AUDIO_CTRL */
        /* Report locked unconditionally: firmware waits for STABLE=1 (often
         * before it (re)asserts POWERUP), assuming the boot ROM already brought
         * the PLL up.  Instant lock. */
        return v | PLL_STABLE;
    case 0x4610:                   /* PMU_BIAS_CTRL2: body-bias network */
        /* PMU_EnableFBB() enables the well-bias network (WB_EN, bit 24) then
         * spins on WB_OK (bit 26).  The virtual bias network settles instantly,
         * so report WB_OK whenever WB_EN is set (as the PLLs report lock). */
        if (v & PMU_BIAS_CTRL2_WB_EN) {
            v |= PMU_BIAS_CTRL2_WB_OK;
        }
        return v;
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

    /*
     * The AUDIO_PLL fractional-PLL block (0x4280..0x42BF) is a PLL_Type: each 32-bit
     * register is a 16-byte RW/SET/CLR/TOG group.  Any alias reads back the RW value.
     */
    if (offset >= ANADIG_AUDIO_PLL_BASE && offset < ANADIG_AUDIO_PLL_END) {
        return s->regs[(offset & ~0xFull) / 4];
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
    /*
     * AUDIO_PLL block: apply RW/SET/CLR/TOG to the group's RW register.  The audio
     * PLL config driver (ANATOP_PllConfigure) sets DIV_SELECT/POST_DIV_SEL through
     * the CTRL0.CLR then CTRL0.SET aliases, so a generic store would drop them.
     */
    if (offset >= ANADIG_AUDIO_PLL_BASE && offset < ANADIG_AUDIO_PLL_END) {
        uint32_t *rw = &s->regs[(offset & ~0xFull) / 4];
        switch (offset & 0xF) {
        case 0x0: *rw = value;                    break;   /* RW  */
        case 0x4: *rw |= value;                   break;   /* SET */
        case 0x8: *rw &= ~(uint32_t)value;        break;   /* CLR */
        case 0xC: *rw ^= value;                   break;   /* TOG */
        }
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

/*
 * POWER-ON RESET VALUES, taken from the RM's reset column (IMXRT1180RM rev 10) and
 * verified by tests/imxrt1180-reset-values, which reads every register back and
 * diffs it against the manual.
 *
 * ⚠ A ZERO RESET VALUE IS NOT THE ABSENCE OF A CLAIM. IT IS A CLAIM, AND THE GUEST
 *   BELIEVES IT.  This block used to be a bare memset(regs, 0) -- which read as a
 *   safe, neutral default and was in fact eighteen false statements about the
 *   silicon.  (mcxn947qemu, 2026-07-12, who found the extreme case: ONE bit that is
 *   set out of reset, that firmware never sets because on silicon it is ALREADY set,
 *   and whose absence hard-faulted every driver in a peripheral family.)
 *
 * THE ONES THAT MATTER MOST ARE NOT THE STATUS BITS -- THEY ARE THE DIVIDERS:
 *
 *   SYS_PLL2_MFI = 0x16 (22).  24 MHz x 22 = 528 MHz, which is what SYS_PLL2 is.
 *   SYS_PLL2_MFD = 0x0FFF_FFFF.  ARM_PLL_CTRL[DIV_SELECT] = 0xA6 (166).
 *
 * fsl_clock.c's CLOCK_GetPllFreq() COMPUTES the PLL frequency from exactly these
 * fields.  With them reading zero, firmware that asks the hardware what frequency it
 * is running at gets an answer derived from a divider of zero -- and believes it.
 * That is the trust anchor the README flags as unverified for the PWM carrier.
 */
static const struct { hwaddr off; uint32_t val; } anadig_por[] = {
    /* ANADIG_PLL @ +0x4000 */
    { 0x4000, 0x400000A6 },   /* ARM_PLL_CTRL   -- DIV_SELECT = 166 */
    { 0x4010, 0x40000003 },   /* SYS_PLL3_CTRL  */
    { 0x4030, 0x8CA0918D },   /* SYS_PLL3_PFD   */
    { 0x4040, 0x40000000 },   /* SYS_PLL2_CTRL  */
    { 0x4070, 0xA098909B },   /* SYS_PLL2_PFD   */
    { 0x4090, 0x00000016 },   /* SYS_PLL2_MFI   -- 22 => 24 MHz x 22 = 528 MHz */
    { 0x40A0, 0x0FFFFFFF },   /* SYS_PLL2_MFD   */
    { 0x4100, 0x00004000 },   /* SYS_PLL1_CTRL  */
    { 0x4200, 0x00004000 },   /* PLL_AUDIO_CTRL */
    /* ANADIG_OSC @ +0x4300 */
    { 0x4310, 0x007901F2 },   /* OSC_RC24M_CTRL */
    { 0x4320, 0x00000080 },   /* OSC_24M_CTRL   */
    { 0x4340, 0x80000000 },   /* OSC_400M_CTRL0 */
    { 0x4350, 0x00000001 },   /* OSC_400M_CTRL1 */
    /* ANADIG_PMU / LDO @ +0x4600 */
    { 0x4600, 0x00008000 },   /* PMU_BIAS_CTRL  */
    { 0x4640, 0x00000005 },   /* PMU_LDO_PLL    */
    { 0x4710, 0x00000040 },   /* PMU_REF_CTRL   */
    { 0x4740, 0x00000108 },   /* PMU_LDO_AON_ANA */
    { 0x4760, 0x01301C05 },   /* PMU_LDO_AON_DIG */
};

static void imxrt1180_anadig_reset(DeviceState *dev)
{
    IMXRT1180AnadigState *s = IMXRT1180_ANADIG(dev);

    memset(s->regs, 0, sizeof(s->regs));
    for (size_t i = 0; i < ARRAY_SIZE(anadig_por); i++) {
        s->regs[anadig_por[i].off / 4] = anadig_por[i].val;
    }
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
