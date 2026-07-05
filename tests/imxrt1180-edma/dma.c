/*
 * eDMA memory-to-memory test (Cortex-M33).
 *
 * Programs eDMA3 channel 0 with a TCD to copy a 16-byte source buffer to a
 * destination buffer (four 32-bit elements, one major loop), triggers it via
 * TCD_CSR.START, and verifies the copy + the channel DONE flag.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* eDMA3 @ 0x4400_0000; channel 0 block @ +0x1000. */
#define CH0 0x44001000u
#define R32(o) (*(volatile uint32_t *)(CH0 + (o)))
#define R16(o) (*(volatile uint16_t *)(CH0 + (o)))
#define CH_CSR     R32(0x000)
#define TCD_SADDR  R32(0x020)
#define TCD_SOFF   R16(0x024)
#define TCD_ATTR   R16(0x026)
#define TCD_NBYTES R32(0x028)
#define TCD_SLAST  R32(0x02C)
#define TCD_DADDR  R32(0x030)
#define TCD_DOFF   R16(0x034)
#define TCD_CITER  R16(0x036)
#define TCD_DLAST  R32(0x038)
#define TCD_CSR    R16(0x03C)
#define TCD_BITER  R16(0x03E)

#define CSR_DONE   (1u << 30)
#define TCD_START  (1u << 0)

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

static volatile uint32_t src[4] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };
static volatile uint32_t dst[4];

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    dst[0] = dst[1] = dst[2] = dst[3] = 0;

    TCD_SADDR  = (uint32_t)(uintptr_t)src;
    TCD_DADDR  = (uint32_t)(uintptr_t)dst;
    TCD_SOFF   = 4;            /* 32-bit element stride */
    TCD_DOFF   = 4;
    TCD_ATTR   = (2u << 8) | 2u;   /* SSIZE=DSIZE=2 -> 4-byte elements */
    TCD_NBYTES = 16;          /* one minor loop = 16 bytes */
    TCD_SLAST  = 0;
    TCD_DLAST  = 0;
    TCD_CITER  = 1;
    TCD_BITER  = 1;
    TCD_CSR    = TCD_START;   /* trigger the transfer */

    int ok = (CH_CSR & CSR_DONE) &&
             dst[0] == 0x11111111 && dst[1] == 0x22222222 &&
             dst[2] == 0x33333333 && dst[3] == 0x44444444;
    if (ok) {
        puts_("eDMA: PASS - 16-byte mem-to-mem copy + DONE\r\n");
    } else {
        puts_("eDMA: FAIL - transfer did not complete correctly\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
