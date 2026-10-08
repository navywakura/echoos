#include "mavlink.h"

/* CRC_EXTRA per message, from the MAVLink common dialect. These are the one
   thing in this file that cannot be derived: they are a checksum over each
   message's field names and types, computed by the generator. A wrong value
   makes every frame of that type fail its CRC — loudly, in crc_errors —
   rather than corrupting a field quietly. */
typedef struct { uint32_t id; uint8_t crc_extra; } mav_msg_info_t;

static const mav_msg_info_t MSG_INFO[] = {
    { MAVLINK_MSG_HEARTBEAT,                    50 },
    { MAVLINK_MSG_SYS_STATUS,                  124 },
    { MAVLINK_MSG_ATTITUDE,                     39 },
    { MAVLINK_MSG_LOCAL_POSITION_NED,          185 },
    { MAVLINK_MSG_GLOBAL_POSITION_INT,         104 },
    { MAVLINK_MSG_COMMAND_LONG,                152 },
    { MAVLINK_MSG_COMMAND_ACK,                 143 },
    { MAVLINK_MSG_SET_POSITION_TARGET_LOCAL_NED, 143 },
    { MAVLINK_MSG_DISTANCE_SENSOR,              85 },
    { MAVLINK_MSG_BATTERY_STATUS,              154 },
};
#define MSG_INFO_N ((uint32_t)(sizeof(MSG_INFO) / sizeof(MSG_INFO[0])))

uint8_t mavlink_crc_extra(uint32_t msgid) {
    uint32_t i;
    for (i = 0; i < MSG_INFO_N; i++)
        if (MSG_INFO[i].id == msgid) return MSG_INFO[i].crc_extra;
    return 0;
}

bool mavlink_known_msgid(uint32_t msgid) {
    uint32_t i;
    for (i = 0; i < MSG_INFO_N; i++)
        if (MSG_INFO[i].id == msgid) return true;
    return false;
}

/* CRC16/MCRF4XX: reflected, poly 0x1021, init 0xFFFF, no final xor. */
uint16_t mavlink_crc16(const uint8_t *data, uint16_t len, uint16_t start) {
    uint16_t crc = start;
    uint16_t i;
    for (i = 0; i < len; i++) {
        uint8_t t = data[i] ^ (uint8_t)(crc & 0xFF);
        t ^= (uint8_t)(t << 4);
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)t << 8) ^ ((uint16_t)t << 3) ^
                         ((uint16_t)t >> 4));
    }
    return crc;
}

/* --- parser -------------------------------------------------------------- */

void mavlink_parser_init(mavlink_parser_t *p) {
    k_memset(p, 0, sizeof(*p));
    p->state = MAV_PARSE_IDLE;
}

static void crc_acc(mavlink_parser_t *p, uint8_t c) {
    p->crc = mavlink_crc16(&c, 1, p->crc);
}

static void track_seq(mavlink_parser_t *p) {
    int i, free_slot = -1;

    for (i = 0; i < MAV_SEQ_TRACKED; i++) {
        if (!p->seq[i].used) { if (free_slot < 0) free_slot = i; continue; }
        if (p->seq[i].sysid != p->msg.sysid ||
            p->seq[i].compid != p->msg.compid) continue;
        {
            uint8_t expected = (uint8_t)(p->seq[i].last_seq + 1u);
            if (p->msg.seq != expected) {
                p->stats.seq_gaps++;
                p->stats.lost_estimate += (uint8_t)(p->msg.seq - expected);
            }
        }
        p->seq[i].last_seq = p->msg.seq;
        return;
    }
    /* First frame from this component: record where its counter starts.
       Counting a gap here would charge us for everything it sent before we
       were listening. */
    if (free_slot < 0) { p->stats.senders_dropped++; return; }
    p->seq[free_slot].used = true;
    p->seq[free_slot].sysid = p->msg.sysid;
    p->seq[free_slot].compid = p->msg.compid;
    p->seq[free_slot].last_seq = p->msg.seq;
    p->stats.senders++;
}

