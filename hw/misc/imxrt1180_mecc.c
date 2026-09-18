/*
 * NXP i.MX RT1180 MECC — OCRAM Error-Correction-Code controller.
 *
 * MECC sits in front of an OCRAM bank and adds SECDED ECC: each 64-bit word is
 * stored with an 8-bit ECC code; a single-bit error is CORRECTED on read (and
 * reported), a double-bit error is DETECTED (uncorrectable) and flagged. The
 * driver exercises this through the error-INJECTION path: ERR_DATA_INJ_* arms a
 * bit-flip that the next write bakes into the stored codeword, and a subsequent
 * read then sees the (corrected or detected) error plus the single/multi error
 * info registers and an interrupt.
 *
 * The OCRAM is organised in four banks interleaved at 8-byte granularity within
 * a 32-byte stride (bank = (word_offset >> 3) & 3), matching the SDK example's
 * comment. This model intercepts the OCRAM data window (an IO region backed by a
 * host buffer) so writes/reads pass through the ECC logic; each 64-bit word
 * carries a captured injection overlay.
 *
 * FIDELITY NOTE: the ECC *code* value reported in {SINGLE,MULTI}_ERR_ADDR_ECC
 * is a genuine (72,64) Hamming SECDED of the data, but NOT NXP's exact
 * (unpublished) bit assignment -- it is algorithm-class-faithful, not
 * silicon-exact. It is only ever PRINTED by the demo, never asserted, so nothing
 * a guest branches on depends on it. Everything the demo DOES assert -- the
 * corrected data, the raw errored data, the bit position, the error address, the
 * single/multi status and the interrupt -- is modelled exactly.
 *
 * Offsets/bits verified against MIMXRT1189 CMSIS (PERI_MECC.h) and fsl_mecc.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "hw/misc/imxrt1180_mecc.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/* --- register offsets (PERI_MECC.h) --- */
#define MECC_ERR_STATUS       0x00   /* W1C */
#define MECC_ERR_STAT_EN      0x04
#define MECC_ERR_SIG_EN       0x08
#define MECC_INJ_BASE         0x0C   /* 3 regs (LOW,HIGH,ECC) x 4 banks         */
#define MECC_INJ_END          0x38
#define MECC_SE_BASE          0x3C   /* 5 regs (ADDR_ECC,LO,HI,POSLO,POSHI) x4  */
#define MECC_SE_END           0x88
#define MECC_ME_BASE          0x8C   /* 3 regs (ADDR_ECC,LO,HI) x4              */
#define MECC_ME_END           0xB8
#define MECC_PIPE_ECC_EN      0x100
#define MECC_PENDING_STAT     0x104

/* ERR_STATUS: SINGLE_ERR<bank> = bit b; MULTI_ERR<bank> = bit 4+b */
#define MECC_SINGLE(b)        (1u << (b))
#define MECC_MULTI(b)         (1u << (4 + (b)))
#define MECC_STATUS_ALL       0xFFu

#define PIPE_ECC_EN_ECC_EN    (1u << 4)

/*
 * A genuine (72,64) Hamming SECDED of the 64-bit data: 7 Hamming parity bits
 * over the data placed at non-power-of-two codeword positions, plus an overall
 * parity bit. Real SECDED, but NOT NXP's exact bit layout (unpublished) -- so
 * the reported ECC code is algorithm-faithful, not silicon-exact. Printed only.
 */
static uint8_t mecc_ecc8(uint64_t d)
{
    uint8_t parity[7] = {0};
    int dpos = 0;

    for (int pos = 1; dpos < 64; pos++) {
        if ((pos & (pos - 1)) == 0) {
            continue;                     /* skip parity positions (powers of two) */
        }
        if ((d >> dpos) & 1u) {
            for (int k = 0; k < 7; k++) {
                if (pos & (1 << k)) {
                    parity[k] ^= 1u;
                }
            }
        }
        dpos++;
    }

    uint8_t ecc = 0;
    for (int k = 0; k < 7; k++) {
        ecc |= parity[k] << k;
    }
    ecc |= (uint8_t)(ctpop64(d) & 1u) << 7;   /* overall parity: double-error detect */
    return ecc;
}

