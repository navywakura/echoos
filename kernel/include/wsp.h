/*
 * RogexWSP — 16-byte fixed frame on the Spiking Event Bus.
 *
 * A transport contract, not a language. Version, opcode, two node ids, a
 * sequence number and eight payload bytes: enough to route a short message
 * between producers on the bus and to say what kind of message it is.
 *
 * The wire is byte-addressed and big-endian for the sequence number, so the
 * same frame reads the same on a future AArch64 target. Fixed size, no
 * allocation, no parsing of text.
 *
 * A SEB spike carries eight payload bytes, so a frame handed to
 * seb_emit_spike(SEB_CH_WSP, ...) travels as its payload; the header half
 * describes it for whoever reads the ring.
 *
 * The 2.x line also had a semantic packet here (atoms, six affect axes and a
 * generator table) that existed to feed a retired chatbot. It is out of the
 * product with the rest of that experiment; git history keeps it.
 */
#ifndef RXOS_WSP_H
#define RXOS_WSP_H

#include "rogex.h"

#define WSP_PACKET_BYTES 16
#define WSP_VERSION      0x01u

enum {
    WSP_OP_PING        = 0x01u,
    WSP_OP_PONG        = 0x02u,
    WSP_OP_DATA        = 0x03u,
    WSP_OP_SPIKE_PULSE = 0x04u
};

#define WSP_PAYLOAD_BYTES 8

typedef struct ROGEX_PACKED {
    uint8_t version;
    uint8_t opcode;
    uint8_t src_node;
    uint8_t dst_node;
    uint8_t seq[4];                      /* big-endian on the wire        */
    uint8_t payload[WSP_PAYLOAD_BYTES];
} wsp_frame_t;

_Static_assert(sizeof(wsp_frame_t) == WSP_PACKET_BYTES, "wsp_frame_t must be 16 B");
_Static_assert(__builtin_offsetof(wsp_frame_t, seq) == 4, "seq at byte 4");
_Static_assert(__builtin_offsetof(wsp_frame_t, payload) == 8, "payload at byte 8");

static inline uint32_t wsp_seq_get(const wsp_frame_t *f) {
    return ((uint32_t)f->seq[0] << 24) | ((uint32_t)f->seq[1] << 16) |
           ((uint32_t)f->seq[2] << 8)  |  (uint32_t)f->seq[3];
}

static inline void wsp_seq_set(wsp_frame_t *f, uint32_t v) {
    f->seq[0] = (uint8_t)(v >> 24); f->seq[1] = (uint8_t)(v >> 16);
    f->seq[2] = (uint8_t)(v >> 8);  f->seq[3] = (uint8_t)v;
}

static inline const char *wsp_opcode_name(uint8_t op) {
    switch (op) {
    case WSP_OP_PING:        return "PING";
    case WSP_OP_PONG:        return "PONG";
    case WSP_OP_DATA:        return "DATA";
    case WSP_OP_SPIKE_PULSE: return "SPIKE_PULSE";
    default:                 return "user-defined";
    }
}

#endif /* RXOS_WSP_H */
