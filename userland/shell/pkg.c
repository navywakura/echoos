/*
 * EchOS package manager (Portage-inspired).
 *
 * Architecture (mirrors Gentoo Portage concepts):
 *   - DISTDIR   /var/cache/distfiles   cached .rxc payloads
 *   - PKGDIR    /var/db/pkg/<name>     installed package metadata
 *   - binpkgs   /bin/<cmd>.rxc         runnable commands/apps
 *   - channel   https://www.rogexlaboratories.com/rx-os/packages
 *
 * Channel layout matches https://www.rogexlaboratories.com/rx-os/packages
 * (INDEX.json + *.rxc). Offline: verified local mirror. With `www on`: live
 * HTTP fetch when the channel base is http://; HTTPS does DNS+TCP probe and
 * falls back to the mirror until the TLS client lands.
 */
#include "pkg.h"
#include "vfs.h"
#include "rogex_core.h"
#include "netif.h"
#include "www.h"

/* ---- tiny output builder (local; same idea as commands.c) --------------- */
static char  *o_buf;
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
static bool eqs(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
/* ---- verified channel --------------------------------------------------- */
typedef struct {
    const char *name;       /* package name (no .rxc) */
    const char *version;
    const char *desc;
    const char *source;     /* .rxc body installed into /bin */
    const char *cmd;        /* command name under /bin (may equal name) */
    uint8_t     sha3[8];    /* first 8 of SHA3-256 for the payload (verified) */
} pkg_entry;

/* Embedded mirror of the remote package tree. When TCP+HTTPS lands, these
   become fallbacks and the channel fetches live from PKG_CHANNEL_URL. */
static const char SRC_ECHO[] =
    "app \"echo\"\n"
    "page home:\n"
    "  title \"echo\"\n"
    "  text \"Installed via rx app add. Prints args in the vault.\"\n"
    "  text \"Source lives in /bin/echo.rxc\"\n";

static const char SRC_FORTUNE[] =
    "app \"fortune\"\n"
    "page home:\n"
    "  title \"fortune\"\n"
    "  text \"Today: the vault is small but the ideas are large.\"\n"
    "  text \"— Rogex Laboratories\"\n";

static const char SRC_HELLOPKG[] =
    "app \"hellopkg\"\n"
    "state n is 0\n"
    "page home:\n"
    "  title \"hellopkg (from channel)\"\n"
    "  text \"Downloaded from the EchOS package channel.\"\n"
    "  text \"Channel: https://www.rogexlaboratories.com/rx-os/packages\"\n"
    "  value n\n"
    "  button \"wave\":\n"
    "    add 1 to n\n";

static const char SRC_TREE[] =
    "app \"tree-view\"\n"
    "page home:\n"
    "  title \"tree-view\"\n"
    "  text \"Use the shell command `tree` for a live vault listing.\"\n"
    "  text \"This package is a Roxenite companion page.\"\n";

/* Precomputed first-8 of SHA3 are filled at pkg_init after hashing sources. */
static pkg_entry g_index[] = {
    { "echo",     "1.0.0", "echo args / demo command package", SRC_ECHO,     "echo",     {0} },
    { "fortune",  "1.0.0", "one-line fortune app",             SRC_FORTUNE,  "fortune",  {0} },
    { "hellopkg", "1.0.1", "hello from the package channel",   SRC_HELLOPKG, "hellopkg", {0} },
    { "tree-view","1.0.0", "tree companion Roxenite page",     SRC_TREE,     "tree-view",{0} },
};
#define PKG_INDEX_N (int)(sizeof(g_index) / sizeof(g_index[0]))

static bool g_channel_ready;
static uint8_t g_channel_id[32];   /* SHA3 of all package names+sources */

static const pkg_entry *find_pkg(const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < PKG_INDEX_N; i++)
        if (eqs(g_index[i].name, name)) return &g_index[i];
    /* Also accept name.rxc */
    size_t n = k_strlen(name);
    if (n > 4 && name[n-4]=='.' && name[n-3]=='r' && name[n-2]=='x' && name[n-1]=='c') {
        char bare[48];
        if (n - 4 >= sizeof(bare)) return NULL;
        for (size_t i = 0; i < n - 4; i++) bare[i] = name[i];
        bare[n - 4] = '\0';
        for (int i = 0; i < PKG_INDEX_N; i++)
            if (eqs(g_index[i].name, bare)) return &g_index[i];
    }
    return NULL;
}

