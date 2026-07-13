/*
 * NXP i.MX RT1180 CCM — Clock Controller Module (clock roots, gates, observe).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_CCM_H
#define HW_MISC_IMXRT1180_CCM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "hw/misc/imxrt1180_anadig.h"

#define TYPE_IMXRT1180_CCM "imxrt1180-ccm"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180CCMState, IMXRT1180_CCM)

#define IMXRT1180_CCM_SIZE 0x10000

/* CCM_CLOCK_ROOT_COUNT, PERI_CCM.h.  Asserted against the mux table at build time. */
#define IMXRT1180_CCM_NROOT 74

struct IMXRT1180CCMState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[IMXRT1180_CCM_SIZE / 4];

    /* The PLL/OSC frequencies are not constants -- they are COMPUTED from the
     * ANADIG registers the guest wrote, exactly as fsl_clock.c does it. */
    IMXRT1180AnadigState *anadig;
};

/*
 * Frequency of clock root `root` (a kCLOCK_Root_* index), derived from
 * CLOCK_ROOT[root].CONTROL's MUX and DIV fields and the ANADIG PLL/OSC state.
 *
 * CALL THIS AT THE POINT OF USE, NEVER AT REALIZE.  At reset every root reads
 * CONTROL == 0, which is MUX=OscRc24M, DIV=0 -- i.e. 24 MHz.  The guest then
 * rewrites the roots in BOARD_InitBootClocks(), and some examples re-mux again
 * later (lpit_pwm moves LPIT3 to OscRc24M *after* boot).  A peripheral that
 * latches its frequency at realize sees the RESET value forever -- which is
 * precisely the 24 MHz that six timers in this model used to hardcode.
 *
 * Returns 0 if the frequency cannot be derived (unknown source).  0 is a
 * REFUSAL, not a frequency: callers must not divide by it.
 */
uint32_t imxrt1180_ccm_root_hz(IMXRT1180CCMState *s, unsigned root);

#endif /* HW_MISC_IMXRT1180_CCM_H */
