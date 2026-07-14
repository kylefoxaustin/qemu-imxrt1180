/*
 * NXP i.MX RT1180 SRC + BLK_CTRL_S_AONMIX — Cortex-M7 boot/release control.
 *
 * The boot Cortex-M33 releases the Cortex-M7 (per the SDK Prepare_CM7):
 *   1. BLK_CTRL_S_AONMIX->M7_CFG.INITVTOR = m7_vtor >> 7   (M7 boot-vector base)
 *   2. SRC_GENERAL->SCR |= BT_RELEASE_M7                    (release the M7)
 *
 * This device models both register blocks (register-backed) and, on the
 * release write, starts the M7 from a BOTTOM-HALF — a store issued from the
 * M33's TCG block must not re-tune another vCPU inline (MCX's lesson).  The
 * start is edge-triggered + idempotent.  On start, the M7's init-SVTOR is set
 * from M7_CFG, the core is reset (M-profile reloads SP/PC from that vector
 * table) and released.
 *
 * Offsets/bits verified against the MIMXRT1189 CMSIS (SRC_GENERAL SCR @0x10
 * BT_RELEASE_M7=0x1; BLK_CTRL_S_AONMIX M7_CFG @0x80 INITVTOR=[31:7]).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "hw/misc/imxrt1180_src.h"
#include "migration/vmstate.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"

#define SRC_SCR              0x10
#define SCR_BT_RELEASE_M7    0x1u

#define BLK_M7_CFG           0x80
#define M7_CFG_INITVTOR_MASK 0xFFFFFF80u

/* Keep cs->halted / PSCI power_state / halt_reason consistent (MCX's note:
 * arm_cpu_has_work() asserts a PSCI_OFF core is HALT_PSCI on its first WFI). */
static void imxrt1180_src_set_cm7_run(ARMCPU *cpu, bool run)
{
    CPUState *cs = CPU(cpu);

    cs->halted = !run;
    cpu->power_state = run ? PSCI_ON : PSCI_OFF;
    cpu->env.halt_reason = run ? NOT_HALTED : HALT_PSCI;
}

static void imxrt1180_src_start_cm7_bh(void *opaque)
{
    IMXRT1180SRCState *s = opaque;
    ARMCPU *cpu = s->cm7;

    if (!cpu) {
        return;
    }
    /*
     * The Cortex-M7 has no TrustZone-M, so it resets NON-secure and fetches its
     * initial SP/PC from vecbase[M_REG_NS] = init_nsvtor (not init_svtor, which
     * only the secure M33 uses).  Set both so the M7 boots from M7_CFG.INITVTOR.
     */
    cpu->init_svtor  = s->blk_regs[BLK_M7_CFG / 4] & M7_CFG_INITVTOR_MASK;
    cpu->init_nsvtor = cpu->init_svtor;
    cpu_reset(CPU(cpu));
    imxrt1180_src_set_cm7_run(cpu, true);
    cpu_resume(CPU(cpu));
    s->cm7_running = true;
}

/* ---- SRC_GENERAL block --------------------------------------------------- */
static uint64_t imxrt1180_src_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180SRCState *s = IMXRT1180_SRC(opaque);
    return s->src_regs[offset / 4];
}

static void imxrt1180_src_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    IMXRT1180SRCState *s = IMXRT1180_SRC(opaque);

    s->src_regs[offset / 4] = value;
    if (offset == SRC_SCR && (value & SCR_BT_RELEASE_M7) &&
        s->cm7 && !s->cm7_running) {
        /* Defer the M7 start off cpu0's execution context. */
        aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                imxrt1180_src_start_cm7_bh, s);
    }
}

static const MemoryRegionOps imxrt1180_src_ops = {
    .read = imxrt1180_src_read,
    .write = imxrt1180_src_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* ---- BLK_CTRL_S_AONMIX block (holds M7_CFG.INITVTOR) --------------------- */
static uint64_t imxrt1180_blk_read(void *opaque, hwaddr offset, unsigned size)
{
    IMXRT1180SRCState *s = IMXRT1180_SRC(opaque);
    return s->blk_regs[offset / 4];
}

static void imxrt1180_blk_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    IMXRT1180SRCState *s = IMXRT1180_SRC(opaque);
    s->blk_regs[offset / 4] = value;
}

