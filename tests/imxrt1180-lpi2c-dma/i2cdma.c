/*
 * i.MX RT1180 — LPI2C register read driven by eDMA HARDWARE REQUEST.
 *
 * tests/imxrt1180-lpi2c does the register read by CPU.  This does the SAME read
 * with the CPU never touching MTDR or MRDR: LPI2C1's TX request line (gated by
 * MDER[TDDE]) drives one eDMA channel that streams the COMMAND words to MTDR, and
 * its RX request line (gated by MDER[RDDE] + the RX FIFO level) drives a second
 * channel that pulls the received bytes MRDR->mem.
 *
 * Unlike SPI, an I2C transaction is self-framed by START/STOP commands in the MTDR
 * stream -- there is no chip-select to wire -- so the flash-style one-shot problem
 * does not arise and the PIO golden read below is a true self-check: read the
 * TMP105's T_HIGH register (reset default 0x5000, non-zero) by CPU, then assert the
 * DMA-moved bytes are byte-identical.
 *
 * THREE PHASES -- the two request lines have INDEPENDENT gates:
 *   NEG-TX  ERQ set, channels armed, MDER=0 -> the TX line must stay down: not one
 *           command word is written (TCD_SADDR must not advance).
 *   NEG-RX  MDER=TDDE only -> TX streams the command sequence and the receive fills
 *           the RX FIFO, but with RDDE clear the RX channel must move NOTHING (its
 *           destination stays the 0x5A sentinel).
 *   POS     MDER=TDDE|RDDE -> both live; the 2 received bytes reach memory by DMA,
 *           byte-exact vs the PIO golden.  Also the witness that makes NEG-TX
 *           non-vacuous (same setup + MDER => it moves).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* ---- LPI2C1 @ 0x4434_0000 (eDMA3 source, SRC 7/8) ---------------------- */
#define LPI2C1 0x44340000u
#define MCR   (*(volatile uint32_t *)(LPI2C1 + 0x10))
#define MSR   (*(volatile uint32_t *)(LPI2C1 + 0x14))
#define MDER  (*(volatile uint32_t *)(LPI2C1 + 0x1C))
#define MTDR_ADDR         (LPI2C1 + 0x60)
#define MRDR_ADDR         (LPI2C1 + 0x70)
#define MRDR  (*(volatile uint32_t *)MRDR_ADDR)

#define MCR_MEN 0x1u
#define MCR_RRF 0x200u
#define MDER_TDDE 0x1u
#define MDER_RDDE 0x2u
#define MSR_NDF   0x400u
#define MRDR_EMPTY 0x4000u

#define CMD_START 0x400u   /* (0b100 << 8) | (addr<<1|rw) */
#define CMD_TX    0x000u
#define CMD_RX    0x100u   /* (0b001 << 8) | (n-1)        */
#define CMD_STOP  0x200u

#define TMP105_ADDR 0x48u
#define REG_THIGH   0x03u

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

#define SRC_LPI2C1_TX 7u
#define SRC_LPI2C1_RX 8u

#define TX_CH 0u
#define RX_CH 1u
#define NTX 5u                      /* command words in the read sequence */
#define NRX 2u                      /* T_HIGH is 2 bytes                   */
#define SENTINEL 0x5Au

/* The command sequence: START+addr(W), pointer, repeated START+addr(R), recv 2,
 * STOP.  The eDMA streams these words to MTDR one per TX request. */
static uint32_t txcmd[NTX] = {
    CMD_START | ((TMP105_ADDR << 1) | 0u),
    CMD_TX    | REG_THIGH,
    CMD_START | ((TMP105_ADDR << 1) | 1u),
    CMD_RX    | (NRX - 1u),
    CMD_STOP,
};
static uint32_t rx_dma[NRX];
static uint8_t  gold[NRX];

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }
static void die(const char *s) { puts_(s); sh(SYS_EXIT, (void *)0x20026u); }

static void arm(unsigned n, uint32_t sa, int16_t soff, uint32_t da, int16_t doff,
                uint32_t src, uint16_t citer)
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
    TCD_CITER(n)  = citer;
    TCD_BITER(n)  = citer;
    TCD_CSR(n)    = TCD_CSR_DREQ;
}

/* Arm TX (command stream) and RX (received bytes) for one read transaction. */
static void arm_both(void)
{
    arm(TX_CH, (uint32_t)txcmd, 4, MTDR_ADDR, 0, SRC_LPI2C1_TX, NTX);
    arm(RX_CH, MRDR_ADDR, 0, (uint32_t)rx_dma, 4, SRC_LPI2C1_RX, NRX);
    CH_CSR(RX_CH) = CH_CSR_ERQ;
    CH_CSR(TX_CH) = CH_CSR_ERQ;
}

