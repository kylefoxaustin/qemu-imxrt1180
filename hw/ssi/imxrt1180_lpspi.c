/*
 * NXP i.MX RT1180 LPSPI — Low-Power SPI, controller (master) mode.
 *
 * Register-accurate against the MIMXRT1189 CMSIS PERI_LPSPI.h.  Drives a real
 * QEMU SSIBus so SPI device models (flash, sensors) respond.  Transfers run
 * synchronously on the TDR write (QEMU SSI is synchronous): TDF stays set and
 * the received word is available in the rx FIFO (RDF) immediately.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/ssi/imxrt1180_lpspi.h"
#include "migration/vmstate.h"

/* Register offsets. */
#define LPSPI_VERID 0x00
#define LPSPI_PARAM 0x04
#define LPSPI_CR    0x10
#define LPSPI_SR    0x14
#define LPSPI_IER   0x18
#define LPSPI_DER   0x1C
#define LPSPI_CFGR0 0x20
#define LPSPI_CFGR1 0x24
#define LPSPI_DMR0  0x30
#define LPSPI_DMR1  0x34
#define LPSPI_CCR   0x40
#define LPSPI_CCR1  0x44
#define LPSPI_FCR   0x58
#define LPSPI_FSR   0x5C
#define LPSPI_TCR   0x60
#define LPSPI_TDR   0x64
#define LPSPI_RSR   0x70
#define LPSPI_RDR   0x74
#define LPSPI_RDROR 0x78

/* CR bits. */
#define CR_MEN 0x1
#define CR_RST 0x2
#define CR_RTF 0x100
#define CR_RRF 0x200

/* DER (DMA Enable, PERI_LPSPI.h): TDDE bit 0, RDDE bit 1. */
#define DER_TDDE 0x1
#define DER_RDDE 0x2

/* FCR (FIFO Control, PERI_LPSPI.h): TXWATER 3:0, RXWATER 19:16. */
#define FCR_TXWATER(v) ((v) & 0xFu)
#define FCR_RXWATER(v) (((v) >> 16) & 0xFu)

/* SR bits. */

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

/* FSL_FEATURE_LPSPI_FIFO_SIZEn(x) == 16 on the MIMXRT1189 -- the SILICON's depth, which
 * is what a guest sizes itself against.  See the note in imxrt1180_lpi2c.c: the model's
 * own depth may be LARGER, and the invariant is model >= advertised. */
#define LPSPI_SILICON_FIFO  16

/* PARAM: PCSNUM in bits 23:16, RXFIFO in 15:8, TXFIFO in 7:0 (FIFOs as exponents). */
#define LPSPI_PCSNUM        4u
#define LPSPI_PARAM_VALUE                                                     \
    ((LPSPI_PCSNUM << 16) |                                                   \
     ((uint32_t)FIFO_EXP(LPSPI_SILICON_FIFO) << 8) |                          \
      (uint32_t)FIFO_EXP(LPSPI_SILICON_FIFO))

#define SR_TDF 0x1
#define SR_RDF 0x2
#define SR_WCF 0x100
#define SR_FCF 0x200
#define SR_TCF 0x400
#define SR_MBF 0x01000000
#define SR_STICKY_MASK 0x00003F00u   /* WCF/FCF/TCF/TEF/REF/DMF (W1C) */
#define SR_INT_MASK    0x00003F03u

/* TCR fields. */
#define TCR_FRAMESZ_MASK 0xFFF
#define TCR_TXMSK 0x40000
#define TCR_RXMSK 0x80000
#define TCR_CONT  0x200000
#define TCR_PCS_SHIFT 24
#define TCR_PCS_MASK  0x3

#define RSR_RXEMPTY 0x2

static uint32_t lpspi_sr(IMXRT1180LPSPIState *s)
{
    uint32_t v = s->sr_sticky;
    /*
     * TDF means "the transmit FIFO count is at or below the watermark" -- TRUE OF AN
     * EMPTY FIFO, enabled or not.  The RM resets SR to 0x1 for that reason.  Gating
     * it on MEN made SR read 0 at reset: "the TX FIFO is FULL", from a module that
     * has never sent a byte.
     */
    v |= SR_TDF;                     /* tx FIFO always has room */
    if (s->rx_count) {
        v |= SR_RDF;
    }
    return v;
}

static void lpspi_update_irq(IMXRT1180LPSPIState *s)
{
    qemu_set_irq(s->irq, (lpspi_sr(s) & s->ier & SR_INT_MASK) != 0);
}

