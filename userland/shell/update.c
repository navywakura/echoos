/*
 * EchOS update channel.
 *
 * What this can do today:
 *   - check a published INDEX (live HTTP, or the verified embedded catalog)
 *   - download a text .rxpatch (file replacements + wallpaper recipes)
 *   - apply it to RXFS, persist if a disk is formatted, reboot
 *
 * What it cannot do (said plainly, not faked):
 *   - rewrite the live USB ISO9660 image (read-only)
 *   - apply a kernel ELF without a writable boot volume
 *   - talk HTTPS to Vercel until the TLS client lands (same as packages)
 *
 * kind=patch  → files + wallpaper recipes (this code)
 * kind=iso    → tell the user to flash the new metal ISO
 */
#include "update.h"
#include "version.h"
#include "vfs.h"
#include "rxfs.h"
#include "disk.h"
#include "www.h"
#include "gui.h"
#include "rogex_core.h"
#include "commands.h"

#define UPD_BODY_MAX  WWW_BODY_MAX

/* ---- tiny output (same contract as pkg.c) -------------------------------- */
static char  *o_buf;
static size_t o_max, o_len;
static void o_reset(char *buf, size_t max) {
    o_buf = buf; o_max = max; o_len = 0;
    if (max) buf[0] = '\0';
}
static void u_str(const char *s) {
    if (!s) return;
    while (*s && o_len + 1 < o_max) o_buf[o_len++] = *s++;
    if (o_max) o_buf[o_len] = '\0';
}
static void u_nl(void) { u_str("\n"); }

static bool eqs(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
static void scopy(char *d, const char *s, size_t max) {
    size_t i = 0;
    if (!s) { if (max) d[0] = '\0'; return; }
    while (s[i] && i + 1 < max) { d[i] = s[i]; i++; }
    d[i] = '\0';
}
static bool starts(const char *s, const char *p) {
    if (!s || !p) return false;
    while (*p) { if (*s != *p) return false; s++; p++; }
    return true;
}

/* ---- version numbers ----------------------------------------------------- */
static int ver_code(const char *s) {
    int a = 0, b = 0, c = 0, part = 0, n = 0;
    if (!s) return 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); }
        else if (*s == '.') {
            if (part == 0) a = n; else if (part == 1) b = n;
            n = 0; part++;
        } else break;
    }
    if (part == 0) a = n;
    else if (part == 1) b = n;
    else c = n;
    return a * 10000 + b * 100 + c;
}

void update_runtime_version(char *out, size_t max) {
    uint32_t len = 0;
    const char *d = vfs_read("/etc/os-release", &len);
    if (d && len) {
        const char *p = d, *end = d + len;
        while (p + 8 < end) {
            if (starts(p, "VERSION=")) {
                p += 8;
                if (*p == '"') p++;
                char *o = out; char *e = out + max;
                while (p < end && *p && *p != '\n' && *p != '"' && o < e - 1)
                    *o++ = *p++;
                *o = '\0';
                if (out[0]) return;
            }
            while (p < end && *p != '\n') p++;
            if (p < end && *p == '\n') p++;
        }
    }
    scopy(out, ECHOS_VERSION, max);
}

/* ---- persist ------------------------------------------------------------- */
static void persist(void) {
    if (disk_present() && rxfs_disk_has_image((struct blockdev *)disk_dev()))
        rxfs_save((struct blockdev *)disk_dev());
}

