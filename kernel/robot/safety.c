#include "safety.h"

static safety_limits_t g_lim;
static safety_stats_t  g_st;
static safety_world_t  g_world;
static uint64_t        g_last_out_us;

static const char *const VERDICT[] = { "OK", "MODIFY", "BLOCK" };
static const char *const REASON[SAFETY_R_COUNT] = {
    "none", "abi-version", "range", "stale-input", "expired",
    "battery", "link", "geofence", "limit", "watchdog"
};

void safety_init(const safety_limits_t *limits) {
    if (limits) g_lim = *limits;
    safety_reset();
}

void safety_reset(void) {
    k_memset(&g_st, 0, sizeof(g_st));
    k_memset(&g_world, 0, sizeof(g_world));
    g_last_out_us = 0;
}

const safety_limits_t *safety_limits(void) { return &g_lim; }
const safety_stats_t  *safety_stats(void)  { return &g_st; }
const safety_world_t  *safety_world(void)  { return &g_world; }

const char *safety_verdict_name(safety_verdict_t v) {
    return (v <= SAFETY_BLOCK) ? VERDICT[v] : "?";
}
const char *safety_reason_name(safety_reason_t r) {
    return (r < SAFETY_R_COUNT) ? REASON[r] : "?";
}

void safety_observe(const sensor_event_t *e) {
    if (!e) return;
    g_world.last_input_us = e->t_us;
    switch (e->type) {
    case SENSOR_BATTERY:
        /* An unreliable reading updates nothing: the gate would rather not
           know than believe a number the producer disowned. */
        if (!(e->flags & SENSOR_F_UNRELIABLE)) {
            g_world.battery_percent = e->u.battery.percent;
            g_world.valid_battery = true;
        }
        break;
    case SENSOR_LINK:
        g_world.link_connected = e->u.link.connected != 0;
        g_world.link_last_us = e->t_us;
        g_world.valid_link = true;
        break;
    case SENSOR_POSITION:
        g_world.x_mm = e->u.position.x_mm;
        g_world.y_mm = e->u.position.y_mm;
        g_world.z_mm = e->u.position.z_mm;
        g_world.valid_position = true;
        break;
    default:
        break;
    }
}

static void degrade(intent_t *out, uint8_t kind, uint8_t reason, uint16_t flag) {
    out->kind = kind;
    out->reason = reason;
    out->flags |= flag;
    /* A degraded intent keeps its causal link but not its ambition. */
    out->x_mm = out->y_mm = out->z_mm = 0;
    if (kind == INTENT_HOLD || kind == INTENT_LAND) out->max_speed_mms = 0;
}

static bool outside_fence(const intent_t *in) {
    if (!g_lim.geofence_on) return false;
    if (in->x_mm >  g_lim.fence_x_mm || in->x_mm < -g_lim.fence_x_mm) return true;
    if (in->y_mm >  g_lim.fence_y_mm || in->y_mm < -g_lim.fence_y_mm) return true;
    if (in->z_mm >  g_lim.fence_z_mm || in->z_mm < -g_lim.fence_z_mm) return true;
    return false;
}

