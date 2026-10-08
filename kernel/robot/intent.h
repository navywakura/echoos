/*
 * Intent ABI — what this machine wants, not what it does.
 *
 * An intent is a high-level wish with a deadline and a confidence. It is
 * deliberately incapable of expressing a motor command: there is no PWM
 * field, no channel, no throttle, no servo. The autopilot owns
 * stabilisation, mixing, failsafes, geofence and landing; this layer says
 * "avoid that" and lets it. If a future producer needs to move an actuator
 * directly, it needs a different ABI and a different argument, not a spare
 * field in this one.
 *
 * Every intent carries the sensor record that caused it. That is what makes
 * "the gate corrected this decision" attributable to the producer instead of
 * being lost in an average.
 */
#ifndef ECHOS_INTENT_ABI_H
#define ECHOS_INTENT_ABI_H

#include "rogex.h"

#define INTENT_ABI_VERSION 1
#define INTENT_CONFIDENCE_MAX 1000u

typedef enum {
    INTENT_NONE = 0,
    INTENT_HOLD,          /* stop where you are and keep station           */
    INTENT_CONTINUE,      /* carry on with the current plan                */
    INTENT_APPROACH,      /* move toward the target, within limits         */
    INTENT_AVOID,         /* move away from the target                     */
    INTENT_RETURN_HOME,
    INTENT_LAND,
    INTENT_ABORT,         /* stop everything now; the strongest wish       */
    INTENT_KIND_COUNT
} intent_kind_t;

/* Why. A reason code is not decoration: the gate logs it, the report counts
   it, and "why did it turn" stops being a guess. */
typedef enum {
    INTENT_REASON_NONE = 0,
    INTENT_REASON_CLEAR,
    INTENT_REASON_OBSTACLE,
    INTENT_REASON_LOW_BATTERY,
    INTENT_REASON_LINK_LOST,
    INTENT_REASON_STALE_INPUT,
    INTENT_REASON_GEOFENCE,
    INTENT_REASON_OPERATOR,
    INTENT_REASON_WATCHDOG,
    INTENT_REASON_COUNT
} intent_reason_t;

/* Safety flags a producer sets to say what it already knows. The gate may
   add its own; it never clears one. */
#define INTENT_F_DEGRADED   0x0001u   /* produced from unreliable input     */
#define INTENT_F_GATE_HOLD  0x0002u   /* the gate replaced the kind         */
#define INTENT_F_GATE_LIMIT 0x0004u   /* the gate tightened the limits      */

typedef struct ROGEX_PACKED {
    uint8_t  version;        /* INTENT_ABI_VERSION                          */
    uint8_t  size;
    uint8_t  kind;           /* intent_kind_t                               */
    uint8_t  reason;         /* intent_reason_t                             */
    uint16_t confidence;     /* per mille                                   */
    uint16_t flags;
    uint32_t seq;            /* this producer's own count                   */
    uint32_t cause_seq;      /* the sensor record that caused it            */
    uint8_t  cause_source;   /* sensor_source_t                             */
    uint8_t  cause_type;     /* sensor_type_t                               */
    uint16_t pad;
    uint32_t pad2;           /* the 64-bit times start 8-aligned            */
    uint64_t cause_t_us;     /* when the world was measured: the only clock
                                that makes an end-to-end latency real, since
                                issued_us is stamped by the same call that
                                would be measuring it                        */
    uint64_t issued_us;      /* monotonic, when the producer decided        */
    uint64_t deadline_us;    /* absolute: after this it means nothing       */
    uint32_t max_speed_mms;  /* limits the producer accepts being held to   */
    uint32_t max_range_mm;
    int32_t  x_mm, y_mm, z_mm;   /* target or direction, per kind           */
    uint32_t pad3;               /* to 64 B, like the sensor record          */
} intent_t;

_Static_assert(sizeof(intent_t) == 72, "intent_t must be 72 B");
_Static_assert(__builtin_offsetof(intent_t, cause_t_us) == 24, "cause_t_us at 24");
_Static_assert(__builtin_offsetof(intent_t, deadline_us) == 40, "deadline at 40");

const char *intent_kind_name(uint8_t kind);
const char *intent_reason_name(uint8_t reason);

#endif /* ECHOS_INTENT_ABI_H */
