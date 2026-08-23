/*
 * NXP i.MX RT1180 LPUART (console model)
 *
 * Standard NXP LPUART register block: TX is synchronous (always ready), RX is a
 * REAL 16-DEEP FIFO with a watermark -- because PARAM and FIFO both advertise 16 and
 * FSL_FEATURE_LPUART_FIFO_SIZEn(x) == 16, and a capability register is a CONTRACT.
 * (It was a one-byte holding register until 2026-07-13.  See lpuart_rdrf().)
 * Offsets and bit positions VERIFIED against the MIMXRT1189 CMSIS PERI_LPUART.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/char/imxrt1180_lpuart.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"  /* DEFINE_PROP_CHR */
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* --- LPUART register offsets ---------------------------------------------- */
#define LPUART_VERID    0x00  /* RO */
#define LPUART_PARAM    0x04  /* RO */
#define LPUART_GLOBAL   0x08
#define LPUART_PINCFG   0x0C
#define LPUART_BAUD     0x10
#define LPUART_STAT     0x14
#define LPUART_CTRL     0x18
#define LPUART_DATA     0x1C
#define LPUART_MATCH    0x20
#define LPUART_MODIR    0x24
#define LPUART_FIFO     0x28
#define LPUART_WATER    0x2C
#define LPUART_DATARO   0x30  /* RO */
#define LPUART_REIR     0x48
#define LPUART_TEIR     0x4C
#define LPUART_HDCR     0x50
#define LPUART_TOCR     0x58
#define LPUART_TOSR     0x5C
#define LPUART_TIMEOUT0 0x60
#define LPUART_TIMEOUT3 0x6C

/* --- Register bits (CMSIS-verified) --------------------------------------- */
#define GLOBAL_RST  0x00000002u

#define STAT_OR     0x00080000u
#define STAT_IDLE   0x00100000u
#define STAT_RDRF   0x00200000u
#define STAT_TC     0x00400000u
#define STAT_TDRE   0x00800000u

#define WATER_RXWATER_SHIFT 16
#define WATER_RXWATER_MASK  0x000F0000u
#define WATER_RXCOUNT_SHIFT 24
#define WATER_RXCOUNT_MASK  0x1F000000u

#define CTRL_RE     0x00040000u
#define CTRL_TE     0x00080000u
#define CTRL_RIE    0x00200000u
#define CTRL_TCIE   0x00400000u
#define CTRL_TIE    0x00800000u

#define DATA_RXEMPT 0x00001000u  /* PERI_LPUART.h DATA_RXEMPT_MASK */
#define FIFO_RXFE   0x00000008u   /* RX FIFO enable                         */
#define FIFO_RXFLUSH 0x00004000u  /* W1 self-clearing: flush the RX FIFO     */
#define FIFO_TXFLUSH 0x00008000u  /* W1 self-clearing: flush the TX FIFO     */
#define FIFO_RXEMPT 0x00400000u
#define FIFO_TXEMPT 0x00800000u

/* Reset values reported by the SDK's LPUART_GetInstance()/VERID probe. */
#define LPUART_VERID_VALUE  0x04010003u

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
 * PARAM: RXFIFO in bits 15:8, TXFIFO in bits 7:0, both as exponents.
 *
 * RX is a real IMXRT1180_LPUART_RXFIFO-deep FIFO -- so this is exactly what we deliver.
 * TX transmits SYNCHRONOUSLY in this model (every write goes straight to the chardev,
 * the FIFO never fills), so our TX depth is effectively UNBOUNDED and advertising the
 * same figure is an UNDER-promise.  Under-reporting is the safe direction on a
 * capability register; over-reporting is a promise the emulator makes on the chip's
 * behalf.  (91emulator)
 */
/* FSL_FEATURE_LPUART_FIFO_SIZEn(x) == 16 -- the SILICON's depth.  See the note in
 * imxrt1180_lpi2c.c: PARAM describes the CHIP, not this model, and the invariant is
 * model >= advertised. */
#define LPUART_SILICON_FIFO  16

#define LPUART_PARAM_VALUE                                                    \
    (((uint32_t)FIFO_EXP(LPUART_SILICON_FIFO) << 8) |                         \
      (uint32_t)FIFO_EXP(LPUART_SILICON_FIFO))

/* BAUD DMA-enable bits — PERI_LPUART.h (RT1180): TDMAE=23, RDMAE=21. */
#define BAUD_RDMAE  0x00200000u
#define BAUD_TDMAE  0x00800000u

