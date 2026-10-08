/* Minimal single-connection TCP client. Enough for HTTP GET / TLS. */
#include "tcp.h"
#include "ipstack.h"
#include "pit.h"
#include "rogex.h"

enum { TS_CLOSED=0, TS_SYN_SENT, TS_EST, TS_FIN_WAIT };

static int      g_st;
static uint8_t  g_dip[4];
static uint16_t g_dport, g_sport;
static uint32_t g_seq, g_ack;
static uint8_t  g_rx[48 * 1024];
static uint32_t g_rx_len;
static bool     g_got_synack;
static bool     g_got_rst;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void cpy4(uint8_t *d, const uint8_t *s) { for (int i=0;i<4;i++) d[i]=s[i]; }

/* RFC 793 TCP checksum over the IPv4 pseudo-header + segment.
   A zero checksum is invalid for TCP (unlike UDP) — slirp and real
   stacks drop those SYNs, which is why curl saw "tcp connect fail". */
static uint32_t sum16(const uint8_t *p, uint32_t len) {
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2; len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    return sum;
}

static uint16_t tcp_cksum(const uint8_t sip[4], const uint8_t dip[4],
                          const uint8_t *th, uint16_t tlen) {
    uint8_t ph[12];
    uint32_t sum;
    cpy4(ph, sip);
    cpy4(ph + 4, dip);
    ph[8] = 0;
    ph[9] = 6;
    ph[10] = (uint8_t)(tlen >> 8);
    ph[11] = (uint8_t)tlen;
    sum = sum16(ph, 12) + sum16(th, tlen);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)(~sum);
}

static bool tcp_xmit(uint8_t flags, const void *data, uint16_t dlen) {
    uint8_t seg[20 + 1160];
    const ip_cfg_t *cfg = ipstack_cfg();
    uint16_t tlen, csum;
    if (!cfg || !cfg->configured) return false;
    if (dlen > 1160) dlen = 1160;
    wr16(seg + 0, g_sport);
    wr16(seg + 2, g_dport);
    wr32(seg + 4, g_seq);
    wr32(seg + 8, g_ack);
    seg[12] = 0x50;
    seg[13] = flags;
    wr16(seg + 14, (uint16_t)(sizeof(g_rx) > g_rx_len
                             ? (sizeof(g_rx) - g_rx_len) : 0));
    wr16(seg + 16, 0);
    wr16(seg + 18, 0);
    if (data && dlen) k_memcpy(seg + 20, data, dlen);
    tlen = (uint16_t)(20 + dlen);
    csum = tcp_cksum(cfg->ip, g_dip, seg, tlen);
    wr16(seg + 16, csum);
    return ip_send(g_dip, 6, seg, tlen);
}

static bool tcp_xmit_syn(void) {
    /* SYN + MSS 1460 (kind=2,len=4). 24-byte header. */
    uint8_t seg[24];
    const ip_cfg_t *cfg = ipstack_cfg();
    uint16_t csum;
    if (!cfg || !cfg->configured) return false;
    wr16(seg + 0, g_sport);
    wr16(seg + 2, g_dport);
    wr32(seg + 4, g_seq);
    wr32(seg + 8, 0);
    seg[12] = 0x60;          /* data offset = 6 (24 bytes) */
    seg[13] = 0x02;          /* SYN */
    wr16(seg + 14, (uint16_t)(sizeof(g_rx) > g_rx_len
                             ? (sizeof(g_rx) - g_rx_len) : 0));
    wr16(seg + 16, 0);
    wr16(seg + 18, 0);
    seg[20] = 2; seg[21] = 4; seg[22] = 0x05; seg[23] = 0xb4; /* MSS 1460 */
    csum = tcp_cksum(cfg->ip, g_dip, seg, 24);
    wr16(seg + 16, csum);
    return ip_send(g_dip, 6, seg, 24);
}