static void mecc_update_irq(IMXRT1180MECCState *s)
{
    /* A source contributes to the line only if its status is enabled (ERR_STAT_EN)
     * and its interrupt (signal) is enabled (ERR_SIG_EN). */
    bool active = (s->err_status & s->err_stat_en & s->err_sig_en) != 0;
    qemu_set_irq(s->irq, active);
}

/* ---- OCRAM data intercept (ECC path) ---- */

static uint64_t mecc_ocram_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180MECCState *s = opaque;
    hwaddr word = off & ~(hwaddr)7;
    uint32_t widx = word >> 3;
    unsigned bank = widx & 3u;

    if ((s->pipe_ecc_en & PIPE_ECC_EN_ECC_EN) &&
        widx < s->ocram_size / 8 && s->poison[widx].poisoned) {
        uint64_t host = ldq_le_p(s->data + word);
        uint32_t il = s->poison[widx].inj_low;
        uint32_t ih = s->poison[widx].inj_high;
        uint8_t  ie = s->poison[widx].inj_ecc;
        uint32_t rl = (uint32_t)host ^ il;
        uint32_t rh = (uint32_t)(host >> 32) ^ ih;
        int nbits = ctpop32(il) + ctpop32(ih) + ctpop32(ie);

        if (nbits == 1) {
            /* Single-bit: correctable -> read returns the CLEAN (stored) data. */
            s->err_status |= MECC_SINGLE(bank);
            s->se_addr_ecc[bank] = ((uint32_t)word << 8) | mecc_ecc8(host);
            s->se_data_low[bank] = rl;
            s->se_data_high[bank] = rh;
            /* SINGLE_ERR_POS_* is a ONE-HOT mask of the flipped bit (the driver
             * does log2() of it via a shift-count loop), i.e. the injection mask
             * itself for a single-bit error -- NOT the bit index. */
            s->se_pos_low[bank] = il;
            s->se_pos_high[bank] = ih;
            mecc_update_irq(s);
            /* fall through: return the corrected (host) bytes */
        } else if (nbits >= 2) {
            /* Multi-bit: uncorrectable -> read returns the CORRUPTED data. */
            uint64_t corrupt = host ^ (((uint64_t)ih << 32) | il);
            uint8_t tmp[8];
            s->err_status |= MECC_MULTI(bank);
            s->me_addr_ecc[bank] = ((uint32_t)word << 8) | mecc_ecc8(host);
            s->me_data_low[bank] = rl;
            s->me_data_high[bank] = rh;
            mecc_update_irq(s);
            stq_le_p(tmp, corrupt);
            return ldn_le_p(tmp + (off & 7), size);
        }
    }

    return ldn_le_p(s->data + off, size);
}

static void mecc_ocram_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IMXRT1180MECCState *s = opaque;
    hwaddr word = off & ~(hwaddr)7;
    uint32_t widx = word >> 3;
    unsigned bank = widx & 3u;

    /* Store the data the CPU wrote (clean) in the backing buffer. */
    stn_le_p(s->data + off, size, val);

    /* If ECC is on and this bank has an injection armed, poison the word: the
     * hardware bakes the ERR_DATA_INJ_* flip into the stored codeword. */
    if ((s->pipe_ecc_en & PIPE_ECC_EN_ECC_EN) && widx < s->ocram_size / 8) {
        if (s->inj_low[bank] | s->inj_high[bank] | s->inj_ecc[bank]) {
            s->poison[widx].inj_low = s->inj_low[bank];
            s->poison[widx].inj_high = s->inj_high[bank];
            s->poison[widx].inj_ecc = s->inj_ecc[bank];
            s->poison[widx].poisoned = true;
        } else {
            s->poison[widx].poisoned = false;   /* a clean write scrubs the word */
        }
    }
}

