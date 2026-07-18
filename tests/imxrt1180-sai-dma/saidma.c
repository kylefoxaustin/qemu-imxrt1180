/*
 * i.MX RT1180 — SAI TX driven by eDMA HARDWARE REQUEST (Cortex-M33).
 *
 * The PIO SAI test (tests/imxrt1180-sai) proves the transmit path renders real
 * audio when the CPU feeds TDR.  This test proves the OTHER feed path — the one
 * every real audio driver uses: the SAI's TX FIFO asserts a DMA REQUEST LINE when
 * it drains to the watermark, the eDMA services it, and the CPU never touches TDR.
 *
 * It is the SAI half of the request-line mechanism the LPUART exercises in
 * tests/imxrt1180-dmareq.  What is NEW and SAI-specific here is that the SAI has
 * to ASSERT the line at all, and has to gate that assertion on TCSR[FRDE] — the
 * FIFO-Request DMA-Enable bit the SDK's sai_edma driver sets.  So:
 *
 *   NEG  TE=1, ERQ=1, TCD armed, FIFO empty (so the FIFO-request CONDITION holds)
 *        but **FRDE = 0** -> the SAI must NOT assert the line, so not one word may
 *        move.  The FIFO condition is genuinely satisfied, so a model that keyed
 *        the request on the FIFO alone (ignoring FRDE) would run the transfer —
 *        this is the non-vacuous negative that catches it.
 *   POS  FRDE = 1 -> the line asserts, the eDMA fills the FIFO from memory, the
 *        samples clock out, and the wav (rendered outside the guest) is byte-exact.
 *
 * The observable for the negative is the eDMA's own TCD_SADDR / CITER / DONE: if
 * the channel moved anything, SADDR walked forward off the source buffer.  The
 * observable for the positive is the SAMPLES in the wav — the verdict is rendered
 * OUTSIDE the guest, because a verdict computed inside it cannot see a FIFO that
 * clocked silence (see the header of hw/misc/imxrt1180_sai.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* ---- CCM: a REAL MCLK for SAI1 (root 65, OSC_RC_24M, divisor 1) --------- */
#define CCM_BASE   0x44450000u
#define CCM_ROOT_CTRL(n) (*(volatile uint32_t *)(CCM_BASE + (n) * 0x80u))
#define CCM_ROOT_SAI1    65u

/* ---- SAI1 (I2S, CMSIS offsets) ----------------------------------------- */
#define SAI1  0x443B0000u
#define TCSR (*(volatile uint32_t *)(SAI1 + 0x08))
#define TCR1 (*(volatile uint32_t *)(SAI1 + 0x0C))
#define TCR2 (*(volatile uint32_t *)(SAI1 + 0x10))
#define TCR3 (*(volatile uint32_t *)(SAI1 + 0x14))
#define TCR4 (*(volatile uint32_t *)(SAI1 + 0x18))
#define TCR5 (*(volatile uint32_t *)(SAI1 + 0x1C))
#define TDR0_ADDR         (SAI1 + 0x20)
#define TFR0 (*(volatile uint32_t *)(SAI1 + 0x40))
#define PARAM (*(volatile uint32_t *)(SAI1 + 0x04))

#define TCR2_BCD (1u << 24)   /* bit-clock direction: 1 = MASTER */
#define CSR_FEF  (1u << 18)   /* FIFO error: over/underrun (W1C) */
#define CSR_FR   (1u << 25)   /* FIFO reset (momentary)          */
#define CSR_FRDE (1u << 0)    /* FIFO Request DMA Enable          */
#define CSR_EN   (1u << 31)   /* TE                               */

