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
 *
 * ============ THE TRUST ANCHOR -- FIXED 2026-07-13, AND THIS IS THE STORY ======
 *
 * "A test cannot validate its own trust anchor." (ollama_95_neutron, 2026-07-12.)
 *
 * THIS FILE USED TO SAY, IN THIS COMMENT, IN SO MANY WORDS:
 *
 *     "PWM_HZ below is 200 MHz BECAUSE THAT IS WHAT THE MODEL USES
 *      (PWM_CLK_DEFAULT, a documented *nominal* fast-peripheral clock)...
 *      if PWM_CLK were wrong, this golden would be wrong in exactly the same
 *      direction and still pass."
 *
 * IT WAS. THE MODEL'S PWM CLOCK WAS WRONG BY 1.5x. This test took its golden FROM
 * THE MODEL, so it asked "does the PWM run at the frequency the model says it runs
 * at" -- a tautology -- and it passed, every time, for the life of the project.
 * THE MIRROR WAS WRITTEN DOWN, IN THIS COMMENT, AND NOBODY ACTED ON IT.
 *
 * The old comment also named the fix and called it open work: "have CCM COMPUTE the
 * PWM root frequency from the PLL/root config the firmware actually programs, and
 * have the PWM model take its clock from CCM. Then the anchor stops being ours."
 * That is now done, so the golden is now anchored where it belongs:
 *
 *     THE FIRMWARE PROGRAMS THE CLOCK ROOT, EXACTLY AS BOARD_InitBootClocks DOES,
 *     AND THE EXPECTED CARRIER IS DERIVED FROM THE SDK's OWN CONSTANTS:
 *
 *         SYS_PLL2 = XTAL * PLL_SYS2_528_MFI = 24 MHz * 22 = 528 MHz
 *         PWM root = SYS_PLL2 / root_div
 *
 * AND THE ROOT DIVIDER IS NOW A SWEPT AXIS. A model that ignores the clock tree --
 * which is exactly what this model did -- reports the SAME period at every root
 * divider, and fails here. That is the property the old golden could not express,
 * because it had no notion of a clock tree at all.
 */
#define CCM_BASE          0x44450000u
#define CCM_ROOT_CTRL(n)  (*(volatile uint32_t *)(CCM_BASE + (n) * 0x80u))
#define CLKROOT_BUS_WAKEUP 4u          /* eFlexPWM lives in WAKEUPMIX (0x4265_0000) */
#define ROOT_MUX_SYSPLL2  (2u << 8)    /* s_clockSourceName[BUS_WAKEUP][2] */

#define XTAL_HZ           24000000u
#define SYS_PLL2_HZ       (XTAL_HZ * 22u)              /* 528 MHz, fsl_clock.h */

#define CPU_HZ            300000000u
#define PWM_PERIOD_TICKS  1000u
#define PWM_PRESCALE      64u          /* CTRL.PRSC = 6 -> divide by 64 */
#define CTRL_PRSC_6       (6u << 4)

/* Program a clock root exactly as the firmware does: MUX + (divisor - 1). */
static void set_pwm_root_div(uint32_t div)
{
    CCM_ROOT_CTRL(CLKROOT_BUS_WAKEUP) = ROOT_MUX_SYSPLL2 | (div - 1u);
}

