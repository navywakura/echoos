/*
 * WWW mode — opt-in IPv4/DNS/TCP/HTTP(S) for package channel + www get.
 * Default OFF. HTTPS is TLS 1.3 AES-128-GCM (fallback 1.2 ECDHE-RSA-AES128-GCM)
 * + SNI (no CA pin yet).
 */
#include "www.h"
#include "ipstack.h"
#include "tcp.h"
#include "netif.h"
#include "arch.h"
#include "pkg.h"
#include "tls_min.h"
#include "version.h"

static bool g_on;
static char g_channel[128];
static char g_last_err[80];
static char g_last_url[196];
static char g_location[196];
static int  g_last_status;
static char g_http_raw[WWW_BODY_MAX + 1024];
static char g_extra_hdr[400];
static uint32_t g_read_ticks = 1500;

static void set_err(const char *s) {
    uint32_t i = 0;
    if (!s) { g_last_err[0] = '\0'; return; }
    while (s[i] && i + 1 < sizeof(g_last_err)) { g_last_err[i] = s[i]; i++; }
    g_last_err[i] = '\0';
}

static bool streq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
static void scopy(char *d, const char *s, uint32_t max) {
    uint32_t i = 0;
    if (!s) { if (max) d[0]='\0'; return; }
    while (s[i] && i + 1 < max) { d[i] = s[i]; i++; }
    d[i] = '\0';
}
static int hdr_ieq(const char *a, const char *b, int n);

void www_init(void) {
    g_on = false;
    scopy(g_channel, PKG_CHANNEL_URL, sizeof(g_channel));
    g_last_err[0] = '\0';
    ipstack_init();
    tcp_init();
}

bool www_enabled(void) { return g_on; }
bool www_ready(void) { return g_on && net_present() && ipstack_configured(); }
const char *www_last_err(void) { return g_last_err; }
int         www_last_status(void) { return g_last_status; }
const char *www_last_url(void) { return g_last_url[0] ? g_last_url : ""; }

static bool qemu_like_nic(void) {
    const char *n = net_driver_name();
    const uint8_t *m = net_mac();
    if (!n) return false;
    if (n[0]=='v' && n[1]=='i' && n[2]=='r') return true; /* virtio-net */
    if (n[0]=='e' && n[1]=='1' && n[2]=='0') return true; /* e1000 */
    if (m && m[0]==0x52 && m[1]==0x54 && m[2]==0x00) return true; /* QEMU OUI */
    return false;
}

bool www_enable(void) {
    if (!net_present()) net_init();
    if (!net_present()) {
        set_err("no NIC — nics lists PCI. HP: plug Ethernet (RTL8106E)");
        return false;
    }
    if (!ipstack_configured()) {
        if (!ip_dhcp(4000)) {
            if (qemu_like_nic())
                ipstack_use_qemu_defaults(); /* slirp 10.0.2.15/24 */
            else {
                set_err("dhcp failed — cable? or: www config ip mask gw dns");
                return false;
            }
        }
    }
    g_on = true;
    set_err(NULL);
    return true;
}

void www_disable(void) {
    g_on = false;
    tcp_close();
}

bool www_config(const char *ip, const char *mask, const char *gw, const char *dns) {
    uint8_t a[4], m[4], g[4], d[4];
    if (!ip_parse(ip, a) || !ip_parse(mask, m) || !ip_parse(gw, g)) return false;
    if (dns && dns[0]) { if (!ip_parse(dns, d)) return false; }
    else { d[0]=8; d[1]=8; d[2]=8; d[3]=8; }
    ipstack_set_addr(a, m, g, d);
    return true;
}

const char *www_channel_base(void) { return g_channel; }
void www_set_channel_base(const char *url) {
    if (url && url[0]) scopy(g_channel, url, sizeof(g_channel));
}

void www_clear_extra_hdr(void) { g_extra_hdr[0] = '\0'; }

void www_add_extra_hdr(const char *line) {
    uint32_t n = 0, i = 0;
    if (!line || !line[0]) return;
    while (g_extra_hdr[n]) n++;
    while (line[i] && line[i] != '\r' && line[i] != '\n' && n + 3 < sizeof(g_extra_hdr))
        g_extra_hdr[n++] = line[i++];
    if (n + 3 < sizeof(g_extra_hdr)) {
        g_extra_hdr[n++] = '\r';
        g_extra_hdr[n++] = '\n';
    }
    g_extra_hdr[n] = '\0';
}

void www_set_read_ms(uint32_t ms) {
    if (!ms) { g_read_ticks = 1500; return; }
    g_read_ticks = ms / 10;
    if (!g_read_ticks) g_read_ticks = 1;
}

const char *www_last_location(void) { return g_location; }

uint32_t www_last_headers(char *out, uint32_t max) {
    uint32_t i = 0, n = 0;
    if (!out || max < 2) return 0;
    while (g_http_raw[i] && n + 1 < max) {
        if (g_http_raw[i] == '\r' && g_http_raw[i + 1] == '\n' &&
            g_http_raw[i + 2] == '\r' && g_http_raw[i + 3] == '\n') {
            if (n + 2 < max) { out[n++] = '\r'; out[n++] = '\n'; }
            break;
        }
        out[n++] = g_http_raw[i++];
    }
    out[n] = '\0';
    return n;
}

uint32_t www_last_set_cookies(char *out, uint32_t max) {
    uint32_t i = 0, n = 0;
    if (!out || max < 2) return 0;
    out[0] = '\0';
    while (g_http_raw[i]) {
        if (g_http_raw[i] == '\r' && g_http_raw[i + 1] == '\n' &&
            g_http_raw[i + 2] == '\r' && g_http_raw[i + 3] == '\n')
            break;
        if ((g_http_raw[i] == '\n' || i == 0) &&
            hdr_ieq(g_http_raw + (i == 0 ? 0 : i + 1), "set-cookie:", 11)) {
            const char *p = g_http_raw + (i == 0 ? 0 : i + 1) + 11;
            while (*p == ' ' || *p == '\t') p++;
            if (n && n + 2 < max) { out[n++] = ';'; out[n++] = ' '; }
            while (*p && *p != '\r' && *p != '\n' && *p != ';' && n + 1 < max)
                out[n++] = *p++;
        }
        i++;
    }
    out[n] = '\0';
    return n;
}

void www_service(void) {
    /* Always demux L2 (Wired) via ipstack when a NIC is present. */
    if (net_present()) ipstack_service();
    if (g_on) tcp_service();
}

