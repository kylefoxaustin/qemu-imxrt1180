/*
 * eFlexPWM test (Cortex-M33).
 *
 * Sets up PWM1 submodule 0 as a center-aligned PWM the way the FOC motor-control
 * init does (INIT/VAL1 define the period, VAL2/VAL3 the ~50%% duty edges), and
 * verifies the model's key behaviours:
 *   1. Double buffering: INIT/VALx writes are buffered — INIT reads back 0 until
 *      MCTRL.LDOK, then reads back the committed value.
 *   2. Periodic reload interrupt: with INTEN.RIE + MCTRL.RUN, the submodule
 *      reload ISR (IRQ 24) fires repeatedly — the FOC control-loop clock.
 *   3. The counter runs: CNT advances while the submodule is running.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* PWM1 @ 0x4265_0000, submodule 0 registers (16-bit). */
#define PWM1_BASE 0x42650000u
#define SM0_CNT   (*(volatile uint16_t *)(PWM1_BASE + 0x00))
#define SM0_INIT  (*(volatile uint16_t *)(PWM1_BASE + 0x02))
#define SM0_CTRL  (*(volatile uint16_t *)(PWM1_BASE + 0x06))
#define SM0_VAL1  (*(volatile uint16_t *)(PWM1_BASE + 0x0E))
#define SM0_VAL2  (*(volatile uint16_t *)(PWM1_BASE + 0x12))
#define SM0_VAL3  (*(volatile uint16_t *)(PWM1_BASE + 0x16))
#define SM0_STS   (*(volatile uint16_t *)(PWM1_BASE + 0x24))
#define SM0_INTEN (*(volatile uint16_t *)(PWM1_BASE + 0x26))
#define PWM_MCTRL (*(volatile uint16_t *)(PWM1_BASE + 0x188))

#define STS_RF     0x1000u
#define INTEN_RIE  0x1000u
#define MCTRL_LDOK 0x000Fu
#define MCTRL_RUN  0x0F00u   /* RUN[sm] = bit (8 + sm) */

#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100u)
#define PWM1_SM0_IRQ 24

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

static volatile int reloads;

void reset_handler(void);
void pwm_sm0_isr(void);

__attribute__((section(".vectors"), used))
void (* const vt[])(void) = {
    [0]                    = (void (*)(void))STACK_TOP,
    [1]                    = reset_handler,
    [16 + PWM1_SM0_IRQ]    = pwm_sm0_isr,
};

void pwm_sm0_isr(void)
{
    SM0_STS = STS_RF;      /* W1C the reload flag (deasserts the IRQ) */
    reloads++;
}

void reset_handler(void)
{
    int ok = 1;

    /* Center-aligned period 1000, ~50%% duty (VAL3-VAL2 = 500). */
    SM0_INIT = (uint16_t)(-500);
    SM0_VAL1 = 499;
    SM0_VAL2 = (uint16_t)(-250);
    SM0_VAL3 = 250;

    /* (1) Buffered: INIT must still read 0 before LDOK. */
    if (SM0_INIT != 0) { ok = 0; }

    PWM_MCTRL = MCTRL_LDOK;                    /* commit all submodules */

    /* (1b) Committed: INIT now reads back the written value. */
    if (SM0_INIT != (uint16_t)(-500)) { ok = 0; }

    /* (2) Enable reload interrupt + NVIC line. */
    SM0_INTEN = INTEN_RIE;
    NVIC_ISER0 = (1u << PWM1_SM0_IRQ);

    /* (3) Start submodule 0. */
    PWM_MCTRL = MCTRL_RUN & (1u << 8);

    uint16_t c1 = SM0_CNT;
    for (volatile int i = 0; i < 200; i++) {
    }
    uint16_t c2 = SM0_CNT;

    while (reloads < 3) {                      /* wait for periodic reloads */
    }

    if (ok && reloads >= 3 && c1 != c2) {
        puts_("PWM: PASS - double-buffer commit + periodic reload IRQ + counter runs\r\n");
    } else {
        puts_("PWM: FAIL - buffering / reload IRQ / counter misbehaved\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
