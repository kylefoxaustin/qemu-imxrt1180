/*
 * i.MX RT1180 virtual-motor plant — calibrated dq PMSM model.
 *
 * Ties the motor-control peripherals into a closed physical loop:
 *
 *   eFlexPWM duty (A/B/C) --> phase voltages --> Clarke/Park --> dq stator
 *   currents (with stator-inductance dynamics + back-EMF) --> torque -->
 *   integrate mechanical velocity + angle -->
 *      * EQDC position counter (rotor angle the guest reads back)
 *      * LPADC phase-current samples (what the ADC "measures")
 *
 * This is a full two-axis (dq) permanent-magnet-synchronous-motor model with
 * saliency (Ld != Lq) and a settable constant load torque, parameterised from
 * the MCUXpresso motor-control demo's M1 motor (the 24 V Teknic/Linix on the
 * MIMXRT1180-EVK).  Real electrical dynamics: a step of stator voltage ramps the
 * current with the L/R time constant, torque follows, and the rotor accelerates
 * against its inertia and load.  Enough for a field-oriented-control loop to
 * close and behave like the bench setup.  When the PWM is idle the inverter is
 * tristated, so the rotor coasts FREELY (open stator, no braking current) under
 * the mechanical load -- the physically-correct free-wheel.  Two optional plant
 * refinements, off by default so every existing golden holds:
 *   - a winding-THERMAL model (Rs rises with I^2R heating; -global
 *     imxrt1180-motor.thermal=1) droops the phase current to a closed-form hot
 *     steady state -- tests/imxrt1180-motor-thermal;
 *   - a speed-SQUARED (fan/pump/windage) LOAD term (-global
 *     imxrt1180-motor.load-fan-unms=k) whose coast-down angle has the closed form
 *     theta = (J/k) ln(1 + k*w0/B) -- tests/imxrt1180-motor-load;
 *   - MAGNETIC SATURATION (-global imxrt1180-motor.sat-isat-ma=i_sat): the
 *     incremental inductance falls with current, Ld_eff = Ld0/(1 + |id|/i_sat),
 *     so the current-rise time constant shrinks at high current -- verified by
 *     the d-axis rise-time ratio (tests/imxrt1180-motor-sat).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include <math.h>
#include "hw/misc/imxrt1180_motor.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/* M1 motor parameters (MCUXpresso mc_pmsm m1_pmsm_appconfig.h / MCAT). */
#define M_PP       4          /* pole pairs                          */
#define M_RS       0.54       /* phase resistance (ohm)              */
#define M_LD       0.0003356  /* d-axis inductance (H)               */
#define M_LQ       0.000218   /* q-axis inductance (H)               */
#define M_KT       0.05477461 /* torque constant (N*m/A)             */
#define M_PSI      (M_KT / (1.5 * M_PP))  /* PM flux linkage (Wb)    */
#define M_J        0.00001    /* rotor inertia (kg*m^2)              */
#define M_B        0.0001     /* viscous damping (N*m*s)             */
#define M_VBUS     24.0       /* DC-bus voltage (V)                  */
#define M_IMAX     8.25       /* rated peak current (A)              */

/* Copper resistance temperature coefficient (per deg C).  Rs is specified at the
 * ambient temperature; the winding heats from its own I^2R loss and Rs rises with
 * it.  M_RS above is thus the COLD value Rs(T_amb). */
#define M_ALPHA_CU 0.00393

/*
 * Encoder counts per revolution.  The mc_pmsm encoder driver configures the EQDC
 * modulus LMOD = 4*M1_POSPE_ENC_PULSES - 1 and scales ALL its position/speed gains
 * for 4x the line count -- for the EVK's 2000-line quadrature encoder that is
 * 8000 cts/rev.  A mismatched CPR here makes the FOC read the rotor angle at the
 * wrong rate, so its dq frame diverges from the plant's (torque current lands in
 * the d-axis) and the closed loop cannot sustain a spin.
 */
