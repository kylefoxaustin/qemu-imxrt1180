/*
 * Dual-core release test — Cortex-M7 (secondary) side.
 * Released by the M33; prints via semihosting then exits (ends the run).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define M7_STACK_TOP 0x30440000u   /* top of the CM7 TCM window (system view) */
static long sh(long op, void *arg) {
    register long r0 asm("r0") = op; register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory"); return r0;
}
void m7_reset(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))M7_STACK_TOP, m7_reset };
void m7_reset(void) {
    sh(SYS_WRITE0, (void *)"M7:  alive! Cortex-M7 running, released by the M33.\r\n");
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {}
}
