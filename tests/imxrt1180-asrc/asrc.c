/*
 * RT1180 ASRC (Asynchronous Sample Rate Converter) data-path value-test (M33).
 *
 * Drives the ASRC directly for a 1:2 upsample (ASRCDR1 encodes in_div=2/out_div=1,
 * so out_rate/in_rate = 2), feeds a linear RAMP (L = 10*i, R = 100 + 10*i), and
 * checks the resampled output against the EXACT linear-interpolation golden --
 * because interpolating a straight line is exact, the 2x-upsampled ramp is a
 * predictable half-step ramp: out[2j] = 5*j (L), out[2j+1] = 100 + 5*j (R).
 *
 * This verifies the ASRC produces genuinely resampled output (correct rate + a
 * correct function of the input), not un-computed data -- independent of the SAI
 * playback path the stock m2m example also uses.
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

#define NFRAMES  16
#define NCHECK   20                /* output samples to verify (clean region) */

static long sh(long op, void *a){register long r0 asm("r0")=op;register void*r1 asm("r1")=a;asm volatile("bkpt 0xAB":"+r"(r0):"r"(r1):"memory");return r0;}
static void puts_(const char *s){sh(SYS_WRITE0,(void*)s);}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { [0] = (void (*)(void))STACK_TOP, [1] = reset_handler };

void reset_handler(void)
{
    R(ASRCNCR) = 2;                 /* pair A: 2 channels          */
    R(ASRCDR1) = 0x08;              /* in_div=2, out_div=1 -> 1:2  */
    R(ASRCTR)  = 1u | (1u << 20);   /* enable + ATSA (init pair A) */

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
        puts_("ASRC: PASS - 1:2 upsample output matches the exact linear-interp golden\r\n");
    } else {
        puts_("ASRC: FAIL - resampled output does not match the golden\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) { }
}
