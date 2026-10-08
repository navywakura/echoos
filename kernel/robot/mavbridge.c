#include "mavbridge.h"
#include "robot.h"
#include "sensor.h"
#include "intent.h"
#include "mono.h"
#include "ipstack.h"
#include "arch.h"

/* MAVLink commands we are willing to send. Every one of them is a
   high-level request the autopilot is free to refuse; none of them touches
   an actuator. */
#define MAV_CMD_NAV_RETURN_TO_LAUNCH 20u
#define MAV_CMD_NAV_LAND             21u
#define MAV_CMD_DO_REPOSITION       192u
#define MAV_CMD_SET_MESSAGE_INTERVAL 511u
#define MAV_CMD_DO_SET_MODE         176u
/* PX4 custom mode: main mode OFFBOARD is 6, packed into the upper bytes. */
#define PX4_CUSTOM_MAIN_OFFBOARD      6u
#define MAV_MODE_FLAG_CUSTOM_ENABLED  1u

#define HEARTBEAT_PERIOD_US  1000000ull   /* 1 Hz, as the protocol expects  */
#define SETPOINT_PERIOD_US     50000ull   /* 20 Hz: Offboard needs > 2 Hz   */
#define LINK_TIMEOUT_US      3000000ull   /* 3 s of silence is a lost link  */

static mavlink_parser_t   g_rx;
static mavlink_tx_t       g_tx;
static mavbridge_stats_t  g_st;
static bool     g_running;
static uint8_t  g_peer_ip[4];
static uint16_t g_peer_port, g_local_port;
static uint64_t g_last_hb_tx_us, g_last_sp_tx_us;
static bool     g_link_reported;

/* What we ask the autopilot to send, and how often. A companion computer
   that drinks from the default firehose loses the messages it actually needs:
   PX4's onboard stream is megabytes per second, our ring is finite, and the
   1 Hz heartbeat was the first thing to drown in it. Asking is also what
   produces COMMAND_ACKs, which is how we know the far end is listening. */
typedef struct { uint32_t msgid; uint32_t interval_us; } stream_req_t;
static const stream_req_t STREAMS[] = {
    { MAVLINK_MSG_HEARTBEAT,             1000000u },   /*  1 Hz */
    { MAVLINK_MSG_SYS_STATUS,            1000000u },
    { MAVLINK_MSG_BATTERY_STATUS,        1000000u },
    { MAVLINK_MSG_ATTITUDE,               100000u },   /* 10 Hz */
    { MAVLINK_MSG_LOCAL_POSITION_NED,     100000u },
    { MAVLINK_MSG_GLOBAL_POSITION_INT,    500000u },   /*  2 Hz */
    { MAVLINK_MSG_DISTANCE_SENSOR,        100000u },
};
#define STREAM_COUNT ((uint32_t)(sizeof(STREAMS) / sizeof(STREAMS[0])))
static uint32_t g_stream_next;          /* one request per poll, in order   */
static uint64_t g_last_req_us;

/* Messages we have asked the autopilot to stop sending. PX4's default stream
   is megabytes per second of things a companion cannot use, and this NIC
   drops what it cannot absorb — measured: a 46 % frame loss that took the
   1 Hz heartbeat with it. So each unknown id gets one polite request to
   stop, once, and the count is published: a link that had to silence the
   other end is a fact about the link. */
#define QUIET_MAX 32
static uint32_t g_quiet[QUIET_MAX];
static uint32_t g_quiet_n;
static uint64_t g_last_quiet_us;

/* --- IEEE-754 without an FPU ---------------------------------------------
 * MAVLink carries floats. This kernel has no floating point, so they are
 * built and read as bit patterns with integer arithmetic. Only the range a
 * vehicle actually uses is handled: values beyond it saturate rather than
 * producing a wrong number quietly.
 */
static int32_t f32_to_milli(uint32_t bits) {
    uint32_t mant = bits & 0x7FFFFFu;
    int32_t  exp  = (int32_t)((bits >> 23) & 0xFF) - 127;
    int64_t  v;
    bool neg = (bits >> 31) != 0;
    if (((bits >> 23) & 0xFF) == 0) return 0;             /* zero/denormal   */
    if (((bits >> 23) & 0xFF) == 0xFF) return neg ? -2147483647 : 2147483647;
    mant |= 0x800000u;                                    /* implicit 1      */
    /* value = mant / 2^23 * 2^exp, in milli-units: * 1000 */
    exp -= 23;
    v = (int64_t)mant * 1000;
    if (exp > 0) {
        if (exp > 31) return neg ? -2147483647 : 2147483647;
        v <<= exp;
    } else if (exp < 0) {
        if (exp < -40) return 0;
        v >>= -exp;
    }
    if (v > 2147483647ll) v = 2147483647ll;
    return neg ? (int32_t)(-v) : (int32_t)v;
}

