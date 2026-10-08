/*
 * In-kernel epk. Catalog lives on the ISO *and* at
 * https://www.rogexlaboratories.com/packages  (`epk get` / `epk install`
 * fetch the INDEX then the .rxp when `www` is on).
 */
#include "epk.h"
#include "vfs.h"
#include "version.h"
#include "heap.h"
#include "www.h"
#include "pkg.h"
int  commands_verbose(void);
void commands_v(const char *s);

#define EPK_F_MIN  (1u << 0)
#define EPK_F_COM  (1u << 1)
#define EPK_F_EDG  (1u << 2)
#define EPK_F_SRV  (1u << 3)
#define EPK_F_ALL  (EPK_F_MIN | EPK_F_COM | EPK_F_EDG | EPK_F_SRV)
#define EPK_F_LAB  (1u << 8)   /* host/lab note: hidden from `epk list` */

typedef struct {
    const char *name;
    const char *version;
    const char *desc;
    uint32_t    flags;
    const char *body;       /* installed into /bin/<name> as a note file */
} epk_pkg_t;

static const char BODY_BASE[] =
    "echos-base 2.1.0\nHeap-0 kernel, ROSH, epk, JetBrains Mono CLI.\n";
static const char BODY_SHELL[] =
    "echos-shell 2.1.0\ndoas rcctl pfctl disklabel — flags, not daemons.\n";
static const char BODY_CLI[] =
    "NOTE (host): echos-cli. less/grep/find/du exist in-kernel; htop/tmux do not.\n";
static const char BODY_DEV[] =
    "NOTE (host): compilers live on the host + echlibc. not in this unikernel.\n";
static const char BODY_DIAG[] =
    "echos-diag 2.1.0\nechofetch, memtest, prisma5, bench-snn (in-kernel SNN).\n";
static const char BODY_NPU[] =
    "echos-npu 2.1.0\nAkida PCI probe 1e7c:bca1. No card = software LIF. No offload yet.\n";
static const char BODY_IO[] =
    "echos-io 2.1.0\nStatic I/O drivers already in the kernel. This is a note.\n";
static const char BODY_NET[] =
    "echos-net 2.1.0\nIPv4 + one TCP + HTTP. TLS handshake; HTTPS body may be empty.\n";
static const char BODY_HTTP[] =
    "NOTE: rcctl flag httpd only. no HTTP daemon process in this unikernel.\n";
static const char BODY_FTP[] =
    "NOTE: rcctl flag ftpd only. no FTP daemon process in this unikernel.\n";
static const char BODY_SSH[] =
    "NOTE: rcctl flag sshd only. no SSH daemon process in this unikernel.\n";
static const char BODY_PQC[] =
    "NOTE: rcctl flag pqc only. ML-KEM is in rogex-core; no tunnel process.\n";
static const char BODY_TCC[] =
    "NOTE (host): TinyCC. not an in-OS binary. host toolchain + echlibc.\n";
static const char BODY_MAKE[] =
    "NOTE (host): make. not an in-OS binary.\n";
static const char BODY_EPKBUILD[] =
    "NOTE (host): epk-build packs RXP1 on the host. not in-OS.\n";
static const char BODY_PY[] =
    "NOTE (host): CPython. RXFS is 64 KiB/file. not an in-OS interpreter.\n";
static const char BODY_QJS[] =
    "NOTE (host): QuickJS. not an in-OS JS engine.\n";
static const char BODY_NASM[] =
    "NOTE (host): nasm. not an in-OS assembler.\n";
static const char BODY_RUSTC[] =
    "NOTE (host): rustc target sketch. no rustc in this unikernel.\n";
static const char BODY_AKIDA[] =
    "echos-akida 2.1.0\nPCI 1e7c:bca1 reservation. Probe only. No milliwatt claim without a card.\n";
static const char BODY_RG[] =
    "NOTE (host): rg. in-kernel grep exists; this package is a host binary.\n";
static const char BODY_FD[] =
    "NOTE (host): fd. in-kernel find exists; this package is a host binary.\n";
static const char BODY_BAT[] =
    "NOTE (host): bat. in-kernel cat exists; this package is a host binary.\n";
static const char BODY_EZA[] =
    "NOTE (host): eza. in-kernel ls exists; this package is a host binary.\n";
static const char BODY_Z[] =
    "NOTE (host): z. not an in-OS jumper.\n";