/* ---- DNS A query (RFC 1035, CNAME-aware, cache + fallback resolvers) ---- */
#define DNS_CACHE_N 16
static struct {
    char     host[80];
    uint8_t  ip[4];
    uint64_t tick;
    int      used;
} g_dns_cache[DNS_CACHE_N];
static uint16_t g_dns_sport = 53100;

static int dns_skip_name(const uint8_t *m, int len, int off) {
    int guard = 0;
    while (off < len && guard++ < 128) {
        uint8_t l = m[off];
        if (l == 0) return off + 1;
        if ((l & 0xc0) == 0xc0) {
            if (off + 1 >= len) return -1;
            return off + 2;
        }
        if (l & 0xc0) return -1;
        off += 1 + l;
    }
    return -1;
}

static int dns_read_name(const uint8_t *m, int len, int off, char *out, int omax) {
    int o = 0, guard = 0, jumped = 0, consumed = off;
    if (!out || omax < 2) return -1;
    out[0] = 0;
    while (off < len && guard++ < 128) {
        uint8_t l = m[off];
        if (l == 0) {
            if (!jumped) consumed = off + 1;
            out[o] = 0;
            return consumed;
        }
        if ((l & 0xc0) == 0xc0) {
            if (off + 1 >= len) return -1;
            if (!jumped) consumed = off + 2;
            off = ((l & 0x3f) << 8) | m[off + 1];
            jumped = 1;
            continue;
        }
        if (l & 0xc0) return -1;
        off++;
        if (o && o < omax - 1) out[o++] = '.';
        {
            int i;
            for (i = 0; i < l && off < len && o < omax - 1; i++)
                out[o++] = (char)m[off++];
        }
    }
    return -1;
}

static int dns_parse_a(const uint8_t *m, int len, uint16_t want_id, uint8_t ip[4],
                       char *cname, int cmax) {
    uint16_t id, flags, qd, an, ns, ar;
    int off, i, nrr;
    if (len < 12) return 0;
    id    = (uint16_t)((m[0] << 8) | m[1]);
    flags = (uint16_t)((m[2] << 8) | m[3]);
    qd    = (uint16_t)((m[4] << 8) | m[5]);
    an    = (uint16_t)((m[6] << 8) | m[7]);
    ns    = (uint16_t)((m[8] << 8) | m[9]);
    ar    = (uint16_t)((m[10] << 8) | m[11]);
    if (id != want_id) return -1;          /* stale / other query */
    if ((flags & 0x8000) == 0) return 0;
    if ((flags & 0x000f) != 0) return 0;   /* RCODE */
    if (cname && cmax) cname[0] = 0;
    off = 12;
    for (i = 0; i < qd; i++) {
        off = dns_skip_name(m, len, off);
        if (off < 0 || off + 4 > len) return 0;
        off += 4;
    }
    /* AN + NS + AR: Vercel/CDN often put the A in additional after a CNAME. */
    nrr = (int)an + (int)ns + (int)ar;
    for (i = 0; i < nrr; i++) {
        uint16_t typ, cls, rdlen;
        off = dns_skip_name(m, len, off);
        if (off < 0 || off + 10 > len) return 0;
        typ   = (uint16_t)((m[off] << 8) | m[off + 1]);
        cls   = (uint16_t)((m[off + 2] << 8) | m[off + 3]);
        rdlen = (uint16_t)((m[off + 8] << 8) | m[off + 9]);
        off += 10;
        if (off + rdlen > len) return 0;
        if (typ == 1 && cls == 1 && rdlen == 4) {
            ip[0]=m[off]; ip[1]=m[off+1]; ip[2]=m[off+2]; ip[3]=m[off+3];
            if (!(ip[0]==0 && ip[1]==0 && ip[2]==0 && ip[3]==0))
                return 1;
        }
        if (typ == 5 && cls == 1 && cname && cmax > 1 && !cname[0])
            dns_read_name(m, len, off, cname, cmax);
        off += rdlen;
    }
    return 0;
}

static void dns_cache_put(const char *host, const uint8_t ip[4]) {
    int i, slot = 0;
    uint64_t oldest = (uint64_t)-1;
    for (i = 0; i < DNS_CACHE_N; i++) {
        if (!g_dns_cache[i].used) { slot = i; break; }
        if (g_dns_cache[i].tick < oldest) { oldest = g_dns_cache[i].tick; slot = i; }
    }
    scopy(g_dns_cache[slot].host, host, sizeof(g_dns_cache[slot].host));
    g_dns_cache[slot].ip[0]=ip[0]; g_dns_cache[slot].ip[1]=ip[1];
    g_dns_cache[slot].ip[2]=ip[2]; g_dns_cache[slot].ip[3]=ip[3];
    g_dns_cache[slot].tick = arch_timer_ticks();
    g_dns_cache[slot].used = 1;
}

static int dns_cache_get(const char *host, uint8_t ip[4]) {
    int i;
    for (i = 0; i < DNS_CACHE_N; i++) {
        if (!g_dns_cache[i].used) continue;
        if (arch_timer_ticks() - g_dns_cache[i].tick > 6000) continue; /* ~60 s */
        if (streq(g_dns_cache[i].host, host)) {
            ip[0]=g_dns_cache[i].ip[0]; ip[1]=g_dns_cache[i].ip[1];
            ip[2]=g_dns_cache[i].ip[2]; ip[3]=g_dns_cache[i].ip[3];
            return 1;
        }
    }
    return 0;
}

static int dns_query_one(const uint8_t dns[4], const char *host,
                         uint8_t ip[4], uint32_t timeout_ms); /* hop-limited */

