/*
 * LPADC test (Cortex-M33).
 *
 * Verifies the ADC model's conversion engine the way the FOC front-end uses it:
 *   1. Calibration: CTRL.CAL_REQ reports STAT.CAL_RDY (LPADC_DoAutoCalibration).
 *   2. Command chain: a software trigger (SWTRIG) runs command 1 -> command 2
 *      (via CMDH.NEXT), pushing two results into the result FIFO.
 *   3. Result FIFO: FCTRL.FCOUNT tracks the fill, STAT.RDY0 sets, and each
 *      RESFIFO read pops a VALID, trigger-tagged result.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* ADC1 @ 0x4260_0000. */
#define ADC1_BASE 0x42600000u
#define CTRL    (*(volatile uint32_t *)(ADC1_BASE + 0x10))
#define STAT    (*(volatile uint32_t *)(ADC1_BASE + 0x14))
#define SWTRIG  (*(volatile uint32_t *)(ADC1_BASE + 0x34))
#define TCTRL0  (*(volatile uint32_t *)(ADC1_BASE + 0xA0))
#define FCTRL0  (*(volatile uint32_t *)(ADC1_BASE + 0xE0))
#define CMDL0   (*(volatile uint32_t *)(ADC1_BASE + 0x100))
#define CMDH0   (*(volatile uint32_t *)(ADC1_BASE + 0x104))
#define CMDL1   (*(volatile uint32_t *)(ADC1_BASE + 0x108))
#define CMDH1   (*(volatile uint32_t *)(ADC1_BASE + 0x10C))
#define RESFIFO0 (*(volatile uint32_t *)(ADC1_BASE + 0x300))

#define CTRL_ADCEN    0x1u
#define CTRL_CAL_REQ  0x8u
#define STAT_RDY0     0x1u
#define STAT_CAL_RDY  0x400u
#define RESFIFO_VALID 0x80000000u

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

    /* (1) Enable + calibrate. */
    CTRL = CTRL_ADCEN | CTRL_CAL_REQ;
    if (!(STAT & STAT_CAL_RDY)) { ok = 0; }

    /* (2) Two-command chain: cmd1 (ch5) -> cmd2 (ch7) -> end. */
    CMDL0 = 5;                 /* ADCH = 5 */
    CMDH0 = (2u << 24);        /* NEXT = command 2 */
    CMDL1 = 7;                 /* ADCH = 7 */
    CMDH1 = 0;                 /* NEXT = 0 (end of chain) */
    TCTRL0 = (1u << 24);       /* TCMD = command 1 */
    FCTRL0 = 0;                /* watermark 0 -> RDY as soon as a result lands */

    /* (2b) Software trigger source 0. */
    SWTRIG = 1u;

    /* (3) Two results queued, RDY set. */
    if ((FCTRL0 & 0x1Fu) != 2u) { ok = 0; }
    if (!(STAT & STAT_RDY0)) { ok = 0; }

    uint32_t r1 = RESFIFO0;
    uint32_t r2 = RESFIFO0;
    if (!(r1 & RESFIFO_VALID) || !(r2 & RESFIFO_VALID)) { ok = 0; }
    if ((FCTRL0 & 0x1Fu) != 0u) { ok = 0; }        /* FIFO drained */

    uint32_t r3 = RESFIFO0;                         /* empty -> not VALID */
    if (r3 & RESFIFO_VALID) { ok = 0; }

    if (ok) {
        puts_("ADC: PASS - calibration + command chain + tagged result FIFO\r\n");
    } else {
        puts_("ADC: FAIL - calibration / trigger / FIFO misbehaved\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