/*
 * Drive the eDMA hardware-request lines.  Same handshake the LPUART/SAI use: the
 * line is a level, re-evaluated wherever the FIFO level or a DMA-enable bit
 * changes, and the eDMA services one minor loop per assertion.
 *
 * TX: no TX FIFO is modelled (transfers run synchronously on the TDR write, so the
 * TX level is always 0 -- at or below any watermark).  The TX request therefore
 * reduces to the enable bit while the module is on, exactly as it does for the
 * synchronous-TX LPUART.  (Were a TX FIFO modelled, the condition would be
 * tx_count <= FCR_TXWATER; the SDK sets TXWATER = fifoSize-1, which the empty FIFO
 * always satisfies -- same observable.)
 *
 * RX: the RX FIFO is real, so this is a genuine watermark gate.  The SDK's
 * lpspi_edma leaves RXWATER = 0, i.e. "a request per received word"; a driver that
 * sets a non-zero RX watermark is honoured too.
 */
static void lpspi_update_dma(IMXRT1180LPSPIState *s)
{
    bool men = s->cr & CR_MEN;
    bool tx = men && (s->der & DER_TDDE);
    bool rx = men && (s->der & DER_RDDE) &&
              (s->rx_count > FCR_RXWATER(s->fcr));

    qemu_set_irq(s->dma_tx_req, tx);
    qemu_set_irq(s->dma_rx_req, rx);
}

static void lpspi_set_cs(IMXRT1180LPSPIState *s, int pcs, bool select)
{
    /* CS lines are active-low by default; assert = drive 0. */
    if (pcs >= 0 && pcs < IMXRT1180_LPSPI_NUMCS) {
        qemu_set_irq(s->cs_lines[pcs], select ? 0 : 1);
    }
}

/* Shift one TDR frame out to the SSI slave and capture the received word. */
static void lpspi_transfer(IMXRT1180LPSPIState *s, uint32_t tx)
{
    unsigned bits = (s->tcr & TCR_FRAMESZ_MASK) + 1;
    unsigned nbytes = bits < 8 ? 1 : (bits + 7) / 8;
    int pcs = (s->tcr >> TCR_PCS_SHIFT) & TCR_PCS_MASK;
    uint32_t rx = 0;

    if (s->cs_active != pcs) {
        if (s->cs_active >= 0) {
            lpspi_set_cs(s, s->cs_active, false);
        }
        lpspi_set_cs(s, pcs, true);
        s->cs_active = pcs;
    }

    for (int b = nbytes - 1; b >= 0; b--) {         /* MSB byte first */
        uint8_t out = (s->tcr & TCR_TXMSK) ? 0xFF : (tx >> (b * 8)) & 0xFF;
        uint8_t in = ssi_transfer(s->bus, out);
        rx = (rx << 8) | in;
    }

    if (!(s->tcr & TCR_RXMSK)) {                     /* capture unless masked */
        if (s->rx_count < IMXRT1180_LPSPI_FIFO) {
            s->rx_fifo[(s->rx_head + s->rx_count) % IMXRT1180_LPSPI_FIFO] = rx;
            s->rx_count++;
        }
    }

    s->sr_sticky |= SR_WCF | SR_FCF;
    if (!(s->tcr & TCR_CONT)) {                      /* frame ends -> drop CS */
        lpspi_set_cs(s, pcs, false);
        s->cs_active = -1;
        s->sr_sticky |= SR_TCF;
    }
    lpspi_update_irq(s);
    lpspi_update_dma(s);          /* a received word may raise the RX request */
}

static uint64_t imxrt1180_lpspi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180LPSPIState *s = opaque;

    switch (offset) {
    case LPSPI_VERID:
        return 0x02000004;
    case LPSPI_PARAM:
        return LPSPI_PARAM_VALUE;
    case LPSPI_CR:
        return s->cr;
    case LPSPI_SR:
        return lpspi_sr(s);
    case LPSPI_IER:
        return s->ier;
    case LPSPI_DER:
        return s->der;
    case LPSPI_CFGR0:
    case LPSPI_CFGR1:
        return s->cfgr[(offset - LPSPI_CFGR0) / 4];
    case LPSPI_DMR0:
    case LPSPI_DMR1:
        return s->dmr[(offset - LPSPI_DMR0) / 4];
    case LPSPI_CCR:
        return s->ccr[0];
    case LPSPI_CCR1:
        return s->ccr[1];
    case LPSPI_FCR:
        return s->fcr;
    case LPSPI_FSR:
        return (uint32_t)s->rx_count << 16;   /* RXCOUNT; TXCOUNT = 0 */
    case LPSPI_TCR:
        return s->tcr;
    case LPSPI_RSR:
        return s->rx_count ? 0 : RSR_RXEMPTY;
    case LPSPI_RDR:
    case LPSPI_RDROR: {
        if (s->rx_count == 0) {
            return 0;
        }
        uint32_t d = s->rx_fifo[s->rx_head];
        if (offset == LPSPI_RDR) {
            s->rx_head = (s->rx_head + 1) % IMXRT1180_LPSPI_FIFO;
            s->rx_count--;
            lpspi_update_irq(s);
            lpspi_update_dma(s);   /* draining the FIFO may lower the RX request */
        }
        return d;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @0x%02x\n",
                      __func__, (unsigned)offset);
        return 0;
    }
}