static uint32_t f32_from_milli(int32_t milli) {
    uint32_t sign = 0, exp = 127, mant, sh = 24;
    uint64_t v;
    if (milli == 0) return 0;
    if (milli < 0) { sign = 0x80000000u; v = (uint64_t)(-(int64_t)milli); }
    else v = (uint64_t)milli;
    /* Scale as far left as fits before dividing: 24 bits is enough for a
       metre but not for a millimetre, where truncation costs a hundred ulp.
       The exponent below accounts for whatever shift was used. */
    while (sh < 38 && (v >> (63 - sh)) == 0) sh++;
    v <<= sh;
    v = (v + 500) / 1000;                 /* nearest, not toward zero       */
    while (v >= (1ull << 24)) { v >>= 1; exp++; }
    while (v && v < (1ull << 23)) { v <<= 1; exp--; }
    /* v is 1.m x 2^23 and the value is v x 2^(-sh), so the exponent field is
       127 + 23 + s - sh, and the loops above left exp at 127 + s. */
    exp += 23 - sh;
    mant = (uint32_t)(v & 0x7FFFFFu);
    return sign | ((exp & 0xFF) << 23) | mant;
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* --- telemetry -> sensor records ----------------------------------------- */

static void emit(uint8_t type, uint16_t quality, void (*fill)(sensor_event_t *)) {
    static uint32_t seq;
    sensor_event_t e;
    k_memset(&e, 0, sizeof(e));
    e.version = SENSOR_ABI_VERSION;
    e.size = (uint8_t)sizeof(sensor_event_t);
    e.type = type;
    e.source = SENSOR_SRC_AUTOPILOT;
    e.quality = quality;
    e.seq = ++seq;
    e.t_us = mono_us();
    if (fill) fill(&e);
    if (robot_ingest(&e)) g_st.sensors_produced++;
}

static const uint8_t *g_pl;      /* payload of the message being decoded    */

static void fill_autopilot(sensor_event_t *e) {
    e->u.autopilot.mode = (uint8_t)(g_pl[6] & 0xFF);        /* base_mode    */
    e->u.autopilot.armed = (uint8_t)((g_pl[6] & 0x80) ? 1 : 0);
    e->u.autopilot.failsafe = 0;
    e->u.autopilot.status_flags = rd_u32(g_pl + 0);         /* custom_mode  */
}
static void fill_battery_sys(sensor_event_t *e) {
    /* SYS_STATUS: voltage_battery u16 at 14, current_battery i16 at 16,
       battery_remaining i8 at 30. */
    e->u.battery.voltage_mv = rd_u16(g_pl + 14);
    e->u.battery.current_ma = (int16_t)rd_u16(g_pl + 16) * 10;
    e->u.battery.percent = (uint16_t)((int8_t)g_pl[30] < 0 ? 0 : (uint8_t)g_pl[30]);
}
static void fill_battery_full(sensor_event_t *e) {
    /* BATTERY_STATUS: voltages[0] u16 at 10, current i16 at 30,
       battery_remaining i8 at 35. */
    uint16_t mv = rd_u16(g_pl + 10);
    e->u.battery.voltage_mv = (mv == 0xFFFFu) ? 0 : mv;
    e->u.battery.current_ma = (int16_t)rd_u16(g_pl + 30) * 10;
    e->u.battery.percent = (uint16_t)((int8_t)g_pl[35] < 0 ? 0 : (uint8_t)g_pl[35]);
}
static void fill_attitude(sensor_event_t *e) {
    /* ATTITUDE: time_boot_ms u32, then roll/pitch/yaw and their rates, all
       float radians. Stored as milli-degrees: rad * 57296 / 1000. */
    int32_t roll  = f32_to_milli(rd_u32(g_pl + 4));
    int32_t pitch = f32_to_milli(rd_u32(g_pl + 8));
    int32_t yaw   = f32_to_milli(rd_u32(g_pl + 12));
    e->u.imu.gyro_mdps[0] = f32_to_milli(rd_u32(g_pl + 16));
    e->u.imu.gyro_mdps[1] = f32_to_milli(rd_u32(g_pl + 20));
    e->u.imu.gyro_mdps[2] = f32_to_milli(rd_u32(g_pl + 24));
    e->u.imu.accel_mg[0] = roll;      /* milli-radians, named by the header */
    e->u.imu.accel_mg[1] = pitch;
    e->u.imu.heading_mdeg = (int32_t)(((int64_t)yaw * 57296) / 1000);
}
static void fill_local_position(sensor_event_t *e) {
    /* LOCAL_POSITION_NED: time u32, x/y/z float m, vx/vy/vz float m/s. */
    e->u.position.x_mm = f32_to_milli(rd_u32(g_pl + 4));
    e->u.position.y_mm = f32_to_milli(rd_u32(g_pl + 8));
    e->u.position.z_mm = f32_to_milli(rd_u32(g_pl + 12));
    e->u.position.frame = 1;                       /* NED, body-relative   */
}
static void fill_velocity(sensor_event_t *e) {
    int32_t vx = f32_to_milli(rd_u32(g_pl + 16));
    int32_t vy = f32_to_milli(rd_u32(g_pl + 20));
    int32_t vz = f32_to_milli(rd_u32(g_pl + 24));
    e->u.velocity.vx_mms = vx;
    e->u.velocity.vy_mms = vy;
    e->u.velocity.vz_mms = vz;
    /* No sqrt down here: the magnitude a gate compares against is the
       largest component, and that bound is honest. */
    e->u.velocity.speed_mms = (uint32_t)((vx < 0 ? -vx : vx) >
                                         (vy < 0 ? -vy : vy)
                                         ? (vx < 0 ? -vx : vx)
                                         : (vy < 0 ? -vy : vy));
}
static void fill_global_position(sensor_event_t *e) {
    /* GLOBAL_POSITION_INT is already integer: lat/lon 1e7 deg, alt mm. */
    e->u.position.x_mm = (int32_t)rd_u32(g_pl + 4);
    e->u.position.y_mm = (int32_t)rd_u32(g_pl + 8);
    e->u.position.z_mm = (int32_t)rd_u32(g_pl + 12);
    e->u.position.frame = 2;                       /* global, 1e7 degrees  */
}
static void fill_distance(sensor_event_t *e) {
    /* DISTANCE_SENSOR: time u32, min u16, max u16, current u16 — centimetres. */
    e->u.distance.min_mm = (uint32_t)rd_u16(g_pl + 4) * 10u;
    e->u.distance.max_mm = (uint32_t)rd_u16(g_pl + 6) * 10u;
    e->u.distance.distance_mm = (uint32_t)rd_u16(g_pl + 8) * 10u;
    e->u.distance.sensor_id = g_pl[12];
}
static void fill_link(sensor_event_t *e) {
    const mavlink_rx_stats_t *rs = &g_rx.stats;
    e->u.link.connected = g_st.link_up ? 1u : 0u;
    e->u.link.rx_packets = rs->frames;
    e->u.link.tx_packets = g_st.tx_frames;
    e->u.link.loss_permille = rs->frames
        ? (uint16_t)((uint64_t)rs->lost_estimate * 1000ull /
                     (rs->frames + rs->lost_estimate))
        : 0;
}

static void report_link(bool up) {
    if (g_link_reported && up == g_st.link_up) return;
    if (g_link_reported && !up) g_st.link_drops++;
    g_st.link_up = up;
    g_link_reported = true;
    emit(SENSOR_LINK, 1000, fill_link);
}

static void handle_message(const mavlink_message_t *m) {
    g_pl = m->payload;
    g_st.rx_frames++;
    switch (m->msgid) {
    case MAVLINK_MSG_HEARTBEAT: {
        uint64_t t = mono_us();
        if (g_st.last_heartbeat_us) {
            g_st.last_hb_gap_us = t - g_st.last_heartbeat_us;
            if (g_st.last_hb_gap_us > g_st.max_hb_gap_us)
                g_st.max_hb_gap_us = g_st.last_hb_gap_us;
        }
        g_st.heartbeats_rx++;
        g_st.last_heartbeat_us = t;
        g_st.peer_sysid = m->sysid;
        g_st.peer_compid = m->compid;
        g_st.autopilot_mode = m->payload[6];
        g_st.autopilot_armed = (uint8_t)((m->payload[6] & 0x80) ? 1 : 0);
        g_st.rx_by_type[0]++;
        report_link(true);
        emit(SENSOR_AUTOPILOT, 1000, fill_autopilot);
        break;
    }
    case MAVLINK_MSG_SYS_STATUS:
        g_st.rx_by_type[1]++;
        emit(SENSOR_BATTERY, 900, fill_battery_sys);
        break;
    case MAVLINK_MSG_BATTERY_STATUS:
        g_st.rx_by_type[2]++;
        emit(SENSOR_BATTERY, 1000, fill_battery_full);
        break;
    case MAVLINK_MSG_ATTITUDE:
        g_st.rx_by_type[3]++;
        emit(SENSOR_IMU, 1000, fill_attitude);
        break;
    case MAVLINK_MSG_LOCAL_POSITION_NED:
        g_st.rx_by_type[4]++;
        emit(SENSOR_POSITION, 1000, fill_local_position);
        emit(SENSOR_VELOCITY, 1000, fill_velocity);
        break;
    case MAVLINK_MSG_GLOBAL_POSITION_INT:
        g_st.rx_by_type[5]++;
        emit(SENSOR_POSITION, 900, fill_global_position);
        break;
    case MAVLINK_MSG_DISTANCE_SENSOR:
        g_st.rx_by_type[6]++;
        emit(SENSOR_DISTANCE, 1000, fill_distance);
        break;
    case MAVLINK_MSG_COMMAND_ACK:
        g_st.rx_by_type[7]++;
        g_st.command_acks++;
        g_st.last_ack_command = rd_u16(m->payload + 0);
        g_st.last_ack_result = m->payload[2];
        break;
    default:
        break;                          /* known id, nothing to do with it  */
    }
}

/* --- outbound ------------------------------------------------------------ */

static bool send_frame(uint32_t msgid, const uint8_t *payload, uint16_t len) {
    uint8_t frame[MAV_MAX_FRAME];
    uint16_t n = mavlink_pack(&g_tx, msgid, payload, len, frame, sizeof(frame));
    if (!n) return false;
    if (!ip_udp_send(g_peer_ip, g_peer_port, g_local_port, frame, n)) return false;
    g_st.tx_frames++;
    return true;
}

static void send_heartbeat(void) {
    uint8_t pl[9];
    k_memset(pl, 0, sizeof(pl));
    wr_u32(pl + 0, 0);            /* custom_mode                             */
    pl[4] = 18;                   /* MAV_TYPE_ONBOARD_CONTROLLER             */
    pl[5] = 8;                    /* MAV_AUTOPILOT_INVALID: we are not one   */
    pl[6] = 0;                    /* base_mode                               */
    pl[7] = 4;                    /* MAV_STATE_ACTIVE                        */
    pl[8] = 3;                    /* mavlink_version                         */
    if (send_frame(MAVLINK_MSG_HEARTBEAT, pl, sizeof(pl)))
        g_st.heartbeats_tx++;
}

static void send_command_long_2(uint16_t command, int32_t p1_milli, int32_t p2_milli) {
    uint8_t pl[33];
    k_memset(pl, 0, sizeof(pl));
    wr_u32(pl + 0, f32_from_milli(p1_milli));      /* param1                */
    wr_u32(pl + 4, f32_from_milli(p2_milli));      /* param2                */
    wr_u16(pl + 28, command);
    pl[30] = g_st.peer_sysid ? g_st.peer_sysid : 1;
    pl[31] = g_st.peer_compid ? g_st.peer_compid : 1;
    pl[32] = 0;                                    /* confirmation          */
    send_frame(MAVLINK_MSG_COMMAND_LONG, pl, sizeof(pl));
}

static void send_command_long_3(uint16_t command, int32_t p1_milli,
                                int32_t p2_milli, int32_t p3_milli) {
    uint8_t pl[33];
    k_memset(pl, 0, sizeof(pl));
    wr_u32(pl + 0, f32_from_milli(p1_milli));
    wr_u32(pl + 4, f32_from_milli(p2_milli));
    wr_u32(pl + 8, f32_from_milli(p3_milli));
    wr_u16(pl + 28, command);
    pl[30] = g_st.peer_sysid ? g_st.peer_sysid : 1;
    pl[31] = g_st.peer_compid ? g_st.peer_compid : 1;
    pl[32] = 0;
    send_frame(MAVLINK_MSG_COMMAND_LONG, pl, sizeof(pl));
}

static void send_command_long(uint16_t command, int32_t p1_milli) {
    send_command_long_2(command, p1_milli, 0);
}

/* One stream request per poll: a burst of seven commands in one packet train
   is exactly the kind of thing a NAT'd link drops. */
static void request_next_stream(uint64_t now) {
    const stream_req_t *r;
    if (g_stream_next >= STREAM_COUNT) return;
    if (now - g_last_req_us < 200000ull) return;      /* 5 Hz, unhurried    */
    g_last_req_us = now;
    r = &STREAMS[g_stream_next++];
    /* param1 = message id, param2 = interval in microseconds. Both are
       floats on the wire, which is why the conversion is checked on the
       host before anything trusts it. */
    send_command_long_2(MAV_CMD_SET_MESSAGE_INTERVAL,
                        (int32_t)(r->msgid * 1000u),
                        (int32_t)(r->interval_us * 1000u));
}

/* SET_POSITION_TARGET_LOCAL_NED: 53 bytes. We send velocity only, with the
   position/acceleration/yaw bits masked off — a velocity setpoint is the
   smallest thing that expresses "go this way, this fast" without pretending
   to know the vehicle's frame better than the autopilot does. */
#define SPTLN_IGNORE_POS   (7u)          /* bits 0..2                       */
#define SPTLN_IGNORE_ACC   (7u << 6)
#define SPTLN_IGNORE_YAW   (3u << 10)

static void send_velocity_setpoint(int32_t vx_mms, int32_t vy_mms, int32_t vz_mms) {
    uint8_t pl[53];
    k_memset(pl, 0, sizeof(pl));
    wr_u32(pl + 0, 0);                                  /* time_boot_ms     */
    wr_u32(pl + 16, f32_from_milli(vx_mms));            /* vx  (m/s)        */
    wr_u32(pl + 20, f32_from_milli(vy_mms));
    wr_u32(pl + 24, f32_from_milli(vz_mms));
    wr_u16(pl + 48, (uint16_t)(SPTLN_IGNORE_POS | SPTLN_IGNORE_ACC | SPTLN_IGNORE_YAW));
    pl[50] = g_st.peer_sysid ? g_st.peer_sysid : 1;
    pl[51] = g_st.peer_compid ? g_st.peer_compid : 1;
    pl[52] = 1;                                         /* MAV_FRAME_LOCAL_NED */
    send_frame(MAVLINK_MSG_SET_POSITION_TARGET_LOCAL_NED, pl, sizeof(pl));
}

/* The transport the runtime hands approved intents to. Nothing here can
   move an actuator: it is a request the autopilot may refuse. */
static bool transport(const intent_t *in) {
    uint64_t now = mono_us();
    switch (in->kind) {
    case INTENT_RETURN_HOME:
        send_command_long(MAV_CMD_NAV_RETURN_TO_LAUNCH, 0);
        break;
    case INTENT_LAND:
    case INTENT_ABORT:
        send_command_long(MAV_CMD_NAV_LAND, 0);
        break;
    case INTENT_HOLD:
        send_velocity_setpoint(0, 0, 0);
        break;
    case INTENT_AVOID:
    case INTENT_APPROACH:
    case INTENT_CONTINUE: {
        /* Rate-limit the streaming setpoint: Offboard needs more than 2 Hz
           to stay engaged, and more than 20 Hz buys nothing here. */
        int32_t speed = (int32_t)in->max_speed_mms;
        int32_t vx = in->kind == INTENT_AVOID ? -speed : speed;
        if (now - g_last_sp_tx_us < SETPOINT_PERIOD_US) return true;
        g_last_sp_tx_us = now;
        send_velocity_setpoint(vx, 0, 0);
        break;
    }
    default:
        return true;                    /* NONE: nothing to say             */
    }
    g_st.intents_sent++;
    return true;
}

static bool already_quiet(uint32_t id) {
    uint32_t i;
    for (i = 0; i < g_quiet_n; i++) if (g_quiet[i] == id) return true;
    return false;
}

/* Ask for silence on one message we cannot use. Interval -1 disables it. */
static void quieten_unknown(uint64_t now) {
    uint32_t id = g_rx.stats.last_unknown;
    if (!id || g_quiet_n >= QUIET_MAX) return;
    if (already_quiet(id)) { g_rx.stats.last_unknown = 0; return; }
    if (now - g_last_quiet_us < 100000ull) return;      /* 10 Hz at most    */
    g_last_quiet_us = now;
    g_quiet[g_quiet_n++] = id;
    g_rx.stats.last_unknown = 0;
    send_command_long_2(MAV_CMD_SET_MESSAGE_INTERVAL,
                        (int32_t)(id * 1000u), -1000);   /* -1.0 = disable  */
}

uint32_t mavbridge_quieted(void) { return g_quiet_n; }

/* Ask the autopilot to take its setpoints from us. This is a request, and a
   refusal is a valid answer: PX4 will not enter Offboard unless it is
   already receiving setpoints at a usable rate, which is precisely the point
   of the 20 Hz stream. Once engaged, PX4 runs its own failsafe when that
   stream stops — the safe behaviour belongs to the autopilot, not to us. */
bool mavbridge_offboard(void) {
    if (!g_running) return false;
    send_command_long_3(MAV_CMD_DO_SET_MODE,
                        (int32_t)(MAV_MODE_FLAG_CUSTOM_ENABLED * 1000),
                        (int32_t)(PX4_CUSTOM_MAIN_OFFBOARD * 1000), 0);
    return true;
}

/* --- lifecycle ----------------------------------------------------------- */

bool mavbridge_start(const uint8_t peer_ip[4], uint16_t peer_port,
                     uint16_t local_port) {
    if (!peer_ip) return false;
    k_memset(&g_st, 0, sizeof(g_st));
    mavlink_parser_init(&g_rx);
    mavlink_tx_init(&g_tx, 1, 191);      /* sysid 1, MAV_COMP_ID_ONBOARD    */
    for (int i = 0; i < 4; i++) g_peer_ip[i] = peer_ip[i];
    g_peer_port = peer_port;
    g_local_port = local_port;
    g_last_hb_tx_us = 0;
    g_last_sp_tx_us = 0;
    g_stream_next = 0;
    g_last_req_us = 0;
    g_quiet_n = 0;
    g_last_quiet_us = 0;
    g_link_reported = false;
    ip_udp_bind(local_port);
    robot_set_transport(transport);
    g_running = true;
    return true;
}

void mavbridge_stop(void) {
    g_running = false;
    ip_udp_unbind();
    robot_set_transport(NULL);
}

bool mavbridge_running(void) { return g_running; }

uint32_t mavbridge_poll(void) {
    /* A whole datagram, not a whole frame. MAVLink senders pack several
       messages into one UDP payload; a buffer one frame wide truncates the
       rest away, and the parser then reports the messages it never saw as a
       sequence gap. That is what an 8% "packet loss" turned out to be. */
    uint8_t buf[IP_UDP_MAX_DATAGRAM];
    uint8_t sip[4];
    uint16_t sport, n;
    uint32_t frames = 0;
    uint64_t now;

    if (!g_running) return 0;
    now = mono_us();

    while ((n = ip_udp_poll(g_local_port, sip, &sport, buf, sizeof(buf))) > 0) {
        for (uint16_t i = 0; i < n; i++)
            if (mavlink_parse_byte(&g_rx, buf[i])) {
                handle_message(&g_rx.msg);
                frames++;
            }
    }

    /* Silence is a link state, not an absence of information.
       The clock is read again here, after the drain: a heartbeat that
       arrived during it carries a newer timestamp than the `now` sampled at
       the top, and `now - last_heartbeat` on unsigned values then underflows
       into a number larger than any timeout. That declared the link down on
       every heartbeat it received — eighteen times in twenty-five seconds,
       with the heartbeats arriving at exactly 1 Hz. */
    now = mono_us();
    if (g_st.last_heartbeat_us && now > g_st.last_heartbeat_us &&
        now - g_st.last_heartbeat_us > LINK_TIMEOUT_US) report_link(false);

    if (now - g_last_hb_tx_us >= HEARTBEAT_PERIOD_US) {
        g_last_hb_tx_us = now;
        send_heartbeat();
    }
    request_next_stream(now);
    quieten_unknown(now);

    robot_step(now);
    return frames;
}

const mavbridge_stats_t  *mavbridge_stats(void)  { return &g_st; }
const mavlink_rx_stats_t *mavbridge_rx_stats(void) { return &g_rx.stats; }