static const MemoryRegionOps mecc_ocram_ops = {
    .read = mecc_ocram_read,
    .write = mecc_ocram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/* ---- register block ---- */

static uint64_t mecc_regs_read(void *opaque, hwaddr off, unsigned size)
{
    IMXRT1180MECCState *s = opaque;

    switch (off) {
    case MECC_ERR_STATUS:   return s->err_status;
    case MECC_ERR_STAT_EN:  return s->err_stat_en;
    case MECC_ERR_SIG_EN:   return s->err_sig_en;
    case MECC_PIPE_ECC_EN:  return s->pipe_ecc_en;
    case MECC_PENDING_STAT: return s->pending_stat;
    default:
        if (off >= MECC_INJ_BASE && off <= MECC_INJ_END) {
            unsigned idx = (off - MECC_INJ_BASE) / 4, b = idx / 3, sub = idx % 3;
            return sub == 0 ? s->inj_low[b] : sub == 1 ? s->inj_high[b] : s->inj_ecc[b];
        }
        if (off >= MECC_SE_BASE && off <= MECC_SE_END) {
            unsigned idx = (off - MECC_SE_BASE) / 4, b = idx / 5, sub = idx % 5;
            switch (sub) {
            case 0: return s->se_addr_ecc[b];
            case 1: return s->se_data_low[b];
            case 2: return s->se_data_high[b];
            case 3: return s->se_pos_low[b];
            default: return s->se_pos_high[b];
            }
        }
        if (off >= MECC_ME_BASE && off <= MECC_ME_END) {
            unsigned idx = (off - MECC_ME_BASE) / 4, b = idx / 3, sub = idx % 3;
            return sub == 0 ? s->me_addr_ecc[b] : sub == 1 ? s->me_data_low[b]
                                                           : s->me_data_high[b];
        }
        qemu_log_mask(LOG_GUEST_ERROR, "imxrt1180-mecc: rd off 0x%" HWADDR_PRIx "\n", off);
        return 0;
    }
}

static void mecc_regs_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IMXRT1180MECCState *s = opaque;

    switch (off) {
    case MECC_ERR_STATUS:                 /* W1C */
        s->err_status &= ~(uint32_t)val;
        mecc_update_irq(s);
        return;
    case MECC_ERR_STAT_EN:  s->err_stat_en  = val; mecc_update_irq(s); return;
    case MECC_ERR_SIG_EN:   s->err_sig_en   = val; mecc_update_irq(s); return;
    case MECC_PIPE_ECC_EN:  s->pipe_ecc_en  = val; return;
    case MECC_PENDING_STAT: s->pending_stat = val; return;
    default:
        if (off >= MECC_INJ_BASE && off <= MECC_INJ_END) {
            unsigned idx = (off - MECC_INJ_BASE) / 4, b = idx / 3, sub = idx % 3;
            if (sub == 0)      { s->inj_low[b]  = val; }
            else if (sub == 1) { s->inj_high[b] = val; }
            else               { s->inj_ecc[b]  = val; }
            return;
        }
        /* SINGLE/MULTI error info are read-only (__I); ignore writes. */
        if (off >= MECC_SE_BASE && off <= MECC_ME_END) {
            return;
        }
        qemu_log_mask(LOG_GUEST_ERROR,
                      "imxrt1180-mecc: wr off 0x%" HWADDR_PRIx " val 0x%" PRIx64 "\n",
                      off, val);
        return;
    }
}

