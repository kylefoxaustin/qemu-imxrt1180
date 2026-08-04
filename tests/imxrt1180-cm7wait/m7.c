/*
 * CM7 CPUWAIT gating test — Cortex-M7 (secondary) side.
 *
 * When the M33 finally lets it run, the M7 stamps a magic word into shared OCRAM
 * and spins.  The M33 watches that word to prove the M7 stayed HELD while
 * M7_CFG.WAIT was high and only ran once WAIT was cleared.  It must NOT exit
 * (that would end the run before the M33 renders its verdict).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define M7_STACK_TOP 0x30440000u                 /* top of the CM7 TCM window   */
#define SHARED_FLAG  (*(volatile uint32_t *)0x20490000u)  /* OCRAM, seen by both */
#define M7_RAN_MAGIC 0xCAFEBABEu

void m7_reset(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))M7_STACK_TOP, m7_reset };

void m7_reset(void)
{
    SHARED_FLAG = M7_RAN_MAGIC;    /* "I am running" — only reachable after WAIT clears */
    for (;;) {
    }
}
