/*
 * NXP i.MX RT1180 S3MU — Messaging Unit to the EdgeLock secure enclave (ELE).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_S3MU_H
#define HW_MISC_IMXRT1180_S3MU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_S3MU "imxrt1180-s3mu"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180S3MUState, IMXRT1180_S3MU)

#define IMXRT1180_S3MU_TR_COUNT 4
#define IMXRT1180_S3MU_RR_COUNT 4

struct IMXRT1180S3MUState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;

    /* Control/status backing (VER/PAR/CR/SR/FCR/... — register-accurate). */
    uint32_t cr, sr, fcr, fsr, gier, gcr, gsr, tcr, rcr;

    /* Transmit reassembly: words of the in-flight request message. */
    uint32_t tx_buf[IMXRT1180_S3MU_TR_COUNT];
    uint8_t  tx_count;      /* words written so far in this message */
    uint8_t  tx_expected;   /* total words expected (from header size) */

    /* Response the enclave "returns": RR[] contents + RSR full bits. */
    uint32_t rr[IMXRT1180_S3MU_RR_COUNT];
    uint8_t  rr_full;       /* bitmask of RR registers holding a response word */
};

#endif /* HW_MISC_IMXRT1180_S3MU_H */
