/*
 * NXP i.MX RT1180 SAI (Serial Audio Interface / I2S).
 *
 * The transmit path is REAL: words the guest writes to TDR land in a FIFO, are
 * clocked out at the rate the guest's own registers describe, and are handed to
 * QEMU's audio backend.  With `-audio driver=wav,path=out.wav` the samples the
 * firmware wrote appear in a file, byte-for-byte, and can be ASSERTED.
 *
 * They used to be DISCARDED ("no audio sink modelled"), and the only test we had
 * asserted a register handshake -- so the SAI could clock silence, or nothing at
 * all, and the suite stayed green.  91emulator proved the general case on their
 * own SAI: they memset the capture ring and the guest's ALSA oracle STILL said
 * PASS, because snd_pcm_writei() succeeds perfectly well against a device that is
 * faithfully clocking zeros.
 *
 *   ⭐ THE ORACLE'S WORD IS NOT THE ORACLE.  A verdict computed INSIDE the guest
 *      cannot see a device that accepted every write and emitted nothing.  Only
 *      something outside it, looking at the SAMPLES, can.
 *
 * Offsets/bits from the MIMXRT1189 CMSIS header (I2S_Type); PARAM per-instance
 * reset values and field semantics from the RM (SAI chapter, "Register reset
 * values").
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_SAI_H
#define HW_MISC_IMXRT1180_SAI_H

#include "hw/core/sysbus.h"
#include "qemu/audio.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_SAI "imxrt1180-sai"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180SAIState, IMXRT1180_SAI)

#define IMXRT1180_SAI_SIZE 0x1000

/*
 * THE DEEPEST FIFO ANY INSTANCE HAS.  PARAM tells the guest how deep ITS OWN
 * instance is (2^FIFO), and that number is per-instance -- SAI1 has 16 words,
 * SAI2..4 have 32.  This array is the storage; `fifo_depth` is what the guest
 * was told.  The invariant is MODEL >= ADVERTISED, checked at realize().
 */
#define IMXRT1180_SAI_FIFO_MAX 32

struct IMXRT1180SAIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq dma_tx_req;    /* TX FIFO -> eDMA hardware request line */
    uint32_t regs[IMXRT1180_SAI_SIZE / 4];

    /* --- properties: what THIS instance of the silicon is --- */
    uint32_t param;         /* PARAM reset value, from the RM, per instance */
    uint32_t clk_root;      /* CCM clock root feeding this SAI's MCLK */
    void *ccm;              /* link to the CCM, for a REAL MCLK */

    /* --- the transmit path --- */
    uint32_t tx_fifo[IMXRT1180_SAI_FIFO_MAX];
    uint32_t tx_count;      /* words currently in the FIFO */
    uint32_t tx_rptr;       /* read pointer (for TFR0's RFP) */
    uint32_t tx_wptr;       /* write pointer (for TFR0's WFP) */
    uint32_t fifo_depth;    /* 2^PARAM[FIFO] -- what the guest was TOLD */

    /* --- the audio sink --- */
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    uint32_t voice_hz;      /* the rate the voice is currently open at (0 = closed) */

    /*
     * The codec pulls samples out of the TX FIFO at the bit clock -- continuously,
     * at fs -- with no host involvement.  QEMU's audio backend instead notifies a
     * SW-voice callback at the *audiodev timer* rate (default 100 Hz), and each
     * call can only move as many words as the FIFO holds (<= 16).  100 x 16 is far
     * below fs, so a 16-word FIFO cannot bridge a 100 Hz callback to a 48 kHz
     * stream: an interrupt-driven transfer starves and crawls.  This periodic
     * virtual-clock timer models the codec's own fs-paced pull -- it drains the
     * FIFO often enough that the backend's RateCtl (not the callback cadence) is
     * the governor, so the stream runs at real fs.  Armed while TX is active.
     */
    QEMUTimer *drain_timer;
    int64_t  drain_last_ns;   /* QEMU_CLOCK_VIRTUAL at the last drain tick        */
    uint64_t drain_acc;       /* fractional words carried, in units of words*1e9  */
    int64_t  drain_out_ns;    /* virtual time the last word was clocked to the sink;
                               * the codec-pipeline grace holds "FIFO empty" back
                               * from the guest for a short window past this, so the
                               * host audio backend can flush its own buffered tail
                               * to the wav before the guest exits (see the .c).    */
};

/*
 * The TX bit-clock frequency (Hz) this SAI drives as a master (MCLK/(2*(DIV+1))),
 * or 0 if it is a slave / has no MCLK.  Exposed for the ASRC, which may use a
 * SAI's TX bit clock as its sample-rate-conversion reference.
 */
uint32_t imxrt1180_sai_tx_bclk_hz(DeviceState *dev);

#endif /* HW_MISC_IMXRT1180_SAI_H */