static void imxrt1180_lpspi_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    IMXRT1180LPSPIState *s = opaque;
    uint32_t v = value;

    switch (offset) {
    case LPSPI_CR:
        s->cr = v & (CR_MEN | CR_RST | 0x8 | 0x4);
        if (v & CR_RST) {
            s->sr_sticky = 0;
            s->rx_head = s->rx_count = 0;
        }
        if (v & CR_RRF) {
            s->rx_head = s->rx_count = 0;
        }
        lpspi_update_irq(s);
        lpspi_update_dma(s);       /* MEN and RX-FIFO-reset both move the lines */
        break;
    case LPSPI_SR:
        s->sr_sticky &= ~(v & SR_STICKY_MASK);
        lpspi_update_irq(s);
        break;
    case LPSPI_IER:
        s->ier = v;
        lpspi_update_irq(s);
        break;
    case LPSPI_DER:
        s->der = v;
        lpspi_update_dma(s);       /* TDDE/RDDE just changed */
        break;
    case LPSPI_CFGR0:
    case LPSPI_CFGR1:
        s->cfgr[(offset - LPSPI_CFGR0) / 4] = v;
        break;
    case LPSPI_DMR0:
    case LPSPI_DMR1:
        s->dmr[(offset - LPSPI_DMR0) / 4] = v;
        break;
    case LPSPI_CCR:
        s->ccr[0] = v;
        break;
    case LPSPI_CCR1:
        s->ccr[1] = v;
        break;
    case LPSPI_FCR:
        s->fcr = v;
        lpspi_update_dma(s);       /* RXWATER may have moved */
        break;
    case LPSPI_TCR:
        s->tcr = v;
        break;
    case LPSPI_TDR:
        if (s->cr & CR_MEN) {
            lpspi_transfer(s, v);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @0x%02x=0x%08x\n",
                      __func__, (unsigned)offset, v);
        break;
    }
}

