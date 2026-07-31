/*
 * RT1180 ASRC TRUE-ASYNC ratio value-test (M33).
 *
 * The ASRC output-per-input ratio is
 *
 *     ratio = (outSrcHz / inSrcHz) * (in_period / out_period)
 *
 * where in_period/out_period is the ASRCDR divider ratio and outSrcHz/inSrcHz is
 * the ratio of the two selected clock sources.  Every m2m example uses ONE source
 * for both (ASRCSR AICSA == AOCSA), so the source factor is exactly 1 and the
 * divider ratio alone is correct.  This test drives a genuinely ASYNC conversion:
 *
 *   - input  clocked by SAI1_TX bit clock  (ASRCSR AICSA = 0)
 *   - output clocked by SAI2_TX bit clock  (ASRCSR AOCSA = 2)
 *   - EQUAL ASRCDR dividers (ASRCDR1 = 0), so the divider ratio is 1
 *   - SAI1 TCR2[DIV]=1 -> bclk = MCLK/4;  SAI2 TCR2[DIV]=0 -> bclk = MCLK/2
 *     => outSrcHz/inSrcHz = (MCLK/2)/(MCLK/4) = 2   (MCLK cancels)
 *
 * so the true ratio is 2*1 = 2 -- a 1:2 upsample driven PURELY by the source-clock
 * ratio.  We feed the same linear ramp as the divider-based ASRC test and check
 * the identical exact-interpolation golden (out[2j]=5j (L), out[2j+1]=100+5j (R)).
 *
 * A model that ignores the source factor computes ratio = 1 (equal dividers) and
 * copies the input through unchanged -- the golden then fails on the very first
 * output sample.  Both SAIs are mastered off the always-on 24 MHz RC oscillator
 * (CCM root mux slot 0), so no PLL bring-up is needed and MCLK cancels exactly.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* --- CCM: point SAI1/SAI2 clock roots at the 24 MHz RC osc (mux 0, div 0). --- */
#define CCM_BASE   0x44450000u
#define CCM_ROOT(n) (*(volatile uint32_t *)(CCM_BASE + (n) * 0x80u))
#define ROOT_SAI1  65u
#define ROOT_SAI2  66u
#define ROOT_MUX_OSC24M 0u        /* mux slot 0 = CLK_OSC_RC_24M = 24 MHz */

/* --- two SAIs, bit-clock masters at different dividers. --- */
#define SAI1_TCR2 (*(volatile uint32_t *)(0x443B0000u + 0x10))
#define SAI2_TCR2 (*(volatile uint32_t *)(0x42BB0000u + 0x10))
#define TCR2_BCD  (1u << 24)      /* bit-clock MASTER */

/* --- ASRC --- */
#define ASRC 0x429A0000u
#define R(o) (*(volatile uint32_t *)(ASRC + (o)))
#define ASRCTR  0x00
#define ASRCNCR 0x0C
#define ASRCSR  0x14
#define ASRCDR1 0x18
#define ASRSTR  0x20
#define ASRDIA  0x60
#define ASRDOA  0x64
#define AODFA   (1u << 3)          /* pair-A output ready */

#define NFRAMES  16
#define NCHECK   20                /* output samples to verify (clean region) */

static long sh(long op, void *a){register long r0 asm("r0")=op;register void*r1 asm("r1")=a;asm volatile("bkpt 0xAB":"+r"(r0):"r"(r1):"memory");return r0;}
static void puts_(const char *s){sh(SYS_WRITE0,(void*)s);}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { [0] = (void (*)(void))STACK_TOP, [1] = reset_handler };

void reset_handler(void)
{
    /* Both SAI roots off the 24 MHz RC osc, div 0 -> MCLK = 24 MHz for each. */
    CCM_ROOT(ROOT_SAI1) = (ROOT_MUX_OSC24M << 8) | 0u;
    CCM_ROOT(ROOT_SAI2) = (ROOT_MUX_OSC24M << 8) | 0u;

    /* SAI1 bclk = MCLK/(2*(1+1)) = 6 MHz ; SAI2 bclk = MCLK/(2*(0+1)) = 12 MHz. */
    SAI1_TCR2 = TCR2_BCD | 1u;     /* input-source bit clock  (6 MHz)  */
    SAI2_TCR2 = TCR2_BCD | 0u;     /* output-source bit clock (12 MHz) */

    /* ASRC pair A: input = SAI1_TX (sel 0), output = SAI2_TX (sel 2). */
    R(ASRCSR)  = (2u << 12) | 0u;  /* AOCSA=2 (SAI2_TX), AICSA=0 (SAI1_TX) */
    R(ASRCNCR) = 2;                /* pair A: 2 channels */
    R(ASRCDR1) = 0x00;             /* in_div=out_div=1, presc 0 -> divider ratio 1 */
    R(ASRCTR)  = 1u | (1u << 20);  /* enable + ATSA (init pair A) */

    for (int i = 0; i < NFRAMES; i++) {
        R(ASRDIA) = (uint32_t)(10 * i);          /* L */
        R(ASRDIA) = (uint32_t)(100 + 10 * i);    /* R */
    }

    int ok = 1, m = 0, guard = 0;
    while (m < NCHECK) {
        if (!(R(ASRSTR) & AODFA)) { if (++guard > 100000) { ok = 0; break; } continue; }
        int32_t s = (int32_t)(R(ASRDOA) << 8) >> 8;   /* 24-bit signed */
        int j = m / 2;
        int32_t expect = (m & 1) ? (100 + 5 * j) : (5 * j);
        if (s != expect) { ok = 0; break; }
        m++;
    }

    if (ok && m == NCHECK) {
        puts_("ASRC-ASYNC: PASS - 1:2 upsample driven by the SAI2/SAI1 clock-source "
              "ratio (equal dividers), output matches the exact linear-interp golden\r\n");
    } else {
        puts_("ASRC-ASYNC: FAIL - the true-async source-clock ratio was not applied "
              "(output does not match the golden)\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) { }
}