/*
 * WHY THE CARRIER IS PRESCALED FOR THIS MEASUREMENT.  Un-prescaled the reload
 * rate is ~132 kHz -- an interrupt every ~7.5 us of VIRTUAL time, which the
 * emulated core cannot service in time. Reloads then COALESCE (the second sets
 * STS.RF again before the ISR has run, so the guest sees ONE interrupt), and
 * counting ISR entries under-counts the true rate. That is an artefact of
 * measuring by interrupt, not a model defect -- but it means the golden must be
 * measured at a rate the guest can actually keep up with.
 *   at root div 4:  reload = 132e6 / 64 / 1000 = 2062.5 Hz
 *                   CPU cycles per period = 300e6 / 2062.5 = 145454
 *
 * (There WAS an EXPECT_CPU_CYCLES_PER_PWM macro here, hardcoding 96000 off the
 * old fabricated 200 MHz. It was DEFINED AND NEVER USED, so it kept compiling
 * after PWM_HZ was deleted -- a dead golden that still looked authoritative.
 * Deleted: an unused constant that names a number nobody computes is a trap.)
 */
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
     * (4) THE PERIOD ITSELF, against an independent reference clock -- SWEPT.
     *
     * ONE SHAPE IS NOT A GOLDEN. ollama_95_neutron found real silicon (NXP's
     * Neutron) that computes the RIGHT answer at one tensor shape and GARBAGE at
     * another, non-monotonically -- and had reported "it computes the right
     * answer" off a single cosine of 0.999938. "A single 'correct' stamp is a
     * worse lie than a single perf number, because it buys your trust and stops
     * you looking." A period check at one prescaler would pass a model that
     * mishandles PRSC entirely, or that is right at /64 and wrong at /16.
     *
     * So sweep BOTH axes the hardware actually has -- prescaler AND modulo --
     * and require every point to match its own independently-derived golden:
     *      cycles_per_period = CPU_HZ / (PWM_HZ / prescale / modulo)
     */
    static const struct { uint16_t prsc; uint32_t prescale; uint16_t val1;
                          uint32_t modulo; uint32_t root_div; } shapes[] = {
        /* root_div 4 => 528/4 = 132 MHz: THE EVK's ACTUAL Bus_Wakeup SETTING. */
        { 6, 64,  499,  1000, 4 },   /* the FOC-shaped carrier                  */
        { 6, 64,  249,   500, 4 },   /* half the modulo -> half the period      */
        { 7, 128, 499,  1000, 4 },   /* double the prescaler -> double the period */
        { 5, 32,  999,  2000, 4 },   /* different prescaler AND modulo          */

        /*
         * THE AXIS THE OLD GOLDEN COULD NOT EXPRESS: THE CLOCK ROOT ITSELF.
         * Same PWM registers, different root divider. A model that ignores the
         * clock tree -- which is what this model did until today -- reports an
         * IDENTICAL period for all three of these and fails.
         */
        { 6, 64,  499,  1000, 8  },  /* 66 MHz  -> period doubles vs div 4   */
        { 6, 64,  499,  1000, 16 },  /* 33 MHz  -> period doubles again      */
        { 6, 64,  499,  1000, 2  },  /* 264 MHz -> period halves vs div 4    */
    };
    int period_ok = 1;

    for (unsigned k = 0; k < sizeof(shapes) / sizeof(shapes[0]); k++) {
        /* Program the clock root FIRST -- the PWM must read it when it starts. */
        set_pwm_root_div(shapes[k].root_div);
        /*
         * DIVIDE ONCE, AND LAST. The obvious form,
         *     CPU_HZ / (PWM_HZ / prescale / modulo)
         * truncates in the INNER division: at prescale 128 it yields 192061 where
         * the true value is 192000. The MODEL was right and MY EXPECTED VALUE was
         * wrong -- and the +/-10% tolerance hid it, right up until -icount made
         * the measurement exact and the 61-cycle gap became visible.
         *
         * A GOLDEN IS A CLAIM TOO. A loose tolerance hides a broken golden exactly
         * as well as it hides a broken model, and you will blame the model.
         *
         * (Scaled to MHz to keep the product inside 32 bits: this is a freestanding
         * -nostdlib test, so a uint64_t divide would pull in __aeabi_uldivmod.)
         */
        uint32_t pwm_hz = SYS_PLL2_HZ / shapes[k].root_div;
        uint32_t expect = (shapes[k].prescale * shapes[k].modulo) *
                          (CPU_HZ / 1000000u) / (pwm_hz / 1000000u);

        /* Reprogram this shape: INIT = -(modulo/2), VAL1 = modulo/2 - 1. */
        PWM_MCTRL = 0;                                  /* stop  */
        SM0_INIT = (uint16_t)(-(int)(shapes[k].modulo / 2u));
        SM0_VAL1 = shapes[k].val1;
        SM0_CTRL = (uint16_t)(shapes[k].prsc << 4);
        PWM_MCTRL = MCTRL_LDOK;
        PWM_MCTRL = MCTRL_RUN & (1u << 8);

        SYST_RVR = 0x00FFFFFFu;
        SYST_CVR = 0;
        SYST_CSR = 0x5u;                                /* ENABLE | core clk */

        int s0 = reloads;
        while (reloads == s0) { }                       /* align to an edge  */
        uint32_t c0 = SYST_CVR;
        int from = reloads;
        while (reloads < from + (int)MEASURE_PERIODS) { }
        uint32_t c1 = SYST_CVR;

        uint32_t per = ((c0 - c1) & 0x00FFFFFFu) / MEASURE_PERIODS;

        puts_("  prescale "); dec(shapes[k].prescale);
        puts_(" modulo "); dec(shapes[k].modulo);
        puts_(": measured "); dec(per);
        puts_(" cycles, expected "); dec(expect);

        /*
         * SysTick wrap guard: refuse to report a number the instrument cannot
         * measure, rather than reporting a wrong one.
         */
        if (expect * MEASURE_PERIODS >= 0x01000000u) {
            puts_("  <-- WINDOW EXCEEDS SYSTICK RANGE (would wrap)\r\n");
            period_ok = 0;
            continue;
        }

        /*
         * TOLERANCE +/-1%, and it can be this tight ONLY because the harness runs
         * under -icount: virtual time is then derived from instructions retired,
         * not host wall-clock, and the measurement is BIT-EXACT run to run
         * (96000/96000, three runs, three times). Without -icount the same
         * measurement jitters (96711 / 96207 / 95848) and a tolerance wide enough
         * to absorb that noise is, by construction, too wide to detect the
         * accumulating-drift bug this test exists to catch.
         *
         * "Rung 3 requires a DETERMINISTIC instrument. A correct golden compared
         *  against a noisy measurement produces a confident, reproducible-looking,
         *  wrong answer." -- mcxn947qemu, after nearly diagnosing a real bug from
         *  a broken instrument, and then nearly un-diagnosing it when the noise
         *  flipped sign.
         */
        uint32_t lo = expect - expect / 100u;   /* +/-1% (needs -icount) */
        uint32_t hi = expect + expect / 100u;
        if (per < lo || per > hi) {
            period_ok = 0;
            puts_("  <-- MISMATCH\r\n");
        } else {
            puts_("  ok\r\n");
        }
    }
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