safety_verdict_t safety_check(const intent_t *in, uint64_t now_us,
                              intent_t *out, safety_reason_t *reason) {
    safety_reason_t why = SAFETY_R_NONE;
    bool modified = false;

    if (!in || !out) return SAFETY_BLOCK;
    g_st.seen++;

    /* --- things that make the record unreadable: block, do not guess --- */
    if (in->version != INTENT_ABI_VERSION || in->size != sizeof(intent_t)) {
        why = SAFETY_R_BAD_VERSION;
        goto blocked;
    }
    if (in->kind >= INTENT_KIND_COUNT || in->reason >= INTENT_REASON_COUNT ||
        in->confidence > INTENT_CONFIDENCE_MAX) {
        why = SAFETY_R_BAD_RANGE;
        goto blocked;
    }
    /* An intent whose deadline has already passed is not late, it is void. */
    if (in->deadline_us && now_us > in->deadline_us) {
        why = SAFETY_R_EXPIRED;
        goto blocked;
    }
    /* Built on input older than we accept: block rather than act on it. */
    /* now_us can be older than the stamp when a producer ran between two
       clock reads: an unsigned subtraction there does not go negative, it
       goes enormous, and everything downstream reads it as "ancient". */
    if (g_lim.input_max_age_us && in->issued_us > 0 && now_us > in->issued_us &&
        now_us - in->issued_us > g_lim.input_max_age_us) {
        why = SAFETY_R_STALE;
        goto blocked;
    }

    *out = *in;

    /* --- things that make the wish unsafe: degrade, and say so ---------- */
    if (g_world.valid_battery && g_lim.battery_land_percent &&
        g_world.battery_percent <= g_lim.battery_land_percent &&
        in->kind != INTENT_LAND) {
        degrade(out, INTENT_LAND, INTENT_REASON_LOW_BATTERY, INTENT_F_GATE_HOLD);
        why = SAFETY_R_BATTERY;
        modified = true;
    } else if (g_world.valid_battery && g_lim.battery_return_percent &&
               g_world.battery_percent <= g_lim.battery_return_percent &&
               in->kind != INTENT_RETURN_HOME && in->kind != INTENT_LAND &&
               in->kind != INTENT_ABORT) {
        degrade(out, INTENT_RETURN_HOME, INTENT_REASON_LOW_BATTERY, INTENT_F_GATE_HOLD);
        why = SAFETY_R_BATTERY;
        modified = true;
    }

    /* Link: silence for longer than the timeout is the same as "down". The
       autopilot has its own failsafe; this stops us pushing new ambition
       into a link we cannot hear. */
    if (!modified && g_world.valid_link) {
        bool silent = g_lim.link_timeout_us && now_us > g_world.link_last_us &&
                      now_us - g_world.link_last_us > g_lim.link_timeout_us;
        if ((!g_world.link_connected || silent) &&
            in->kind != INTENT_HOLD && in->kind != INTENT_LAND &&
            in->kind != INTENT_RETURN_HOME && in->kind != INTENT_ABORT) {
            degrade(out, INTENT_HOLD, INTENT_REASON_LINK_LOST, INTENT_F_GATE_HOLD);
            why = SAFETY_R_LINK;
            modified = true;
        }
    }

    if (!modified && outside_fence(in)) {
        degrade(out, INTENT_HOLD, INTENT_REASON_GEOFENCE, INTENT_F_GATE_HOLD);
        why = SAFETY_R_GEOFENCE;
        modified = true;
    }

    /* Limits are a ceiling, not a suggestion: clamp and mark it. */
    if (g_lim.max_speed_mms && out->max_speed_mms > g_lim.max_speed_mms) {
        out->max_speed_mms = g_lim.max_speed_mms;
        out->flags |= INTENT_F_GATE_LIMIT;
        if (!modified) { why = SAFETY_R_LIMIT; modified = true; }
    }
    if (g_lim.max_range_mm && out->max_range_mm > g_lim.max_range_mm) {
        out->max_range_mm = g_lim.max_range_mm;
        out->flags |= INTENT_F_GATE_LIMIT;
        if (!modified) { why = SAFETY_R_LIMIT; modified = true; }
    }

    g_last_out_us = now_us;
    if (out->kind < INTENT_KIND_COUNT) g_st.out_kind[out->kind]++;
    g_st.by_reason[why]++;
    if (modified) { g_st.modified++; if (reason) *reason = why; return SAFETY_MODIFY; }
    g_st.accepted++;
    if (reason) *reason = SAFETY_R_NONE;
    return SAFETY_OK;

blocked:
    g_st.blocked++;
    g_st.by_reason[why]++;
    if (reason) *reason = why;
    return SAFETY_BLOCK;
}

bool safety_watchdog(uint64_t now_us, intent_t *out) {
    if (!out || !g_lim.watchdog_us) return false;
    if (g_last_out_us && (now_us <= g_last_out_us ||
                          now_us - g_last_out_us < g_lim.watchdog_us)) return false;
    if (!g_last_out_us) { g_last_out_us = now_us; return false; }

    k_memset(out, 0, sizeof(*out));
    out->version = INTENT_ABI_VERSION;
    out->size = (uint8_t)sizeof(intent_t);
    out->kind = INTENT_HOLD;
    out->reason = INTENT_REASON_WATCHDOG;
    out->confidence = INTENT_CONFIDENCE_MAX;   /* certain, because it is ours */
    out->flags = INTENT_F_GATE_HOLD;
    out->issued_us = now_us;
    out->deadline_us = now_us + g_lim.watchdog_us;
    g_last_out_us = now_us;
    g_st.by_reason[SAFETY_R_WATCHDOG]++;
    g_st.out_kind[INTENT_HOLD]++;
    return true;
}
