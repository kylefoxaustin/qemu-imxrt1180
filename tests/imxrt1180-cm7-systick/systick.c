/*
 * Bare-metal Cortex-M7 SysTick probe for the i.MX RT1180.
 *
 * DIAGNOSTIC (2026-09-18): the Zephyr ztest kernel suites halt on the cm7 boot
 * after the kernel's first SVC with ZERO SysTick ticks, while printf samples
 * run.  0 SysTicks is either the CAUSE (SysTick never fires on the M7, so a
 * tick-driven kernel idles forever) or a CONSEQUENCE (the guest halts before it
 * enables SysTick).  This probe decides it: a bare-metal M7 image programs
 * SysTick itself and reports whether the exception fires.
 *
 *   PASS : the SysTick handler ran      -> SysTick works on the M7; the ztest
 *                                          halt is kernel-path, not the timer.
 *   FAIL : no tick after a bounded spin -> SysTick does not fire on the M7 boot;
 *                                          that is the ztest blocker.
 *
 * Links like a real cm7 image (vectors+code in local ITCM @0x0, stack in local
 * DTCM @0x20000000), so the machine auto-detects it and boots the M7.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define ADP_Stopped_ApplicationExit 0x20026u

/* ARMv7-M SysTick (SysTick_Type @ 0xE000E010) */
#define SYST_CSR  (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define CSR_ENABLE    (1u << 0)
#define CSR_TICKINT   (1u << 1)
#define CSR_CLKSOURCE (1u << 2)   /* 1 = processor clock (cpuclk) */
#define CSR_COUNTFLAG (1u << 16)

#define M7_STACK_TOP  0x20040000u   /* top of the M7 local DTCM (256 KiB) */
/* A fixed DTCM cell used as the tick flag -- avoids .bss (the cm7 link discards it). */
#define TICK_FLAG     (*(volatile uint32_t *)0x2000F000u)

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }
static void exit_(void) { sh(SYS_EXIT, (void *)ADP_Stopped_ApplicationExit); for (;;) {} }

void m7_reset(void);
void systick_handler(void);
void default_handler(void);

/* Full ARMv7-M system-exception vector table (SysTick is vector 15). */
__attribute__((section(".vectors"), used))
void (*const vt[])(void) = {
    (void (*)(void))M7_STACK_TOP,  /* [0]  initial SP   */
    m7_reset,                      /* [1]  Reset        */
    default_handler,               /* [2]  NMI          */
    default_handler,               /* [3]  HardFault    */
    default_handler,               /* [4]  MemManage    */
    default_handler,               /* [5]  BusFault     */
    default_handler,               /* [6]  UsageFault   */
    0, 0, 0, 0,                    /* [7..10] reserved  */
    default_handler,               /* [11] SVCall       */
    default_handler,               /* [12] DebugMon     */
    0,                             /* [13] reserved     */
    default_handler,               /* [14] PendSV       */
    systick_handler,               /* [15] SysTick      */
};

void systick_handler(void)
{
    TICK_FLAG = TICK_FLAG + 1u;
}

void default_handler(void)
{
    puts_("\r\nUNEXPECTED EXCEPTION -> FAIL\r\n");
    exit_();
}

void m7_reset(void)
{
    puts_("\r\n=== RT1180 CM7 SysTick probe ===\r\n");

    TICK_FLAG = 0;

    /* Program SysTick: reload small so it wraps quickly, processor clock,
     * interrupt on wrap, enabled. */
    SYST_RVR = 0x1000u;            /* 4096-count period */
    SYST_CVR = 0;                  /* clear current -> reload on next tick */
    SYST_CSR = CSR_CLKSOURCE | CSR_TICKINT | CSR_ENABLE;
    asm volatile("dsb; isb" ::: "memory");

    puts_("SysTick armed (CSR=ENABLE|TICKINT|CLKSOURCE); waiting for a tick...\r\n");

    /* Bounded wait: if SysTick fires, TICK_FLAG goes non-zero long before this
     * exhausts. If it never fires, we FALL OUT and report FAIL rather than hang. */
    for (volatile uint64_t i = 0; i < 200000000ull; i++) {
        if (TICK_FLAG != 0u) {
            puts_("\r\nSYSTICK FIRED -> PASS (SysTick works on the M7)\r\n");
            exit_();
        }
    }

    puts_("\r\nNO SYSTICK after bounded spin -> FAIL (SysTick does not fire on the M7 boot)\r\n");
    exit_();
}
