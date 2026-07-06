/*
 * i.MX RT1180 virtual-motor plant — first-order PMSM model.
 *
 * Ties the motor-control peripherals into a closed physical loop:
 *
 *   eFlexPWM duty (A/B/C) --> phase voltages --> Clarke/Park --> (R-only) dq
 *   currents --> torque --> integrate mechanical velocity + angle -->
 *      * EQDC position counter (rotor angle the guest reads back)
 *      * LPADC phase-current samples (what the ADC "measures")
 *
 * The electrical model is first-order (resistive, no stator-inductance dynamics)
 * with q-axis back-EMF; the mechanical model is a damped inertia.  It is a
 * lumped, normalised plant — enough to make a field-oriented-control loop close
 * and a virtual rotor spin/hold, NOT a calibrated model of a specific motor.
 * That approximation is deliberate and documented (a detailed dq/back-EMF model
 * with real parameters is future work).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include <math.h>
#include "hw/misc/imxrt1180_motor.h"
#include "migration/vmstate.h"

/* Normalised lumped parameters, tuned for a stable, visible rotor response. */
#define M_R        1.00      /* phase resistance                */
#define M_KT       1.00      /* torque constant                 */
#define M_KE       0.05      /* back-EMF constant               */
#define M_J        0.0005    /* rotor inertia                   */
#define M_B        0.001     /* viscous damping                 */
#define M_POLES    1         /* pole pairs (1 -> elec == mech)  */
#define M_TLOAD    0.0       /* load torque                     */

#define M_CPR      4096      /* encoder counts per revolution   */
#define M_CUR_FS   28672.0   /* ADC code span for +/-1.0 current */
#define M_ADC_MID  0x8000

#define M_RATE_DEFAULT 20000u    /* physics steps/s */

#define TWO_PI (2.0 * M_PI)
#define SQRT3_2 0.8660254037844386

/* Channels the plant drives (a modelling choice; see README). */
#define CH_PHASE_A 5      /* ADC1 */
#define CH_PHASE_B 6      /* ADC1 */
#define CH_PHASE_C 2      /* ADC2 */

static uint16_t current_to_code(double i)
{
    double code = (double)M_ADC_MID + i * M_CUR_FS;
    if (code < 0) {
        code = 0;
    } else if (code > 0xFFFF) {
        code = 0xFFFF;
    }
    return (uint16_t)code;
}

static void motor_step(void *opaque)
{
    IMXRT1180MotorState *s = IMXRT1180_MOTOR(opaque);
    double dt = 1.0 / (double)s->rate_hz;

    bool run = s->pwm && (imxrt1180_pwm_running(s->pwm, 0) ||
                          imxrt1180_pwm_running(s->pwm, 1) ||
                          imxrt1180_pwm_running(s->pwm, 2));

    /*
     * Stay dormant until a motor is actually driven: while the PWM is idle and
     * the rotor is at rest, don't touch the EQDC/ADC at all, so peripherals a
     * non-motor workload uses are left exactly as firmware set them.  (Once
     * driven, the plant keeps updating while the rotor coasts down.)
     */
    if (!run && fabs(s->omega) < 1e-4) {
        return;
    }

    /* Phase voltages from the PWM duty (centred: 0.5 duty = 0 V). */
    double va = 0, vb = 0, vc = 0;
    if (run) {
        va = imxrt1180_pwm_duty(s->pwm, 0) / 1000.0 - 0.5;
        vb = imxrt1180_pwm_duty(s->pwm, 1) / 1000.0 - 0.5;
        vc = imxrt1180_pwm_duty(s->pwm, 2) / 1000.0 - 0.5;
    }

    /* Amplitude-invariant Clarke transform. */
    double valpha = (2.0 * va - vb - vc) / 3.0;
    double vbeta  = (vb - vc) / (2.0 * SQRT3_2);

    /* Park transform into the rotor (dq) frame. */
    double theta_e = M_POLES * s->theta;
    double c = cos(theta_e), sn = sin(theta_e);
    double vd =  valpha * c + vbeta * sn;
    double vq = -valpha * sn + vbeta * c;

    /* First-order (resistive) currents; back-EMF opposes q. */
    double omega_e = M_POLES * s->omega;
    double id = vd / M_R;
    double iq = (vq - M_KE * omega_e) / M_R;

    /* Electromagnetic torque, then the mechanical integration. */
    double te = M_KT * iq * M_POLES;
    s->omega += (te - M_B * s->omega - M_TLOAD) / M_J * dt;
    s->theta += s->omega * dt;

    /* Inverse Park/Clarke -> phase currents for the ADC. */
    double ialpha = id * c - iq * sn;
    double ibeta  = id * sn + iq * c;
    double ia = ialpha;
    double ib = -0.5 * ialpha + SQRT3_2 * ibeta;
    double ic = -0.5 * ialpha - SQRT3_2 * ibeta;

    if (s->adc_a) {
        imxrt1180_adc_set_channel_input(s->adc_a, CH_PHASE_A, current_to_code(ia));
        imxrt1180_adc_set_channel_input(s->adc_a, CH_PHASE_B, current_to_code(ib));
    }
    if (s->adc_c) {
        imxrt1180_adc_set_channel_input(s->adc_c, CH_PHASE_C, current_to_code(ic));
    }

    /* Encoder position from the mechanical angle. */
    if (s->eqdc) {
        double rounds = s->theta / TWO_PI;
        int64_t total = (int64_t)llround(rounds * M_CPR);
        int64_t rev = total / M_CPR;
        int64_t pos = total % M_CPR;
        if (pos < 0) {                 /* keep pos in [0, CPR) */
            pos += M_CPR;
            rev -= 1;
        }
        imxrt1180_eqdc_set_position(s->eqdc, (uint32_t)pos, (uint16_t)rev);
    }
}

static void imxrt1180_motor_reset(DeviceState *dev)
{
    IMXRT1180MotorState *s = IMXRT1180_MOTOR(dev);

    s->theta = 0.0;
    s->omega = 0.0;
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, s->rate_hz);
    ptimer_set_limit(s->timer, 1, 1);
    ptimer_run(s->timer, 0);           /* free-running physics clock */
    ptimer_transaction_commit(s->timer);
}

static void imxrt1180_motor_realize(DeviceState *dev, Error **errp)
{
    IMXRT1180MotorState *s = IMXRT1180_MOTOR(dev);

    if (s->rate_hz == 0) {
        s->rate_hz = M_RATE_DEFAULT;
    }
    s->timer = ptimer_init(motor_step, s, PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                                          PTIMER_POLICY_NO_IMMEDIATE_RELOAD);
}

/* The continuous rotor state (theta/omega) is a simulation aid and is not
 * migrated; on migration the virtual rotor resets to standstill. */
static const VMStateDescription vmstate_imxrt1180_motor = {
    .name = TYPE_IMXRT1180_MOTOR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PTIMER(timer, IMXRT1180MotorState),
        VMSTATE_END_OF_LIST()
    },
};

static void imxrt1180_motor_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_motor_realize;
    device_class_set_legacy_reset(dc, imxrt1180_motor_reset);
    dc->vmsd = &vmstate_imxrt1180_motor;
    dc->user_creatable = false;
}

static const TypeInfo imxrt1180_motor_types[] = {
    {
        .name          = TYPE_IMXRT1180_MOTOR,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMXRT1180MotorState),
        .class_init    = imxrt1180_motor_class_init,
    },
};

DEFINE_TYPES(imxrt1180_motor_types)
