/*
 * NETC switch WIRE->WIRE routing test (Cortex-M33) — the multi-physical-port path.
 *
 * The switch has one netdev per wire port (port 1 <-> mcast G1, port 2 <-> mcast G2).
 * The host (run.sh + portfwd.py) drives it: it sends a frame with SRC=MAC_B into
 * port 2 (so the switch learns MAC_B is on port 2), then a frame DST=MAC_B into
 * port 1 -- which the switch must route OUT port 2's own netdev (G2), not port 1's.
 *
 * This firmware only brings the switch's receive path up (enable the RX ring so the
 * ports accept + forward frames) and idles; all traffic + verification is host-side.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#define STACK_TOP 0x20020000u
#define SI 0x60B00000u
#define RB_MR (*(volatile uint32_t *)(SI + 0x8100))   /* RX ring mode; EN=bit31 */
#define RB_MR_EN 0x80000000u
#define SYS_WRITE0 0x04
static long sh(long op,void*a){register long r0 asm("r0")=op;register void*r1 asm("r1")=a;asm volatile("bkpt 0xAB":"+r"(r0):"r"(r1):"memory");return r0;}
void reset_handler(void);
__attribute__((section(".vectors"),used)) void(*const vt[])(void)={[0]=(void(*)(void))STACK_TOP,[1]=reset_handler};
void reset_handler(void){
    RB_MR = RB_MR_EN;                 /* ports now accept + forward frames */
    sh(SYS_WRITE0,(void*)"PORTFWD: switch up\r\n");
    for(;;){ __asm__ volatile("wfi"); }
}