static const MemoryRegionOps imxrt1180_lpspi_ops = {
    .read = imxrt1180_lpspi_read,
    .write = imxrt1180_lpspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_lpspi_reset(DeviceState *dev)
{
    IMXRT1180LPSPIState *s = IMXRT1180_LPSPI(dev);

    s->cr = s->sr_sticky = s->ier = s->der = s->fcr = 0;
    /* TCR resets with FRAMESZ = 31 (a 32-bit frame).  Zero would be a 1-bit frame,
     * and LPSPI_MasterTransfer read-modify-writes TCR. */
    s->tcr = 0x0000001F;
    memset(s->cfgr, 0, sizeof(s->cfgr));
    memset(s->dmr, 0, sizeof(s->dmr));
    memset(s->ccr, 0, sizeof(s->ccr));
    s->rx_head = s->rx_count = 0;
    s->cs_active = -1;
    for (int i = 0; i < IMXRT1180_LPSPI_NUMCS; i++) {
        qemu_set_irq(s->cs_lines[i], 1);   /* deassert (active-low) */
    }
    qemu_set_irq(s->irq, 0);
    qemu_set_irq(s->dma_tx_req, 0);
    qemu_set_irq(s->dma_rx_req, 0);
}

static void imxrt1180_lpspi_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180LPSPIState *s = IMXRT1180_LPSPI(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_lpspi_ops, s,
                          TYPE_IMXRT1180_LPSPI, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qdev_init_gpio_out_named(dev, &s->dma_tx_req, "dma-tx-req", 1);
    qdev_init_gpio_out_named(dev, &s->dma_rx_req, "dma-rx-req", 1);
    for (int i = 0; i < IMXRT1180_LPSPI_NUMCS; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->cs_lines[i]);
    }
    /* Name the bus after the instance -- see the note in imxrt1180_lpi2c.c.  A
     * bus-less `-device` lands on whichever bus QEMU picks last, so a test that
     * does not pin its bus is not testing the controller it names. */
    {
        g_autofree char *bname = g_strdup_printf(
            "%s-bus", object_get_canonical_path_component(OBJECT(dev)));
            QEMU_BUILD_BUG_ON(FIFO_EXP(LPSPI_SILICON_FIFO) < 0);
        /* deliver at least what we advertise -- over-delivering is safe. */
        /*
         * 91emulator, 2026-07-14:
         *   "⭐ AN ASSERTION THAT CANNOT FIRE IS DECORATION.  And a BUILD_BUG_ON is the easiest
         *    place in C to write one, because it LOOKS like rigour and costs nothing to be wrong."
         *
         * Mine could fire -- but it compared two CONSTANTS, and the guest does not read constants.
         * It reads PARAM, decodes the exponent, and sizes itself.  Meanwhile the bytes land in an
         * ARRAY whose bound is spelled with a macro that a later hand-edit can simply bypass:
         *
         *      uint8_t rx_fifo[4];          <-- someone edits the BOUND, not the macro
         *                                       my old assertion compares macro vs macro: STILL PASSES
         *                                       PARAM still advertises 8.  THE FIFO IS NOW HALF THAT.
         *
         * So decode the register THE WAY THE GUEST DOES, and compare against the array WE ACTUALLY
         * HAVE.  That is the only pair of numbers whose disagreement is the bug.
         *
         *   ⭐ ASSERT AGAINST THE THING THE BYTES LAND IN, NOT AGAINST THE NAME YOU GAVE ITS SIZE.
         *      A capability drifts from its implementation precisely by someone touching the
         *      implementation without touching the name.
         *
         * The comparison is `>`, not `!=` (91's is `!=`, and for them that is right): our invariant
         * is MODEL >= ADVERTISED.  We deliberately over-deliver on LPI2C -- silicon 8, model 16 --
         * because a guest sized against the silicon always fits, HERE AND ON THE BOARD.  An `!=`
         * here would forbid the safe direction and force us to advertise what we hold, which is the
         * lie we just removed.
         */
                QEMU_BUILD_BUG_ON((1u << (LPSPI_PARAM_VALUE & 0xf)) >
                                  ARRAY_SIZE(((IMXRT1180LPSPIState *)0)->rx_fifo));
    s->bus = ssi_create_bus(dev, bname);
    }
    s->cs_active = -1;
}

/* Migration: re-drive the DMA-request output line from restored state.  This is
 * the DEVICE-TO-DEVICE line the eDMA sees -- unlike the NVIC IRQ (whose level the
 * NVIC's own vmstate restores), nothing else re-asserts dma_req, so a migrate
 * mid-DMA would stall the transfer on the destination without this. */
static int vmstate_imxrt1180_lpspi_post_load(void *opaque, int version_id)
{
    lpspi_update_dma(opaque);
    return 0;
}

static const VMStateDescription vmstate_imxrt1180_lpspi = {
    .name = TYPE_IMXRT1180_LPSPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = vmstate_imxrt1180_lpspi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr, IMXRT1180LPSPIState),
        VMSTATE_UINT32(sr_sticky, IMXRT1180LPSPIState),
        VMSTATE_UINT32(ier, IMXRT1180LPSPIState),
        VMSTATE_UINT32(der, IMXRT1180LPSPIState),
        VMSTATE_UINT32_ARRAY(cfgr, IMXRT1180LPSPIState, 2),
        VMSTATE_UINT32_ARRAY(dmr, IMXRT1180LPSPIState, 2),
        VMSTATE_UINT32_ARRAY(ccr, IMXRT1180LPSPIState, 2),
        VMSTATE_UINT32(fcr, IMXRT1180LPSPIState),
        VMSTATE_UINT32(tcr, IMXRT1180LPSPIState),
        VMSTATE_INT32(cs_active, IMXRT1180LPSPIState),
        VMSTATE_UINT32_ARRAY(rx_fifo, IMXRT1180LPSPIState, IMXRT1180_LPSPI_FIFO),
        VMSTATE_UINT8(rx_head, IMXRT1180LPSPIState),
        VMSTATE_UINT8(rx_count, IMXRT1180LPSPIState),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_lpspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_lpspi_realize;
    device_class_set_legacy_reset(dc, imxrt1180_lpspi_reset);
    dc->vmsd = &vmstate_imxrt1180_lpspi;
}

static const TypeInfo imxrt1180_lpspi_types[] = {
    {
        .name          = TYPE_IMXRT1180_LPSPI,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180LPSPIState),
        .class_init    = imxrt1180_lpspi_class_init,
    },
};

DEFINE_TYPES(imxrt1180_lpspi_types)