static const char BODY_UNP[] =
    "NOTE (host): unp. not an in-OS unpacker.\n";
static const char BODY_DUST[] =
    "NOTE (host): dust. in-kernel du exists.\n";
static const char BODY_DUF[] =
    "NOTE (host): duf. in-kernel df exists.\n";
static const char BODY_HYPER[] =
    "NOTE (host): hyperfine. in-kernel bench exists.\n";
static const char BODY_LOCAL[] =
    "NOTE (host): localsend. not an in-OS transfer daemon.\n";
static const char BODY_RXPKG[] =
    "NOTE (host): rx-pkg RXP1 packer. host + Complete desktop line.\n";

static const epk_pkg_t CATALOG[] = {
    /* Public: names that describe pieces this unikernel actually has. */
    { "echos-base",  "2.1.0", "kernel + Heap-0 + ROSH + epk",           EPK_F_ALL, BODY_BASE },
    { "echos-shell", "2.1.0", "doas rcctl pfctl (flags, no daemons)",   EPK_F_ALL, BODY_SHELL },
    { "echofetch",   "2.1.0", "system card + droplet",                  EPK_F_ALL, BODY_DIAG },
    { "echos-diag",  "2.1.0", "echofetch memtest prisma5 bench-snn",    EPK_F_ALL, BODY_DIAG },
    { "echos-net",   "2.1.0", "IPv4 + one TCP + HTTP client",           EPK_F_ALL, BODY_NET },
    { "echos-npu",   "2.1.0", "Akida PCI probe; LIF if no card",        EPK_F_ALL, BODY_NPU },
    { "echos-io",    "2.1.0", "static I/O drivers (already in kernel)", EPK_F_ALL, BODY_IO },
    { "echos-akida", "2.1.0", "AKD1000 PCI 1e7c:bca1 reservation",      EPK_F_ALL, BODY_AKIDA },
    /* Lab: host toolchain / daemon flags. `epk list --lab` only. */
    { "echos-cli",   "2.1.0", "NOTE host: extra CLI names",             EPK_F_LAB | EPK_F_COM | EPK_F_SRV, BODY_CLI },
    { "echos-dev",   "2.1.0", "NOTE host: compilers",                   EPK_F_LAB | EPK_F_COM, BODY_DEV },
    { "tcc",         "note",  "NOTE host: TinyCC, not in-OS",           EPK_F_LAB | EPK_F_COM, BODY_TCC },
    { "make",        "note",  "NOTE host: make, not in-OS",             EPK_F_LAB | EPK_F_COM, BODY_MAKE },
    { "epk-build",   "note",  "NOTE host: RXP1 packer",                 EPK_F_LAB | EPK_F_COM, BODY_EPKBUILD },
    { "python314",   "note",  "NOTE host: CPython, not in-OS",          EPK_F_LAB | EPK_F_COM, BODY_PY },
    { "quickjs",     "note",  "NOTE host: QuickJS, not in-OS",          EPK_F_LAB | EPK_F_COM, BODY_QJS },
    { "nasm",        "note",  "NOTE host: nasm, not in-OS",             EPK_F_LAB | EPK_F_COM, BODY_NASM },
    { "rustc-echos", "note",  "NOTE host: rustc target, not in-OS",     EPK_F_LAB | EPK_F_COM, BODY_RUSTC },
    { "echos-httpd", "note",  "NOTE: rcctl flag, no httpd process",     EPK_F_LAB | EPK_F_SRV, BODY_HTTP },
    { "echos-ftpd",  "note",  "NOTE: rcctl flag, no ftpd process",      EPK_F_LAB | EPK_F_SRV, BODY_FTP },
    { "echos-sshd",  "note",  "NOTE: rcctl flag, no sshd process",      EPK_F_LAB | EPK_F_SRV, BODY_SSH },
    { "echos-pqc",   "note",  "NOTE: rcctl flag, no tunnel process",    EPK_F_LAB | EPK_F_SRV, BODY_PQC },
    { "rg",          "note",  "NOTE host: rg (in-kernel: grep)",        EPK_F_LAB | EPK_F_ALL, BODY_RG },
    { "fd",          "note",  "NOTE host: fd (in-kernel: find)",        EPK_F_LAB | EPK_F_ALL, BODY_FD },
    { "bat",         "note",  "NOTE host: bat (in-kernel: cat)",        EPK_F_LAB | EPK_F_ALL, BODY_BAT },
    { "eza",         "note",  "NOTE host: eza (in-kernel: ls)",         EPK_F_LAB | EPK_F_ALL, BODY_EZA },
    { "z",           "note",  "NOTE host: z",                           EPK_F_LAB | EPK_F_ALL, BODY_Z },
    { "unp",         "note",  "NOTE host: unp",                         EPK_F_LAB | EPK_F_ALL, BODY_UNP },
    { "dust",        "note",  "NOTE host: dust (in-kernel: du)",        EPK_F_LAB | EPK_F_ALL, BODY_DUST },
    { "duf",         "note",  "NOTE host: duf (in-kernel: df)",         EPK_F_LAB | EPK_F_ALL, BODY_DUF },
    { "hyperfine",   "note",  "NOTE host: hyperfine (in-kernel: bench)",EPK_F_LAB | EPK_F_ALL, BODY_HYPER },
    { "localsend",   "note",  "NOTE host: localsend",                   EPK_F_LAB | EPK_F_ALL, BODY_LOCAL },
    { "rx-pkg",      "note",  "NOTE host: RXP1 packer",                 EPK_F_LAB | EPK_F_COM, BODY_RXPKG },
};
#define CATALOG_N ((int)(sizeof(CATALOG) / sizeof(CATALOG[0])))

