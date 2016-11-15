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
#include <string.h>

#include "platform.h"

#ifdef TELEMETRY

#include "build/version.h"

#if (FC_VERSION_MAJOR == 3) // not a very good way of finding out if this is betaflight or Cleanflight
#define BETAFLIGHT
#else
#define CLEANFLIGHT
#endif

#ifdef CLEANFLIGHT
#include "config/parameter_group.h"
#include "config/parameter_group_ids.h"
#endif

#include "common/streambuf.h"
#include "common/utils.h"

#include "sensors/battery.h"

#include "io/gps.h"
#include "io/serial.h"

#include "fc/rc_controls.h"
#include "fc/runtime_config.h"

#include "io/gps.h"

#include "flight/imu.h"

#include "rx/rx.h"
#include "rx/crsf.h"

#include "telemetry/telemetry.h"
#include "telemetry/crsf.h"

#ifdef CLEANFLIGHT
#include "fc/fc_serial.h"
#include "sensors/amperage.h"
#else
#include "fc/config.h"
#endif

#define TELEMETRY_CRSF_INITIAL_PORT_MODE    MODE_RXTX
#define CRSF_CYCLETIME_US                   100000

static serialPort_t *serialPort;
static serialPortConfig_t *serialPortConfig;
static portSharing_e portSharing;
static bool crsfTelemetryEnabled;
static uint8_t crsfCrc;
static uint8_t crsfFrame[CRSF_FRAME_SIZE_MAX];

static void crsfInitializeFrame(sbuf_t *dst)
{
    crsfCrc = 0;
    dst->ptr = crsfFrame;
    dst->end = ARRAYEND(crsfFrame);

    sbufWriteU8(dst, CRSF_ADDRESS_CRSF_RECEIVER);
}

static void crsfSerialize8(sbuf_t *dst, uint8_t v)
{
    sbufWriteU8(dst, v);
    crsfCrc = crc8_dvb_s2(crsfCrc, v);
}

static void crsfSerialize16(sbuf_t *dst, uint16_t v)
{
    // Use BigEndian format
    crsfSerialize8(dst,  (v >> 8));
    crsfSerialize8(dst, (uint8_t)v);
}

static void crsfSerialize32(sbuf_t *dst, uint32_t v)
{
    // Use BigEndian format
    crsfSerialize8(dst, (v >> 24));
    crsfSerialize8(dst, (v >> 16));
    crsfSerialize8(dst, (v >> 8));
    crsfSerialize8(dst, (uint8_t)v);
}

static void crsfSerializeData(sbuf_t *dst, const uint8_t *data, int len)
{
    for (int ii = 0; ii< len; ++ii) {
        crsfSerialize8(dst, data[ii]);
    }
}

static void crsfFinalize(sbuf_t *dst)
{
    sbufWriteU8(dst, crsfCrc);
    sbufSwitchToReader(dst, crsfFrame);
    serialWriteBuf(serialPort, sbufPtr(dst), sbufBytesRemaining(dst));
}

static int crsfFinalizeBuf(sbuf_t *dst, uint8_t *frame)
{
    sbufWriteU8(dst, crsfCrc);
    sbufSwitchToReader(dst, crsfFrame);
    const int frameSize = sbufBytesRemaining(dst);
    for (int ii = 0; sbufBytesRemaining(dst); ++ii) {
        frame[ii] = sbufReadU8(dst);
    }
    return frameSize;
}
/*
CRSF frame has the structure:
<Device address> <Frame length> <Type> <Payload> <CRC>
Device address: (uint8_t)
Frame length:   length in  bytes including Type (uint8_t)
Type:           (uint8_t)
CRC:            (uint8_t)
*/

