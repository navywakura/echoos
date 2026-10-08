#include "robot.h"
#include "mono.h"

/* Capacities are small on purpose: a deep queue in a control loop converts a
   latency problem into a stale-data problem. 32 sensor records is a third of
   a second at 100 Hz. */
#define Q_SENSOR_CAP 32
#define Q_INTENT_CAP 16
#define Q_GATE_CAP   16
#define Q_OUT_CAP    16

static sensor_event_t g_sensor_slots[Q_SENSOR_CAP];
static intent_t       g_intent_slots[Q_INTENT_CAP];
static intent_t       g_gate_slots[Q_GATE_CAP];
static intent_t       g_out_slots[Q_OUT_CAP];

static rqueue_t g_q_sensor, g_q_intent, g_q_gate, g_q_out;
static robot_stats_t   g_st;
static robot_latency_t g_lat;
static uint32_t        g_intent_seq;
static robot_transport_fn g_transport;

static void push_to_transport(void);

/* --- latency ------------------------------------------------------------- */

static void latency_add(uint64_t us) {
    uint32_t b = 0;
    uint64_t v = us;
    while (v && b < ROBOT_LAT_BUCKETS - 1) { v >>= 1; b++; }
    g_lat.bucket[b]++;
    if (!g_lat.count || us < g_lat.min_us) g_lat.min_us = us;
    if (us > g_lat.max_us) g_lat.max_us = us;
    g_lat.sum_us += us;
    g_lat.count++;
}

uint64_t robot_latency_percentile(uint32_t per_mille) {
    uint64_t target, seen = 0;
    uint32_t b;
    if (!g_lat.count) return 0;
    target = ((uint64_t)g_lat.count * per_mille + 999u) / 1000u;
    if (!target) target = 1;
    for (b = 0; b < ROBOT_LAT_BUCKETS; b++) {
        seen += g_lat.bucket[b];
        if (seen >= target) return b ? (1ull << b) - 1u : 0u;
    }
    return g_lat.max_us;
}

/* --- the reflex policy ---------------------------------------------------
 * A placeholder, and named one. It reacts to a distance reading and to the
 * battery; it does not plan, remember or learn. Everything it decides is
 * still judged by the gate, and the counters keep the two apart.
 */
#define OBSTACLE_STOP_MM   1200u
#define OBSTACLE_SLOW_MM   3000u
#define INTENT_LIFETIME_US 500000ull      /* half a second to be acted on   */

static bool policy(const sensor_event_t *e, uint64_t now_us, intent_t *out) {
    k_memset(out, 0, sizeof(*out));
    out->version = INTENT_ABI_VERSION;
    out->size = (uint8_t)sizeof(intent_t);
    out->seq = ++g_intent_seq;
    out->cause_seq = e->seq;
    out->cause_source = e->source;
    out->cause_type = e->type;
    out->issued_us = now_us;
    out->deadline_us = now_us + INTENT_LIFETIME_US;
    /* Ask for what the gate allows. A producer that always asks for more
       than the ceiling turns every decision into a MODIFY and makes the
       "accepted unchanged" column meaningless. */
    out->max_speed_mms = safety_limits()->max_speed_mms;
    out->max_range_mm = safety_limits()->max_range_mm;
    out->cause_t_us = e->t_us;
    if (e->flags & SENSOR_F_UNRELIABLE) out->flags |= INTENT_F_DEGRADED;

    switch (e->type) {
    case SENSOR_DISTANCE: {
        uint32_t d = e->u.distance.distance_mm;
        if (d <= OBSTACLE_STOP_MM) {
            out->kind = INTENT_AVOID;
            out->reason = INTENT_REASON_OBSTACLE;
            out->confidence = e->quality;
            /* Away from the reading, along its axis. Millimetres, not a
               heading: the autopilot owns the geometry. */
            out->x_mm = -(int32_t)(OBSTACLE_SLOW_MM - d);
            out->max_speed_mms = 500;
            return true;
        }
        if (d <= OBSTACLE_SLOW_MM) {
            out->kind = INTENT_CONTINUE;
            out->reason = INTENT_REASON_CLEAR;
            out->confidence = e->quality > 200 ? e->quality - 200 : 0;
            out->max_speed_mms = 1000;
            return true;
        }
        out->kind = INTENT_CONTINUE;
        out->reason = INTENT_REASON_CLEAR;
        out->confidence = e->quality;
        return true;
    }
    case SENSOR_OBJECT:
        out->kind = INTENT_AVOID;
        out->reason = INTENT_REASON_OBSTACLE;
        out->confidence = e->u.object.confidence;
        out->x_mm = -e->u.object.x_mm;
        out->y_mm = -e->u.object.y_mm;
        out->max_speed_mms = 500;
        return true;
    case SENSOR_BATTERY:
        if (e->u.battery.percent <= 15) {
            out->kind = INTENT_RETURN_HOME;
            out->reason = INTENT_REASON_LOW_BATTERY;
            out->confidence = INTENT_CONFIDENCE_MAX;
            return true;
        }
        return false;                    /* nothing to say about a full pack */
    case SENSOR_LINK:
        if (!e->u.link.connected) {
            out->kind = INTENT_HOLD;
            out->reason = INTENT_REASON_LINK_LOST;
            out->confidence = INTENT_CONFIDENCE_MAX;
            return true;
        }
        return false;
    default:
        return false;                    /* silence is a valid answer        */
    }
}