/*
 * Drive the two eDMA request lines.
 *
 * TX: this model transmits synchronously, so TDRE is permanently set -- the TX
 * request is asserted whenever the guest enables it. That is honest: our TX FIFO
 * genuinely never fills, so a real "wait for TDRE" would be a wait on a condition
 * that is always true.
 * RX: asserted only while a byte is actually in the holding register (RDRF). A
 * DMA channel armed on LPUART RX therefore moves EXACTLY as many bytes as arrive
 * on the wire -- if the line were hardwired asserted, the channel would happily
 * copy the same stale byte CITER times and the test would still "pass".
 */
/*
 * RDRF is NOT "a byte arrived".  The RM defines it as
 *
 *     "datawords in the receive buffer GREATER THAN WATER[RXWATER]"
 *
 * and RXWATER RESETS TO ZERO -- which is the only reason a one-byte holding register
 * ever passed for a 16-deep FIFO here.  With no watermark set the two are identical.
 * Set one, and they are not: a driver that asks to be woken at 4 bytes and is woken at
 * 1 then reads 4, and THREE OF THEM ARE STALE.  Silent corruption, on the console.
 *
 *   ⭐ A CAPABILITY NOBODY EXERCISES AND A FRAME NOBODY INSPECTS ARE THE SAME BUG.
 *      THE SYSTEM IS NOT CORRECT -- IT IS UNTESTED IN THE ONE DIMENSION IT CLAIMS.
 *      (mcxn947qemu, who hit this on their own console the same evening.)
 *
 * When RXFE is clear the FIFO is bypassed and the receiver is one deep -- so RDRF is
 * simply "a byte is here", which is what the pre-FIFO model always did.
 */
static bool lpuart_rdrf(IMXRT1180LPUARTState *s)
{
    unsigned water;

    if (!(s->fifo & FIFO_RXFE)) {
        return s->rx_count > 0;          /* FIFO bypassed: 1-deep receiver */
    }
    water = (s->water & WATER_RXWATER_MASK) >> WATER_RXWATER_SHIFT;
    return s->rx_count > water;
}

static void imxrt1180_lpuart_update_dma(IMXRT1180LPUARTState *s)
{
    qemu_set_irq(s->dma_tx_req, !!(s->baud & BAUD_TDMAE));
    qemu_set_irq(s->dma_rx_req, (s->baud & BAUD_RDMAE) && lpuart_rdrf(s));
}

/*
 * Interrupt condition: TX data-register-empty and transmit-complete are always
 * asserted in this model (writes are synchronous), so TIE/TCIE assert
 * immediately; RIE asserts while the rx holding register is full.
 */
static void imxrt1180_lpuart_update_irq(IMXRT1180LPUARTState *s)
{
    bool tx = s->ctrl & (CTRL_TIE | CTRL_TCIE);
    bool rx = (s->ctrl & CTRL_RIE) && lpuart_rdrf(s);

    qemu_set_irq(s->irq, tx || rx);
}

