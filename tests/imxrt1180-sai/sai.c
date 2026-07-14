/*
 * SAI (I2S) TRANSMIT test (Cortex-M33) — it PLAYS, and the samples are the assertion.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT THIS TEST USED TO BE
 *
 *     TCSR = CSR_SR;  ...  TCSR = CSR_EN;
 *     ok = reset bits self-cleared && TE stuck && FWF set && !FEF;
 *     puts_("SAI: PASS - TX init handshake settles (reset self-clear + FWF)");
 *
 * A REGISTER HANDSHAKE.  It never wrote a sample.  The model, meanwhile, did this:
 *
 *     case SAI_TDR0:  /​* Transmit data is accepted and discarded *​/  return;
 *
 * So the SAI could swallow every sample the firmware ever wrote and emit NOTHING,
 * and this test — and every suite run that reported "sai: pass" — stayed green.
 *
 * 91emulator proved the general case on their own SAI and it is sharper than "we
 * had no sink": they clocked PURE SILENCE and the guest's own ALSA oracle STILL
 * SAID PASS.  snd_pcm_writei() succeeds perfectly well against a device faithfully
 * clocking zeros.  THE GUEST CANNOT HEAR ITSELF.
 *
 *   ⭐ THE ORACLE'S WORD IS NOT THE ORACLE.  So this firmware makes no verdict about
 *      the audio at all.  It plays a waveform and says so.  THE VERDICT IS RENDERED
 *      OUTSIDE THE GUEST, BY check.py, AGAINST THE SAMPLES IN THE WAV FILE.
 *      A finding read from the SUBJECT survives a bug in the OBSERVER — and an
 *      oracle living inside the subject is an observer with the subject's bugs.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * THE CLOCK IS REAL AND THE RATE IS EXACT
 *
 *     CCM root 65 (SAI1): MUX=0 -> OSC_RC_24M, DIV=1     MCLK = 24 000 000 Hz
 *     BCLK = MCLK / (2 * (TCR2[DIV]+1)),  DIV=14      ->  BCLK =    800 000 Hz
 *     rate = BCLK / (2 words * 16 bits)               ->         25 000 Hz EXACTLY
 *
 * No rounding anywhere, so check.py can derive 25000 from the RM's formula and the
 * registers below WITHOUT asking the model what rate it chose.  (A golden taken
 * from the thing under test is a mirror: tests/imxrt1180-pwm asserted a carrier
 * period against a PWM_HZ it had read out of the model, and passed a clock that was
 * 1.5x wrong.)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define SYS_WRITE0 0x04
#define SYS_EXIT   0x18
#define STACK_TOP  0x20020000u

#define CCM_BASE   0x44450000u
#define CCM_ROOT_CTRL(n) (*(volatile uint32_t *)(CCM_BASE + (n) * 0x80u))
#define CCM_ROOT_SAI1    65u          /* kCLOCK_Root_Sai1 (fsl_clock.h) */

#define SAI1  0x443B0000u
#define TCSR (*(volatile uint32_t *)(SAI1 + 0x08))
#define TCR1 (*(volatile uint32_t *)(SAI1 + 0x0C))
#define TCR2 (*(volatile uint32_t *)(SAI1 + 0x10))
#define TCR3 (*(volatile uint32_t *)(SAI1 + 0x14))
#define TCR4 (*(volatile uint32_t *)(SAI1 + 0x18))
#define TCR5 (*(volatile uint32_t *)(SAI1 + 0x1C))
#define TDR0 (*(volatile uint32_t *)(SAI1 + 0x20))
#define TFR0 (*(volatile uint32_t *)(SAI1 + 0x40))
#define PARAM (*(volatile uint32_t *)(SAI1 + 0x04))

#define TCR2_BCD (1u << 24)  /* bit-clock direction: 1 = MASTER  */

/*
 * THE RATE IS AN ARGUMENT, NOT A CONSTANT -- AND THE HARNESS MUST ARM IT.
 *
 * 93emulator: "A FORMULA THAT IS CORRECT AT THE POINT YOU TESTED IT IS NOT A FORMULA YOU
 * HAVE TESTED. Only a SECOND operating point exposed mine." Their SAI ignored the divider
 * entirely and every audio test in the fleet asked for 48 kHz -- the ONE rate that cannot
 * see it. This test asked for 25 kHz, once, and had exactly the same blind spot.
 *
 * check.py pokes {MAGIC, DIV} here with `-device loader,addr=...,data=...` and runs the
 * firmware TWICE at two different dividers. A model that ignores TCR2[DIV] transposes the
 * second run and is caught.
 *
 * NO DEFAULT. If the slot is not armed we FAIL loudly rather than fall back to a divider
 * nobody chose -- a test that silently supplies its own input is testing itself.
 * (95emulator's harness "armed" an impostor that never reached the guest, and then accused
 * their own correct model. A NEGATIVE TEST THAT DID NOT PRODUCE THE CONDITION IT NAMES
 * MANUFACTURES A BUG.)
 */
#define SEL_MAGIC_ADDR (*(volatile uint32_t *)0x20001000u)
#define SEL_DIV_ADDR   (*(volatile uint32_t *)0x20001004u)
#define SEL_MAGIC      0x53414931u   /* "SAI1" */

#define CSR_FRF (1u << 16)   /* FIFO has room                    */
#define CSR_FWF (1u << 17)   /* FIFO at/below watermark          */
#define CSR_FEF (1u << 18)   /* FIFO error: over/underrun        */
#define CSR_SR  (1u << 24)
#define CSR_FR  (1u << 25)
#define CSR_EN  (1u << 31)   /* TE */

