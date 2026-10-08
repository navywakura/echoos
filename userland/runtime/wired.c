/*
 * Rogex Wired: raw Ethernet EtherType 0x88B5.
 * PING/PONG by pseudonym + CHAT (no IP/DNS). Real L2 frames on the wire.
 */
#include "wired.h"
#include "netif.h"
#include "ipstack.h"
#include "arch.h"

#define WT_PING 1
#define WT_PONG 2
#define WT_CHAT 3
#define WT_ACK  4
#define OFF_MAGIC 14
#define OFF_VER   18
#define OFF_TYPE  19
#define OFF_FROM  20
#define OFF_TARG  52
#define OFF_MSG   84
#define FRAME_PING 84
#define FRAME_CHAT (OFF_MSG + WIRED_CHAT_MAX)

static char    g_alias[WIRED_ALIAS_MAX + 1];
static char    g_replies[WIRED_MAX_PEERS][WIRED_ALIAS_MAX + 1];
static uint32_t g_reply_rtt[WIRED_MAX_PEERS];
static int     g_nreplies;
static bool    g_up;
static uint64_t g_ping_t0;
static bool     g_chat_acked;
static char     g_chat_ack_from[WIRED_ALIAS_MAX + 1];

/* Peer MAC cache from PONGs */
static struct {
    char    alias[WIRED_ALIAS_MAX + 1];
    uint8_t mac[6];
    bool    used;
} g_peers[WIRED_MAX_PEERS];

static struct {
    char from[WIRED_ALIAS_MAX + 1];
    char text[WIRED_CHAT_MAX + 1];
} g_chat[WIRED_CHAT_LOG];
static int g_chat_n, g_chat_i;

static bool aeq(const char *a, const char *b) {
    for (int i = 0; i < WIRED_ALIAS_MAX; i++) {
        if (a[i] != b[i]) return false;
        if (!a[i]) return true;
    }
    return true;
}
static void acopy(char *d, const char *s) {
    int i = 0;
    for (; i < WIRED_ALIAS_MAX && s && s[i]; i++) d[i] = s[i];
    for (; i < WIRED_ALIAS_MAX; i++) d[i] = '\0';
}
static void mcopy(uint8_t *d, const uint8_t *s) {
    for (int i = 0; i < 6; i++) d[i] = s[i];
}

static void peer_put(const char *alias, const uint8_t mac[6]) {
    for (int i = 0; i < WIRED_MAX_PEERS; i++) {
        if (g_peers[i].used && aeq(g_peers[i].alias, alias)) {
            mcopy(g_peers[i].mac, mac); return;
        }
    }
    for (int i = 0; i < WIRED_MAX_PEERS; i++) {
        if (!g_peers[i].used) {
            g_peers[i].used = true;
            acopy(g_peers[i].alias, alias);
            g_peers[i].alias[WIRED_ALIAS_MAX] = '\0';
            mcopy(g_peers[i].mac, mac);
            return;
        }
    }
}
static bool peer_mac(const char *alias, uint8_t mac[6]) {
    for (int i = 0; i < WIRED_MAX_PEERS; i++)
        if (g_peers[i].used && aeq(g_peers[i].alias, alias)) {
            mcopy(mac, g_peers[i].mac); return true;
        }
    return false;
}

void wired_init(const char *my) {
    acopy(g_alias, my);
    g_alias[WIRED_ALIAS_MAX] = '\0';
    g_nreplies = 0;
    g_up = net_present();
    for (int i = 0; i < WIRED_MAX_PEERS; i++) g_peers[i].used = false;
    g_chat_n = g_chat_i = 0;
    g_chat_acked = false;
    g_chat_ack_from[0] = '\0';
    g_ping_t0 = 0;
}
bool wired_up(void) { return g_up && net_present(); }
const char *wired_my_alias(void) { return g_alias; }

void wired_mac_str(char *out, uint32_t max) {
    static const char H[] = "0123456789abcdef";
    const uint8_t *m = net_mac();
    uint32_t n = 0;
    if (!m) { if (max) out[0]='\0'; return; }
    for (int i = 0; i < NET_MAC_LEN && n + 3 < max; i++) {
        out[n++] = H[m[i] >> 4]; out[n++] = H[m[i] & 0xf];
        if (i < NET_MAC_LEN - 1 && n < max - 1) out[n++] = ':';
    }
    out[n] = '\0';
}