/* ---- wallpaper recipes (text, fits in 4 KiB; no new ISO needed) ---------- */
static int hexdig(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool parse_hex6(const char *s, uint32_t *out) {
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (int i = 0; i < 6; i++) {
        int d = hexdig(s[i]);
        if (d < 0) return false;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}
static const char *after_key(const char *text, const char *key) {
    const char *p = text;
    size_t klen = k_strlen(key);
    while (*p) {
        if (starts(p, key)) {
            p += klen;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return NULL;
}

static bool apply_wallpaper_text(const char *text) {
    if (!text || !text[0]) return false;
    /* First token is the style name. "default" restores the baked QOI. */
    char name[32];
    size_t i = 0;
    const char *p = text;
    while (*p == ' ' || *p == '\n' || *p == '\t') p++;
    while (*p && *p != '\n' && *p != ' ' && i + 1 < sizeof(name)) name[i++] = *p++;
    name[i] = '\0';
    if (!name[0]) return false;
    gui_wallpaper_set_name(name);
    if (eqs(name, "default")) return gui_wallpaper_restore_default();
    for (int i = 0; i < gui_wp_count(); i++)
        if (eqs(name, gui_wp_name_at(i)))
            return gui_wallpaper_load_index(i);

    uint32_t from = 0x0a1628, to = 0x3d7ea6, accent = 0x7ec8e3, grain = 8;
    const char *v;
    if ((v = after_key(text, "from "))) parse_hex6(v, &from);
    if ((v = after_key(text, "to "))) parse_hex6(v, &to);
    if ((v = after_key(text, "accent "))) parse_hex6(v, &accent);
    if ((v = after_key(text, "grain "))) {
        uint32_t g = 0;
        while (*v >= '0' && *v <= '9') { g = g * 10 + (uint32_t)(*v - '0'); v++; }
        grain = g;
    }
    /* Named built-ins when the file is just a name and no recipe keys. */
    if (!after_key(text, "from ") && !after_key(text, "kind ")) {
        if (eqs(name, "aurora"))   { from = 0x0a1628; to = 0x3d7ea6; accent = 0x7ec8e3; grain = 8; }
        else if (eqs(name, "ember"))    { from = 0x1a0a08; to = 0xc04018; accent = 0xf0a060; grain = 10; }
        else if (eqs(name, "midnight")) { from = 0x04060e; to = 0x1a2450; accent = 0x6a80c0; grain = 6; }
        else if (eqs(name, "solar"))    { from = 0x2a0a08; to = 0xe8a020; accent = 0xffd27a; grain = 10; }
    }
    return gui_wallpaper_fill_gradient(from, to, accent, grain);
}

static void apply_wallpaper_pref(void) {
    uint32_t len = 0;
    const char *d = vfs_read("/etc/wallpaper", &len);
    if (!d || !len) return;
    char tmp[RXFS_SIZE + 1];
    uint32_t n = len < RXFS_SIZE ? len : RXFS_SIZE;
    k_memcpy(tmp, d, n);
    tmp[n] = '\0';
    apply_wallpaper_text(tmp);
}

/* ---- embedded catalog (offline / HTTPS fallback, like packages) ---------- */
static const char PATCH_601[] =
    "rxpatch 1\n"
    "id 6.0.1\n"
    "from 6.0.0\n"
    "to 6.0.1\n"
    "kind patch\n"
    "reboot yes\n"
    "summary Extra wallpaper (solar) + version stamp\n"
    "\n"
    "file /etc/wallpaper\n"
    "---\n"
    "solar\n"
    "kind gradient\n"
    "from 2a0a08\n"
    "to e8a020\n"
    "accent ffd27a\n"
    "grain 10\n"
    "---\n"
    "file /etc/motd\n"
    "---\n"
    "EchOS 6.0.1 — solar wallpaper pack.\n"
    "Check for more: rx update check\n"
    "---\n";

typedef struct {
    const char *id;
    const char *from;
    const char *to;
    const char *kind;       /* "patch" or "iso" */
    const char *summary;
    const char *body;       /* NULL for kind=iso */
} upd_rel;

static const char PATCH_651[] =
    "rxpatch 1\n"
    "id 6.5.1\n"
    "from 6.5.0\n"
    "to 6.5.1\n"
    "kind patch\n"
    "reboot yes\n"
    "summary Ember wallpaper + 6.5 notes\n"
    "\n"
    "file /etc/wallpaper\n"
    "---\n"
    "ember\n"
    "---\n"
    "file /etc/motd\n"
    "---\n"
    "EchOS 6.5.1 — ember wallpaper. nics / browse / fortune\n"
    "---\n";

static const char PATCH_701[] =
    "rxpatch 1\n"
    "id 7.0.1\n"
    "from 7.0.0\n"
    "to 7.0.1\n"
    "kind patch\n"
    "reboot yes\n"
    "summary Ember wallpaper\n"
    "\n"
    "file /etc/wallpaper\n"
    "---\n"
    "ember\n"
    "---\n"
    "file /etc/motd\n"
    "---\n"
    "EchOS 7.0.1 — ember wallpaper.\n"
    "---\n";

static const upd_rel g_rels[] = {
    { "6.0.1", "6.0.0", "6.0.1", "patch",
      "Extra wallpaper (solar) + version stamp", PATCH_601 },
    { "6.5.1", "6.5.0", "6.5.1", "patch",
      "Ember wallpaper + 6.5 notes", PATCH_651 },
    { "7.0.1", "7.0.0", "7.0.1", "patch",
      "Ember wallpaper", PATCH_701 },
};
#define UPD_N  ((int)(sizeof(g_rels) / sizeof(g_rels[0])))

static char g_channel[128];
static char g_live_idx[UPD_BODY_MAX];
static bool g_live_idx_ok;

static const upd_rel *next_for(const char *cur) {
    int c = ver_code(cur);
    const upd_rel *best = NULL;
    int best_c = 0;
    for (int i = 0; i < UPD_N; i++) {
        int t = ver_code(g_rels[i].to);
        if (t > c && (!best || t < best_c)) { best = &g_rels[i]; best_c = t; }
    }
    return best;
}

/* ---- live INDEX (HTTP only; HTTPS falls back to the embedded catalog) ---- */
static uint32_t fetch_channel(const char *path, char *out, uint32_t outmax, bool *live) {
    if (live) *live = false;
    if (!www_enabled() || !www_ready()) return 0;
    char url[256];
    const char *base = g_channel;
    uint32_t bl = 0; while (base[bl]) bl++;
    while (bl && base[bl - 1] == '/') bl--;
    uint32_t n = 0;
    for (uint32_t i = 0; i < bl && n + 1 < sizeof(url); i++) url[n++] = base[i];
    if (n + 1 < sizeof(url)) url[n++] = '/';
    if (path[0] == '/') path++;
    while (*path && n + 1 < sizeof(url)) url[n++] = *path++;
    url[n] = '\0';
    int st = 0;
    uint32_t len = www_http_get(url, out, outmax, &st);
    if (len && st >= 200 && st < 300) {
        if (live) *live = true;
        return len;
    }
    return 0;
}

static const char *json_str(const char *j, const char *key, char *out, size_t max) {
    if (!j || !key) return NULL;
    char pat[40];
    char *d = pat;
    *d++ = '"';
    const char *k = key; while (*k && d < pat + sizeof(pat) - 4) *d++ = *k++;
    *d++ = '"'; *d++ = ':'; *d = '\0';
    const char *p = j;
    while (*p) {
        if (starts(p, pat)) {
            p += k_strlen(pat);
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') p++;
            size_t i = 0;
            while (*p && *p != '"' && i + 1 < max) out[i++] = *p++;
            out[i] = '\0';
            return out;
        }
        p++;
    }
    return NULL;
}

/* ---- patch apply --------------------------------------------------------- */
static bool write_os_release(const char *ver) {
    char buf[240];
    char *d = buf, *e = buf + sizeof(buf) - 1;
    const char *p;
    p = "NAME=EchOS\nVERSION="; while (*p && d < e) *d++ = *p++;
    p = ver; while (*p && d < e) *d++ = *p++;
    p = "\nID=rxos\nPRETTY_NAME=EchOS "; while (*p && d < e) *d++ = *p++;
    p = ver; while (*p && d < e) *d++ = *p++;
    p = "\nCHANNEL="; while (*p && d < e) *d++ = *p++;
    p = g_channel; while (*p && d < e) *d++ = *p++;
    *d++ = '\n'; *d = '\0';
    return vfs_write("/etc/os-release", buf, (uint32_t)(d - buf));
}

static bool write_applied(const char *ver) {
    vfs_mkdir("/var");
    vfs_mkdir("/var/db");
    vfs_mkdir("/var/db/update");
    return vfs_write("/var/db/update/applied", ver, (uint32_t)k_strlen(ver));
}

/* Apply a .rxpatch body. Writes each `file <path>` section, then stamps
   VERSION and applied. Wallpaper files take effect immediately. */
static bool apply_patch_body(const char *body, const char *to_ver) {
    if (!body || !starts(body, "rxpatch")) return false;
    const char *p = body;
    while (*p) {
        if (starts(p, "file ")) {
            p += 5;
            char path[RXFS_NAME];
            size_t i = 0;
            while (*p && *p != '\n' && *p != '\r' && i + 1 < sizeof(path))
                path[i++] = *p++;
            path[i] = '\0';
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            if (!starts(p, "---")) return false;
            p += 3;
            if (*p == '\r') p++;
            if (*p == '\n') p++;
            const char *start = p;
            const char *end = p;
            while (*p) {
                if (p[0] == '-' && p[1] == '-' && p[2] == '-' &&
                    (p[3] == '\n' || p[3] == '\r' || p[3] == '\0')) {
                    end = p;
                    p += 3;
                    if (*p == '\r') p++;
                    if (*p == '\n') p++;
                    break;
                }
                p++;
                end = p;
            }
            uint32_t len = (uint32_t)(end - start);
            if (len && end[-1] == '\n') len--;
            if (len && start[len - 1] == '\r') len--;
            if (!vfs_write(path, start, len)) return false;
            if (eqs(path, "/etc/wallpaper")) {
                char tmp[RXFS_SIZE + 1];
                uint32_t n = len < RXFS_SIZE ? len : RXFS_SIZE;
                k_memcpy(tmp, start, n);
                tmp[n] = '\0';
                apply_wallpaper_text(tmp);
            }
            continue;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    if (to_ver && to_ver[0]) {
        write_os_release(to_ver);
        write_applied(to_ver);
    }
    persist();
    return true;
}

static bool stage_body(const char *body) {
    vfs_mkdir("/var");
    vfs_mkdir("/var/cache");
    vfs_mkdir("/var/cache/updates");
    return vfs_write("/var/cache/updates/pending", body, (uint32_t)k_strlen(body));
}

static const char *staged_body(uint32_t *len) {
    return vfs_read("/var/cache/updates/pending", len);
}

static void parse_hdr(const char *body, char *to, size_t tmax,
                      char *kind, size_t kmax, char *sum, size_t smax) {
    to[0] = kind[0] = sum[0] = '\0';
    const char *p = body;
    while (*p) {
        if (starts(p, "to ")) {
            p += 3; size_t i = 0;
            while (*p && *p != '\n' && i + 1 < tmax) to[i++] = *p++;
            to[i] = '\0';
        } else if (starts(p, "kind ")) {
            p += 5; size_t i = 0;
            while (*p && *p != '\n' && i + 1 < kmax) kind[i++] = *p++;
            kind[i] = '\0';
        } else if (starts(p, "summary ")) {
            p += 8; size_t i = 0;
            while (*p && *p != '\n' && i + 1 < smax) sum[i++] = *p++;
            sum[i] = '\0';
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (starts(p, "file ")) break;
    }
}

/* ---- public boot --------------------------------------------------------- */
void update_init(void) {
    scopy(g_channel, UPDATE_CHANNEL_URL, sizeof(g_channel));
    vfs_mkdir("/etc");
    vfs_mkdir("/var");
    vfs_mkdir("/var/db");
    vfs_mkdir("/var/db/update");
    vfs_mkdir("/var/cache");
    vfs_mkdir("/var/cache/updates");
    if (vfs_size("/etc/os-release") < 0)
        write_os_release(ECHOS_VERSION);
}

void update_boot_apply(void) {
    uint32_t plen = 0;
    const char *pend = staged_body(&plen);
    char cur[24];
    update_runtime_version(cur, sizeof(cur));
    uint32_t alen = 0;
    const char *applied = vfs_read("/var/db/update/applied", &alen);
    bool need = false;
    if (pend && plen) {
        char to[24], kind[16], sum[80];
        parse_hdr(pend, to, sizeof(to), kind, sizeof(kind), sum, sizeof(sum));
        if (to[0] && ver_code(to) > ver_code(cur)) need = true;
        if (applied && alen) {
            char a[24];
            uint32_t n = alen < 23 ? alen : 23;
            k_memcpy(a, applied, n); a[n] = '\0';
            if (to[0] && eqs(a, to)) need = false;
        }
        if (need) {
            char body[UPD_BODY_MAX];
            uint32_t n = plen < UPD_BODY_MAX - 1 ? plen : UPD_BODY_MAX - 1;
            k_memcpy(body, pend, n); body[n] = '\0';
            if (apply_patch_body(body, to[0] ? to : NULL)) {
                console_write("[echOS] update: applied ");
                console_write(to[0] ? to : "?");
                console_write(" (staged patch)\n");
                vfs_delete("/var/cache/updates/pending");
                persist();
            } else {
                klog_warn("update: staged patch failed to apply");
            }
        }
    }
    apply_wallpaper_pref();
}

/* ---- commands ------------------------------------------------------------ */
static void cmd_status(void) {
    char ver[24];
    update_runtime_version(ver, sizeof(ver));
    u_str("EchOS update channel\n \n");
    u_str("  running:  "); u_str(ver); u_nl();
    u_str("  iso:      "); u_str(ECHOS_VERSION); u_str(" (compiled kernel)\n");
    u_str("  channel:  "); u_str(g_channel); u_nl();
    u_str("  www:      "); u_str(www_enabled() ? "ON" : "OFF"); u_nl();
    u_str("  wallpaper:"); u_str(gui_wallpaper_name());
    u_str(" ("); u_str(gui_wallpaper_status()); u_str(")\n");
    uint32_t alen = 0;
    const char *ap = vfs_read("/var/db/update/applied", &alen);
    u_str("  applied:  ");
    if (ap && alen) {
        char a[24]; uint32_t n = alen < 23 ? alen : 23;
        k_memcpy(a, ap, n); a[n] = '\0';
        u_str(a);
    } else u_str("(none)");
    u_nl();
    uint32_t plen = 0;
    u_str("  staged:   ");
    u_str(staged_body(&plen) ? "yes (/var/cache/updates/pending)" : "no");
    u_nl();
    if (!disk_present())
        u_str("  persist:  amnesic (no disk — apply lasts this session)\n");
    else if (rxfs_disk_has_image((struct blockdev *)disk_dev()))
        u_str("  persist:  disk will keep patches across reboot\n");
    else
        u_str("  persist:  disk present but not formatted (`install`)\n");
}

static void cmd_channel(const char *url) {
    if (url && url[0]) {
        scopy(g_channel, url, sizeof(g_channel));
        g_live_idx_ok = false;
        u_str("update channel set to "); u_str(g_channel); u_nl();
        return;
    }
    u_str("update channel\n \n");
    u_str("  host: "); u_str(UPDATE_CHANNEL_HOST); u_nl();
    u_str("  path: "); u_str(UPDATE_CHANNEL_PATH); u_nl();
    u_str("  url:  "); u_str(g_channel); u_nl();
    u_str("  note: HTTPS uses the verified local catalog until TLS lands.\n");
    u_str("        Point at an http:// mirror for live INDEX + .rxpatch.\n");
}

static void print_rel(const upd_rel *r) {
    u_str("  "); u_str(r->from); u_str(" -> "); u_str(r->to);
    u_str("  ["); u_str(r->kind); u_str("]  "); u_str(r->summary); u_nl();
}

static void cmd_check(void) {
    char ver[24];
    update_runtime_version(ver, sizeof(ver));
    u_str("update channel\n");
    u_str("  running "); u_str(ver); u_nl();

    bool live = false;
    char live_cur[24];
    live_cur[0] = '\0';
    if (fetch_channel("INDEX.json", g_live_idx, sizeof(g_live_idx), &live) && live) {
        g_live_idx_ok = true;
        json_str(g_live_idx, "current", live_cur, sizeof(live_cur));
        u_str("  live INDEX: current ");
        u_str(live_cur[0] ? live_cur : "?");
        u_nl();
    } else {
        u_str("  catalog: verified local (www off, or HTTPS TLS pending)\n");
    }

    const upd_rel *n = next_for(ver);
    if (!n) {
        u_str("  up to date — no file patch waiting\n");
        if (live_cur[0] && ver_code(live_cur) > ver_code(ver)) {
            u_str("  live current is "); u_str(live_cur);
            u_str(" — fetch that .rxpatch from an HTTP mirror\n");
        }
        return;
    }
    u_str("  available:\n");
    print_rel(n);
    if (eqs(n->kind, "iso")) {
        u_str("  this is a kernel/ISO update. The live USB is ISO9660\n");
        u_str("  (read-only). Flash the new metal ISO on the host:\n");
        u_str("    make iso-metal && dd if=build/EchOS-");
        u_str(n->to); u_str("-metal.iso of=/dev/sdX\n");
        return;
    }
    u_str("  next: rx update download   then   rx update apply\n");
    u_str("        or: rx update now    (apply + reboot)\n");
}

static bool get_patch_for(const upd_rel *r, char *out, uint32_t outmax) {
    if (!r) return false;
    bool live = false;
    char fname[40];
    scopy(fname, r->id, sizeof(fname));
    size_t L = k_strlen(fname);
    if (L + 9 < sizeof(fname)) {
        fname[L] = '.'; fname[L+1]='r'; fname[L+2]='x'; fname[L+3]='p';
        fname[L+4]='a'; fname[L+5]='t'; fname[L+6]='c'; fname[L+7]='h';
        fname[L+8] = '\0';
    }
    uint32_t n = fetch_channel(fname, out, outmax, &live);
    if (n && live && starts(out, "rxpatch")) return true;
    if (r->body) {
        scopy(out, r->body, outmax);
        return true;
    }
    return false;
}

static void cmd_download(void) {
    char ver[24];
    update_runtime_version(ver, sizeof(ver));
    const upd_rel *n = next_for(ver);
    if (!n) { u_str("nothing to download — already up to date\n"); return; }
    if (eqs(n->kind, "iso")) {
        u_str("6.x ISO updates are not downloaded in-OS (USB is read-only).\n");
        u_str("Flash the published metal ISO on the host.\n");
        return;
    }
    char body[UPD_BODY_MAX];
    if (!get_patch_for(n, body, sizeof(body))) {
        u_str("download failed\n"); return;
    }
    if (!stage_body(body)) { u_str("could not stage patch (fs full?)\n"); return; }
    persist();
    u_str("downloaded "); u_str(n->id); u_str(".rxpatch\n");
    u_str("staged at /var/cache/updates/pending\n");
    u_str("next: rx update apply   (or rx update now to apply + reboot)\n");
}

static bool do_apply(char *err, size_t errmax) {
    uint32_t plen = 0;
    const char *pend = staged_body(&plen);
    char body[UPD_BODY_MAX];
    char to[24], kind[16], sum[80];
    to[0] = kind[0] = sum[0] = '\0';
    if (pend && plen) {
        uint32_t n = plen < UPD_BODY_MAX - 1 ? plen : UPD_BODY_MAX - 1;
        k_memcpy(body, pend, n); body[n] = '\0';
    } else {
        char ver[24];
        update_runtime_version(ver, sizeof(ver));
        const upd_rel *r = next_for(ver);
        if (!r) { scopy(err, "nothing to apply", errmax); return false; }
        if (eqs(r->kind, "iso")) {
            scopy(err, "iso update — flash a new metal ISO on the host", errmax);
            return false;
        }
        if (!get_patch_for(r, body, sizeof(body))) {
            scopy(err, "could not load patch", errmax); return false;
        }
        stage_body(body);
    }
    parse_hdr(body, to, sizeof(to), kind, sizeof(kind), sum, sizeof(sum));
    if (eqs(kind, "iso")) {
        scopy(err, "iso update — cannot apply in-OS", errmax); return false;
    }
    if (!apply_patch_body(body, to[0] ? to : NULL)) {
        scopy(err, "apply failed (bad patch or fs full)", errmax); return false;
    }
    vfs_delete("/var/cache/updates/pending");
    persist();
    err[0] = '\0';
    return true;
}

static void cmd_apply(void) {
    char err[80];
    char before[24];
    update_runtime_version(before, sizeof(before));
    if (!do_apply(err, sizeof(err))) { u_str(err); u_nl(); return; }
    char after[24];
    update_runtime_version(after, sizeof(after));
    u_str("applied "); u_str(after[0] ? after : "?"); u_nl();
    u_str("was "); u_str(before); u_str(" -> "); u_str(after); u_nl();
    u_str("wallpaper: "); u_str(gui_wallpaper_name()); u_nl();
    if (disk_present() && rxfs_disk_has_image((struct blockdev *)disk_dev()))
        u_str("saved to disk — reboot to start on the new version\n");
    else
        u_str("amnesic session: reboot without a disk loses this patch\n");
}

static void cmd_now(void) {
    cmd_apply();
    if (starts(o_buf, "applied ")) {
        u_str("rebooting to finish the update...\n");
        system_reboot();
    }
}

static void cmd_help(void) {
    u_str("rx update — check the channel, download a patch, apply, reboot\n \n");
    u_str("  rx update check       see if a newer patch or ISO is out\n");
    u_str("  rx update download    fetch the next .rxpatch into the vault\n");
    u_str("  rx update apply       write files + wallpaper, persist if disk\n");
    u_str("  rx update now         apply and reboot\n");
    u_str("  rx update status      running version, staged patch, persist\n");
    u_str("  rx update channel     show / set the channel URL\n");
    u_str("  wallpaper [name]      default | aurora | ember | midnight | solar\n \n");
    u_str("File patches (apps, wallpaper recipes, notes) apply in-OS.\n");
    u_str("Kernel/ISO bugfixes publish as kind=iso — flash a new USB.\n");
    u_str("Channel: "); u_str(UPDATE_CHANNEL_URL); u_nl();
}

static void cmd_wallpaper(const char *name) {
    if (!name || !name[0] || eqs(name, "list")) {
        u_str("wallpapers:\n");
        u_str("  default   baked QOI (ISO)\n");
        u_str("  aurora    cool blue gradient (recipe)\n");
        u_str("  ember     warm red gradient (recipe)\n");
        u_str("  midnight  deep indigo (recipe)\n");
        u_str("  solar     6.0.1 patch pack (recipe)\n");
        u_str("packed JPEG papers:\n");
        for (int i = 0; i < gui_wp_count(); i++) {
            u_str("  "); u_str(gui_wp_name_at(i)); u_nl();
        }
        u_str("current: "); u_str(gui_wallpaper_name()); u_nl();
        u_str("set: wallpaper <name>\n");
        return;
    }
    for (int i = 0; i < gui_wp_count(); i++) {
        if (eqs(name, gui_wp_name_at(i))) {
            if (!gui_wallpaper_load_index(i)) {
                u_str("decode failed\n"); return;
            }
            vfs_write("/etc/wallpaper", name, (uint32_t)k_strlen(name));
            persist();
            u_str("wallpaper: "); u_str(gui_wallpaper_name()); u_nl();
            return;
        }
    }
    char rec[160];
    char *d = rec, *e = rec + sizeof(rec) - 1;
    const char *p = name;
    while (*p && d < e) *d++ = *p++;
    *d++ = '\n'; *d = '\0';
    if (!apply_wallpaper_text(rec)) {
        u_str("unknown wallpaper: "); u_str(name); u_nl();
        u_str("try: wallpaper list\n");
        return;
    }
    vfs_write("/etc/wallpaper", rec, (uint32_t)(d - rec));
    persist();
    u_str("wallpaper: "); u_str(gui_wallpaper_name()); u_nl();
}

void update_cmd(const char *verb, const char *sub, const char *arg,
                char *out, size_t outmax) {
    o_reset(out, outmax);
    if (eqs(verb, "wallpaper")) { cmd_wallpaper(sub); return; }
    if (!sub || !sub[0] || eqs(sub, "help") || eqs(sub, "--help")) {
        cmd_help(); return;
    }
    if (eqs(sub, "check") || eqs(sub, "upgrade")) { cmd_check(); return; }
    if (eqs(sub, "download") || eqs(sub, "fetch") || eqs(sub, "get")) {
        cmd_download(); return;
    }
    if (eqs(sub, "apply")) { cmd_apply(); return; }
    if (eqs(sub, "now") || eqs(sub, "upgrade-now")) { cmd_now(); return; }
    if (eqs(sub, "status")) { cmd_status(); return; }
    if (eqs(sub, "channel")) { cmd_channel(arg); return; }
    u_str("unknown: rx update "); u_str(sub); u_nl();
    u_str("try: rx update help\n");
}
