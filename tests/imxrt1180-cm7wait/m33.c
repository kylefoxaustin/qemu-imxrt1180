/*
 * CM7 CPUWAIT gating test — Cortex-M33 (primary) side.
 *
 * The RT1180 M7 boot is a TWO-gate handshake (RM: "on POR the M7 is held in reset
 * AND CPUWAIT is high"): the M33 releases the reset (SRC.SCR.BT_RELEASE_M7) AND
 * clears CPUWAIT (M7_CFG.WAIT).  The core runs only once BOTH are done.  The SDK
 * Prepare_CM7 releases the reset with the image not yet copied and WAIT still high,
 * and the later MCMGR_StartCore clears WAIT once the image is in place -- so a model
 * that starts the M7 on the SCR write alone boots garbage.
 *
 * This test proves the gate:
 *   1. release the reset (SCR) with WAIT still HIGH  -> the M7 must stay HELD
 *      (SHARED_FLAG stays 0 across a long spin), then
 *   2. clear WAIT                                     -> the M7 runs and stamps the
 *      magic into SHARED_FLAG.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

#define BLK_M7_CFG (*(volatile uint32_t *)0x444F0080u)   /* BLK_CTRL_S_AONMIX     */
#define SRC_SCR    (*(volatile uint32_t *)0x44460010u)   /* SRC_GENERAL           */
#define SCR_BT_RELEASE_M7 0x1u
#define M7_CFG_WAIT       0x10u                           /* CPUWAIT (bit 4)       */
#define M7_IMAGE_ADDR     0x303C0000u                     /* M7 INITVTOR (its TCM) */

#define SHARED_FLAG  (*(volatile uint32_t *)0x20490000u)  /* OCRAM, seen by both   */
#define M7_RAN_MAGIC 0xCAFEBABEu
#define SPIN         8000000u

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
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    SHARED_FLAG = 0u;

    /* Gate 1 only: reset released, but CPUWAIT still HIGH -> M7 must NOT run. */
    BLK_M7_CFG = M7_IMAGE_ADDR | M7_CFG_WAIT;   /* INITVTOR set, WAIT=1 */
    SRC_SCR    = SCR_BT_RELEASE_M7;             /* release the reset   */

    int ran_while_held = 0;
    for (volatile uint32_t i = 0; i < SPIN; i++) {
        if (SHARED_FLAG == M7_RAN_MAGIC) { ran_while_held = 1; break; }
    }

    /* Gate 2: clear CPUWAIT -> the M7 now runs and stamps the magic. */
    BLK_M7_CFG = M7_IMAGE_ADDR;                 /* WAIT=0 */

    int ran_after_release = 0;
    for (volatile uint32_t i = 0; i < SPIN; i++) {
        if (SHARED_FLAG == M7_RAN_MAGIC) { ran_after_release = 1; break; }
    }

    if (!ran_while_held && ran_after_release) {
        puts_("CM7WAIT: PASS - M7 held while CPUWAIT high, ran only after WAIT cleared\r\n");
    } else if (ran_while_held) {
        puts_("CM7WAIT: FAIL - M7 ran while CPUWAIT was still high (WAIT gate ignored)\r\n");
    } else {
        puts_("CM7WAIT: FAIL - M7 never ran after CPUWAIT was cleared\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
