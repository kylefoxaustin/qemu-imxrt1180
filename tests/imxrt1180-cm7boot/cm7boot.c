/*
 * Standalone Cortex-M7 boot test.
 *
 * Proves the machine's opt-in `boot-cm7` path end to end: a cm7 `-kernel` image
 * (vector table linked to the M7's local ITCM @0x0, stack in local DTCM
 * @0x20000000) is auto-detected, the M7 is powered and the M33 held, the image
 * is loaded into the M7 TCM, and the M7 re-reads its now-populated ITCM vector
 * at machine reset.  If any of that is wrong the M7 never reaches this code (it
 * reads SP/PC = 0 and locks up), so no PASS is printed.
 *
 * Beyond "it ran", it checks the three regions of the M7's per-core view are
 * each correct, so a mutation that breaks ONE of them turns PASS into FAIL:
 *   (1) it is executing         -> booted from the M7's own ITCM vector
 *   (2) ITCM[0] == our SP        -> the vector table really sits at local 0x0
 *   (3) DTCM read/write          -> local 0x20000000 is real RAM
 *   (4) ADC1 VERID via backgnd   -> off-TCM peripherals are reachable and read
 *                                   their real reset value through the M7 view
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18

#define M7_STACK_TOP  0x20040000u   /* top of the M7 local DTCM (256 KiB) */

/* Off-TCM peripheral reached only through the M7 view's background alias.
 * ADC1 VERID (RM 80.6.2) resets to 0x0200_2C1B on the MIMXRT1189. */
#define ADC1_VERID        (*(volatile uint32_t *)0x42600000u)
#define ADC1_VERID_EXPECT 0x02002C1Bu

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }

void m7_reset(void);
__attribute__((section(".vectors"), used))
void (*const vt[])(void) = { (void (*)(void))M7_STACK_TOP, m7_reset };

void m7_reset(void)
{
    int ok = 1;

    /* (2) The vector table is at local ITCM 0x0 -- exactly what the M7's reset
     *     read.  The ITCM overlay (priority 1) must win over the background. */
    volatile uint32_t *itcm = (volatile uint32_t *)0x00000000u;
    if (itcm[0] != M7_STACK_TOP) {
        ok = 0;
    }

    /* (3) Local DTCM @0x20000000 is real, writable RAM. */
    volatile uint32_t *dtcm = (volatile uint32_t *)0x20000000u;
    dtcm[0] = 0xA5A5A5A5u;
    dtcm[1] = 0x5A5A5A5Au;
    if (dtcm[0] != 0xA5A5A5A5u || dtcm[1] != 0x5A5A5A5Au) {
        ok = 0;
    }

    /* (4) The background alias reaches the SoC's peripherals. */
    if (ADC1_VERID != ADC1_VERID_EXPECT) {
        ok = 0;
    }

    if (ok) {
        puts_("CM7BOOT: PASS - M7 booted its own ITCM vector; "
              "ITCM/DTCM/background view all live\r\n");
    } else {
        puts_("CM7BOOT: FAIL - M7 per-core view wrong (ITCM/DTCM/background)\r\n");
    }
    sh(SYS_EXIT, (void *)(unsigned long)(ok ? 0x20026u : 1u));
    for (;;) {
    }
}