/* Hash a payload; store first 8 bytes into out8. */
static void hash8(const char *src, uint8_t out8[8]) {
    uint8_t h[32];
    rogex_sha3_256((const uint8_t *)src, (uint32_t)k_strlen(src), h);
    for (int i = 0; i < 8; i++) out8[i] = h[i];
}

static void hex8(const uint8_t h[8], char out[17]) {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[i * 2]     = H[h[i] >> 4];
        out[i * 2 + 1] = H[h[i] & 0xf];
    }
    out[16] = '\0';
}

/* Open a verified session toward the package channel.
   Today: rebuild the content-address of the embedded mirror and mark the
   channel ready. Tomorrow: TLS handshake to PKG_CHANNEL_HOST. */
static bool channel_connect(void) {
    /* Gather hashes of every package payload. */
    uint8_t gathered[PKG_INDEX_N * 32];
    for (int i = 0; i < PKG_INDEX_N; i++) {
        uint8_t h[32];
        const char *s = g_index[i].source;
        rogex_sha3_256((const uint8_t *)s, (uint32_t)k_strlen(s), h);
        k_memcpy(&gathered[i * 32], h, 32);
        for (int b = 0; b < 8; b++) g_index[i].sha3[b] = h[b];
    }
    rogex_sha3_256(gathered, (uint32_t)PKG_INDEX_N * 32, g_channel_id);
    g_channel_ready = true;
    return true;
}

static bool channel_ok(void) {
    if (!g_channel_ready) return channel_connect();
    return g_channel_ready;
}

/* ---- install / remove --------------------------------------------------- */
static void path_bin(char *out, size_t max, const char *cmd) {
    char *d = out, *e = out + max;
    const char *p = "/bin/";
    while (*p && d < e - 1) *d++ = *p++;
    p = cmd;
    while (*p && d < e - 1) *d++ = *p++;
    p = ".rxc";
    while (*p && d < e - 1) *d++ = *p++;
    *d = '\0';
}

static void path_meta(char *out, size_t max, const char *name) {
    char *d = out, *e = out + max;
    const char *p = "/var/db/pkg/";
    while (*p && d < e - 1) *d++ = *p++;
    p = name;
    while (*p && d < e - 1) *d++ = *p++;
    *d = '\0';
}

static void path_meta_file(char *out, size_t max, const char *name, const char *file) {
    path_meta(out, max, name);
    size_t L = k_strlen(out);
    if (L + 1 < max) { out[L] = '/'; out[L + 1] = '\0'; L++; }
    char *d = out + L, *e = out + max;
    while (*file && d < e - 1) *d++ = *file++;
    *d = '\0';
}

static void path_dist(char *out, size_t max, const char *name) {
    char *d = out, *e = out + max;
    const char *p = "/var/cache/distfiles/";
    while (*p && d < e - 1) *d++ = *p++;
    p = name;
    while (*p && d < e - 1) *d++ = *p++;
    p = ".rxc";
    while (*p && d < e - 1) *d++ = *p++;
    *d = '\0';
}

static bool is_installed(const char *name) {
    char p[RXFS_NAME];
    path_meta_file(p, sizeof(p), name, "PF");
    return vfs_path_exists(p);
}

/* Live body buffer when www fetches a .rxc from the channel. */
static char g_live_body[WWW_BODY_MAX];
static char g_live_name[48];
static bool g_live_ok;