static uint64_t imxrt1180_lpuart_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180LPUARTState *s = IMXRT1180_LPUART(opaque);
    uint32_t r = 0;

    switch (offset) {
    case LPUART_VERID:
        r = LPUART_VERID_VALUE;
        break;
    case LPUART_PARAM:
        r = LPUART_PARAM_VALUE;
        break;
    case LPUART_GLOBAL:
        r = s->global;
        break;
    case LPUART_PINCFG:
        r = s->pincfg;
        break;
    case LPUART_BAUD:
        r = s->baud;
        break;
    case LPUART_STAT:
        /* TX always ready; RDRF reflects the 1-byte rx holding register. */
        r = STAT_TDRE | STAT_TC;
        if (lpuart_rdrf(s)) {
            r |= STAT_RDRF;
        }
        break;
    case LPUART_CTRL:
        r = s->ctrl;
        break;
    case LPUART_DATA:
    case LPUART_DATARO:
        /*
         * RXEMPT (bit 12) -- WITHOUT IT, AN EMPTY RECEIVER HANDS OUT A PHANTOM NUL.
         *
         * This returned the raw byte and nothing else, so with no character received
         * DATA read 0x0000: RXEMPT CLEAR, data 0x00.  To a driver that is not "the
         * receiver is empty" -- IT IS THE BYTE 0x00, AND IT IS VALID.  The RM resets
         * DATA to 0x1000 for exactly this reason.
         *
         *   AN UNMODELLED BIT IS NOT A FREE BIT.  IT STILL ANSWERS, AND ZERO IS AN
         *   ANSWER.  (91emulator and mcxn947qemu found the same thing in LPI2C's
         *   MRDR alias the same day: "I was telling the guest the receive FIFO HAS
         *   DATA when nothing had been received.")
         */
        if (s->rx_count == 0) {
            r = DATA_RXEMPT;            /* honestly empty -- NOT a phantom NUL */
            break;
        }
        r = s->rx_fifo[s->rx_head];
        if (offset == LPUART_DATA) {    /* DATARO is a non-destructive peek */
            s->rx_head = (s->rx_head + 1) % IMXRT1180_LPUART_RXFIFO;
            s->rx_count--;
            imxrt1180_lpuart_update_irq(s);
            imxrt1180_lpuart_update_dma(s);
            /* Room again -- tell the chardev to resume input, or a continuous
             * stream stalls once the FIFO fills. */
            qemu_chr_fe_accept_input(&s->chr);
        }
        break;
    case LPUART_MATCH:
        r = s->match;
        break;
    case LPUART_MODIR:
        r = s->modir;
        break;
    case LPUART_FIFO:
        r = s->fifo | FIFO_TXEMPT;
        if (s->rx_count == 0) {
            r |= FIFO_RXEMPT;
        }
        break;
    case LPUART_WATER:
        /* RXCOUNT is LIVE -- it is how a driver sizes its next burst read.  It used
         * to read 0 forever, so a guest asking "how many bytes are waiting?" was told
         * NONE while a byte sat in the holding register. */
        r = (s->water & ~WATER_RXCOUNT_MASK) |
            ((uint32_t)s->rx_count << WATER_RXCOUNT_SHIFT);
        break;
    case LPUART_REIR:
        r = s->reir;
        break;
    case LPUART_TEIR:
        r = s->teir;
        break;
    case LPUART_HDCR:
        r = s->hdcr;
        break;
    case LPUART_TOCR:
        r = s->tocr;
        break;
    case LPUART_TOSR:
        r = s->tosr;
        break;
    case LPUART_TIMEOUT0 ... LPUART_TIMEOUT3:
        r = s->timeout[(offset - LPUART_TIMEOUT0) >> 2];
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled read @0x%03" HWADDR_PRIx "\n",
                      __func__, offset);
        break;
    }
    return r;
}

