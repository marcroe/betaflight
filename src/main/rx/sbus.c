/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "platform.h"

#include "build_config.h"

#include "drivers/system.h"

#include "drivers/gpio.h"
#include "drivers/inverter.h"

#include "drivers/serial.h"
#include "drivers/serial_uart.h"
#include "io/serial.h"

#include "rx/rx.h"
#include "rx/sbus.h"

/*
 * Observations
 *
 * FrSky X8R
 * time between frames: 6ms.
 * time to send frame: 3ms.
*
 * Futaba R6208SB/R6303SB
 * time between frames: 11ms.
 * time to send frame: 3ms.
 */

#define SBUS_TIME_NEEDED_PER_FRAME 3000

#ifndef CJMCU
//#define DEBUG_SBUS_PACKETS
#endif

#ifdef DEBUG_SBUS_PACKETS
static uint16_t sbusStateFlags = 0;

#define SBUS_STATE_FAILSAFE (1 << 0)
#define SBUS_STATE_SIGNALLOSS (1 << 1)

#endif

#define SBUS_MAX_CHANNEL 18
#define SBUS_FRAME_SIZE 25

#define SBUS_FRAME_BEGIN_BYTE 0x0F
#define SBUS_FRAME_END_BYTE 0x00

#define SBUS_BAUDRATE 100000

#define SBUS_DIGITAL_CHANNEL_MIN 173
#define SBUS_DIGITAL_CHANNEL_MAX 1812

static bool sbusFrameDone = false;
static void sbusDataReceive(uint16_t c);
static uint16_t sbusReadRawRC(rxRuntimeConfig_t *rxRuntimeConfig, uint8_t chan);

static uint32_t sbusChannelData[SBUS_MAX_CHANNEL];

static serialPort_t *sBusPort;


void sbusUpdateSerialRxFunctionConstraint(functionConstraint_t *functionConstraint)
{
    functionConstraint->minBaudRate = SBUS_BAUDRATE;
    functionConstraint->maxBaudRate = SBUS_BAUDRATE;
    functionConstraint->requiredSerialPortFeatures = SPF_SUPPORTS_CALLBACK | SPF_SUPPORTS_SBUS_MODE;
}

bool sbusInit(rxConfig_t *rxConfig, rxRuntimeConfig_t *rxRuntimeConfig, rcReadRawDataPtr *callback)
{
    int b;

    sBusPort = openSerialPort(FUNCTION_SERIAL_RX, sbusDataReceive, SBUS_BAUDRATE, (portMode_t)(MODE_RX | MODE_SBUS), SERIAL_INVERTED);

    for (b = 0; b < SBUS_MAX_CHANNEL; b++)
        sbusChannelData[b] = (1.6f * rxConfig->midrc) - 1408;
    if (callback)
        *callback = sbusReadRawRC;
    rxRuntimeConfig->channelCount = SBUS_MAX_CHANNEL;

    return sBusPort != NULL;
}

#define SBUS_FLAG_CHANNEL_17        (1 << 0)
#define SBUS_FLAG_CHANNEL_18        (1 << 1)
#define SBUS_FLAG_SIGNAL_LOSS       (1 << 2)
#define SBUS_FLAG_FAILSAFE_ACTIVE   (1 << 3)

struct sbusFrame_s {
    uint8_t syncByte;
    // 176 bits of data (11 bits per channel * 16 channels) = 22 bytes.
    unsigned int chan0 : 11;
    unsigned int chan1 : 11;
    unsigned int chan2 : 11;
    unsigned int chan3 : 11;
    unsigned int chan4 : 11;
    unsigned int chan5 : 11;
    unsigned int chan6 : 11;
    unsigned int chan7 : 11;
    unsigned int chan8 : 11;
    unsigned int chan9 : 11;
    unsigned int chan10 : 11;
    unsigned int chan11 : 11;
    unsigned int chan12 : 11;
    unsigned int chan13 : 11;
    unsigned int chan14 : 11;
    unsigned int chan15 : 11;
    uint8_t flags;
    uint8_t endByte;
} __attribute__ ((__packed__));

typedef union {
    uint8_t bytes[SBUS_FRAME_SIZE];
    struct sbusFrame_s frame;
} sbusFrame_t;

static sbusFrame_t sbusFrame;

