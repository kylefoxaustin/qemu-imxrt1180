/*
 * NXP i.MX RT1180 LPI2C — Low-Power I2C, controller (master) mode.
 *
 * Register-accurate against the MIMXRT1189 CMSIS PERI_LPI2C.h.  Drives a real
 * QEMU I2CBus, so device models (sensors, EEPROMs) attach to it and respond.
 * Transfers run synchronously on the MTDR command write (QEMU I2C is
 * synchronous), so the TX FIFO always reads empty (TDF set) and received bytes
 * are available immediately (RDF set).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/i2c/imxrt1180_lpi2c.h"
#include "migration/vmstate.h"

/* Register offsets. */
#define LPI2C_VERID  0x00
#define LPI2C_PARAM  0x04
#define LPI2C_MCR    0x10
#define LPI2C_MSR    0x14
#define LPI2C_MIER   0x18
#define LPI2C_MDER   0x1C
#define LPI2C_MCFGR0 0x20
#define LPI2C_MCFGR3 0x2C
#define LPI2C_MDMR   0x40
#define LPI2C_MCCR0  0x48
#define LPI2C_MCCR1  0x50
#define LPI2C_MFCR   0x58
#define LPI2C_MFSR   0x5C
#define LPI2C_MTDR   0x60
#define LPI2C_MRDR   0x70
#define LPI2C_MRDROR 0x78

/* MCR bits. */
#define MCR_MEN 0x1
#define MCR_RST 0x2
#define MCR_RTF 0x100
#define MCR_RRF 0x200

/* MSR bits. */
#define MSR_TDF  0x1
#define MSR_RDF  0x2
#define MSR_EPF  0x100
#define MSR_SDF  0x200
#define MSR_NDF  0x400
#define MSR_ALF  0x800
#define MSR_FEF  0x1000
#define MSR_STF  0x8000
#define MSR_MBF  0x01000000
#define MSR_BBF  0x02000000
#define MSR_STICKY_MASK 0x0000FF00u   /* EPF/SDF/NDF/ALF/FEF/PLTF/DMF/STF (W1C) */
#define MSR_INT_MASK    0x0000FF03u   /* bits that can raise an interrupt */

/* MTDR fields. */
#define MTDR_DATA 0xFF
#define MTDR_CMD_SHIFT 8
#define MTDR_CMD_MASK  0x7

/*
 * ============ THE CAPABILITY IS COMPUTED FROM THE THING IT DESCRIBES ============
 *
 * PARAM's FIFO fields are EXPONENTS.  The SDK reads them as `1U << (PARAM & MASK)` and
 * pushes that many words BEFORE it checks the ready flag -- so an OVER-report is not a
 * cosmetic lie.  A guest told "16 words" over a 1-word register pushes sixteen and
 * SILENTLY DROPS FIFTEEN.  (mcxn947qemu found exactly that in their LPSPI and LPI2C.)
 *
 *   ⭐ A CAPABILITY REGISTER THAT IS A *CONSTANT* CAN DRIFT FROM THE THING IT
 *      DESCRIBES.  ONE *COMPUTED FROM* IT CANNOT.
 *
 * Ours were right -- BY LUCK.  A hand-written 0x0404 next to a `#define ..._FIFO 16`
 * agrees today and silently stops agreeing the moment somebody changes the depth.
 * (mcxn's SAI value was correct while its own comment said "FIFO=32" and the value
 * encoded 8: THE COMMENT HAD ALREADY DRIFTED FROM THE VALUE IT DESCRIBED.)
 *
 * So: derive the exponent FROM the depth, and make a disagreement a COMPILE ERROR.
 * Negative-tested -- change a depth to a non-power-of-two and the build fails.
 */
#define FIFO_EXP(d)                                                           \
    ((d) == 1 ? 0 : (d) == 2 ? 1 : (d) == 4 ? 2 : (d) == 8 ? 3 :              \
     (d) == 16 ? 4 : (d) == 32 ? 5 : (d) == 64 ? 6 : -1)


