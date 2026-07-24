/*
 * Virtual-motor plant — speed-SQUARED (fan/pump) LOAD value-test (Cortex-M33).
 *
 * The plant is seeded with an initial rotor speed (-global imxrt1180-motor.init-mrads)
 * and NO drive, so the rotor coasts freely to rest under the fan load
 * (-global imxrt1180-motor.load-fan-unms) plus the model's viscous damping B.
 * (Free coast is exact here because a tristated inverter carries no phase current
 * and applies no braking torque -- see the plant's PWM-idle path.)
 *
 * ===================== THE COAST-DOWN GOLDEN ===============================
 * Dividing the mechanical ODE  J dw/dt = -(k w^2 + B w)  by w = dtheta/dt gives
 *   J dw/dtheta = -(k w + B)   =>   theta_total = (J/k) * ln(1 + k*w0/B)
 * a closed form for the TOTAL ANGLE the rotor turns before stopping -- read here
 * as the final encoder count REV*CPR + LPOS.  It depends only on J, B (SDK M1
 * motor params), k (the load under test) and w0 (the seed) -- NOT on any line of
 * the plant, and it needs no clock or timebase (angle, not time).  This firmware
 * only REPORTS the measured total count; run.sh computes the golden per config
 * and sweeps w0 and k, so one wrong shape cannot pass.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u
#define EQDC1 0x42710000u
#define REV   (*(volatile uint16_t *)(EQDC1 + 0x1E))
#define LPOS  (*(volatile uint16_t *)(EQDC1 + 0x0E))

static long sh(long op, void *a){register long r0 asm("r0")=op;register void*r1 asm("r1")=a;asm volatile("bkpt 0xAB":"+r"(r0):"r"(r1):"memory");return r0;}
static void puts_(const char *s){sh(SYS_WRITE0,(void*)s);}
static void puti(int32_t v){char b[12];int i=11;b[i--]=0;uint32_t u=v<0?(uint32_t)(-v):(uint32_t)v;if(!u)b[i--]='0';while(u&&i>=0){b[i--]='0'+u%10;u/=10;}if(v<0)b[i--]='-';puts_(&b[i+1]);}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { [0] = (void (*)(void))STACK_TOP, [1] = reset_handler };

void reset_handler(void)
{
    /* Wait for the rotor to fully coast to rest (the plant goes dormant below
     * 1e-4 rad/s), sampling the total angle until it stops advancing. */
    int32_t prev = -1, tot = 0;
    for (int n = 0; n < 80; n++) {
        for (volatile int d = 0; d < 400000; d++) {
        }
        tot = (int32_t)REV * 8000 + LPOS;
        if (tot == prev) {
            break;                 /* settled */
        }
        prev = tot;
    }
    puts_("LOAD total_counts=");
    puti(tot);
    puts_("\r\n");
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