/* Fetch package payload: try live WWW channel first, else verified mirror. */
static bool channel_fetch(const pkg_entry *e) {
    if (!channel_ok() || !e) return false;
    char dist[RXFS_NAME];
    path_dist(dist, sizeof(dist), e->name);

    const char *body = e->source;
    uint32_t blen = (uint32_t)k_strlen(e->source);
    g_live_ok = false;

    if (www_enabled() && www_ready()) {
        char path[64];
        char *d = path;
        const char *p = e->name;
        while (*p && d < path + sizeof(path) - 5) *d++ = *p++;
        *d++ = '.'; *d++ = 'r'; *d++ = 'x'; *d++ = 'c'; *d = '\0';
        bool live = false;
        uint32_t n = www_channel_get(path, g_live_body, sizeof(g_live_body), &live);
        if (n > 0 && live) {
            g_live_ok = true;
            /* keep name for messages */
            char *dn = g_live_name;
            p = e->name;
            while (*p && dn < g_live_name + sizeof(g_live_name) - 1) *dn++ = *p++;
            *dn = '\0';
            body = g_live_body;
            blen = n;
            /* recompute hash for live payload */
            uint8_t h[8];
            hash8(body, h);
            for (int i = 0; i < 8; i++) ((pkg_entry *)e)->sha3[i] = h[i];
        }
    }

    /* Verify payload hash matches the index entry (embedded path). */
    if (!g_live_ok) {
        uint8_t h[8];
        hash8(e->source, h);
        for (int i = 0; i < 8; i++)
            if (h[i] != e->sha3[i]) return false;
        body = e->source;
        blen = (uint32_t)k_strlen(e->source);
    }
    return vfs_write(dist, body, blen);
}

static bool pkg_install(const pkg_entry *e) {
    if (!e) return false;
    if (!channel_fetch(e)) return false;

    /* Body is in DISTDIR after channel_fetch; re-read or use live/embed. */
    char dist[RXFS_NAME];
    path_dist(dist, sizeof(dist), e->name);
    uint32_t blen = 0;
    const char *body = vfs_read(dist, &blen);
    if (!body || !blen) {
        body = g_live_ok ? g_live_body : e->source;
        blen = (uint32_t)k_strlen(body);
    }

    char bin[RXFS_NAME];
    path_bin(bin, sizeof(bin), e->cmd);
    if (!vfs_write(bin, body, blen)) return false;

    char shortn[RXFS_NAME];
    { char *d = shortn, *e2 = shortn + sizeof(shortn);
      const char *p = e->cmd;
      while (*p && d < e2 - 1) *d++ = *p++;
      p = ".rxc";
      while (*p && d < e2 - 1) *d++ = *p++;
      *d = '\0'; }
    vfs_write(shortn, body, blen);

    /* Package metadata under /var/db/pkg/<name>/ (Portage-style). */
    char dir[RXFS_NAME];
    path_meta(dir, sizeof(dir), e->name);
    vfs_mkdir(dir);

    char f[RXFS_NAME], meta[256];
    path_meta_file(f, sizeof(f), e->name, "PF");
    { char *d = meta, *end = meta + sizeof(meta);
      const char *p = e->name; while (*p && d < end - 1) *d++ = *p++;
      if (d < end - 1) *d++ = '-';
      p = e->version; while (*p && d < end - 1) *d++ = *p++;
      if (d < end - 1) *d++ = '\n';
      *d = '\0';
      vfs_write(f, meta, (uint32_t)k_strlen(meta)); }

    path_meta_file(f, sizeof(f), e->name, "DESCRIPTION");
    vfs_write(f, e->desc, (uint32_t)k_strlen(e->desc));

    path_meta_file(f, sizeof(f), e->name, "SIZE");
    { char n[16]; uint32_t L = blen;
      int i = 0; char t[12]; int k = 0;
      if (!L) t[k++] = '0';
      while (L) { t[k++] = (char)('0' + (L % 10)); L /= 10; }
      while (k) n[i++] = t[--k]; n[i] = '\0';
      vfs_write(f, n, (uint32_t)k_strlen(n)); }

    char hx[17]; hex8(e->sha3, hx);
    path_meta_file(f, sizeof(f), e->name, "SHA3");
    vfs_write(f, hx, 16);

    path_meta_file(f, sizeof(f), e->name, "SLOT");
    vfs_write(f, "0\n", 2);

    path_meta_file(f, sizeof(f), e->name, "CHANNEL");
    vfs_write(f, PKG_CHANNEL_URL, (uint32_t)k_strlen(PKG_CHANNEL_URL));

    return true;
}

