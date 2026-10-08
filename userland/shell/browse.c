/*
 * Wired browser — fetches HTML over HTTP and TLS 1.2, renders a CSS subset
 * (color, font-size, weight, align, headings, links) into styled lines for
 * the Nova GUI. JS engines (React/Vite SPAs) are staged — see docs.
 */
#include "browse.h"
#include "arch.h"
#include "shell.h"
#include "www.h"
#include "ipstack.h"
#include "wired.h"
#include "netif.h"
#include "vfs.h"
#include "arch.h"
#include "tcp.h"

static char g_page[WWW_BODY_MAX];
static char g_trace[2048];
static char g_url[160];
static char *o_buf;
static size_t o_max, o_len;

static void o_reset(char *buf, size_t max) {
    o_buf = buf; o_max = max; o_len = 0;
    if (max) buf[0] = '\0';
}
static void o_str(const char *s) {
    if (!s) return;
    while (*s && o_len + 1 < o_max) o_buf[o_len++] = *s++;
    if (o_max) o_buf[o_len] = '\0';
}
static void o_nl(void) { o_str("\n"); }
static void o_dec(uint64_t v) {
    char t[21]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0 && o_len + 1 < o_max) o_buf[o_len++] = t[--i];
    if (o_max) o_buf[o_len] = '\0';
}
static void scopy(char *d, const char *s, size_t max) {
    size_t i = 0;
    if (!s) { if (max) d[0] = '\0'; return; }
    while (s[i] && i + 1 < max) { d[i] = s[i]; i++; }
    d[i] = '\0';
}


const char *browse_last_page(void) { return g_page; }
const char *browse_last_trace(void) { return g_trace; }
const char *browse_last_url(void) { return g_url[0] ? g_url : "(none)"; }

/* ---- tiny HTML → text ---------------------------------------------------- */
static char h_ent(const char **pp) {
    const char *p = *pp;
    if (p[0]=='a'&&p[1]=='m'&&p[2]=='p'&&p[3]==';') { *pp = p+4; return '&'; }
    if (p[0]=='l'&&p[1]=='t'&&p[2]==';') { *pp = p+3; return '<'; }
    if (p[0]=='g'&&p[1]=='t'&&p[2]==';') { *pp = p+3; return '>'; }
    if (p[0]=='n'&&p[1]=='b'&&p[2]=='s'&&p[3]=='p'&&p[4]==';') { *pp = p+5; return ' '; }
    if (p[0]=='q'&&p[1]=='u'&&p[2]=='o'&&p[3]=='t'&&p[4]==';') { *pp = p+5; return '"'; }
    return '&';
}

static bool tag_is(const char *t, const char *n) {
    while (*n) {
        char a = *t, b = *n;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
        t++; n++;
    }
    return *t == 0 || *t == ' ' || *t == '>' || *t == '/';
}

