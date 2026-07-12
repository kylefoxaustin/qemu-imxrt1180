/*
 * i.MX RT1180 — PERIPHERAL-TRIGGERED eDMA (Cortex-M33).
 *
 * The eDMA has TWO ways to move a byte and they are not the same thing:
 *
 *   TCD_CSR[START]  — software says "go", the whole major loop runs. This is the
 *                     one every mem-to-mem test exercises, and it needs no
 *                     peripheral at all.
 *   a REQUEST LINE  — a peripheral says "I have a byte" / "I can take a byte",
 *                     and ONE minor loop runs. This is how every real DMA-driven
 *                     driver in the SDK works, and it is the one that is easy to
 *                     have never implemented while all your tests pass.
 *
 * This test drives the second path END TO END over a real wire: LPUART2 (the b2b
 * port, 0x4439_0000, bound to a socket chardev with an echo peer). eDMA3 CH0 is
 * armed on the LPUART2 **TX** request (CH_MUX[SRC]=18) and CH1 on the LPUART2
 * **RX** request (SRC=19). The CPU never touches DATA in the positive phase: the
 * bytes leave memory by DMA, cross the socket, are echoed, and land back in
 * memory by DMA. Then they are compared byte-exact.
 *
 * THE TWO NEGATIVE PHASES ARE THE POINT. A positive-only test passes on a model
 * that ignores the gates entirely — it would just run the transfer the moment the
 * TCD is written. So, before the positive phase, we prove each gate INDEPENDENTLY
 * REFUSES, with real bytes arriving on the wire the whole time:
 *
 *   NEG-A  request line asserted (BAUD[RDMAE]=1), TCD armed, **ERQ = 0**
 *          -> not one byte may move.
 *   NEG-B  ERQ = 1, TCD armed, **request line down** (BAUD[RDMAE]=0)
 *          -> not one byte may move.
 *
 * And the destination buffers are PRE-FILLED WITH A NON-ZERO SENTINEL (0x5A), not
 * left zeroed. 95emulator's finding: a negative test whose source is zeros passes
 * VACUOUSLY on a model that wrongly runs the transfer — it copies zeros over zeros
 * and the check cannot tell. Source is 0x10..0x2F, sentinel is 0x5A, and neither
 * collides with the other or with the GO byte.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* ---- LPUART2 (PERI_LPUART.h offsets; b2b port) ------------------------- */
#define LPUART2_BASE 0x44390000u
#define LPUART_BAUD  (*(volatile uint32_t *)(LPUART2_BASE + 0x10))
#define LPUART_STAT  (*(volatile uint32_t *)(LPUART2_BASE + 0x14))
#define LPUART_CTRL  (*(volatile uint32_t *)(LPUART2_BASE + 0x18))
#define LPUART_DATA_ADDR              (LPUART2_BASE + 0x1C)
#define LPUART_DATA  (*(volatile uint32_t *)LPUART_DATA_ADDR)
#define CTRL_TE      0x00080000u
#define CTRL_RE      0x00040000u
#define STAT_RDRF    0x00200000u
#define STAT_TDRE    0x00800000u
/* BAUD DMA enables — PERI_LPUART.h: RDMAE=21, TDMAE=23. */
#define BAUD_RDMAE   0x00200000u
#define BAUD_TDMAE   0x00800000u

/* ---- eDMA3 (PERI_DMA.h) ------------------------------------------------- */
/* CH[n] at base + 0x10000 + n * 0x10000 -- "array offset: 0x10000, array step:
 * 0x10000". (This said + n*0x1000 until 2026-07-12: the MCXN947 geometry, which
 * the model shared, so the test could not see it. The stock NXP driver could.) */
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
 * CH_MUX[SRC] values. PERI_DMA4.h spells these kDma3RequestMuxLPUART2Tx = 18|0x100
 * and ...Rx = 19|0x100; SRC is an 8-bit field and DMA_CH_MUX_SOURCE()'s & 0xFF
 * strips the 0x100 instance tag, so 18 and 19 are what the hardware sees.
 */
#define SRC_LPUART2_TX 18u
#define SRC_LPUART2_RX 19u

#define GO_BYTE   0xA5u
#define SENTINEL  0x5Au
#define N_BYTES   32

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

static volatile uint8_t pattern[N_BYTES];
static volatile uint8_t dst_a[N_BYTES];   /* NEG-A: must stay all SENTINEL */
static volatile uint8_t dst_b[N_BYTES];   /* NEG-B: must stay all SENTINEL */
static volatile uint8_t dst_c[N_BYTES];   /* POS:   must become `pattern`  */

