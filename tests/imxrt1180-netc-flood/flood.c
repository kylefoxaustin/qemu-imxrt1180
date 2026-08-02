/*
 * NETC switch BROADCAST-FLOOD test (Cortex-M33) — multi-physical-port flood path.
 *
 * The switch has a netdev on wire ports 0, 1 and 2 (each a point-to-point link to
 * the host).  The host (run.sh + flood.py) injects a BROADCAST frame on wire port 1
 * and asserts the switch FLOODS it out BOTH other wires (ports 0 and 2), while
 * split-horizon keeps it off the ingress wire (port 1).  This is the data-plane
 * mechanism cross-silicon beacon discovery relies on when the RT1180 switch sits
 * between two other silicon's Ethernet MACs -- distinct from netc-portfwd, which
 * exercises a LEARNED-UNICAST forward to a single port.
 *
 * This firmware only enables the switch's receive path (so the ports accept +
 * forward) and idles; all traffic + verification is host-side.
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
    sh(SYS_WRITE0,(void*)"FLOOD: switch up\r\n");
    for(;;){ __asm__ volatile("wfi"); }
}
