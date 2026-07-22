/*
 * NETC switch egress-forwarding test (Cortex-M33).
 *
 * Proves the switch actually FORWARDS a frame according to its forwarding
 * database, not just stores the FDB.  A CPU-injected frame ingresses on the
 * management port; the switch resolves its destination and egresses it out the
 * matching ports.  We have one physical port (the wire), so a frame reaches the
 * wire iff the destination floods (unknown unicast / broadcast) or its FDB entry
 * names the wire port.  A destination that maps only to the CPU's own port is
 * dropped by split-horizon.  The observable is the wire port MAC's transmit-frame
 * counter PM0_TFRMN (aFramesTransmittedOK) -- a real statistics register:
 *
 *   dest X, FDB entry -> WIRE port      : injected -> counter += 1   (forwarded)
 *   dest Y, FDB entry -> CPU port only  : injected -> counter unchanged (dropped)
 *   broadcast                           : injected -> counter += 1   (flooded)
 *
 * A "the FDB stores an entry" check (the netc-fdb test) does NOT prove the entry
 * is CONSULTED to move a frame; this does.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* SW0 command BD ring 0 (NTMP), for programming the FDB. */
#define SW0_BASE   0x60A00000u
#define CBDRMR   (*(volatile uint32_t *)(SW0_BASE + 0x800))
#define CBDRBAR0 (*(volatile uint32_t *)(SW0_BASE + 0x810))
#define CBDRBAR1 (*(volatile uint32_t *)(SW0_BASE + 0x814))
#define CBDRPIR  (*(volatile uint32_t *)(SW0_BASE + 0x818))
#define CBDRCIR  (*(volatile uint32_t *)(SW0_BASE + 0x81C))
#define CBDRLENR (*(volatile uint32_t *)(SW0_BASE + 0x820))
#define CBDRMR_EN 0x80000000u

/* Wire port (switch port 0) MAC transmit-frame counter PM0_TFRMN. */
#define PM0_TFRMN (*(volatile uint32_t *)(0x60A05220u))

/* ENETC0 SI0 TX ring, to inject frames through the switch. */
#define TB_BAR0 (*(volatile uint32_t *)(0x60B08010u))
#define TB_BAR1 (*(volatile uint32_t *)(0x60B08014u))
#define TB_PIR  (*(volatile uint32_t *)(0x60B08018u))
#define TB_CIR  (*(volatile uint32_t *)(0x60B0801Cu))
#define TB_LENR (*(volatile uint32_t *)(0x60B08020u))
#define MSIX0_CTRL (*(volatile uint32_t *)(0x60BF000Cu))

/* DMA-visible OCRAM buffers. */
#define RING_BASE    0x20484000u        /* NTMP command BD ring (8 x 32) */
#define DBUF_BASE    0x20484100u        /* NTMP data buffer              */
#define TXRING_BASE  0x20484200u        /* SI TX BD ring (8 x 16)        */
#define TXFRAME_BASE 0x20484300u        /* injected frame                */
#define RING_LEN 8u

#define TB_FDB       15u
#define CMD_ADDQUERY 0xCu
#define ACC_EXACTKEY 1u

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

/* Add an FDB entry {mac, fid 0} -> port_bitmap via one NTMP command BD at slot idx. */
static int fdb_add(unsigned idx, const uint8_t mac[6], uint32_t port_bitmap)
{
    volatile uint8_t *b = (volatile uint8_t *)DBUF_BASE;
    for (int i = 0; i < 48; i++) { b[i] = 0; }
    for (int i = 0; i < 6; i++)  { b[4 + i] = mac[i]; }     /* keye.macAddr @ +4 */
    *(volatile uint32_t *)(DBUF_BASE + 12) = 0;             /* keye.fid = 0      */
    *(volatile uint32_t *)(DBUF_BASE + 36) = port_bitmap;   /* cfge.portBitmap   */

    volatile uint32_t *bd = (volatile uint32_t *)(RING_BASE + idx * 32u);
    bd[0] = DBUF_BASE; bd[1] = 0;
    bd[2] = (36u & 0xFFFFFu) | (48u << 20);
    bd[3] = CMD_ADDQUERY | (ACC_EXACTKEY << 12) | (TB_FDB << 16);
    bd[4] = bd[5] = bd[6] = bd[7] = 0;

    uint32_t pir = (idx + 1) % RING_LEN;
    CBDRPIR = pir;
    uint32_t g = 0;
    while ((CBDRCIR & 0x3FFu) != pir) { if (++g > 2000000u) { return 0; } }
    return ((bd[3] >> 16) & 0xFFFu) == 0;                   /* resp.error == 0 */
}

/* Inject one 64-byte frame to `dest` through the SI TX ring at slot idx. */
static void inject(unsigned idx, const uint8_t dest[6])
{
    volatile uint8_t *fr = (volatile uint8_t *)TXFRAME_BASE;
    for (int i = 0; i < 6; i++) { fr[i] = dest[i]; }        /* destination */
    for (int i = 0; i < 6; i++) { fr[6 + i] = 0x02; }       /* some unicast source */
    fr[12] = 0x88; fr[13] = 0xB6;
    for (int i = 14; i < 64; i++) { fr[i] = 0x5A; }

    volatile uint32_t *bd = (volatile uint32_t *)(TXRING_BASE + idx * 16u);
    bd[0] = TXFRAME_BASE; bd[1] = 0;
    *(volatile uint16_t *)(TXRING_BASE + idx * 16u + 10) = 64;  /* frameLen @ +10 */
    TB_PIR = idx + 1;                                          /* GO */
}

void reset_handler(void)
{
    int ok = 1;
    static const uint8_t X[6]  = { 0x02, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
    static const uint8_t Y[6]  = { 0x02, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB };
    static const uint8_t BC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    /* Command BD ring up (for NTMP FDB programming). */
    CBDRBAR0 = RING_BASE; CBDRBAR1 = 0; CBDRLENR = RING_LEN; CBDRMR = CBDRMR_EN;
    /* TX ring up. */
    MSIX0_CTRL = 1;
    TB_BAR0 = TXRING_BASE; TB_BAR1 = 0; TB_CIR = 0; TB_LENR = 8;

    /* Program the forwarding database: X reachable via the WIRE port (bit 0),
     * Y reachable only via the CPU/management port (bit 4). */
    if (!fdb_add(0, X, 0x1u))  { ok = 0; }     /* X -> wire port  */
    if (!fdb_add(1, Y, 0x10u)) { ok = 0; }     /* Y -> CPU port   */

    uint32_t c0 = PM0_TFRMN;

    /* Unicast to X -> its FDB entry names the wire -> forwarded to the wire. */
    inject(0, X);
    uint32_t c1 = PM0_TFRMN;
    if (c1 != c0 + 1) { ok = 0; }

    /* Unicast to Y -> FDB entry is CPU-only -> nothing egresses the wire. */
    inject(1, Y);
    uint32_t c2 = PM0_TFRMN;
    if (c2 != c1) { ok = 0; }

    /* Broadcast -> floods all ports -> reaches the wire. */
    inject(2, BC);
    uint32_t c3 = PM0_TFRMN;
    if (c3 != c2 + 1) { ok = 0; }

    if (ok) {
        puts_("NETC-FWD: PASS - unicast to a wire-mapped MAC is forwarded to the wire\r\n");
        puts_("NETC-FWD: PASS - unicast to a CPU-only MAC is NOT put on the wire\r\n");
        puts_("NETC-FWD: PASS - broadcast floods to the wire\r\n");
    } else {
        puts_("NETC-FWD: FAIL - switch egress decision wrong (see which assert)\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