static int dns_query_one_hops(const uint8_t dns[4], const char *host,
                              uint8_t ip[4], uint32_t timeout_ms, int hops) {
    uint8_t q[512], resp[512], sip[4];
    uint16_t id, n = 12, sport, sp, rlen;
    const char *p = host;
    int pr;
    if (hops < 0 || !host || !host[0]) return 0;
    id = (uint16_t)(arch_timer_ticks() ^ (uint16_t)((dns[0]<<8) | dns[3]) ^ g_dns_sport);
    if (!id) id = 1;
    q[0] = (uint8_t)(id >> 8); q[1] = (uint8_t)id;
    q[2] = 0x01; q[3] = 0x00;
    q[4] = 0; q[5] = 1;
    q[6] = q[7] = q[8] = q[9] = q[10] = q[11] = 0;
    while (*p) {
        const char *dot = p;
        uint8_t lab;
        while (*dot && *dot != '.') dot++;
        lab = (uint8_t)(dot - p);
        if (lab == 0 || n + 1 + lab >= sizeof(q)) return 0;
        q[n++] = lab;
        { uint8_t i; for (i = 0; i < lab; i++) q[n++] = (uint8_t)p[i]; }
        p = *dot == '.' ? dot + 1 : dot;
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;
    q[n++] = 0; q[n++] = 1;

    g_dns_sport++;
    if (g_dns_sport < 53100) g_dns_sport = 53100;
    sport = g_dns_sport;

    ip_udp_listen(sport);
    if (!ip_udp_send(dns, 53, sport, q, n)) { set_err("dns send fail"); return 0; }
    rlen = ip_udp_recv(sport, sip, &sp, resp, sizeof(resp), timeout_ms);
    if (rlen < 12) { set_err("dns timeout"); return 0; }
    {
        char cname[80];
        cname[0] = 0;
        pr = dns_parse_a(resp, (int)rlen, id, ip, cname, (int)sizeof cname);
        if (pr == 1) return 1;
        if (pr < 0) { set_err("dns stale"); return 0; }
        /* Follow one CNAME hop (www.rogexlaboratories.com → Vercel). */
        if (cname[0] && hops > 0 && !streq(cname, host)) {
            if (dns_query_one_hops(dns, cname, ip, timeout_ms, hops - 1)) return 1;
        }
    }
    set_err("dns no A record");
    return 0;
}

static int dns_query_one(const uint8_t dns[4], const char *host,
                         uint8_t ip[4], uint32_t timeout_ms) {
    return dns_query_one_hops(dns, host, ip, timeout_ms, 2);
}

bool www_dns(const char *host, uint8_t ip[4], uint32_t timeout_ms) {
    uint8_t servers[4][4];
    int ns = 0, s, attempt;
    const ip_cfg_t *cfg;
    if (!www_ready() || !host || !host[0]) {
        if (!www_ready()) set_err("www off");
        return false;
    }
    if (ip_parse(host, ip)) return true;
    if (dns_cache_get(host, ip)) return true;

    cfg = ipstack_cfg();
    if (cfg && cfg->configured) {
        servers[ns][0]=cfg->dns[0]; servers[ns][1]=cfg->dns[1];
        servers[ns][2]=cfg->dns[2]; servers[ns][3]=cfg->dns[3];
        ns++;
    }
    /* QEMU slirp 10.0.2.3 is flaky on macOS — always also try public resolvers. */
    servers[ns][0]=8;  servers[ns][1]=8;  servers[ns][2]=8;  servers[ns][3]=8;  ns++;
    servers[ns][0]=1;  servers[ns][1]=1;  servers[ns][2]=1;  servers[ns][3]=1;  ns++;
    servers[ns][0]=10; servers[ns][1]=0;  servers[ns][2]=2;  servers[ns][3]=3;  ns++;

    if (!timeout_ms) timeout_ms = 2000;
    for (attempt = 0; attempt < 2; attempt++) {
        for (s = 0; s < ns; s++) {
            /* skip duplicate of configured DNS */
            if (s > 0 && servers[s][0]==servers[0][0] && servers[s][1]==servers[0][1] &&
                servers[s][2]==servers[0][2] && servers[s][3]==servers[0][3])
                continue;
            if (dns_query_one(servers[s], host, ip, timeout_ms)) {
                dns_cache_put(host, ip);
                set_err(NULL);
                return true;
            }
        }
    }
    if (!g_last_err[0]) set_err("dns no A record");
    return false;
}

/* Parse http(s)://host[:port]/path — or a bare host (https by default). */
static bool parse_url(const char *url, bool *https, char *host, uint32_t hmax,
                      uint16_t *port, char *path, uint32_t pmax) {
    *https = true;
    *port = 443;
    const char *u = url;
    if (!u || !u[0]) return false;
    if (u[0]=='h'&&u[1]=='t'&&u[2]=='t'&&u[3]=='p'&&u[4]=='s'&&u[5]==':'&&u[6]=='/'&&u[7]=='/') {
        *https = true; u += 8; *port = 443;
    } else if (u[0]=='h'&&u[1]=='t'&&u[2]=='t'&&u[3]=='p'&&u[4]==':'&&u[5]=='/'&&u[6]=='/') {
        *https = false; u += 7; *port = 80;
    } else if (u[0]=='/' && u[1]=='/') {
        *https = true; u += 2; *port = 443;
    }
    uint32_t hi = 0;
    while (*u && *u != '/' && *u != ':' && *u != '?' && *u != '#' && hi + 1 < hmax)
        host[hi++] = *u++;
    host[hi] = '\0';
    if (!hi) return false;
    if (*u == ':') {
        u++;
        int pr = 0;
        while (*u >= '0' && *u <= '9') { pr = pr * 10 + (*u - '0'); u++; }
        if (pr > 0 && pr < 65536) *port = (uint16_t)pr;
    }
    if (*u == '\0' || *u == '#') { path[0] = '/'; path[1] = '\0'; return true; }
    uint32_t pi = 0;
    while (*u && *u != '#' && pi + 1 < pmax) path[pi++] = *u++;
    path[pi] = '\0';
    if (!path[0]) { path[0] = '/'; path[1] = '\0'; }
    return true;
}

static void append_req(char *req, uint32_t *n, uint32_t max, const char *s) {
    while (*s && *n + 1 < max) req[(*n)++] = *s++;
}

static void put_dec_req(char *req, uint32_t *n, uint32_t max, uint32_t v) {
    char t[12];
    int i = 0;
    if (!v) t[i++] = '0';
    while (v && i < 11) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0 && *n + 1 < max) req[(*n)++] = t[--i];
}

static int hdr_ieq(const char *a, const char *b, int n) {
    int i;
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

static int headers_contain(const char *raw, uint32_t total, const char *needle) {
    uint32_t i, end = total, nlen = 0;
    if (!raw || !needle) return 0;
    while (needle[nlen]) nlen++;
    if (!nlen) return 0;
    for (i = 0; i + 3 < total; i++) {
        if (raw[i]=='\r' && raw[i+1]=='\n' && raw[i+2]=='\r' && raw[i+3]=='\n') {
            end = i; break;
        }
    }
    for (i = 0; i + nlen <= end; i++) {
        if (hdr_ieq(raw + i, needle, (int)nlen)) return 1;
    }
    return 0;
}

/* HTTP/1.1 chunked → flat body. In-place. Returns new length. */
static uint32_t decode_chunked(char *body, uint32_t n) {
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint32_t sz = 0, k;
        int hex = 0;
        while (i < n && body[i] != '\r' && body[i] != '\n') {
            char c = body[i++];
            if (c >= '0' && c <= '9') { sz = (sz << 4) | (uint32_t)(c - '0'); hex = 1; }
            else if (c >= 'a' && c <= 'f') { sz = (sz << 4) | (uint32_t)(c - 'a' + 10); hex = 1; }
            else if (c >= 'A' && c <= 'F') { sz = (sz << 4) | (uint32_t)(c - 'A' + 10); hex = 1; }
            else if (c == ';') { while (i < n && body[i] != '\r' && body[i] != '\n') i++; break; }
            else break;
        }
        if (i < n && body[i] == '\r') i++;
        if (i < n && body[i] == '\n') i++;
        if (!hex || !sz) break;
        if (i + sz > n) sz = n - i;
        for (k = 0; k < sz; k++) body[o + k] = body[i + k];
        o += sz;
        i += sz;
        if (i < n && body[i] == '\r') i++;
        if (i < n && body[i] == '\n') i++;
    }
    if (o < n) body[o] = '\0';
    return o;
}

