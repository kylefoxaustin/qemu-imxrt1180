/*
 * USB PHY + OTG controller test (Cortex-M33).
 *
 * Exercises the two i.MX RT1180 USB bring-up models the way the SDK USB stack
 * does, without the full device stack:
 *   1. USBPHY1: program the PHY PLL (power + enable USB clocks) and poll
 *      PLL_SIC for PLL_LOCK — the model reports lock once powered.
 *   2. USB_OTG1 (ChipIdea): soft-reset the controller (USBCMD.RST) and confirm
 *      it self-clears; read DCCPARAMS and confirm it reports device capability
 *      (DC) and 8 endpoints (DEN); start the controller (USBCMD.RS) and confirm
 *      the run bit reads back.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* USBPHY1 @ 0x42CA_0000. */
#define USBPHY1_BASE   0x42CA0000u
#define PHY_CTRL_CLR   (*(volatile uint32_t *)(USBPHY1_BASE + 0x38))
#define PHY_PLL_SIC    (*(volatile uint32_t *)(USBPHY1_BASE + 0xA0))
#define PHY_PLL_SIC_SET (*(volatile uint32_t *)(USBPHY1_BASE + 0xA4))
#define CTRL_SFTRST        0x80000000u
#define PLL_SIC_POWER      0x00001000u
#define PLL_SIC_EN_CLKS    0x00000040u
#define PLL_SIC_LOCK       0x80000000u

/* USB_OTG1 @ 0x42C8_0000 (ChipIdea). */
#define USB1_BASE      0x42C80000u
#define USB_DCCPARAMS  (*(volatile uint32_t *)(USB1_BASE + 0x124))
#define USB_USBCMD     (*(volatile uint32_t *)(USB1_BASE + 0x140))
#define USBCMD_RS      0x00000001u
#define USBCMD_RST     0x00000002u
#define DCCPARAMS_DEN  0x0000001Fu
#define DCCPARAMS_DC   0x00000080u

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
void (* const vt[])(void) = {
    [0] = (void (*)(void))STACK_TOP,
    [1] = reset_handler,
};

void reset_handler(void)
{
    int ok = 1;

    /* 1. USB PHY PLL bring-up. */
    PHY_CTRL_CLR = CTRL_SFTRST;           /* release PHY soft reset */
    PHY_PLL_SIC_SET = PLL_SIC_POWER;      /* power the PLL */
    PHY_PLL_SIC_SET = PLL_SIC_EN_CLKS;    /* enable USB clocks */
    uint32_t spins = 0;
    while (0u == (PHY_PLL_SIC & PLL_SIC_LOCK)) {
        if (++spins > 1000000u) { ok = 0; break; }   /* must lock, not hang */
    }

    /* 2. USB controller reset self-clears. */
    USB_USBCMD = USBCMD_RST;
    if (USB_USBCMD & USBCMD_RST) { ok = 0; }

    /* 3. Device capabilities: device-capable + 8 endpoints. */
    uint32_t dcc = USB_DCCPARAMS;
    if (!(dcc & DCCPARAMS_DC) || ((dcc & DCCPARAMS_DEN) != 8u)) { ok = 0; }

    /* 4. Run bit latches. */
    USB_USBCMD = USBCMD_RS;
    if (!(USB_USBCMD & USBCMD_RS)) { ok = 0; }

    if (ok) {
        puts_("USB: PASS - PHY PLL locks + controller reset/DCCPARAMS/run OK\r\n");
    } else {
        puts_("USB: FAIL - PHY PLL or controller model misbehaved\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
