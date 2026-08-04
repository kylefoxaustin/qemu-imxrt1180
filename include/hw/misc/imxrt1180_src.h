/*
 * NXP i.MX RT1180 SRC + BLK_CTRL_S_AONMIX — Cortex-M7 boot/release control.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_SRC_H
#define HW_MISC_IMXRT1180_SRC_H

#include "hw/core/sysbus.h"
#include "target/arm/cpu-qom.h"   /* ARMCPU forward decl */
#include "qom/object.h"

#define TYPE_IMXRT1180_SRC "imxrt1180-src"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180SRCState, IMXRT1180_SRC)

#define IMXRT1180_SRC_WIN 0x1000

struct IMXRT1180SRCState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem_src;   /* SRC_GENERAL        @ 0x44460000 */
    MemoryRegion iomem_blk;   /* BLK_CTRL_S_AONMIX  @ 0x444F0000 */

    uint32_t src_regs[IMXRT1180_SRC_WIN / 4];
    uint32_t blk_regs[IMXRT1180_SRC_WIN / 4];

    bool     cm7_running;
    /*
     * The M7 boot is a TWO-gate handshake (RM: "on POR the M7 is held in reset AND
     * CPUWAIT is high"): the M33 releases the reset (SRC.SCR.BT_RELEASE_M7) AND
     * clears CPUWAIT (M7_CFG.WAIT).  The core starts only once BOTH are satisfied,
     * in either order -- a bare-metal test writes M7_CFG with WAIT=0 then SCR, while
     * the SDK Prepare_CM7 sets SCR with WAIT still 1 (image not copied yet) and the
     * later MCMGR_StartCore clears WAIT.  This remembers a release seen while WAIT
     * was still high, so clearing WAIT afterwards starts the core.
     */
    bool     m7_release_pending;
    ARMCPU  *cm7;             /* the Cortex-M7 (cpu1); set by the SoC */
};

#endif /* HW_MISC_IMXRT1180_SRC_H */
