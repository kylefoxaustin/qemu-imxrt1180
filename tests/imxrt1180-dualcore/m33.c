/*
 * Dual-core release test — Cortex-M33 (primary) side.
 *
 * The M33 sets the Cortex-M7's initial vector table (BLK_CTRL_S_AONMIX.M7_CFG)
 * and releases it (SRC_GENERAL.SCR.BT_RELEASE_M7), exactly like the SDK
 * Prepare_CM7().  The M7 image is loaded at 0x303C0000 (its TCM, system view).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04

#define BLK_M7_CFG (*(volatile uint32_t *)0x444F0080u) /* BLK_CTRL_S_AONMIX */
#define SRC_SCR    (*(volatile uint32_t *)0x44460010u) /* SRC_GENERAL       */
#define SCR_BT_RELEASE_M7 0x1u
#define M7_IMAGE_ADDR     0x303C0000u   /* 128-byte aligned M7 vector base   */

#define STACK_TOP 0x20020000u

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

void reset_handler(void);

__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    sh(SYS_WRITE0, (void *)"M33: booting; releasing the Cortex-M7...\r\n");

    BLK_M7_CFG = M7_IMAGE_ADDR;          /* M7 INITVTOR = 0x303C0000 */
    SRC_SCR    = SCR_BT_RELEASE_M7;      /* release the M7           */

    sh(SYS_WRITE0, (void *)"M33: M7 released; M33 continues.\r\n");
    for (;;) {
    }
}