static void build_hdr(uint8_t *f, const uint8_t *dst, uint8_t type,
                      const char *from, const char *target) {
    for (int i = 0; i < 6; i++) f[i] = dst[i];
    const uint8_t *src = net_mac();
    for (int i = 0; i < 6; i++) f[6 + i] = src ? src[i] : 0;
    f[12] = (WIRED_ETHERTYPE >> 8) & 0xff;
    f[13] = WIRED_ETHERTYPE & 0xff;
    f[OFF_MAGIC] = 'R'; f[OFF_MAGIC+1] = 'G'; f[OFF_MAGIC+2] = 'X'; f[OFF_MAGIC+3] = 'W';
    f[OFF_VER] = 1;
    f[OFF_TYPE] = type;
    acopy((char *)&f[OFF_FROM], from);
    acopy((char *)&f[OFF_TARG], target ? target : "");
}

bool wired_send_ping(const char *target) {
    if (!wired_up()) return false;
    static const uint8_t BCAST[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
    uint8_t f[FRAME_PING];
    build_hdr(f, BCAST, WT_PING, g_alias, target);
    g_ping_t0 = arch_timer_ticks();
    return net_send(f, FRAME_PING);
}

bool wired_send_chat(const char *target_alias, const char *msg) {
    if (!wired_up() || !msg || !msg[0]) return false;
    uint8_t dst[6];
    static const uint8_t BCAST[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
    if (target_alias && target_alias[0] && peer_mac(target_alias, dst)) {
        /* unicast */
    } else {
        for (int i = 0; i < 6; i++) dst[i] = BCAST[i];
    }
    uint8_t f[FRAME_CHAT];
    build_hdr(f, dst, WT_CHAT, g_alias, target_alias ? target_alias : "");
    int i = 0;
    for (; msg[i] && i < WIRED_CHAT_MAX; i++) f[OFF_MSG + i] = (uint8_t)msg[i];
    for (; i < WIRED_CHAT_MAX; i++) f[OFF_MSG + i] = 0;
    uint16_t len = (uint16_t)(OFF_MSG + WIRED_CHAT_MAX);
    if (len < 60) len = 60;
    g_chat_acked = false;
    g_chat_ack_from[0] = '\0';
    return net_send(f, len);
}

static void add_reply(const char *alias) {
    for (int i = 0; i < g_nreplies; i++) if (aeq(g_replies[i], alias)) return;
    if (g_nreplies < WIRED_MAX_PEERS) {
        acopy(g_replies[g_nreplies], alias);
        g_replies[g_nreplies][WIRED_ALIAS_MAX] = '\0';
        {
            uint64_t dt = arch_timer_ticks() - g_ping_t0;
            g_reply_rtt[g_nreplies] = (uint32_t)(dt * 10);
        }
        g_nreplies++;
    }
}
void wired_clear_replies(void) { g_nreplies = 0; }
int  wired_reply_count(void) { return g_nreplies; }
const char *wired_reply_alias(int i) {
    return (i >= 0 && i < g_nreplies) ? g_replies[i] : "";
}
uint32_t wired_reply_rtt_ms(int i) {
    return (i >= 0 && i < g_nreplies) ? g_reply_rtt[i] : 0;
}
bool wired_chat_acked(void) { return g_chat_acked; }
const char *wired_chat_ack_from(void) { return g_chat_ack_from; }

static void chat_push(const char *from, const char *text) {
    int slot = g_chat_i % WIRED_CHAT_LOG;
    acopy(g_chat[slot].from, from);
    g_chat[slot].from[WIRED_ALIAS_MAX] = '\0';
    int i = 0;
    for (; text && text[i] && i < WIRED_CHAT_MAX; i++) g_chat[slot].text[i] = text[i];
    g_chat[slot].text[i] = '\0';
    g_chat_i++;
    if (g_chat_n < WIRED_CHAT_LOG) g_chat_n++;
}
int wired_chat_count(void) { return g_chat_n; }
const char *wired_chat_from(int i) {
    if (i < 0 || i >= g_chat_n) return "";
    int base = (g_chat_i - g_chat_n + WIRED_CHAT_LOG) % WIRED_CHAT_LOG;
    return g_chat[(base + i) % WIRED_CHAT_LOG].from;
}
const char *wired_chat_text(int i) {
    if (i < 0 || i >= g_chat_n) return "";
    int base = (g_chat_i - g_chat_n + WIRED_CHAT_LOG) % WIRED_CHAT_LOG;
    return g_chat[(base + i) % WIRED_CHAT_LOG].text;
}
void wired_chat_clear(void) { g_chat_n = g_chat_i = 0; }

static void handle_frame(const uint8_t *f, uint32_t n) {
    if (n < FRAME_PING) return;
    if (!(f[12] == ((WIRED_ETHERTYPE >> 8) & 0xff) && f[13] == (WIRED_ETHERTYPE & 0xff))) return;
    if (!(f[OFF_MAGIC]=='R'&&f[OFF_MAGIC+1]=='G'&&f[OFF_MAGIC+2]=='X'&&f[OFF_MAGIC+3]=='W')) return;
    if (f[OFF_VER] != 1) return;

    char from[WIRED_ALIAS_MAX + 1], targ[WIRED_ALIAS_MAX + 1];
    k_memcpy(from, &f[OFF_FROM], WIRED_ALIAS_MAX); from[WIRED_ALIAS_MAX] = '\0';
    k_memcpy(targ, &f[OFF_TARG], WIRED_ALIAS_MAX); targ[WIRED_ALIAS_MAX] = '\0';

    if (f[OFF_TYPE] == WT_PING) {
        if (aeq(targ, g_alias)) {
            uint8_t reply[FRAME_PING];
            build_hdr(reply, &f[6], WT_PONG, g_alias, from);
            net_send(reply, FRAME_PING);
            peer_put(from, &f[6]);
        }
    } else if (f[OFF_TYPE] == WT_PONG) {
        if (aeq(targ, g_alias)) {
            add_reply(from);
            peer_put(from, &f[6]);
        }
    } else if (f[OFF_TYPE] == WT_CHAT && n >= OFF_MSG) {
        /* Deliver if broadcast (empty target) or addressed to us. */
        if (!targ[0] || aeq(targ, g_alias)) {
            char msg[WIRED_CHAT_MAX + 1];
            uint32_t mlen = n - OFF_MSG;
            if (mlen > WIRED_CHAT_MAX) mlen = WIRED_CHAT_MAX;
            for (uint32_t i = 0; i < mlen; i++) {
                char c = (char)f[OFF_MSG + i];
                if (!c) { mlen = i; break; }
                msg[i] = c;
            }
            msg[mlen] = '\0';
            chat_push(from, msg);
            peer_put(from, &f[6]);
            /* Confirm delivery so the sender can show ACK. */
            uint8_t ack[FRAME_PING];
            build_hdr(ack, &f[6], WT_ACK, g_alias, from);
            net_send(ack, FRAME_PING);
        }
    } else if (f[OFF_TYPE] == WT_ACK) {
        if (aeq(targ, g_alias)) {
            g_chat_acked = true;
            acopy(g_chat_ack_from, from);
            g_chat_ack_from[WIRED_ALIAS_MAX] = '\0';
            peer_put(from, &f[6]);
        }
    }
}

static void (*g_demux_svc)(void);

void wired_service(void) {
    if (!wired_up()) return;
    if (g_demux_svc) {
        /* Frames demuxed by ipstack_service (www idle path). */
        g_demux_svc();
        return;
    }
    net_take_pending();
    uint8_t f[NET_FRAME_MAX];
    for (int guard = 0; guard < 64; guard++) {
        uint32_t n = net_poll(f, sizeof(f));
        if (n == 0) break;
        handle_frame(f, n);
    }
}

static void l2_rx(const uint8_t *frame, uint32_t len) {
    handle_frame(frame, len);
}

void wired_attach_ipstack(void) {
    g_demux_svc = ipstack_service;
    ip_set_l2_rx(l2_rx);
}
