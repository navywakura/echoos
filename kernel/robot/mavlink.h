/*
 * MAVLink 2 — the subset this machine needs to talk to a flight controller.
 *
 * Hand-written, not generated: a generator would drag a build-time Python
 * dependency and 300 messages into a kernel that needs ten. What is here is
 * the framing (parser, CRC, sequence, sysid/compid) plus the messages the
 * robotic runtime actually sends and receives.
 *
 * Framing, MAVLink 2 (v2.0):
 *   0  STX 0xFD
 *   1  payload length
 *   2  incompatibility flags   (unsupported values must be rejected)
 *   3  compatibility flags     (unknown values may be ignored)
 *   4  sequence
 *   5  system id
 *   6  component id
 *   7..9  message id, 24-bit little endian
 *   10.. payload (trailing zero bytes truncated on the wire)
 *   ..   CRC16/MCRF4XX over bytes 1.. plus the message's CRC_EXTRA
 *
 * Signing (incompat flag 0x01) is not implemented, so a signed frame is
 * rejected rather than parsed as if the signature were payload.
 */
#ifndef ECHOS_MAVLINK_H
#define ECHOS_MAVLINK_H

#include "rogex.h"

#define MAV_STX_V2        0xFDu
#define MAV_HDR_BYTES     10u
#define MAV_CRC_BYTES     2u
#define MAV_MAX_PAYLOAD   255u
#define MAV_MAX_FRAME     (MAV_HDR_BYTES + MAV_MAX_PAYLOAD + MAV_CRC_BYTES)
#define MAV_INCOMPAT_SIGNED 0x01u

/* Message ids we know. Anything else is counted and skipped, never guessed. */
#define MAVLINK_MSG_HEARTBEAT                   0u
#define MAVLINK_MSG_SYS_STATUS                  1u
#define MAVLINK_MSG_COMMAND_LONG              76u
#define MAVLINK_MSG_COMMAND_ACK               77u
#define MAVLINK_MSG_ATTITUDE                  30u
#define MAVLINK_MSG_LOCAL_POSITION_NED        32u
#define MAVLINK_MSG_GLOBAL_POSITION_INT       33u
#define MAVLINK_MSG_SET_POSITION_TARGET_LOCAL_NED  84u
#define MAVLINK_MSG_DISTANCE_SENSOR          132u
#define MAVLINK_MSG_BATTERY_STATUS           147u

/* CRC_EXTRA, from the message definitions. Wrong values here do not corrupt
   data quietly: every frame fails its CRC and the link never comes up, which
   is why the receiver counts CRC errors separately from unknown ids. */
uint8_t mavlink_crc_extra(uint32_t msgid);
bool    mavlink_known_msgid(uint32_t msgid);

/* --- incremental parser -------------------------------------------------- */
typedef struct {
    uint8_t  magic, len, incompat, compat, seq, sysid, compid;
    uint32_t msgid;
    uint8_t  payload[MAV_MAX_PAYLOAD];
} mavlink_message_t;

typedef enum {
    MAV_PARSE_IDLE = 0, MAV_PARSE_LEN, MAV_PARSE_INCOMPAT, MAV_PARSE_COMPAT,
    MAV_PARSE_SEQ, MAV_PARSE_SYSID, MAV_PARSE_COMPID, MAV_PARSE_MSGID1,
    MAV_PARSE_MSGID2, MAV_PARSE_MSGID3, MAV_PARSE_PAYLOAD,
    MAV_PARSE_CRC1, MAV_PARSE_CRC2
} mavlink_parse_state_t;

typedef struct {
    uint32_t frames;            /* complete, CRC-valid frames               */
    uint32_t crc_errors;
    uint32_t unknown_msgid;     /* well-formed but not in our table         */
    uint32_t last_unknown;      /* the id of the most recent one            */
    uint32_t rejected_incompat; /* signed or otherwise unsupported          */
    uint32_t bad_stx;           /* bytes discarded looking for a frame      */
    uint32_t seq_gaps;          /* frames the sender says we never saw      */
    uint32_t lost_estimate;     /* how many, summed over the gaps           */
    uint32_t unverified_seq;    /* seq advances taken from unchecked frames */
    /* Which ids we are ignoring, not just how many: "202 unknown" is a
       number, "202 of ESTIMATOR_STATUS and friends" is a finding. */
    uint32_t unknown_ids[8];
    uint32_t unknown_ids_n;
    uint32_t senders;           /* distinct sysid/compid pairs seen         */
    uint32_t senders_dropped;   /* pairs the table had no room for          */
} mavlink_rx_stats_t;

/* Sequence numbers are per sending component, not per link: PX4 talks from
   its autopilot and from other components down the same socket, and one
   shared counter turns that interleaving into gaps that never happened.
   Eight is more than a companion link ever sees; overflow is counted rather
   than folded into a shared slot, so the number stays readable. */
#define MAV_SEQ_TRACKED 8

typedef struct {
    uint8_t sysid, compid, last_seq;
    bool    used;
} mavlink_seq_track_t;

typedef struct {
    mavlink_parse_state_t state;
    mavlink_message_t     msg;
    uint16_t crc, rx_crc;
    uint8_t  payload_index;
    mavlink_seq_track_t seq[MAV_SEQ_TRACKED];
    mavlink_rx_stats_t stats;
} mavlink_parser_t;

void mavlink_parser_init(mavlink_parser_t *p);
/* Feed one byte. True when `p->msg` now holds a complete, verified frame. */
bool mavlink_parse_byte(mavlink_parser_t *p, uint8_t c);

/* --- writing ------------------------------------------------------------- */
typedef struct {
    uint8_t sysid, compid, seq;
    uint32_t sent;
} mavlink_tx_t;

void     mavlink_tx_init(mavlink_tx_t *tx, uint8_t sysid, uint8_t compid);
/* Frame `payload` into `buf` (>= MAV_MAX_FRAME). Returns the frame length.
   Trailing zero bytes are truncated, as v2 requires. */
uint16_t mavlink_pack(mavlink_tx_t *tx, uint32_t msgid, const uint8_t *payload,
                      uint16_t payload_len, uint8_t *buf, uint16_t buf_len);

/* CRC16/MCRF4XX, the one MAVLink uses. Exposed for the frame builders. */
uint16_t mavlink_crc16(const uint8_t *data, uint16_t len, uint16_t start);

#endif /* ECHOS_MAVLINK_H */
