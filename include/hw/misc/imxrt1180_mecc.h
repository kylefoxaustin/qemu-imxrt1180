/*
 * NXP i.MX RT1180 MECC — OCRAM ECC controller.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_MECC_H
#define HW_MISC_IMXRT1180_MECC_H

#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_MECC "imxrt1180-mecc"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180MECCState, IMXRT1180_MECC)

#define IMXRT1180_MECC_NUM_BANKS 4

/* One captured error injection per 64-bit OCRAM word (sparse-in-practice). */
typedef struct {
    uint32_t inj_low;
    uint32_t inj_high;
    uint8_t  inj_ecc;
    bool     poisoned;
} IMXRT1180MECCPoison;

struct IMXRT1180MECCState {
    SysBusDevice parent_obj;

    MemoryRegion regs;        /* MMIO register block                     */
    MemoryRegion ocram;       /* ECC-fronted OCRAM data (IO-intercepted)  */
    qemu_irq irq;

    /* OCRAM geometry (properties) */
    uint32_t ocram_size;      /* bytes                                    */

    /* registers */
    uint32_t err_status;      /* 0x00 ERR_STATUS      (W1C)               */
    uint32_t err_stat_en;     /* 0x04 ERR_STAT_EN                         */
    uint32_t err_sig_en;      /* 0x08 ERR_SIG_EN                          */
    uint32_t inj_low[IMXRT1180_MECC_NUM_BANKS];
    uint32_t inj_high[IMXRT1180_MECC_NUM_BANKS];
    uint32_t inj_ecc[IMXRT1180_MECC_NUM_BANKS];
    uint32_t se_addr_ecc[IMXRT1180_MECC_NUM_BANKS];
    uint32_t se_data_low[IMXRT1180_MECC_NUM_BANKS];
    uint32_t se_data_high[IMXRT1180_MECC_NUM_BANKS];
    uint32_t se_pos_low[IMXRT1180_MECC_NUM_BANKS];
    uint32_t se_pos_high[IMXRT1180_MECC_NUM_BANKS];
    uint32_t me_addr_ecc[IMXRT1180_MECC_NUM_BANKS];
    uint32_t me_data_low[IMXRT1180_MECC_NUM_BANKS];
    uint32_t me_data_high[IMXRT1180_MECC_NUM_BANKS];
    uint32_t pipe_ecc_en;     /* 0x100 PIPE_ECC_EN                        */
    uint32_t pending_stat;    /* 0x104 PENDING_STAT                       */

    /* OCRAM backing + per-word injected-error overlay */
    uint8_t *data;                    /* host storage for the OCRAM       */
    IMXRT1180MECCPoison *poison;      /* one entry per 64-bit word        */
};

#endif /* HW_MISC_IMXRT1180_MECC_H */
