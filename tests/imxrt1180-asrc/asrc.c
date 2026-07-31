/*
 * RT1180 ASRC data-path value-test (M33) -- polyphase windowed-sinc resampler.
 *
 * The model's resampler is a real bandlimited (anti-imaging / anti-aliasing) FIR,
 * NOT the crude linear interpolation it replaced, and NOT NXP's exact polyphase
 * taps (those are not published, so output values cannot bit-match silicon).  So
 * this test asserts the two DSP FIRST-PRINCIPLES properties any correct resampler
 * must have -- neither read back from the model:
 *
 *   1. UNITY DC GAIN (pass-band).  A constant input must come out constant: the
 *      per-output tap-sum normalisation guarantees gain exactly 1 at DC, so a
 *      1:2 up-conversion of a constant C is C, byte-exact.  (A resampler that
 *      emitted un-computed data, or scaled wrong, fails this immediately.)
 *
 *   2. STOP-BAND REJECTION (anti-aliasing) -- the whole reason the hardware uses a
 *      FIR.  A full-scale NYQUIST tone [C,-C,C,-C,...] down-converted 2:1 sits
 *      ABOVE the output Nyquist and must be REJECTED (~0).  Linear interpolation
 *      (or any non-bandlimited scheme) instead samples it straight through at the
 *      integer down-conversion instants -> it ALIASES to DC at full amplitude C.
 *      So this single threshold cleanly separates a real anti-alias filter from
 *      the old model:  FIR gives |out| ~ 15, linear interp gives ~ 10000, for an
 *      input amplitude of 10000.  The gate is 500 (33x above the FIR result, 20x
 *      below the aliasing result) -- MEASURED, with margin, and it FAILS if the
 *      resampler stops bandlimiting (proven by reverting to linear interp).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u
#define ASRC 0x429A0000u
#define R(o) (*(volatile uint32_t *)(ASRC + (o)))
#define ASRCTR  0x00
#define ASRCNCR 0x0C
#define ASRCDR1 0x18
#define ASRSTR  0x20
#define ASRDIA  0x60
#define ASRDOA  0x64
#define AODFA   (1u << 3)          /* pair-A output ready */

#define DC_C       1000            /* constant for the DC-gain test */
#define TONE_AMPL  10000           /* Nyquist-tone amplitude for the stop-band test */
#define ALIAS_GATE 500             /* max|out| allowed for the rejected tone (measured) */

static long sh(long op, void *a){register long r0 asm("r0")=op;register void*r1 asm("r1")=a;asm volatile("bkpt 0xAB":"+r"(r0):"r"(r1):"memory");return r0;}
static void puts_(const char *s){sh(SYS_WRITE0,(void*)s);}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { [0] = (void (*)(void))STACK_TOP, [1] = reset_handler };

void reset_handler(void)
{
    int ok = 1;

    /* --- 1. UNITY DC GAIN: constant C, 1:2 up-conversion, out must be C. ------
     * ASRCDR1 = 0x08 -> in_div=2, out_div=1 -> out_rate/in_rate = 2. */
    R(ASRCNCR) = 1;
    R(ASRCDR1) = 0x08;
    R(ASRCTR)  = 1u | (1u << 20);          /* enable + ATSA (init pair A) */
    for (int i = 0; i < 40; i++) {
        R(ASRDIA) = DC_C;
    }
    int n = 0, guard = 0;
    while (n < 40) {
        if (!(R(ASRSTR) & AODFA)) { if (++guard > 200000) { ok = 0; break; } continue; }
        int32_t s = (int32_t)(R(ASRDOA) << 8) >> 8;   /* 24-bit signed */
        if (n >= 16 && n < 32 && s != DC_C) { ok = 0; }   /* steady region == C */
        n++;
    }

    /* --- 2. STOP-BAND: Nyquist tone, 2:1 down-conversion, must be rejected. ---
     * ASRCDR1 = 0x8000 -> out_div=2, in_div=1 -> out_rate/in_rate = 1/2. */
    R(ASRCTR)  = 1u | (1u << 20);          /* ATSA re-init pair A */
    R(ASRCNCR) = 1;
    R(ASRCDR1) = 0x8000;
    for (int i = 0; i < 80; i++) {
        R(ASRDIA) = (i & 1) ? -TONE_AMPL : TONE_AMPL;
    }
    int maxabs = 0, m = 0;
    guard = 0;
    while (m < 30) {
        if (!(R(ASRSTR) & AODFA)) { if (++guard > 200000) { ok = 0; break; } continue; }
        int32_t s = (int32_t)(R(ASRDOA) << 8) >> 8;
        int a = s < 0 ? -s : s;
        if (m >= 8 && a > maxabs) { maxabs = a; }       /* skip the edge transient */
        m++;
    }
    if (maxabs >= ALIAS_GATE) { ok = 0; }

    if (ok) {
        puts_("ASRC: PASS - unity DC gain (1:2 const->const) and Nyquist-tone "
              "rejection on 2:1 down-conversion (anti-aliasing FIR)\r\n");
    } else {
        puts_("ASRC: FAIL - resampler is not a correct bandlimited FIR "
              "(DC gain or stop-band rejection wrong)\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) { }
}