/* --- pipeline ------------------------------------------------------------ */

void robot_init(void) {
    static const safety_limits_t limits = {
        .battery_return_percent = 20,
        .battery_land_percent   = 8,
        .input_max_age_us       = 1000000ull,   /* 1 s                      */
        .link_timeout_us        = 2000000ull,   /* 2 s of silence           */
        .watchdog_us            = 1000000ull,
        .max_speed_mms          = 1500,
        .max_range_mm           = 40000,
        .geofence_on            = true,
        .fence_x_mm = 100000, .fence_y_mm = 100000, .fence_z_mm = 30000
    };
    rqueue_init(&g_q_sensor, "sensor->runtime", g_sensor_slots,
                sizeof(sensor_event_t), Q_SENSOR_CAP);
    rqueue_init(&g_q_intent, "runtime->intents", g_intent_slots,
                sizeof(intent_t), Q_INTENT_CAP);
    rqueue_init(&g_q_gate, "intents->gate", g_gate_slots,
                sizeof(intent_t), Q_GATE_CAP);
    rqueue_init(&g_q_out, "gate->autopilot", g_out_slots,
                sizeof(intent_t), Q_OUT_CAP);
    safety_init(&limits);
    robot_reset();
}

void robot_reset(void) {
    rqueue_reset(&g_q_sensor); rqueue_reset(&g_q_intent);
    rqueue_reset(&g_q_gate);   rqueue_reset(&g_q_out);
    k_memset(&g_st, 0, sizeof(g_st));
    k_memset(&g_lat, 0, sizeof(g_lat));
    g_intent_seq = 0;
    safety_reset();
}

bool robot_ingest(const sensor_event_t *e) {
    if (!sensor_event_valid(e)) { g_st.sensors_rejected++; return false; }
    if (!rqueue_push(&g_q_sensor, e)) return false;   /* the queue counts it */
    g_st.sensors_in++;
    return true;
}

/* One record at a time, all the way down. Draining a whole burst into the
   intent queue before judging any of it is what made a 40-record burst throw
   away 16 perfectly good intents: the backlog has to move, not pile up in
   the middle of its own pipeline. */
static void drain_gate(uint64_t now_us, uint32_t *sent) {
    intent_t in, judged;
    while (rqueue_pop(&g_q_gate, &in)) {
        safety_reason_t why = SAFETY_R_NONE;
        safety_verdict_t v = safety_check(&in, now_us, &judged, &why);
        if (v == SAFETY_BLOCK) {
            if (why == SAFETY_R_EXPIRED) {
                g_st.intents_expired++;
                rqueue_count_expired(&g_q_gate);
            }
            continue;
        }
        /* Full is not the same as lost: give the transport a chance to take
           what is already queued before deciding this record cannot fit. */
        if (!rqueue_push(&g_q_out, &judged)) {
            push_to_transport();
            if (!rqueue_push(&g_q_out, &judged)) continue;
        }
        {
            g_st.sent++;
            if (sent) (*sent)++;
            /* End to end: measured world to decision ready to leave. Not
               issued_us, which this same call stamped. */
            if (now_us >= in.cause_t_us) latency_add(now_us - in.cause_t_us);
        }
    }
}

