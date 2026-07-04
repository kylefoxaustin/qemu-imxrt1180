/*
 * Inter-core MU test — Cortex-M33 (MUA) side.
 *
 * Releases the Cortex-M7, sends a word through MU1 (MUA.TR[0]), and waits for
 * the M7 to echo it back +1 on MUA.RR[0].  A correct reply proves the whole
 * path: MUA.TR -> MUB.RR, the M7's MU receive IRQ firing + its ISR replying
 * MUB.TR -> MUA.RR.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18

#define BLK_M7_CFG (*(volatile uint32_t *)0x444F0080u) /* BLK_CTRL_S_AONMIX  */
#define SRC_SCR    (*(volatile uint32_t *)0x44460010u) /* SRC_GENERAL        */
#define M7_IMAGE_ADDR 0x303C0000u
#define STACK_TOP     0x20020000u

/* MUA = CM33 side of MU1 @ 0x4422_0000. */
#define MUA_RSR (*(volatile uint32_t *)0x4422012Cu)
#define MUA_TR0 (*(volatile uint32_t *)0x44220200u)
#define MUA_RR0 (*(volatile uint32_t *)0x44220280u)

#define MSG 0xCAFE0001u

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
    puts_("M33: releasing the Cortex-M7...\r\n");
    BLK_M7_CFG = M7_IMAGE_ADDR;
    SRC_SCR    = 0x1u;

    puts_("M33: sending 0xCAFE0001 to the M7 over MU1...\r\n");
    MUA_TR0 = MSG;

    volatile uint32_t timeout = 50000000u;
    while (!(MUA_RSR & 0x1u) && --timeout) {
    }
    if ((MUA_RSR & 0x1u) && MUA_RR0 == (MSG + 1u)) {
        puts_("M33: PASS - got 0xCAFE0002 back (MU messaging + M7 IRQ work)\r\n");
    } else {
        puts_("M33: FAIL - no/incorrect MU reply from the M7\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
