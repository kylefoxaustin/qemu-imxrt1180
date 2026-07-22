/*
 * NETC switch wire->CPU forwarding test (Cortex-M33), over a QEMU mcast socket.
 *
 * A QEMU `-nic socket,mcast=...` hands a node back its OWN transmitted frames, so
 * one instance can exercise the wire ingress path with no peer.  We prove that the
 * switch only delivers a received frame to the CPU when its destination resolves
 * to the management port:
 *
 *   - program the FDB so MAC Z is reachable via the WIRE port (not the CPU);
 *   - TX a "drop" frame to Z (seq D1), then a "sentinel" broadcast (seq 5E);
 *   - both are echoed back in order.  On wire ingress the switch drops the Z frame
 *     (its only egress port is the wire = the ingress port -> split-horizon), and
 *     floods the broadcast (which includes the CPU).
 *
 * SENTINEL BARRIER (a strong oracle, not a timeout): because the echo preserves
 * order, if the drop frame had been delivered it would sit in the RX ring BEFORE
 * the sentinel.  So we wait for the sentinel (a bounded, positive event) and then
 * assert the drop frame never arrived.  PASS = saw sentinel AND never saw drop.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* SW0 command BD ring 0 (NTMP FDB programming). */
#define SW0_BASE 0x60A00000u
#define CBDRMR   (*(volatile uint32_t *)(SW0_BASE + 0x800))
#define CBDRBAR0 (*(volatile uint32_t *)(SW0_BASE + 0x810))
#define CBDRBAR1 (*(volatile uint32_t *)(SW0_BASE + 0x814))
#define CBDRPIR  (*(volatile uint32_t *)(SW0_BASE + 0x818))
#define CBDRCIR  (*(volatile uint32_t *)(SW0_BASE + 0x81C))
#define CBDRLENR (*(volatile uint32_t *)(SW0_BASE + 0x820))
#define CBDRMR_EN 0x80000000u

/* ENETC0 SI0 TX + RX rings. */
#define SI 0x60B00000u
#define TB_BAR0 (*(volatile uint32_t *)(SI + 0x8010))
#define TB_BAR1 (*(volatile uint32_t *)(SI + 0x8014))
#define TB_PIR  (*(volatile uint32_t *)(SI + 0x8018))
#define TB_CIR  (*(volatile uint32_t *)(SI + 0x801C))
#define TB_LENR (*(volatile uint32_t *)(SI + 0x8020))
#define RB_MR   (*(volatile uint32_t *)(SI + 0x8100))
#define RB_BSR  (*(volatile uint32_t *)(SI + 0x8108))
#define RB_CIR  (*(volatile uint32_t *)(SI + 0x810C))
#define RB_BAR0 (*(volatile uint32_t *)(SI + 0x8110))
#define RB_BAR1 (*(volatile uint32_t *)(SI + 0x8114))
#define RB_PIR  (*(volatile uint32_t *)(SI + 0x8118))
#define RB_LENR (*(volatile uint32_t *)(SI + 0x8120))
#define RB_MR_EN 0x80000000u
#define MSIX0_CTRL (*(volatile uint32_t *)(0x60BF000Cu))

/* DMA-visible OCRAM. */
#define RING_BASE    0x20484000u        /* NTMP command BD ring     */
#define DBUF_BASE    0x20484100u        /* NTMP data buffer         */
#define TXRING_BASE  0x20484200u        /* TX BD ring               */
#define TXFRAME_BASE 0x20484300u        /* TX frame                 */
#define RXRING_BASE  0x20484400u        /* RX BD ring (8 x 16)      */
#define RXBUF_BASE   0x20484600u        /* RX buffers (8 x 128)     */
#define RXBUF(i)     (RXBUF_BASE + (i) * 128u)
#define RING_LEN 8u

#define TB_FDB       15u
#define CMD_ADDQUERY 0xCu
#define ACC_EXACTKEY 1u
#define SEQ_DROP     0xD1u
#define SEQ_SENT     0x5Eu

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = {
    [0] = (void (*)(void))STACK_TOP,
    [1] = reset_handler,
};

static int fdb_add(unsigned idx, const uint8_t mac[6], uint32_t port_bitmap)
{
    volatile uint8_t *b = (volatile uint8_t *)DBUF_BASE;
    for (int i = 0; i < 48; i++) { b[i] = 0; }
    for (int i = 0; i < 6; i++)  { b[4 + i] = mac[i]; }
    *(volatile uint32_t *)(DBUF_BASE + 12) = 0;
    *(volatile uint32_t *)(DBUF_BASE + 36) = port_bitmap;
    volatile uint32_t *bd = (volatile uint32_t *)(RING_BASE + idx * 32u);
    bd[0] = DBUF_BASE; bd[1] = 0;
    bd[2] = (36u & 0xFFFFFu) | (48u << 20);
    bd[3] = CMD_ADDQUERY | (ACC_EXACTKEY << 12) | (TB_FDB << 16);
    bd[4] = bd[5] = bd[6] = bd[7] = 0;
    uint32_t pir = (idx + 1) % RING_LEN;
    CBDRPIR = pir;
    uint32_t g = 0;
    while ((CBDRCIR & 0x3FFu) != pir) { if (++g > 2000000u) { return 0; } }
    return ((bd[3] >> 16) & 0xFFFu) == 0;
}

