/*
 * Bare-metal Cortex-M7 MPU region-count check for the i.MX RT1180.
 *
 * The MIMXRT1189 Cortex-M7 has a 16-region MPU (MPU_TYPE.DREGION == 16, as on
 * the RT1170 M7). QEMU's generic cortex-m7 defaults to 8, which is a wrong
 * register COUNT -- it silently under-reports the MPU. That is not cosmetic: a
 * CONFIG_USERSPACE guest (Zephyr's ztest kernel suites) computes too few dynamic
 * MPU regions from DREGION, the board's static regions leave no room, and
 * k_mem_domain_init() fails -> kernel PANIC before the console is up (a silent
 * hang). soc.c forwards the real count to the M7 via the armv7m mpu-ns-regions
 * property; this test pins it.
 *
 *   PASS : MPU_TYPE.DREGION == 16
 *   FAIL : anything else (mutation: drop the soc.c override -> reads 8 -> FAIL)
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

/* ARMv7-M MPU Type Register (MPU_TYPE @ 0xE000ED90); DREGION = bits [15:8]. */
#define MPU_TYPE   (*(volatile uint32_t *)0xE000ED90u)
#define EXPECT_DREGION 16u

#define M7_STACK_TOP  0x20040000u

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }
static void exit_(void) { sh(SYS_EXIT, (void *)ADP_Stopped_ApplicationExit); for (;;) {} }

/* Emit a one-hex-digit count so a wrong value is visible, not just "FAIL". */
static void put_nibble(unsigned v)
{
    char c = (v < 10) ? (char)('0' + v) : (char)('a' + (v - 10));
    char s[2] = { c, 0 };
    puts_(s);
}

void m7_reset(void);
__attribute__((section(".vectors"), used))
void (*const vt[])(void) = { (void (*)(void))M7_STACK_TOP, m7_reset };

void m7_reset(void)
{
    unsigned dregion = (MPU_TYPE >> 8) & 0xFFu;

    puts_("\r\n=== RT1180 CM7 MPU region-count check ===\r\nMPU_TYPE.DREGION = 0x");
    put_nibble((dregion >> 4) & 0xF);
    put_nibble(dregion & 0xF);
    puts_("\r\n");

    if (dregion == EXPECT_DREGION) {
        puts_("CM7-MPU: PASS - 16 MPU regions (matches MIMXRT1189 silicon)\r\n");
    } else {
        puts_("CM7-MPU: FAIL - wrong MPU region count (Zephyr CONFIG_USERSPACE will panic)\r\n");
    }
    exit_();
}
