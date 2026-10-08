/* Minimal IPv4 + ARP + ICMP + UDP. Frames demuxed with Rogex Wired L2. */
#include "ipstack.h"
#include "netif.h"
#include "mono.h"

#define ARP_ eth 1
#define IP_HLEN  20
#define UDP_HLEN 8

static ip_cfg_t g_cfg;
static ip_tcp_rx_fn g_tcp_rx;
static ip_l2_rx_fn  g_l2_rx;
static uint16_t g_ip_id;

/* ARP cache */
#define ARP_N 16
static struct {
    uint8_t ip[4];
    uint8_t mac[6];
    bool    used;
} g_arp[ARP_N];

/* pending UDP recv for one port */
static struct {
    bool     armed;
    uint16_t port;
    uint8_t  sip[4];
    uint16_t sport;
    uint8_t  buf[1500];
    uint16_t len;
} g_udp_rx;

/* A second receive path, for a port that gets a stream rather than a reply.
   The one-shot above keeps exactly one datagram, which is right for DNS and
   wrong for a control link: a flight controller sends several messages per
   period and they would overwrite each other. This is a small static ring,
   non-blocking, with its own overrun counter — losing a packet here is our
   fault and has to be countable separately from loss on the wire. */
#define UDP_RING_SLOTS 48
/* A full UDP payload inside one Ethernet frame: 1514 - 14 - 20 - 8. The
   ring was 320 bytes and refused anything larger, which is invisible until
   the sender packs several messages into one datagram — PX4 does, and half
   its telemetry was being counted as a sequence gap with nothing in this
   kernel able to say why. 48 slots of it is 69 KiB of BSS, paid once. */
#define UDP_RING_MTU   IP_UDP_MAX_DATAGRAM

static struct {
    bool     bound;
    uint16_t port;
    uint32_t head, tail, drops;
    struct {
        uint8_t  sip[4];
        uint16_t sport, len;
        uint8_t  data[UDP_RING_MTU];
    } slot[UDP_RING_SLOTS];
    uint32_t accepted;               /* datagrams that reached the ring */
} g_udp_ring;

static void cpy4(uint8_t *d, const uint8_t *s) { for (int i = 0; i < 4; i++) d[i] = s[i]; }
static void cpy6(uint8_t *d, const uint8_t *s) { for (int i = 0; i < 6; i++) d[i] = s[i]; }
static bool eq4(const uint8_t *a, const uint8_t *b) {
    return a[0]==b[0]&&a[1]==b[1]&&a[2]==b[2]&&a[3]==b[3];
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}

uint16_t ip_checksum(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2; len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)(~sum);
}

void ip_fmt(const uint8_t ip[4], char *out, uint32_t max) {
    char t[16]; int n = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = ip[i];
        char d[4]; int k = 0;
        if (!v) d[k++] = '0';
        else { char r[4]; int j = 0; while (v) { r[j++] = (char)('0' + (v % 10)); v /= 10; }
               while (j) d[k++] = r[--j]; }
        for (int j = 0; j < k && n + 1 < (int)max; j++) t[n++] = d[j];
        if (i < 3 && n + 1 < (int)max) t[n++] = '.';
    }
    t[n] = '\0';
    for (uint32_t i = 0; i < max; i++) { out[i] = t[i]; if (!t[i]) break; }
}

bool ip_parse(const char *s, uint8_t out[4]) {
    if (!s) return false;
    for (int i = 0; i < 4; i++) {
        int v = 0, dig = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); if (v > 255) return false; s++; dig++; }
        if (!dig) return false;
        out[i] = (uint8_t)v;
        if (i < 3) { if (*s != '.') return false; s++; }
    }
    return *s == '\0';
}

void ipstack_init(void) {
    for (int i = 0; i < ARP_N; i++) g_arp[i].used = false;
    g_cfg.configured = false;
    g_tcp_rx = NULL;
    g_l2_rx = NULL;
    g_udp_rx.armed = false;
}

void ip_set_tcp_rx(ip_tcp_rx_fn fn) { g_tcp_rx = fn; }
void ip_set_l2_rx(ip_l2_rx_fn fn)   { g_l2_rx = fn; }

