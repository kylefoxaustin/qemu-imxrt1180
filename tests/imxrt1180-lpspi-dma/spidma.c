/*
 * i.MX RT1180 — LPSPI full-duplex JEDEC read driven by eDMA HARDWARE REQUEST.
 *
 * tests/imxrt1180-lpspi does the JEDEC-ID read by CPU.  This test does the SAME
 * read with the CPU never touching TDR or RDR: LPSPI1's TX request line (gated by
 * DER[TDDE]) drives one eDMA channel mem->TDR, and its RX request line (gated by
 * DER[RDDE] + the RX FIFO level) drives a second channel RDR->mem.  The bytes the
 * flash returned reach memory entirely by DMA.
 *
 * ⭐ CHIP-SELECT IS NOT WIRED FROM LPSPI TO THE SLAVE IN THIS MODEL (the SoC wires
 *    the FlexSPI's CS but not the LPSPI cs_lines).  So the attached m25p80 never
 *    sees a CS deassert, cannot reframe, and answers RDID exactly ONCE per boot --
 *    subsequent reads return zeros (continued-stream data, not a new command).
 *    The existing PIO test hides this by doing a single read.  This test is
 *    STRUCTURED around it: the positive phase is the flash's FIRST command, and the
 *    negatives never depend on the flash resetting.  (Wiring LPSPI CS to -device
 *    slaves is a separate gap, tracked in PERIPHERALS.md.)
 *
 * THREE PHASES, because the two request lines have INDEPENDENT gates and a test
 * that cannot fail on either is decoration:
 *   NEG-TX  ERQ set, channels armed, DER=0  -> the TX line must stay down: not one
 *           frame is clocked (TCD_SADDR must not advance).  Runs FIRST so the flash
 *           is pristine; MEN is on, so a model keying TX on MEN alone would run.
 *   POS     DER=TDDE|RDDE -> the flash's first command; byte-exact vs the at25df321a
 *           JEDEC ID (SOURCED, m25p80.c: 0x1f4701).  Proves full-duplex DMA and is
 *           the witness that makes NEG-TX non-vacuous (same setup + DER => it moves).
 *   NEG-RX  DER=TDDE only -> TX clocks (the flash is spent now, but its data is
 *           irrelevant here); with RDDE clear the RX channel must move NOTHING (its
 *           destination stays the 0x5A sentinel).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* ---- LPSPI1 @ 0x4436_0000 (eDMA3 source, SRC 11/12) -------------------- */
#define LPSPI1 0x44360000u
#define CR    (*(volatile uint32_t *)(LPSPI1 + 0x10))
#define DER   (*(volatile uint32_t *)(LPSPI1 + 0x1C))
#define CFGR1 (*(volatile uint32_t *)(LPSPI1 + 0x24))
#define TCR   (*(volatile uint32_t *)(LPSPI1 + 0x60))
#define TDR_ADDR          (LPSPI1 + 0x64)
#define RDR_ADDR          (LPSPI1 + 0x74)

#define CR_MEN 0x1u
#define CR_RST 0x2u
#define DER_TDDE 0x1u
#define DER_RDDE 0x2u
#define TCR_FRAMESZ8 0x7u          /* FRAMESZ = 8 bits - 1 */
#define TCR_CONT     0x200000u
#define CMD_READ_ID  0x9Fu

/* ---- eDMA3 (channel block at base + 0x10000 + n*0x10000) --------------- */
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
#define ATTR_32BIT    ((2u << 8) | 2u)

#define SRC_LPSPI1_TX 11u
#define SRC_LPSPI1_RX 12u

#define TX_CH 0u
#define RX_CH 1u
#define NFRAME 4u                   /* 0x9F + 3 bytes clocked to read the ID */
#define SENTINEL 0x5Au

/* at25df321a JEDEC ID = 0x1f4701 (hw/block/m25p80.c); byte 0 (during the 0x9F
 * command) is 0.  SOURCED anchor -- the SLAVE's identity, not the LPSPI. */
static const uint8_t GOLD[NFRAME] = { 0x00, 0x1f, 0x47, 0x01 };

static uint32_t txbuf[NFRAME] = { CMD_READ_ID, 0, 0, 0 };
static uint32_t rx_dma[NFRAME];

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

/* Arm channel `n` to move NFRAME 32-bit words, one per request, sa<->da. */
static void arm(unsigned n, uint32_t sa, int16_t soff,
                uint32_t da, int16_t doff, uint32_t src)
{
    CH_CSR(n)     = 0;
    CH_MUX(n)     = 0;
    CH_MUX(n)     = src;
    TCD_SADDR(n)  = sa;
    TCD_SOFF(n)   = (uint16_t)soff;
    TCD_ATTR(n)   = ATTR_32BIT;
    TCD_NBYTES(n) = 4;
    TCD_DADDR(n)  = da;
    TCD_DOFF(n)   = (uint16_t)doff;
    TCD_CITER(n)  = NFRAME;
    TCD_BITER(n)  = NFRAME;
    TCD_CSR(n)    = TCD_CSR_DREQ;
}

