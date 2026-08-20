/*
 * NXP MCX N eDMA (enhanced DMA) — functional model.  See header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/dma/imxrt1180_edma.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

/*
 * A software service request (TCD_CSR[START]) does NOT complete instantly: the
 * transfer takes real bus time on silicon, and firmware depends on it.  The SDK's
 * InitCM7DMA issues START, then W1C-clears CH_CSR[DONE] to drop any STALE flag,
 * then polls DONE for THIS transfer -- a sequence that is only correct while the
 * transfer is still IN FLIGHT across the clear.  An instant (BH-at-next-TB)
 * completion sets DONE before the clear, the clear wipes it, and the poll hangs
 * forever.  So we complete on a QEMU_CLOCK_VIRTUAL timer after a first-order
 * duration -- long enough that the transfer is genuinely mid-flight while the
 * driver clears the stale flag and enters its poll (the same class of fix as the
 * LPADC RESFIFO "instant conversion racing the ISR").
 *
 * The RATE below is a modelled AXI-ish first order (eDMA4 ~240 MHz x 64-bit) and is
 * NOT asserted by any test -- only the ORDERING property is (tests/imxrt1180-edma-
 * swstart-order).  The floor keeps even a tiny transfer in flight past the handful
 * of instructions between the driver's START and its DONE-clear.
 */
#define EDMA_SW_NS_PER_BYTE   1u        /* ~1 GB/s: first-order, not a golden */
#define EDMA_SW_MIN_NS        1000u     /* ordering floor (see above) */

/* Management-page registers. */
#define R_MP_CSR  0x00
#define R_MP_ES   0x04
#define R_MP_INT  0x08
#define R_MP_HRS  0x0C
#define R_CH_GRPRI 0x100   /* [16] */

/* Per-channel register offsets (within the channel's 0x1000 block). */
#define R_CH_CSR   0x00
#define R_CH_ES    0x04
#define R_CH_INT   0x08
#define R_CH_SBR   0x0C
#define R_CH_PRI   0x10
#define R_CH_MUX   0x14
#define R_CH_MATTR 0x18   /* DMA4 only; reserved on DMA3 */
#define R_TCD_SADDR 0x20
#define R_TCD_SOFF  0x24
#define R_TCD_ATTR  0x26
#define R_TCD_NBYTES 0x28
#define R_TCD_SLAST 0x2C
#define R_TCD_DADDR 0x30
#define R_TCD_DOFF  0x34
#define R_TCD_CITER 0x36
#define R_TCD_DLAST 0x38
#define R_TCD_CSR   0x3C
#define R_TCD_BITER 0x3E

#define CH_CSR_ERQ   (1u << 0)
#define CH_CSR_DONE  (1u << 30)
#define CH_INT_INT   (1u << 0)
#define TCD_CSR_START    (1u << 0)
#define TCD_CSR_INTMAJOR (1u << 1)
#define TCD_CSR_DREQ     (1u << 3)   /* auto-clear ERQ at major completion */
#define TCD_CSR_ESG      (1u << 4)   /* scatter-gather: fetch the next TCD */
#define CH_MUX_SRC_MASK  0xFFu       /* PERI_DMA4.h DMA4_CH_MUX_SRC_MASK       */

/*
 * CHANNEL-BLOCK GEOMETRY -- and this was WRONG, silently, for the life of the model.
 *
 *   DMA3 (PERI_DMA.h  DMA_Type):  CH[n] at base + 0x10000 + n * 0x10000
 *   DMA4 (PERI_DMA4.h DMA4_Type): TCD[n] at base + 0x10000 + n * 0x8000
 *
 * This model had channel n at base + 0x1000 * (n + 1) -- inherited verbatim from
 * the MCXN947 eDMA it was adapted from, and never re-derived from the RT1180
 * CMSIS header. The register offsets WITHIN a block were right; the block base and
 * stride were not.
 *
 * ⚠ AND EVERY eDMA TEST WE HAD PASSED ANYWAY, because every one of them was
 * written against the MODEL'S addresses. The test and the model shared the same
 * wrong map, so the test could only ever confirm the model agreed with itself.
 * The stock NXP driver, which reads the addresses from the CMSIS header, hung --
 * its DMA4 channel-0 writes landed on our channel 15, so the completion interrupt
 * went to NVIC 135 while the guest was waiting on 128. THAT is what a real driver
 * is for. (Class IV, "the oracle you wrote yourself".)
 */
