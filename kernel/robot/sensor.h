/*
 * Sensor ABI — what the world tells this machine.
 *
 * One fixed-size record for every kind of reading. Fixed size because the
 * queues are static and a variable-length record on a bounded ring is a
 * fragmentation bug waiting for the worst moment; integers because there is
 * no FPU on this path and a float is a portability argument nobody wins.
 *
 * Every record carries its own version and size, so a producer built against
 * an older ABI is rejected at the door instead of being reinterpreted.
 *
 * Units are stated once, here, and never converted in transit:
 *   distance, position   millimetres
 *   velocity             millimetres per second
 *   angles               milli-degrees
 *   angular rate         milli-degrees per second
 *   acceleration         milli-g
 *   voltage              millivolts        current  milliamps
 *   quality/confidence   per mille (0..1000)
 *   time                 monotonic microseconds since boot (arch/mono.h)
 */
#ifndef ECHOS_SENSOR_ABI_H
#define ECHOS_SENSOR_ABI_H

#include "rogex.h"

#define SENSOR_ABI_VERSION 1
#define SENSOR_PAYLOAD_BYTES 40
#define SENSOR_QUALITY_MAX 1000u

typedef enum {
    SENSOR_NONE = 0,
    SENSOR_TIME,        /* monotonic heartbeat, no payload beyond the header */
    SENSOR_IMU,
    SENSOR_POSITION,
    SENSOR_VELOCITY,
    SENSOR_BATTERY,
    SENSOR_DISTANCE,
    SENSOR_OBJECT,
    SENSOR_LINK,
    SENSOR_AUTOPILOT,
    SENSOR_WSP,         /* optional: a 16-byte WSP frame off the spike bus  */
    SENSOR_TYPE_COUNT
} sensor_type_t;

/* Where a reading came from. Named, not numbered by accident: the safety
   gate refuses to average two sources into one truth. */
typedef enum {
    SENSOR_SRC_NONE = 0,
    SENSOR_SRC_SYNTHETIC,      /* the scripted scenario in robot.c          */
    SENSOR_SRC_AUTOPILOT,      /* MAVLink telemetry (phase E)               */
    SENSOR_SRC_LOCAL,          /* a device on this board                    */
    SENSOR_SRC_COUNT
} sensor_source_t;

/* Flags a producer may set. UNRELIABLE says "I am reporting, do not trust
   me" and the gate treats it as such rather than dropping the record. */
#define SENSOR_F_UNRELIABLE 0x0001u
#define SENSOR_F_ESTIMATED  0x0002u   /* derived, not measured              */

typedef struct ROGEX_PACKED { int32_t gyro_mdps[3]; int32_t accel_mg[3];
                              int32_t heading_mdeg; uint8_t pad[12]; } sensor_imu_t;
typedef struct ROGEX_PACKED { int32_t x_mm, y_mm, z_mm; uint32_t frame;
                              uint32_t accuracy_mm; uint8_t pad[20]; } sensor_position_t;
typedef struct ROGEX_PACKED { int32_t vx_mms, vy_mms, vz_mms;
                              uint32_t speed_mms; uint8_t pad[24]; } sensor_velocity_t;
typedef struct ROGEX_PACKED { uint32_t voltage_mv; int32_t current_ma;
                              uint16_t percent; uint16_t cells;
                              uint32_t remaining_s; uint8_t pad[24]; } sensor_battery_t;
typedef struct ROGEX_PACKED { uint32_t distance_mm; uint32_t min_mm, max_mm;
                              uint16_t fov_mdeg; uint8_t sensor_id;
                              uint8_t pad[25]; } sensor_distance_t;
typedef struct ROGEX_PACKED { int32_t x_mm, y_mm, z_mm; uint16_t class_id;
                              uint16_t confidence; uint32_t size_mm;
                              uint8_t pad[20]; } sensor_object_t;
typedef struct ROGEX_PACKED { uint32_t rx_packets, tx_packets;
                              uint16_t loss_permille, rtt_ms;
                              uint8_t connected; uint8_t pad[27]; } sensor_link_t;
typedef struct ROGEX_PACKED { uint8_t mode, armed, failsafe, pad0;
                              uint32_t status_flags; uint8_t pad[32]; } sensor_autopilot_t;
typedef struct ROGEX_PACKED { uint8_t frame[16]; uint8_t pad[24]; } sensor_wsp_t;

typedef struct ROGEX_PACKED {
    uint8_t  version;      /* SENSOR_ABI_VERSION at the producer            */
    uint8_t  size;         /* sizeof(sensor_event_t); a short record is not
                              a truncated one, it is a different ABI        */
    uint8_t  type;         /* sensor_type_t                                 */
    uint8_t  source;       /* sensor_source_t                               */
    uint16_t quality;      /* per mille; 0 means "no idea", not "zero"      */
    uint16_t flags;
    uint32_t seq;          /* per source, wraps; gaps are packet loss       */
    uint32_t pad;
    uint64_t t_us;         /* monotonic, at capture, not at enqueue         */
    union {
        sensor_imu_t       imu;
        sensor_position_t  position;
        sensor_velocity_t  velocity;
        sensor_battery_t   battery;
        sensor_distance_t  distance;
        sensor_object_t    object;
        sensor_link_t      link;
        sensor_autopilot_t autopilot;
        sensor_wsp_t       wsp;
        uint8_t            raw[SENSOR_PAYLOAD_BYTES];
    } u;
} sensor_event_t;

_Static_assert(sizeof(sensor_event_t) == 64, "sensor_event_t must be 64 B");
_Static_assert(__builtin_offsetof(sensor_event_t, t_us) == 16, "t_us at 16");
_Static_assert(__builtin_offsetof(sensor_event_t, u) == 24, "payload at 24");
_Static_assert(sizeof(sensor_imu_t) == SENSOR_PAYLOAD_BYTES, "imu payload");
_Static_assert(sizeof(sensor_battery_t) == SENSOR_PAYLOAD_BYTES, "battery payload");
_Static_assert(sizeof(sensor_distance_t) == SENSOR_PAYLOAD_BYTES, "distance payload");

/* True when the record is one this build can read at all: right ABI, right
   size, known type and source, quality in range. Checked at the door of the
   first queue, so nothing downstream has to wonder. */
bool sensor_event_valid(const sensor_event_t *e);
const char *sensor_type_name(uint8_t type);
const char *sensor_source_name(uint8_t source);

#endif /* ECHOS_SENSOR_ABI_H */