static void fill_sentinel(void)
{
    for (unsigned i = 0; i < NRX; i++) {
        rx_dma[i] = SENTINEL;
    }
}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    int fail = 0;

    MCR = MCR_MEN;

    /* ---------------- PIO: capture the golden T_HIGH bytes --------------- */
    for (unsigned i = 0; i < NTX; i++) {
        *(volatile uint32_t *)MTDR_ADDR = txcmd[i];
    }
    for (unsigned i = 0; i < NRX; i++) {
        uint32_t d = MRDR;
        if (d & MRDR_EMPTY) {
            die("I2CDMA: FAIL - PIO: TMP105 returned no data; test can't arm\r\n");
        }
        gold[i] = d & 0xFF;
    }
    if ((MSR & MSR_NDF) || gold[0] == 0x00) {
        die("I2CDMA: FAIL - PIO: no ACK, or T_HIGH read as zero (vacuous)\r\n");
    }

    /* ---------------- NEG-TX: MDER=0, the TX line must stay down --------- */
    /* Discipline (tests/imxrt1180-dmareq): lines down BEFORE arming ERQ, up LAST. */
    MDER = 0;
    fill_sentinel();
    arm_both();
    /* MDER stays 0: neither request line enabled. */
    for (volatile uint32_t sp = 0; sp < 2000000u; sp++) {
    }
    if (TCD_SADDR(TX_CH) != (uint32_t)txcmd) {
        puts_("I2CDMA: FAIL - NEG-TX: eDMA wrote MTDR with MDER[TDDE] CLEAR\r\n");
        fail = 1;
    } else if (TCD_CITER(TX_CH) != NTX) {
        puts_("I2CDMA: FAIL - NEG-TX: TX CITER advanced with MDER[TDDE] CLEAR\r\n");
        fail = 1;
    }
    CH_CSR(TX_CH) = 0;
    CH_CSR(RX_CH) = 0;

    /* ---------------- NEG-RX: MDER=TDDE only, the RX line must stay down -- */
    MDER = 0;
    fill_sentinel();
    arm_both();
    MDER = MDER_TDDE;             /* TX only: it streams the read, filling the FIFO */
    for (uint32_t sp = 0; sp < 40000000u && !(CH_CSR(TX_CH) & CH_CSR_DONE); sp++) {
    }
    if (!(CH_CSR(TX_CH) & CH_CSR_DONE)) {
        puts_("I2CDMA: FAIL - NEG-RX: TX channel never completed (TX gate stuck?)\r\n");
        fail = 1;
    }
    for (unsigned i = 0; i < NRX; i++) {
        if ((rx_dma[i] & 0xFF) != SENTINEL) {
            puts_("I2CDMA: FAIL - NEG-RX: RX channel moved data with MDER[RDDE] CLEAR\r\n");
            fail = 1;
            break;
        }
    }
    CH_CSR(TX_CH) = 0;
    CH_CSR(RX_CH) = 0;
    MCR = MCR_MEN | MCR_RRF;      /* flush the RX FIFO the spent read left behind */

    /* ---------------- POSITIVE: both lines live, byte-exact by DMA -------- */
    MDER = 0;
    fill_sentinel();
    arm_both();
    MDER = MDER_TDDE | MDER_RDDE; /* raise both request lines LAST -> starts it */

    uint32_t spins = 0;
    while (!(CH_CSR(RX_CH) & CH_CSR_DONE) && spins < 200000000u) {
        spins++;
    }
    if (!(CH_CSR(RX_CH) & CH_CSR_DONE)) {
        puts_("I2CDMA: FAIL - POS: RX channel never completed its major loop\r\n");
        fail = 1;
    } else if (!(CH_CSR(TX_CH) & CH_CSR_DONE)) {
        puts_("I2CDMA: FAIL - POS: TX channel never completed its major loop\r\n");
        fail = 1;
    } else if (CH_CSR(TX_CH) & CH_CSR_ERQ) {
        puts_("I2CDMA: FAIL - POS: TCD_CSR[DREQ] did not clear TX ERQ at completion\r\n");
        fail = 1;
    } else {
        for (unsigned i = 0; i < NRX; i++) {
            if ((rx_dma[i] & 0xFF) != gold[i]) {
                puts_("I2CDMA: FAIL - POS: DMA-read bytes differ from the PIO read\r\n");
                fail = 1;
                break;
            }
        }
    }
    CH_CSR(TX_CH) = 0;
    CH_CSR(RX_CH) = 0;

    if (!fail) {
        puts_("I2CDMA: PASS - TDDE gate refuses, RDDE gate refuses, both open => "
              "TMP105 register read by eDMA, byte-exact vs PIO, CPU never touched MTDR/MRDR\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