/*
 * ====== THE CAPABILITY DESCRIBES THE *SILICON*, NOT THIS MODEL. ======
 *
 * I got this backwards an hour ago.  mcxn947qemu's rule -- "a capability computed FROM
 * the thing it describes cannot drift from it" -- is right, and I tied PARAM to THE
 * MODEL'S FIFO DEPTH.  But PARAM does not describe the model.  IT DESCRIBES THE CHIP.
 *
 * 93emulator, 2026-07-14, on the identical register:
 *   "LPI2C PARAM returned 0x0404 -- 2^4 = 16-deep FIFOs.  The RM says 0x0303: EIGHT.
 *    And i2c-imx-lpi2c derives BOTH its watermark (MFCR = txfifosize >> 1) AND its read
 *    chunking (rxfifosize >> 1) from that field -- so I was sizing the driver to a FIFO
 *    TWICE THE SIZE OF THE SILICON'S.  It works here and MIS-SIZES THE DRIVER ON
 *    HARDWARE."
 *
 * The RT1180's SDK is compiled against FSL_FEATURE_LPI2C_FIFO_SIZEn(x) == 8.  We were
 * advertising 16.  Same bug, same register, different chip.
 *
 * So there are TWO numbers and they are not the same number:
 *
 *   SILICON depth  -- what PARAM must report.  The guest sizes itself against this, and
 *                     it must be right ON HARDWARE, not just here.
 *   MODEL depth    -- what we actually hold.  It may be LARGER (this model over-delivers
 *                     deliberately; the header has always said ">= real").
 *
 *   ⭐ THE INVARIANT IS  model >= advertised,  NOT  model == advertised.
 *      Over-delivering is safe: a guest sized against the silicon's 8 always fits in our
 *      16, HERE AND ON THE BOARD.  Advertising more than the silicon has is the unsafe
 *      direction, and it fails only on hardware -- where nobody is watching.
 *      (95emulator: "under-reporting is the only direction that is safe in BOTH worlds.")
 *
 * QEMU_BUILD_BUG_ON enforces it.  Negative-tested: advertise more than we hold -> build
 * fails.
 */

/* FSL_FEATURE_LPI2C_FIFO_SIZEn(x) == 8 on the MIMXRT1189.  The guest is COMPILED
 * against this, and i2c-imx-lpi2c derives its watermark and read chunking from it. */
#define LPI2C_SILICON_FIFO   8

/* PARAM: MRXFIFO in bits 15:8, MTXFIFO in bits 7:0, as exponents. */
#define LPI2C_PARAM_VALUE                                                     \
    (((uint32_t)FIFO_EXP(LPI2C_SILICON_FIFO) << 8) |                          \
      (uint32_t)FIFO_EXP(LPI2C_SILICON_FIFO))

#define MRDR_RXEMPTY 0x4000

/*
 * The TARGET (slave) engine is not modelled -- but ITS REGISTERS STILL ANSWER, and
 * ZERO IS AN ANSWER.  SASR/SRDR/SRDROR reset with RXEMPTY (bit 14) SET; returning 0
 * tells the guest THE TARGET RECEIVE FIFO HAS DATA when nothing has been received,
 * and a driver polling the FIFO reads a phantom byte out of an empty one.
 *
 *   AN UNMODELLED REGISTER IS NOT A FREE REGISTER.
 *
 * (mcxn947qemu and 91emulator both found exactly this, in the same block, the same
 * day: "MRDR itself was correct; its ALIAS was not.")
 */
#define LPI2C_SASR   0x150
#define LPI2C_SRDR   0x170
#define LPI2C_SRDROR 0x178
#define SLAVE_RXEMPTY 0x4000

static uint32_t lpi2c_msr(IMXRT1180LPI2CState *s)
{
    uint32_t v = s->msr_sticky;
    /*
     * TDF is "the transmit FIFO count is at or below the watermark" -- which is TRUE
     * OF AN EMPTY FIFO, enabled or not.  The RM resets MSR to 0x1 for that reason.
     * Gating it on MEN made MSR read 0 at reset: "the TX FIFO is full", from a
     * module that has never sent anything.
     */
    v |= MSR_TDF;                         /* tx FIFO always has room */
    if (s->rx_count) {
        v |= MSR_RDF;
    }
    if (s->active) {
        v |= MSR_MBF | MSR_BBF;
    }
    return v;
}

static void lpi2c_update_irq(IMXRT1180LPI2CState *s)
{
    qemu_set_irq(s->irq, (lpi2c_msr(s) & s->mier & MSR_INT_MASK) != 0);
}

static void lpi2c_rx_push(IMXRT1180LPI2CState *s, uint8_t byte)
{
    if (s->rx_count < IMXRT1180_LPI2C_FIFO) {
        s->rx_fifo[(s->rx_head + s->rx_count) % IMXRT1180_LPI2C_FIFO] = byte;
        s->rx_count++;
    } else {
        s->msr_sticky |= MSR_FEF;          /* rx FIFO overflow */
    }
}