static void html_to_text(const char *html, char *out, size_t outmax) {
    o_reset(out, outmax);
    o_str("Wired browser  (HTML only — no CSS/JS/TLS)\n");
    if (g_url[0]) { o_str("url: "); o_str(g_url); o_nl(); }
    o_nl();
    bool skip = false;
    bool in_a = false;
    char href[120];
    href[0] = '\0';
    const char *p = html;
    char linkbuf[8][120];
    int nlinks = 0;
    while (*p && o_len + 8 < o_max) {
        if (*p == '&') { p++; char c = h_ent(&p); if (!skip) {
            char b[2] = { c, 0 }; o_str(b);
        } continue; }
        if (*p != '<') {
            if (!skip) {
                char b[2] = { *p, 0 };
                if (*p == '\r') { p++; continue; }
                o_str(b);
            }
            p++;
            continue;
        }
        p++;
        bool closing = (*p == '/');
        if (closing) p++;
        char name[16]; int ni = 0;
        while (*p && *p != ' ' && *p != '>' && *p != '/' && ni < 15)
            name[ni++] = *p++;
        name[ni] = '\0';
        /* lowercase */
        for (int i = 0; i < ni; i++)
            if (name[i] >= 'A' && name[i] <= 'Z')
                name[i] = (char)(name[i] - 'A' + 'a');

        if (!closing && (tag_is(name, "script") || tag_is(name, "style")))
            skip = true;
        if (closing && (tag_is(name, "script") || tag_is(name, "style")))
            skip = false;

        if (!closing && tag_is(name, "a")) {
            in_a = true;
            href[0] = '\0';
            /* find href= */
            const char *q = p;
            while (*q && *q != '>') {
                if ((q[0]=='h'||q[0]=='H')&&(q[1]=='r'||q[1]=='R')&&
                    (q[2]=='e'||q[2]=='E')&&(q[3]=='f'||q[3]=='F')) {
                    q += 4;
                    while (*q == ' ' || *q == '=') q++;
                    if (*q == '"' || *q == '\'') {
                        char qch = *q++; int hi = 0;
                        while (*q && *q != qch && hi < (int)sizeof(href)-1)
                            href[hi++] = *q++;
                        href[hi] = '\0';
                    }
                    break;
                }
                q++;
            }
        }
        if (closing && tag_is(name, "a")) {
            if (href[0] && nlinks < 8) {
                scopy(linkbuf[nlinks], href, sizeof(linkbuf[0]));
                nlinks++;
            }
            in_a = false;
            (void)in_a;
        }
        if (!closing && (tag_is(name, "p") || tag_is(name, "br") ||
                         tag_is(name, "div") || tag_is(name, "tr") ||
                         tag_is(name, "li") || tag_is(name, "h1") ||
                         tag_is(name, "h2") || tag_is(name, "h3")))
            o_nl();
        if (!closing && tag_is(name, "title")) o_str("[ ");
        if (closing && tag_is(name, "title")) o_str(" ]\n");

        while (*p && *p != '>') p++;
        if (*p == '>') p++;
    }
    if (nlinks) {
        o_nl(); o_str("links:\n");
        for (int i = 0; i < nlinks; i++) {
            o_str("  "); o_dec((uint64_t)(i + 1)); o_str(") ");
            o_str(linkbuf[i]); o_nl();
        }
    }
}

/* ---- styled render: HTML + CSS subset ------------------------------------
 * Parses <style> blocks and inline style="" for: color (named/#hex),
 * font-size (px), font-weight (bold), text-align (center/right). Headings
 * get default sizes; links get the corona accent. JS/React pages need the
 * staged JS engine — until then SPAs render their no-JS fallback.
 * ------------------------------------------------------------------------ */
#define NOVA_MAXLINES 160
typedef struct {
    char     text[112];
    uint32_t color;
    uint8_t  size;      /* 0 = body, 1 = h2-ish, 2 = h1-ish   */
    uint8_t  bold;
    uint8_t  align;     /* 0 left, 1 center, 2 right          */
} nova_line;
static nova_line g_nova_lines[NOVA_MAXLINES];
static int       g_nova_line_n;
static uint32_t  g_nova_bg;

static uint32_t css_color(const char *s) {
    static const struct { const char *n; uint32_t c; } N[] = {
        {"white",0xFFFFFFFF},{"black",0xFF101014},{"red",0xFFFF5555},
        {"green",0xFF55CC55},{"blue",0xFF6FA8FF},{"orange",0xFFFF9D45},
        {"yellow",0xFFFFE066},{"purple",0xFFB48CFF},{"pink",0xFFFF8AC2},
        {"gray",0xFF9A9AA4},{"grey",0xFF9A9AA4},{"cyan",0xFF66E0E0},
        {"teal",0xFF4FC3B8},{"coral",0xFFFF7F66},
    };
    while (*s == ' ') s++;
    if (s[0] == '#') {
        unsigned v = 0; int n = 0;
        s++;
        while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')) {
            char c = *s++; int d = (c <= '9') ? c - '0' : (c | 32) - 'a' + 10;
            v = v * 16 + d; n++;
        }
        if (n == 3) v = ((v & 0xF00) >> 8) * 17 * 0x10000 | ((v & 0x0F0) >> 4) * 17 * 0x100 | (v & 0xF) * 17;
        return 0xFF000000u | (v & 0xFFFFFF);
    }
    for (unsigned i = 0; i < sizeof(N)/sizeof(N[0]); i++)
        if (tag_is(s, N[i].n)) return N[i].c;
    return 0;
}

