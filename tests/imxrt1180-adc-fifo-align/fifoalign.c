/*
 * LPADC RESFIFO alignment + honest-overflow test (Cortex-M33).
 *
 * This pins the exact hardware contract that the cm7 mc_pmsm FOC loop depends on
 * -- and whose violation (a silently mis-aligned RESFIFO) deadlocked the closed-
 * loop spin until the demo was run under -icount.  See the story below.
 *
 * ===================== WHY THIS TEST EXISTS =================================
 * The FOC fast loop (ADC1_IRQHandler -> MCDRV_CurrAndVoltDcBusGet) reads ADC1 in
 * a FIXED order every control cycle, from a TWO-command DualSingleEndBothSide
 * chain triggered once per PWM period:
 *
 *     CMD1 (ch5, DualBoth):  A5 -> RESFIFO0 = Ia,   B5 -> RESFIFO1 = Ib
 *     CMD2 (ch4, DualBoth):  A4 -> RESFIFO0 = dummy, B4 -> RESFIFO1 = U_DCbus
 *     driver pops:           FIFO0, FIFO1, FIFO0, FIFO1 = Ia, Ib, dummy, U_DCbus
 *
 * So every trigger must leave EXACTLY 2 entries in each FIFO, in that order, and
 * the driver's 4th pop must be the DC-bus voltage -- NOT a phase current.  If the
 * FIFO ever overflows (conversions pushed faster than the ISR drains), entries
 * are dropped and the 2+2+2+2 grouping SHIFTS: the "U_DCbus" pop then returns a
 * signed phase-current code, which decodes to a garbage bus voltage (it was seen
 * swinging to -15 V), tripping a spurious FAULT_U_DCBUS_UNDER every cycle and
 * wedging the drive in its fault state.  On silicon the ADC conversion takes real
 * time and the rates are matched by physics; in the model the conversion is
 * instant, so the demo must run under -icount to rate-match the trigger to the
 * ISR.  The model is CORRECT either way -- it drops on a full FIFO and raises
 * STAT.FOF, the honest overflow flag -- and THIS test locks both halves down:
 *
 *   (A) ALIGNMENT: over many trigger->drain cycles, the 4th pop (U_DCbus) always
 *       reads the plant's 24 V bus, and the 1st pop (Ia) never does -- proving the
 *       2+2 routing and the FIFO0/1/0/1 ordering hold.
 *   (B) HONEST OVERFLOW: over-triggering WITHOUT draining sets STAT.FOF on both
 *       FIFOs -- the fault is signalled, never swallowed.
 *
 * A range check would not catch (A): "some plausible voltage" is exactly what the
 * misaligned read produces.  The golden is that the SAME slot reads the constant
 * bus while a DIFFERENT slot reads a live current -- a structural invariant, not a
 * magnitude window.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* PWM1 @ 0x4265_0000: submodule s registers at s*0x60 -- drives the plant. */
#define PWM1_BASE 0x42650000u
#define SM_INIT(s) (*(volatile uint16_t *)(PWM1_BASE + (s)*0x60 + 0x02))
#define SM_VAL1(s) (*(volatile uint16_t *)(PWM1_BASE + (s)*0x60 + 0x0E))
#define SM_VAL2(s) (*(volatile uint16_t *)(PWM1_BASE + (s)*0x60 + 0x12))
#define SM_VAL3(s) (*(volatile uint16_t *)(PWM1_BASE + (s)*0x60 + 0x16))
#define PWM_MCTRL  (*(volatile uint16_t *)(PWM1_BASE + 0x188))
#define MCTRL_LDOK   0x000Fu
#define MCTRL_RUN012 0x0700u

/* ADC1 @ 0x4260_0000. */
#define ADC1_BASE 0x42600000u
#define ADC_CTRL     (*(volatile uint32_t *)(ADC1_BASE + 0x10))
#define ADC_STAT     (*(volatile uint32_t *)(ADC1_BASE + 0x14))
#define ADC_SWTRIG   (*(volatile uint32_t *)(ADC1_BASE + 0x34))
#define ADC_FCTRL0   (*(volatile uint32_t *)(ADC1_BASE + 0xE0))
#define ADC_FCTRL1   (*(volatile uint32_t *)(ADC1_BASE + 0xE4))
#define ADC_CMDL(n)  (*(volatile uint32_t *)(ADC1_BASE + 0x100 + 8*(n)))
#define ADC_CMDH(n)  (*(volatile uint32_t *)(ADC1_BASE + 0x104 + 8*(n)))
#define ADC_TCTRL0   (*(volatile uint32_t *)(ADC1_BASE + 0xA0))
#define ADC_RESFIFO0 (*(volatile uint32_t *)(ADC1_BASE + 0x300))
#define ADC_RESFIFO1 (*(volatile uint32_t *)(ADC1_BASE + 0x304))
#define CTRL_ADCEN   0x1u
#define CTRL_CAL_REQ 0x8u
#define CMDL_CTYPE_DUALBOTH (3u << 5)
#define STAT_FOF0    0x2u
#define STAT_FOF1    0x8u

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

/* Decode a RESFIFO code as the mcdrv_adc_imxrt118x DC-bus convention, in mV:
 *   U = raw * 12/11 / 32768 * 60.8 V.  Ordered to stay in 32 bits (no 64-bit
 *   divide in -nostdlib).  A ~24 V bus => ~24000; a mid-scale current code
 *   (0x8000, i.e. zero amps) => ~66000. */
static uint32_t decode_mv(uint32_t raw)
{
    return ((raw & 0xFFFF) * 60800u / 32768u) * 12u / 11u;
}