#define CH_BLOCK_BASE    0x10000u
#define ATTR_SSIZE(a)  (((a) >> 8) & 0x7)
#define ATTR_DSIZE(a)  ((a) & 0x7)
#define NBYTES_MASK  0x3FFFFFFFu

/*
 * CITER/BITER ARE TWO DIFFERENT REGISTERS DEPENDING ON ONE BIT, and that bit is
 * inside them (PERI_DMA4.h):
 *
 *   ELINK = 0  -> CITER_ELINKNO :  CITER is 15 bits (0x7FFF)
 *   ELINK = 1  -> CITER_ELINKYES:  CITER is  9 bits (0x1FF), and bits 9..14 are
 *                                  LINKCH -- the channel to link to.
 *
 * A fixed 0x7FFF mask therefore reads a linked channel's major count as
 * `citer | (linkch << 9)`: a silently, wildly wrong iteration count, with no
 * wrong offset and no hang to give it away. (CLAUDE.md: a wrong COUNT truncates
 * quietly, which is worse than a wrong offset.)
 */
#define CITER_ELINK      0x8000u
#define CITER_LINKCH_MASK 0x7E00u
#define CITER_LINKCH_SHIFT 9
#define CITER_MASK_NOLINK 0x7FFFu
#define CITER_MASK_LINK   0x01FFu

/* Major-loop channel link: TCD_CSR[MAJORELINK], TCD_CSR[MAJORLINKCH]. */
#define TCD_CSR_MAJORELINK  0x0020u
#define TCD_CSR_MAJORLINKCH_MASK  0x3F00u
#define TCD_CSR_MAJORLINKCH_SHIFT 8

static inline uint32_t citer_mask(uint16_t reg)
{
    return (reg & CITER_ELINK) ? CITER_MASK_LINK : CITER_MASK_NOLINK;
}
static inline unsigned citer_linkch(uint16_t reg)
{
    return (reg & CITER_LINKCH_MASK) >> CITER_LINKCH_SHIFT;
}

static void edma_update_irq(IMXRT1180EDMAState *s, int n)
{
    qemu_set_irq(s->irq[n], !!(s->ch[n].intr & CH_INT_INT));
}

/*
 * Move ONE MINOR LOOP (NBYTES) and account for it. Returns true if the MAJOR loop
 * completed. This is the unit a hardware request buys you: the peripheral's FIFO
 * says "I can take/give NBYTES", the channel moves exactly that, CITER decrements,
 * and the peripheral must ask again. A software START runs it CITER times back to
 * back instead.
 */
static bool edma_minor_loop(IMXRT1180EDMAState *s, int n);

/*
 * CHANNEL LINKING. RM 5.5.3: "one channel sets the TCDn_CSR[START] field of
 * ANOTHER CHANNEL (OR ITSELF), thus initiating a service request for that
 * channel." So a link is exactly one service request = one minor loop -- the same
 * event as a software START or a peripheral request.
 *
 * "or itself" is why the depth guard exists and is not paranoia: a channel may
 * legally link to itself, and A->B->A is legal too. Real hardware bounds this with
 * its own arbitration; we bound it with a depth counter and stop, rather than
 * recursing until the host stack dies.
 */
/*
 * SCATTER-GATHER. RM 5.5: at major-loop exhaustion, "a fetch of a new TCD from
 * memory using the scatter/gather address pointer included in the descriptor (if
 * scatter/gather is enabled)".
 *
 * The pointer lives in TCD_DLAST_SGA -- the SAME register that otherwise holds the
 * destination address adjustment. So when ESG is set, DLAST is NOT an address
 * adjustment and must not be added to DADDR; it is a pointer. One register, two
 * meanings, selected by a bit in a different register.
 *
 * The in-memory descriptor mirrors the channel's TCD register file exactly -- the
 * offsets below are the PERI_DMA4.h channel-block offsets 0x20..0x3E with 0x20
 * subtracted, which is what makes scatter-gather a straight register reload.
 *
 * The fetch does NOT run the new descriptor: the channel still needs a service
 * request. The stock edma4/scatter_gather example says so by calling
 * EDMA_TriggerChannelStart() a second time, commented "Trigger the second tcd".
 */