bool ipstack_configured(void) { return g_cfg.configured && net_present(); }
const ip_cfg_t *ipstack_cfg(void) { return &g_cfg; }

void ipstack_set_addr(const uint8_t ip[4], const uint8_t mask[4],
                      const uint8_t gw[4], const uint8_t dns[4]) {
    cpy4(g_cfg.ip, ip); cpy4(g_cfg.mask, mask);
    cpy4(g_cfg.gw, gw); cpy4(g_cfg.dns, dns);
    g_cfg.configured = true;
}

void ipstack_use_qemu_defaults(void) {
    uint8_t ip[4]   = {10,0,2,15};
    uint8_t mask[4] = {255,255,255,0};
    uint8_t gw[4]   = {10,0,2,2};
    uint8_t dns[4]  = {10,0,2,3};
    ipstack_set_addr(ip, mask, gw, dns);
}

static void arp_cache_put(const uint8_t ip[4], const uint8_t mac[6]) {
    for (int i = 0; i < ARP_N; i++)
        if (g_arp[i].used && eq4(g_arp[i].ip, ip)) { cpy6(g_arp[i].mac, mac); return; }
    for (int i = 0; i < ARP_N; i++)
        if (!g_arp[i].used) {
            g_arp[i].used = true; cpy4(g_arp[i].ip, ip); cpy6(g_arp[i].mac, mac); return;
        }
    /* overwrite slot 0 */
    g_arp[0].used = true; cpy4(g_arp[0].ip, ip); cpy6(g_arp[0].mac, mac);
}

static bool arp_cache_get(const uint8_t ip[4], uint8_t mac[6]) {
    for (int i = 0; i < ARP_N; i++)
        if (g_arp[i].used && eq4(g_arp[i].ip, ip)) { cpy6(mac, g_arp[i].mac); return true; }
    return false;
}

static bool eth_send(const uint8_t dmac[6], uint16_t etype,
                     const void *payload, uint16_t plen) {
    uint8_t f[1514];
    const uint8_t *smac = net_mac();
    if (!smac || plen + 14 > sizeof(f)) return false;
    cpy6(f, dmac); cpy6(f + 6, smac);
    wr16(f + 12, etype);
    const uint8_t *p = (const uint8_t *)payload;
    for (uint16_t i = 0; i < plen; i++) f[14 + i] = p[i];
    uint16_t total = (uint16_t)(14 + plen);
    if (total < 60) { /* pad Ethernet minimum */
        for (uint16_t i = total; i < 60; i++) f[i] = 0;
        total = 60;
    }
    return net_send(f, total);
}

static bool arp_request(const uint8_t tip[4]) {
    uint8_t p[28];
    wr16(p + 0, 1);      /* htype eth */
    wr16(p + 2, 0x0800); /* ptype ipv4 */
    p[4] = 6; p[5] = 4;
    wr16(p + 6, 1);      /* request */
    cpy6(p + 8, net_mac());
    cpy4(p + 14, g_cfg.ip);
    for (int i = 0; i < 6; i++) p[18 + i] = 0;
    cpy4(p + 24, tip);
    static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    return eth_send(bcast, ETH_TYPE_ARP, p, 28);
}

static void arp_reply_to(const uint8_t *req) {
    /* req is ARP payload; answer who-has us */
    if (rd16(req + 6) != 1) return;
    if (!eq4(req + 24, g_cfg.ip)) return;
    uint8_t p[28];
    wr16(p + 0, 1); wr16(p + 2, 0x0800); p[4] = 6; p[5] = 4;
    wr16(p + 6, 2); /* reply */
    cpy6(p + 8, net_mac());
    cpy4(p + 14, g_cfg.ip);
    cpy6(p + 18, req + 8);
    cpy4(p + 24, req + 14);
    eth_send(req + 8, ETH_TYPE_ARP, p, 28);
    arp_cache_put(req + 14, req + 8);
}

