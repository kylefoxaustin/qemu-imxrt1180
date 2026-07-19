/*
 * NXP i.MX RT1180 EQDC — Enhanced Quadrature Decoder.
 *
 * The rotor-position sensor of the motor-control subsystem.  Register-accurate
 * against the MIMXRT1189 DFP PERI_EQDC.h, reproducing the two behaviours the
 * FOC encoder driver (fsl_eqdc) depends on:
 *
 *   - CTRL.LDOK is a self-clearing software load: firmware sets it and spins on
 *     `while (CTRL & LDOK)`.  The model loads the modulus/compare shadow and, if
 *     CTRL.SWIP is set, preloads the position counter from UINIT:LINIT, then
 *     clears LDOK.
 *   - Coherent 32-bit position read: reading UPOS atomically snapshots LPOS,
 *     REV, POSD (and the period/last-edge registers) into their hold registers,
 *     so EQDC_GetPosition() (read UPOS, then LPOSH) sees a consistent pair.
 *     Reading POSD likewise refreshes POSDH.
 *
 * FIDELITY NOTE — no quadrature encoder is wired to the inputs and no motor
 * plant drives them, so the position/revolution counters do NOT advance on their
 * own: they read back exactly what firmware (or a future virtual plant) writes,
 * and no index/compare/watchdog interrupt is generated.  This is the honest
 * "encoder present, shaft not turning" state; a virtual-motor plant that turns
 * PWM duty into position is flagged future work, not faked here.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/misc/imxrt1180_eqdc.h"
#include "migration/vmstate.h"

/* Register offsets (16-bit registers). */
#define R_CTRL       0x00
#define R_CTRL2      0x02
#define R_FILT       0x04
#define R_LASTEDGE   0x06
#define R_POSDPER    0x08
#define R_UPOS       0x0C
#define R_LPOS       0x0E
#define R_POSD       0x10
#define R_POSDH      0x12
#define R_UPOSH      0x14
#define R_LPOSH      0x16
#define R_LASTEDGEH  0x18
#define R_POSDPERH   0x1A
#define R_REVH       0x1C
#define R_REV        0x1E
#define R_UINIT      0x20
#define R_LINIT      0x22
#define R_UVERID     0x50
#define R_LVERID     0x52

/* CTRL bits. */
#define CTRL_LDOK    0x0001
#define CTRL_SWIP    0x0800

#define REG(s, off)  ((s)->regs[(off) / 2])

/* Reading UPOS snapshots the live counters into their hold registers. */
static void eqdc_snapshot(IMXRT1180EQDCState *s)
{
    REG(s, R_UPOSH)     = REG(s, R_UPOS);
    REG(s, R_LPOSH)     = REG(s, R_LPOS);
    REG(s, R_REVH)      = REG(s, R_REV);
    REG(s, R_POSDH)     = REG(s, R_POSD);
    REG(s, R_POSDPERH)  = REG(s, R_POSDPER);
    REG(s, R_LASTEDGEH) = REG(s, R_LASTEDGE);
}

void imxrt1180_eqdc_set_position(IMXRT1180EQDCState *s, uint32_t pos,
                                 uint16_t rev)
{
    REG(s, R_LPOS) = pos & 0xFFFF;
    REG(s, R_UPOS) = pos >> 16;
    REG(s, R_REV)  = rev;
    /*
     * The mc_pmsm qdc2 encoder driver reads POSITION from the HOLD registers
     * (UPOSH:LPOSH), not the live counters, and its fast loop does so without a
     * preceding snapshot read -- on silicon a hardware position-hold trigger
     * (synced to the PWM) refreshes them every control cycle.  Keep the hold
     * equal to the live position so that read always sees the current shaft
     * angle; without this it read the reset 0 and the FOC had no feedback.
     */
    REG(s, R_LPOSH) = REG(s, R_LPOS);
    REG(s, R_UPOSH) = REG(s, R_UPOS);
    REG(s, R_REVH)  = REG(s, R_REV);
}

/*
 * Hardware speed measurement.  The driver computes rotor speed from POSDH (the
 * position CHANGE) over POSDPERH (the number of QD-timer clocks that change took),
 * i.e. POSDH/POSDPERH = counts per QD clock; LASTEDGE (QD clocks since the last
 * encoder edge) drives the low-speed estimate.  A virtual plant knows the shaft
 * velocity, so it presents these directly.  Signed POSD carries the direction.
 */
void imxrt1180_eqdc_set_speed(IMXRT1180EQDCState *s, int16_t posd,
                              uint16_t posdper, uint16_t lastedge)
{
    REG(s, R_POSD)     = (uint16_t)posd;
    REG(s, R_POSDPER)  = posdper;
    REG(s, R_LASTEDGE) = lastedge;
}

/* QD-timer prescaler exponent from FILT[PRSC] (bits 14:12): clock = bus / 2^PRSC. */
unsigned imxrt1180_eqdc_filt_prsc(IMXRT1180EQDCState *s)
{
    return (REG(s, R_FILT) >> 12) & 0x7;
}