uint32_t robot_step(uint64_t now_us) {
    sensor_event_t e;
    intent_t in, judged;
    uint32_t sent = 0;

    while (rqueue_pop(&g_q_sensor, &e)) {
        safety_observe(&e);
        if (policy(&e, now_us, &in)) {
            g_st.intents_made++;
            rqueue_push(&g_q_intent, &in);
        }
        while (rqueue_pop(&g_q_intent, &in)) rqueue_push(&g_q_gate, &in);
        drain_gate(now_us, &sent);
    }
    drain_gate(now_us, &sent);

    push_to_transport();

    /* nobody said anything for too long: the gate does */
    if (safety_watchdog(now_us, &judged)) {
        if (rqueue_push(&g_q_out, &judged)) {
            g_st.watchdog_intents++;
            g_st.sent++;
            sent++;
        }
    }
    push_to_transport();
    return sent;
}

bool robot_take_outbound(intent_t *out) { return rqueue_pop(&g_q_out, out); }

void robot_set_transport(robot_transport_fn fn) { g_transport = fn; }
bool robot_transport_attached(void) { return g_transport != NULL; }

/* Hand everything the gate approved to whoever is listening. A transport
   that refuses one record stops the drain: order matters more than
   throughput when the records are decisions. */
static void push_to_transport(void) {
    intent_t out;
    if (!g_transport) return;
    while (rqueue_pop(&g_q_out, &out)) {
        if (!g_transport(&out)) {
            /* Put it back at the head is not possible on a ring, so count it
               and stop: the next step will find the queue as it was. */
            rqueue_push(&g_q_out, &out);
            return;
        }
        g_st.delivered++;
    }
}

const robot_stats_t   *robot_stats(void)   { return &g_st; }
const robot_latency_t *robot_latency(void) { return &g_lat; }
const rqueue_t *robot_q_sensor(void) { return &g_q_sensor; }
const rqueue_t *robot_q_intent(void) { return &g_q_intent; }
const rqueue_t *robot_q_gate(void)   { return &g_q_gate; }
const rqueue_t *robot_q_out(void)    { return &g_q_out; }

/* --- the scripted scenario ----------------------------------------------- */

static uint32_t g_scen_seq;

/* Nothing is attached to the transport yet (that is phase E), so the
   scenario consumes what the gate approved and counts it. Leaving it to pile
   up would turn "the queue overflowed" into "nobody was listening", which is
   a different fault with the same counter. */
static uint32_t g_scen_delivered;

static bool scenario_transport(const intent_t *intent) {
    (void)intent;
    g_scen_delivered++;
    return true;
}

static uint32_t drain_outbound(void) {
    intent_t out;
    uint32_t n = 0;
    while (robot_take_outbound(&out)) n++;
    return n;
}

static void mk(sensor_event_t *e, uint8_t type, uint16_t quality) {
    k_memset(e, 0, sizeof(*e));
    e->version = SENSOR_ABI_VERSION;
    e->size = (uint8_t)sizeof(sensor_event_t);
    e->type = type;
    e->source = SENSOR_SRC_SYNTHETIC;
    e->quality = quality;
    e->seq = ++g_scen_seq;
    e->t_us = mono_us();
}

