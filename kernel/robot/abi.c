/* Names and validation for the two ABIs. Kept apart from the pipeline so a
   producer can link the contract without dragging the runtime in. */
#include "sensor.h"
#include "intent.h"

static const char *const SENSOR_NAMES[SENSOR_TYPE_COUNT] = {
    "none", "time", "imu", "position", "velocity", "battery",
    "distance", "object", "link", "autopilot", "wsp"
};
static const char *const SOURCE_NAMES[SENSOR_SRC_COUNT] = {
    "none", "synthetic", "autopilot", "local"
};
static const char *const INTENT_NAMES[INTENT_KIND_COUNT] = {
    "NONE", "HOLD", "CONTINUE", "APPROACH", "AVOID",
    "RETURN_HOME", "LAND", "ABORT"
};
static const char *const REASON_NAMES[INTENT_REASON_COUNT] = {
    "none", "clear", "obstacle", "low-battery", "link-lost",
    "stale-input", "geofence", "operator", "watchdog"
};

const char *sensor_type_name(uint8_t t) {
    return t < SENSOR_TYPE_COUNT ? SENSOR_NAMES[t] : "?";
}
const char *sensor_source_name(uint8_t s) {
    return s < SENSOR_SRC_COUNT ? SOURCE_NAMES[s] : "?";
}
const char *intent_kind_name(uint8_t k) {
    return k < INTENT_KIND_COUNT ? INTENT_NAMES[k] : "?";
}
const char *intent_reason_name(uint8_t r) {
    return r < INTENT_REASON_COUNT ? REASON_NAMES[r] : "?";
}

bool sensor_event_valid(const sensor_event_t *e) {
    if (!e) return false;
    if (e->version != SENSOR_ABI_VERSION) return false;
    if (e->size != sizeof(sensor_event_t)) return false;
    if (e->type == SENSOR_NONE || e->type >= SENSOR_TYPE_COUNT) return false;
    if (e->source >= SENSOR_SRC_COUNT) return false;
    if (e->quality > SENSOR_QUALITY_MAX) return false;
    return true;
}