static const MemoryRegionOps imxrt1180_blk_ops = {
    .read = imxrt1180_blk_read,
    .write = imxrt1180_blk_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void imxrt1180_src_reset(DeviceState *dev)
{
    IMXRT1180SRCState *s = IMXRT1180_SRC(dev);

    memset(s->src_regs, 0, sizeof(s->src_regs));
    memset(s->blk_regs, 0, sizeof(s->blk_regs));

    /*
     * SRC_GENERAL reset values (offsets PERI_SRC_GENERAL.h, values from the RM).
     *
     * SRSR / SRSR_BBSM = 1: bit 0 is the POWER-ON-RESET flag, and it IS SET after a
     * power-on reset -- which is precisely the state we are in.  Reading 0 tells the
     * guest "no reset cause recorded", from a machine that has just come out of one.
     * Boot code branches on this.
     */
    s->src_regs[0x18 / 4] = 0x00003FBF;   /* SRMASK    */
    s->src_regs[0x44 / 4] = 0x08000000;   /* SBMR2     */
    s->src_regs[0x4C / 4] = 0x00000001;   /* SRSR_BBSM */
    s->src_regs[0x50 / 4] = 0x00000001;   /* SRSR: power-on reset happened */

    /*
     * BLK_CTRL_S_AONMIX CM33_IRQ_MASK[0..7] @0x00 and CM7_IRQ_MASK[0..7] @0x20 reset
     * to 0xFFFFFFFF -- EVERY INTERRUPT MASKED.  A memset to zero says the exact
     * opposite: every one of 512 interrupt sources UNMASKED out of reset.
     *
     * A ZERO RESET VALUE IS NOT THE ABSENCE OF A CLAIM.  Here the claim is
     * "everything is armed", and it is the inverse of the silicon.  Found only once
     * the reset-value gate could see flat arrays sized by a #define -- these are
     * CM33_IRQ_MASK[CM33_IRQ_MASK_COUNT], and the extractor had never expanded a
     * single macro-sized array on this chip.
     */
    for (int i = 0; i < 8; i++) {
        s->blk_regs[(0x00 + i * 4) / 4] = 0xFFFFFFFF;   /* CM33_IRQ_MASK[i] */
        s->blk_regs[(0x20 + i * 4) / 4] = 0xFFFFFFFF;   /* CM7_IRQ_MASK[i]  */
    }
    s->cm7_running = false;
}

static void imxrt1180_src_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180SRCState *s = IMXRT1180_SRC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem_src, OBJECT(s), &imxrt1180_src_ops, s,
                          "imxrt1180.src-general", IMXRT1180_SRC_WIN);
    sysbus_init_mmio(sbd, &s->iomem_src);
    memory_region_init_io(&s->iomem_blk, OBJECT(s), &imxrt1180_blk_ops, s,
                          "imxrt1180.blk-ctrl-s-aonmix", IMXRT1180_SRC_WIN);
    sysbus_init_mmio(sbd, &s->iomem_blk);
}

static const VMStateDescription vmstate_imxrt1180_src = {
    .name = TYPE_IMXRT1180_SRC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(src_regs, IMXRT1180SRCState, IMXRT1180_SRC_WIN / 4),
        VMSTATE_UINT32_ARRAY(blk_regs, IMXRT1180SRCState, IMXRT1180_SRC_WIN / 4),
        VMSTATE_BOOL(cm7_running, IMXRT1180SRCState),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_src_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_src_realize;
    device_class_set_legacy_reset(dc, imxrt1180_src_reset);
    dc->vmsd = &vmstate_imxrt1180_src;
}

static const TypeInfo imxrt1180_src_types[] = {
    {
        .name          = TYPE_IMXRT1180_SRC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180SRCState),
        .class_init    = imxrt1180_src_class_init,
    },
};

DEFINE_TYPES(imxrt1180_src_types)