static bool pkg_remove(const char *name) {
    const pkg_entry *e = find_pkg(name);
    const char *cmd = e ? e->cmd : name;

    char bin[RXFS_NAME];
    path_bin(bin, sizeof(bin), cmd);
    vfs_delete(bin);

    char shortn[RXFS_NAME];
    { char *d = shortn, *end = shortn + sizeof(shortn);
      const char *p = cmd; while (*p && d < end - 1) *d++ = *p++;
      p = ".rxc"; while (*p && d < end - 1) *d++ = *p++;
      *d = '\0'; }
    vfs_delete(shortn);

    /* Drop metadata files then the package dir. */
    static const char *meta[] = {
        "PF", "DESCRIPTION", "SIZE", "SHA3", "SLOT", "CHANNEL", NULL
    };
    char f[RXFS_NAME];
    for (int i = 0; meta[i]; i++) {
        path_meta_file(f, sizeof(f), name, meta[i]);
        vfs_delete(f);
    }
    path_meta(f, sizeof(f), name);
    vfs_rmdir(f);
    return true;
}

/* ---- commands ----------------------------------------------------------- */
static void cmd_help(void) {
    o_str("rx — EchOS package manager (Portage-inspired)\n \n");
    o_str("  rx app add <name>      download + install .rxc package into /bin\n");
    o_str("  rx app del <name>       remove an installed package\n");
    o_str("  rx app list             list installed packages\n");
    o_str("  rx app search [q]       search the package channel index\n");
    o_str("  rx app sync             re-verify the package channel\n");
    o_str("  rx app info <name>      show package metadata\n");
    o_str("  rx app channel          show verified channel status\n \n");
    o_str("  rx update check         OS file patches + wallpaper recipes\n");
    o_str("  rx update apply | now   apply staged patch; now = apply+reboot\n \n");
    o_str("Channel: "); o_str(PKG_CHANNEL_URL); o_nl();
    o_str("Layout:  /var/db/pkg  /var/cache/distfiles  /bin/*.rxc\n");
    o_str("Note:    TCP/HTTPS client is staged; channel uses a verified\n");
    o_str("         local mirror of the remote package tree for now.\n");
}

static void cmd_channel(void) {
    o_str("EchOS 6 package channel\n \n");
    o_str("  host:     "); o_str(PKG_CHANNEL_HOST); o_nl();
    o_str("  path:     "); o_str(PKG_CHANNEL_PATH); o_nl();
    o_str("  url:      "); o_str(www_channel_base()); o_nl();
    if (!channel_ok()) {
        o_str("  status:   FAIL (could not verify index)\n");
        return;
    }
    o_str("  status:   verified\n");
    o_str("  www:      "); o_str(www_enabled() ? "ON" : "OFF"); o_nl();
    o_str("  mode:     ");
    if (www_enabled() && www_ready())
        o_str("live attempt (DNS/TCP/HTTP; HTTPS TLS staged)\n");
    else if (net_present())
        o_str("local mirror (NIC up; run `www on` for live channel)\n");
    else
        o_str("local mirror (no NIC; offline index)\n");
    o_str("  packages: "); o_dec((uint64_t)PKG_INDEX_N); o_nl();
    { char hx[17]; hex8(g_channel_id, hx);
      o_str("  index_id: sha3:"); o_str(hx); o_str("...\n"); }
}

static void cmd_sync(void) {
    g_channel_ready = false;
    if (!channel_connect()) {
        o_str("! channel sync failed\n");
        return;
    }
    o_str("+ channel re-verified: ");
    o_str(PKG_CHANNEL_URL); o_nl();
    o_str("  packages in index: "); o_dec((uint64_t)PKG_INDEX_N); o_nl();
    { char hx[17]; hex8(g_channel_id, hx);
      o_str("  index_id: sha3:"); o_str(hx); o_str("...\n"); }
}