/* ---- eDMA3 (PERI_DMA.h; channel block at base + 0x10000 + n*0x10000) ---- */
#define EDMA3_CH(n)  (0x44010000u + (n) * 0x10000u)
#define D32(n, o)    (*(volatile uint32_t *)(EDMA3_CH(n) + (o)))
#define D16(n, o)    (*(volatile uint16_t *)(EDMA3_CH(n) + (o)))
#define CH_CSR(n)     D32(n, 0x000)
#define CH_MUX(n)     D32(n, 0x014)
#define TCD_SADDR(n)  D32(n, 0x020)
#define TCD_SOFF(n)   D16(n, 0x024)
#define TCD_ATTR(n)   D16(n, 0x026)
#define TCD_NBYTES(n) D32(n, 0x028)
#define TCD_DADDR(n)  D32(n, 0x030)
#define TCD_DOFF(n)   D16(n, 0x034)
#define TCD_CITER(n)  D16(n, 0x036)
#define TCD_CSR(n)    D16(n, 0x03C)
#define TCD_BITER(n)  D16(n, 0x03E)

#define CH_CSR_ERQ    (1u << 0)
#define CH_CSR_DONE   (1u << 30)
#define TCD_CSR_DREQ  (1u << 3)

/*
 * CH_MUX[SRC] for SAI1 TX.  PERI_DMA4.h: kDma3RequestMuxSai1Tx = 21|0x100; SRC is
 * an 8-bit field and DMA_CH_MUX_SOURCE()'s & 0xFF strips the 0x100 tag, so 21 is
 * what the hardware sees.
 */
#define SRC_SAI1_TX 21u

/* 32-bit access, both sides: ATTR = (SSIZE<<8)|DSIZE, size code 2 = 32-bit. */
#define ATTR_32BIT  ((2u << 8) | 2u)

/*
 * THE RATE IS AN ARGUMENT, AND THE HARNESS MUST ARM IT.  Same discipline as the
 * PIO test: {MAGIC, DIV} poked by `-device loader`, no default, fail loudly if
 * unarmed — a test that supplies its own input is testing itself.
 */
#define SEL_MAGIC_ADDR (*(volatile uint32_t *)0x20001000u)
#define SEL_DIV_ADDR   (*(volatile uint32_t *)0x20001004u)
#define SEL_MAGIC      0x53414931u   /* "SAI1" */

/* The DMA source buffer, at a fixed DTCM address clear of code, the loader slots
 * (0x2000_1000) and the stack (top 0x2002_0000). */
#define NSAMPLES 2048u
#define SAMP ((volatile uint32_t *)0x20008000u)

/* Deterministic ramp, reproduced in three lines of Python by the oracle; 977 is
 * odd so the ramp sweeps the whole 16-bit range and a stuck sample can't hide. */
static inline int16_t wave(uint32_t n) { return (int16_t)(uint16_t)(n * 977u); }

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }
static void die(const char *s) { puts_(s); sh(SYS_EXIT, (void *)0x20026u); }