// Receive ISR callback
static void sbusDataReceive(uint16_t c)
{
    static uint8_t sbusFramePosition = 0;
    static uint32_t sbusFrameStartAt = 0;
    uint32_t now = micros();

    int32_t sbusFrameTime = now - sbusFrameStartAt;

    if (sbusFrameTime > (long)(SBUS_TIME_NEEDED_PER_FRAME + 500)) {
        sbusFramePosition = 0;
    }

    sbusFrame.bytes[sbusFramePosition] = (uint8_t)c;

    if (sbusFramePosition == 0) {
        if (c != SBUS_FRAME_BEGIN_BYTE) {
            return;
        }
        sbusFrameStartAt = now;
    }

    sbusFramePosition++;

    if (sbusFramePosition == SBUS_FRAME_SIZE) {
        if (sbusFrame.frame.endByte == SBUS_FRAME_END_BYTE) {
            sbusFrameDone = true;
#ifdef DEBUG_SBUS_PACKETS
            debug[2] = sbusFrameTime;
#endif
        }
    } else {
        sbusFrameDone = false;
    }
}

bool sbusFrameComplete(void)
{
    if (!sbusFrameDone) {
        return false;
    }
    sbusFrameDone = false;

#ifdef DEBUG_SBUS_PACKETS
    debug[1] = sbusFrame.frame.flags;
#endif

    if (sbusFrame.frame.flags & SBUS_FLAG_SIGNAL_LOSS) {
        // internal failsafe enabled and rx failsafe flag set
#ifdef DEBUG_SBUS_PACKETS
        sbusStateFlags |= SBUS_STATE_SIGNALLOSS;
        debug[0] |= SBUS_STATE_SIGNALLOSS;
#endif
    }
    if (sbusFrame.frame.flags & SBUS_FLAG_FAILSAFE_ACTIVE) {
#ifdef DEBUG_SBUS_PACKETS
        sbusStateFlags |= SBUS_STATE_FAILSAFE;
        debug[0] = sbusStateFlags;
#endif
        // internal failsafe enabled and rx failsafe flag set
        return false;
    }

    sbusChannelData[0] = sbusFrame.frame.chan0;
    sbusChannelData[1] = sbusFrame.frame.chan1;
    sbusChannelData[2] = sbusFrame.frame.chan2;
    sbusChannelData[3] = sbusFrame.frame.chan3;
    sbusChannelData[4] = sbusFrame.frame.chan4;
    sbusChannelData[5] = sbusFrame.frame.chan5;
    sbusChannelData[6] = sbusFrame.frame.chan6;
    sbusChannelData[7] = sbusFrame.frame.chan7;
    sbusChannelData[8] = sbusFrame.frame.chan8;
    sbusChannelData[9] = sbusFrame.frame.chan9;
    sbusChannelData[10] = sbusFrame.frame.chan10;
    sbusChannelData[11] = sbusFrame.frame.chan11;
    sbusChannelData[12] = sbusFrame.frame.chan12;
    sbusChannelData[13] = sbusFrame.frame.chan13;
    sbusChannelData[14] = sbusFrame.frame.chan14;
    sbusChannelData[15] = sbusFrame.frame.chan15;

    if (sbusFrame.frame.flags & SBUS_FLAG_CHANNEL_17) {
        sbusChannelData[16] = SBUS_DIGITAL_CHANNEL_MAX;
    } else {
        sbusChannelData[16] = SBUS_DIGITAL_CHANNEL_MIN;
    }

    if (sbusFrame.frame.flags & SBUS_FLAG_CHANNEL_18) {
        sbusChannelData[17] = SBUS_DIGITAL_CHANNEL_MAX;
    } else {
        sbusChannelData[17] = SBUS_DIGITAL_CHANNEL_MIN;
    }

#ifdef DEBUG_SBUS_PACKETS
    sbusStateFlags = 0;
    debug[0] = sbusStateFlags;
#endif
    return true;
}

static uint16_t sbusReadRawRC(rxRuntimeConfig_t *rxRuntimeConfig, uint8_t chan)
{
    UNUSED(rxRuntimeConfig);
    // Linear fitting values read from OpenTX-ppmus and comparing with values received by X4R
    // http://www.wolframalpha.com/input/?i=linear+fit+%7B173%2C+988%7D%2C+%7B1812%2C+2012%7D%2C+%7B993%2C+1500%7D
    return (0.625f * sbusChannelData[chan]) + 880;
}