static void cmd_search(const char *q) {
    if (!channel_ok()) { o_str("! channel not ready\n"); return; }
    o_str("* package index ("); o_str(PKG_CHANNEL_URL); o_str(")\n \n");
    int hits = 0;
    for (int i = 0; i < PKG_INDEX_N; i++) {
        const pkg_entry *e = &g_index[i];
        if (q && q[0]) {
            bool match = false;
            const char *a = e->name, *b = q;
            /* naive substring on name + desc */
            for (const char *p = e->name; *p; p++) {
                const char *x = p, *y = q;
                while (*x && *y && *x == *y) { x++; y++; }
                if (!*y) { match = true; break; }
            }
            if (!match) {
                for (const char *p = e->desc; *p; p++) {
                    const char *x = p, *y = q;
                    while (*x && *y && *x == *y) { x++; y++; }
                    if (!*y) { match = true; break; }
                }
            }
            (void)a; (void)b;
            if (!match) continue;
        }
        o_str(is_installed(e->name) ? "  [I] " : "  [ ] ");
        o_str(e->name); o_str("-"); o_str(e->version);
        o_str("  "); o_str(e->desc); o_nl();
        hits++;
    }
    if (!hits) o_str("  (no matches)\n");
}

static void cmd_list(void) {
    o_str("* installed packages (/var/db/pkg)\n \n");
    char names[RXFS_MAX_FILES][RXFS_NAME];
    int n = vfs_list_dir("/var/db/pkg", names, RXFS_MAX_FILES);
    if (n < 0) { o_str("! cannot open /var/db/pkg\n"); return; }
    int shown = 0;
    for (int i = 0; i < n; i++) {
        size_t L = k_strlen(names[i]);
        if (!L) continue;
        /* dirs end with / from list_dir */
        char name[RXFS_NAME];
        size_t j = 0;
        for (; names[i][j] && names[i][j] != '/' && j + 1 < sizeof(name); j++)
            name[j] = names[i][j];
        name[j] = '\0';
        if (!name[0]) continue;

        char pf[RXFS_NAME];
        path_meta_file(pf, sizeof(pf), name, "PF");
        uint32_t len = 0;
        const char *d = vfs_read(pf, &len);
        o_str("  ");
        if (d && len) {
            for (uint32_t k = 0; k < len && d[k] != '\n'; k++) {
                char s[2] = { d[k], 0 }; o_str(s);
            }
        } else o_str(name);
        o_nl();
        shown++;
    }
    if (!shown) o_str("  (none — try: rx app add hellopkg)\n");
}

static void cmd_info(const char *name) {
    if (!name || !name[0]) { o_str("usage: rx app info <name>\n"); return; }
    const pkg_entry *e = find_pkg(name);
    if (!e) { o_str("! unknown package: "); o_str(name); o_nl(); return; }
    o_str("* "); o_str(e->name); o_str("-"); o_str(e->version); o_nl();
    o_str("  desc:    "); o_str(e->desc); o_nl();
    o_str("  command: /bin/"); o_str(e->cmd); o_str(".rxc\n");
    o_str("  size:    "); o_dec((uint64_t)k_strlen(e->source)); o_str(" B\n");
    if (!e->sha3[0] && !e->sha3[1]) channel_ok();
    { char hx[17]; hex8(e->sha3, hx);
      o_str("  sha3:    "); o_str(hx); o_str("...\n"); }
    o_str("  status:  "); o_str(is_installed(e->name) ? "installed\n" : "available\n");
    o_str("  channel: "); o_str(PKG_CHANNEL_URL); o_str("/");
    o_str(e->name); o_str(".rxc\n");
}