/* THE WAVEFORM.  Deterministic, integer, and reproducible in three lines of
 * Python — so the golden is DERIVED, never read back from the device. 977 is odd,
 * so the ramp wraps through the whole 16-bit range and a stuck/duplicated sample
 * cannot hide inside it. */
#define NSAMPLES 4096u
static inline int16_t wave(uint32_t n) { return (int16_t)(uint16_t)(n * 977u); }

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
    uint32_t n;
    uint32_t div;

    /* A REAL MCLK: root 65, MUX 0 (OSC_RC_24M), DIV field = divisor-1 = 0. */
    CCM_ROOT_CTRL(CCM_ROOT_SAI1) = (0u << 8) | 0u;

    TCSR = CSR_SR;                 /* software reset (momentary) */
    uint32_t after_sr = TCSR;
    TCSR = CSR_FR;                 /* FIFO reset (momentary)     */
    uint32_t after_fr = TCSR;

    if (SEL_MAGIC_ADDR != SEL_MAGIC) {
        puts_("SAI: FAIL - the rate selector was never armed by the harness\r\n");
        sh(SYS_EXIT, (void *)0x20026u);
    }
    div = SEL_DIV_ADDR;

    /*
     * ⭐ BCD = 1: THIS SAI IS THE BIT-CLOCK MASTER, AND IT HAS TO SAY SO.
     *
     * We used to write TCR2 = 14 -- leaving BCD at 0, i.e. bit-clock SLAVE -- while the
     * model happily computed a MASTER-mode rate from the divider. 93emulator's trap,
     * exactly: on a slave the codec drives BCLK, the divider does nothing, and a formula
     * that reads it is a fabrication with an RM citation attached.
     *
     * A bare-metal test with no codec on the board has no external bit clock, so MASTER is
     * not a convenience here -- it is the only configuration that is physically coherent.
     */
    TCR2 = TCR2_BCD | (div & 0xFFu);
    TCR3 = (1u << 16);             /* enable TX data channel 0                   */
    TCR4 = (1u << 16);             /* FRSZ=1  -> 2 words per frame (stereo)      */
    TCR5 = (15u << 16) | (15u << 24);  /* W0W=WNW=15 -> 16-bit words             */
    TCR1 = 8u;                     /* watermark                                   */

    TCSR = CSR_EN;                 /* enable the transmitter                      */
    uint32_t en = TCSR;

    int init_ok = !(after_sr & CSR_SR) && !(after_fr & CSR_FR) &&
                  (en & CSR_EN) && (en & CSR_FWF) && !(en & CSR_FEF);
    if (!init_ok) {
        puts_("SAI: FAIL - init handshake did not settle\r\n");
        sh(SYS_EXIT, (void *)0x20026u);
    }

    /*
     * PARAM IS A CONTRACT AND THE FIRMWARE IS ENTITLED TO BELIEVE IT.
     * SAI1's silicon has a 16-word FIFO (PARAM[11:8] = 4, 2^4).  The model used to
     * advertise 2^3 = 8 under a comment claiming 32.  If the FIFO the guest gets is
     * SHALLOWER than the one it was promised, a driver sizing a burst from PARAM
     * overruns it -- so check it here, from the guest's side of the bus.
     */
    if (((PARAM >> 8) & 0xFu) != 4u) {
        puts_("SAI: FAIL - PARAM does not advertise SAI1's 16-word FIFO\r\n");
        sh(SYS_EXIT, (void *)0x20026u);
    }

    /*
     * PLAY.  Wait for room (FRF) rather than assuming it: the FIFO is REAL now, and
     * a write to a full FIFO is an overrun the hardware reports in FEF.  The old
     * model "always had space", so this loop could not have blocked -- and could not
     * have detected anything either.
     */
    for (n = 0; n < NSAMPLES; n++) {
        while (!(TCSR & CSR_FRF)) {
            /* the codec is draining; wait for a slot */
        }
        TDR0 = (uint32_t)(uint16_t)wave(n);
    }

    /*
     * WAIT FOR THE FIFO TO ACTUALLY EMPTY, using TFR0's read/write pointers.
     *
     * A fixed delay left 16 samples -- one FIFO's worth -- still sitting in the block
     * when QEMU exited, and the wav was 4080 of 4096 samples long. That is a REAL
     * drop and the test should not have to tolerate it, so do not paper it over with
     * a bigger sleep: ASK THE BLOCK.
     *
     * TFR0 used to read a hardcoded 0 -- "read pointer == write pointer, FIFO always
     * empty" -- so this loop could not have worked, and a driver waiting for drain
     * would have been told it was done before a single sample left.
     */
    for (uint32_t guard = 0; guard < 20000000u; guard++) {
        uint32_t tfr = TFR0;
        uint32_t rfp = tfr & 0x3Fu;
        uint32_t wfp = (tfr >> 16) & 0x3Fu;
        if (rfp == wfp) {
            break;              /* pointers equal => the FIFO is empty */
        }
    }
    if (TCSR & CSR_FEF) {
        puts_("SAI: FIFO ERROR (FEF) during playback\r\n");
    }

    puts_("SAI: PLAYED 4096 samples (the WAV is the assertion)\r\n");
    sh(SYS_EXIT, (void *)0x20026u);
    for (;;) {
    }
}