static uint64_t imxrt1180_eqdc_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180EQDCState *s = IMXRT1180_EQDC(opaque);

    if (offset + 2 > IMXRT1180_EQDC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }

    switch (offset) {
    case R_UPOS:
        /* Coherent read: latch the lower/rev/diff halves at this instant. */
        eqdc_snapshot(s);
        return REG(s, R_UPOS);
    case R_POSD:
        /* Reading POSD latches the difference, its period and the last-edge time
         * into the hold registers the speed driver then reads. */
        REG(s, R_POSDH)     = REG(s, R_POSD);
        REG(s, R_POSDPERH)  = REG(s, R_POSDPER);
        REG(s, R_LASTEDGEH) = REG(s, R_LASTEDGE);
        return REG(s, R_POSD);
    default:
        return REG(s, offset);
    }
}

static void imxrt1180_eqdc_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    IMXRT1180EQDCState *s = IMXRT1180_EQDC(opaque);
    uint16_t v = (uint16_t)value;

    if (offset + 2 > IMXRT1180_EQDC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }

    switch (offset) {
    case R_CTRL:
        /*
         * LDOK triggers a software load of the modulus/compare/init shadow and
         * self-clears when done.  With SWIP set, the position counter is
         * preloaded from UINIT:LINIT.  Store CTRL with LDOK cleared.
         */
        if (v & CTRL_LDOK) {
            if (v & CTRL_SWIP) {
                REG(s, R_UPOS) = REG(s, R_UINIT);
                REG(s, R_LPOS) = REG(s, R_LINIT);
            }
        }
        REG(s, R_CTRL) = v & ~CTRL_LDOK;
        if ((v & CTRL_SWIP) && !s->no_encoder_logged) {
            s->no_encoder_logged = true;
            qemu_log_mask(LOG_UNIMP, "%s: enabled, but no quadrature encoder / "
                "motor plant drives the inputs -- position stays as written "
                "(flagged)\n", __func__);
        }
        return;
    case R_UVERID:
    case R_LVERID:
        return;                             /* version registers are read-only */
    default:
        REG(s, offset) = v;
        return;
    }
}

static const MemoryRegionOps imxrt1180_eqdc_ops = {
    .read = imxrt1180_eqdc_read,
    .write = imxrt1180_eqdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
    .impl.min_access_size = 2,
    .impl.max_access_size = 2,
};

static void imxrt1180_eqdc_reset(DeviceState *dev)
{
    IMXRT1180EQDCState *s = IMXRT1180_EQDC(dev);

    memset(s->regs, 0, sizeof(s->regs));

    /*
     * ============ ZERO IS NOT "NO SPEED" HERE -- IT IS INFINITE SPEED ============
     *
     * POSDPER is the Position Difference PERIOD counter: the number of clocks between
     * successive encoder edges.  A speed observer DIVIDES BY IT.  On silicon it resets
     * to 0xFFFF -- the maximum period, i.e. "no edge has been seen, the shaft is not
     * turning".  A memset to zero says the exact opposite: ZERO CLOCKS BETWEEN EDGES.
     *
     *   A FOC SPEED LOOP READING OUR RESET VALUE COMPUTES A DIVIDE-BY-ZERO, OR AN
     *   INFINITE ROTOR VELOCITY, BEFORE THE MOTOR HAS MOVED AT ALL.
     *
     * Same for its buffer/hold registers and LASTEDGE (the timestamp of the last edge:
     * 0xFFFF = "none yet").  Offsets from PERI_EQDC.h, values from the RM's reset
     * column.  All 16-bit -- which is why the gate had never once looked at them: it
     * kept only 32-bit registers and dropped 33 EQDC rows without a counter.
     */
    s->regs[0x06 / 2] = 0xFFFF;      /* LASTEDGE    -- no edge seen yet   */
    s->regs[0x08 / 2] = 0xFFFF;      /* POSDPER     -- maximum period     */
    s->regs[0x0A / 2] = 0xFFFF;      /* POSDPERBFR                        */
    s->regs[0x18 / 2] = 0xFFFF;      /* LASTEDGEH                         */
    s->regs[0x1A / 2] = 0xFFFF;      /* POSDPERH                          */
    s->regs[0x28 / 2] = 0x8000;      /* UCOMP0                            */
    s->regs[0x50 / 2] = 0x0001;      /* UVERID                            */
    s->regs[0x52 / 2] = 0x0001;      /* LVERID                            */

    s->no_encoder_logged = false;
    qemu_set_irq(s->irq, 0);
}

static void imxrt1180_eqdc_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180EQDCState *s = IMXRT1180_EQDC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_eqdc_ops, s,
                          TYPE_IMXRT1180_EQDC, IMXRT1180_EQDC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_imxrt1180_eqdc = {
    .name = TYPE_IMXRT1180_EQDC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(no_encoder_logged, IMXRT1180EQDCState),
        VMSTATE_UINT16_ARRAY(regs, IMXRT1180EQDCState, IMXRT1180_EQDC_SIZE / 2),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_eqdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_eqdc_realize;
    device_class_set_legacy_reset(dc, imxrt1180_eqdc_reset);
    dc->vmsd = &vmstate_imxrt1180_eqdc;
}

static const TypeInfo imxrt1180_eqdc_types[] = {
    {
        .name          = TYPE_IMXRT1180_EQDC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180EQDCState),
        .class_init    = imxrt1180_eqdc_class_init,
    },
};

DEFINE_TYPES(imxrt1180_eqdc_types)