/* pull `prop: value` pairs from a css fragment into the current style */
typedef struct { uint32_t color; int size; int bold; int align; } css_state;

static void css_apply(css_state *st, const char *decls) {
    const char *p = decls;
    while (*p) {
        const char *e = p;
        while (*e && *e != ';') e++;
        const char *c = p;
        while (c < e && *c != ':') c++;
        if (c < e) {
            if ((c - p) == 5 && (p[0]=='c'||p[0]=='C') && (p[1]=='o') && (p[2]=='l') && (p[3]=='o') && (p[4]=='r')) {
                uint32_t v = css_color(c + 1);
                if (v) st->color = v;
            } else if ((c - p) >= 9 && (p[0]=='f') && (p[1]=='o') && (p[2]=='n') && (p[3]=='t') && (p[4]=='-') && (p[5]=='s')) {
                const char *q = c + 1;
                int px = 0;
                while (*q && (*q < '0' || *q > '9')) q++;
                while (*q >= '0' && *q <= '9') { px = px * 10 + (*q - '0'); q++; }
                if (px >= 20) st->size = 2;
                else if (px >= 15) st->size = 1;
                else if (px) st->size = 0;
            } else if ((c - p) >= 11 && (p[0]=='f') && (p[1]=='o') && (p[2]=='n') && (p[3]=='t') && (p[4]=='-') && (p[5]=='w')) {
                const char *q = c + 1;
                if (tag_is(q, "bold") || tag_is(q, "bolder")) st->bold = 1;
                else if (tag_is(q, "normal")) st->bold = 0;
            } else if ((c - p) >= 10 && (p[0]=='t') && (p[1]=='e') && (p[2]=='x') && (p[3]=='t') && (p[4]=='-') && (p[5]=='a')) {
                const char *q = c + 1;
                if (tag_is(q, "center")) st->align = 1;
                else if (tag_is(q, "right")) st->align = 2;
                else st->align = 0;
            }
        }
        p = (*e) ? e + 1 : e;
    }
}

/* scan a <style> block for rules that set body background */
static void css_scan_style(const char *s, const char *end, css_state *body_st) {
    while (s < end) {
        const char *brace = s;
        while (brace < end && *brace != '{') brace++;
        if (brace >= end) break;
        const char *close = brace;
        while (close < end && *close != '}') close++;
        /* does the selector mention body? */
        const char *sel = brace;
        while (sel > s && (sel[-1] == ' ' || sel[-1] == '\n' || sel[-1] == '\t' || sel[-1] == ',')) sel--;
        const char *w = brace;
        while (w > s && w[-1] != '}' && w[-1] != ',' && w[-1] != '\n') w--;
        (void)sel;
        {
            /* crude: if the selector text contains "body", apply to body state */
            const char *q = w;
            int is_body = 0;
            while (q < brace) { if ((q[0]=='b'||q[0]=='B') && (q[1]=='o') && (q[2]=='d') && (q[3]=='y')) { is_body = 1; break; } q++; }
            if (is_body) css_apply(body_st, brace + 1);
        }
        s = (close < end) ? close + 1 : end;
    }
}

static void nova_push_line(const css_state *st, const char *txt, int len) {
    if (g_nova_line_n >= NOVA_MAXLINES) return;
    if (len <= 0) {                       /* keep blank lines slim */
        if (g_nova_line_n && g_nova_lines[g_nova_line_n - 1].text[0] == 0) return;
        len = 0;
    }
    nova_line *L = &g_nova_lines[g_nova_line_n++];
    int i = 0;
    for (int k = 0; k < len && i < (int)sizeof(L->text) - 1; k++) L->text[i++] = txt[k];
    L->text[i] = '\0';
    L->color = st->color;
    L->size  = (uint8_t)st->size;
    L->bold  = (uint8_t)st->bold;
    L->align = (uint8_t)st->align;
}