#define M_ENC_PULSES 2000     /* M1_POSPE_ENC_PULSES (EVK encoder lines) */
#define M_CPR      (4 * M_ENC_PULSES)   /* 8000 quadrature counts/rev */
#define M_ADC_MID  0x8000
/*
 * ADC code span per amp.  The mc_pmsm driver reads a phase current as
 *     I = ((raw*12/11 - offset) << 1) / 32768 * M1_I_MAX
 * (offset is the 0-current calibration = MID*12/11; <<1 is the sign-halved-range
 * recovery; the frac16 full scale 32768 maps to M1_I_MAX).  Inverting, the code
 * the converter must present for a current i is
 *     raw = MID + i * 32768 / (2 * (12/11) * M1_I_MAX)
 * i.e. M_CUR_FS = 1820 cts/A, NOT 0x7000/I_MAX = 3475.  The old span read ~1.9x
 * high through the driver's decode and tripped its over-current fault at startup.
 * (tests/imxrt1180-motor and -adc-ab carry the matching goldens.)
 */
#define M_CUR_FS   (32768.0 / (2.0 * (12.0 / 11.0) * M_IMAX))

#define M_RATE_DEFAULT 50000u    /* physics steps/s (fast dq dynamics)  */

/* kCLOCK_Root_Bus_Wakeup (fsl_clock.h) -- the EQDC QD-timer clock root. */
#define IMXRT1180_CLKROOT_BUS_WAKEUP 4

#define TWO_PI (2.0 * M_PI)
#define SQRT3_2 0.8660254037844386

/* Channels the plant drives (a modelling choice; see README). */
#define CH_PHASE_A 5      /* ADC1 */
#define CH_PHASE_B 6      /* ADC1 */
#define CH_PHASE_C 2      /* ADC2 */
#define CH_UDCB    4      /* ADC1 — DC-bus voltage (mc_pmsm: M1_ADC1_UDCB = 4) */

/*
 * DC-bus voltage sense.  Full scale of the EVK's divider is M1_U_DCB_MAX =
 * 60.8 V (mc_pmsm m1_pmsm_appconfig.h).
 *
 * The mc_pmsm driver (mcdrv_adc_imxrt118x.c) does NOT read the LPADC result as a
 * plain 16-bit fraction: it treats it as a Q15 frac16 and applies the EVK's
 * VIN_HW/VIN_MAX = 11/12 board compensation before scaling:
 *     U_dcb = (raw * 12/11 / 32768) * 60.8 V
 * so the code the converter must PRESENT for a bus voltage v is the inverse,
 *     raw = v / 60.8 * 32768 * 11/12
 * NOT v/60.8 * 0xFFFF.  The old 16-bit-full-scale code was 0xFFFF/(32768*11/12)
 * = 2.18x too high for that decode: it read 24 V as ~52 V and tripped the stock
 * FOC's 30 V over-voltage lockout, so the cm7 mc_pmsm demo never left AppStop.
 * (tests/imxrt1180-motor decodes with the same convention.)
 *
 * This channel MUST be driven by the plant.  Left at the ADC's neutral
 * mid-scale placeholder it reads 0x8000 -> 30.4 V, which is a *plausible* bus
 * voltage (and, as it happens, just over the demo's 30.0 V overvoltage trip) --
 * the dangerous kind of fake: an FOC loop normalises its duty cycles by U_DCB
 * and runs its under/over-voltage protection off it, so a fabricated value has
 * the loop regulating against, and protecting against, a number nothing
 * measured.  Reporting the bus voltage the plant *actually applies* to the
 * phases makes it a real measurement of the modelled system rather than a
 * constant.
 *
 * (The bus is ideal/stiff: no sag under load, since that needs bus capacitance
 * and inverter DC-link current.  Flagged as future work, not faked.)
 */
#define M_UDCB_FS   60.8            /* M1_U_DCB_MAX (V)                       */
#define M_UDCB_COMP (11.0 / 12.0)   /* VIN_HW/VIN_MAX; the driver applies 12/11 */

