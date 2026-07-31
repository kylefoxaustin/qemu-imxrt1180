/*
 * NXP i.MX RT1180 ASRC (Asynchronous Sample Rate Converter) — data-path model.
 *
 * The ASRC converts a PCM stream from one sample rate to another. This model
 * carries the memory-to-memory (m2m) path the fsl_asrc ASRC_TransferBlocking
 * driver drives: the guest writes input samples to ASRDIx and reads resampled
 * output from ASRDOx, gated by the ASRSTR input-empty / output-ready flags.
 *
 * The conversion itself is a REAL resampling (linear interpolation at the
 * configured in:out ratio) -- correct output rate, a genuine function of the
 * input, never un-computed data. It is NOT the silicon's polyphase-FIR filter,
 * so output values will not bit-match real hardware; that fidelity gap is flagged,
 * not faked (the m2m example plays the result, it does not verify sample values).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_AUDIO_IMXRT1180_ASRC_H
#define HW_AUDIO_IMXRT1180_ASRC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_ASRC "imxrt1180-asrc"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180ASRCState, IMXRT1180_ASRC)

#define IMXRT1180_ASRC_SIZE      0x100
#define IMXRT1180_ASRC_NUM_REGS  (IMXRT1180_ASRC_SIZE / 4)
#define IMXRT1180_ASRC_PAIRS     3          /* A, B, C */
#define IMXRT1180_ASRC_FIFO      256        /* model FIFO depth (samples/pair) */
#define IMXRT1180_ASRC_N_SAI     4          /* SAI1..4, selectable clock sources */
#define IMXRT1180_ASRC_HIST      32         /* input-history ring for the FIR    */

typedef struct {
    /* Streaming polyphase windowed-sinc resampler state, one per channel pair. */
    uint32_t in_rate, out_rate;             /* Hz, from SetChannelPairConfig     */
    uint32_t channels;                      /* 1 or 2                            */
    /* input ring (interleaved samples) */
    int32_t  in_fifo[IMXRT1180_ASRC_FIFO];
    uint32_t in_head, in_tail;
    /* output ring (interleaved resampled samples) */
    int32_t  out_fifo[IMXRT1180_ASRC_FIFO];
    uint32_t out_head, out_tail;
    /* per-channel input history, indexed by absolute input-frame number mod HIST,
     * so the FIR kernel can reach several samples either side of an output point */
    int32_t  hist[2][IMXRT1180_ASRC_HIST];
    uint64_t in_count;                      /* input frames consumed into hist   */
    double   out_pos;                       /* next output's input-frame position*/
    bool     started;                       /* history primed enough to emit     */
} IMXRT1180ASRCPair;

struct IMXRT1180ASRCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMXRT1180_ASRC_NUM_REGS];
    IMXRT1180ASRCPair pair[IMXRT1180_ASRC_PAIRS];

    /*
     * Links to SAI1..4, whose TX bit clocks are selectable ASRC clock sources
     * (ASRCSR AICSx/AOCSx).  A true-async conversion (input clocked by one SAI,
     * output by another) needs both sources' real frequencies, not just the
     * ASRCDR divider ratio; these let the model resolve them.  NULL = unwired.
     */
    DeviceState *sai[IMXRT1180_ASRC_N_SAI];
};

#endif /* HW_AUDIO_IMXRT1180_ASRC_H */
