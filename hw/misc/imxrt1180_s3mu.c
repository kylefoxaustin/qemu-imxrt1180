/*
 * NXP i.MX RT1180 S3MU — Messaging Unit to the EdgeLock secure enclave (ELE).
 *
 * The ELE (Sentinel / S3) is a proprietary secure enclave we do not model.  The
 * SDK talks to it over this MU: it writes a request message word-by-word into
 * the TR[] registers (polling TSR for transmit-empty) and reads the reply from
 * the RR[] registers (polling RSR for receive-full).  A message header word is
 * 0xTT_CC_SS_VV = {tag, command, size, version}; a command has tag 0x17, its
 * reply tag 0xE1, and reply[1] == 0xD6 (RESPONSE_SUCCESS).
 *
 * WHAT THIS MODEL MAY AND MAY NOT SAY
 * -----------------------------------
 * The enclave is proprietary and NOT modelled.  Declining to compute is fine.
 * Telling the GUEST we computed something we did not is not.
 *
 * The SDK's ELE_* wrappers validate a reply with exactly:
 *      if (rmsg[0] == <CMD>_RESPONSE_HDR && rmsg[1] == RESPONSE_SUCCESS)
 *              -> kStatus_Success, and the caller then USES the output buffer
 *      else    -> kStatus_Fail
 * (ele_crypto.c / fsl_ele_base_api.c).  So a blanket SUCCESS reply is not a
 * harmless stub — it is a fabricated cryptographic result.  Concretely, this
 * model used to answer GET_RNG_RANDOM (0x17CD0407) with the byte-exact expected
 * header 0xE1CD0207 + RESPONSE_SUCCESS while never touching the output buffer,
 * so ELE_RngGetRandom() returned kStatus_Success and handed its caller
 * un-initialised memory AS CRYPTOGRAPHIC RANDOMNESS.  Nothing was logged either,
 * because that command's reply is only 2 words.
 *
 * Logging the truth to the HOST (a LOG_UNIMP the firmware cannot see) while
 * reporting SUCCESS to the GUEST is not honesty: the guest is the thing we are
 * pretending to be hardware for.  (Fleet finding, mcxn947qemu 2026-07-11, whose
 * Neutron NPU had the same disease.)
 *
 * So: an explicit whitelist of commands whose real-silicon OUTCOME this model
 * genuinely reproduces (below).  Everything else gets a well-formed reply with a
 * NON-SUCCESS status.  That INFORMS without GATING: the handshake still
 * completes and firmware never hangs — the stock driver simply returns
 * kStatus_Fail, which is the truth.
 *
 * Register offsets/bits verified against the MIMXRT1189 CMSIS PERI_MU.h;
 * command IDs and RESPONSE_SUCCESS against the SDK's ele_crypto_internal.h /
 * fsl_ele_base_api.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/imxrt1180_s3mu.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#define MU_VER    0x000
#define MU_PAR    0x004
#define MU_CR     0x008
#define MU_SR     0x00C
#define MU_FCR    0x100
#define MU_FSR    0x104
#define MU_GIER   0x110
#define MU_GCR    0x114
#define MU_GSR    0x118
#define MU_TCR    0x120
#define MU_TSR    0x124
#define MU_RCR    0x128
#define MU_RSR    0x12C
#define MU_TR0    0x200
#define MU_RR0    0x280

#define MSG_TAG_RESP      0xE1u
#define RESPONSE_SUCCESS  0xD6u

/*
 * Non-success status.  The SDK never enumerates ELE's abort codes (they are
 * proprietary and it does not decode them): every wrapper tests only
 * `rmsg[1] == RESPONSE_SUCCESS` and treats ANY other value as failure.  So the
 * exact byte is not load-bearing for firmware behaviour, and we do not pretend
 * to model ELE's indication/abort encoding.  0x29 is chosen because it is the
 * low byte of the SDK's one non-success response constant, RESPONSE_ERROR_SIZE
 * (0x1d29, ele_crypto_internal.h) — i.e. the SDK's own shape for a failure
 * status.  That is an INFERENCE about the encoding, flagged as such; what is
 * NOT an inference is that it makes the stock driver return kStatus_Fail.
 */
#define RESPONSE_FAILURE  0x29u

/* All TX registers always report empty -> transmit never blocks. */
#define TSR_ALL_EMPTY ((1u << IMXRT1180_S3MU_TR_COUNT) - 1u)

/*
 * ELE commands whose real-silicon OUTCOME this model actually reproduces, and
 * may therefore answer with SUCCESS.  Command IDs from the SDK's
 * fsl_ele_base_api.h.  This is a whitelist ON PURPOSE: a new/unknown ELE command
 * must fail closed (reported as un-computed), never fall through to a fabricated
 * success.
 *
 * Verified by tracing every ELE command the SDK actually issues across the whole
 * prebuilt cm33 demo corpus + the driver_example boot path: only CLOCK_CHANGE_
 * START/FINISH, RELEASE_RDC and GET_FW_STATUS ever appear -- all of them here.
 */