bool ip_arp_resolve(const uint8_t ip[4], uint8_t mac[6], uint32_t timeout_ms) {
    if (!g_cfg.configured) return false;
    if (arp_cache_get(ip, mac)) return true;
    /* same subnet? else resolve gateway */
    uint8_t target[4];
    bool local = true;
    for (int i = 0; i < 4; i++)
        if ((ip[i] & g_cfg.mask[i]) != (g_cfg.ip[i] & g_cfg.mask[i])) local = false;
    cpy4(target, local ? ip : g_cfg.gw);
    if (arp_cache_get(target, mac)) return true;
    arp_request(target);
    uint64_t start = mono_us();
    uint64_t budget = (uint64_t)timeout_ms * 1000u;
    while (mono_us() - start < budget) {
        ipstack_service();
        if (arp_cache_get(target, mac)) return true;
    }
    return false;
}

static struct {
    bool    got;
    uint8_t type;      /* 0 echo reply, 11 time exceeded */
    uint8_t sip[4];
    uint16_t id;
} g_icmp_rx;

bool ip_send_ttl(const uint8_t dip[4], uint8_t proto, uint8_t ttl,
                 const void *payload, uint16_t plen) {
    if (!g_cfg.configured || !net_present()) return false;
    uint8_t dmac[6];
    if (!ip_arp_resolve(dip, dmac, 500)) return false;
    uint8_t pkt[1500];
    if ((uint32_t)IP_HLEN + plen > sizeof(pkt)) return false;
    pkt[0] = 0x45; /* v4, ihl=5 */
    pkt[1] = 0;
    wr16(pkt + 2, (uint16_t)(IP_HLEN + plen));
    wr16(pkt + 4, ++g_ip_id); /* id — not always 0; some NATs drop reuse */
    wr16(pkt + 6, 0); /* flags/frag */
    pkt[8] = ttl ? ttl : 64;
    pkt[9] = proto;
    wr16(pkt + 10, 0);
    cpy4(pkt + 12, g_cfg.ip);
    cpy4(pkt + 16, dip);
    wr16(pkt + 10, ip_checksum(pkt, IP_HLEN));
    const uint8_t *pl = (const uint8_t *)payload;
    for (uint16_t i = 0; i < plen; i++) pkt[IP_HLEN + i] = pl[i];
    return eth_send(dmac, ETH_TYPE_IPV4, pkt, (uint16_t)(IP_HLEN + plen));
}

bool ip_send(const uint8_t dip[4], uint8_t proto,
             const void *payload, uint16_t plen) {
    return ip_send_ttl(dip, proto, 64, payload, plen);
}

