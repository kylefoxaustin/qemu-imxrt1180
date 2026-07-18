/*
 * i.MX RT1180 — LPADC result FIFO drained by eDMA HARDWARE REQUEST.
 *
 * The LPADC's DMA request is CONVERSION-driven, not FIFO-space-driven: a result
 * FIFO filling ABOVE its watermark (FCTRL[FWMARK]) raises the request while
 * DE[FWMDE0] is set, and the serviced RESFIFO reads drain it back down.  Unlike
 * the LPUART/LPSPI/LPI2C sources this one wires to eDMA4 (SRC 57), and there is
 * only ONE gate (the result-collection direction) -- there is no TX side.
 *
 * A software trigger (SWTRIG) with CMDH.LOOP = 7 pushes EIGHT results in one shot,
 * each tagged with its loop count (bits 23:20), so the eight RESFIFO entries are
 * DISTINCT -- 0x8000_8000, 0x8010_8000, ... 0x8070_8000 (VALID | loopcnt | the
 * mid-scale placeholder sample, since no plant drives the channel).  That makes
 * the byte-exact check non-trivial: a stuck or mis-ordered drain is visible.
 *
 * THREE PHASES:
 *   PIO  trigger + CPU-read the 8 results -> golden (self-check; the RESFIFO format
 *        is the device's, not ours to assert from thin air).
 *   NEG  DE=0, channel armed + ERQ, SWTRIG fills the FIFO -> the request must stay
 *        down, so the DMA moves NOTHING (dest stays 0x5A) while FCOUNT proves the
 *        results really are queued (non-vacuous).
 *   POS  DE=FWMDE0 -> the eDMA drains all 8 RESFIFO entries to memory, byte-exact
 *        vs the PIO golden, CPU never reading RESFIFO.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

/* ---- LPADC1 @ 0x4260_0000 (eDMA4 source, SRC 57) ----------------------- */
#define ADC1 0x42600000u
#define ADC_CTRL   (*(volatile uint32_t *)(ADC1 + 0x10))
#define ADC_DE     (*(volatile uint32_t *)(ADC1 + 0x1C))
#define ADC_SWTRIG (*(volatile uint32_t *)(ADC1 + 0x34))
#define ADC_TCTRL0 (*(volatile uint32_t *)(ADC1 + 0xA0))
#define ADC_FCTRL0 (*(volatile uint32_t *)(ADC1 + 0xE0))
#define ADC_CMDL0  (*(volatile uint32_t *)(ADC1 + 0x100))
#define ADC_CMDH0  (*(volatile uint32_t *)(ADC1 + 0x104))
#define ADC_RESFIFO0_ADDR   (ADC1 + 0x300)
#define ADC_RESFIFO0 (*(volatile uint32_t *)ADC_RESFIFO0_ADDR)

#define CTRL_ADCEN 0x1u
#define CTRL_RSTFIFO0 0x100u
#define DE_FWMDE0  0x1u
#define TCTRL_TCMD1 (1u << 24)      /* select CMD0 (1-based) */
#define CMDH_LOOP(n) (((n) & 0xFu) << 16)
#define RESFIFO_VALID 0x80000000u
#define NRES 8u                     /* LOOP=7 -> 8 results */

/* ---- eDMA4 (channel block at base + 0x10000 + n*0x8000) ---------------- */
#define EDMA4_CH(n)  (0x42010000u + (n) * 0x8000u)
#define D32(n, o)    (*(volatile uint32_t *)(EDMA4_CH(n) + (o)))
#define D16(n, o)    (*(volatile uint16_t *)(EDMA4_CH(n) + (o)))
#define CH_CSR(n)     D32(n, 0x000)
#define CH_MUX(n)     D32(n, 0x014)
#define TCD_SADDR(n)  D32(n, 0x020)
#define TCD_SOFF(n)   D16(n, 0x024)
#define TCD_ATTR(n)   D16(n, 0x026)
#define TCD_NBYTES(n) D32(n, 0x028)
#define TCD_DADDR(n)  D32(n, 0x030)
#define TCD_DOFF(n)   D16(n, 0x034)
#define TCD_CITER(n)  D16(n, 0x036)
#define TCD_CSR(n)    D16(n, 0x03C)
#define TCD_BITER(n)  D16(n, 0x03E)
#define CH_CSR_ERQ    (1u << 0)
#define CH_CSR_DONE   (1u << 30)
#define TCD_CSR_DREQ  (1u << 3)
#define ATTR_32BIT    ((2u << 8) | 2u)

#define SRC_ADC1_FIFO0 57u
#define DMA_CH 0u
#define SENTINEL 0x5Au

static uint32_t rx_dma[NRES];
static uint32_t gold[NRES];