bool mavlink_parse_byte(mavlink_parser_t *p, uint8_t c) {
    switch (p->state) {
    case MAV_PARSE_IDLE:
        if (c != MAV_STX_V2) { p->stats.bad_stx++; return false; }
        p->msg.magic = c;
        p->crc = 0xFFFF;
        p->state = MAV_PARSE_LEN;
        return false;
    case MAV_PARSE_LEN:
        p->msg.len = c; crc_acc(p, c); p->state = MAV_PARSE_INCOMPAT; return false;
    case MAV_PARSE_INCOMPAT:
        p->msg.incompat = c; crc_acc(p, c);
        /* We implement no incompatibility flag, so any of them means the
           rest of the frame is not what we would think it is. Signed frames
           (0x01) land here: their signature would otherwise be read as
           payload. */
        if (c != 0) {
            p->stats.rejected_incompat++;
            p->state = MAV_PARSE_IDLE;
            return false;
        }
        p->state = MAV_PARSE_COMPAT;
        return false;
    case MAV_PARSE_COMPAT:
        p->msg.compat = c; crc_acc(p, c); p->state = MAV_PARSE_SEQ; return false;
    case MAV_PARSE_SEQ:
        p->msg.seq = c; crc_acc(p, c); p->state = MAV_PARSE_SYSID; return false;
    case MAV_PARSE_SYSID:
        p->msg.sysid = c; crc_acc(p, c); p->state = MAV_PARSE_COMPID; return false;
    case MAV_PARSE_COMPID:
        p->msg.compid = c; crc_acc(p, c); p->state = MAV_PARSE_MSGID1; return false;
    case MAV_PARSE_MSGID1:
        p->msg.msgid = c; crc_acc(p, c); p->state = MAV_PARSE_MSGID2; return false;
    case MAV_PARSE_MSGID2:
        p->msg.msgid |= (uint32_t)c << 8; crc_acc(p, c);
        p->state = MAV_PARSE_MSGID3; return false;
    case MAV_PARSE_MSGID3:
        p->msg.msgid |= (uint32_t)c << 16; crc_acc(p, c);
        p->payload_index = 0;
        k_memset(p->msg.payload, 0, sizeof(p->msg.payload));
        p->state = p->msg.len ? MAV_PARSE_PAYLOAD : MAV_PARSE_CRC1;
        return false;
    case MAV_PARSE_PAYLOAD:
        p->msg.payload[p->payload_index++] = c;
        crc_acc(p, c);
        if (p->payload_index >= p->msg.len) p->state = MAV_PARSE_CRC1;
        return false;
    case MAV_PARSE_CRC1:
        p->rx_crc = c;
        p->state = MAV_PARSE_CRC2;
        return false;
    case MAV_PARSE_CRC2: {
        uint8_t extra = mavlink_crc_extra(p->msg.msgid);
        bool known = mavlink_known_msgid(p->msg.msgid);
        p->rx_crc |= (uint16_t)c << 8;
        p->state = MAV_PARSE_IDLE;
        if (!known) {
            /* Cannot check the CRC of a message whose CRC_EXTRA we do not
               have. Count it and drop it; do not hand up a frame we could
               not verify. */
            p->stats.unknown_msgid++;
            p->stats.last_unknown = p->msg.msgid;
            {   uint32_t k, seen = 0;
                for (k = 0; k < p->stats.unknown_ids_n; k++)
                    if (p->stats.unknown_ids[k] == p->msg.msgid) { seen = 1; break; }
                if (!seen && p->stats.unknown_ids_n < 8)
                    p->stats.unknown_ids[p->stats.unknown_ids_n++] = p->msg.msgid;
            }
            /* The sequence byte is the sender's counter whether or not we
               can decode the message. Returning without recording it makes
               every message we decline to parse look like a frame that went
               missing — which is how a clean PX4 link came to report 25%
               packet loss. The CRC could not be checked without the
               CRC_EXTRA, so the advance is counted as unverified and the
               frame is still not handed up. */
            p->stats.unverified_seq++;
            track_seq(p);
            return false;
        }
        crc_acc(p, extra);
        if (p->crc != p->rx_crc) { p->stats.crc_errors++; return false; }
        p->stats.frames++;
        track_seq(p);
        return true;
    }
    default:
        p->state = MAV_PARSE_IDLE;
        return false;
    }
}

/* --- writing ------------------------------------------------------------- */

void mavlink_tx_init(mavlink_tx_t *tx, uint8_t sysid, uint8_t compid) {
    k_memset(tx, 0, sizeof(*tx));
    tx->sysid = sysid;
    tx->compid = compid;
}

uint16_t mavlink_pack(mavlink_tx_t *tx, uint32_t msgid, const uint8_t *payload,
                      uint16_t payload_len, uint8_t *buf, uint16_t buf_len) {
    uint16_t len = payload_len, crc, i;
    if (!tx || !buf || payload_len > MAV_MAX_PAYLOAD) return 0;
    /* v2 truncates trailing zeroes; a receiver zero-fills what is missing. */
    while (len > 0 && payload[len - 1] == 0) len--;
    if (buf_len < MAV_HDR_BYTES + len + MAV_CRC_BYTES) return 0;

    buf[0] = MAV_STX_V2;
    buf[1] = (uint8_t)len;
    buf[2] = 0;                       /* no incompatibility flags: unsigned */
    buf[3] = 0;
    buf[4] = tx->seq++;
    buf[5] = tx->sysid;
    buf[6] = tx->compid;
    buf[7] = (uint8_t)(msgid & 0xFF);
    buf[8] = (uint8_t)((msgid >> 8) & 0xFF);
    buf[9] = (uint8_t)((msgid >> 16) & 0xFF);
    for (i = 0; i < len; i++) buf[MAV_HDR_BYTES + i] = payload[i];

    crc = mavlink_crc16(buf + 1, (uint16_t)(MAV_HDR_BYTES - 1 + len), 0xFFFF);
    { uint8_t extra = mavlink_crc_extra(msgid);
      crc = mavlink_crc16(&extra, 1, crc); }
    buf[MAV_HDR_BYTES + len]     = (uint8_t)(crc & 0xFF);
    buf[MAV_HDR_BYTES + len + 1] = (uint8_t)(crc >> 8);
    tx->sent++;
    return (uint16_t)(MAV_HDR_BYTES + len + MAV_CRC_BYTES);
}