/* Execute one MTDR command. */
static void lpi2c_command(IMXRT1180LPI2CState *s, uint32_t val)
{
    uint32_t cmd = (val >> MTDR_CMD_SHIFT) & MTDR_CMD_MASK;
    uint8_t data = val & MTDR_DATA;

    switch (cmd) {
    case 0x4:   /* (repeated) START + transmit address */
    case 0x5:   /* START + transmit address, expect NACK */
        if (i2c_start_transfer(s->bus, data >> 1, data & 1)) {
            s->msr_sticky |= MSR_NDF;      /* no device ACKed */
            i2c_end_transfer(s->bus);
            s->active = false;
        } else {
            s->active = true;
        }
        break;
    case 0x0:   /* transmit DATA */
        if (s->active && i2c_send(s->bus, data)) {
            s->msr_sticky |= MSR_NDF;
        }
        break;
    case 0x1:   /* receive DATA+1 bytes */
    case 0x3:   /* receive and discard DATA+1 bytes */
        if (s->active) {
            for (unsigned i = 0; i <= data; i++) {
                uint8_t b = i2c_recv(s->bus);
                if (cmd == 0x1) {
                    lpi2c_rx_push(s, b);
                }
            }
        }
        break;
    case 0x2:   /* generate STOP */
        if (s->active) {
            i2c_end_transfer(s->bus);
            s->active = false;
        }
        s->msr_sticky |= MSR_SDF | MSR_EPF;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: MTDR CMD %u not modelled\n",
                      __func__, cmd);
        break;
    }
    lpi2c_update_irq(s);
}

static uint64_t imxrt1180_lpi2c_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180LPI2CState *s = opaque;

    switch (offset) {
    case LPI2C_VERID:
        return 0x02000004;                 /* v2.0 */
    case LPI2C_PARAM:
        return LPI2C_PARAM_VALUE;
    case LPI2C_MCR:
        return s->mcr;
    case LPI2C_MSR:
        return lpi2c_msr(s);
    case LPI2C_SASR:
    case LPI2C_SRDR:
    case LPI2C_SRDROR:
        return SLAVE_RXEMPTY;             /* target engine: honestly EMPTY */
    case LPI2C_MIER:
        return s->mier;
    case LPI2C_MDER:
        return s->mder;
    case LPI2C_MCFGR0 ... LPI2C_MCFGR3:
        return s->mcfgr[(offset - LPI2C_MCFGR0) / 4];
    case LPI2C_MDMR:
        return s->mdmr;
    case LPI2C_MCCR0:
        return s->mccr[0];
    case LPI2C_MCCR1:
        return s->mccr[1];
    case LPI2C_MFCR:
        return s->mfcr;
    case LPI2C_MFSR:
        return (uint32_t)s->rx_count << 16;   /* RXCOUNT; TXCOUNT = 0 */
    case LPI2C_MRDR:
    case LPI2C_MRDROR: {
        if (s->rx_count == 0) {
            return MRDR_RXEMPTY;
        }
        uint8_t d = s->rx_fifo[s->rx_head];
        if (offset == LPI2C_MRDR) {           /* MRDROR is a non-destructive peek */
            s->rx_head = (s->rx_head + 1) % IMXRT1180_LPI2C_FIFO;
            s->rx_count--;
            lpi2c_update_irq(s);
        }
        return d;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @0x%02x\n",
                      __func__, (unsigned)offset);
        return 0;
    }
}

static void imxrt1180_lpi2c_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    IMXRT1180LPI2CState *s = opaque;
    uint32_t v = value;

    switch (offset) {
    case LPI2C_MCR:
        s->mcr = v & (MCR_MEN | MCR_RST | 0x4 | 0x8);
        if (v & MCR_RST) {
            s->msr_sticky = 0;
            s->rx_head = s->rx_count = 0;
            s->active = false;
        }
        if (v & MCR_RRF) {
            s->rx_head = s->rx_count = 0;
        }
        /* RTF (tx FIFO reset) is a no-op: our TX FIFO is always drained. */
        lpi2c_update_irq(s);
        break;
    case LPI2C_MSR:
        s->msr_sticky &= ~(v & MSR_STICKY_MASK);   /* W1C */
        lpi2c_update_irq(s);
        break;
    case LPI2C_MIER:
        s->mier = v;
        lpi2c_update_irq(s);
        break;
    case LPI2C_MDER:
        s->mder = v;
        break;
    case LPI2C_MCFGR0 ... LPI2C_MCFGR3:
        s->mcfgr[(offset - LPI2C_MCFGR0) / 4] = v;
        break;
    case LPI2C_MDMR:
        s->mdmr = v;
        break;
    case LPI2C_MCCR0:
        s->mccr[0] = v;
        break;
    case LPI2C_MCCR1:
        s->mccr[1] = v;
        break;
    case LPI2C_MFCR:
        s->mfcr = v;
        break;
    case LPI2C_MTDR:
        if (s->mcr & MCR_MEN) {
            lpi2c_command(s, v);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @0x%02x=0x%08x\n",
                      __func__, (unsigned)offset, v);
        break;
    }
}