static long sh(long op, void *arg)
{
    register long r0 asm("r0") = op;
    register void *r1 asm("r1") = arg;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
static void puts_(const char *s) { sh(SYS_WRITE0, (void *)s); }
static void die(const char *s) { puts_(s); sh(SYS_EXIT, (void *)0x20026u); }

static void adc_setup(void)
{
    ADC_CTRL   = CTRL_ADCEN;
    ADC_CMDL0  = 0;                 /* ADCH = channel 0 */
    ADC_CMDH0  = CMDH_LOOP(NRES - 1u);  /* LOOP=7 -> 8 results, NEXT=0 */
    ADC_TCTRL0 = TCTRL_TCMD1;       /* trigger 0 runs CMD0 */
    ADC_FCTRL0 = 0;                 /* FWMARK=0 -> request as soon as count>0 */
}

static void arm_rx(void)
{
    CH_CSR(DMA_CH)     = 0;
    CH_MUX(DMA_CH)     = 0;
    CH_MUX(DMA_CH)     = SRC_ADC1_FIFO0;
    TCD_SADDR(DMA_CH)  = ADC_RESFIFO0_ADDR;
    TCD_SOFF(DMA_CH)   = 0;         /* RESFIFO0 is a fixed register */
    TCD_ATTR(DMA_CH)   = ATTR_32BIT;
    TCD_NBYTES(DMA_CH) = 4;         /* one result per request */
    TCD_DADDR(DMA_CH)  = (uint32_t)rx_dma;
    TCD_DOFF(DMA_CH)   = 4;
    TCD_CITER(DMA_CH)  = NRES;
    TCD_BITER(DMA_CH)  = NRES;
    TCD_CSR(DMA_CH)    = TCD_CSR_DREQ;
}

static void fill_sentinel(void)
{
    for (unsigned i = 0; i < NRES; i++) {
        rx_dma[i] = SENTINEL;
    }
}

void reset_handler(void);
__attribute__((section(".vectors"), used))
void (* const vt[])(void) = { (void (*)(void))STACK_TOP, reset_handler };

void reset_handler(void)
{
    int fail = 0;

    adc_setup();

    /* ---------------- PIO: capture the golden results ------------------- */
    ADC_SWTRIG = 1u;                /* trigger source 0 -> 8 results */
    for (unsigned i = 0; i < NRES; i++) {
        gold[i] = ADC_RESFIFO0;
    }
    if (!(gold[0] & RESFIFO_VALID) || (gold[0] & 0xFFFFu) == 0) {
        die("ADCDMA: FAIL - PIO: RESFIFO returned no valid result; test can't arm\r\n");
    }

    /* ---------------- NEG: DE=0, the request line must stay down --------- */
    ADC_CTRL = CTRL_ADCEN | CTRL_RSTFIFO0;   /* flush the FIFO first */
    ADC_DE = 0;
    fill_sentinel();
    arm_rx();
    CH_CSR(DMA_CH) = CH_CSR_ERQ;
    ADC_SWTRIG = 1u;               /* results queue, but DE=0 so no request */
    for (volatile uint32_t sp = 0; sp < 2000000u; sp++) {
    }
    if ((ADC_FCTRL0 & 0x1Fu) != NRES) {
        puts_("ADCDMA: FAIL - NEG: results not queued (trigger did nothing?)\r\n");
        fail = 1;
    }
    for (unsigned i = 0; i < NRES; i++) {
        if (rx_dma[i] != SENTINEL) {
            puts_("ADCDMA: FAIL - NEG: eDMA drained RESFIFO with DE[FWMDE0] CLEAR\r\n");
            fail = 1;
            break;
        }
    }
    CH_CSR(DMA_CH) = 0;

    /* ---------------- POSITIVE: DE=FWMDE0 -> eDMA drains the FIFO -------- */
    ADC_CTRL = CTRL_ADCEN | CTRL_RSTFIFO0;   /* fresh FIFO */
    ADC_DE = DE_FWMDE0;            /* enable first (count=0 -> no request yet) */
    fill_sentinel();
    arm_rx();
    CH_CSR(DMA_CH) = CH_CSR_ERQ;
    ADC_SWTRIG = 1u;              /* push 8 results LAST -> request rises, DMA drains */

    uint32_t spins = 0;
    while (!(CH_CSR(DMA_CH) & CH_CSR_DONE) && spins < 200000000u) {
        spins++;
    }
    if (!(CH_CSR(DMA_CH) & CH_CSR_DONE)) {
        puts_("ADCDMA: FAIL - POS: DMA channel never completed its major loop\r\n");
        fail = 1;
    } else if (CH_CSR(DMA_CH) & CH_CSR_ERQ) {
        puts_("ADCDMA: FAIL - POS: TCD_CSR[DREQ] did not clear ERQ at completion\r\n");
        fail = 1;
    } else {
        for (unsigned i = 0; i < NRES; i++) {
            if (rx_dma[i] != gold[i]) {
                puts_("ADCDMA: FAIL - POS: DMA-drained results differ from the PIO read\r\n");
                fail = 1;
                break;
            }
        }
    }
    CH_CSR(DMA_CH) = 0;

    if (!fail) {
        puts_("ADCDMA: PASS - FWMDE0 gate refuses, and open => 8 RESFIFO results "
              "drained by eDMA4, byte-exact vs PIO, CPU never read RESFIFO\r\n");
    }
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