static void grab_location(const char *raw, uint32_t total) {
    uint32_t i;
    g_location[0] = '\0';
    for (i = 0; i + 10 < total; i++) {
        if (raw[i] == '\n' && hdr_ieq(raw + i + 1, "location:", 9)) {
            const char *p = raw + i + 1 + 9;
            uint32_t n = 0;
            while (*p == ' ' || *p == '\t') p++;
            while (*p && *p != '\r' && *p != '\n' && n + 1 < sizeof(g_location))
                g_location[n++] = *p++;
            g_location[n] = '\0';
            return;
        }
        if (raw[i]=='\r' && raw[i+1]=='\n' && raw[i+2]=='\r' && raw[i+3]=='\n')
            return;
    }
}

static uint32_t http_transact_ex(const uint8_t ip[4], uint16_t port, const char *host,
                                 const char *path, const char *method,
                                 const char *body, uint32_t bodylen,
                                 const char *ctype,
                                 char *out, uint32_t outmax, int *status,
                                 int use_tls) {
    char req[1400];
    uint32_t n = 0, body_in_req = 0;
    const char *m = (method && method[0]) ? method : "GET";
    int is_head = (m[0]=='H' && m[1]=='E' && m[2]=='A' && m[3]=='D');
    if (status) *status = 0;
    g_last_status = 0;
    g_location[0] = '\0';
    if (!tcp_connect(ip, port, 5000)) { set_err("tcp connect fail"); return 0; }
    if (use_tls) {
        char why[80];
        if (!tls_handshake(host, why, (int)sizeof(why))) {
            tcp_close();
            set_err(why[0] ? why : "tls handshake fail");
            return 0;
        }
    }

    append_req(req, &n, sizeof(req), m);
    append_req(req, &n, sizeof(req), " ");
    append_req(req, &n, sizeof(req), path);
    append_req(req, &n, sizeof(req),
               " HTTP/1.1\r\nHost: ");
    append_req(req, &n, sizeof(req), host);
    append_req(req, &n, sizeof(req),
               "\r\nUser-Agent: EchOS/2.0 (echOS Universal)\r\n"
               "Accept: text/html, */*\r\n"
               "Accept-Encoding: identity\r\n"
               "Connection: close\r\n");
    if (g_extra_hdr[0]) append_req(req, &n, sizeof(req), g_extra_hdr);
    if (body && bodylen) {
        if (!ctype || !ctype[0]) ctype = "application/x-www-form-urlencoded";
        append_req(req, &n, sizeof(req), "Content-Type: ");
        append_req(req, &n, sizeof(req), ctype);
        append_req(req, &n, sizeof(req), "\r\nContent-Length: ");
        put_dec_req(req, &n, sizeof(req), bodylen);
        append_req(req, &n, sizeof(req), "\r\n");
    }
    append_req(req, &n, sizeof(req), "\r\n");
    if (body && bodylen) {
        uint32_t i;
        for (i = 0; i < bodylen && n + 1 < sizeof(req); i++) {
            req[n++] = body[i];
            body_in_req++;
        }
    }
    req[n] = '\0';
    if (use_tls) {
        if (!tls_write(req, (uint16_t)n)) {
            tls_close(); tcp_close(); set_err("tls send fail"); return 0;
        }
    } else if (!tcp_send(req, (uint16_t)n)) {
        tcp_close(); set_err("tcp send fail"); return 0;
    }
    /* leftover POST body if it did not fit in the first packet */
    if (body && body_in_req < bodylen) {
        uint32_t off = body_in_req;
        while (off < bodylen) {
            uint16_t chunk = (uint16_t)(bodylen - off);
            if (chunk > 1024) chunk = 1024;
            if (use_tls) { if (!tls_write(body + off, chunk)) break; }
            else if (!tcp_send(body + off, chunk)) break;
            off += chunk;
        }
    }

    {
        uint32_t total = 0;
        uint64_t start = arch_timer_ticks();
        uint32_t window = g_read_ticks ? g_read_ticks : 1500;
        while (arch_timer_ticks() - start < window && total + 1 < sizeof(g_http_raw)) {
            uint16_t room = (uint16_t)(sizeof(g_http_raw) - 1 - total);
            uint16_t got;
            if (room > 1400) room = 1400;
            if (use_tls) got = (uint16_t)tls_read(g_http_raw + total, room, 1000);
            else         got = tcp_recv(g_http_raw + total, room, 300);
            if (!got) {
                /* tls_ready() only tracks handshake state — the server closing
                   shows up on TCP. Without this the loop burns its whole
                   window after the body arrives. */
                if (!tcp_connected()) break;
                continue;
            }
            total += got;
            start = arch_timer_ticks();
        }
        if (use_tls) tls_close();
        tcp_close();
        g_http_raw[total] = '\0';

        if (total < 12) { set_err("empty response"); return 0; }
        int st = 0;
        if (g_http_raw[0]=='H' && g_http_raw[1]=='T' &&
            g_http_raw[2]=='T' && g_http_raw[3]=='P') {
            const char *sp = g_http_raw;
            while (*sp && *sp != ' ') sp++;
            if (*sp == ' ') {
                sp++;
                while (*sp >= '0' && *sp <= '9') { st = st * 10 + (*sp - '0'); sp++; }
            }
        }
        g_last_status = st;
        if (status) *status = st;
        grab_location(g_http_raw, total);

        uint32_t bodyoff = 0;
        for (uint32_t i = 0; i + 1 < total; i++) {
            if (i + 3 < total &&
                g_http_raw[i]=='\r'&&g_http_raw[i+1]=='\n'&&
                g_http_raw[i+2]=='\r'&&g_http_raw[i+3]=='\n') {
                bodyoff = i + 4; break;
            }
            if (g_http_raw[i]=='\n' && g_http_raw[i+1]=='\n') {
                bodyoff = i + 2; break;
            }
        }
        if (!bodyoff) {
            /* SPA / odd stacks: body starts at <!doctype or <html. */
            for (uint32_t i = 0; i + 5 < total; i++) {
                char c1 = g_http_raw[i], c2 = g_http_raw[i + 1];
                if (c1 == '<' && (c2 == '!' || c2 == 'h' || c2 == 'H')) {
                    bodyoff = i; break;
                }
            }
        }
        if (!bodyoff) {
            /* HEAD (and some 1xx/204) have headers only. */
            if (is_head || st == 204 || st == 304) {
                if (out && outmax) out[0] = '\0';
                if (st >= 200 && st < 300) set_err(NULL);
                else set_err("http non-2xx");
                return 0;
            }
            set_err("no http body");
            return 0;
        }
        uint32_t blen = total - bodyoff;
        if (blen + 1 > outmax) blen = outmax - 1;
        for (uint32_t i = 0; i < blen; i++) out[i] = g_http_raw[bodyoff + i];
        out[blen] = '\0';
        if (headers_contain(g_http_raw, total, "chunked")) {
            blen = decode_chunked(out, blen);
        }
        if (st >= 200 && st < 300) set_err(NULL);
        else if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308)
            set_err("http redirect");
        else set_err("http non-2xx");
        return blen;
    }
}