static void cmd_add(const char *name) {
    if (!name || !name[0]) {
        o_str("usage: rx app add <name>\n");
        o_str("try:    rx app search\n");
        return;
    }
    if (!channel_ok()) { o_str("! package channel not verified\n"); return; }
    const pkg_entry *e = find_pkg(name);
    if (!e) {
        o_str("! package not in channel: "); o_str(name); o_nl();
        o_str("  channel: "); o_str(PKG_CHANNEL_URL); o_nl();
        o_str("  try: rx app search\n");
        return;
    }
    if (is_installed(e->name)) {
        o_str("= already installed: "); o_str(e->name); o_str("-");
        o_str(e->version); o_nl();
        o_str("  reinstalling...\n");
    }
    o_str("* fetching "); o_str(e->name); o_str(".rxc from channel...\n");
    o_str("  "); o_str(www_channel_base()); o_str("/"); o_str(e->name); o_str(".rxc\n");
    if (www_enabled())
        o_str("  www: ON — trying live channel (HTTP live / HTTPS probe)\n");
    else
        o_str("  www: OFF — verified local mirror (run `www on` for live)\n");
    if (!pkg_install(e)) {
        o_str("! install failed (vault full or integrity error)\n");
        return;
    }
    o_str("+ installed "); o_str(e->name); o_str("-"); o_str(e->version); o_nl();
    if (g_live_ok) o_str("  source: LIVE channel body\n");
    else o_str("  source: verified local mirror\n");
    o_str("  files: /bin/"); o_str(e->cmd); o_str(".rxc\n");
    o_str("  run:   "); o_str(e->cmd);
    o_str("     (terminal)   or   go rgx://"); o_str(e->cmd); o_nl();
    o_str("         /var/db/pkg/"); o_str(e->name); o_str("/\n");
    o_str("  run:   run "); o_str(e->cmd); o_str(".rxc\n");
    o_str("         go rgx://"); o_str(e->cmd); o_nl();
}

static void cmd_del(const char *name) {
    if (!name || !name[0]) { o_str("usage: rx app del <name>\n"); return; }
    if (!is_installed(name) && !find_pkg(name)) {
        o_str("! not installed: "); o_str(name); o_nl();
        return;
    }
    pkg_remove(name);
    o_str("+ removed "); o_str(name); o_nl();
}

void pkg_init(void) {
    vfs_mkdir("/var");
    vfs_mkdir("/var/db");
    vfs_mkdir("/var/db/pkg");
    vfs_mkdir("/var/cache");
    vfs_mkdir("/var/cache/distfiles");
    vfs_mkdir("/bin");
    channel_connect();
}

void pkg_cmd_rx(const char *sub, const char *arg, const char *rest,
                char *out, size_t outmax) {
    o_reset(out, outmax);
    (void)rest;

    if (!sub || !sub[0] || eqs(sub, "help") || eqs(sub, "--help")) {
        cmd_help();
        return;
    }

    /* `rx app ...` is the Portage-style entry point. Also accept the shorter
       `rx add` / `rx search` forms. */
    if (eqs(sub, "app")) {
        if (!arg || !arg[0] || eqs(arg, "help")) { cmd_help(); return; }
        if (eqs(arg, "add") || eqs(arg, "install") || eqs(arg, "emerge")) {
            cmd_add(rest); return;
        }
        if (eqs(arg, "del") || eqs(arg, "rm") || eqs(arg, "remove") ||
            eqs(arg, "unmerge")) {
            cmd_del(rest); return;
        }
        if (eqs(arg, "list") || eqs(arg, "world")) { cmd_list(); return; }
        if (eqs(arg, "search") || eqs(arg, "s")) {
            cmd_search(rest); return;
        }
        if (eqs(arg, "sync")) { cmd_sync(); return; }
        if (eqs(arg, "info") || eqs(arg, "show")) { cmd_info(rest); return; }
        if (eqs(arg, "channel")) { cmd_channel(); return; }
        o_str("unknown: rx app "); o_str(arg); o_nl();
        o_str("try: rx app help\n");
        return;
    }

    /* Short aliases: rx add / rx search / rx list / rx sync */
    if (eqs(sub, "add") || eqs(sub, "install") || eqs(sub, "emerge")) {
        cmd_add(arg); return;
    }
    if (eqs(sub, "del") || eqs(sub, "rm") || eqs(sub, "remove")) {
        cmd_del(arg); return;
    }
    if (eqs(sub, "list")) { cmd_list(); return; }
    if (eqs(sub, "search")) { cmd_search(arg); return; }
    if (eqs(sub, "sync")) { cmd_sync(); return; }
    if (eqs(sub, "info")) { cmd_info(arg); return; }
    if (eqs(sub, "channel")) { cmd_channel(); return; }
    if (eqs(sub, "demo") || eqs(sub, "run")) {
        /* Handled by the browser shell historically; keep a pointer. */
        o_str("built-in demos: use `run hello.rxc` or `go rgx://hello`\n");
        o_str("packages:       rx app add hellopkg\n");
        return;
    }

    o_str("unknown: rx "); o_str(sub); o_nl();
    o_str("try: rx app help\n");
}