static bool s3mu_command_is_truthful(uint8_t command)
{
    switch (command) {
    case 0x01:   /* PING                  — pure coordination, nothing computed */
    case 0x10:   /* CLOCK_CHANGE_START    — the SDK brackets PLL changes with    */
    case 0x11:   /* CLOCK_CHANGE_FINISH     these; there is no enclave result    */
    case 0x12:   /* VOLTAGE_CHANGE_START  — same class                           */
    case 0x13:   /* VOLTAGE_CHANGE_FINISH                                        */
        return true;
    case 0xC4:
        /*
         * RELEASE_RDC: the enclave hands a peripheral's resource-domain to the
         * core.  Our TRDC grants full access anyway, so the OUTCOME the guest
         * checks for (the peripheral is reachable) genuinely holds here.
         */
        return true;
    case 0xC5:
        /*
         * GET_FW_STATUS: reply data word is the ELE firmware status, and the
         * SDK's own ELE example reads 0 as "no ELE FW in place" -- which is
         * exactly true of this model.  So the zero we return is the correct
         * answer, not a fabricated one.
         */
        return true;
    default:
        return false;
    }
}

/* Reply word count for a given command byte (default 2 = header + status). */
static uint8_t s3mu_response_size(uint8_t command)
{
    switch (command) {
    case 0x9D: return 4;   /* GET_FW_VERSION  */
    case 0xC5: return 3;   /* GET_FW_STATUS   */
    default:   return 2;   /* header + status */
    }
}

/* A full request has been written to TR[] — synthesize the enclave reply. */
static void s3mu_build_response(IMXRT1180S3MUState *s)
{
    uint32_t hdr = s->tx_buf[0];
    uint8_t  command = (hdr >> 16) & 0xFF;
    uint8_t  version = hdr & 0xFF;
    bool     truthful = s3mu_command_is_truthful(command);
    uint8_t  rsize;

    /*
     * A command we cannot answer gets a header + status only.  We do NOT invent
     * a longer reply carrying zeroed "data": the point is to decline, not to
     * decline more elaborately.
     */
    rsize = truthful ? s3mu_response_size(command) : 2;
    if (rsize > IMXRT1180_S3MU_RR_COUNT) {
        rsize = IMXRT1180_S3MU_RR_COUNT;
    }

    memset(s->rr, 0, sizeof(s->rr));
    s->rr[0] = ((uint32_t)MSG_TAG_RESP << 24) | ((uint32_t)command << 16) |
               ((uint32_t)rsize << 8) | version;

    if (truthful || s->fake_uncomputed_success) {
        s->rr[1] = RESPONSE_SUCCESS;
    } else {
        /*
         * TELL THE GUEST.  The enclave did not compute this, so the reply says
         * so -- the stock driver returns kStatus_Fail and firmware handles it.
         * The handshake still completes, so nothing hangs; only the ANSWER is
         * "no".  Reporting SUCCESS here would hand the caller un-computed memory
         * (for GET_RNG_RANDOM: zeroes, as cryptographic randomness) while the
         * driver reported kStatus_Success.
         */
        s->rr[1] = RESPONSE_FAILURE;
    }

    if (!truthful) {
        qemu_log_mask(LOG_UNIMP, "%s: ELE command 0x%02x is not modelled -- "
                      "reporting %s to the guest%s\n", __func__, command,
                      s->fake_uncomputed_success ? "SUCCESS (FABRICATED)"
                                                 : "failure (un-computed)",
                      s->fake_uncomputed_success
                          ? "; fake-uncomputed-success=on: the guest is being "
                            "told a crypto/enclave result it never got"
                          : "");
    }

    s->rr_full = (1u << rsize) - 1u;

    s->tx_count = 0;
    s->tx_expected = 0;
}