static void edma_load_tcd(IMXRT1180EDMAState *s, int n, uint32_t addr)
{
    IMXRT1180EDMAChan *c = &s->ch[n];
    uint8_t d[32];

    address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                       d, sizeof(d));
    c->tcd_saddr  = ldl_le_p(d + 0x00);
    c->tcd_soff   = lduw_le_p(d + 0x04);
    c->tcd_attr   = lduw_le_p(d + 0x06);
    c->tcd_nbytes = ldl_le_p(d + 0x08);
    c->tcd_slast  = ldl_le_p(d + 0x0C);
    c->tcd_daddr  = ldl_le_p(d + 0x10);
    c->tcd_doff   = lduw_le_p(d + 0x14);
    c->tcd_citer  = lduw_le_p(d + 0x16);
    c->tcd_dlast  = ldl_le_p(d + 0x18);
    c->tcd_csr    = lduw_le_p(d + 0x1C);
    c->tcd_biter  = lduw_le_p(d + 0x1E);
    c->tcd_csr   &= ~TCD_CSR_START;   /* a fetched TCD does not self-trigger */
}

static void edma_link(IMXRT1180EDMAState *s, unsigned ch)
{
    if (ch >= s->num_channels || s->link_depth >= IMXRT1180_EDMA_MAX_LINK_DEPTH) {
        return;
    }
    s->link_depth++;
    s->ch[ch].tcd_csr &= ~TCD_CSR_START;   /* auto-cleared as the channel executes */
    s->ch[ch].csr &= ~CH_CSR_DONE;
    (void)edma_minor_loop(s, ch);
    s->link_depth--;
}

static bool edma_minor_loop(IMXRT1180EDMAState *s, int n)
{
    IMXRT1180EDMAChan *c = &s->ch[n];
    uint32_t ssize = 1u << ATTR_SSIZE(c->tcd_attr);
    uint32_t dsize = 1u << ATTR_DSIZE(c->tcd_attr);
    uint32_t nbytes = c->tcd_nbytes & NBYTES_MASK;
    uint32_t cmask = citer_mask(c->tcd_citer);
    uint32_t citer = c->tcd_citer & cmask;
    int16_t soff = (int16_t)c->tcd_soff;
    int16_t doff = (int16_t)c->tcd_doff;
    uint8_t buf[32];
    uint32_t step = ssize ? ssize : 1;
    uint32_t b;

    if (dsize == 0 || nbytes == 0 || citer == 0) {
        c->csr |= CH_CSR_DONE;
        return true;
    }
    if (step > sizeof(buf)) {
        step = sizeof(buf);
    }

    for (b = 0; b + step <= nbytes; b += step) {
        address_space_read(&address_space_memory, c->tcd_saddr,
                           MEMTXATTRS_UNSPECIFIED, buf, step);
        address_space_write(&address_space_memory, c->tcd_daddr,
                            MEMTXATTRS_UNSPECIFIED, buf, step);
        c->tcd_saddr += soff;
        c->tcd_daddr += doff;
    }

    c->tcd_citer = (c->tcd_citer & ~cmask) | ((citer - 1) & cmask);
    if ((c->tcd_citer & cmask) != 0) {
        /*
         * MINOR-LOOP CHANNEL LINK. RM 5.5.3: "the channel link is made after each
         * iteration of the major loop EXCEPT FOR THE LAST. When the major loop is
         * exhausted, ONLY the major loop channel link fields are used." So this
         * lives here, on the not-yet-complete path, and never races the major link.
         */
        if (c->tcd_citer & CITER_ELINK) {
            edma_link(s, citer_linkch(c->tcd_citer));
        }
        return false;                          /* major loop still running */
    }

    /* ---- major loop complete ---- */
    c->tcd_saddr += (int32_t)c->tcd_slast;
    if (!(c->tcd_csr & TCD_CSR_ESG)) {
        c->tcd_daddr += (int32_t)c->tcd_dlast;   /* DLAST is an ADJUSTMENT ... */
    }                                            /* ... unless ESG, when it is a
                                                  * POINTER. See edma_load_tcd(). */
    c->tcd_citer = c->tcd_biter;               /* reload major count (BITER mirrors
                                                * CITER's layout, ELINK/LINKCH too) */
    c->csr |= CH_CSR_DONE;
    if (c->tcd_csr & TCD_CSR_DREQ) {
        c->csr &= ~CH_CSR_ERQ;                 /* hardware requests off */
    }
    if (c->tcd_csr & TCD_CSR_INTMAJOR) {
        c->intr |= CH_INT_INT;
        edma_update_irq(s, n);
    }
    /* MAJOR-LOOP CHANNEL LINK -- TCD_CSR[MAJORELINK]/[MAJORLINKCH]. */
    if (c->tcd_csr & TCD_CSR_MAJORELINK) {
        edma_link(s, (c->tcd_csr & TCD_CSR_MAJORLINKCH_MASK)
                     >> TCD_CSR_MAJORLINKCH_SHIFT);
    }
    /* SCATTER-GATHER: pull in the next descriptor. Last, so it overwrites the
     * reloaded CITER/addresses above with the new TCD's own values. */
    if (c->tcd_csr & TCD_CSR_ESG) {
        edma_load_tcd(s, n, c->tcd_dlast);
    }
    return true;
}