static void html_render(const char *html) {
    g_nova_line_n = 0;
    g_nova_bg = 0xFF0B0D12;
    css_state st = { 0xFFE8E8F0, 0, 0, 0 };
    const char *p = html;
    char word[112]; int wn = 0;

    while (*p) {
        if (*p == '<') {
            if (wn) { nova_push_line(&st, word, wn); wn = 0; }
            const char *close = p + 1;
            while (*close && *close != '>') close++;
            if (!*close) break;
            /* <style> ... </style> */
            if ((p[1]=='s'||p[1]=='S') && (p[2]=='t'||p[2]=='T')) {
                const char *body = close + 1;
                const char *send = body;
                while (*send && !(send[0]=='<' && (send[1]=='/'||send[1]=='/') )) {
                    if (send[0]=='<' && (send[1]=='s'||send[1]=='S')) break;
                    send++;
                }
                css_scan_style(body, send, &st);
                p = (*send) ? send : close + 1;
                continue;
            }
            /* inline style="..." on any open tag */
            {
                const char *q = p + 1;
                while (q < close) {
                    if ((q[0]=='s'||q[0]=='S') && (q[1]=='t'||q[1]=='T') && (q[2]=='y'||q[2]=='Y') &&
                        (q[3]=='l'||q[3]=='L') && (q[4]=='e') && (q[5]=='=')) {
                        q += 6;
                        if (*q == '"' || *q == '\'') q++;
                        css_apply(&st, q);
                        break;
                    }
                    q++;
                }
            }
            /* tag semantics */
            char tag[12]; int tn = 0;
            const char *q = p + 1;
            if (*q == '/') q++;
            while (q < close && tn < 11 && *q != ' ' && *q != '/' && *q != '\t') tag[tn++] = *q++;
            tag[tn] = '\0';
            if (tag_is(tag, "h1"))      { st.size = 2; st.bold = 1; if (!wn) nova_push_line(&st, "", 0); }
            else if (tag_is(tag, "h2")) { st.size = 1; st.bold = 1; if (!wn) nova_push_line(&st, "", 0); }
            else if (tag_is(tag, "h3")) { st.bold = 1; }
            else if (tag_is(tag, "p") || tag_is(tag, "div") || tag_is(tag, "br") ||
                     tag_is(tag, "li") || tag_is(tag, "tr")) {
                nova_push_line(&st, word, wn); wn = 0;
                nova_push_line(&st, "", 0);
            }
            else if (tag_is(tag, "li")) { /* bullet handled above */ }
            else if (tag_is(tag, "a"))  { st.color = 0xFFFF9D45; }   /* corona accent */
            else if (tag_is(tag, "b") || tag_is(tag, "strong")) st.bold = 1;
            else if (tag_is(tag, "center")) st.align = 1;
            p = close + 1;
            continue;
        }
        if (*p == '\n' || *p == '\r') { if (wn) { nova_push_line(&st, word, wn); wn = 0; } p++; continue; }
        if (*p == ' ') {
            if (wn) { nova_push_line(&st, word, wn); wn = 0; }
            p++; continue;
        }
        if (wn == 0 && (p[0]=='&' && p[1]=='n')) { word[wn++] = ' '; p += 6; continue; }
        if (wn < (int)sizeof(word) - 1) word[wn++] = *p;
        else { nova_push_line(&st, word, wn); wn = 0; }
        p++;
    }
    if (wn) nova_push_line(&st, word, wn);
}


/* Styled render output for the Nova GUI (HTML + CSS subset). */
const void *browse_lines(void)      { return g_nova_lines; }
int         browse_line_count(void) { return g_nova_line_n; }
uint32_t    browse_page_bg(void)    { return g_nova_bg; }