/* Arm channel 0 to move NSAMPLES 32-bit words, one PER REQUEST, from SAMP to TDR0. */
static void arm_tx(void)
{
    CH_CSR(0)     = 0;                 /* ERQ off while we program the TCD */
    CH_MUX(0)     = 0;
    CH_MUX(0)     = SRC_SAI1_TX;
    TCD_SADDR(0)  = (uint32_t)(uintptr_t)SAMP;
    TCD_SOFF(0)   = 4;                 /* advance one 32-bit word per minor loop */
    TCD_ATTR(0)   = ATTR_32BIT;
    TCD_NBYTES(0) = 4;                 /* one word (= one FIFO request) per minor loop */
    TCD_DADDR(0)  = TDR0_ADDR;
    TCD_DOFF(0)   = 0;                 /* TDR is a fixed register */
    TCD_CITER(0)  = NSAMPLES;
    TCD_BITER(0)  = NSAMPLES;
    TCD_CSR(0)    = TCD_CSR_DREQ;      /* stop requesting once the major loop ends */
}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    int fail = 0;
    uint32_t div;

    /* A REAL MCLK: root 65, MUX 0 (OSC_RC_24M), DIV field = divisor-1 = 0. */
    CCM_ROOT_CTRL(CCM_ROOT_SAI1) = (0u << 8) | 0u;

    if (SEL_MAGIC_ADDR != SEL_MAGIC) {
        die("SAIDMA: FAIL - the rate selector was never armed by the harness\r\n");
    }
    div = SEL_DIV_ADDR;

    for (uint32_t n = 0; n < NSAMPLES; n++) {
        SAMP[n] = (uint32_t)(uint16_t)wave(n);   /* right-justified 16-bit sample */
    }

    /* SAI1 as bit-clock MASTER (no codec on a bare board => the only coherent
     * config), 16-bit words, watermark 8.  See the PIO test for why BCD matters. */
    TCSR = CSR_FR;                    /* FIFO reset (momentary) */
    TCR2 = TCR2_BCD | (div & 0xFFu);
    TCR3 = (1u << 16);                /* enable TX data channel 0            */
    TCR4 = (1u << 16);                /* FRSZ=1 -> 2 words/frame (stereo)    */
    TCR5 = (15u << 16) | (15u << 24); /* 16-bit words                        */
    TCR1 = 8u;                        /* watermark                           */

    /* ---------------- NEGATIVE: FRDE = 0, so no request may fire ---------- */
    /*
     * TE on, FIFO empty (tx_count=0 <= watermark => the FIFO-request condition is
     * SATISFIED), channel armed and ERQ set.  The ONLY thing withheld is FRDE.  A
     * model that asserted the request from the FIFO alone would run the transfer.
     */
    TCSR = CSR_EN;                    /* enable TX; FRDE deliberately NOT set */
    arm_tx();
    CH_CSR(0) = CH_CSR_ERQ;           /* armed, and (correctly) nothing is asking */

    for (volatile uint32_t sp = 0; sp < 2000000u; sp++) {
        /* give a broken model all the time in the world to move a word */
    }
    if (TCD_SADDR(0) != (uint32_t)(uintptr_t)SAMP) {
        puts_("SAIDMA: FAIL - NEG: eDMA moved words with TCSR[FRDE] CLEAR\r\n");
        fail = 1;
    } else if (TCD_CITER(0) != NSAMPLES) {
        puts_("SAIDMA: FAIL - NEG: CITER advanced with TCSR[FRDE] CLEAR\r\n");
        fail = 1;
    } else if (CH_CSR(0) & CH_CSR_DONE) {
        puts_("SAIDMA: FAIL - NEG: channel reported DONE with TCSR[FRDE] CLEAR\r\n");
        fail = 1;
    }

    /* ---------------- POSITIVE: FRDE = 1, the eDMA feeds the FIFO --------- */
    CH_CSR(0) = 0;                    /* disarm before re-programming */
    arm_tx();                         /* fresh SADDR/CITER */
    /* Re-enable TX, clear the NEG-phase underrun FEF, and OPEN the DMA gate. The
     * CPU does not touch TDR from here on: the words leave memory by DMA. */
    TCSR = CSR_EN | CSR_FRDE | CSR_FEF | CSR_FR;
    CH_CSR(0) = CH_CSR_ERQ;

    uint32_t spins = 0;
    while (!(CH_CSR(0) & CH_CSR_DONE) && spins < 200000000u) {
        spins++;
    }
    if (!(CH_CSR(0) & CH_CSR_DONE)) {
        puts_("SAIDMA: FAIL - POS: TX channel never completed its major loop\r\n");
        fail = 1;
    } else if (CH_CSR(0) & CH_CSR_ERQ) {
        /* TCD_CSR[DREQ] should have cleared ERQ at completion. */
        puts_("SAIDMA: FAIL - POS: TCD_CSR[DREQ] did not clear ERQ at completion\r\n");
        fail = 1;
    }

    /* Wait for the FIFO to actually empty so every DMA'd word reaches the sink,
     * using TFR0's read/write pointers (see the PIO test). */
    for (uint32_t guard = 0; guard < 200000000u; guard++) {
        uint32_t tfr = TFR0;
        if ((tfr & 0x3Fu) == ((tfr >> 16) & 0x3Fu)) {
            break;                    /* rptr == wptr => empty */
        }
    }

    if (!fail) {
        puts_("SAIDMA: PASS - FRDE gate refuses, and open => "
              "2048 samples mem->eDMA->SAI FIFO->sink, and the WAV is the assertion\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