static void fill_sentinel(void)
{
    for (unsigned i = 0; i < NFRAME; i++) {
        rx_dma[i] = SENTINEL;
    }
}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    int fail = 0;

    CR = CR_RST; CR = 0;
    CFGR1 = 0x1;                   /* MASTER mode */
    CR = CR_MEN;
    TCR = TCR_FRAMESZ8 | TCR_CONT; /* 8-bit frames, CS held across the transaction */

    /* ---------------- NEG-TX: DER=0, the TX line must stay down ---------- */
    /* Discipline (see tests/imxrt1180-dmareq): drop every request line BEFORE
     * arming ERQ, then raise the phase's lines LAST -- otherwise a line left high
     * from the previous phase clocks the transfer the instant ERQ is set. */
    DER = 0;
    fill_sentinel();
    arm(TX_CH, (uint32_t)txbuf, 4, TDR_ADDR, 0, SRC_LPSPI1_TX);
    arm(RX_CH, RDR_ADDR, 0, (uint32_t)rx_dma, 4, SRC_LPSPI1_RX);
    CH_CSR(RX_CH) = CH_CSR_ERQ;
    CH_CSR(TX_CH) = CH_CSR_ERQ;
    /* DER stays 0: neither request line enabled. */
    for (volatile uint32_t sp = 0; sp < 2000000u; sp++) {
    }
    if (TCD_SADDR(TX_CH) != (uint32_t)txbuf) {
        puts_("SPIDMA: FAIL - NEG-TX: eDMA clocked frames with DER[TDDE] CLEAR\r\n");
        fail = 1;
    } else if (TCD_CITER(TX_CH) != NFRAME) {
        puts_("SPIDMA: FAIL - NEG-TX: TX CITER advanced with DER[TDDE] CLEAR\r\n");
        fail = 1;
    }
    CH_CSR(TX_CH) = 0;
    CH_CSR(RX_CH) = 0;

    /* ---------------- POSITIVE: both lines live, byte-exact by DMA -------- */
    /* The flash is pristine (NEG-TX clocked nothing): this is its first command. */
    DER = 0;                       /* lines down before arming */
    fill_sentinel();
    arm(TX_CH, (uint32_t)txbuf, 4, TDR_ADDR, 0, SRC_LPSPI1_TX);
    arm(RX_CH, RDR_ADDR, 0, (uint32_t)rx_dma, 4, SRC_LPSPI1_RX);
    CH_CSR(RX_CH) = CH_CSR_ERQ;
    CH_CSR(TX_CH) = CH_CSR_ERQ;
    DER = DER_TDDE | DER_RDDE;     /* raise both request lines LAST -> starts it */

    uint32_t spins = 0;
    while (!(CH_CSR(RX_CH) & CH_CSR_DONE) && spins < 200000000u) {
        spins++;
    }
    if (!(CH_CSR(RX_CH) & CH_CSR_DONE)) {
        puts_("SPIDMA: FAIL - POS: RX channel never completed its major loop\r\n");
        fail = 1;
    } else if (!(CH_CSR(TX_CH) & CH_CSR_DONE)) {
        puts_("SPIDMA: FAIL - POS: TX channel never completed its major loop\r\n");
        fail = 1;
    } else if (CH_CSR(TX_CH) & CH_CSR_ERQ) {
        puts_("SPIDMA: FAIL - POS: TCD_CSR[DREQ] did not clear TX ERQ at completion\r\n");
        fail = 1;
    } else {
        for (unsigned i = 0; i < NFRAME; i++) {
            if ((rx_dma[i] & 0xFF) != GOLD[i]) {
                puts_("SPIDMA: FAIL - POS: DMA-read JEDEC bytes are not the at25df321a ID\r\n");
                fail = 1;
                break;
            }
        }
    }
    CH_CSR(TX_CH) = 0;
    CH_CSR(RX_CH) = 0;

    /* ---------------- NEG-RX: DER=TDDE only, the RX line must stay down --- */
    /* TX still clocks (proving the TX gate); the flash is spent so its bytes are
     * zeros, but that is irrelevant -- we assert the RX channel moved NOTHING. */
    DER = 0;                       /* drop POS's RDDE before arming, or it drains */
    fill_sentinel();
    arm(TX_CH, (uint32_t)txbuf, 4, TDR_ADDR, 0, SRC_LPSPI1_TX);
    arm(RX_CH, RDR_ADDR, 0, (uint32_t)rx_dma, 4, SRC_LPSPI1_RX);
    CH_CSR(RX_CH) = CH_CSR_ERQ;
    CH_CSR(TX_CH) = CH_CSR_ERQ;
    DER = DER_TDDE;                /* TX only, raised LAST */
    for (uint32_t sp = 0; sp < 40000000u && !(CH_CSR(TX_CH) & CH_CSR_DONE); sp++) {
    }
    if (!(CH_CSR(TX_CH) & CH_CSR_DONE)) {
        puts_("SPIDMA: FAIL - NEG-RX: TX channel never completed (TX gate stuck?)\r\n");
        fail = 1;
    }
    for (unsigned i = 0; i < NFRAME; i++) {
        if ((rx_dma[i] & 0xFF) != SENTINEL) {
            puts_("SPIDMA: FAIL - NEG-RX: RX channel moved data with DER[RDDE] CLEAR\r\n");
            fail = 1;
            break;
        }
    }
    CH_CSR(TX_CH) = 0;
    CH_CSR(RX_CH) = 0;

    if (!fail) {
        puts_("SPIDMA: PASS - TDDE gate refuses, RDDE gate refuses, both open => "
              "JEDEC ID read full-duplex by eDMA, byte-exact, CPU never touched TDR/RDR\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