void browse_cmd(const char *url, char *out, size_t outmax) {
    o_reset(out, outmax);
    if (!url || !url[0]) {
        o_str("Wired browser\n");
        o_str("  browse <url>   fetch page — http:// and https:// (TLS 1.2)\n");
        o_str("  last: "); o_str(browse_last_url()); o_nl();
        if (g_page[0]) { o_nl(); o_str(g_page); }
        return;
    }
    if (url[0]=='r' && url[1]=='g' && url[2]=='x' && url[3]==':' && url[4]=='/' && url[5]=='/') {
        /* Local pages: rgx://about, rgx://laws, rgx://guide */
        char buf[1600];
        if (!rgx_local_page(url, buf, sizeof(buf))) {
            o_str("browse: no local page "); o_str(url); o_nl();
            o_str("local: rgx://about  rgx://laws  rgx://guide\n");
            return;
        }
        scopy(g_url, url, sizeof(g_url));
        scopy(g_page, buf, sizeof(g_page));
        o_str("rgx:// local page\n \n");
        o_str(g_page);
        return;
    }
    if (!(url[0]=='h' && url[1]=='t' && url[2]=='t' && url[3]=='p')) {
        o_str("browse: need an http:// or https:// URL\n");
        o_str("example: browse https://www.rogexlaboratories.com/\n");
        return;
    }
    if (!www_ready()) {
        o_str("browse: www is off.  www on   then retry.\n");
        return;
    }
    scopy(g_url, url, sizeof(g_url));
    int st = 0;
    char raw[WWW_BODY_MAX];
    uint32_t n = www_http_get(url, raw, sizeof(raw), &st);
    if (!n) {
        o_str("browse: fetch failed ("); o_str(url); o_str(")\n");
        o_str("hint: https sites need TLS — check 'www status' and DNS.\n");
        g_page[0] = '\0';
        return;
    }
    o_str("HTTP "); o_dec((uint64_t)st); o_str("  "); o_dec((uint64_t)n);
    o_str(" bytes\n \n");
    html_render(raw);                    /* styled lines for the Nova GUI */
    html_to_text(raw, g_page, sizeof(g_page));
    o_str(g_page);
}

static void fmt_ip(const uint8_t ip[4], char *b, size_t max) {
    /* reuse ip_fmt if available */
    ip_fmt(ip, b, (uint32_t)max);
}

void trace_cmd(const char *target, char *out, size_t outmax) {
    o_reset(out, outmax);
    if (!target || !target[0]) {
        o_str("usage: trace <alias|dotted-ip|host>\n");
        o_str("  mesh alias → L2 hop (same Ethernet domain)\n");
        o_str("  IPv4        → ICMP TTL traceroute (www on)\n");
        return;
    }
    /* Mesh alias? not an IP and not looking like a hostname with dots only digits */
    uint8_t ip[4];
    if (!ip_parse(target, ip)) {
        /* L2 traceroute: one hop on EtherType 0x88B5 */
        o_str("L2 traceroute  (Rogex Wired 0x88B5)\n");
        o_str("  target: "); o_str(target); o_nl();
        if (!wired_up()) {
            o_str("  link DOWN — attach a NIC first\n");
            scopy(g_trace, out, sizeof(g_trace));
            return;
        }
        o_str("  hop 1  "); o_str(wired_my_alias());
        o_str("  (you)\n");
        wired_clear_replies();
        if (!wired_send_ping(target)) {
            o_str("  hop 2  send failed\n");
            scopy(g_trace, out, sizeof(g_trace));
            return;
        }
        uint64_t start = arch_timer_ticks();
        while (arch_timer_ticks() - start < 150) {
            wired_service();
            arch_idle_wait();
        }
        if (wired_reply_count() == 0) {
            o_str("  hop 2  *  timeout (no PONG / ACK from ");
            o_str(target); o_str(")\n");
        } else {
            for (int i = 0; i < wired_reply_count(); i++) {
                o_str("  hop 2  "); o_str(wired_reply_alias(i));
                o_str("  ACK  "); o_dec((uint64_t)wired_reply_rtt_ms(i));
                o_str(" ms   (same L2 domain — no IP routers)\n");
            }
        }
        scopy(g_trace, out, sizeof(g_trace));
        return;
    }

    o_str("ICMP traceroute  ");
    { char b[20]; fmt_ip(ip, b, sizeof(b)); o_str(b); } o_nl();
    if (!www_ready() && !ipstack_configured()) {
        o_str("  www is off and no IP config.  www on\n");
        scopy(g_trace, out, sizeof(g_trace));
        return;
    }
    if (!ipstack_configured()) ipstack_use_qemu_defaults();
    ip_trace_hop_t hops[IP_TRACE_MAX];
    int n = ip_traceroute(ip, hops, 12, 800);
    if (!n) { o_str("  no hops (ARP/send failed)\n"); scopy(g_trace, out, sizeof(g_trace)); return; }
    for (int i = 0; i < n; i++) {
        o_str("  "); o_dec((uint64_t)hops[i].ttl); o_str("  ");
        if (hops[i].kind == 0) o_str("*  timeout");
        else {
            char b[20]; fmt_ip(hops[i].ip, b, sizeof(b)); o_str(b);
            o_str("  "); o_dec((uint64_t)hops[i].rtt_ms); o_str(" ms");
            if (hops[i].kind == 2) o_str("  (destination)");
        }
        o_nl();
    }
    scopy(g_trace, out, sizeof(g_trace));
}