/*
0x02 GPS
Payload:
int32_t     Latitude ( degree / 10`000`000 )
int32_t     Longitude (degree / 10`000`000 )
uint16_t    Groundspeed ( km/h / 100 )
uint16_t    GPS heading ( degree / 100 )
uint16      Altitude ( meter ­ 1000m offset )
uint8_t     Satellites in use ( counter )
*/
void crsfFrameGps(sbuf_t *dst)
{
    // use sbufWrite since CRC does not include frame length
    sbufWriteU8(dst, CRSF_FRAME_GPS_PAYLOAD_SIZE + CRSF_FRAME_LENGTH_TYPE_CRC);
    crsfSerialize8(dst, CRSF_FRAMETYPE_GPS);
    crsfSerialize32(dst, GPS_coord[LAT]); // CRSF and betaflight use same units for degrees
    crsfSerialize32(dst, GPS_coord[LON]);
    crsfSerialize16(dst, GPS_speed * 36); // GPS_speed is in 0.1m/s
    crsfSerialize16(dst, GPS_ground_course * 10); // GPS_ground_course is degrees * 10
    //Send real GPS altitude only if it's reliable (there's a GPS fix)
    const uint16_t altitude = (STATE(GPS_FIX) ? GPS_altitude : 0) + 1000;
    crsfSerialize16(dst, altitude);
    crsfSerialize8(dst, GPS_numSat);
}

/*
0x08 Battery sensor
Payload:
uint16_t    Voltage ( mV * 100 )
uint16_t    Current ( mA * 100 )
uint24_t    Capacity ( mAh )
uint8_t     Battery remaining ( percent )
*/
void crsfFrameBatterySensor(sbuf_t *dst)
{
    // use sbufWrite since CRC does not include frame length
    sbufWriteU8(dst, CRSF_FRAME_BATTERY_SENSOR_PAYLOAD_SIZE + CRSF_FRAME_LENGTH_TYPE_CRC);
    crsfSerialize8(dst, CRSF_FRAMETYPE_BATTERY_SENSOR);
    crsfSerialize16(dst, vbat); // vbat is in units of 0.1V
#ifdef CLEANFLIGHT
    const amperageMeter_t *amperageMeter = getAmperageMeter(batteryConfig()->amperageMeterSource);
    const int16_t amperage = constrain(amperageMeter->amperage, -0x8000, 0x7FFF) / 10; // send amperage in 0.01 A steps, range is -320A to 320A
    crsfSerialize16(dst, amperage); // amperage is in units of 0.1A
    const uint32_t batteryCapacity = batteryConfig()->batteryCapacity;
    const uint8_t batteryRemainingPercentage = batteryCapacityRemainingPercentage();
#else
    crsfSerialize16(dst, amperage / 10);
    const uint32_t batteryCapacity = batteryConfig->batteryCapacity;
    const uint8_t batteryRemainingPercentage = calculateBatteryCapacityRemainingPercentage();
#endif
    crsfSerialize8(dst, (batteryCapacity >> 16));
    crsfSerialize8(dst, (batteryCapacity >> 8));
    crsfSerialize8(dst, (uint8_t)batteryCapacity);

    crsfSerialize8(dst, batteryRemainingPercentage);
}

typedef enum {
    CRSF_ACTIVE_ANTENNA1 = 0,
    CRSF_ACTIVE_ANTENNA2 = 1,
} crsfActiveAntenna_e;

typedef enum {
    CRSF_RF_MODE_4_HZ = 0,
    CRSF_RF_MODE_50_HZ = 1,
    CRSF_RF_MODE_150_HZ = 2,
} crsrRfMode_e;

typedef enum {
    CRSF_RF_POWER_0_mW = 0,
    CRSF_RF_POWER_10_mW = 1,
    CRSF_RF_POWER_25_mW = 2,
    CRSF_RF_POWER_100_mW = 3,
    CRSF_RF_POWER_500_mW = 4,
    CRSF_RF_POWER_1000_mW = 5,
    CRSF_RF_POWER_2000_mW = 6,
} crsrRfPower_e;

/*
0x14 Link statistics
Uplink is the connection from the ground to the UAV and downlink the opposite direction.
Payload:
uint8_t     UplinkRSSI Ant.1(dBm*­1)
uint8_t     UplinkRSSI Ant.2(dBm*­1)
uint8_t     Uplink Package success rate / Link quality ( % )
int8_t      Uplink SNR ( db )
uint8_t     Diversity active antenna ( enum ant. 1 = 0, ant. 2 )
uint8_t     RF Mode ( enum 4fps = 0 , 50fps, 150hz)
uint8_t     Uplink TX Power ( enum 0mW = 0, 10mW, 25 mW, 100 mW, 500 mW, 1000 mW, 2000mW )
uint8_t     Downlink RSSI ( dBm * ­-1 )
uint8_t     Downlink package success rate / Link quality ( % )
int8_t      Downlink SNR ( db )
*/

