/*
 * USDHC presence test (Cortex-M33).
 *
 * Confirms both USDHC controllers respond with a real Host Controller
 * Capabilities value (the upstream imx-usdhc model), not the catch-all zero,
 * and that a scratch register round-trips.  Full SD/MMC transfers use a QEMU
 * SD card attached via -drive if=sd (see the board).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u
#define USDHC1 0x42850000u
#define USDHC2 0x42860000u
#define CAP(base) (*(volatile uint32_t *)((base) + 0x40))
#define DSADDR(base) (*(volatile uint32_t *)((base) + 0x00))
static long sh(long op,void*a){register long r0 asm("r0")=op;register void*r1 asm("r1")=a;asm volatile("bkpt 0xAB":"+r"(r0):"r"(r1):"memory");return r0;}
static void puts_(const char*s){sh(SYS_WRITE0,(void*)s);}
void reset_handler(void);
__attribute__((section(".vectors"),used)) void(*const vt[])(void)={[0]=(void(*)(void))STACK_TOP,[1]=reset_handler};
void reset_handler(void)
{
    int ok = 1;
    if (CAP(USDHC1) == 0 || CAP(USDHC1) == 0xFFFFFFFF) ok = 0;   /* real caps */
    if (CAP(USDHC2) == 0) ok = 0;
    DSADDR(USDHC1) = 0x20001000u;                                /* DMA sys addr */
    if ((DSADDR(USDHC1) & ~0x3u) != 0x20001000u) ok = 0;         /* round-trips */
    puts_(ok ? "USDHC: PASS - both controllers report caps + register round-trip\r\n"
             : "USDHC: FAIL\r\n");
    sh(SYS_EXIT,(void*)0x20026u); for(;;){}
}