static void tx_frame(unsigned idx, const uint8_t dest[6], uint32_t seq)
{
    volatile uint8_t *fr = (volatile uint8_t *)TXFRAME_BASE;
    for (int i = 0; i < 6; i++) { fr[i] = dest[i]; }
    for (int i = 0; i < 6; i++) { fr[6 + i] = 0x02; }
    fr[12] = 0x88; fr[13] = 0xB6;                  /* our ethertype */
    fr[14] = seq; fr[15] = 0; fr[16] = 0; fr[17] = 0;
    for (int i = 18; i < 64; i++) { fr[i] = 0x5A; }
    volatile uint32_t *bd = (volatile uint32_t *)(TXRING_BASE + idx * 16u);
    bd[0] = TXFRAME_BASE; bd[1] = 0;
    *(volatile uint16_t *)(TXRING_BASE + idx * 16u + 10) = 64;
    TB_PIR = idx + 1;
    /* Small settle so the two TX frames leave in order before we poll. */
    for (volatile int d = 0; d < 20000; d++) { }
}

void reset_handler(void)
{
    static const uint8_t Z[6]  = { 0x02, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC };
    static const uint8_t BC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    int saw_drop = 0, saw_sent = 0;

    MSIX0_CTRL = 1;

    /* RX ring: post 8 buffers, arm. */
    for (unsigned i = 0; i < RING_LEN; i++) {
        volatile uint32_t *bd = (volatile uint32_t *)(RXRING_BASE + i * 16u);
        bd[0] = RXBUF(i); bd[1] = 0; bd[2] = 0; bd[3] = 0;
    }
    RB_BAR0 = RXRING_BASE; RB_BAR1 = 0; RB_BSR = 128; RB_CIR = 0; RB_PIR = 0;
    RB_LENR = 8;
    RB_MR = RB_MR_EN;                              /* arm (flushes queued RX) */

    /* TX ring + NTMP ring up. */
    TB_BAR0 = TXRING_BASE; TB_BAR1 = 0; TB_CIR = 0; TB_LENR = 8;
    CBDRBAR0 = RING_BASE; CBDRBAR1 = 0; CBDRLENR = RING_LEN; CBDRMR = CBDRMR_EN;

    /* Z is reachable via the WIRE port only. */
    if (!fdb_add(0, Z, 0x1u)) {
        puts_("NETC-RXFWD: FAIL - could not program the FDB\r\n");
        sh(SYS_EXIT, (void *)0x20026u);
    }

    /* Send the drop frame (to Z), then the sentinel (broadcast). */
    tx_frame(0, Z, SEQ_DROP);
    tx_frame(1, BC, SEQ_SENT);

    /* Drain the RX ring, noting every seq we see with our ethertype.  If the switch
     * wrongly delivered the drop frame it would appear (before the sentinel, by echo
     * order).  BOUNDED wait: a fixed number of settle+poll rounds -- enough for the
     * echo to return, and short enough that "sentinel never arrived" reports a clean
     * FAIL well inside the harness timeout (not a hang). */
    uint32_t consumed = 0;
    for (int round = 0; round < 400 && !saw_sent; round++) {
        for (volatile int d = 0; d < 100000; d++) { }   /* let the echo come back */
        uint32_t pir = RB_PIR & 0xFFFFu;
        while (consumed != pir) {
            volatile uint8_t *fr = (volatile uint8_t *)RXBUF(consumed);
            if (fr[12] == 0x88 && fr[13] == 0xB6) { /* our ethertype only */
                if (fr[14] == SEQ_DROP) { saw_drop = 1; }
                if (fr[14] == SEQ_SENT) { saw_sent = 1; }
            }
            /* free the descriptor: re-post its buffer + advance the consumer index */
            *(volatile uint32_t *)(RXRING_BASE + consumed * 16u) = RXBUF(consumed);
            consumed = (consumed + 1) % RING_LEN;
            RB_CIR = consumed;
        }
    }

    if (saw_sent && !saw_drop) {
        puts_("NETC-RXFWD: PASS - broadcast was delivered to the CPU (flood)\r\n");
        puts_("NETC-RXFWD: PASS - unicast to a wire-only MAC was NOT delivered to the CPU\r\n");
    } else if (!saw_sent) {
        puts_("NETC-RXFWD: FAIL - sentinel never arrived (RX path/echo broken)\r\n");
    } else {
        puts_("NETC-RXFWD: FAIL - a frame switched away from the CPU was still delivered\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
