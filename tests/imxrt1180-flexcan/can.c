/*
 * FlexCAN loopback test (Cortex-M33).
 *
 * Puts FlexCAN1 in internal loopback (CTRL1.LPB), transmits a data frame from
 * message buffer 0, and checks it is received into message buffer 1 with the
 * correct payload + the RX interrupt flag set.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* FlexCAN1 @ 0x443A_0000. */
#define CAN 0x443A0000u
#define R32(off) (*(volatile uint32_t *)(CAN + (off)))
#define MCR      R32(0x000)
#define CTRL1    R32(0x004)
#define RXMGMASK R32(0x010)
#define IFLAG1   R32(0x030)
/* Message buffer n: CS @ 0x80+n*0x10, ID +4, DATA0 +8, DATA1 +12. */
#define MB(n, w) R32(0x080 + (n) * 0x10 + (w) * 4)

#define CODE_RX_EMPTY 0x4u
#define CODE_TX_DATA  0xCu
#define CTRL1_LPB     0x1000u

#define TX_D0 0xDEADBEEFu
#define TX_D1 0xCAFEBABEu

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
    MCR = 0x1F;                 /* MAXMB=31, MDIS/FRZ/HALT=0 -> operational */
    CTRL1 = CTRL1_LPB;          /* internal loopback */
    RXMGMASK = 0;               /* accept any ID */

    MB(1, 0) = CODE_RX_EMPTY << 24;   /* MB1 armed to receive */

    MB(0, 1) = 0x123u << 18;          /* std ID 0x123 */
    MB(0, 2) = TX_D0;
    MB(0, 3) = TX_D1;
    MB(0, 0) = (CODE_TX_DATA << 24) | (8u << 16);  /* CS write -> transmit */

    uint32_t iflag = IFLAG1;
    uint32_t rd0 = MB(1, 2), rd1 = MB(1, 3);

    if ((iflag & 0x2u) && rd0 == TX_D0 && rd1 == TX_D1) {
        puts_("FlexCAN: PASS - loopback frame received (data + RX IFLAG)\r\n");
    } else {
        puts_("FlexCAN: FAIL - loopback frame not received correctly\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