bool ip_send_bcast(uint8_t proto, const void *payload, uint16_t plen) {
    static const uint8_t bmac[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    static const uint8_t bdip[4] = {255,255,255,255};
    uint8_t pkt[1500];
    const uint8_t *pl;
    uint16_t i;
    if (!net_present()) return false;
    if ((uint32_t)20 + plen > sizeof(pkt)) return false;
    pkt[0] = 0x45; pkt[1] = 0;
    wr16(pkt + 2, (uint16_t)(20 + plen));
    wr16(pkt + 4, ++g_ip_id); wr16(pkt + 6, 0);
    pkt[8] = 64; pkt[9] = proto; wr16(pkt + 10, 0);
    if (g_cfg.configured) cpy4(pkt + 12, g_cfg.ip);
    else { pkt[12]=0; pkt[13]=0; pkt[14]=0; pkt[15]=0; }
    cpy4(pkt + 16, bdip);
    wr16(pkt + 10, ip_checksum(pkt, 20));
    pl = (const uint8_t *)payload;
    for (i = 0; i < plen; i++) pkt[20 + i] = pl[i];
    return eth_send(bmac, ETH_TYPE_IPV4, pkt, (uint16_t)(20 + plen));
}

static void dhcp_opt(uint8_t *p, int *n, uint8_t t, uint8_t l, const uint8_t *v) {
    int i;
    p[(*n)++] = t; p[(*n)++] = l;
    for (i = 0; i < l; i++) p[(*n)++] = v[i];
}

static int dhcp_find(const uint8_t *p, int n, uint8_t t, uint8_t *out, int max) {
    int i = 240; /* after magic */
    if (n < 244) return 0;
    while (i < n) {
        uint8_t tag = p[i++];
        uint8_t len;
        if (tag == 0xff) break;
        if (tag == 0) continue;
        if (i >= n) break;
        len = p[i++];
        if (i + len > n) break;
        if (tag == t) {
            int c = len < max ? len : max;
            int k;
            for (k = 0; k < c; k++) out[k] = p[i + k];
            return c;
        }
        i += len;
    }
    return 0;
}

bool ip_dhcp(uint32_t timeout_ms) {
    uint8_t pkt[300], udp[8 + 300], rx[576];
    uint8_t sip[4], yi[4], mask[4], gw[4], dns[4], typ[4], sid[4];
    uint8_t xid[4];
    uint16_t sport;
    int n = 0, i;
    uint32_t xid32 = (uint32_t)mono_us() * 2654435761u;
    static const uint8_t magic[4] = {99, 130, 83, 99};
    static const uint8_t disc[1] = {1};
    static const uint8_t reqt[1] = {3};
    static const uint8_t params[3] = {1, 3, 6};

    for (i = 0; i < (int)sizeof(pkt); i++) pkt[i] = 0;
    pkt[0] = 1; pkt[1] = 1; pkt[2] = 6; pkt[3] = 0;
    xid[0] = (uint8_t)(xid32 >> 24); xid[1] = (uint8_t)(xid32 >> 16);
    xid[2] = (uint8_t)(xid32 >> 8);  xid[3] = (uint8_t)xid32;
    for (i = 0; i < 4; i++) pkt[4 + i] = xid[i];
    {
        const uint8_t *mac = net_mac();
        if (mac) for (i = 0; i < 6; i++) pkt[28 + i] = mac[i];
    }
    for (i = 0; i < 4; i++) pkt[236 + i] = magic[i];
    n = 240;
    dhcp_opt(pkt, &n, 53, 1, disc);
    dhcp_opt(pkt, &n, 55, 3, params);
    pkt[n++] = 0xff;

    wr16(udp + 0, 68); wr16(udp + 2, 67);
    wr16(udp + 4, (uint16_t)(8 + n)); wr16(udp + 6, 0);
    for (i = 0; i < n; i++) udp[8 + i] = pkt[i];
    if (!ip_send_bcast(17, udp, (uint16_t)(8 + n))) return false;

    if (!ip_udp_recv(68, sip, &sport, rx, sizeof(rx), timeout_ms)) return false;
    if (dhcp_find(rx, sizeof(rx), 53, typ, 1) < 1 || typ[0] != 2) return false;
    for (i = 0; i < 4; i++) yi[i] = rx[16 + i];
    if (dhcp_find(rx, sizeof(rx), 54, sid, 4) < 4) cpy4(sid, sip);
    if (dhcp_find(rx, sizeof(rx), 1, mask, 4) < 4) { mask[0]=255; mask[1]=255; mask[2]=255; mask[3]=0; }
    if (dhcp_find(rx, sizeof(rx), 3, gw, 4) < 4) cpy4(gw, sip);
    if (dhcp_find(rx, sizeof(rx), 6, dns, 4) < 4) cpy4(dns, gw);

    /* REQUEST */
    n = 240;
    dhcp_opt(pkt, &n, 53, 1, reqt);
    dhcp_opt(pkt, &n, 50, 4, yi);
    dhcp_opt(pkt, &n, 54, 4, sid);
    dhcp_opt(pkt, &n, 55, 3, params);
    pkt[n++] = 0xff;
    wr16(udp + 4, (uint16_t)(8 + n));
    for (i = 0; i < n; i++) udp[8 + i] = pkt[i];
    if (!ip_send_bcast(17, udp, (uint16_t)(8 + n))) return false;
    if (!ip_udp_recv(68, sip, &sport, rx, sizeof(rx), timeout_ms)) return false;
    if (dhcp_find(rx, sizeof(rx), 53, typ, 1) < 1 || typ[0] != 5) return false;
    ipstack_set_addr(yi, mask, gw, dns);
    return true;
}

/* Round trips on a local link are well under a millisecond, so the reply
   time is kept in microseconds: reporting a real 300 us as "0 ms" reads like
   a broken measurement. Callers that want milliseconds divide. */
static bool icmp_wait(uint8_t want_type, uint8_t also_type,
                      uint32_t timeout_ms, uint8_t sip[4], uint32_t *rtt_us) {
    g_icmp_rx.got = false;
    uint64_t start = mono_us();
    uint64_t budget = (uint64_t)timeout_ms * 1000u;
    while (mono_us() - start < budget) {
        ipstack_service();
        if (g_icmp_rx.got &&
            (g_icmp_rx.type == want_type || g_icmp_rx.type == also_type)) {
            if (sip) cpy4(sip, g_icmp_rx.sip);
            if (rtt_us) *rtt_us = (uint32_t)(mono_us() - start);
            return true;
        }
    }
    return false;
}

bool ip_icmp_ping_us(const uint8_t ip[4], uint32_t timeout_ms, uint32_t *rtt_us) {
    uint8_t icmp[8 + 16];
    icmp[0] = 8; icmp[1] = 0;
    wr16(icmp + 2, 0);
    wr16(icmp + 4, 0x0042);
    wr16(icmp + 6, 1);
    for (int i = 0; i < 16; i++) icmp[8 + i] = (uint8_t)('A' + i);
    wr16(icmp + 2, ip_checksum(icmp, sizeof(icmp)));
    g_icmp_rx.got = false;
    if (!ip_send(ip, 1, icmp, sizeof(icmp))) return false;
    uint8_t sip[4];
    return icmp_wait(0, 0, timeout_ms, sip, rtt_us);
}

bool ip_icmp_ping_rtt(const uint8_t ip[4], uint32_t timeout_ms, uint32_t *rtt_ms) {
    uint32_t us = 0;
    bool ok = ip_icmp_ping_us(ip, timeout_ms, &us);
    if (rtt_ms) *rtt_ms = us / 1000u;
    return ok;
}

bool ip_icmp_ping(const uint8_t ip[4], uint32_t timeout_ms) {
    return ip_icmp_ping_us(ip, timeout_ms, NULL);
}

int ip_traceroute(const uint8_t dst[4], ip_trace_hop_t *hops, int maxhops,
                  uint32_t per_hop_ms) {
    if (!hops || maxhops <= 0) return 0;
    if (maxhops > IP_TRACE_MAX) maxhops = IP_TRACE_MAX;
    int n = 0;
    for (int ttl = 1; ttl <= maxhops; ttl++) {
        uint8_t icmp[8 + 16];
        icmp[0] = 8; icmp[1] = 0;
        wr16(icmp + 2, 0);
        wr16(icmp + 4, 0x0042);
        wr16(icmp + 6, (uint16_t)ttl);
        for (int i = 0; i < 16; i++) icmp[8 + i] = (uint8_t)('A' + i);
        wr16(icmp + 2, ip_checksum(icmp, sizeof(icmp)));
        g_icmp_rx.got = false;
        hops[n].ttl = (uint8_t)ttl;
        hops[n].kind = 0;
        hops[n].rtt_ms = 0;
        hops[n].ip[0] = hops[n].ip[1] = hops[n].ip[2] = hops[n].ip[3] = 0;
        if (!ip_send_ttl(dst, 1, (uint8_t)ttl, icmp, sizeof(icmp))) {
            n++;
            continue;
        }
        uint8_t sip[4];
        uint32_t rtt = 0;
        if (icmp_wait(11, 0, per_hop_ms, sip, &rtt)) {
            cpy4(hops[n].ip, sip);
            hops[n].rtt_ms = rtt;
            hops[n].kind = (g_icmp_rx.type == 0) ? 2 : 1;
            n++;
            if (g_icmp_rx.type == 0) break;
        } else {
            n++;
        }
    }
    return n;
}

bool ip_udp_send(const uint8_t dip[4], uint16_t dport, uint16_t sport,
                 const void *data, uint16_t len) {
    uint8_t u[8 + 1400];
    if (len > 1400) return false;
    wr16(u + 0, sport);
    wr16(u + 2, dport);
    wr16(u + 4, (uint16_t)(8 + len));
    wr16(u + 6, 0); /* checksum optional for IPv4 */
    const uint8_t *p = (const uint8_t *)data;
    for (uint16_t i = 0; i < len; i++) u[8 + i] = p[i];
    return ip_send(dip, 17, u, (uint16_t)(8 + len));
}

void ip_udp_listen(uint16_t sport) {
    g_udp_rx.armed = true;
    g_udp_rx.port = sport;
    g_udp_rx.len = 0;
}

uint16_t ip_udp_recv(uint16_t sport, uint8_t sip[4], uint16_t *sport_out,
                     void *buf, uint16_t max, uint32_t timeout_ms) {
    /* Keep a packet that arrived after ip_udp_listen() + send. */
    if (!(g_udp_rx.armed && g_udp_rx.port == sport)) {
        g_udp_rx.armed = true;
        g_udp_rx.port = sport;
        g_udp_rx.len = 0;
    }
    uint64_t start = mono_us();
    uint64_t budget = (uint64_t)timeout_ms * 1000u;
    while (mono_us() - start < budget) {
        ipstack_service();
        if (g_udp_rx.len) {
            uint16_t n = g_udp_rx.len;
            if (n > max) n = max;
            uint8_t *d = (uint8_t *)buf;
            for (uint16_t i = 0; i < n; i++) d[i] = g_udp_rx.buf[i];
            if (sip) cpy4(sip, g_udp_rx.sip);
            if (sport_out) *sport_out = g_udp_rx.sport;
            g_udp_rx.armed = false;
            g_udp_rx.len = 0;
            return n;
        }
    }
    g_udp_rx.armed = false;
    return 0;
}

static void handle_ipv4(const uint8_t *ip, uint32_t len) {
    if (len < IP_HLEN) return;
    if ((ip[0] >> 4) != 4) return;
    uint8_t ihl = (uint8_t)((ip[0] & 0x0f) * 4);
    if (ihl < 20 || len < ihl) return;
    uint16_t total = rd16(ip + 2);
    if (total > len) total = (uint16_t)len;
    uint8_t proto = ip[9];
    const uint8_t *sip = ip + 12;
    const uint8_t *dip = ip + 16;
    /* Accept unicast to us or broadcast */
    if (g_cfg.configured && !eq4(dip, g_cfg.ip)) {
        if (!(dip[0]==255&&dip[1]==255&&dip[2]==255&&dip[3]==255))
            return;
    }
    const uint8_t *payload = ip + ihl;
    uint16_t plen = (uint16_t)(total - ihl);

    if (proto == 1 && plen >= 8) { /* ICMP */
        if (payload[0] == 8) { /* echo request → reply */
            uint8_t rep[1500];
            if (plen > sizeof(rep)) return;
            for (uint16_t i = 0; i < plen; i++) rep[i] = payload[i];
            rep[0] = 0; /* echo reply */
            wr16(rep + 2, 0);
            wr16(rep + 2, ip_checksum(rep, plen));
            ip_send(sip, 1, rep, plen);
        } else if (payload[0] == 0 || payload[0] == 11) {
            /* echo reply or time-exceeded (traceroute hop) */
            g_icmp_rx.got = true;
            g_icmp_rx.type = payload[0];
            cpy4(g_icmp_rx.sip, sip);
            g_icmp_rx.id = (payload[0] == 0) ? rd16(payload + 4) : 0;
        }
        return;
    }
    if (proto == 17 && plen >= 8) { /* UDP */
        uint16_t dport = rd16(payload + 2);
        uint16_t sport = rd16(payload + 0);
        uint16_t ulen  = rd16(payload + 4);
        if (ulen < 8 || ulen > plen) ulen = plen;
        uint16_t dlen = (uint16_t)(ulen - 8);
        if (g_udp_ring.bound && dport == g_udp_ring.port) {
            if (g_udp_ring.head - g_udp_ring.tail >= UDP_RING_SLOTS) {
                g_udp_ring.drops++;
            } else if (dlen <= UDP_RING_MTU) {
                uint32_t i = g_udp_ring.head % UDP_RING_SLOTS;
                cpy4(g_udp_ring.slot[i].sip, sip);
                g_udp_ring.slot[i].sport = sport;
                g_udp_ring.slot[i].len = dlen;
                for (uint16_t k = 0; k < dlen; k++)
                    g_udp_ring.slot[i].data[k] = payload[8 + k];
                g_udp_ring.head++;
                g_udp_ring.accepted++;
            } else {
                g_udp_ring.drops++;      /* too big for this ring, not lost
                                            silently */
            }
            return;
        }
        if (g_udp_rx.armed && dport == g_udp_rx.port && dlen <= sizeof(g_udp_rx.buf)) {
            cpy4(g_udp_rx.sip, sip);
            g_udp_rx.sport = sport;
            for (uint16_t i = 0; i < dlen; i++) g_udp_rx.buf[i] = payload[8 + i];
            g_udp_rx.len = dlen;
        }
        return;
    }
    if (proto == 6 && g_tcp_rx) {
        g_tcp_rx(sip, dip, payload, plen);
    }
}

static void handle_frame(const uint8_t *f, uint32_t n) {
    if (n < 14) return;
    uint16_t et = rd16(f + 12);
    if (et == ETH_TYPE_ARP && n >= 14 + 28) {
        const uint8_t *arp = f + 14;
        if (rd16(arp + 6) == 2) { /* reply */
            arp_cache_put(arp + 14, arp + 8);
        } else if (rd16(arp + 6) == 1 && g_cfg.configured) {
            arp_reply_to(arp);
        }
        return;
    }
    if (et == ETH_TYPE_IPV4) {
        handle_ipv4(f + 14, n - 14);
        return;
    }
    if (et == ETH_TYPE_RGXW && g_l2_rx) {
        g_l2_rx(f, n);
        return;
    }
}

void ipstack_service(void) {
    if (!net_present()) return;
    net_take_pending();
    uint8_t f[NET_FRAME_MAX];
    /* Bounded below UDP_RING_SLOTS: one service call must not be able to
       enqueue more datagrams than the ring can hold. */
    for (int guard = 0; guard < 32; guard++) {
        uint32_t n = net_poll(f, sizeof(f));
        if (!n) break;
        handle_frame(f, n);
    }
}

/* --- streaming UDP port (control links) ---------------------------------- */

void ip_udp_bind(uint16_t port) {
    g_udp_ring.bound = true;
    g_udp_ring.port = port;
    g_udp_ring.head = g_udp_ring.tail = 0;
    g_udp_ring.drops = 0;
}

void ip_udp_unbind(void) { g_udp_ring.bound = false; }

uint16_t ip_udp_poll(uint16_t port, uint8_t sip[4], uint16_t *sport_out,
                     void *buf, uint16_t max) {
    uint32_t i;
    uint16_t n;
    if (!g_udp_ring.bound || g_udp_ring.port != port) return 0;
    /* Refill only when there is nothing left to hand out. Servicing on every
       call drains the NIC into a ring that still holds unread datagrams, and
       whatever does not fit is dropped here — inside the receiver, not on
       the wire. That is where half of PX4's telemetry was going. */
    if (g_udp_ring.head == g_udp_ring.tail) ipstack_service();
    if (g_udp_ring.head == g_udp_ring.tail) return 0;
    i = g_udp_ring.tail % UDP_RING_SLOTS;
    n = g_udp_ring.slot[i].len;
    if (n > max) n = max;
    for (uint16_t k = 0; k < n; k++) ((uint8_t *)buf)[k] = g_udp_ring.slot[i].data[k];
    if (sip) cpy4(sip, g_udp_ring.slot[i].sip);
    if (sport_out) *sport_out = g_udp_ring.slot[i].sport;
    g_udp_ring.tail++;
    return n;
}

uint32_t ip_udp_ring_drops(void) { return g_udp_ring.drops; }
uint32_t ip_udp_ring_accepted(void) { return g_udp_ring.accepted; }
uint32_t ip_udp_ring_depth(void) { return g_udp_ring.head - g_udp_ring.tail; }