/*
 * Bottom half: drain every channel whose selected request line is asserted.
 *
 * MUST NOT run inline from the peripheral's MMIO write handler -- see the header.
 * A DMA write back into the requesting peripheral would be a re-entrant MMIO
 * access, QEMU would silently drop it, and the channel would still report DONE +
 * INTMAJOR over a FIFO that never received a byte.
 */
static void edma_service_bh(void *opaque)
{
    IMXRT1180EDMAState *s = opaque;
    bool progressed;
    unsigned guard = 0;

    do {
        progressed = false;
        for (unsigned n = 0; n < s->num_channels; n++) {
            IMXRT1180EDMAChan *c = &s->ch[n];
            unsigned src = c->mux & CH_MUX_SRC_MASK;

            /*
             * THE TWO GATES. Both are re-evaluated on EVERY pass of the outer
             * loop, which is what makes them gates rather than a one-time
             * admission check:
             *
             *  ERQ      -- edma_minor_loop() clears it at major completion when
             *              TCD_CSR[DREQ] is set. Re-reading it here is what stops
             *              a channel whose ERQ has just gone away from being
             *              serviced again by the same drain.
             *  req[src] -- a minor loop's own MMIO access can LOWER the line that
             *              caused it: the DMA reads LPUART DATA, the holding
             *              register empties, RDRF drops, and the RX request is
             *              gone until the next byte lands. Re-reading it is what
             *              stops the channel from copying the same stale byte
             *              CITER times -- which would still be byte-count-correct
             *              and still pass any test that only checks the count.
             *
             * (95emulator reported that gating ERQ in one place was not enough to
             * be caught by a mutation. That is TRUE OF THEIR STRUCTURE, not of
             * this one: an earlier draft of this function read ERQ twice in a row
             * with nothing in between that could change it, so the second read was
             * DEAD CODE wearing a comment that claimed it was a defence. Deleted.
             * Import a peer's conclusion, not their control flow.)
             */
            if (!(c->csr & CH_CSR_ERQ)) {
                continue;
            }
            if (src == 0 || src >= IMXRT1180_EDMA_NUM_REQ || !s->req[src]) {
                continue;
            }

            /*
             * ONE MINOR LOOP PER PASS -- not the whole major loop. A hardware
             * request buys exactly NBYTES; the peripheral must ask again for the
             * next NBYTES. Draining the major loop here instead would "work" for
             * a TX (the line is held asserted anyway) and would silently corrupt
             * every RX (32 copies of byte 0).
             */
            (void)edma_minor_loop(s, n);
            progressed = true;
        }
    } while (progressed && ++guard < 100000);
}

/*
 * Software service requests complete here, off a virtual-time timer -- NOT at the
 * next TB boundary like the hardware-request BH.  That deferral-in-VIRTUAL-TIME is
 * the whole point: the transfer must still be running while the driver clears a
 * stale CH_CSR[DONE] and enters its poll.  Drain every channel's pending STARTs (a
 * COUNTER, so N back-to-back STARTs each buy exactly one minor loop).
 */
static void edma_sw_complete(void *opaque)
{
    IMXRT1180EDMAState *s = opaque;

    for (unsigned n = 0; n < s->num_channels; n++) {
        IMXRT1180EDMAChan *c = &s->ch[n];
        while (c->sw_start_pending > 0) {
            c->sw_start_pending--;
            (void)edma_minor_loop(s, n);
        }
    }
}

/* A peripheral asserted/deasserted its DMA request line. */
static void edma_req_set(void *opaque, int src, int level)
{
    IMXRT1180EDMAState *s = opaque;

    if (src < 0 || src >= IMXRT1180_EDMA_NUM_REQ) {
        return;
    }
    s->req[src] = !!level;
    if (level) {
        qemu_bh_schedule(s->bh);   /* service OUTSIDE this MMIO dispatch */
    }
}

