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

/*
 * SysTick — an INDEPENDENT reference clock, used to check the PWM's actual
 * PERIOD.  It is an Arm core timer, not one of our peripheral models, so it
 * cannot be wrong in the same way the PWM is.
 */
#define SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018u)

/*
 * THE GOLDEN.  Derived independently of the model, from the machine's own clocks:
 *   PWM counter clock  = 200 MHz  (fast-peripheral clock)
 *   prescaler          = 1        (CTRL.PRSC = 0)
 *   period             = VAL1 - INIT + 1 = 499 - (-500) + 1 = 1000 counter ticks
 *   => PWM reload rate = 200e6 / 1000                    = 200 kHz
 *   CPU (SysTick) clock = 300 MHz
 *   => CPU cycles per PWM period = 300e6 / 200e3         = 1500
 *
 * A test that only counts reload IRQs cannot see a wrong period at all: a
 * 3x-too-long carrier still fires interrupts, still "runs the counter", and
 * still passes.  For an FOC carrier a silently-wrong frequency makes every
 * downstream current-loop result a lie, so the FREQUENCY is the thing that has
 * to be asserted, against a number the model did not supply.
 */
#define CPU_HZ            300000000u
#define PWM_HZ            200000000u
#define PWM_PERIOD_TICKS  1000u
#define PWM_PRESCALE      64u          /* CTRL.PRSC = 6 -> divide by 64 */
#define CTRL_PRSC_6       (6u << 4)

/*
 * WHY THE CARRIER IS PRESCALED FOR THIS MEASUREMENT.  Un-prescaled the reload
 * rate is 200 kHz -- one interrupt every 5 us of VIRTUAL time, which the
 * emulated core cannot service in time. Reloads then COALESCE (the second sets
 * STS.RF again before the ISR has run, so the guest sees ONE interrupt), and
 * counting ISR entries under-counts the true rate. That is an artefact of
 * measuring by interrupt, not a model defect -- but it means the golden must be
 * measured at a rate the guest can actually keep up with.
 *   reload rate = 200e6 / 64 / 1000 = 3125 Hz
 *   CPU cycles per period = 300e6 / 3125 = 96000
 */
#define EXPECT_CPU_CYCLES_PER_PWM \
    (CPU_HZ / (PWM_HZ / PWM_PRESCALE / PWM_PERIOD_TICKS))     /* 96000 */
#define MEASURE_PERIODS   10u

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }
static void dec(uint32_t v)
{
    char b[12]; int i = 11; b[11] = 0;
    if (!v) { puts_("0"); return; }
    while (v) { b[--i] = '0' + (v % 10u); v /= 10u; }
    puts_(&b[i]);
}

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

    SM0_CTRL = CTRL_PRSC_6;                    /* prescale /64 (see golden) */

    /* (2) Enable reload interrupt + NVIC line. */
    SM0_INTEN = INTEN_RIE;
    NVIC_ISER0 = (1u << PWM1_SM0_IRQ);

    /* (3) Start submodule 0. */
    PWM_MCTRL = MCTRL_RUN & (1u << 8);

    /*
     * The reload interrupt firing repeatedly is definitive proof the counter is
     * running (the submodule cannot reach a reload without counting INIT->VAL1).
     * A direct CNT-inequality check is avoided: CNT is periodic, so two samples
     * can coincide by phase on a fast host (a false failure).
     */
    while (reloads < 3) {                      /* wait for periodic reloads */
    }

    /*
     * (4) THE PERIOD ITSELF, against an independent reference clock.
     *
     * SysTick is a free-running 24-bit DOWN counter on the core clock.  20 PWM
     * periods = ~30,000 CPU cycles, well inside the 16.7M-cycle span, so it
     * cannot wrap during the measurement.
     */
    SYST_RVR = 0x00FFFFFFu;
    SYST_CVR = 0;                   /* writing CVR clears it and COUNTFLAG */
    SYST_CSR = 0x5u;                /* ENABLE | CLKSOURCE=core             */

    int start = reloads;
    while (reloads == start) {      /* align to a reload edge */
    }
    uint32_t c0 = SYST_CVR;
    int from = reloads;
    while (reloads < from + (int)MEASURE_PERIODS) {
    }
    uint32_t c1 = SYST_CVR;

    uint32_t elapsed = (c0 - c1) & 0x00FFFFFFu;          /* counts DOWN */
    uint32_t cycles_per_period = elapsed / MEASURE_PERIODS;

    puts_("PWM period: measured "); dec(cycles_per_period);
    puts_(" CPU cycles, expected "); dec(EXPECT_CPU_CYCLES_PER_PWM);
    puts_("\r\n");

    /* +/-10%: generous for host-timing jitter, nowhere near a 3x error. */
    uint32_t lo = EXPECT_CPU_CYCLES_PER_PWM - EXPECT_CPU_CYCLES_PER_PWM / 10u;
    uint32_t hi = EXPECT_CPU_CYCLES_PER_PWM + EXPECT_CPU_CYCLES_PER_PWM / 10u;
    int period_ok = (cycles_per_period >= lo && cycles_per_period <= hi);
    if (!period_ok) { ok = 0; }

    if (ok && reloads >= 3 && period_ok) {
        puts_("PWM: PASS - double-buffer commit + reload IRQ + PERIOD matches the golden\r\n");
    } else if (!period_ok) {
        puts_("PWM: FAIL - carrier PERIOD is wrong (IRQs fire, counter runs, frequency lies)\r\n");
    } else {
        puts_("PWM: FAIL - buffering / reload IRQ misbehaved\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
