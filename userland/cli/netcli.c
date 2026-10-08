/*
 * curl / wdl / wget / ipconf — HTTP(S) transfer + NIC inventory.
 */
#include "netcli.h"
#include "commands.h"
#include "www.h"
#include "tls_min.h"
#include "netif.h"
#include "ipstack.h"
#include "wired.h"
#include "vfs.h"
#include "arch.h"
#include "scheduler.h"

static int eqs(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static void cpy(char *d, const char *s, int max);

static int looks_url(const char *s) {
    int i, dots = 0;
    if (!s || !s[0]) return 0;
    if (s[0]=='h' && s[1]=='t' && s[2]=='t' && s[3]=='p') return 1;
    if (s[0]=='/' && s[1]=='/') return 1;
    /* host.tld[/path] — "google.com" is a URL, not a search phrase */
    for (i = 0; s[i] && s[i] != '/' && s[i] != '?' && s[i] != ':'; i++) {
        char c = s[i];
        if (c == '.') dots++;
        else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-'))
            return 0;
    }
    return dots >= 1 && i >= 3;
}

/* Bare hosts (youtube.com) become https://youtube.com. */
static void ensure_url(char *url, int max) {
    char tmp[196];
    int i = 0;
    if (!url || !url[0] || max < 10) return;
    if (url[0]=='h' && url[1]=='t' && url[2]=='t' && url[3]=='p') return;
    tmp[0]='h'; tmp[1]='t'; tmp[2]='t'; tmp[3]='p';
    tmp[4]='s'; tmp[5]=':'; tmp[6]='/'; tmp[7]='/';
    if (url[0]=='/' && url[1]=='/') url += 2;
    while (url[i] && i + 9 < max && i + 8 < (int)sizeof(tmp) - 1) {
        tmp[8 + i] = url[i];
        i++;
    }
    tmp[8 + i] = 0;
    cpy(url, tmp, max);
}

static void cpy(char *d, const char *s, int max) {
    int i = 0;
    if (!d || max < 1) return;
    if (!s) { d[0] = 0; return; }
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void wget_basename(const char *url, char *out, int max) {
    const char *s = url, *last = url;
    int n = 0;
    while (*s) {
        if (*s == '/') last = s + 1;
        s++;
    }
    while (*last && *last != '?' && *last != '#' && n < max - 1) out[n++] = *last++;
    out[n] = 0;
    if (!out[0]) { out[0] = 'i'; out[1] = 'n'; out[2] = 'd'; out[3] = 'e';
                   out[4] = 'x'; out[5] = '.'; out[6] = 'h'; out[7] = 't';
                   out[8] = 'm'; out[9] = 'l'; out[10] = 0; }
}

/* Quoted tokenizer: "a b" and 'a b' stay one token. */
static int tok_line(char *s, const char **tok, int tmax) {
    int n = 0;
    while (*s && n < tmax) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (*s == '"' || *s == '\'') {
            char q = *s++;
            tok[n++] = s;
            while (*s && *s != q) s++;
            if (*s) *s++ = 0;
        } else {
            tok[n++] = s;
            while (*s && *s != ' ' && *s != '\t') s++;
            if (*s) *s++ = 0;
        }
    }
    return n;
}

/* Form bodies often written with $ instead of & (the shell does not expand $). */
static void dollar_to_amp(char *s) {
    int eq = 0, amp = 0, dol = 0;
    char *p;
    for (p = s; *p; p++) {
        if (*p == '=') eq++;
        if (*p == '&') amp++;
        if (*p == '$') dol++;
    }
    if (dol && eq >= 2 && !amp) {
        for (p = s; *p; p++) if (*p == '$') *p = '&';
    }
}

static void wait_ms(uint32_t ms) {
    uint64_t start = arch_timer_ticks();
    uint32_t ticks = ms / 10;
    if (!ticks) ticks = 1;
    while (arch_timer_ticks() - start < ticks) {
        www_service();
        sched_poll_relax(1);
    }
}

static void print_chunk(const char *s, uint32_t n, uint32_t cap) {
    uint32_t i, show = n < cap ? n : cap;
    for (i = 0; i < show; i++) {
        char c = s[i];
        char b[2];
        if (c == '\n' || c == '\t' || ((unsigned char)c >= 0x20 && (unsigned char)c != 0x7f)) {
            b[0] = c; b[1] = 0; o_str(b);
        }
    }
    if (n > show) o_str("\n... (truncated)\n");
    else if (show && s[show - 1] != '\n') o_nl();
}

static void print_visible(const char *body, uint32_t n) {
    static char text[4096];
    int tn;
    if (!n) return;
    tn = www_html_text(body, (int)n, text, (int)sizeof(text));
    print_chunk(text, (uint32_t)(tn > 0 ? tn : 0), 3600);
}

static void apply_headers(const char *cookie, const char *range, const char *extra_h) {
    www_clear_extra_hdr();
    if (cookie && cookie[0]) {
        char line[240];
        line[0]='C'; line[1]='o'; line[2]='o'; line[3]='k'; line[4]='i';
        line[5]='e'; line[6]=':'; line[7]=' ';
        cpy(line + 8, cookie, (int)(sizeof(line) - 8));
        www_add_extra_hdr(line);
    }
    if (range && range[0]) www_add_extra_hdr(range);
    if (extra_h && extra_h[0]) www_add_extra_hdr(extra_h);
}

static int save_body(const char *path, const char *body, uint32_t n) {
    if (!path || !path[0] || !n) return 0;
    vfs_mkdir("/tmp");
    if (vfs_write(path, body, n)) {
        o_str("saved "); o_str(path); o_str("  "); o_dec(n); o_str(" B\n");
        return 1;
    }
    o_str("! write failed: "); o_str(path); o_nl();
    return 0;
}

static uint32_t resume_merge(const char *path, const char *fresh, uint32_t nfresh,
                             char *out, uint32_t omax, int st) {
    uint32_t oldn = 0, i;
    const char *old;
    if (st != 206) return 0;
    old = vfs_read(path, &oldn);
    if (!old || !oldn) return 0;
    if (oldn + nfresh + 1 > omax) {
        o_str("! resume: combined body larger than buffer\n");
        return 0;
    }
    for (i = 0; i < oldn; i++) out[i] = old[i];
    for (i = 0; i < nfresh; i++) out[oldn + i] = fresh[i];
    out[oldn + nfresh] = 0;
    return oldn + nfresh;
}

static void jar_write(const char *path) {
    char jar[400];
    uint32_t n;
    if (!path || !path[0]) return;
    n = www_last_set_cookies(jar, sizeof(jar));
    if (!n) { o_str("(no Set-Cookie)\n"); return; }
    if (vfs_write(path, jar, n)) {
        o_str("cookie-jar "); o_str(path); o_str("  "); o_dec(n); o_str(" B\n");
    } else {
        o_str("! cookie-jar write failed: "); o_str(path); o_nl();
    }
}

static void curl_usage(void) {
    o_str("curl — HTTP/1.1 GET/POST + HTTPS TLS 1.3 (fallback 1.2).  man curl\n");
    o_str("  curl <url>                         GET, follow redirects, visible text\n");
    o_str("  curl -I <url>                      headers only\n");
    o_str("  curl -X POST -d \"a=b&c=d\" <url>    POST form\n");
    o_str("  curl -O <url>   curl -o PATH <url> save body (HTML)\n");
    o_str("  curl -b COOKIE -c JAR <url>        cookies\n");
    o_str("  curl -dom <url>                    wait + visible HTML (no JS VM)\n");
    o_str("  curl --html <url>                  raw HTML on the console\n");
    o_str("  curl search <query>\n");
}

void netcli_curl(const char *a1, const char *rest) {
    char method[8] = "GET";
    char url[196];
    char data[400];
    char outpath[96];
    char cookie[200];
    char jar[96];
    char extra_h[200];
    char buf[500];
    const char *tok[24];
    int nt, i, di;
    int raw = 0, search = 0, have_url = 0, have_data = 0, have_out = 0;
    int head = 0, follow = 1, remote = 0, dom = 0;
    char *w, *we;

    url[0] = data[0] = outpath[0] = cookie[0] = jar[0] = extra_h[0] = 0;

    w = buf; we = buf + sizeof(buf);
    if (a1 && a1[0]) { while (*a1 && w < we - 1) *w++ = *a1++; }
    if (rest && rest[0]) {
        if (w > buf && w < we - 1) *w++ = ' ';
        while (*rest && w < we - 1) *w++ = *rest++;
    }
    *w = 0;
    nt = tok_line(buf, tok, 24);
    if (nt == 0) { curl_usage(); return; }

    for (i = 0; i < nt; i++) {
        if (eqs(tok[i], "-o") || eqs(tok[i], "--output")) {
            if (i + 1 < nt) { cpy(outpath, tok[++i], (int)sizeof(outpath)); have_out = 1; }
            continue;
        }
        if (eqs(tok[i], "-O") || eqs(tok[i], "--remote-name")) { remote = 1; continue; }
        if (eqs(tok[i], "-I") || eqs(tok[i], "--head")) {
            head = 1;
            method[0]='H'; method[1]='E'; method[2]='A'; method[3]='D'; method[4]=0;
            continue;
        }
        if (eqs(tok[i], "-L") || eqs(tok[i], "--location")) { follow = 1; continue; }
        if (eqs(tok[i], "-dom") || eqs(tok[i], "--dom")) { dom = 1; follow = 1; continue; }
        if (eqs(tok[i], "--html") || eqs(tok[i], "-html")) { raw = 1; continue; }
        if (eqs(tok[i], "--text") || eqs(tok[i], "-text")) { raw = 0; continue; }
        if (eqs(tok[i], "-b") || eqs(tok[i], "--cookie")) {
            if (i + 1 < nt) cpy(cookie, tok[++i], (int)sizeof(cookie));
            continue;
        }
        if (eqs(tok[i], "-c") || eqs(tok[i], "--cookie-jar")) {
            if (i + 1 < nt) cpy(jar, tok[++i], (int)sizeof(jar));
            continue;
        }
        if (eqs(tok[i], "-H") || eqs(tok[i], "--header")) {
            if (i + 1 < nt) cpy(extra_h, tok[++i], (int)sizeof(extra_h));
            continue;
        }
        if (eqs(tok[i], "-X") || eqs(tok[i], "--request")) {
            if (i + 1 < nt) {
                const char *m = tok[++i];
                di = 0;
                while (m[di] && di < 7) { method[di] = m[di]; di++; }
                method[di] = 0;
            }
            continue;
        }
        if (eqs(tok[i], "-d") || eqs(tok[i], "--data") || eqs(tok[i], "--data-raw")) {
            if (i + 1 < nt) {
                cpy(data, tok[++i], (int)sizeof(data));
                dollar_to_amp(data);
                have_data = 1;
                if (method[0]=='G') {
                    method[0]='P'; method[1]='O'; method[2]='S'; method[3]='T'; method[4]=0;
                }
            }
            continue;
        }
        if (eqs(tok[i], "search") || eqs(tok[i], "google") || eqs(tok[i], "--search")) {
            search = 1;
            w = data; we = data + sizeof(data);
            for (i = i + 1; i < nt && w < we - 1; i++) {
                const char *p = tok[i];
                if (w > data) *w++ = ' ';
                while (*p && w < we - 1) *w++ = *p++;
            }
            *w = 0;
            have_data = data[0] != 0;
            break;
        }
        if (eqs(tok[i], "get") || eqs(tok[i], "GET")) {
            method[0]='G'; method[1]='E'; method[2]='T'; method[3]=0;
            continue;
        }
        if (eqs(tok[i], "post") || eqs(tok[i], "POST")) {
            method[0]='P'; method[1]='O'; method[2]='S'; method[3]='T'; method[4]=0;
            continue;
        }
        if (looks_url(tok[i]) && !have_url) {
            cpy(url, tok[i], (int)sizeof(url));
            have_url = 1;
            if (i + 1 < nt && !have_data && method[0]=='P') {
                w = data; we = data + sizeof(data);
                for (i = i + 1; i < nt && w < we - 1; i++) {
                    const char *p = tok[i];
                    if (w > data) *w++ = ' ';
                    while (*p && w < we - 1) *w++ = *p++;
                }
                *w = 0;
                dollar_to_amp(data);
                if (data[0]) have_data = 1;
            }
            continue;
        }
        if (!have_url && !search && tok[i][0] != '-') {
            if (looks_url(tok[i]) || (tok[i][0] && tok[i][0] != '-')) {
                /* Last non-flag token: treat as URL if it looks like a host,
                   otherwise as a search query. */
                if (looks_url(tok[i])) {
                    cpy(url, tok[i], (int)sizeof(url));
                    have_url = 1;
                    continue;
                }
            }
            search = 1;
            w = data; we = data + sizeof(data);
            for (; i < nt && w < we - 1; i++) {
                const char *p = tok[i];
                if (w > data) *w++ = ' ';
                while (*p && w < we - 1) *w++ = *p++;
            }
            *w = 0;
            have_data = data[0] != 0;
            break;
        }
    }

    if (!www_enable()) {
        o_str("! curl: "); o_str(www_last_err()[0] ? www_last_err() : "need a NIC"); o_nl();
        return;
    }

    if (search) {
        static char ans[1200];
        int st = 0, n;
        if (!data[0]) { o_str("usage: curl search <query>\n"); return; }
        n = www_web_search(data, ans, (int)sizeof(ans), &st);
        o_str(ans);
        if (n && ans[n - 1] != '\n') o_nl();
        return;
    }
    if (!have_url) { curl_usage(); return; }
    ensure_url(url, (int)sizeof(url));
    if (commands_verbose()) {
        commands_v("one TCP socket, HTTP/1.0, no CA pin, no JavaScript");
        commands_v(url);
    }

    if (remote && !have_out) {
        char base[64];
        wget_basename(url, base, (int)sizeof(base));
        outpath[0]='/'; outpath[1]='t'; outpath[2]='m'; outpath[3]='p'; outpath[4]='/';
        cpy(outpath + 5, base, (int)sizeof(outpath) - 5);
        have_out = 1;
    }

    apply_headers(cookie, 0, extra_h);
    if (dom) www_set_read_ms(4000);

    {
        static char body[WWW_BODY_MAX];
        int st = 0;
        uint32_t n;
        const char *ctype = have_data ? "application/x-www-form-urlencoded" : 0;

        n = www_http_request_ex(method, url,
                                have_data ? data : 0,
                                have_data ? (uint32_t)k_strlen(data) : 0,
                                ctype, body, sizeof(body), &st, follow);

        if (dom) {
            o_str("curl -dom: waiting 2s (no JS engine; Vite SPAs stay as shell HTML)...\n");
            wait_ms(2000);
            apply_headers(cookie, 0, extra_h);
            n = www_http_request_ex("GET", url, 0, 0, 0, body, sizeof(body), &st, 1);
        }

        o_str("HTTP "); o_dec((uint64_t)st); o_str("  "); o_dec(n);
        o_str(" bytes  "); o_str(www_last_url()[0] ? www_last_url() : url);
        if (url[4] == 's' || (url[0]=='h' && url[4]=='s')) {
            o_str("  ["); o_str(tls_version_name()); o_str("]");
        }
        o_nl();
        if (!n && www_last_err()[0]) { o_str("! "); o_str(www_last_err()); o_nl(); }

        if (head) {
            char hdr[1600];
            uint32_t hn = www_last_headers(hdr, sizeof(hdr));
            print_chunk(hdr, hn, 1500);
        } else if (dom) {
            o_str("--- visible text ---\n");
            print_visible(body, n);
            o_str("--- html ---\n");
            print_chunk(body, n, 2400);
            o_str("note: echOS has no JavaScript VM. -dom re-fetches after 2s.\n");
        } else if (!have_out) {
            if (raw) print_chunk(body, n, 1200);
            else print_visible(body, n);
            if (!n && www_last_err()[0]) { o_str("! "); o_str(www_last_err()); o_nl(); }
        }

        if (have_out && n) save_body(outpath, body, n);
        if (jar[0]) jar_write(jar);
    }
    www_clear_extra_hdr();
    www_set_read_ms(0);
}

static void dl_usage(const char *verb) {
    o_str(verb); o_str(" — download a URL to a file (www on).  man "); o_str(verb); o_nl();
    o_str("  "); o_str(verb); o_str(" <url>\n");
    o_str("  "); o_str(verb); o_str(" -O PATH <url>     output file\n");
    o_str("  "); o_str(verb); o_str(" -c -O PATH <url>  continue (Range)\n");
    o_str("  "); o_str(verb); o_str(" -b <url>          no-fork \"background\"\n");
    o_str("  "); o_str(verb); o_str(" -L <url>          follow redirects (default)\n");
}

static void download(const char *verb, const char *a1, const char *rest) {
    char url[196], dest[128], cookie[200], extra_h[200];
    char buf[500];
    const char *tok[20];
    int nt, i, have_url = 0, resume = 0, background = 0, follow = 1;
    char *w, *we;
    static char body[WWW_BODY_MAX];
    static char merged[64 * 1024];
    int st = 0;
    uint32_t n;
    char range[40];

    url[0] = dest[0] = cookie[0] = extra_h[0] = range[0] = 0;
    w = buf; we = buf + sizeof(buf);
    if (a1 && a1[0]) { while (*a1 && w < we - 1) *w++ = *a1++; }
    if (rest && rest[0]) {
        if (w > buf && w < we - 1) *w++ = ' ';
        while (*rest && w < we - 1) *w++ = *rest++;
    }
    *w = 0;
    nt = tok_line(buf, tok, 20);
    if (nt == 0) { dl_usage(verb); return; }

    for (i = 0; i < nt; i++) {
        if (eqs(tok[i], "-O") || eqs(tok[i], "-o") || eqs(tok[i], "--output")) {
            if (i + 1 < nt) cpy(dest, tok[++i], (int)sizeof(dest));
            continue;
        }
        if (eqs(tok[i], "-c") || eqs(tok[i], "--continue")) { resume = 1; continue; }
        if (eqs(tok[i], "-b") || eqs(tok[i], "--background")) { background = 1; continue; }
        if (eqs(tok[i], "-L") || eqs(tok[i], "--location")) { follow = 1; continue; }
        if ((eqs(tok[i], "--cookie") || eqs(tok[i], "--header")) && i + 1 < nt) {
            if (eqs(tok[i], "--cookie")) cpy(cookie, tok[++i], (int)sizeof(cookie));
            else cpy(extra_h, tok[++i], (int)sizeof(extra_h));
            continue;
        }
        if (looks_url(tok[i]) && !have_url) {
            cpy(url, tok[i], (int)sizeof(url));
            have_url = 1;
            continue;
        }
        /* legacy: wget <url> /path */
        if (have_url && tok[i][0] == '/' && !dest[0]) {
            cpy(dest, tok[i], (int)sizeof(dest));
            continue;
        }
        if (!have_url && tok[i][0] != '-') {
            cpy(url, tok[i], (int)sizeof(url));
            have_url = 1;
        }
    }
    if (!have_url) { dl_usage(verb); return; }
    ensure_url(url, (int)sizeof(url));
    if (commands_verbose()) {
        commands_v("download into RXFS (64 KiB/file). needs www on");
        commands_v(url);
    }

    if (!dest[0]) {
        char base[64];
        int di = 0;
        wget_basename(url, base, (int)sizeof(base));
        dest[di++] = '/'; dest[di++] = 't'; dest[di++] = 'm'; dest[di++] = 'p'; dest[di++] = '/';
        cpy(dest + di, base, (int)sizeof(dest) - di);
    }

    if (!www_enable()) {
        o_str("! "); o_str(verb); o_str(": ");
        o_str(www_last_err()[0] ? www_last_err() : "need a NIC"); o_nl();
        return;
    }
    if (background)
        o_str("(no fork — transfer runs to completion)\n");

    if (resume) {
        int sz = vfs_size(dest);
        if (sz > 0) {
            char *d = range;
            const char *p = "Range: bytes=";
            uint32_t v = (uint32_t)sz;
            char t[12]; int k = 0;
            while (*p) *d++ = *p++;
            if (!v) t[k++] = '0';
            while (v && k < 11) { t[k++] = (char)('0' + (v % 10)); v /= 10; }
            while (k) *d++ = t[--k];
            *d++ = '-'; *d = 0;
            o_str("resume from byte "); o_dec((uint64_t)sz); o_nl();
        }
    }

    apply_headers(cookie, range[0] ? range : 0, extra_h);
    n = www_http_request_ex("GET", url, 0, 0, 0, body, sizeof(body), &st, follow);
    if (resume && st == 206 && n) {
        uint32_t m = resume_merge(dest, body, n, merged, sizeof(merged), st);
        o_str("HTTP "); o_dec((uint64_t)st); o_str("  ");
        o_dec(m ? m : n);
        o_str(" bytes  "); o_str(www_last_url()[0] ? www_last_url() : url); o_nl();
        if (m) save_body(dest, merged, m);
        else save_body(dest, body, n);
    } else {
        o_str("HTTP "); o_dec((uint64_t)st); o_str("  "); o_dec(n);
        o_str(" bytes  "); o_str(www_last_url()[0] ? www_last_url() : url); o_nl();
        if (!n) { o_str("! "); o_str(verb); o_str(": empty body\n"); }
        else save_body(dest, body, n);
    }
    www_clear_extra_hdr();
}

void netcli_wdl(const char *a1, const char *rest) { download("wdl", a1, rest); }
void netcli_wget(const char *a1, const char *rest) { download("wget", a1, rest); }

static void hex2(uint16_t v, char *out) {
    static const char H[] = "0123456789abcdef";
    out[0] = H[(v >> 12) & 0xf]; out[1] = H[(v >> 8) & 0xf];
    out[2] = H[(v >> 4) & 0xf];  out[3] = H[v & 0xf]; out[4] = 0;
}

void netcli_ipconf(void) {
    const ip_cfg_t *c;
    net_pci_dev_t list[NET_PCI_MAX];
    int n, i;
    char b[20], id[12];

    commands_v("bound NIC only; Wi-Fi chips in `nics` are not driven");
    o_str("ipconf — network interfaces\n \n");
    o_str("bound:\n  iface0  ");
    o_str(net_present() ? net_driver_name() : "(none)");
    o_nl();
    if (net_present()) {
        char mac[24];
        wired_mac_str(mac, sizeof(mac));
        o_str("    state    up\n");
        o_str("    ether    "); o_str(mac); o_nl();
    } else {
        o_str("    state    down  (no NIC driver bound)\n");
    }

    if (ipstack_configured()) {
        c = ipstack_cfg();
        o_str("    inet     "); ip_fmt(c->ip, b, sizeof(b)); o_str(b); o_nl();
        o_str("    netmask  "); ip_fmt(c->mask, b, sizeof(b)); o_str(b); o_nl();
        o_str("    gateway  "); ip_fmt(c->gw, b, sizeof(b)); o_str(b); o_nl();
        o_str("    dns      "); ip_fmt(c->dns, b, sizeof(b)); o_str(b); o_nl();
    } else {
        o_str("    inet     (none — run `www on` or `www config ...`)\n");
    }
    o_str("    www      "); o_str(www_enabled() ? "ON" : "OFF"); o_nl();

    o_str(" \nPCI network controllers:\n");
    n = net_pci_list(list, NET_PCI_MAX);
    if (!n) o_str("  (none)\n");
    for (i = 0; i < n; i++) {
        hex2(list[i].vendor, id);
        o_str("  "); o_str(id); o_str(":");
        hex2(list[i].device, id); o_str(id);
        o_str("  class "); o_dec(list[i].cls); o_str("/"); o_dec(list[i].subcls);
        o_str("  "); o_str(list[i].note); o_nl();
    }
    o_str(" \nSee also: nics, www status, man ipconf\n");
}