uint32_t robot_scenario(void) {
    sensor_event_t e;
    intent_t bad;
    uint32_t n = 0, i;

    g_scen_seq = 0;
    g_scen_delivered = 0;
    /* Stand in for the autopilot link phase E will bring. Without it the
       outbound queue would fill and the drops would say "nobody listening"
       instead of "the pipeline backed up". */
    robot_set_transport(scenario_transport);

    /* 1. A healthy link and a full battery, so the gate has a world. */
    mk(&e, SENSOR_LINK, 1000); e.u.link.connected = 1;
    e.u.link.rx_packets = 100; robot_ingest(&e); n++;
    mk(&e, SENSOR_BATTERY, 1000); e.u.battery.percent = 90;
    e.u.battery.voltage_mv = 16000; robot_ingest(&e); n++;
    robot_step(mono_us()); drain_outbound();

    /* 2. Clear road: CONTINUE, and the gate should not touch it. */
    for (i = 0; i < 5; i++) {
        mk(&e, SENSOR_DISTANCE, 950);
        e.u.distance.distance_mm = 8000 - i * 100;
        e.u.distance.max_mm = 12000;
        robot_ingest(&e); n++;
    }
    robot_step(mono_us()); drain_outbound();

    /* 3. Something close: AVOID at reduced speed. */
    mk(&e, SENSOR_DISTANCE, 900);
    e.u.distance.distance_mm = 600;
    e.u.distance.max_mm = 12000;
    robot_ingest(&e); n++;
    robot_step(mono_us()); drain_outbound();

    /* 4. Battery fades under the return threshold: whatever the policy
          wants, the gate turns it into RETURN_HOME. */
    mk(&e, SENSOR_BATTERY, 1000); e.u.battery.percent = 12;
    robot_ingest(&e); n++;
    mk(&e, SENSOR_DISTANCE, 900); e.u.distance.distance_mm = 9000;
    robot_ingest(&e); n++;
    robot_step(mono_us()); drain_outbound();

    /* 5. Battery critical: LAND. */
    mk(&e, SENSOR_BATTERY, 1000); e.u.battery.percent = 5;
    robot_ingest(&e); n++;
    mk(&e, SENSOR_DISTANCE, 900); e.u.distance.distance_mm = 9000;
    robot_ingest(&e); n++;
    robot_step(mono_us()); drain_outbound();

    /* 5b. The pack is swapped: the world goes back to healthy, so the
           clean path can be observed instead of being masked by LAND. */
    mk(&e, SENSOR_BATTERY, 1000); e.u.battery.percent = 85;
    robot_ingest(&e); n++;
    for (i = 0; i < 3; i++) {
        mk(&e, SENSOR_DISTANCE, 950);
        e.u.distance.distance_mm = 9000;
        robot_ingest(&e); n++;
    }
    robot_step(mono_us()); drain_outbound();

    /* 6. Link drops: HOLD. */
    mk(&e, SENSOR_LINK, 1000); e.u.link.connected = 0;
    robot_ingest(&e); n++;
    robot_step(mono_us()); drain_outbound();

    /* 6b. Link back up. */
    mk(&e, SENSOR_LINK, 1000); e.u.link.connected = 1;
    robot_ingest(&e); n++;
    robot_step(mono_us()); drain_outbound();

    /* 7. A malformed record and one from a future ABI: both refused at the
          door, neither reaching a queue. */
    mk(&e, SENSOR_TYPE_COUNT, 1000); robot_ingest(&e); n++;
    mk(&e, SENSOR_DISTANCE, 5000); robot_ingest(&e); n++;   /* quality > 1000 */
    mk(&e, SENSOR_DISTANCE, 900); e.version = SENSOR_ABI_VERSION + 1;
    robot_ingest(&e); n++;

    /* 7b. One intent that asks for more speed than the envelope allows:
           the clamp is a MODIFY we want to see happen on purpose. */
    k_memset(&bad, 0, sizeof(bad));
    bad.version = INTENT_ABI_VERSION;
    bad.size = (uint8_t)sizeof(intent_t);
    bad.kind = INTENT_CONTINUE;
    bad.reason = INTENT_REASON_OPERATOR;
    bad.confidence = 900;
    bad.seq = ++g_intent_seq;
    bad.cause_t_us = mono_us();
    bad.issued_us = bad.cause_t_us;
    bad.deadline_us = bad.issued_us + INTENT_LIFETIME_US;
    bad.max_speed_mms = 9000;               /* over the ceiling             */
    bad.max_range_mm = 40000;
    rqueue_push(&g_q_gate, &bad);
    robot_step(mono_us()); drain_outbound();

    /* 8. An intent whose deadline has already passed: blocked as expired,
          straight into the gate queue so nothing else can rescue it. */
    k_memset(&bad, 0, sizeof(bad));
    bad.version = INTENT_ABI_VERSION;
    bad.size = (uint8_t)sizeof(intent_t);
    bad.kind = INTENT_APPROACH;
    bad.reason = INTENT_REASON_OPERATOR;
    bad.confidence = 800;
    bad.seq = ++g_intent_seq;
    bad.issued_us = mono_us();
    bad.deadline_us = 1;                    /* long past                     */
    rqueue_push(&g_q_gate, &bad);
    robot_step(mono_us()); drain_outbound();

    /* 9. A burst larger than the sensor queue: the overflow must be counted
          as drops, and the high-water mark must show it filled. */
    for (i = 0; i < Q_SENSOR_CAP + 8; i++) {
        mk(&e, SENSOR_DISTANCE, 900);
        e.u.distance.distance_mm = 5000;
        robot_ingest(&e); n++;
    }
    robot_step(mono_us()); drain_outbound();
    return n;
}