static char  *o_buf;
static size_t o_max, o_len;
static char   g_edbuf[32];

static void o_reset(char *b, size_t m) {
    o_buf = b; o_max = m; o_len = 0; if (m) b[0] = '\0';
}
static void o_s(const char *s) {
    if (!s) return;
    while (*s && o_len + 1 < o_max) o_buf[o_len++] = *s++;
    if (o_max) o_buf[o_len] = '\0';
}
static void o_nl(void) { o_s("\n"); }
static void o_dec(uint64_t v) {
    char t[24]; int i = 0;
    if (!v) { o_s("0"); return; }
    while (v && i < 23) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) { char c[2] = { t[--i], 0 }; o_s(c); }
}
static bool eqs(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
static int is_lab(const epk_pkg_t *e) {
    return e && (e->flags & EPK_F_LAB) != 0;
}

static uint32_t flag_of(epk_edition_t ed) {
    switch (ed) {
    case EPK_ED_MINIMAL:  return EPK_F_MIN;
    case EPK_ED_COMPLETE: return EPK_F_COM;
    case EPK_ED_EDGE:     return EPK_F_EDG;
    case EPK_ED_SERVER:   return EPK_F_SRV;
    default:              return EPK_F_MIN;
    }
}

const char *epk_edition_name(epk_edition_t ed) {
    switch (ed) {
    case EPK_ED_MINIMAL:  return "minimal";
    case EPK_ED_COMPLETE: return "complete";
    case EPK_ED_EDGE:     return "edge";
    case EPK_ED_SERVER:   return "server";
    default:              return "minimal";
    }
}

epk_edition_t epk_edition_from_name(const char *s) {
    if (!s) return EPK_ED_MINIMAL;
    if (eqs(s, "complete") || eqs(s, "2")) return EPK_ED_COMPLETE;
    if (eqs(s, "edge")     || eqs(s, "3")) return EPK_ED_EDGE;
    if (eqs(s, "server")   || eqs(s, "4")) return EPK_ED_SERVER;
    return EPK_ED_MINIMAL;
}

static const epk_pkg_t *find_pkg(const char *name) {
    int i;
    if (!name || !name[0]) return NULL;
    for (i = 0; i < CATALOG_N; i++)
        if (eqs(CATALOG[i].name, name)) return &CATALOG[i];
    return NULL;
}

static void path_join(char *d, size_t max, const char *a, const char *b) {
    size_t n = 0;
    while (a && *a && n + 1 < max) d[n++] = *a++;
    if (n && d[n - 1] != '/' && n + 1 < max) d[n++] = '/';
    while (b && *b && n + 1 < max) d[n++] = *b++;
    d[n] = '\0';
}

static bool g_live_world;

static bool installed(const char *name) {
    char p[RXFS_NAME];
    const epk_pkg_t *e = find_pkg(name);
    /* LIVE seeds public notes only. Lab names are not "installed" until
       someone writes them with `epk install`. */
    if (g_live_world && e && !is_lab(e)) return true;
    path_join(p, sizeof(p), EPK_DB_DIR, name);
    return vfs_path_exists(p);
}

bool epk_is_installed(const char *name) { return installed(name); }

const char *epk_pkg_for_command(const char *cmd) {
    static const struct { const char *cmd; const char *pkg; } map[] = {
        { "tcc", "tcc" }, { "make", "make" }, { "epk-build", "epk-build" },
        { "python3.14", "python314" }, { "python314", "python314" },
        { "python3", "python314" }, { "qjs", "quickjs" }, { "quickjs", "quickjs" },
        { "nasm", "nasm" }, { "rustc", "rustc-echos" }, { "rustc-echos", "rustc-echos" },
        { "httpd", "echos-httpd" }, { "ftpd", "echos-ftpd" }, { "sshd", "echos-sshd" },
        { "pqc", "echos-pqc" }, { "git", "echos-cli" },
        { 0, 0 }
    };
    int i;
    if (!cmd || !cmd[0]) return NULL;
    for (i = 0; map[i].cmd; i++)
        if (eqs(cmd, map[i].cmd)) return map[i].pkg;
    return NULL;
}

bool epk_run_command(const char *cmd, const char *a1, const char *rest,
                     char *out, size_t outmax) {
    const char *pkg = epk_pkg_for_command(cmd);
    (void)rest;
    if (!pkg) return false;
    if (!installed(pkg)) return false;
    o_reset(out, outmax);
    o_s(cmd); o_s(": not in this unikernel (no user process).\n");
    if (eqs(cmd, "httpd") || eqs(cmd, "ftpd") || eqs(cmd, "sshd") || eqs(cmd, "pqc"))
        o_s("rcctl flags exist; they do not start a daemon here.\n");
    else
        o_s("host toolchain + echlibc, not an in-OS binary.\n");
    if (a1 && a1[0] && !eqs(a1, "--help") && !eqs(a1, "-h") && !eqs(a1, "help")) {
        o_s("(no payload for '"); o_s(a1); o_s("' in the stub)\n");
    }
    return true;
}

void epk_live_all(void) {
    char buf[1024];
    int i, n = 0;
    g_live_world = true;
    for (i = 0; i < CATALOG_N && n + 80 < (int)sizeof(buf); i++) {
        const char *p;
        if (is_lab(&CATALOG[i])) continue;
        p = CATALOG[i].name;
        while (*p && n + 1 < (int)sizeof(buf)) buf[n++] = *p++;
        buf[n++] = '\n';
    }
    vfs_mkdir("/epk");
    vfs_mkdir(EPK_DB_DIR);
    vfs_write("/epk/db/WORLD", buf, (uint32_t)n);
    vfs_write("/etc/echos-edition", "live", 4);
}

static bool put_pkg(const epk_pkg_t *e) {
    char bin[RXFS_NAME], db[RXFS_NAME];
    if (!e) return false;
    path_join(bin, sizeof(bin), EPK_BIN_DIR, e->name);
    path_join(db, sizeof(db), EPK_DB_DIR, e->name);
    if (!vfs_write(bin, e->body, (uint32_t)k_strlen(e->body))) return false;
    if (!vfs_write(db, e->version, (uint32_t)k_strlen(e->version))) return false;
    return true;
}

static void del_pkg(const char *name) {
    char bin[RXFS_NAME], db[RXFS_NAME];
    path_join(bin, sizeof(bin), EPK_BIN_DIR, name);
    path_join(db, sizeof(db), EPK_DB_DIR, name);
    vfs_delete(bin);
    vfs_delete(db);
}

void epk_init(void) {
    vfs_mkdir("/epk");
    vfs_mkdir(EPK_DB_DIR);
    vfs_mkdir(EPK_MANIFEST_DIR);
    vfs_mkdir(EPK_BIN_DIR);
    vfs_mkdir("/sys");
    vfs_mkdir("/sys/editions");
    /* LIVE / first boot on the shared 2.0 kernel: seed Minimal. 1.x
       editions keep the classic package channel in pkg.c. */
#if ECHOS_SHARED_KERNEL
    epk_live_all();
#endif
}

const char *epk_runtime_edition(void) {
    uint32_t n = 0;
    const char *s = vfs_read("/etc/echos-edition", &n);
    uint32_t i = 0;
    if (!s || !n) return ECHOS_EDITION;
    while (i < n && i + 1 < sizeof(g_edbuf) && s[i] != '\n' && s[i] != '\r') {
        g_edbuf[i] = s[i];
        i++;
    }
    g_edbuf[i] = '\0';
    return g_edbuf[0] ? g_edbuf : ECHOS_EDITION;
}

bool epk_apply_edition(epk_edition_t ed, char *err, uint32_t errmax) {
    uint32_t f = flag_of(ed);
    int i;
    g_live_world = false;
    if (err && errmax) err[0] = '\0';
    for (i = 0; i < CATALOG_N; i++) {
        if (is_lab(&CATALOG[i])) continue;
        if (CATALOG[i].flags & f) {
            if (!put_pkg(&CATALOG[i])) {
                if (err && errmax) {
                    const char *m = "epk: vault full";
                    uint32_t k = 0;
                    while (m[k] && k + 1 < errmax) { err[k] = m[k]; k++; }
                    err[k] = '\0';
                }
                return false;
            }
        }
    }
    vfs_mkdir("/etc");
    vfs_mkdir("/sys/editions");
    vfs_mkdir(EPK_MANIFEST_DIR);
    vfs_write("/etc/echos-edition", epk_edition_name(ed),
              (uint32_t)k_strlen(epk_edition_name(ed)));
    {
        char p[RXFS_NAME];
        const char *n = epk_edition_name(ed);
        path_join(p, sizeof(p), "/sys/editions", n);
        vfs_write(p, n, (uint32_t)k_strlen(n));
        path_join(p, sizeof(p), EPK_MANIFEST_DIR, n);
        vfs_write(p, n, (uint32_t)k_strlen(n));
    }
    return true;
}

static void cmd_stress(const char *arg) {
    const epk_pkg_t *e = find_pkg("echos-shell");
    size_t h0, h1;
    int n = 32, i, fail = 0;
    if (arg && arg[0] >= '1' && arg[0] <= '9') {
        n = 0;
        while (*arg >= '0' && *arg <= '9') n = n * 10 + (*arg++ - '0');
        if (n > 128) n = 128;
        if (n < 1) n = 1;
    }
    if (!e) { o_s("epk stress: catalog missing\n"); return; }
    h0 = heap_used();
    for (i = 0; i < n; i++) {
        if (!put_pkg(e)) fail++;
        del_pkg(e->name);
        if (!put_pkg(e)) fail++;
    }
    h1 = heap_used();
    o_s("[STRESS] epk install/remove x");
    { char t[12]; int k = 0, j = 0; int v = n;
      char b[12];
      if (!v) b[k++] = '0';
      while (v) { b[k++] = (char)('0' + v % 10); v /= 10; }
      while (k) t[j++] = b[--k]; t[j] = 0;
      o_s(t); }
    o_s(" cycles  fail=");
    { char t[12]; int k = 0, j = 0; int v = fail;
      char b[12];
      if (!v) b[k++] = '0';
      while (v) { b[k++] = (char)('0' + v % 10); v /= 10; }
      while (k) t[j++] = b[--k]; t[j] = 0;
      o_s(t); }
    o_s("  heap_used before=");
    { uint64_t v = h0; char t[24]; int i2 = 0;
      if (!v) t[i2++] = '0';
      while (v) { t[i2++] = (char)('0' + v % 10); v /= 10; }
      while (i2) { char c[2] = { t[--i2], 0 }; o_s(c); } }
    o_s(" after=");
    { uint64_t v = h1; char t[24]; int i2 = 0;
      if (!v) t[i2++] = '0';
      while (v) { t[i2++] = (char)('0' + v % 10); v /= 10; }
      while (i2) { char c[2] = { t[--i2], 0 }; o_s(c); } }
    o_s("  delta=");
    o_s(h1 == h0 ? "0 B  PASS (Heap-0 stable)\n" : "NONZERO  FAIL\n");
}

static int json_str(const char *j, const char *key, char *out, int omax) {
    const char *p = j;
    int klen = 0, n;
    if (!j || !key || !out || omax < 2) return 0;
    while (key[klen]) klen++;
    while (*p) {
        if (*p == '"' && p[klen + 1] == '"' && p[klen + 2] == ':') {
            int i, ok = 1;
            for (i = 0; i < klen; i++) if (p[1 + i] != key[i]) { ok = 0; break; }
            if (ok) {
                p += klen + 3;
                while (*p == ' ' || *p == '\t') p++;
                if (*p != '"') return 0;
                p++;
                n = 0;
                while (*p && *p != '"' && n + 1 < omax) out[n++] = *p++;
                out[n] = 0;
                return n;
            }
        }
        p++;
    }
    return 0;
}

/* Pull INDEX.json then the .rxp into /tmp. Also records the local stub. */
static void cmd_get(const char *name) {
    static char idx[8192];
    static char body[WWW_BODY_MAX];
    char url[220], file[96], dest[96];
    const char *p, *hit;
    int st = 0;
    uint32_t n;
    const epk_pkg_t *e;

    if (!name || !name[0]) {
        o_s("usage: epk get <pkg>     fetch .rxp from ");
        o_s(PKG_CHANNEL_URL); o_nl();
        return;
    }
    if (!www_enable()) {
        o_s("epk get: "); o_s(www_last_err()[0] ? www_last_err() : "need a NIC (`www on`)");
        o_nl();
        return;
    }
    {
        const char *b = PKG_CHANNEL_URL;
        char *d = url, *end = url + sizeof(url);
        while (*b && d < end - 20) *d++ = *b++;
        b = "/INDEX.json";
        while (*b && d < end - 1) *d++ = *b++;
        *d = 0;
    }
    n = www_http_request("GET", url, NULL, 0, NULL, idx, sizeof(idx), &st);
    if (!n || st < 200 || st >= 300) {
        o_s("epk get: INDEX HTTP "); o_dec((uint64_t)st); o_s("  ");
        o_s(www_last_err()[0] ? www_last_err() : "empty"); o_nl();
        o_s("channel: "); o_s(PKG_CHANNEL_URL); o_nl();
        return;
    }
    /* Find the object that names this package, then its file field. */
    hit = 0;
    for (p = idx; *p; p++) {
        if (p[0] == '"' && p[1] == 'n' && p[2] == 'a' && p[3] == 'm' &&
            p[4] == 'e' && p[5] == '"' && p[6] == ':') {
            const char *q = p + 7;
            const char *nm = name;
            while (*q == ' ' || *q == '"') q++;
            while (*nm && *q && *nm == *q) { nm++; q++; }
            if (!*nm && (*q == '"' || *q == ',' || *q == '}')) { hit = p; break; }
        }
    }
    if (!hit) {
        o_s("epk get: '"); o_s(name); o_s("' is not on "); o_s(PKG_CHANNEL_URL); o_nl();
        return;
    }
    file[0] = 0;
    json_str(hit, "file", file, (int)sizeof(file));
    if (!file[0]) {
        o_s("epk get: INDEX has no file for "); o_s(name); o_nl();
        return;
    }
    {
        const char *b = PKG_CHANNEL_URL;
        char *d = url, *end = url + sizeof(url);
        while (*b && d < end - 2) *d++ = *b++;
        if (d > url && d[-1] != '/') *d++ = '/';
        b = file;
        while (*b && d < end - 1) *d++ = *b++;
        *d = 0;
    }
    n = www_http_request("GET", url, NULL, 0, NULL, body, sizeof(body), &st);
    o_s("HTTP "); o_dec((uint64_t)st); o_s("  "); o_dec(n); o_s(" bytes  "); o_s(url); o_nl();
    if (!n || st < 200 || st >= 300) {
        o_s("epk get: download failed\n");
        return;
    }
    vfs_mkdir("/tmp");
    dest[0] = '/'; dest[1] = 't'; dest[2] = 'm'; dest[3] = 'p'; dest[4] = '/';
    {
        int i = 0;
        while (file[i] && i + 6 < (int)sizeof(dest)) { dest[5 + i] = file[i]; i++; }
        dest[5 + i] = 0;
    }
    if (!vfs_write(dest, body, n)) {
        o_s("epk get: write failed (RXFS 64 KiB/file — body "); o_dec(n); o_s(" B)\n");
        return;
    }
    o_s("saved "); o_s(dest); o_s("  "); o_dec(n); o_s(" B\n");
    e = find_pkg(name);
    if (e && put_pkg(e)) {
        o_s("installed stub "); o_s(e->name); o_s(" "); o_s(e->version); o_nl();
    }
    o_s("see also:  curl -O "); o_s(url); o_nl();
}

static void cmd_help(void) {
    o_s("epk — echOS package notes (RXFS text, not ELFs)\n \n");
    o_s("  epk list              kernel notes in this unikernel\n");
    o_s("  epk list --lab        host toolchain / daemon flags (not runnable here)\n");
    o_s("  epk info [pkg]        catalog / installed metadata\n");
    o_s("  epk install <pkg>     write an RXFS note (alias: epk_add)\n");
    o_s("  epk remove  <pkg>     delete files + record        (alias: epk_delete)\n");
    o_s("  epk search [q]        search public names (add --lab for host notes)\n");
    o_s("  epk apply <edition>   minimal|complete|edge|server (public notes only)\n");
    o_s("  epk get <pkg>         HTTP GET .rxp (needs www on; 64 KiB cap)\n");
    o_s("  epk stress [N]        N install/remove cycles (Heap-0)\n");
    o_s("  epk media             show /epk policy + channel URL\n \n");
    o_s("Channel: "); o_s(PKG_CHANNEL_URL); o_s("  (optional; ISO catalog is enough)\n");
    o_s("Media:   "); o_s(EPK_MEDIA_DIR); o_s("   DB: "); o_s(EPK_DB_DIR); o_nl();
}

static void cmd_media(void) {
    o_s("epk media\n \n");
    o_s("  path:     "); o_s(EPK_MEDIA_DIR); o_nl();
    o_s("  channel:  "); o_s(PKG_CHANNEL_URL); o_nl();
    o_s("  policy:   ISO catalog + live fetch (`epk get`, needs www on)\n");
    o_s("  catalog:  "); { char n[8]; int v = CATALOG_N, i = 0, k = 0; char t[8];
        if (!v) t[i++] = '0'; while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
        while (i) n[k++] = t[--i]; n[k] = 0; o_s(n); }
    o_s(" packages embedded (mirrors the ISO)\n");
    o_s("  edition:  "); o_s(epk_runtime_edition()); o_nl();
    o_s("  kernel:   "); o_s(ECHOS_VERSION); o_s(" shared="); o_s(ECHOS_SHARED_KERNEL ? "yes" : "no"); o_nl();
}

static void cmd_list(const char *arg) {
    int i, n = 0, lab = 0;
    if (arg && (eqs(arg, "--lab") || eqs(arg, "lab") || eqs(arg, "-l")))
        lab = 1;
    if (lab) {
        o_s("* lab notes (host toolchain / daemon flags — not in this unikernel)\n \n");
    } else {
        o_s("* kernel notes ("); o_s(EPK_DB_DIR); o_s(")  — RXFS text, not ELFs\n \n");
    }
    for (i = 0; i < CATALOG_N; i++) {
        if (lab) {
            if (!is_lab(&CATALOG[i])) continue;
        } else {
            if (is_lab(&CATALOG[i])) continue;
            if (!installed(CATALOG[i].name)) continue;
        }
        o_s("  "); o_s(CATALOG[i].name); o_s("  "); o_s(CATALOG[i].version);
        o_s("  "); o_s(CATALOG[i].desc); o_nl();
        n++;
    }
    if (!n) o_s(lab ? "  (none)\n" : "  (none — epk apply minimal)\n");
    if (!lab) o_s("\n  host notes:  epk list --lab\n");
}

static void cmd_search(const char *q) {
    int i, hits = 0;
    o_s("* public catalog (kernel notes). host notes: epk list --lab\n \n");
    for (i = 0; i < CATALOG_N; i++) {
        const epk_pkg_t *e = &CATALOG[i];
        if (is_lab(e)) continue;
        if (q && q[0]) {
            const char *p; int ok = 0;
            for (p = e->name; *p; p++) {
                const char *a = p, *b = q;
                while (*a && *b && *a == *b) { a++; b++; }
                if (!*b) { ok = 1; break; }
            }
            if (!ok) continue;
        }
        o_s(installed(e->name) ? "  [I] " : "  [ ] ");
        o_s(e->name); o_s("-"); o_s(e->version); o_s("  "); o_s(e->desc); o_nl();
        hits++;
    }
    if (!hits) o_s("  (no matches)\n");
}

static void cmd_info(const char *name) {
    const epk_pkg_t *e;
    if (!name || !name[0]) { cmd_media(); return; }
    e = find_pkg(name);
    if (!e) { o_s("epk: unknown package: "); o_s(name); o_nl(); return; }
    o_s("Name:        "); o_s(e->name); o_nl();
    o_s("Version:     "); o_s(e->version); o_nl();
    o_s("Description: "); o_s(e->desc); o_nl();
    o_s("Status:      "); o_s(installed(e->name) ? "installed\n" : "available\n");
    o_s("Local:       "); o_s(EPK_MEDIA_DIR); o_s("/"); o_s(e->name); o_s(".rxp\n");
    o_s("Channel:     "); o_s(PKG_CHANNEL_URL); o_s("/"); o_s(e->name); o_s("-");
    o_s(e->version); o_s(".rxp\n");
}

static void cmd_install(const char *name) {
    const epk_pkg_t *e;
    if (!name || !name[0]) { o_s("usage: epk install <pkg>\n"); return; }
    e = find_pkg(name);
    if (e) {
        if (!put_pkg(e)) { o_s("epk: install failed (vault full)\n"); return; }
        o_s("installed note "); o_s(e->name); o_s(" "); o_s(e->version);
        o_s(" (RXFS text, not an ELF)\n");
        if (is_lab(e))
            o_s("this is a host/lab marker; it does not add a process.\n");
        return;
    }
    o_s("not in local catalog — trying "); o_s(PKG_CHANNEL_URL); o_nl();
    cmd_get(name);
}

static void cmd_remove(const char *name) {
    if (!name || !name[0]) { o_s("usage: epk remove <pkg>\n"); return; }
    if (!installed(name) && !find_pkg(name)) {
        o_s("epk: not installed: "); o_s(name); o_nl();
        return;
    }
    del_pkg(name);
    o_s("removed "); o_s(name); o_nl();
}

static void cmd_apply(const char *name) {
    epk_edition_t ed = epk_edition_from_name(name);
    char err[64];
    if (!name || !name[0]) {
        o_s("usage: epk apply minimal|complete|edge|server\n");
        return;
    }
    if (!epk_apply_edition(ed, err, sizeof(err))) {
        o_s("epk: "); o_s(err[0] ? err : "apply failed"); o_nl();
        return;
    }
    o_s("applied edition "); o_s(epk_edition_name(ed)); o_s(" from local manifests\n");
}

void epk_cmd(const char *sub, const char *arg, const char *rest,
             char *out, size_t outmax) {
    o_reset(out, outmax);
    (void)rest;
    if (commands_verbose() && sub && sub[0] &&
        !eqs(sub, "help") && !eqs(sub, "--help")) {
        if (eqs(sub, "install") || eqs(sub, "add") || eqs(sub, "epk_add"))
            commands_v("writing an RXFS note, not a user-process ELF");
        else if (eqs(sub, "get") || eqs(sub, "fetch") || eqs(sub, "download"))
            commands_v("HTTP GET INDEX.json then the .rxp (64 KiB cap, needs www on)");
        else if (eqs(sub, "list"))
            commands_v("public kernel notes; --lab lists host toolchain / daemon flags");
        else if (eqs(sub, "apply"))
            commands_v("edition = a list of notes, not a second kernel");
    }
    if (!sub || !sub[0] || eqs(sub, "help") || eqs(sub, "--help")) { cmd_help(); return; }
    if (eqs(sub, "get") || eqs(sub, "fetch") || eqs(sub, "download")) {
        cmd_get(arg); return;
    }
    if (eqs(sub, "install") || eqs(sub, "add") || eqs(sub, "epk_add")) {
        cmd_install(arg); return;
    }
    if (eqs(sub, "remove") || eqs(sub, "delete") || eqs(sub, "del") ||
        eqs(sub, "epk_delete")) {
        cmd_remove(arg); return;
    }
    if (eqs(sub, "info"))   { cmd_info(arg); return; }
    if (eqs(sub, "list"))   { cmd_list(arg); return; }
    if (eqs(sub, "search")) { cmd_search(arg); return; }
    if (eqs(sub, "apply"))  { cmd_apply(arg); return; }
    if (eqs(sub, "stress")) { cmd_stress(arg); return; }
    if (eqs(sub, "media") || eqs(sub, "channel")) { cmd_media(); return; }
    o_s("epk: unknown command '"); o_s(sub); o_s("'\ntry: epk help\n");
}