static uint32_t http_transact(const uint8_t ip[4], uint16_t port, const char *host,
                              const char *path, char *out, uint32_t outmax, int *status) {
    return http_transact_ex(ip, port, host, path, "GET", NULL, 0, NULL, out, outmax, status, 0);
}

static uint32_t http_once(const char *method, const char *url,
                          const char *body, uint32_t bodylen, const char *ctype,
                          char *out, uint32_t outmax, int *http_status) {
    bool https = false;
    char host[128], path[320];
    uint16_t port = 80;
    scopy(g_last_url, url, sizeof(g_last_url));
    if (!www_ready()) { set_err("www off"); return 0; }
    if (!parse_url(url, &https, host, sizeof(host), &port, path, sizeof(path))) {
        set_err("bad url"); return 0;
    }
    {
        uint8_t ip[4];
        if (!www_dns(host, ip, 3000)) return 0;
        return http_transact_ex(ip, port, host, path, method, body, bodylen, ctype,
                                out, outmax, http_status, https ? 1 : 0);
    }
}

static void join_location(const char *base, const char *loc, char *out, uint32_t omax) {
    bool hs;
    char host[128], path[320];
    uint16_t port;
    char *d, *e;
    const char *p;
    if (!loc || !loc[0]) { out[0] = '\0'; return; }
    if (parse_url(loc, &hs, host, sizeof(host), &port, path, sizeof(path))) {
        scopy(out, loc, omax);
        return;
    }
    if (loc[0] != '/') { scopy(out, loc, omax); return; }
    if (!parse_url(base, &hs, host, sizeof(host), &port, path, sizeof(path))) {
        scopy(out, loc, omax);
        return;
    }
    d = out;
    e = out + omax;
    p = hs ? "https://" : "http://";
    while (*p && d < e - 1) *d++ = *p++;
    for (p = host; *p && d < e - 1; p++) *d++ = *p;
    if (port && port != 80 && port != 443 && d + 6 < e) {
        char t[8];
        int i = 0;
        uint32_t v = port;
        *d++ = ':';
        if (!v) t[i++] = '0';
        while (v && i < 7) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
        while (i > 0 && d < e - 1) *d++ = t[--i];
    }
    for (p = loc; *p && d < e - 1; p++) *d++ = *p;
    *d = '\0';
}

uint32_t www_http_request_ex(const char *method, const char *url,
                             const char *body, uint32_t bodylen,
                             const char *content_type,
                             char *out, uint32_t outmax, int *http_status,
                             int follow) {
    char cur[196];
    int hop, maxhop = follow ? 4 : 1;
    scopy(cur, url, sizeof(cur));
    for (hop = 0; hop < maxhop; hop++) {
        int st = 0;
        uint32_t n = http_once(method, cur, body, bodylen, content_type,
                               out, outmax, &st);
        if (http_status) *http_status = st;
        if (!follow) return n;
        if (st != 301 && st != 302 && st != 303 && st != 307 && st != 308)
            return n;
        if (!g_location[0]) return n;
        {
            char nxt[196];
            join_location(cur, g_location, nxt, sizeof(nxt));
            if (!nxt[0]) return n;
            scopy(cur, nxt, sizeof(cur));
            /* POST becomes GET after 301/302/303 */
            if (st == 301 || st == 302 || st == 303) {
                method = "GET";
                body = NULL;
                bodylen = 0;
            }
        }
    }
    set_err("too many redirects");
    return 0;
}

uint32_t www_http_request(const char *method, const char *url,
                          const char *body, uint32_t bodylen,
                          const char *content_type,
                          char *out, uint32_t outmax, int *http_status) {
    return www_http_request_ex(method, url, body, bodylen, content_type,
                               out, outmax, http_status, 1);
}

uint32_t www_http_get(const char *url, char *out, uint32_t outmax, int *http_status) {
    return http_once("GET", url, NULL, 0, NULL, out, outmax, http_status);
}

uint32_t www_http_get_follow(const char *url, char *out, uint32_t outmax, int *http_status) {
    return www_http_request("GET", url, NULL, 0, NULL, out, outmax, http_status);
}

uint32_t www_http_post(const char *url, const char *body, const char *ctype,
                       char *out, uint32_t outmax, int *http_status) {
    uint32_t bl = 0;
    if (body) while (body[bl]) bl++;
    return www_http_request("POST", url, body, bl, ctype, out, outmax, http_status);
}