static void tx_cpu(uint8_t b)
{
    while (!(LPUART_STAT & STAT_TDRE)) {
    }
    LPUART_DATA = b;
}

static int rx_cpu(uint32_t spins)
{
    while (spins--) {
        if (LPUART_STAT & STAT_RDRF) {
            return (int)(LPUART_DATA & 0xFF);
        }
    }
    return -1;
}

/* Arm channel `n` to move N_BYTES one at a time between `sa` and `da`. */
static void arm(unsigned n, uint32_t sa, int16_t soff,
                uint32_t da, int16_t doff, uint32_t src)
{
    CH_CSR(n)     = 0;                 /* ERQ off while we program the TCD */
    CH_MUX(n)     = 0;                 /* the SDK resets the mux first too */
    CH_MUX(n)     = src;
    TCD_SADDR(n)  = sa;
    TCD_SOFF(n)   = (uint16_t)soff;
    TCD_ATTR(n)   = 0;                 /* SSIZE = DSIZE = 8-bit */
    TCD_NBYTES(n) = 1;                 /* one byte per REQUEST */
    TCD_DADDR(n)  = da;
    TCD_DOFF(n)   = (uint16_t)doff;
    TCD_CITER(n)  = N_BYTES;
    TCD_BITER(n)  = N_BYTES;
    TCD_CSR(n)    = TCD_CSR_DREQ;      /* stop requesting once the major loop ends */
}

static void fill(volatile uint8_t *b, uint8_t v)
{
    for (int i = 0; i < N_BYTES; i++) {
        b[i] = v;
    }
}
static int all_are(volatile uint8_t *b, uint8_t v)
{
    for (int i = 0; i < N_BYTES; i++) {
        if (b[i] != v) {
            return 0;
        }
    }
    return 1;
}

/*
 * Make the peer echo N_BYTES back at us, and WAIT UNTIL AT LEAST ONE HAS LANDED.
 * That wait is what makes a negative phase mean anything: without it, "nothing was
 * copied" could simply mean "nothing had arrived yet", and the phase would pass on
 * a model with no gates at all.  Returns 1 if a byte really reached the LPUART.
 */
static int provoke_rx(void)
{
    for (int i = 0; i < N_BYTES; i++) {
        tx_cpu(pattern[i]);
    }
    for (uint32_t spins = 0; spins < 20000000u; spins++) {
        if (LPUART_STAT & STAT_RDRF) {
            return 1;               /* a byte is sitting in the holding register */
        }
    }
    return 0;
}

static void drain_cpu(void)
{
    while (rx_cpu(400000) >= 0) {
    }
}

void reset_handler(void);

__attribute__((section(".vectors"), used))
void (* const vt[])(void) = {
    [0] = (void (*)(void))STACK_TOP,
    [1] = reset_handler,
};