static uint16_t voltage_to_code(double v)
{
    double code = (v / M_UDCB_FS) * 32768.0 * M_UDCB_COMP;

    if (code < 0) {
        code = 0;
    } else if (code > 65535.0) {
        code = 65535.0;
    }
    return (uint16_t)code;
}

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
    if (!run && fabs(s->omega) < 1e-4 &&
        fabs(s->id) < 1e-3 && fabs(s->iq) < 1e-3) {
        return;                        /* dormant: motor idle, currents decayed */
    }

    /* Rotor electrical angle (needed by both the driven and the coasting path). */
    double theta_e = M_PP * s->theta;
    double c = cos(theta_e), sn = sin(theta_e);

    double id, iq, te;

    if (run) {
        /* Phase voltages from the PWM duty (centred: 0.5 duty = 0 V). */
        double va = (imxrt1180_pwm_duty(s->pwm, 0) / 1000.0 - 0.5) * M_VBUS;
        double vb = (imxrt1180_pwm_duty(s->pwm, 1) / 1000.0 - 0.5) * M_VBUS;
        double vc = (imxrt1180_pwm_duty(s->pwm, 2) / 1000.0 - 0.5) * M_VBUS;

        /* Amplitude-invariant Clarke, then Park into the rotor (dq) frame. */
        double valpha = (2.0 * va - vb - vc) / 3.0;
        double vbeta  = (vb - vc) / (2.0 * SQRT3_2);
        double vd =  valpha * c + vbeta * sn;
        double vq = -valpha * sn + vbeta * c;

        /*
         * Winding resistance.  Cold (thermal off) it is the datasheet Rs; with
         * the thermal model on it rises with the winding temperature the I^2R loss
         * drives (copper tempco), so a hard-working motor's phase current droops
         * -- a real, closed-form-verifiable effect (tests/imxrt1180-motor-thermal).
         */
        double rs = M_RS;
        if (s->thermal) {
            rs = M_RS * (1.0 + M_ALPHA_CU * (s->temp_c - (double)s->therm_amb_c));
        }

        /*
         * Magnetic saturation: the incremental inductance falls as current rises
         * (the iron saturates), Ld_eff = Ld0/(1 + |id|/i_sat).  Off by default
         * (i_sat = 0 -> constant Ld0/Lq0).  This is exact for the di/dt term the
         * current transient rides on; it is also applied to the w_e cross-coupling
         * terms as a first approximation -- tests/imxrt1180-motor-sat exercises the
         * w=0 d-axis transient, where only the di/dt inductance appears, so the
         * approximation is untested and flagged, not faked.
         */
        double ld = M_LD, lq = M_LQ;
        if (s->sat_isat_ma) {
            double isat = s->sat_isat_ma / 1000.0;
            ld = M_LD / (1.0 + fabs(s->id) / isat);
            lq = M_LQ / (1.0 + fabs(s->iq) / isat);
        }

        /*
         * dq stator-current dynamics (with cross-coupling + PM back-EMF):
         *   L_d did/dt = v_d - R i_d + w_e L_q i_q
         *   L_q diq/dt = v_q - R i_q - w_e L_d i_d - w_e psi_m
         */
        double omega_e = M_PP * s->omega;
        double did = (vd - rs * s->id + omega_e * lq * s->iq) / ld;
        double diq = (vq - rs * s->iq - omega_e * ld * s->id
                         - omega_e * M_PSI) / lq;
        s->id += did * dt;
        s->iq += diq * dt;
        id = s->id;
        iq = s->iq;

        /*
         * Winding thermal state: C_th dT/dt = P_loss - (T - T_amb)/R_th, copper
         * loss P_loss = 1.5 (id^2 + iq^2) rs.  tau = R_th*C_th, so dT/dt =
         * (P*R_th - dT_rise)/tau; steady state T_ss = T_amb + P_loss*R_th.
         */
        if (s->thermal && s->therm_tau_ms > 0) {
            double rth = s->therm_rth_mcw / 1000.0;    /* degC/W  */
            double tau = s->therm_tau_ms / 1000.0;     /* s       */
            double p_loss = 1.5 * (id * id + iq * iq) * rs;
            double dtr = s->temp_c - (double)s->therm_amb_c;
            s->temp_c += (p_loss * rth - dtr) / tau * dt;
        }

        /* Electromagnetic torque (magnet + reluctance/saliency). */
        te = 1.5 * M_PP * (M_PSI * iq + (M_LD - M_LQ) * id * iq);
    } else {
        /*
         * PWM idle: the inverter is tristated, so the stator is open-circuit --
         * no phase current can flow and there is no electromagnetic torque.  The
         * rotor coasts FREELY under the mechanical load alone (a tristated
         * inverter free-wheels; it does NOT dynamically brake).  Zeroing the
         * currents here is what makes the coast-down a clean mechanical problem
         * (tests/imxrt1180-motor-load).
         */
        s->id = 0.0;
        s->iq = 0.0;
        id = 0.0;
        iq = 0.0;
        te = 0.0;
    }

    /*
     * Mechanics.  Load torque = constant term (load-mnm) + a speed-SQUARED
     * (fan / pump / windage) term k*w*|w| (load-fan-unms, micro-N*m per (rad/s)^2)
     * -- the physical shape of a rotating load, always opposing motion.  With the
     * drive removed the rotor's total coast-down angle has a closed form,
     * theta = (J/k) ln(1 + k*w0/B) -- value-verified by tests/imxrt1180-motor-load.
     */
    double t_load = s->load_mnm / 1000.0
                    + (s->load_fan_unms / 1.0e6) * s->omega * fabs(s->omega);
    s->omega += (te - M_B * s->omega - t_load) / M_J * dt;
    s->theta += s->omega * dt;

    /* Inverse Park/Clarke -> phase currents for the ADC. */
    double ialpha = id * c - iq * sn;
    double ibeta  = id * sn + iq * c;
    double ia = ialpha;
    double ib = -0.5 * ialpha + SQRT3_2 * ibeta;
    double ic = -0.5 * ialpha - SQRT3_2 * ibeta;

    if (s->adc_a) {
        uint16_t code_ia = current_to_code(ia);
        uint16_t code_ib = current_to_code(ib);
        uint16_t code_ud = voltage_to_code(M_VBUS);   /* bus V the plant applies */
        /* A-side single-ended mapping (the hand-written M33 FOC test reads these:
         * Ia on ch5, Ib on ch6, UDCB on ch4). */
        imxrt1180_adc_set_channel_input(s->adc_a, CH_PHASE_A, IMXRT1180_ADC_SIDE_A, code_ia);
        imxrt1180_adc_set_channel_input(s->adc_a, CH_PHASE_B, IMXRT1180_ADC_SIDE_A, code_ib);
        imxrt1180_adc_set_channel_input(s->adc_a, CH_UDCB,    IMXRT1180_ADC_SIDE_A, code_ud);
        /*
         * The stock mc_pmsm cm7 demo reads Ia/Ib as the A/B sides of ONE channel
         * (ADC1 CMD1 = DualSingleEndBothSide on ch5: A5=Ia -> RESFIFO[0],
         * B5=Ib -> RESFIFO[1]) and UDCB on B4.  Drive those B-side muxes too.
         */
        imxrt1180_adc_set_channel_input(s->adc_a, CH_PHASE_A, IMXRT1180_ADC_SIDE_B, code_ib);
        imxrt1180_adc_set_channel_input(s->adc_a, CH_UDCB,    IMXRT1180_ADC_SIDE_B, code_ud);
    }
    if (s->adc_c) {
        /* mc_pmsm: ADC2 CMD1 = DualSingleEndBothSide on ch2, A2 = Ic -> RESFIFO[0]. */
        imxrt1180_adc_set_channel_input(s->adc_c, CH_PHASE_C, IMXRT1180_ADC_SIDE_A,
                                        current_to_code(ic));
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

        /*
         * Present the EQDC's hardware speed measurement.  The mc_pmsm qdc2 driver
         * reads speed as POSDH / POSDPERH = counts-per-QD-clock, then scales by
         * (2*pi*QDTimerFreq)/(4*pulses) -- so to report the shaft's real velocity
         * we set POSD = vel_counts_per_sec / QDTimerFreq * POSDPER for a fixed
         * period window.  QDTimerFreq = Bus_Wakeup clock >> FILT[PRSC], exactly
         * what the driver derived when it built its speed constant.
         */
        uint32_t qd_hz = s->pwm
            ? imxrt1180_ccm_root_hz(s->pwm->ccm, IMXRT1180_CLKROOT_BUS_WAKEUP)
              >> imxrt1180_eqdc_filt_prsc(s->eqdc)
            : 0;
        if (qd_hz) {
            double vel_cnt_s = s->omega * M_CPR / TWO_PI;   /* signed mech cts/s */
            const uint16_t posdper = 2048;                  /* measurement window */
            double posd = vel_cnt_s / (double)qd_hz * (double)posdper;
            if (posd > 32767.0) {
                posd = 32767.0;
            } else if (posd < -32768.0) {
                posd = -32768.0;
            }
            /* LASTEDGE = QD clocks between single-count edges (low-speed path);
             * 0xFFFF = no edge seen (shaft stopped). */
            uint16_t lastedge = 0xFFFF;
            double avel = fabs(vel_cnt_s);
            if (avel > 1.0) {
                double le = (double)qd_hz / avel;
                lastedge = le < 65535.0 ? (uint16_t)le : 0xFFFF;
            }
            imxrt1180_eqdc_set_speed(s->eqdc, (int16_t)llround(posd),
                                     posdper, lastedge);
        }
    }
}