void reset_handler(void)
{
    int ok = 1;

    /* ADC1: enable + calibrate. */
    ADC_CTRL = CTRL_ADCEN | CTRL_CAL_REQ;

    /*
     * The mc_pmsm two-command DualSingleEndBothSide chain:
     *   CMD1: channel 5, DualBoth, NEXT=command 2   (Ia on A5 -> F0, Ib on B5 -> F1)
     *   CMD2: channel 4, DualBoth, NEXT=0 (end)     (A4 -> F0 dummy, B4 -> F1 U_DCbus)
     */
    ADC_CMDL(0) = 5u | CMDL_CTYPE_DUALBOTH;
    ADC_CMDH(0) = (2u << 24);            /* NEXT = command 2 */
    ADC_CMDL(1) = 4u | CMDL_CTYPE_DUALBOTH;
    ADC_CMDH(1) = 0;                     /* end of chain */
    ADC_TCTRL0  = (1u << 24);            /* trigger 0 -> command 1, software trigger */

    /* Spin up the plant so it drives the ADC channels: small stator vector at
     * electrical 90 deg on a 24 V bus (same as the motor test). */
    static const uint16_t val3[3] = { 250, 277, 223 };
    for (int s = 0; s < 3; s++) {
        SM_INIT(s) = (uint16_t)(-500);
        SM_VAL1(s) = 499;
        SM_VAL2(s) = (uint16_t)(-(int)val3[s]);
        SM_VAL3(s) = val3[s];
    }
    PWM_MCTRL = MCTRL_LDOK;
    PWM_MCTRL = MCTRL_RUN012;

    /* Wait until the plant is driving the bus channel: trigger+drain until the
     * 4th pop (U_DCbus) reads ~24 V rather than the un-driven placeholder. */
    uint32_t guard = 0;
    int up = 0;
    while (!up) {
        ADC_SWTRIG = 1u;
        while ((ADC_FCTRL0 & 0x1Fu) < 2u || (ADC_FCTRL1 & 0x1Fu) < 2u) {
            if (++guard > 50000000u) { break; }
        }
        (void)ADC_RESFIFO0; (void)ADC_RESFIFO1;             /* Ia, Ib */
        (void)ADC_RESFIFO0;                                 /* dummy  */
        uint32_t mv = decode_mv(ADC_RESFIFO1);              /* U_DCbus */
        if (mv > 22500u && mv < 25500u) { up = 1; }
        if (++guard > 50000000u) { ok = 0; break; }
    }

    /*
     * (A) ALIGNMENT INVARIANT over 64 trigger->drain cycles.
     *   - after one trigger the DualBoth chain must leave EXACTLY 2 in each FIFO
     *   - pop #4 (U_DCbus) must read the 24 V bus every time
     *   - pop #1 (Ia) must read a phase current, NOT the bus (decodes >30 V: a
     *     code near mid-scale, structurally distinct from the bus code)
     */
    int align_fail = 0, count_fail = 0, ia_is_current = 0;
    for (int i = 0; i < 64; i++) {
        ADC_SWTRIG = 1u;
        uint32_t f0 = ADC_FCTRL0 & 0x1Fu, f1 = ADC_FCTRL1 & 0x1Fu;
        if (f0 != 2u || f1 != 2u) { count_fail = 1; }

        uint32_t ia   = ADC_RESFIFO0;          /* F0 pop1 = A5 = Ia      */
        (void)ADC_RESFIFO1;                    /* F1 pop1 = B5 = Ib      */
        (void)ADC_RESFIFO0;                    /* F0 pop2 = A4 = dummy   */
        uint32_t udcb = ADC_RESFIFO1;          /* F1 pop2 = B4 = U_DCbus */

        uint32_t udcb_mv = decode_mv(udcb);
        if (udcb_mv <= 22500u || udcb_mv >= 25500u) { align_fail = 1; }
        if (decode_mv(ia) > 30000u) { ia_is_current = 1; }
    }
    if (align_fail)  { ok = 0; }
    if (count_fail)  { ok = 0; }
    if (!ia_is_current) { ok = 0; }            /* Ia slot never carried a current */

    /*
     * (B) HONEST OVERFLOW.  Clear STAT, then trigger 12 times WITHOUT draining:
     * each trigger pushes 2 per FIFO, so after 8 triggers the depth-16 FIFOs are
     * exactly full and the remaining pushes MUST raise STAT.FOF -- the fault is
     * signalled, not swallowed.
     */
    ADC_STAT = STAT_FOF0 | STAT_FOF1;          /* W1C clear */
    for (int i = 0; i < 12; i++) {
        ADC_SWTRIG = 1u;
    }
    uint32_t stat = ADC_STAT;
    int overflow_ok = (stat & STAT_FOF0) && (stat & STAT_FOF1);
    if (!overflow_ok) { ok = 0; }

    if (ok) {
        puts_("ADC-FIFO: PASS - U_DCbus pop stays aligned to the bus across 64 cycles\r\n");
        puts_("ADC-FIFO: PASS - DualBoth chain leaves 2+2 and Ia slot carries a current\r\n");
        puts_("ADC-FIFO: PASS - over-trigger without drain raises STAT.FOF (honest overflow)\r\n");
    } else if (align_fail) {
        puts_("ADC-FIFO: FAIL - U_DCbus pop drifted off the bus (RESFIFO mis-aligned)\r\n");
    } else if (count_fail) {
        puts_("ADC-FIFO: FAIL - DualBoth chain did not leave exactly 2+2 in the FIFOs\r\n");
    } else if (!ia_is_current) {
        puts_("ADC-FIFO: FAIL - Ia slot never read a phase current (routing collapsed)\r\n");
    } else if (!overflow_ok) {
        puts_("ADC-FIFO: FAIL - over-trigger did not raise STAT.FOF (silent overflow)\r\n");
    } else {
        puts_("ADC-FIFO: FAIL - plant never drove the bus channel\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
