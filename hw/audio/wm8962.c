/*
 * Cirrus/Wolfson WM8962 audio codec — I2C CONTROL PLANE only.
 *
 * On the MIMXRT1180-EVK the WM8962 is the I2S SLAVE (the SAI is bit-clock and
 * frame master), so the codec never sees or shapes the audio SAMPLE stream — that
 * leaves the SoC entirely through the SAI's own audio backend (the SAI's wav IS
 * the DAC output in this model).  The codec's whole job here is to ANSWER the
 * control driver over I2C so `CODEC_Init`/`WM8962_Init` completes instead of
 * `assert(false)`-ing, letting the stock `sai/edma_transfer` demo proceed to
 * stream.
 *
 * So this is a faithful I2C register file, NOT an audio device.  It is honestly
 * control-plane: it does not open an audio voice, does not touch samples, and
 * makes no claim about analog output.  Modelling it as an audio data-path device
 * would be the fabrication — the sound genuinely comes out of the SAI.
 *
 * Protocol (fsl_wm8962.c): a WM8962 register is a 2-byte address followed by a
 * 16-bit big-endian value; one register per transaction.  Reads latch the address
 * in the preceding write phase, then clock the value out MSB-first.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/i2c/i2c.h"
#include "hw/audio/wm8962.h"
#include "migration/vmstate.h"
#include "qom/object.h"

/* Register map spans up to INT_STATUS_2 @ 0x231 (fsl_wm8962.h) plus the write
 * sequencer; hold the whole space so a stored write always reads back. */
#define WM8962_NREG 0x300

/*
 * WM8962_StartSequence() writes the write-sequencer control register and then
 * POLLS 0x5D, looping while bit0 (BUSY) is set (fsl_wm8962.c).  We complete every
 * sequence instantly, so bit0 always reads 0 -- the same "ready now" model the ADC
 * uses for calibration.  Reported on the READ so a driver-written BUSY bit never
 * hangs the poll.
 */
#define WM8962_WSEQ_BUSY_REG  0x5D
#define WM8962_WSEQ_BUSY_BIT  0x0001

struct WM8962State {
    I2CSlave parent_obj;

    uint16_t regs[WM8962_NREG];
    uint16_t pointer;      /* current register address                     */
    uint8_t  phase;        /* 0,1 = address byte; 2,3 = data byte           */
    uint8_t  data_hi;      /* first (MSB) data byte, awaiting the LSB        */
    uint8_t  rd_lsb;       /* 0 = next read byte is MSB, 1 = LSB            */
};

OBJECT_DECLARE_SIMPLE_TYPE(WM8962State, WM8962)

static uint16_t wm8962_regread(WM8962State *s, uint16_t reg)
{
    uint16_t v = (reg < WM8962_NREG) ? s->regs[reg] : 0;

    if (reg == WM8962_WSEQ_BUSY_REG) {
        v &= (uint16_t)~WM8962_WSEQ_BUSY_BIT;   /* sequencer is never busy */
    }
    return v;
}

static uint8_t wm8962_recv(I2CSlave *i2c)
{
    WM8962State *s = WM8962(i2c);
    uint16_t v = wm8962_regread(s, s->pointer);
    uint8_t b;

    if (!s->rd_lsb) {
        b = (uint8_t)(v >> 8);                  /* MSB first (big-endian) */
        s->rd_lsb = 1;
    } else {
        b = (uint8_t)(v & 0xFF);
        s->rd_lsb = 0;
        s->pointer++;                           /* auto-increment for bursts */
    }
    return b;
}

static int wm8962_send(I2CSlave *i2c, uint8_t data)
{
    WM8962State *s = WM8962(i2c);

    switch (s->phase) {
    case 0:                                     /* register address MSB */
        s->pointer = (uint16_t)data << 8;
        s->phase = 1;
        break;
    case 1:                                     /* register address LSB */
        s->pointer |= data;
        s->phase = 2;
        break;
    case 2:                                     /* data MSB */
        s->data_hi = data;
        s->phase = 3;
        break;
    default:                                    /* data LSB -> commit 16 bits */
        if (s->pointer < WM8962_NREG) {
            s->regs[s->pointer] = ((uint16_t)s->data_hi << 8) | data;
        }
        s->pointer++;                           /* auto-increment for bursts */
        s->phase = 2;                           /* another value may follow */
        break;
    }
    return 0;
}

static int wm8962_event(I2CSlave *i2c, enum i2c_event event)
{
    WM8962State *s = WM8962(i2c);

    if (event == I2C_START_SEND) {
        s->phase = 0;                           /* fresh address */
    } else if (event == I2C_START_RECV) {
        s->rd_lsb = 0;                          /* address already latched */
    }
    return 0;
}

static void wm8962_reset(DeviceState *dev)
{
    WM8962State *s = WM8962(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->pointer = 0;
    s->phase = 0;
    s->data_hi = 0;
    s->rd_lsb = 0;
}

static const VMStateDescription vmstate_wm8962 = {
    .name = "wm8962",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, WM8962State),
        VMSTATE_UINT16_ARRAY(regs, WM8962State, WM8962_NREG),
        VMSTATE_UINT16(pointer, WM8962State),
        VMSTATE_UINT8(phase, WM8962State),
        VMSTATE_UINT8(data_hi, WM8962State),
        VMSTATE_UINT8(rd_lsb, WM8962State),
        VMSTATE_END_OF_LIST()
    },
};

static void wm8962_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = wm8962_event;
    k->recv  = wm8962_recv;
    k->send  = wm8962_send;
    device_class_set_legacy_reset(dc, wm8962_reset);
    dc->vmsd = &vmstate_wm8962;
}

static const TypeInfo wm8962_types[] = {
    {
        .name          = TYPE_WM8962,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(WM8962State),
        .class_init    = wm8962_class_init,
    },
};

DEFINE_TYPES(wm8962_types)