/*
 * A SOFTWARE service request is TCD_CSR[START]: ONE MINOR LOOP, exactly like a
 * hardware request -- RM 5.4: "software and the TCDn_CSR[START] field follows the
 * same basic flow as peripheral requests", and fsl_edma.h's
 * EDMA_TriggerChannelStart(): "This function starts a MINOR LOOP transfer." A
 * channel with CITER=N needs N of these.  (It used to drain the WHOLE major loop;
 * see the retraction in the header -- every test we had used CITER=1, where the two
 * are indistinguishable.)
 *
 * It is serviced DEFERRED, in edma_service_bh via sw_start_pending, not inline from
 * the TCD_CSR write -- so the transfer completes after that write returns.  See the
 * sw_start_pending comment in the header for why (InitCM7DMA's clear-then-poll).
 */

static uint64_t edma_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180EDMAState *s = IMXRT1180_EDMA(opaque);
    IMXRT1180EDMAChan *c;
    int n;

    if (off < CH_BLOCK_BASE) {          /* management page */
        switch (off) {
        case R_MP_CSR: return s->mp_csr;
        case R_MP_ES:  return s->mp_es;
        case R_MP_INT: {
            uint32_t r = 0;
            for (n = 0; n < s->num_channels && n < 32; n++) {
                if (s->ch[n].intr & CH_INT_INT) {
                    r |= (1u << n);
                }
            }
            return r;
        }
        case R_MP_HRS: return 0;
        default:
            if (off >= R_CH_GRPRI && off < R_CH_GRPRI + 4 * s->num_channels) {
                return s->ch_grpri[(off - R_CH_GRPRI) / 4];
            }
            return 0;
        }
    }

    n = (off - CH_BLOCK_BASE) / s->ch_stride;
    if (off < CH_BLOCK_BASE || n >= s->num_channels) {
        return 0;
    }
    c = &s->ch[n];
    switch ((off - CH_BLOCK_BASE) % s->ch_stride) {
    case R_CH_CSR:    return c->csr;
    case R_CH_ES:     return c->es;
    case R_CH_INT:    return c->intr;
    case R_CH_SBR:    return c->sbr;
    case R_CH_PRI:    return c->pri;
    case R_CH_MUX:    return c->mux;
    case R_CH_MATTR:  return c->mattr;
    case R_TCD_SADDR: return c->tcd_saddr;
    case R_TCD_SOFF:  return c->tcd_soff;
    case R_TCD_ATTR:  return c->tcd_attr;
    case R_TCD_NBYTES: return c->tcd_nbytes;
    case R_TCD_SLAST: return c->tcd_slast;
    case R_TCD_DADDR: return c->tcd_daddr;
    case R_TCD_DOFF:  return c->tcd_doff;
    case R_TCD_CITER: return c->tcd_citer;
    case R_TCD_DLAST: return c->tcd_dlast;
    case R_TCD_CSR:   return c->tcd_csr;
    case R_TCD_BITER: return c->tcd_biter;
    default:          return 0;
    }
}