int www_url_encode(const char *in, char *out, int omax) {
    static const char *hex = "0123456789ABCDEF";
    int o = 0;
    if (!in || !out || omax < 2) return 0;
    while (*in && o + 1 < omax) {
        unsigned char c = (unsigned char)*in++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '+';
        } else {
            if (o + 3 >= omax) break;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
    return o;
}

int www_html_text(const char *in, int nin, char *out, int omax) {
    int i, o = 0, tag = 0, skip = 0;
    if (!in || !out || omax < 2) return 0;
    if (nin < 0) { nin = 0; while (in[nin]) nin++; }
    for (i = 0; i < nin && in[i] && o + 1 < omax; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '<') {
            if ((in[i+1]=='s'||in[i+1]=='S') &&
                ((in[i+2]=='c'||in[i+2]=='C') || (in[i+2]=='t'||in[i+2]=='T')))
                skip = 1;
            if (in[i+1]=='/' && (in[i+2]=='s'||in[i+2]=='S')) skip = 0;
            tag = 1;
            continue;
        }
        if (c == '>') { tag = 0; continue; }
        if (tag || skip) continue;
        if (c == '&') {
            if (in[i+1]=='a'&&in[i+2]=='m'&&in[i+3]=='p'&&in[i+4]==';') {
                i += 4; c = '&';
            } else if (in[i+1]=='l'&&in[i+2]=='t'&&in[i+3]==';') {
                i += 3; c = '<';
            } else if (in[i+1]=='g'&&in[i+2]=='t'&&in[i+3]==';') {
                i += 3; c = '>';
            } else if (in[i+1]=='n'&&in[i+2]=='b'&&in[i+3]=='s'&&in[i+4]=='p') {
                i += 4; if (in[i+1]==';') i++; c = ' ';
            } else if (in[i+1]=='q'&&in[i+2]=='u'&&in[i+3]=='o'&&in[i+4]=='t') {
                i += 4; if (in[i+1]==';') i++; c = '"';
            } else if (in[i+1]=='#') {
                int v = 0; i += 2;
                while (in[i] >= '0' && in[i] <= '9') { v = v * 10 + (in[i] - '0'); i++; }
                if (in[i] != ';') i--;
                c = (v > 0 && v < 127) ? (unsigned char)v : ' ';
            } else {
                while (i < nin && in[i] && in[i] != ';') i++;
                c = ' ';
            }
        }
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c < 32 || c > 126) continue;
        if (c == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return o;
}

static int copy_inner_text(const char *s, const char *end, char *out, int omax) {
    int o = 0, tag = 0;
    while (s < end && o + 1 < omax) {
        unsigned char c = (unsigned char)*s++;
        if (c == '<') { tag = 1; continue; }
        if (c == '>') { tag = 0; continue; }
        if (tag) continue;
        if (c == '&') {
            if (s[0]=='a'&&s[1]=='m'&&s[2]=='p'&&s[3]==';') { s += 4; c = '&'; }
            else if (s[0]=='l'&&s[1]=='t'&&s[2]==';') { s += 3; c = '<'; }
            else if (s[0]=='g'&&s[1]=='t'&&s[2]==';') { s += 3; c = '>'; }
            else if (s[0]=='a'&&s[1]=='m'&&s[2]=='p') { c = '&'; }
            else {
                while (s < end && *s && *s != ';') s++;
                if (s < end && *s == ';') s++;
                c = ' ';
            }
        }
        if (c < 32 || c > 126) continue;
        if (c == ' ' && (o == 0 || out[o - 1] == ' ')) continue;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return o;
}

int www_google_search(const char *query, char *out, int omax, int *http_status) {
    static char html[WWW_BODY_MAX];
    char qenc[180], url[256], form[220];
    char *d, *e;
    int st = 0, n = 0, hits = 0, i;
    if (!out || omax < 16) return 0;
    if (!query || !query[0]) {
        set_err("query vacia");
        return 0;
    }
    www_url_encode(query, qenc, (int)sizeof(qenc));
    /* GET first (Google still serves gbv=1 over HTTP/1.0). */
    d = url; e = url + sizeof(url);
    *d = 0;
    {
        const char *p = "http://www.google.com/search?gbv=1&hl=es&num=8&q=";
        while (*p && d < e - 1) *d++ = *p++;
        p = qenc;
        while (*p && d < e - 1) *d++ = *p++;
        *d = 0;
    }
    n = (int)www_http_request("GET", url, NULL, 0, NULL, html, sizeof(html), &st);
    if (!n) {
        /* POST fallback — same form the search box would send. */
        d = form; e = form + sizeof(form);
        *d = 0;
        {
            const char *p = "gbv=1&hl=es&q=";
            while (*p && d < e - 1) *d++ = *p++;
            p = qenc;
            while (*p && d < e - 1) *d++ = *p++;
            *d = 0;
        }
        n = (int)www_http_post("http://www.google.com/search", form,
                               "application/x-www-form-urlencoded",
                               html, sizeof(html), &st);
    }
    if (http_status) *http_status = st;
    d = out; e = out + omax;
    {
        const char *p = "G_news google \"";
        while (*p && d < e - 1) *d++ = *p++;
        p = query;
        i = 0;
        while (*p && i < 48 && d < e - 1) { *d++ = *p++; i++; }
        p = "\" HTTP ";
        while (*p && d < e - 1) *d++ = *p++;
    }
    {
        int v = st, k = 0; char t[8];
        if (!v) t[k++] = '0';
        while (v && k < 7) { t[k++] = (char)('0' + (v % 10)); v /= 10; }
        while (k > 0 && d < e - 1) *d++ = t[--k];
    }
    if (n) {
        const char *p = "  ";
        while (*p && d < e - 1) *d++ = *p++;
        {
            int v = n, k = 0; char t[12];
            if (!v) t[k++] = '0';
            while (v && k < 11) { t[k++] = (char)('0' + (v % 10)); v /= 10; }
            while (k > 0 && d < e - 1) *d++ = t[--k];
        }
        p = " B\n";
        while (*p && d < e - 1) *d++ = *p++;
    } else {
        const char *p = "  vacio. ";
        while (*p && d < e - 1) *d++ = *p++;
        p = g_last_err;
        while (*p && d < e - 1) *d++ = *p++;
        *d = 0;
        return (int)(d - out);
    }

    /* Pull <h3>...</h3> titles — that is the gbv=1 result list. */
    for (i = 0; i < n && hits < 6 && d + 20 < e; i++) {
        if ((html[i]=='<' ) &&
            (html[i+1]=='h'||html[i+1]=='H') &&
            html[i+2]=='3') {
            const char *gt = html + i;
            const char *lt;
            char title[96];
            int tn;
            while (*gt && *gt != '>') gt++;
            if (*gt != '>') continue;
            gt++;
            lt = gt;
            while (*lt && !(lt[0]=='<' && (lt[1]=='/' ) &&
                            (lt[2]=='h'||lt[2]=='H') && lt[3]=='3'))
                lt++;
            tn = copy_inner_text(gt, lt, title, (int)sizeof(title));
            if (tn < 4) continue;
            hits++;
            *d++ = '0' + (char)hits;
            *d++ = ')';
            *d++ = ' ';
            {
                int k;
                for (k = 0; title[k] && d < e - 2; k++) *d++ = title[k];
            }
            *d++ = '\n';
            *d = 0;
        }
    }
    if (!hits) {
        char plain[400];
        int pn = www_html_text(html, n, plain, (int)sizeof(plain));
        if (pn > 0) {
            int k = 0, skip = 0;
            /* skip the chrome "G o o g l e / Imagenes / ..." prefix if present */
            while (plain[k] && skip < 80) {
                if (plain[k]=='.' && plain[k+1]==' ' ) { k += 2; break; }
                k++; skip++;
            }
            if (skip >= 80) k = 0;
            while (plain[k] && d < e - 1) *d++ = plain[k++];
        } else {
            const char *p = "(html sin titulos extraibles)\n";
            while (*p && d < e - 1) *d++ = *p++;
        }
    }
    *d = 0;
    if (hits) set_err(NULL);
    return (int)(d - out);
}

static int hexn(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static char fold_latin(int cp) {
    if (cp < 32) return ' ';
    if (cp < 127) return (char)cp;
    if (cp == 0xA0) return ' ';
    if (cp == 0xE1 || cp == 0xC1) return 'a';
    if (cp == 0xE9 || cp == 0xC9) return 'e';
    if (cp == 0xED || cp == 0xCD) return 'i';
    if (cp == 0xF3 || cp == 0xD3) return 'o';
    if (cp == 0xFA || cp == 0xDA || cp == 0xFC || cp == 0xDC) return 'u';
    if (cp == 0xF1 || cp == 0xD1) return 'n';
    if (cp == 0xE7 || cp == 0xC7) return 'c';
    return 0;
}

static int put_ascii(char *out, int o, int omax, unsigned char c) {
    if (c < 32 || c > 126) return o;
    if (c == ' ' && (o == 0 || out[o - 1] == ' ')) return o;
    if (o + 1 < omax) out[o++] = (char)c;
    return o;
}

static int json_get_str(const char *src, const char *key, char *out, int omax) {
    const char *p;
    int klen = 0;
    if (!src || !key || !out || omax < 2) return 0;
    while (key[klen]) klen++;
    p = src;
    while (*p) {
        if (*p == '"') {
            int i = 0;
            const char *q = p + 1;
            while (i < klen && q[i] && q[i] == key[i]) i++;
            if (i == klen && q[i] == '"') {
                q += i + 1;
                while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
                if (*q == ':') {
                    q++;
                    while (*q == ' ' || *q == '\t') q++;
                    if (*q == '"') {
                        int o = 0;
                        q++;
                        while (*q && o + 1 < omax) {
                            unsigned char c = (unsigned char)*q++;
                            if (c == '"') break;
                            if (c == '\\') {
                                unsigned char e = (unsigned char)*q;
                                if (!e) break;
                                q++;
                                if (e == 'n' || e == 'r' || e == 't') {
                                    o = put_ascii(out, o, omax, ' ');
                                    continue;
                                }
                                if (e == 'u' && q[0] && q[1] && q[2] && q[3]) {
                                    int cp = (hexn(q[0]) << 12) | (hexn(q[1]) << 8) |
                                             (hexn(q[2]) << 4) | hexn(q[3]);
                                    char f = fold_latin(cp);
                                    q += 4;
                                    if (f) o = put_ascii(out, o, omax, (unsigned char)f);
                                    continue;
                                }
                                o = put_ascii(out, o, omax, e);
                                continue;
                            }
                            if ((c & 0xE0) == 0xC0 && (*q)) {
                                int cp = ((c & 0x1F) << 6) | ((unsigned char)*q & 0x3F);
                                char f = fold_latin(cp);
                                q++;
                                if (f) o = put_ascii(out, o, omax, (unsigned char)f);
                                continue;
                            }
                            if ((c & 0xF0) == 0xE0 && q[0] && q[1]) {
                                q += 2; /* skip BMP utf-8 */
                                continue;
                            }
                            o = put_ascii(out, o, omax, c);
                        }
                        out[o] = 0;
                        return o;
                    }
                }
            }
        }
        p++;
    }
    out[0] = 0;
    return 0;
}

static void slug_path(const char *title, char *out, int omax) {
    int o = 0;
    const char *p = title;
    while (*p && o + 4 < omax) {
        unsigned char c = (unsigned char)*p++;
        if (c == ' ') {
            out[o++] = '_';
            continue;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out[o++] = (char)c;
            continue;
        }
        out[o++] = '%';
        out[o++] = "0123456789ABCDEF"[c >> 4];
        out[o++] = "0123456789ABCDEF"[c & 15];
    }
    out[o] = 0;
}

int www_wiki_summary(const char *title, char *out, int omax, int *http_status) {
    static char body[WWW_BODY_MAX];
    char slug[160], url[256], *d, *e;
    int st = 0, n, elen;
    const char *hosts[2] = {
        "https://es.wikipedia.org/api/rest_v1/page/summary/",
        "https://en.wikipedia.org/api/rest_v1/page/summary/",
    };
    int h;
    if (!out || omax < 8 || !title || !title[0]) return 0;
    slug_path(title, slug, (int)sizeof slug);
    for (h = 0; h < 2; h++) {
        d = url; e = url + sizeof(url);
        *d = 0;
        {
            const char *p = hosts[h];
            while (*p && d < e - 1) *d++ = *p++;
            p = slug;
            while (*p && d < e - 1) *d++ = *p++;
            *d = 0;
        }
        n = (int)www_http_get_follow(url, body, sizeof body, &st);
        if (http_status) *http_status = st;
        if (n <= 0 || st != 200) continue;
        elen = json_get_str(body, "extract", out, omax);
        if (elen >= 24) {
            set_err(NULL);
            return elen;
        }
    }
    return 0;
}

int www_ddg_search(const char *query, char *out, int omax, int *http_status) {
    static char body[WWW_BODY_MAX];
    char qenc[180], url[320], *d, *e;
    int st = 0, n, alen, i, hits = 0;
    if (!out || omax < 8 || !query || !query[0]) return 0;
    www_url_encode(query, qenc, (int)sizeof qenc);
    d = url; e = url + sizeof(url);
    {
        const char *p = "https://api.duckduckgo.com/?q=";
        while (*p && d < e - 1) *d++ = *p++;
        p = qenc;
        while (*p && d < e - 1) *d++ = *p++;
        p = "&format=json&no_html=1&skip_disambig=1";
        while (*p && d < e - 1) *d++ = *p++;
        *d = 0;
    }
    n = (int)www_http_get_follow(url, body, sizeof body, &st);
    if (http_status) *http_status = st;
    d = out; e = out + omax;
    *d = 0;
    if (n > 0 && st == 200) {
        char abs[360];
        alen = json_get_str(body, "AbstractText", abs, (int)sizeof abs);
        if (!alen) alen = json_get_str(body, "Abstract", abs, (int)sizeof abs);
        if (alen >= 20) {
            const char *p = abs;
            while (*p && d < e - 1) *d++ = *p++;
            *d = 0;
            set_err(NULL);
            return (int)(d - out);
        }
    }
    /* lite HTML titles */
    d = url; e = url + sizeof(url);
    {
        const char *p = "https://lite.duckduckgo.com/lite/?q=";
        while (*p && d < e - 1) *d++ = *p++;
        p = qenc;
        while (*p && d < e - 1) *d++ = *p++;
        *d = 0;
    }
    n = (int)www_http_get_follow(url, body, sizeof body, &st);
    if (http_status) *http_status = st;
    d = out; e = out + omax;
    *d = 0;
    if (n <= 0) return 0;
    for (i = 0; i < n && hits < 5 && d + 24 < e; i++) {
        if (body[i]=='<' && (body[i+1]=='a'||body[i+1]=='A') && body[i+2]==' ') {
            const char *gt = body + i;
            const char *lt;
            char title[96];
            int tn;
            while (*gt && *gt != '>') gt++;
            if (*gt != '>') continue;
            gt++;
            lt = gt;
            while (*lt && !(lt[0]=='<' && (lt[1]=='/' ) && (lt[2]=='a'||lt[2]=='A')))
                lt++;
            tn = copy_inner_text(gt, lt, title, (int)sizeof title);
            if (tn < 8) continue;
            if ((title[0]=='D'||title[0]=='d') && title[1]=='u') continue;
            hits++;
            if (hits > 1) *d++ = ' ';
            {
                int k;
                for (k = 0; title[k] && d < e - 2; k++) *d++ = title[k];
            }
            *d++ = '.';
        }
    }
    *d = 0;
    if (hits) { set_err(NULL); return (int)(d - out); }
    return 0;
}

int www_web_search(const char *query, char *out, int omax, int *http_status) {
    char *d, *e;
    int n;
    if (!out || omax < 16) return 0;
    n = www_wiki_summary(query, out, omax, http_status);
    if (n > 0) return n;
    n = www_ddg_search(query, out, omax, http_status);
    if (n > 0) return n;
    n = www_google_search(query, out, omax, http_status);
    if (n > 0) {
        /* already formatted */
        return n;
    }
    d = out; e = out + omax;
    {
        const char *p = "Sin extracto para «";
        while (*p && d < e - 1) *d++ = *p++;
        p = query ? query : "";
        while (*p && d < e - 1) *d++ = *p++;
        p = "». ";
        while (*p && d < e - 1) *d++ = *p++;
        p = g_last_err[0] ? g_last_err : "harvest vacio";
        while (*p && d < e - 1) *d++ = *p++;
    }
    *d = 0;
    return (int)(d - out);
}

uint32_t www_channel_get(const char *path, char *out, uint32_t outmax, bool *live) {
    if (live) *live = false;
    if (!path || !path[0]) return 0;

    char url[256];
    const char *base = g_channel;
    /* strip trailing slash on base */
    uint32_t bl = 0; while (base[bl]) bl++;
    while (bl && base[bl-1] == '/') bl--;
    uint32_t n = 0;
    for (uint32_t i = 0; i < bl && n + 1 < sizeof(url); i++) url[n++] = base[i];
    if (n + 1 < sizeof(url)) url[n++] = '/';
    if (path[0] == '/') path++;
    while (*path && n + 1 < sizeof(url)) url[n++] = *path++;
    url[n] = '\0';

    if (!www_ready()) { set_err("www off"); return 0; }

    bool https = false;
    char host[128], pth[256];
    uint16_t port = 80;
    if (!parse_url(url, &https, host, sizeof(host), &port, pth, sizeof(pth))) {
        set_err("bad channel url"); return 0;
    }

    uint8_t ip[4];
    if (!www_dns(host, ip, 3000)) return 0;

    if (https) {
        int st = 0;
        uint32_t len = http_transact_ex(ip, 443, host, pth, "GET", NULL, 0, NULL,
                                        out, outmax, &st, 1);
        if (len && st >= 200 && st < 300) {
            if (live) *live = true;
            return len;
        }
        return 0;
    }

    int st = 0;
    uint32_t len = http_transact(ip, port, host, pth, out, outmax, &st);
    if (len && st >= 200 && st < 300) {
        if (live) *live = true;
        return len;
    }
    return 0;
}

void www_status_text(char *out, uint32_t outmax) {
    char *d = out; char *e = out + outmax;
    #define PUT(s) do { const char *_s=(s); while(*_s && d<e-1) *d++=*_s++; } while(0)
    #define PUTNL() PUT("\n")
    PUT(ECHOS_VERSION_NAME " — WWW mode\n \n");
    PUT("  state:   "); PUT(g_on ? "ON (opt-in active)" : "OFF (default)"); PUTNL();
    PUT("  nic:     "); PUT(net_present() ? net_driver_name() : "none"); PUTNL();
    if (ipstack_configured()) {
        char b[20];
        const ip_cfg_t *c = ipstack_cfg();
        PUT("  ip:      "); ip_fmt(c->ip, b, sizeof(b)); PUT(b); PUTNL();
        PUT("  gateway: "); ip_fmt(c->gw, b, sizeof(b)); PUT(b); PUTNL();
        PUT("  dns:     "); ip_fmt(c->dns, b, sizeof(b)); PUT(b); PUTNL();
    } else PUT("  ip:      (not configured)\n");
    PUT("  channel: "); PUT(g_channel); PUTNL();
    if (g_last_err[0]) { PUT("  last:    "); PUT(g_last_err); PUTNL(); }
    PUT(" \n");
    PUT("  www on | off | status | config <ip> <mask> <gw> [dns]\n");
    PUT("  www get <http-url>    HTTP/1.0 GET (sigue Location http)\n");
    PUT("  www post <url> [body] HTTP/1.0 POST\n");
    PUT("  curl <url> | curl search <q> | curl post <url> <body>\n");
    PUT("  www channel [url]     set package base (default HTTPS channel)\n");
    PUT("  www dns <host>\n");
    PUT("  Packages: enable www, then `rx app add <name>` tries live channel.\n");
    PUT("  HTTP GET/POST live. HTTPS = TLS 1.3 AES-128-GCM (fallback TLS 1.2).\n");
    PUT("  curl -I -L -O -o -d -X -b -c -dom.  wdl  ipconf  man curl\n");
    PUT("  Google search sigue siendo HTTP gbv=1; curl https:// tambien funciona.\n");
    *d = '\0';
    #undef PUT
    #undef PUTNL
}