void reset_handler(void)
{
    int fail = 0;

    for (int i = 0; i < N_BYTES; i++) {
        pattern[i] = (uint8_t)(0x10 + i);      /* 0x10..0x2F: never 0x5A, never 0xA5 */
    }
    LPUART_CTRL = CTRL_TE | CTRL_RE;
    LPUART_BAUD = 0;                           /* both DMA request lines DOWN */

    /* Link up: resend GO until it echoes (boot-order-immune), then drain stale
     * echoes -- every GO we sent gets echoed, and a leftover one would corrupt
     * the first pattern byte. (The uartlink test learned this under load.) */
    int connected = 0;
    for (int a = 0; a < 100000 && !connected; a++) {
        tx_cpu(GO_BYTE);
        if (rx_cpu(20000) == (int)GO_BYTE) {
            connected = 1;
        }
    }
    if (!connected) {
        puts_("DMAREQ: FAIL - peer never echoed GO (link down)\r\n");
        sh(SYS_EXIT, (void *)0x20026u);
    }
    drain_cpu();

    /* ---------------- NEG-A: request asserted, but ERQ = 0 ---------------- */
    fill(dst_a, SENTINEL);
    arm(1, LPUART_DATA_ADDR, 0, (uint32_t)dst_a, 1, SRC_LPUART2_RX);
    LPUART_BAUD = BAUD_RDMAE;                  /* the RX request line is LIVE */
    /* CH_CSR(1) stays 0 -- ERQ deliberately NOT set. */
    int arrived = provoke_rx();
    /*
     * ASSERT THE BUFFER *BEFORE* THE VACUITY GUARD -- the order is not cosmetic.
     *
     * A model that ignores ERQ does not merely copy the bytes; it copies them
     * FASTER THAN THE CPU CAN SEE THEM. The DMA drains the holding register from
     * the bottom half, so RDRF never latches for the polling loop, so `arrived`
     * comes back 0 -- and a vacuity-guard-first ordering reports "no byte ever
     * arrived", which is the exact opposite of what happened. Right verdict,
     * wrong cause, and an hour of chasing the wire.
     *
     * A clobbered sentinel PROVES bytes arrived (they are sitting in the buffer),
     * so it needs no vacuity guard. The guard only has a job when the buffer is
     * intact, where "nothing moved" really could mean "nothing showed up".
     */
    if (!all_are(dst_a, SENTINEL)) {
        puts_("DMAREQ: FAIL - NEG-A: eDMA moved bytes with CH_CSR[ERQ] CLEAR\r\n");
        fail = 1;
    } else if (!arrived) {
        puts_("DMAREQ: FAIL - NEG-A: no byte ever arrived; the phase proves nothing\r\n");
        fail = 1;
    } else if (CH_CSR(1) & CH_CSR_DONE) {
        puts_("DMAREQ: FAIL - NEG-A: channel reported DONE without ERQ\r\n");
        fail = 1;
    }
    drain_cpu();

    /* ---------------- NEG-B: ERQ = 1, but the request line is DOWN -------- */
    fill(dst_b, SENTINEL);
    LPUART_BAUD = 0;                           /* RDMAE off => RX request line DOWN */
    arm(1, LPUART_DATA_ADDR, 0, (uint32_t)dst_b, 1, SRC_LPUART2_RX);
    CH_CSR(1) = CH_CSR_ERQ;                    /* armed, and nothing is asking */
    arrived = provoke_rx();
    if (!all_are(dst_b, SENTINEL)) {         /* buffer first -- see NEG-A above */
        puts_("DMAREQ: FAIL - NEG-B: eDMA moved bytes with NO peripheral request\r\n");
        fail = 1;
    } else if (!arrived) {
        puts_("DMAREQ: FAIL - NEG-B: no byte ever arrived; the phase proves nothing\r\n");
        fail = 1;
    }
    CH_CSR(1) = 0;
    drain_cpu();

    /* ---------------- POSITIVE: both gates open, both directions by DMA --- */
    fill(dst_c, SENTINEL);
    arm(1, LPUART_DATA_ADDR, 0, (uint32_t)dst_c, 1, SRC_LPUART2_RX);  /* RX: wire -> mem */
    arm(0, (uint32_t)pattern, 1, LPUART_DATA_ADDR, 0, SRC_LPUART2_TX); /* TX: mem -> wire */
    CH_CSR(1) = CH_CSR_ERQ;
    CH_CSR(0) = CH_CSR_ERQ;
    /* Raise BOTH request lines LAST: the TX line asserts the instant TDMAE is set
     * (this LPUART transmits synchronously, so TDRE is always true), and that is
     * what starts the whole thing. The CPU does not touch DATA from here on. */
    LPUART_BAUD = BAUD_TDMAE | BAUD_RDMAE;

    uint32_t spins = 0;
    while (!(CH_CSR(1) & CH_CSR_DONE) && spins < 40000000u) {
        spins++;
    }
    if (!(CH_CSR(1) & CH_CSR_DONE)) {
        puts_("DMAREQ: FAIL - POS: RX channel never completed its major loop\r\n");
        fail = 1;
    } else if (!(CH_CSR(0) & CH_CSR_DONE)) {
        puts_("DMAREQ: FAIL - POS: TX channel never completed its major loop\r\n");
        fail = 1;
    } else {
        for (int i = 0; i < N_BYTES; i++) {
            if (dst_c[i] != pattern[i]) {
                puts_("DMAREQ: FAIL - POS: echoed bytes are not byte-exact\r\n");
                fail = 1;
                break;
            }
        }
    }

    /*
     * TCD_CSR[DREQ] must have cleared ERQ at major completion -- otherwise a
     * still-asserted TX line (TDMAE is still set) would re-run the channel from
     * the reloaded CITER forever, spraying the buffer onto the wire again.
     */
    if (!fail && (CH_CSR(0) & CH_CSR_ERQ)) {
        puts_("DMAREQ: FAIL - POS: TCD_CSR[DREQ] did not clear ERQ at completion\r\n");
        fail = 1;
    }

    if (!fail) {
        puts_("DMAREQ: PASS - ERQ gate refuses, request-line gate refuses, "
              "and both open => 32 bytes mem->LPUART2->wire->LPUART2->mem, byte-exact\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