static const MemoryRegionOps mecc_regs_ops = {
    .read = mecc_regs_read,
    .write = mecc_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void mecc_reset(DeviceState *dev)
{
    IMXRT1180MECCState *s = IMXRT1180_MECC(dev);

    s->err_status = 0;
    s->err_stat_en = 0;
    s->err_sig_en = 0;
    s->pipe_ecc_en = 0;
    s->pending_stat = 0;
    memset(s->inj_low, 0, sizeof(s->inj_low));
    memset(s->inj_high, 0, sizeof(s->inj_high));
    memset(s->inj_ecc, 0, sizeof(s->inj_ecc));
    memset(s->se_addr_ecc, 0, sizeof(s->se_addr_ecc));
    memset(s->se_data_low, 0, sizeof(s->se_data_low));
    memset(s->se_data_high, 0, sizeof(s->se_data_high));
    memset(s->se_pos_low, 0, sizeof(s->se_pos_low));
    memset(s->se_pos_high, 0, sizeof(s->se_pos_high));
    memset(s->me_addr_ecc, 0, sizeof(s->me_addr_ecc));
    memset(s->me_data_low, 0, sizeof(s->me_data_low));
    memset(s->me_data_high, 0, sizeof(s->me_data_high));
    if (s->poison) {
        memset(s->poison, 0, sizeof(*s->poison) * (s->ocram_size / 8));
    }
    qemu_set_irq(s->irq, 0);
}

static void mecc_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180MECCState *s = IMXRT1180_MECC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (s->ocram_size == 0 || (s->ocram_size & 7)) {
        error_setg(errp, "imxrt1180-mecc: ocram-size must be a nonzero multiple of 8");
        return;
    }

    memory_region_init_io(&s->regs, OBJECT(dev), &mecc_regs_ops, s,
                          "imxrt1180.mecc.regs", 0x1000);
    sysbus_init_mmio(sbd, &s->regs);

    s->data = g_malloc0(s->ocram_size);
    s->poison = g_new0(IMXRT1180MECCPoison, s->ocram_size / 8);
    memory_region_init_io(&s->ocram, OBJECT(dev), &mecc_ocram_ops, s,
                          "imxrt1180.mecc.ocram", s->ocram_size);
    sysbus_init_mmio(sbd, &s->ocram);   /* mmio[1] = the ECC-fronted OCRAM data */

    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription mecc_vmstate = {
    .name = "imxrt1180-mecc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(err_status, IMXRT1180MECCState),
        VMSTATE_UINT32(err_stat_en, IMXRT1180MECCState),
        VMSTATE_UINT32(err_sig_en, IMXRT1180MECCState),
        VMSTATE_UINT32_ARRAY(inj_low, IMXRT1180MECCState, IMXRT1180_MECC_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(inj_high, IMXRT1180MECCState, IMXRT1180_MECC_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(inj_ecc, IMXRT1180MECCState, IMXRT1180_MECC_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(se_addr_ecc, IMXRT1180MECCState, IMXRT1180_MECC_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(se_data_low, IMXRT1180MECCState, IMXRT1180_MECC_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(se_data_high, IMXRT1180MECCState, IMXRT1180_MECC_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(se_pos_low, IMXRT1180MECCState, IMXRT1180_MECC_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(se_pos_high, IMXRT1180MECCState, IMXRT1180_MECC_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(me_addr_ecc, IMXRT1180MECCState, IMXRT1180_MECC_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(me_data_low, IMXRT1180MECCState, IMXRT1180_MECC_NUM_BANKS),
        VMSTATE_UINT32_ARRAY(me_data_high, IMXRT1180MECCState, IMXRT1180_MECC_NUM_BANKS),
        VMSTATE_UINT32(pipe_ecc_en, IMXRT1180MECCState),
        VMSTATE_UINT32(pending_stat, IMXRT1180MECCState),
        VMSTATE_VBUFFER_UINT32(data, IMXRT1180MECCState, 1, NULL, ocram_size),
        VMSTATE_END_OF_LIST()
    }
};

static const Property mecc_props[] = {
    DEFINE_PROP_UINT32("ocram-size", IMXRT1180MECCState, ocram_size, 0),
};

static void mecc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mecc_realize;
    device_class_set_legacy_reset(dc, mecc_reset);
    dc->vmsd = &mecc_vmstate;
    device_class_set_props(dc, mecc_props);
}

static const TypeInfo mecc_types[] = {
    {
        .name = TYPE_IMXRT1180_MECC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180MECCState),
        .class_init = mecc_class_init,
    },
};

DEFINE_TYPES(mecc_types)
