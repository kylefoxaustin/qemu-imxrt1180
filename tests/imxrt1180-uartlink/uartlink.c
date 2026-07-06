/*
 * i.MX RT1180 board-to-board UART link test (Cortex-M33).
 *
 * Drives LPUART2 (the b2b link port, 0x4439_0000, bound to serial_hd(1) = a
 * socket chardev to a peer).  Follows the fleet's proven pattern: a
 * resend-until-connected GO handshake that is immune to boot order, then a
 * known 32-byte pattern is sent and its echo verified byte-exact.  The peer
 * (uart_peer.py) simply echoes every byte it receives.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

#define LPUART2_BASE 0x44390000u
#define LPUART_STAT  (*(volatile uint32_t *)(LPUART2_BASE + 0x14))
#define LPUART_CTRL  (*(volatile uint32_t *)(LPUART2_BASE + 0x18))
#define LPUART_DATA  (*(volatile uint32_t *)(LPUART2_BASE + 0x1C))
#define CTRL_TE   0x00080000u
#define CTRL_RE   0x00040000u
#define STAT_RDRF 0x00200000u
#define STAT_TDRE 0x00800000u

#define GO_BYTE   0xA5u
#define N_BYTES   32

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

static void tx(uint8_t b)
{
    while (!(LPUART_STAT & STAT_TDRE)) {
    }
    LPUART_DATA = b;
}

/* Poll for a received byte with a bounded wait; returns -1 on timeout. */
static int rx_timeout(uint32_t spins)
{
    while (spins--) {
        if (LPUART_STAT & STAT_RDRF) {
            return (int)(LPUART_DATA & 0xFF);
        }
    }
    return -1;
}

void reset_handler(void);

__attribute__((section(".vectors"), used))
void (* const vt[])(void) = {
    [0] = (void (*)(void))STACK_TOP,
    [1] = reset_handler,
};

void reset_handler(void)
{
    int ok = 1;

    LPUART_CTRL = CTRL_TE | CTRL_RE;         /* enable TX + RX */

    /* Resend GO until the peer echoes it -> the link is up (boot-order-immune). */
    int connected = 0;
    for (int attempt = 0; attempt < 100000 && !connected; attempt++) {
        tx(GO_BYTE);
        if (rx_timeout(20000) == (int)GO_BYTE) {
            connected = 1;
        }
    }
    if (!connected) { ok = 0; }

    /* Send a known pattern and verify each echoed byte. */
    for (int i = 0; i < N_BYTES && ok; i++) {
        uint8_t b = (uint8_t)(0x10 + i);
        tx(b);
        int r = rx_timeout(2000000);
        if (r != (int)b) { ok = 0; }
    }

    if (ok) {
        puts_("UARTLINK: PASS - GO handshake + 32-byte echo byte-exact\r\n");
    } else {
        puts_("UARTLINK: FAIL - link handshake or echo mismatch\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
