/*
 * NXP i.MX RT1180 XCACHE (platform cache controller) — control-plane model.
 *
 * The RT1180 has two XCACHE controllers, XCACHE_PC (code, @0x44400000) and
 * XCACHE_PS (system, @0x44400800), each a 4-register block (CCR/CLCR/CSAR/CCVR).
 * QEMU's guest memory is coherent and has no data cache, so every cache
 * maintenance op (invalidate / push / line-invalidate) is a genuine no-op — the
 * only thing that must be modelled is COMPLETION: the fsl_cache driver sets a
 * command together with CCR.GO (or CSAR.LGO) and then spins on that bit until it
 * self-clears. If the bit never clears, the driver hangs (the FlexSPI polling
 * example wedged here). We clear the transient command bits on write, so the
 * first poll reads "done" — truthful, since the op really is complete (nothing
 * to do). Config bits (ENCACHE, etc.) stick and read back.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_XCACHE_H
#define HW_MISC_IMXRT1180_XCACHE_H
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMXRT1180_XCACHE "imxrt1180-xcache"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180XcacheState, IMXRT1180_XCACHE)

/* One 0x1000 window covers both instances: PC at +0x000, PS at +0x800. */
#define IMXRT1180_XCACHE_SIZE      0x1000
#define IMXRT1180_XCACHE_NUM_REGS  (IMXRT1180_XCACHE_SIZE / 4)

struct IMXRT1180XcacheState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[IMXRT1180_XCACHE_NUM_REGS];
};
#endif