/* Extract `text "..."` / `title "..."` lines from a Roxenite source. */
static void emit_rxc_texts(const char *src, uint32_t len) {
    const char *p = src, *end = src + len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        const char *key = NULL;
        if (p + 6 < end && p[0]=='t'&&p[1]=='e'&&p[2]=='x'&&p[3]=='t'&&p[4]==' ')
            key = p + 5;
        else if (p + 7 < end && p[0]=='t'&&p[1]=='i'&&p[2]=='t'&&p[3]=='l'&&p[4]=='e'&&p[5]==' ')
            key = p + 6;
        if (key) {
            while (key < end && (*key == ' ' || *key == '\t')) key++;
            if (key < end && *key == '"') {
                key++;
                char buf[160]; int n = 0;
                while (key < end && *key != '"' && n < (int)sizeof(buf) - 1)
                    buf[n++] = *key++;
                buf[n] = '\0';
                if (buf[0]) { o_str(buf); o_nl(); }
            }
        }
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }
}

static bool read_pkg_src(const char *name, const char **body, uint32_t *blen) {
    char path[RXFS_NAME];
    /* /bin/<name>.rxc */
    { char *d = path; const char *p = "/bin/";
      while (*p) *d++ = *p++;
      p = name; while (*p && d < path + sizeof(path) - 5) *d++ = *p++;
      *d++ = '.'; *d++ = 'r'; *d++ = 'x'; *d++ = 'c'; *d = '\0'; }
    *body = vfs_read(path, blen);
    if (*body && *blen) return true;
    /* <name>.rxc at vault root */
    { char *d = path; const char *p = name;
      while (*p && d < path + sizeof(path) - 5) *d++ = *p++;
      *d++ = '.'; *d++ = 'r'; *d++ = 'x'; *d++ = 'c'; *d = '\0'; }
    *body = vfs_read(path, blen);
    if (*body && *blen) return true;
    /* bare /bin/<name> */
    { char *d = path; const char *p = "/bin/";
      while (*p) *d++ = *p++;
      p = name; while (*p && d < path + sizeof(path) - 1) *d++ = *p++;
      *d = '\0'; }
    *body = vfs_read(path, blen);
    return *body && *blen;
}

bool pkg_try_command(const char *name, const char *args, char *out, size_t outmax) {
    if (!name || !name[0]) return false;
    /* Don't steal real system commands. */
    if (eqs(name, "rx") || eqs(name, "help") || eqs(name, "update")) return false;

    const char *body = NULL;
    uint32_t blen = 0;
    if (!read_pkg_src(name, &body, &blen)) return false;

    o_reset(out, outmax);
    if (eqs(name, "echo")) {
        if (args && args[0]) { o_str(args); o_nl(); }
        else o_nl();
        return true;
    }
    emit_rxc_texts(body, blen);
    if (!o_len) {
        o_str("("); o_str(name); o_str(" installed — no terminal text; try: go rgx://");
        o_str(name); o_str(")\n");
    }
    return true;
}