static void tcp_rx(const uint8_t *sip, const uint8_t *dip,
                   const uint8_t *seg, uint16_t seglen) {
    uint16_t sport, dport, plen;
    uint8_t  doff, flags;
    uint32_t seq, ack;
    (void)dip;
    if (g_st == TS_CLOSED || seglen < 20) return;
    if (sip[0]!=g_dip[0]||sip[1]!=g_dip[1]||sip[2]!=g_dip[2]||sip[3]!=g_dip[3]) return;
    sport = rd16(seg + 0);
    dport = rd16(seg + 2);
    if (sport != g_dport || dport != g_sport) return;
    doff = (uint8_t)((seg[12] >> 4) * 4);
    if (doff < 20 || seglen < doff) return;
    flags = seg[13];
    seq = rd32(seg + 4);
    ack = rd32(seg + 8);
    plen = (uint16_t)(seglen - doff);

    if (flags & 0x04) { /* RST */
        g_got_rst = true;
        g_st = TS_CLOSED;
        return;
    }

    if (g_st == TS_SYN_SENT && (flags & 0x12) == 0x12) { /* SYN+ACK */
        g_ack = seq + 1;
        g_seq = ack;
        g_got_synack = true;
        tcp_xmit(0x10, NULL, 0); /* ACK */
        g_st = TS_EST;
        return;
    }
    if (g_st != TS_EST) return;
    (void)ack;

    if (plen && seq == g_ack && g_rx_len + plen <= sizeof(g_rx)) {
        k_memcpy(g_rx + g_rx_len, seg + doff, plen);
        g_rx_len += plen;
        g_ack = seq + plen;
        tcp_xmit(0x10, NULL, 0);
    } else if (plen && seq != g_ack) {
        /* Out of order / dup: re-ACK what we have so the peer retransmits. */
        tcp_xmit(0x10, NULL, 0);
    }

    if (flags & 0x01) { /* FIN — after any data in this segment */
        if (seq == g_ack || (plen && seq + plen == g_ack))
            g_ack += 1;
        tcp_xmit(0x11, NULL, 0); /* FIN+ACK */
        g_st = TS_CLOSED;
    }
}

void tcp_init(void) {
    g_st = TS_CLOSED;
    g_rx_len = 0;
    g_sport = 40000;
    g_got_rst = false;
    ip_set_tcp_rx(tcp_rx);
}

void tcp_service(void) { ipstack_service(); }

bool tcp_connect(const uint8_t dip[4], uint16_t dport, uint32_t timeout_ms) {
    uint64_t start, last_syn;
    uint32_t ticks;
    if (!ipstack_configured()) return false;
    tcp_close();
    cpy4(g_dip, dip);
    g_dport = dport;
    g_sport = (uint16_t)(40000 + (pit_ticks() & 0x7ff));
    g_seq = 1000 + (uint32_t)pit_ticks();
    g_ack = 0;
    g_rx_len = 0;
    g_got_synack = false;
    g_got_rst = false;
    g_st = TS_SYN_SENT;

    if (!tcp_xmit_syn()) { g_st = TS_CLOSED; return false; }

    start = pit_ticks();
    last_syn = start;
    ticks = timeout_ms / 10;
    if (ticks < 80) ticks = 80; /* at least 800 ms */
    while (pit_ticks() - start < ticks) {
        ipstack_service();
        if (g_st == TS_EST) return true;
        if (g_got_rst) { g_st = TS_CLOSED; return false; }
        /* Retransmit SYN every 500 ms — slirp and metal both drop the first. */
        if (pit_ticks() - last_syn >= 50) {
            tcp_xmit_syn();
            last_syn = pit_ticks();
        }
    }
    g_st = TS_CLOSED;
    return false;
}

bool tcp_connected(void) { return g_st == TS_EST; }

void tcp_close(void) {
    if (g_st == TS_EST) tcp_xmit(0x11, NULL, 0); /* FIN+ACK */
    g_st = TS_CLOSED;
    g_rx_len = 0;
}

bool tcp_send(const void *data, uint16_t len) {
    const uint8_t *p;
    uint16_t off = 0;
    if (g_st != TS_EST || !data) return false;
    p = (const uint8_t *)data;
    while (off < len) {
        uint16_t chunk = (uint16_t)(len - off);
        if (chunk > 1160) chunk = 1160;
        if (!tcp_xmit(0x18, p + off, chunk)) return false; /* PSH+ACK */
        g_seq += chunk;
        off = (uint16_t)(off + chunk);
        ipstack_service();
    }
    return true;
}

uint16_t tcp_recv_nb(void *buf, uint16_t max) {
    uint16_t n;
    uint8_t *d;
    ipstack_service();
    if (!g_rx_len) return 0;
    n = (g_rx_len > max) ? max : (uint16_t)g_rx_len;
    d = (uint8_t *)buf;
    k_memcpy(d, g_rx, n);
    if ((uint32_t)n < g_rx_len) k_memcpy(g_rx, g_rx + n, (size_t)(g_rx_len - n));
    g_rx_len -= n;
    return n;
}

uint16_t tcp_recv(void *buf, uint16_t max, uint32_t timeout_ms) {
    uint64_t start = pit_ticks();
    uint32_t ticks = timeout_ms / 10;
    if (!ticks) ticks = 1;
    while (pit_ticks() - start < ticks) {
        uint16_t n = tcp_recv_nb(buf, max);
        if (n) return n;
        if (g_st == TS_CLOSED && !g_rx_len) return 0;
    }
    return tcp_recv_nb(buf, max);
}