static uint64_t imxrt1180_s3mu_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180S3MUState *s = IMXRT1180_S3MU(opaque);

    if (offset >= MU_RR0 && offset < MU_RR0 + 4 * IMXRT1180_S3MU_RR_COUNT) {
        unsigned idx = (offset - MU_RR0) / 4;
        s->rr_full &= ~(1u << idx);     /* consume this reply word */
        return s->rr[idx];
    }
    if (offset >= MU_TR0 && offset < MU_TR0 + 4 * IMXRT1180_S3MU_TR_COUNT) {
        return s->tx_buf[(offset - MU_TR0) / 4];
    }

    switch (offset) {
    case MU_VER:  return 0x00000100;                 /* plausible version   */
    case MU_PAR:  return (IMXRT1180_S3MU_RR_COUNT << 8) |
                         IMXRT1180_S3MU_TR_COUNT;    /* TR/RR counts        */
    case MU_CR:   return s->cr;
    case MU_SR:   return s->sr;
    case MU_FCR:  return s->fcr;
    case MU_FSR:  return s->fsr;
    case MU_GIER: return s->gier;
    case MU_GCR:  return s->gcr;
    case MU_GSR:  return s->gsr;
    case MU_TCR:  return s->tcr;
    case MU_TSR:  return TSR_ALL_EMPTY;              /* TX always ready     */
    case MU_RCR:  return s->rcr;
    case MU_RSR:  return s->rr_full;                 /* RX-full bits        */
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void imxrt1180_s3mu_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    IMXRT1180S3MUState *s = IMXRT1180_S3MU(opaque);

    if (offset >= MU_TR0 && offset < MU_TR0 + 4 * IMXRT1180_S3MU_TR_COUNT) {
        /* Reassemble the outgoing request; the header (first word) carries the
         * total word count in its size byte. */
        if (s->tx_count < IMXRT1180_S3MU_TR_COUNT) {
            s->tx_buf[s->tx_count] = value;
        }
        if (s->tx_count == 0) {
            s->tx_expected = (value >> 8) & 0xFF;    /* header size field */
            if (s->tx_expected == 0 ||
                s->tx_expected > IMXRT1180_S3MU_TR_COUNT) {
                s->tx_expected = 1;
            }
        }
        s->tx_count++;
        if (s->tx_count >= s->tx_expected) {
            s3mu_build_response(s);
        }
        return;
    }

    switch (offset) {
    case MU_CR:   s->cr = value; break;
    case MU_FCR:  s->fcr = value; break;
    case MU_GIER: s->gier = value; break;
    case MU_GCR:  s->gcr = value; break;
    case MU_TCR:  s->tcr = value; break;
    case MU_RCR:  s->rcr = value; break;
    case MU_VER:
    case MU_PAR:
    case MU_SR:
    case MU_FSR:
    case MU_GSR:
    case MU_TSR:
    case MU_RSR:
        break;    /* read-only */
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled write @0x%" HWADDR_PRIx
                      " = 0x%08x\n", __func__, offset, (uint32_t)value);
        break;
    }
}

static const MemoryRegionOps imxrt1180_s3mu_ops = {
    .read = imxrt1180_s3mu_read,
    .write = imxrt1180_s3mu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_s3mu_reset(DeviceState *dev)
{
    IMXRT1180S3MUState *s = IMXRT1180_S3MU(dev);

    s->cr = s->sr = s->fcr = s->fsr = s->gier = s->gcr = s->gsr = 0;
    s->tcr = s->rcr = 0;
    memset(s->tx_buf, 0, sizeof(s->tx_buf));
    memset(s->rr, 0, sizeof(s->rr));
    s->tx_count = s->tx_expected = 0;
    s->rr_full = 0;
}

static void imxrt1180_s3mu_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180S3MUState *s = IMXRT1180_S3MU(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &imxrt1180_s3mu_ops, s,
                          TYPE_IMXRT1180_S3MU, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_imxrt1180_s3mu = {
    .name = TYPE_IMXRT1180_S3MU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr, IMXRT1180S3MUState),
        VMSTATE_UINT32(sr, IMXRT1180S3MUState),
        VMSTATE_UINT32(fcr, IMXRT1180S3MUState),
        VMSTATE_UINT32(fsr, IMXRT1180S3MUState),
        VMSTATE_UINT32(gier, IMXRT1180S3MUState),
        VMSTATE_UINT32(gcr, IMXRT1180S3MUState),
        VMSTATE_UINT32(gsr, IMXRT1180S3MUState),
        VMSTATE_UINT32(tcr, IMXRT1180S3MUState),
        VMSTATE_UINT32(rcr, IMXRT1180S3MUState),
        VMSTATE_UINT32_ARRAY(tx_buf, IMXRT1180S3MUState,
                             IMXRT1180_S3MU_TR_COUNT),
        VMSTATE_UINT8(tx_count, IMXRT1180S3MUState),
        VMSTATE_UINT8(tx_expected, IMXRT1180S3MUState),
        VMSTATE_UINT32_ARRAY(rr, IMXRT1180S3MUState, IMXRT1180_S3MU_RR_COUNT),
        VMSTATE_UINT8(rr_full, IMXRT1180S3MUState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property imxrt1180_s3mu_properties[] = {
    /*
     * Restores the old behaviour: answer EVERY ELE command with SUCCESS, even
     * ones whose result the enclave never computed.  Default OFF, and it stays
     * off unless someone knowingly asks for it, because ON means the guest is
     * told a cryptographic operation succeeded when nothing was computed --
     * ELE_RngGetRandom() then returns kStatus_Success over an untouched buffer.
     * Provided only so a firmware that merely PROBES the enclave (and would
     * abort on a failure it does not care about) can still be brought up.
     */
    DEFINE_PROP_BOOL("fake-uncomputed-success", IMXRT1180S3MUState,
                     fake_uncomputed_success, false),
};

static void imxrt1180_s3mu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_s3mu_realize;
    device_class_set_legacy_reset(dc, imxrt1180_s3mu_reset);
    dc->vmsd = &vmstate_imxrt1180_s3mu;
    device_class_set_props(dc, imxrt1180_s3mu_properties);
}

static const TypeInfo imxrt1180_s3mu_types[] = {
    {
        .name          = TYPE_IMXRT1180_S3MU,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180S3MUState),
        .class_init    = imxrt1180_s3mu_class_init,
    },
};

DEFINE_TYPES(imxrt1180_s3mu_types)