void crsfFrameLinkStatistics(sbuf_t *dst)
{
    // use sbufWrite since CRC does not include frame length
    sbufWriteU8(dst, CRSF_FRAME_LINK_STATISTICS_PAYLOAD_SIZE + CRSF_FRAME_LENGTH_TYPE_CRC);
    crsfSerialize8(dst, CRSF_FRAMETYPE_LINK_STATISTICS);
    crsfSerialize8(dst, 0);
    crsfSerialize8(dst, 0);
    crsfSerialize8(dst, 0);
    crsfSerialize8(dst, 0);
    crsfSerialize8(dst, 0);
    crsfSerialize8(dst, 0);
    crsfSerialize8(dst, 0);
    crsfSerialize8(dst, 0);
    crsfSerialize8(dst, 0);
    crsfSerialize8(dst, 0);
}

/*
0x1E Attitude
Payload:
int16_t     Pitch angle ( rad / 10000 )
int16_t     Roll angle ( rad / 10000 )
int16_t     Yaw angle ( rad / 10000 )
*/

#define DECIDEGREES_TO_RADIANS10000(angle) (1000.0f * (angle) * RAD)

void crsfFrameAttitude(sbuf_t *dst)
{
     sbufWriteU8(dst, CRSF_FRAME_ATTITUDE_PAYLOAD_SIZE + CRSF_FRAME_LENGTH_TYPE_CRC);
     crsfSerialize8(dst, CRSF_FRAMETYPE_ATTITUDE);
     crsfSerialize16(dst, DECIDEGREES_TO_RADIANS10000(attitude.values.pitch));
     crsfSerialize16(dst, DECIDEGREES_TO_RADIANS10000(attitude.values.roll));
     crsfSerialize16(dst, DECIDEGREES_TO_RADIANS10000(attitude.values.yaw));
}

/*
0x21 Flight mode text based
Payload:
char[]      Flight mode ( Null­terminated string )
*/
void crsfFrameFlightMode(sbuf_t *dst)
{
    // just do Angle for the moment as a placeholder
    // write zero for frame length, since we don't know it yet
    uint8_t *lengthPtr = sbufPtr(dst);
    sbufWriteU8(dst, 0);
    crsfSerialize8(dst, CRSF_FRAMETYPE_FLIGHT_MODE);

    // use same logic as OSD, so telemetry displays same flight text as OSD
    const char *flightMode = "ACRO";
    if (isAirmodeActive()) {
        flightMode = "AIR";
    }
    if (FLIGHT_MODE(FAILSAFE_MODE)) {
        flightMode = "!FS";
    } else if (FLIGHT_MODE(ANGLE_MODE)) {
        flightMode = "STAB";
    } else if (FLIGHT_MODE(HORIZON_MODE)) {
        flightMode = "HOR";
    }
    crsfSerializeData(dst, (const uint8_t*)flightMode, strlen(flightMode));
    crsfSerialize8(dst, 0); // zero terminator for string
    // write in the length
    *lengthPtr = sbufPtr(dst) - lengthPtr;
}

#define BV(x)  (1 << (x)) // bit value

// schedule array to decide how often each type of frame is sent
#define CRSF_SCHEDULE_COUNT     5
static uint8_t crsfSchedule[CRSF_SCHEDULE_COUNT] = {
    BV(CRSF_FRAME_ATTITUDE),
    BV(CRSF_FRAME_BATTERY_SENSOR),
    BV(CRSF_FRAME_LINK_STATISTICS),
    BV(CRSF_FRAME_FLIGHT_MODE),
    BV(CRSF_FRAME_GPS),
};

