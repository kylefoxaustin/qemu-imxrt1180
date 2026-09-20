/*
 * i.MX RT1180 — Cortex-M33 WFI wake-from-interrupt probe (SAFE-PATH only).
 *
 * SCOPE, stated precisely after rt1180renode measured it on tlib (2026-09-20):
 * this test exercises the path where an interrupt arrives AFTER the core has
 * already parked in WFI. Both QEMU and tlib handle that correctly (measured:
 * core enters exception entry with wfi=0). It DOES NOT reach the window of
 * rt1180renode's tlib "defect #14" — where the interrupt is pending AT THE MOMENT
 * WFI executes, armed-and-slept within one translation block, so entry happens
 * with wfi=1 and (on tlib) the core sleeps inside its own handler. Settling
 * whether that window bites the M33 needs a DIFFERENT test (pre-satisfied source
 * + enable-and-WFI with no block boundary between them; NOT via cpsie i, which
 * ends a block). So: this is a valid WFI-wake regression + the positive-control
 * half of the #14 scope question, NOT the discriminating half. Do not read a PASS
 * here as "M33 is unaffected by #14".
 *
 * Why the original design missed the window: the -icount + large-reload trick
 * below guarantees the wake fires AFTER WFI — which on QEMU avoids a false PASS
 * (QEMU's WFI consults pending and no-ops if one is already set), but on tlib is
 * exactly the SAFE path, because tlib's HELPER(wfi) sets wfi=1 UNCONDITIONALLY
 * and pending-at-WFI is #14's TRIGGER, not a false-pass. The WFI semantics differ
 * across the two cores; this test's guarantee is correct for what it tests and
 * silent on what it doesn't.
 *
 * WAKE SOURCE = SysTick, an M-profile core peripheral present on both QEMU and
 * tlib (no LPIT/GPT/TMR dependency), taken through the do_interrupt_v7m path.
 *
 * THE -icount GUARANTEE: WFI warps virtual time to the next timer deadline
 * deterministically, and a SysTick reload comfortably larger than the setup path
 * makes WFI reached first, so the wake lands on the parked core (the safe path
 * this test covers).
 *
 * EXTERNAL-READABLE ORACLE (rt1180renode's ask: from the console alone "probe
 * hung" and "core hung" are the same observation, so stamp DISTINCTIVE VALUES at
 * FIXED addresses a harness/monitor/gdb can read without symbols):
 *
 *   0x20010000  HANDLER marker   0x14C0FFEE = ISR body executed
 *   0x20010004  RESUME  marker   0x9E500DED = main resumed past WFI
 *               both are pre-stamped 0xBADF11A6 ("not filled") BEFORE arming, so a
 *               stale value cannot masquerade as a result.
 *
 *   READ FROM OUTSIDE after the run (or on a hang):
 *     0x20010000==0x14C0FFEE && 0x20010004==0x9E500DED  -> correct (also prints PASS)
 *     0x20010000==0xBADF11A6 (handler never ran), core hung:
 *          frame pushed on the core?  yes -> defect-#14 shape (woke into handler, 0 instr)
 *                                     no  -> never woke
 *     0x20010000==0x14C0FFEE, 0x20010004==0xBADF11A6 -> ran handler, never returned to main
 *
 * Console mirrors it: "armed, entering WFI" then "PASS" only if both magics landed.
 *
 * Links to CM33 code TCM (0x0FFE0000) — NOT the FlexSPI XIP window, which is
 * rt1180emulator's permissive-map axis — so it runs on both tools.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

/* --- semihosting (target=native) --- */
#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

/* --- SysTick (ARMv8-M core peripheral) --- */
#define SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018u)
#define CSR_ENABLE    (1u << 0)
#define CSR_TICKINT   (1u << 1)
#define CSR_CLKSOURCE (1u << 2)   /* processor clock (deterministic under -icount) */

/* Fixed, externally-readable markers in DTCM (64 KiB in — clear of both the tiny
 * .data/.bss at 0x20000000 and the stack descending from 0x20020000). */
#define MARK_HANDLED (*(volatile uint32_t *)0x20010000u)
#define MARK_RESUMED (*(volatile uint32_t *)0x20010004u)
#define MAGIC_HANDLED 0x14C0FFEEu   /* ISR body executed        */
#define MAGIC_RESUMED 0x9E500DEDu   /* main resumed past WFI    */
#define MARK_NOTYET   0xBADF11A6u   /* pre-armed sentinel       */

void systick_handler(void)
{
    SYST_CSR = 0;                  /* one-shot: disarm so we don't re-enter */
    MARK_HANDLED = MAGIC_HANDLED;  /* stamp BEFORE returning */
}

void reset_handler(void);

/* Vector table: [0]=MSP, [1]=reset, [15]=SysTick (exception 15). */
__attribute__((section(".vectors"), used))
void (*const vt[16])(void) = {
    [0]  = (void (*)(void))0x20020000u,   /* initial SP (DTCM top) */
    [1]  = reset_handler,
    [15] = systick_handler,
};

void reset_handler(void)
{
    MARK_HANDLED = MARK_NOTYET;
    MARK_RESUMED = MARK_NOTYET;

    /*
     * Arm SysTick to fire well AFTER we reach WFI. 0x4000 processor cycles is
     * far more than the handful of instructions between here and the wfi, so
     * under -icount WFI is always entered first, then time warps to the reload.
     */
    SYST_RVR = 0x4000u;
    SYST_CVR = 0u;                                   /* clear current + COUNTFLAG */
    SYST_CSR = CSR_ENABLE | CSR_TICKINT | CSR_CLKSOURCE;

    puts_("WFI-TEST: armed, entering WFI\r\n");
    asm volatile("wfi" ::: "memory");
    MARK_RESUMED = MAGIC_RESUMED;

    if (MARK_HANDLED == MAGIC_HANDLED && MARK_RESUMED == MAGIC_RESUMED) {
        puts_("WFI-TEST: PASS - SysTick woke the core from WFI, handler ran, main resumed\r\n");
    } else if (MARK_RESUMED == MAGIC_RESUMED) {
        /* Resumed past WFI but the handler body never ran — unexpected on a
         * correct core; would indicate a wake without the exception being taken. */
        puts_("WFI-TEST: FAIL - resumed past WFI but handler never ran\r\n");
    } else {
        puts_("WFI-TEST: FAIL - unreachable\r\n");
    }

    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) { }
}
