/*
 * PX4 bridge: MAVLink 2 over UDP, wired to the robotic runtime.
 *
 * Inbound telemetry becomes sensor records; the runtime's approved intents
 * become MAVLink commands. echOS produces high-level decisions only — the
 * autopilot keeps stabilisation, state estimation, mixing, failsafes,
 * geofence, return and emergency landing.
 *
 * The link is watched: silence for longer than the timeout is reported into
 * the pipeline as a link-down sensor record, which is what makes the safety
 * gate degrade to HOLD. PX4 runs its own failsafe on its own side; we do not
 * pretend to be it.
 */
#ifndef ECHOS_MAVBRIDGE_H
#define ECHOS_MAVBRIDGE_H

#include "rogex.h"
#include "mavlink.h"

typedef struct {
    uint32_t rx_frames, tx_frames;
    uint32_t rx_by_type[16];      /* indexed by our own small table         */
    uint32_t sensors_produced;    /* telemetry turned into sensor records   */
    uint32_t intents_sent;        /* decisions that left as MAVLink         */
    uint32_t command_acks;
    uint32_t last_ack_command, last_ack_result;
    uint32_t heartbeats_rx, heartbeats_tx;
    uint32_t link_drops;          /* transitions to "down"                  */
    uint64_t last_heartbeat_us;
    uint64_t max_hb_gap_us;     /* the worst silence between heartbeats     */
    uint64_t last_hb_gap_us;
    bool     link_up;
    uint8_t  peer_sysid, peer_compid;
    uint8_t  autopilot_mode, autopilot_armed;
} mavbridge_stats_t;

/* Start talking to `ip:port` from `local_port`. Nothing is sent until
   mavbridge_poll() runs. */
bool mavbridge_start(const uint8_t peer_ip[4], uint16_t peer_port,
                     uint16_t local_port);
void mavbridge_stop(void);
bool mavbridge_running(void);

/* Pump the link: read what arrived, feed the pipeline, send the heartbeat
   when due, and push approved intents out. Call from a task or the idle
   path, never from an interrupt. Returns frames received this call. */
uint32_t mavbridge_poll(void);

/* How many message types we asked the autopilot to stop sending, because we
   cannot use them and the NIC drops what it cannot absorb. */
uint32_t mavbridge_quieted(void);

/* Ask PX4 to take its setpoints from this link (Offboard). A request, not a
   takeover: PX4 decides, and once engaged its own failsafe owns what happens
   when the stream stops. */
bool mavbridge_offboard(void);

const mavbridge_stats_t  *mavbridge_stats(void);
const mavlink_rx_stats_t *mavbridge_rx_stats(void);

#endif /* ECHOS_MAVBRIDGE_H */
