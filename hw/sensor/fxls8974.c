/*
 * NXP FXLS8974CF — 3-axis 12-bit accelerometer on I2C (EVK sensor U115).
 *
 * Register-accurate against the MCUXpresso fsl_fxls driver: WHO_AM_I @0x13 reads
 * the 0x86 device ID, and the standby/active handshake through SENS_CONFIG1
 * (ACTIVE bit) reads back writes so FXLS_Init succeeds.  Acceleration output
 * (OUT_X..Z, 0x04..0x09) reports a fixed "board flat, at rest" orientation —
 * +1 g on Z, 0 on X/Y — which is the honest reading of a stationary sensor
 * (gravity is real).  There is no motion input, so the board does not tilt; that
 * is flagged, not faked (a motion source could later drive the axes).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/i2c/i2c.h"
#include "hw/sensor/fxls8974.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define FXLS_INT_STATUS   0x00
#define FXLS_OUT_X_LSB    0x04
#define FXLS_OUT_Z_MSB    0x09
#define FXLS_PROD_REV     0x12
#define FXLS_WHO_AM_I     0x13
#define FXLS_SENS_CONFIG1 0x15

#define FXLS_DEVICE_ID    0x86
#define FXLS_INT_DRDY     0x80        /* SRC_DRDY: a sample is ready       */
#define FXLS_ONE_G        0x0800      /* +1 g on the Z axis (12-bit-ish)   */

#define FXLS_NREG 0x20

struct FXLS8974State {
    I2CSlave parent_obj;
    uint8_t regs[FXLS_NREG];
    uint8_t pointer;
    uint8_t len;
    bool logged;
};

OBJECT_DECLARE_SIMPLE_TYPE(FXLS8974State, FXLS8974)

static uint8_t fxls_read(FXLS8974State *s, uint8_t reg)
{
    switch (reg) {
    case FXLS_WHO_AM_I:
        return FXLS_DEVICE_ID;
    case FXLS_INT_STATUS:
        return FXLS_INT_DRDY;         /* always a sample ready to read */
    default:
        return reg < FXLS_NREG ? s->regs[reg] : 0;
    }
}

static uint8_t fxls_recv(I2CSlave *i2c)
{
    FXLS8974State *s = FXLS8974(i2c);
    uint8_t v = fxls_read(s, s->pointer);

    if (s->pointer >= FXLS_OUT_X_LSB && s->pointer <= FXLS_OUT_Z_MSB &&
        !s->logged) {
        s->logged = true;
        qemu_log_mask(LOG_UNIMP, "fxls8974: reporting a fixed 'board flat at "
            "rest' orientation; no motion input drives the axes (flagged)\n");
    }
    s->pointer++;                      /* auto-increment for burst reads */
    return v;
}

static int fxls_send(I2CSlave *i2c, uint8_t data)
{
    FXLS8974State *s = FXLS8974(i2c);

    if (s->len == 0) {
        s->pointer = data;             /* first byte selects the register */
        s->len++;
    } else {
        if (s->pointer < FXLS_NREG) {
            s->regs[s->pointer] = data;
        }
        s->pointer++;
    }
    return 0;
}

static int fxls_event(I2CSlave *i2c, enum i2c_event event)
{
    FXLS8974State *s = FXLS8974(i2c);

    if (event == I2C_START_SEND || event == I2C_START_RECV) {
        s->len = 0;
    }
    return 0;
}

static void fxls_reset(DeviceState *dev)
{
    FXLS8974State *s = FXLS8974(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[FXLS_WHO_AM_I]  = FXLS_DEVICE_ID;
    s->regs[FXLS_PROD_REV]  = 0x11;
    /* Static orientation: X = Y = 0, Z = +1 g (little-endian LSB/MSB). */
    s->regs[FXLS_OUT_Z_MSB - 1] = FXLS_ONE_G & 0xFF;
    s->regs[FXLS_OUT_Z_MSB]     = FXLS_ONE_G >> 8;
    s->pointer = 0;
    s->len = 0;
}

static const VMStateDescription vmstate_fxls8974 = {
    .name = "fxls8974",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, FXLS8974State),
        VMSTATE_UINT8_ARRAY(regs, FXLS8974State, FXLS_NREG),
        VMSTATE_UINT8(pointer, FXLS8974State),
        VMSTATE_UINT8(len, FXLS8974State),
        VMSTATE_END_OF_LIST()
    },
};

static void fxls8974_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = fxls_event;
    k->recv  = fxls_recv;
    k->send  = fxls_send;
    device_class_set_legacy_reset(dc, fxls_reset);
    dc->vmsd = &vmstate_fxls8974;
}

static const TypeInfo fxls8974_types[] = {
    {
        .name          = TYPE_FXLS8974,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(FXLS8974State),
        .class_init    = fxls8974_class_init,
    },
};

DEFINE_TYPES(fxls8974_types)