static void imxrt1180_motor_reset(DeviceState *dev)
{
    IMXRT1180MotorState *s = IMXRT1180_MOTOR(dev);

    s->theta = 0.0;
    s->omega = (double)s->init_mrads / 1000.0;  /* 0 normally; !=0 -> coast-down */
    s->id = 0.0;
    s->iq = 0.0;
    s->temp_c = (double)s->therm_amb_c;   /* winding starts at ambient */
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

static const Property imxrt1180_motor_properties[] = {
    /* Constant mechanical load torque, in milli-N*m (a simple load profile). */
    DEFINE_PROP_UINT32("load-mnm", IMXRT1180MotorState, load_mnm, 0),
    /* Speed-squared (fan/pump/windage) load, micro-N*m per (rad/s)^2. */
    DEFINE_PROP_UINT32("load-fan-unms", IMXRT1180MotorState, load_fan_unms, 0),
    /* Initial rotor speed (milli-rad/s): 0 = at rest; seeds a coast-down test. */
    DEFINE_PROP_UINT32("init-mrads", IMXRT1180MotorState, init_mrads, 0),
    /* Magnetic saturation current (mA): 0 = off (constant Ld0/Lq0). */
    DEFINE_PROP_UINT32("sat-isat-ma", IMXRT1180MotorState, sat_isat_ma, 0),
    DEFINE_PROP_UINT32("rate-hz", IMXRT1180MotorState, rate_hz, 0),
    /* Winding-thermal model (off by default; see the struct comment). */
    DEFINE_PROP_UINT32("thermal", IMXRT1180MotorState, thermal, 0),
    DEFINE_PROP_UINT32("therm-rth-mcw", IMXRT1180MotorState, therm_rth_mcw, 5000),
    DEFINE_PROP_UINT32("therm-tau-ms", IMXRT1180MotorState, therm_tau_ms, 30000),
    DEFINE_PROP_UINT32("therm-amb-c", IMXRT1180MotorState, therm_amb_c, 25),
};

static void imxrt1180_motor_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imxrt1180_motor_realize;
    device_class_set_legacy_reset(dc, imxrt1180_motor_reset);
    dc->vmsd = &vmstate_imxrt1180_motor;
    device_class_set_props(dc, imxrt1180_motor_properties);
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
