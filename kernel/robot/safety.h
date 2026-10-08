/*
 * Safety gate — the last thing between a wish and the autopilot.
 *
 * Everything a producer emits passes here. The gate validates ranges, checks
 * how old the input was, refuses what has expired, looks at battery, link
 * and geofence, and degrades what it cannot allow into something it can:
 * HOLD, RETURN_HOME or LAND. It never invents a more ambitious intent than
 * the one it was given.
 *
 * The rule that shapes the counters: a decision the gate had to correct is
 * not credited to the producer. `accepted` counts intents that went through
 * untouched. That is the number that says whether the thing upstream is any
 * good, and merging it with "modified" would flatter it.
 */
#ifndef ECHOS_SAFETY_H
#define ECHOS_SAFETY_H

#include "rogex.h"
#include "intent.h"
#include "sensor.h"

typedef enum { SAFETY_OK = 0, SAFETY_MODIFY, SAFETY_BLOCK } safety_verdict_t;

typedef enum {
    SAFETY_R_NONE = 0,
    SAFETY_R_BAD_VERSION,
    SAFETY_R_BAD_RANGE,
    SAFETY_R_STALE,          /* the sensor behind it is too old            */
    SAFETY_R_EXPIRED,        /* its own deadline has passed                */
    SAFETY_R_BATTERY,
    SAFETY_R_LINK,
    SAFETY_R_GEOFENCE,
    SAFETY_R_LIMIT,          /* limits tightened, kind kept                */
    SAFETY_R_WATCHDOG,       /* nothing arrived in time; the gate spoke    */
    SAFETY_R_COUNT
} safety_reason_t;

/* What the world looks like right now, as far as the gate knows. Fed from
   sensor records, never guessed. `valid_*` says whether we have been told at
   all: an unknown battery is not a full one. */
typedef struct {
    bool     valid_battery, valid_link, valid_position;
    uint16_t battery_percent;
    bool     link_connected;
    uint64_t link_last_us;
    int32_t  x_mm, y_mm, z_mm;
    uint64_t last_input_us;
} safety_world_t;

typedef struct {
    uint16_t battery_return_percent;  /* below this: RETURN_HOME           */
    uint16_t battery_land_percent;    /* below this: LAND                  */
    uint64_t input_max_age_us;        /* older sensor input: not fresh     */
    uint64_t link_timeout_us;         /* silence on the link: degrade      */
    uint64_t watchdog_us;             /* no intent at all: the gate speaks */
    uint32_t max_speed_mms;           /* ceiling for any intent            */
    uint32_t max_range_mm;
    bool     geofence_on;
    int32_t  fence_x_mm, fence_y_mm, fence_z_mm;   /* box half-extents     */
} safety_limits_t;

typedef struct {
    uint32_t seen;
    uint32_t accepted;                /* untouched: the producer's credit  */
    uint32_t modified;
    uint32_t blocked;
    uint32_t by_reason[SAFETY_R_COUNT];
    uint32_t out_kind[INTENT_KIND_COUNT];   /* what actually left the gate */
} safety_stats_t;

void safety_init(const safety_limits_t *limits);
void safety_reset(void);
const safety_limits_t *safety_limits(void);
const safety_stats_t  *safety_stats(void);
const safety_world_t  *safety_world(void);

/* Update the gate's picture of the world from a sensor record. */
void safety_observe(const sensor_event_t *e);

/* Judge one intent. `out` receives what may actually be sent — on BLOCK it
   is left untouched and nothing should be sent. `reason` says why. */
safety_verdict_t safety_check(const intent_t *in, uint64_t now_us,
                              intent_t *out, safety_reason_t *reason);

/* Nothing has arrived for watchdog_us: the gate emits its own HOLD. Returns
   false when no intent is due yet. */
bool safety_watchdog(uint64_t now_us, intent_t *out);

const char *safety_verdict_name(safety_verdict_t v);
const char *safety_reason_name(safety_reason_t r);

#endif /* ECHOS_SAFETY_H */
