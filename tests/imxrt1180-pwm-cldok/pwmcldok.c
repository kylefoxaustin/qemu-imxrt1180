/*
 * eFlexPWM double-buffer commit via the SDK's CLDOK-then-LDOK RMW (Cortex-M33).
 *
 * fsl_pwm / mc_pmsm do not write MCTRL[LDOK] with a bare store -- they clear the
 * load-OK strobe first and then read-modify-write the register to set it:
 *     MCTRL = (MCTRL & ~CLDOK_MASK) | CLDOK(0xF);
 *     MCTRL = (MCTRL & ~LDOK_MASK)  | LDOK(0xF);
 * CLDOK and LDOK are write-only strobes that read back 0 on silicon.  If the
 * model instead PERSISTS CLDOK, the second RMW reads it back and the store then
 * carries CLDOK|LDOK together -- which a "clear was requested, skip the commit"
 * guard reads as "do not load", so INIT/VALx never leave their double buffer.
 * The submodule then runs with a ZERO period, which (in the FOC demo) floods the
 * PWM->ADC trigger and overflows the result FIFO.  This test reproduces exactly
 * that RMW sequence and requires the buffered values to LOAD.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

#define PWM1       0x42650000u
#define SM0_INIT   (*(volatile uint16_t *)(PWM1 + 0x02))
#define SM0_VAL1   (*(volatile uint16_t *)(PWM1 + 0x0E))
#define PWM_MCTRL  (*(volatile uint16_t *)(PWM1 + 0x188))
#define MCTRL_LDOK(x)  ((uint16_t)(x))          /* bits 3:0  */
#define MCTRL_CLDOK(x) ((uint16_t)((x) << 4))   /* bits 7:4  */

/* A 16 kHz carrier off a 132 MHz fast clock: modulo 8250, center-aligned. */
#define WANT_MODULO 8250
#define WANT_INIT   ((uint16_t)(-(WANT_MODULO / 2)))   /* 0xEFE3 */
#define WANT_VAL1   ((uint16_t)((WANT_MODULO / 2) - 1)) /* 0x101C */

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

void reset_handler(void)
{
    int ok = 1;

    /* Program INIT/VAL1 -- these are double-buffered, so the LIVE registers must
     * still read their reset value (0) until a load is committed. */
    SM0_INIT = WANT_INIT;
    SM0_VAL1 = WANT_VAL1;
    if (SM0_INIT != 0 || SM0_VAL1 != 0) {
        ok = 0;                     /* not actually double-buffered */
    }

    /* The exact SDK commit: strobe CLDOK, then RMW LDOK. */
    PWM_MCTRL = (uint16_t)((PWM_MCTRL & ~MCTRL_CLDOK(0xF)) | MCTRL_CLDOK(0xF));
    PWM_MCTRL = (uint16_t)((PWM_MCTRL & ~MCTRL_LDOK(0xF)) | MCTRL_LDOK(0xF));

    /* The buffered values must now be LIVE. */
    int committed = (SM0_INIT == WANT_INIT) && (SM0_VAL1 == WANT_VAL1);
    if (!committed) {
        ok = 0;
    }

    /* CLDOK/LDOK are write-only strobes: they must read back 0, or the next RMW
     * commit re-reads a stale CLDOK and silently skips (the bug this guards). */
    int strobes_clear = (PWM_MCTRL & (MCTRL_CLDOK(0xF) | MCTRL_LDOK(0xF))) == 0;
    if (!strobes_clear) {
        ok = 0;
    }

    if (ok) {
        puts_("PWMCLDOK: PASS - CLDOK-then-RMW-LDOK commits the double buffer; "
              "strobes read back 0\r\n");
    } else if (!committed) {
        puts_("PWMCLDOK: FAIL - RMW LDOK did not load INIT/VAL1 (CLDOK persisted?)\r\n");
    } else {
        puts_("PWMCLDOK: FAIL - LDOK/CLDOK strobe persisted or buffering absent\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