static void imxrt1180_lpuart_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    IMXRT1180LPUARTState *s = IMXRT1180_LPUART(opaque);
    uint8_t ch;

    switch (offset) {
    case LPUART_GLOBAL:
        s->global = value;
        if (value & GLOBAL_RST) {
            /* A software reset restores the RESET VALUES, not zero. */
            s->ctrl = s->water = 0;
            s->baud = 0x0F000004;
            s->fifo = 0x00C00033;
            s->rx_head = s->rx_count = 0;
            imxrt1180_lpuart_update_irq(s);
            imxrt1180_lpuart_update_dma(s);   /* BAUD cleared => requests drop */
        }
        break;
    case LPUART_PINCFG:
        s->pincfg = value;
        break;
    case LPUART_BAUD:
        s->baud = value;
        imxrt1180_lpuart_update_dma(s);
        break;
    case LPUART_STAT:
        /* STAT is computed on read; W1C flag writes have no standalone state. */
        break;
    case LPUART_CTRL:
        s->ctrl = value;
        imxrt1180_lpuart_update_irq(s);
        break;
    case LPUART_DATA:
        ch = value & 0xFF;
        if (s->ctrl & CTRL_TE) {          /* honour TE: only transmit if enabled */
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
        }
        imxrt1180_lpuart_update_irq(s);
        break;
    case LPUART_MATCH:
        s->match = value;
        break;
    case LPUART_MODIR:
        s->modir = value;
        break;
    case LPUART_FIFO:
        if (value & FIFO_RXFLUSH) {          /* W1, self-clearing: empty the FIFO */
            s->rx_head = s->rx_count = 0;
            qemu_chr_fe_accept_input(&s->chr);
        }
        s->fifo = value & ~(FIFO_RXFLUSH | FIFO_TXFLUSH);
        imxrt1180_lpuart_update_irq(s);
        imxrt1180_lpuart_update_dma(s);
        break;
    case LPUART_WATER:
        /* RXCOUNT/TXCOUNT are read-only status; the guest only owns the watermarks. */
        s->water = value & ~(WATER_RXCOUNT_MASK | 0x1F00u);
        imxrt1180_lpuart_update_irq(s);      /* a new watermark re-evaluates RDRF */
        imxrt1180_lpuart_update_dma(s);
        break;
    case LPUART_REIR:
        s->reir = value;
        break;
    case LPUART_TEIR:
        s->teir = value;
        break;
    case LPUART_HDCR:
        s->hdcr = value;
        break;
    case LPUART_TOCR:
        s->tocr = value;
        break;
    case LPUART_TOSR:
        /*
         * ================= TOSR IS W1C, AND I NEARLY SHIPPED A HANG =================
         *
         * The four timeout flags reset SET (TOSR = 0xF) and are WRITE-1-TO-CLEAR.
         * This was a PLAIN STORE.  With TOSR seeded to 0xF -- which I did today, to
         * make the reset-value gate green -- a driver clearing flag 0 writes 0x1, a
         * plain store SETS it, the driver polls for it to drop, AND SPINS FOREVER.
         *
         * Before the seed, TOSR read 0 and the flag was already clear, so the driver
         * never entered that loop.  I TOOK A BENIGN ZERO AND TURNED IT INTO A HANG,
         * AND THE GATE CALLED IT AN IMPROVEMENT.
         *
         *   ⭐ SEED CONFIG.  *IMPLEMENT* STATUS.
         *      A reset value is not a number you return -- it is the state a WORKING
         *      REGISTER STARTS IN.  If the register has behaviour, the reset value is
         *      only half the fix, and shipping the other half is not optional.
         *      (91emulator, 2026-07-13, who caught this on their own TOSR and warned
         *      the fleet hours after I had already done it.)
         */
        s->tosr &= ~(uint32_t)value;
        break;
    case LPUART_TIMEOUT0 ... LPUART_TIMEOUT3:
        s->timeout[(offset - LPUART_TIMEOUT0) >> 2] = value;
        break;
    case LPUART_VERID:
    case LPUART_PARAM:
    case LPUART_DATARO:
        /* read-only */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled write @0x%03" HWADDR_PRIx
                      " = 0x%08x\n", __func__, offset, (uint32_t)value);
        break;
    }
}

