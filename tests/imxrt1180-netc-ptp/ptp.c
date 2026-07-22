/*
 * NETC PTP 1588 timer (TMR0) test (Cortex-M33), run under -icount.
 *
 * The 1588 timer is a digital DDS nanosecond clock: software reads a 64-bit time
 * from TMR_CUR_TIME_H/L, and tunes the frequency by scaling the addend
 * (TMR_ADD + TMR_CTRL[TCLK_PERIOD]).  We check the three properties firmware
 * relies on:
 *
 *   1. ADVANCES  - when enabled the clock moves forward (T2 > T1 over a delay).
 *   2. RATE      - the addend sets the rate: DOUBLING the addend doubles how fast
 *                  the clock advances over the same delay.  A "clock ticks" check
 *                  alone would pass a model that ignored the addend entirely; this
 *                  is the golden that the addend actually controls frequency.
 *   3. FREEZES   - clearing TMR_CTRL[TE] stops the clock (T6 == T5 over a delay).
 *
 * Timing is deterministic only under -icount (the virtual clock advances per
 * instruction), so the rate ratio is exact; the Makefile runs -icount.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* TMR0 @ 0x60B8_0000. */
#define TMR_CTRL  (*(volatile uint32_t *)(0x60B80080u))   /* TE = bit 2 */
#define TMR_CNT_L (*(volatile uint32_t *)(0x60B80098u))
#define TMR_CNT_H (*(volatile uint32_t *)(0x60B8009Cu))
#define TMR_ADD   (*(volatile uint32_t *)(0x60B800A0u))
#define TMR_CUR_L (*(volatile uint32_t *)(0x60B800F0u))
#define TMR_CUR_H (*(volatile uint32_t *)(0x60B800F4u))
#define TMR_CTRL_TE 0x4u

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

/* Coherent 64-bit read, exactly as the fsl_netc_timer driver does it (reading
 * _L latches _H; the H-L-H loop rejects a mid-read high-word rollover). */
static uint64_t read_cur_time(void)
{
    uint32_t hi, lo, hi2;
    hi = TMR_CUR_H;
    do {
        hi2 = hi;
        lo  = TMR_CUR_L;
        hi  = TMR_CUR_H;
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

static void spin(volatile uint32_t n)
{
    while (n--) {
        asm volatile("" ::: "memory");
    }
}

void reset_handler(void)
{
    int ok = 1;
    const uint32_t SPIN = 200000u;

    /* Enable at rate 1: TCLK_PERIOD=0, nominal addend 0x4000_0000, counter 0. */
    TMR_CTRL  = TMR_CTRL_TE;          /* enable (TCLK_PERIOD = 0) */
    TMR_ADD   = 0x40000000u;          /* first addend written => the rate-1 nominal */
    TMR_CNT_L = 0;
    TMR_CNT_H = 0;

    /* 1) ADVANCES: the clock must move forward over a fixed delay. */
    uint64_t t1 = read_cur_time();
    spin(SPIN);
    uint64_t t2 = read_cur_time();
    uint64_t d1 = t2 - t1;
    if (!(t2 > t1)) { ok = 0; }

    /* 2) RATE: doubling the addend must double the advance over the same delay. */
    TMR_ADD = 0x80000000u;            /* full addend now 2x nominal => rate 2 */
    uint64_t t3 = read_cur_time();
    spin(SPIN);
    uint64_t t4 = read_cur_time();
    uint64_t d2 = t4 - t3;
    /* d2 should be ~2*d1; allow +/-10% for the constant read overhead.  Written
     * as cross-multiplications (no 64-bit divide -> no libgcc in -nostdlib):
     *   1.8*d1 < d2 < 2.2*d1  <=>  9*d1 < 5*d2  &&  5*d2 < 11*d1 */
    int rate_ok = (d1 > 0) &&
                  (5u * d2 > 9u * d1) &&
                  (5u * d2 < 11u * d1);
    if (!rate_ok) { ok = 0; }

    /* 3) FREEZES: with TE cleared the clock must stop. */
    TMR_CTRL = 0;                     /* disable */
    uint64_t t5 = read_cur_time();
    spin(SPIN);
    uint64_t t6 = read_cur_time();
    if (t6 != t5) { ok = 0; }

    if (ok) {
        puts_("NETC-PTP: PASS - 1588 clock advances when enabled\r\n");
        puts_("NETC-PTP: PASS - doubling the addend doubles the rate (addend sets frequency)\r\n");
        puts_("NETC-PTP: PASS - clock freezes when TE is cleared\r\n");
    } else if (!(t2 > t1)) {
        puts_("NETC-PTP: FAIL - clock did not advance when enabled\r\n");
    } else if (t6 != t5) {
        puts_("NETC-PTP: FAIL - clock kept running after TE cleared\r\n");
    } else {
        puts_("NETC-PTP: FAIL - addend did not scale the rate (2x expected)\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
