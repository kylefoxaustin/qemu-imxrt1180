/*
 * Bare-metal ARMv8-M UsageFault-delivery probe for the i.MX RT1180 Cortex-M33.
 *
 * Zephyr's tests/arch/arm/arm_interrupt deliberately executes `udf #90`
 * (encoding 0xde5a) and validates that the core delivers a UsageFault
 * (UFSR.UNDEFINSTR) to the guest handler.  The rt1180renode sibling measured
 * that Renode's Cortex-M33 core LOCKS UP on this (PC=0xEFFFFFFE, HFSR.FORCED)
 * instead of delivering the fault, while its Cortex-M7 passes.
 *
 * This probe asks the same architectural question of QEMU's Cortex-M33, with
 * NO Zephyr / external-memory / VTOR harness -- it links straight to the CM33
 * code TCM.  It distinguishes three outcomes unambiguously:
 *   PASS   : the UsageFault handler runs with UFSR.UNDEFINSTR set  (correct)
 *   ESCAL  : the HardFault handler runs (fault escalated, not delivered)
 *   LOCKUP : QEMU aborts with "Lockup" (matches Renode's CM33 defect)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define ADP_Stopped_ApplicationExit 0x20026u

/* System Control Block (ARMv8-M) */
#define SCB_SHCSR (*(volatile uint32_t *)0xE000ED24u)  /* System Handler Ctrl/State */
#define SCB_CFSR  (*(volatile uint32_t *)0xE000ED28u)  /* Configurable Fault Status */
#define SHCSR_USGFAULTENA (1u << 18)
#define CFSR_UNDEFINSTR   (1u << 16)  /* UFSR.UNDEFINSTR (UFSR is CFSR[31:16]) */

#define STACK_TOP 0x20020000u  /* top of the 128 KiB system TCM (DTCM @0x20000000) */

void reset_handler(void);
void usagefault_handler(void);
void hardfault_handler(void);
void default_handler(void);

/* Full ARMv8-M system-exception vector table (indices 0..15). */
__attribute__((section(".vectors"), used))
void (* const vector_table[])(void) = {
    (void (*)(void))STACK_TOP,   /* [0]  initial SP        */
    reset_handler,               /* [1]  Reset             */
    default_handler,             /* [2]  NMI               */
    hardfault_handler,           /* [3]  HardFault         */
    default_handler,             /* [4]  MemManage         */
    default_handler,             /* [5]  BusFault          */
    usagefault_handler,          /* [6]  UsageFault        */
    default_handler,             /* [7]  SecureFault       */
    default_handler,             /* [8]  reserved          */
    default_handler,             /* [9]  reserved          */
    default_handler,             /* [10] reserved          */
    default_handler,             /* [11] SVCall            */
    default_handler,             /* [12] DebugMonitor      */
    default_handler,             /* [13] reserved          */
    default_handler,             /* [14] PendSV            */
    default_handler,             /* [15] SysTick           */
};

static long semihost(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

static void put(const char *s) { semihost(SYS_WRITE0, (void *)s); }

static void exit_now(void)
{
    semihost(SYS_EXIT, (void *)ADP_Stopped_ApplicationExit);
    for (;;) {
    }
}

/* Reached only if the core delivers the UsageFault to the guest handler. */
void usagefault_handler(void)
{
    if (SCB_CFSR & CFSR_UNDEFINSTR) {
        put("\r\nUFAULT-DELIVERED UFSR.UNDEFINSTR=1 -> PASS\r\n");
    } else {
        put("\r\nUFAULT-DELIVERED but UNDEFINSTR=0 -> UNEXPECTED\r\n");
    }
    exit_now();
}

/* Reached if the fault escalated instead of being delivered as UsageFault. */
void hardfault_handler(void)
{
    put("\r\nESCALATED-TO-HARDFAULT -> FAIL (fault not delivered as UsageFault)\r\n");
    exit_now();
}

void default_handler(void)
{
    put("\r\nUNEXPECTED-EXCEPTION -> FAIL\r\n");
    exit_now();
}

void reset_handler(void)
{
    put("\r\n=== RT1180 CM33 UsageFault-delivery probe ===\r\n");

    /* Enable UsageFault so a UNDEFINSTR is delivered to vector [6], not
     * escalated straight to HardFault. (Without this, ARMv8-M escalates.) */
    SCB_SHCSR |= SHCSR_USGFAULTENA;
    asm volatile("dsb; isb" ::: "memory");

    put("about to execute udf #90 (0xde5a)...\r\n");
    asm volatile(".short 0xde5a");   /* udf #90 -- the exact Zephyr stimulus */

    /* If we return here, the core silently swallowed the undefined instruction. */
    put("\r\nRETURNED-FROM-UDF -> FAIL (no fault taken at all)\r\n");
    exit_now();
}