void nics_cmd(char *out, size_t outmax) {
    o_reset(out, outmax);
    o_str("PCI network controllers\n \n");
    net_pci_dev_t list[NET_PCI_MAX];
    int n = net_pci_list(list, NET_PCI_MAX);
    if (!n) o_str("  (none on any PCI bus)\n");
    for (int i = 0; i < n; i++) {
        o_str("  ");
        /* vendor:device hex */
        static const char H[] = "0123456789abcdef";
        char id[12];
        id[0]=H[(list[i].vendor>>12)&0xf]; id[1]=H[(list[i].vendor>>8)&0xf];
        id[2]=H[(list[i].vendor>>4)&0xf];  id[3]=H[list[i].vendor&0xf];
        id[4]=':';
        id[5]=H[(list[i].device>>12)&0xf]; id[6]=H[(list[i].device>>8)&0xf];
        id[7]=H[(list[i].device>>4)&0xf];  id[8]=H[list[i].device&0xf];
        id[9]=0;
        o_str(id);
        o_str("  class "); o_dec(list[i].cls); o_str("/"); o_dec(list[i].subcls);
        o_nl();
        o_str("         "); o_str(list[i].note); o_nl();
    }
    o_nl();
    o_str("bound driver: "); o_str(net_driver_name()); o_nl();
    if (net_present()) {
        char mac[24];
        wired_mac_str(mac, sizeof(mac));
        o_str("MAC: "); o_str(mac); o_nl();
    }
    o_str(" \nHP 15-ac195nl: Ethernet 10EC:8136 (r8169 / RTL8106E).\n");
    o_str("WiFi 10EC:8179 (RTL8188EE) has no driver — use the cable.\n");
    o_str("There is no single universal NIC: we probe virtio, Intel e1000,\n");
    o_str("Realtek 810/8168, and RTL8139. That covers most PCs this OS sees.\n");
}

void nmap_cmd(const char *host, char *out, size_t outmax) {
    static const uint16_t PORTS[] = {
        21, 22, 23, 25, 53, 80, 110, 143, 443, 445, 3306, 8080, 8443, 0
    };
    static const char *NAMES[] = {
        "ftp","ssh","telnet","smtp","dns","http","pop3","imap","https",
        "smb","mysql","http-alt","https-alt"
    };
    uint8_t ip[4];
    int i, open_n = 0;
    o_reset(out, outmax);
    if (!host || !host[0]) {
        o_str("usage: nmap <host|ip>\n");
        o_str("  probes a small TCP set: 22 80 443 8080 ...  (www on)\n");
        return;
    }
    if (!ip_parse(host, ip)) {
        if (!www_ready()) {
            o_str("nmap: need dotted IP or `www on` for DNS\n");
            return;
        }
        if (!www_dns(host, ip, 3000)) {
            o_str("nmap: DNS failed for "); o_str(host); o_nl();
            return;
        }
    }
    if (!ipstack_configured()) ipstack_use_qemu_defaults();
    o_str("nmap "); o_str(host); o_str("  (");
    { char b[20]; ip_fmt(ip, b, sizeof(b)); o_str(b); }
    o_str(")\n");
    o_str("PORT     STATE  SERVICE\n");
    for (i = 0; PORTS[i]; i++) {
        o_str("  "); o_dec(PORTS[i]);
        { int w = 0; uint16_t v = PORTS[i];
          if (v >= 1000) w = 4; else if (v >= 100) w = 3; else if (v >= 10) w = 2; else w = 1;
          while (w < 6) { o_str(" "); w++; } }
        if (tcp_connect(ip, PORTS[i], 350)) {
            o_str("open   "); o_str(NAMES[i]); o_nl();
            tcp_close();
            open_n++;
        } else {
            o_str("closed "); o_str(NAMES[i]); o_nl();
        }
    }
    o_str("\n"); o_dec((uint64_t)open_n); o_str(" open port(s)\n");
}