static void edma_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IMXRT1180EDMAState *s = IMXRT1180_EDMA(opaque);
    IMXRT1180EDMAChan *c;
    uint32_t v = val;
    int n;

    if (off < CH_BLOCK_BASE) {          /* management page */
        switch (off) {
        case R_MP_CSR: s->mp_csr = v; return;
        default:
            if (off >= R_CH_GRPRI && off < R_CH_GRPRI + 4 * s->num_channels) {
                s->ch_grpri[(off - R_CH_GRPRI) / 4] = v;
            }
            return;
        }
    }

    n = (off - CH_BLOCK_BASE) / s->ch_stride;
    if (off < CH_BLOCK_BASE || n >= s->num_channels) {
        return;
    }
    c = &s->ch[n];
    switch ((off - CH_BLOCK_BASE) % s->ch_stride) {
    case R_CH_CSR:
        /* DONE is write-1-to-clear; keep the rest. */
        if (v & CH_CSR_DONE) {
            c->csr &= ~CH_CSR_DONE;
        }
        c->csr = (c->csr & CH_CSR_DONE) | (v & ~CH_CSR_DONE);
        if (c->csr & CH_CSR_ERQ) {
            /*
             * Arming ERQ on a source that is ALREADY asserting (an idle LPUART's
             * TX line, say) must start the transfer -- the peripheral will not
             * re-raise an edge it is already holding. Schedule, never run inline.
             */
            qemu_bh_schedule(s->bh);
        }
        return;
    case R_CH_INT:
        if (v & CH_INT_INT) {                 /* write-1-to-clear */
            c->intr &= ~CH_INT_INT;
            edma_update_irq(s, n);
        }
        return;
    case R_CH_ES:   c->es = v; return;
    case R_CH_SBR:  c->sbr = v; return;
    case R_CH_PRI:  c->pri = v; return;
    case R_CH_MUX:
        c->mux = v;
        qemu_bh_schedule(s->bh);   /* the selected source may already be asserted */
        return;
    case R_CH_MATTR: c->mattr = v; return;   /* DMA4 only; DMA3 has it reserved */
    case R_TCD_SADDR: c->tcd_saddr = v; return;
    case R_TCD_SOFF:  c->tcd_soff = v; return;
    case R_TCD_ATTR:  c->tcd_attr = v; return;
    case R_TCD_NBYTES: c->tcd_nbytes = v; return;
    case R_TCD_SLAST: c->tcd_slast = v; return;
    case R_TCD_DADDR: c->tcd_daddr = v; return;
    case R_TCD_DOFF:  c->tcd_doff = v; return;
    case R_TCD_CITER: c->tcd_citer = v; return;
    case R_TCD_DLAST: c->tcd_dlast = v; return;
    case R_TCD_CSR:
        c->tcd_csr = v;
        if (v & TCD_CSR_START) {
            /*
             * START is auto-cleared by hardware "when the channel begins
             * execution, regardless of how the channel was activated" (RM 5.5.4).
             * It is NOT a sticky control bit, and firmware is entitled to poll it:
             * the RM's own minor-loop-complete test is TCD_CSR[START] and
             * CH_CSR[ACTIVE] both reading 0. Leaving it set would hang that poll.
             *
             * The minor loop runs DEFERRED (via the service BH), never inline: the
             * transfer must complete AFTER this write returns, or the SDK's
             * InitCM7DMA (START -> W1C stale DONE -> poll DONE) sees its fresh DONE
             * cleared and hangs.  DONE is cleared here because a just-started
             * transfer is, by definition, not done.  See sw_start_pending in the
             * header + edma_service_bh.
             */
            c->tcd_csr &= ~TCD_CSR_START;
            c->csr &= ~CH_CSR_DONE;
            c->sw_start_pending++;
            if (!timer_pending(s->sw_timer)) {
                uint32_t nbytes = c->tcd_nbytes & NBYTES_MASK;
                uint64_t dur = (uint64_t)nbytes * EDMA_SW_NS_PER_BYTE;
                if (dur < EDMA_SW_MIN_NS) {
                    dur = EDMA_SW_MIN_NS;
                }
                timer_mod(s->sw_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + dur);
            }
        }
        return;
    case R_TCD_BITER: c->tcd_biter = v; return;
    default: return;
    }
}

static const MemoryRegionOps edma_ops = {
    .read = edma_read,
    .write = edma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void imxrt1180_edma_reset(DeviceState *dev)
{
    IMXRT1180EDMAState *s = IMXRT1180_EDMA(dev);

    s->mp_csr = 0;
    s->mp_es = 0;
    memset(s->ch_grpri, 0, sizeof(s->ch_grpri));
    memset(s->ch, 0, sizeof(s->ch));
    /*
     * s->req is deliberately NOT cleared: it mirrors the LEVEL of an input line
     * that the SOURCE peripheral drives, and the source owns that state. An idle
     * LPUART holds its TX request asserted; zeroing our copy here would make the
     * eDMA blind to a line nobody is going to re-raise (the peripheral sees no
     * edge -- it is already high).
     */
}

static void imxrt1180_edma_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180EDMAState *s = IMXRT1180_EDMA(dev);
    int n;

    if (s->num_channels == 0 || s->num_channels > IMXRT1180_EDMA_MAX_CHANNELS) {
        s->num_channels = IMXRT1180_EDMA_MAX_CHANNELS;
    }
    /* Management page (0x0) + num_channels x 0x1000 blocks. */
    if (s->ch_stride == 0) {
        s->ch_stride = 0x10000;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &edma_ops, s, TYPE_IMXRT1180_EDMA,
                          CH_BLOCK_BASE + (uint64_t)s->ch_stride * s->num_channels);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (n = 0; n < s->num_channels; n++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[n]);
    }

    /*
     * The 128 peripheral DMA request lines (PERI_DMA4.h kDma3RequestMux*).
     * A source peripheral raises its line from inside its OWN MMIO handler, so
     * these are serviced from a bottom half -- never inline. See the header.
     */
    qdev_init_gpio_in_named(dev, edma_req_set, "dma-req", IMXRT1180_EDMA_NUM_REQ);
    s->bh = qemu_bh_new(edma_service_bh, s);
    s->sw_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, edma_sw_complete, s);
}