static const MemoryRegionOps imxrt1180_lpuart_ops = {
    .read = imxrt1180_lpuart_read,
    .write = imxrt1180_lpuart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * Accept 1/2/4-byte access.  A driver moving data over eDMA bursts the DATA
     * register one byte at a time; an over-strict 4-byte-only window would make
     * the memory core SILENTLY DROP those byte writes (fleet lesson: the
     * i.MX95 LPSPI-over-eDMA silent-drop).  impl.min=1 routes each byte to the
     * handler; the data register holds its value in the low byte.
     */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static int imxrt1180_lpuart_can_rx(void *opaque)
{
    IMXRT1180LPUARTState *s = IMXRT1180_LPUART(opaque);
    return (s->ctrl & CTRL_RE) && s->rx_count < IMXRT1180_LPUART_RXFIFO;
}

static void imxrt1180_lpuart_rx(void *opaque, const uint8_t *buf, int size)
{
    IMXRT1180LPUARTState *s = IMXRT1180_LPUART(opaque);

    for (int i = 0; i < size; i++) {
        if (s->rx_count >= IMXRT1180_LPUART_RXFIFO) {
            break;                       /* full: can_rx() should have stopped us */
        }
        s->rx_fifo[(s->rx_head + s->rx_count) % IMXRT1180_LPUART_RXFIFO] = buf[i];
        s->rx_count++;
    }
    imxrt1180_lpuart_update_irq(s);
    imxrt1180_lpuart_update_dma(s);
}

static void imxrt1180_lpuart_reset(DeviceState *dev)
{
    IMXRT1180LPUARTState *s = IMXRT1180_LPUART(dev);

    s->global = s->pincfg = s->ctrl = 0;
    s->match = s->modir = s->water = 0;
    s->reir = s->teir = s->hdcr = s->tocr = 0;

    /*
     * RESET VALUES from the RM's cold-POR column (tests/imxrt1180-reset-values).
     * A memset-to-zero is a CLAIM ABOUT EVERY BIT, and firmware read-modify-writes
     * all three of these -- BAUD in particular: LPUART_SetBaudRate() reads BAUD,
     * masks in OSR/SBR and writes it back, so a zeroed BAUD launders our lie into
     * the guest's own configuration.
     *   BAUD = 0x0F000004  (OSR = 15, SBR = 4)
     *   FIFO = 0x00C00033  (TX/RX FIFO size fields + the empty flags)
     *   TOSR = 0x0000000F
     */
    s->baud = 0x0F000004;
    s->fifo = 0x00C00033;
    s->tosr = 0x0000000F;
    s->timeout[0] = s->timeout[1] = s->timeout[2] = s->timeout[3] = 0;
    s->rx_head = s->rx_count = 0;
}

static void imxrt1180_lpuart_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180LPUARTState *s = IMXRT1180_LPUART(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    /* We must DELIVER at least what we ADVERTISE.  Over-delivering is safe; advertising
     * more than the silicon has fails only on hardware, where nobody is watching. */
    QEMU_BUILD_BUG_ON(FIFO_EXP(LPUART_SILICON_FIFO) < 0);
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
                QEMU_BUILD_BUG_ON((1u << (LPUART_PARAM_VALUE & 0xf)) >
                                  ARRAY_SIZE(((IMXRT1180LPUARTState *)0)->rx_fifo));

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_lpuart_ops, s,
                          TYPE_IMXRT1180_LPUART, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    qdev_init_gpio_out_named(dev, &s->dma_tx_req, "dma-tx-req", 1);
    qdev_init_gpio_out_named(dev, &s->dma_rx_req, "dma-rx-req", 1);

    qemu_chr_fe_set_handlers(&s->chr, imxrt1180_lpuart_can_rx,
                             imxrt1180_lpuart_rx, NULL, NULL, s, NULL, true);
}

/* Migration: re-drive the DMA-request output line from restored state.  This is
 * the DEVICE-TO-DEVICE line the eDMA sees -- unlike the NVIC IRQ (whose level the
 * NVIC's own vmstate restores), nothing else re-asserts dma_req, so a migrate
 * mid-DMA would stall the transfer on the destination without this. */
static int vmstate_imxrt1180_lpuart_post_load(void *opaque, int version_id)
{
    imxrt1180_lpuart_update_dma(opaque);
    return 0;
}

static const VMStateDescription vmstate_imxrt1180_lpuart = {
    .name = TYPE_IMXRT1180_LPUART,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = vmstate_imxrt1180_lpuart_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(global, IMXRT1180LPUARTState),
        VMSTATE_UINT32(pincfg, IMXRT1180LPUARTState),
        VMSTATE_UINT32(baud, IMXRT1180LPUARTState),
        VMSTATE_UINT32(ctrl, IMXRT1180LPUARTState),
        VMSTATE_UINT32(match, IMXRT1180LPUARTState),
        VMSTATE_UINT32(modir, IMXRT1180LPUARTState),
        VMSTATE_UINT32(fifo, IMXRT1180LPUARTState),
        VMSTATE_UINT32(water, IMXRT1180LPUARTState),
        VMSTATE_UINT32(reir, IMXRT1180LPUARTState),
        VMSTATE_UINT32(teir, IMXRT1180LPUARTState),
        VMSTATE_UINT32(hdcr, IMXRT1180LPUARTState),
        VMSTATE_UINT32(tocr, IMXRT1180LPUARTState),
        VMSTATE_UINT32(tosr, IMXRT1180LPUARTState),
        VMSTATE_UINT32_ARRAY(timeout, IMXRT1180LPUARTState, 4),
        VMSTATE_UINT8_ARRAY(rx_fifo, IMXRT1180LPUARTState, IMXRT1180_LPUART_RXFIFO),

        VMSTATE_UINT8(rx_head, IMXRT1180LPUARTState),

        VMSTATE_UINT8(rx_count, IMXRT1180LPUARTState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imxrt1180_lpuart_properties[] = {
    DEFINE_PROP_CHR("chardev", IMXRT1180LPUARTState, chr),
};

static void imxrt1180_lpuart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_lpuart_realize;
    device_class_set_legacy_reset(dc, imxrt1180_lpuart_reset);
    dc->vmsd = &vmstate_imxrt1180_lpuart;
    device_class_set_props(dc, imxrt1180_lpuart_properties);
}

static const TypeInfo imxrt1180_lpuart_types[] = {
    {
        .name          = TYPE_IMXRT1180_LPUART,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180LPUARTState),
        .class_init    = imxrt1180_lpuart_class_init,
    },
};

DEFINE_TYPES(imxrt1180_lpuart_types)