static const MemoryRegionOps imxrt1180_lpi2c_ops = {
    .read = imxrt1180_lpi2c_read,
    .write = imxrt1180_lpi2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_lpi2c_reset(DeviceState *dev)
{
    IMXRT1180LPI2CState *s = IMXRT1180_LPI2C(dev);

    s->mcr = s->msr_sticky = s->mier = s->mder = 0;
    memset(s->mcfgr, 0, sizeof(s->mcfgr));
    s->mdmr = s->mccr[0] = s->mccr[1] = s->mfcr = 0;
    s->rx_head = s->rx_count = 0;
    s->active = false;
    qemu_set_irq(s->irq, 0);
}

static void imxrt1180_lpi2c_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180LPI2CState *s = IMXRT1180_LPI2C(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_lpi2c_ops, s,
                          TYPE_IMXRT1180_LPI2C, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    /*
     * NAME THE BUS AFTER THE INSTANCE.  Every LPI2C used to create a bus called
     * "i2c", so `-device tmp105` (with no bus=) attached to whichever one QEMU
     * happened to pick -- THE LAST ONE.  The lpi2c test therefore worked only
     * because it targeted LPI2C4 and LPI2C4 happened to be the last of four.
     * Adding LPI2C5/6 -- a pure fidelity fix -- silently re-homed the test's
     * device onto a controller the firmware never touches, and the test failed
     * with "the device did not ACK".
     *
     *   A TEST THAT DEPENDS ON HOW MANY INSTANCES EXIST IS NOT TESTING THE ONE
     *   IT NAMES.  Now the bus is "lpi2c4-bus" and the test must say so.
     */
    {
        g_autofree char *bname = g_strdup_printf(
            "%s-bus", object_get_canonical_path_component(OBJECT(dev)));
            QEMU_BUILD_BUG_ON(FIFO_EXP(LPI2C_SILICON_FIFO) < 0);
        /* deliver at least what we advertise -- over-delivering is safe. */
        QEMU_BUILD_BUG_ON(IMXRT1180_LPI2C_FIFO < LPI2C_SILICON_FIFO);
    s->bus = i2c_init_bus(dev, bname);
    }
}

static const VMStateDescription vmstate_imxrt1180_lpi2c = {
    .name = TYPE_IMXRT1180_LPI2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mcr, IMXRT1180LPI2CState),
        VMSTATE_UINT32(msr_sticky, IMXRT1180LPI2CState),
        VMSTATE_UINT32(mier, IMXRT1180LPI2CState),
        VMSTATE_UINT32(mder, IMXRT1180LPI2CState),
        VMSTATE_UINT32_ARRAY(mcfgr, IMXRT1180LPI2CState, 4),
        VMSTATE_UINT32(mdmr, IMXRT1180LPI2CState),
        VMSTATE_UINT32_ARRAY(mccr, IMXRT1180LPI2CState, 2),
        VMSTATE_UINT32(mfcr, IMXRT1180LPI2CState),
        VMSTATE_UINT8_ARRAY(rx_fifo, IMXRT1180LPI2CState, IMXRT1180_LPI2C_FIFO),
        VMSTATE_UINT8(rx_head, IMXRT1180LPI2CState),
        VMSTATE_UINT8(rx_count, IMXRT1180LPI2CState),
        VMSTATE_BOOL(active, IMXRT1180LPI2CState),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_lpi2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_lpi2c_realize;
    device_class_set_legacy_reset(dc, imxrt1180_lpi2c_reset);
    dc->vmsd = &vmstate_imxrt1180_lpi2c;
}

static const TypeInfo imxrt1180_lpi2c_types[] = {
    {
        .name          = TYPE_IMXRT1180_LPI2C,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180LPI2CState),
        .class_init    = imxrt1180_lpi2c_class_init,
    },
};

DEFINE_TYPES(imxrt1180_lpi2c_types)
