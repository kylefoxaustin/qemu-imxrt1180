/*
 * Minimal bare-metal semihosting "hello" for the i.MX RT1180 Cortex-M33.
 *
 * No libc, no startup: just a two-entry vector table (initial SP + reset) and
 * a reset handler that prints via ARM semihosting (BKPT 0xAB) and exits.
 * Proves the QEMU mimxrt1180-evk machine boots the M33: reset vector fetch,
 * code execution from the FlexSPI NOR window, and SP into DTCM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define ADP_Stopped_ApplicationExit 0x20026u

static long semihost(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

void reset_handler(void);

/* Top of the 128 KiB DTCM (system TCM @ 0x20000000). */
#define STACK_TOP 0x20020000u

__attribute__((section(".vectors"), used))
void (* const vector_table[])(void) = {
    (void (*)(void))STACK_TOP,   /* [0] initial SP  */
    reset_handler,               /* [1] reset       */
};

void reset_handler(void)
{
    semihost(SYS_WRITE0,
             (void *)"\r\n=== Hello from i.MX RT1180 (Cortex-M33) on QEMU! ===\r\n");
    semihost(SYS_EXIT, (void *)ADP_Stopped_ApplicationExit);
    for (;;) {
    }
}