static const VMStateDescription vmstate_edma_chan = {
    .name = "imxrt1180-edma-chan",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sw_start_pending, IMXRT1180EDMAChan),
        VMSTATE_UINT32(csr, IMXRT1180EDMAChan),
        VMSTATE_UINT32(es, IMXRT1180EDMAChan),
        VMSTATE_UINT32(intr, IMXRT1180EDMAChan),
        VMSTATE_UINT32(sbr, IMXRT1180EDMAChan),
        VMSTATE_UINT32(pri, IMXRT1180EDMAChan),
        VMSTATE_UINT32(mux, IMXRT1180EDMAChan),
        VMSTATE_UINT32(tcd_saddr, IMXRT1180EDMAChan),
        VMSTATE_UINT32(tcd_slast, IMXRT1180EDMAChan),
        VMSTATE_UINT32(tcd_daddr, IMXRT1180EDMAChan),
        VMSTATE_UINT32(tcd_dlast, IMXRT1180EDMAChan),
        VMSTATE_UINT32(tcd_nbytes, IMXRT1180EDMAChan),
        VMSTATE_UINT16(tcd_soff, IMXRT1180EDMAChan),
        VMSTATE_UINT16(tcd_attr, IMXRT1180EDMAChan),
        VMSTATE_UINT16(tcd_doff, IMXRT1180EDMAChan),
        VMSTATE_UINT16(tcd_citer, IMXRT1180EDMAChan),
        VMSTATE_UINT16(tcd_csr, IMXRT1180EDMAChan),
        VMSTATE_UINT16(tcd_biter, IMXRT1180EDMAChan),
        VMSTATE_END_OF_LIST()
    },
};

/* Migration: re-assert each channel's IRQ line from restored state. */
static int vmstate_imxrt1180_edma_post_load(void *opaque, int version_id)
{
    IMXRT1180EDMAState *s = opaque;
    for (unsigned n = 0; n < s->num_channels; n++) {
        edma_update_irq(s, n);
    }
    return 0;
}

static const VMStateDescription vmstate_imxrt1180_edma = {
    .name = TYPE_IMXRT1180_EDMA,
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = vmstate_imxrt1180_edma_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mp_csr, IMXRT1180EDMAState),
        VMSTATE_UINT32(mp_es, IMXRT1180EDMAState),
        VMSTATE_UINT32_ARRAY(ch_grpri, IMXRT1180EDMAState, IMXRT1180_EDMA_MAX_CHANNELS),
        VMSTATE_STRUCT_ARRAY(ch, IMXRT1180EDMAState, IMXRT1180_EDMA_MAX_CHANNELS, 2,
                             vmstate_edma_chan, IMXRT1180EDMAChan),
        VMSTATE_TIMER_PTR(sw_timer, IMXRT1180EDMAState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imxrt1180_edma_props[] = {
    DEFINE_PROP_UINT32("num-channels", IMXRT1180EDMAState, num_channels, 32),
    /* DMA3 = 0x10000 (PERI_DMA.h), DMA4 = 0x8000 (PERI_DMA4.h). No default that
     * silently works: the two instances genuinely differ and the board must say. */
    DEFINE_PROP_UINT32("channel-stride", IMXRT1180EDMAState, ch_stride, 0x10000),
};

static void imxrt1180_edma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_edma_realize;
    device_class_set_legacy_reset(dc, imxrt1180_edma_reset);
    device_class_set_props(dc, imxrt1180_edma_props);
    dc->vmsd = &vmstate_imxrt1180_edma;
}

static const TypeInfo imxrt1180_edma_types[] = {
    {
        .name          = TYPE_IMXRT1180_EDMA,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180EDMAState),
        .class_init    = imxrt1180_edma_class_init,
    },
};

DEFINE_TYPES(imxrt1180_edma_types)
