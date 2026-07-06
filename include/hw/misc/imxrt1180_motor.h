/*
 * i.MX RT1180 virtual-motor plant (behavioural, not silicon).
 *
 * Closes the motor-control loop in emulation: it reads the eFlexPWM duty cycles,
 * runs a first-order permanent-magnet-synchronous-motor model (Clarke/Park ->
 * torque -> integrate velocity and angle), then drives the EQDC position counter
 * and injects the resulting phase currents into the LPADC channels.  With this
 * in place a field-oriented-control loop running on the guest actually spins a
 * (virtual) rotor and senses it back.
 *
 * This is NOT a device on the SoC — it is a simulation object wired to the PWM,
 * EQDC and ADC models.  A running QEMU with no plant leaves those peripherals in
 * their honest "no motor attached" state.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_IMXRT1180_MOTOR_H
#define HW_MISC_IMXRT1180_MOTOR_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"
#include "hw/misc/imxrt1180_pwm.h"
#include "hw/misc/imxrt1180_eqdc.h"
#include "hw/misc/imxrt1180_adc.h"

#define TYPE_IMXRT1180_MOTOR "imxrt1180-motor"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRT1180MotorState, IMXRT1180_MOTOR)

struct IMXRT1180MotorState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    ptimer_state *timer;

    /* Wired to the peripherals this plant drives (set by the SoC). */
    IMXRT1180PWMState  *pwm;    /* reads duty[0..2] = phases A/B/C */
    IMXRT1180EQDCState *eqdc;   /* driven position                */
    IMXRT1180ADCState  *adc_a;  /* phase-A/B current samples      */
    IMXRT1180ADCState  *adc_c;  /* phase-C current samples        */

    /* Continuous state. */
    double theta;   /* mechanical rotor angle (rad, unbounded) */
    double omega;   /* mechanical angular velocity (rad/s)     */

    uint32_t rate_hz;   /* physics update rate */
};

#endif /* HW_MISC_IMXRT1180_MOTOR_H */