static void processCrsf(void)
{
    static uint8_t crsfScheduleIndex = 0;
    const uint8_t currentSchedule = crsfSchedule[crsfScheduleIndex];

    sbuf_t crsfPayloadBuf;
    sbuf_t *dst = &crsfPayloadBuf;

    if (currentSchedule & BV(CRSF_FRAME_ATTITUDE)) {
        crsfInitializeFrame(dst);
        crsfFrameAttitude(dst);
        crsfFinalize(dst);
    }
    if (currentSchedule & BV(CRSF_FRAME_BATTERY_SENSOR)) {
        crsfInitializeFrame(dst);
        crsfFrameBatterySensor(dst);
        crsfFinalize(dst);
    }
    if (currentSchedule & BV(CRSF_FRAME_LINK_STATISTICS)) {
        crsfInitializeFrame(dst);
        crsfFrameLinkStatistics(dst);
        crsfFinalize(dst);
    }
    if (currentSchedule & BV(CRSF_FRAME_FLIGHT_MODE)) {
        crsfInitializeFrame(dst);
        crsfFrameFlightMode(dst);
        crsfFinalize(dst);
    }
#ifdef GPS
    if (currentSchedule & BV(CRSF_FRAME_GPS)) {
        crsfInitializeFrame(dst);
        crsfFrameGps(dst);
        crsfFinalize(dst);
    }
#endif
    crsfScheduleIndex = (crsfScheduleIndex + 1) % CRSF_SCHEDULE_COUNT;
}

void handleCrsfTelemetry(uint32_t currentTime)
{
    static uint32_t crsfLastCycleTime;
    if (!crsfTelemetryEnabled) {
        return;
    }
    if (!serialPort) {
        return;
    }
    if ((currentTime - crsfLastCycleTime) >= CRSF_CYCLETIME_US) {
        processCrsf();
        crsfLastCycleTime = currentTime;
    }
}

void freeCrsfTelemetryPort(void)
{
    closeSerialPort(serialPort);
    serialPort = NULL;
    crsfTelemetryEnabled = false;
}

void initCrsfTelemetry(void)
{
    serialPortConfig = findSerialPortConfig(FUNCTION_TELEMETRY_CRSF);
    portSharing = determinePortSharing(serialPortConfig, FUNCTION_TELEMETRY_CRSF);
}

void configureCrsfTelemetryPort(void)
{
    if (!serialPortConfig) {
        return;
    }
    serialPort = openSerialPort(serialPortConfig->identifier, FUNCTION_TELEMETRY_CRSF, NULL, CRSF_BAUDRATE, TELEMETRY_CRSF_INITIAL_PORT_MODE, CRSF_PORT_OPTIONS);
    if (!serialPort) {
        return;
    }
    crsfTelemetryEnabled = true;
}

bool checkCrsfTelemetryState(void)
{
    if (serialPortConfig && telemetryCheckRxPortShared(serialPortConfig)) {
        if (!crsfTelemetryEnabled && telemetrySharedPort != NULL) {
            serialPort = telemetrySharedPort;
            crsfTelemetryEnabled = true;
            return true;
        }
        return false;
    } else {
        const bool newTelemetryEnabled = telemetryDetermineEnabledState(portSharing);
        if (newTelemetryEnabled == crsfTelemetryEnabled) {
            return false;
        }
        if (newTelemetryEnabled) {
            configureCrsfTelemetryPort();
        } else {
            freeCrsfTelemetryPort();
        }
        return true;
    }
}

int getCrsfFrame(uint8_t *frame, crsfFrameType_e frameType)
{
    sbuf_t crsfFrameBuf;
    sbuf_t *sbuf = &crsfFrameBuf;

    crsfInitializeFrame(sbuf);
    switch (frameType) {
    default:
    case CRSF_FRAME_ATTITUDE:
        crsfFrameAttitude(sbuf);
        break;
    case CRSF_FRAME_BATTERY_SENSOR:
        crsfFrameBatterySensor(sbuf);
        break;
    case CRSF_FRAME_LINK_STATISTICS:
        crsfFrameLinkStatistics(sbuf);
        break;
    case CRSF_FRAME_FLIGHT_MODE:
        crsfFrameFlightMode(sbuf);
        break;
#if defined(GPS)
    case CRSF_FRAME_GPS:
        crsfFrameGps(sbuf);
        break;
#endif
    }
    const int frameSize = crsfFinalizeBuf(sbuf, frame);
    return frameSize;
}
#endif