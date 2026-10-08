/*
 * OpenBSD-inspired userland: doas, rcctl, pfctl, disklabel, gpt, df, du,
 * dysk, less, tail, grep, find. Freestanding — RXFS + blockdev only.
 */
#include "bsdtools.h"
#include "vfs.h"
#include "blockdev.h"
#include "partition.h"
#include "disk.h"
#include "version.h"
#include "commands.h"

static char  *o_buf;
static size_t o_max, o_len;

static void o_reset(char *b, size_t m) {
    o_buf = b; o_max = m; o_len = 0; if (m) b[0] = '\0';
}
static void o_s(const char *s) {
    if (!s) return;
    while (*s && o_len + 1 < o_max) o_buf[o_len++] = *s++;
    if (o_max) o_buf[o_len] = '\0';
}
static void o_d(uint64_t v) {
    char t[24]; int i = 0;
    if (!v) t[i++] = '0';
    while (v && i < 20) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) { char c[2] = { t[--i], 0 }; o_s(c); }
}
static void bsd_nl(void) { o_s("\n"); }
static bool eqs(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

static const char DOAS_CONF[] =
    "# echOS 2.1 /etc/doas.conf  (policy text; one address space, no isolation)\n"
    "permit persist keepenv :wheel\n"
    "permit nopass keepenv root as root\n";

static const char PF_CONF[] =
    "# echOS 2.1 /etc/pf.conf  (text dump only; pfctl does not filter packets)\n"
    "set skip on lo\n"
    "block in all\n"
    "pass out proto { tcp udp icmp } from any to any keep state\n"
    "pass in proto tcp to port { 22 80 443 } keep state\n";

static const char RC_CONF[] =
    "# echOS 2.1 /etc/rc.conf.local  (flags only; no daemon processes)\n"
    "httpd_flags=NO\n"
    "ftpd_flags=NO\n"
    "sshd_flags=NO\n"
    "pqc_flags=NO\n"
    "ntpd_flags=NO\n";

typedef struct { const char *name; const char *key; const char *desc; } svc_t;
static const svc_t SVCS[] = {
    { "httpd", "httpd_flags", "flag only — no HTTP daemon" },
    { "ftpd",  "ftpd_flags",  "flag only — no FTP daemon" },
    { "sshd",  "sshd_flags",  "flag only — no SSH daemon" },
    { "pqc",   "pqc_flags",   "flag only — no PQC tunnel process" },
    { "ntpd",  "ntpd_flags",  "flag only — no clock daemon" },
};
#define NSVC ((int)(sizeof(SVCS) / sizeof(SVCS[0])))

void bsdtools_init(void) {
    vfs_mkdir("/etc");
    if (!vfs_path_exists("/etc/doas.conf"))
        vfs_write("/etc/doas.conf", DOAS_CONF, (uint32_t)(sizeof(DOAS_CONF) - 1));
    if (!vfs_path_exists("/etc/pf.conf"))
        vfs_write("/etc/pf.conf", PF_CONF, (uint32_t)(sizeof(PF_CONF) - 1));
    if (!vfs_path_exists("/etc/rc.conf.local"))
        vfs_write("/etc/rc.conf.local", RC_CONF, (uint32_t)(sizeof(RC_CONF) - 1));
}

bool doas_permit(const char *user, const char *cmd) {
    uint32_t n = 0;
    const char *s = vfs_read("/etc/doas.conf", &n);
    (void)user; (void)cmd;
    if (!s || !n) return false;
    /* Any `permit` line is enough on a single-address-space kernel. */
    { uint32_t i;
      for (i = 0; i + 6 < n; i++)
        if (s[i]=='p'&&s[i+1]=='e'&&s[i+2]=='r'&&s[i+3]=='m'&&s[i+4]=='i'&&s[i+5]=='t')
            return true; }
    return false;
}

static int rc_enabled(const char *key) {
    uint32_t n = 0;
    const char *s = vfs_read("/etc/rc.conf.local", &n);
    uint32_t i;
    size_t klen;
    if (!s) return 0;
    klen = k_strlen(key);
    for (i = 0; i + klen + 1 < n; i++) {
        uint32_t j = 0;
        while (j < klen && s[i + j] == key[j]) j++;
        if (j != klen) continue;
        if (s[i + j] != '=') continue;
        j++;
        if (s[i + j] == 'N' || s[i + j] == '"') {
            /* NO or empty quotes => disabled. Non-NO => enabled. */
            if (s[i + j] == 'N' && s[i + j + 1] == 'O') return 0;
        }
        return 1;
    }
    return 0;
}

static void rc_set(const char *key, int on) {
    /* Rewrite the matching line; keep the rest. RXFS files are small. */
    uint32_t n = 0;
    const char *s = vfs_read("/etc/rc.conf.local", &n);
    char out[512];
    uint32_t i = 0, o = 0;
    int found = 0;
    size_t klen = k_strlen(key);
    if (!s) s = RC_CONF, n = (uint32_t)(sizeof(RC_CONF) - 1);
    while (i < n && o + 32 < sizeof(out)) {
        uint32_t start = i, j = 0;
        while (i < n && s[i] != '\n') i++;
        while (j < klen && start + j < i && s[start + j] == key[j]) j++;
        if (j == klen && start + j < i && s[start + j] == '=') {
            const char *v = on ? "YES\n" : "NO\n";
            uint32_t p = 0;
            while (p < klen && o + 1 < sizeof(out)) out[o++] = key[p++];
            out[o++] = '=';
            while (*v && o + 1 < sizeof(out)) out[o++] = *v++;
            found = 1;
        } else {
            while (start < i && o + 1 < sizeof(out)) out[o++] = s[start++];
            if (i < n && s[i] == '\n' && o + 1 < sizeof(out)) out[o++] = '\n';
        }
        if (i < n && s[i] == '\n') i++;
    }
    if (!found) {
        const char *v = on ? "YES\n" : "NO\n";
        uint32_t p = 0;
        while (p < klen && o + 1 < sizeof(out)) out[o++] = key[p++];
        out[o++] = '=';
        while (*v && o + 1 < sizeof(out)) out[o++] = *v++;
    }
    vfs_write("/etc/rc.conf.local", out, o);
}

static const svc_t *find_svc(const char *n) {
    int i;
    for (i = 0; i < NSVC; i++) if (eqs(SVCS[i].name, n)) return &SVCS[i];
    return NULL;
}

static void cmd_doas(const char *a1, const char *rest) {
    if (!a1 || !a1[0]) {
        o_s("usage: doas <command>\n");
        o_s("one address space; does not isolate.\n");
        return;
    }
    if (!doas_permit("root", a1)) {
        o_s("doas: Operation not permitted\n");
        o_s("check /etc/doas.conf\n");
        return;
    }
    o_s("doas: permitted  "); o_s(a1);
    if (rest && rest[0]) { o_s(" "); o_s(rest); }
    o_s("\n(single address space — command runs as the kernel shell)\n");
    o_s("re-type the command without doas; elevation is already the default.\n");
}

static void cmd_rcctl(const char *a1, const char *rest) {
    const svc_t *s;
    if (!a1 || !a1[0] || eqs(a1, "help")) {
        o_s("rcctl enable|disable|status|start|stop [httpd|ftpd|sshd|pqc|ntpd]\n");
        o_s("flags in /etc/rc.conf.local; this unikernel has no daemons\n");
        return;
    }
    if (eqs(a1, "ls") || (eqs(a1, "status") && (!rest || !rest[0]))) {
        int i;
        for (i = 0; i < NSVC; i++) {
            o_s(SVCS[i].name); o_s("  ");
            o_s(rc_enabled(SVCS[i].key) ? "enabled" : "disabled");
            o_s("  "); o_s(SVCS[i].desc); bsd_nl();
        }
        return;
    }
    s = find_svc(rest && rest[0] ? rest : "");
    if (!s && !eqs(a1, "status")) {
        o_s("rcctl: unknown service\n"); return;
    }
    if (eqs(a1, "enable"))  { rc_set(s->key, 1); o_s(s->name); o_s(" enabled\n"); return; }
    if (eqs(a1, "disable")) { rc_set(s->key, 0); o_s(s->name); o_s(" disabled\n"); return; }
    if (eqs(a1, "start")) {
        if (!rc_enabled(s->key)) { o_s(s->name); o_s(": not enabled (rcctl enable first)\n"); return; }
        o_s(s->name); o_s(": flag recorded; no daemon process in this unikernel\n"); return;
    }
    if (eqs(a1, "stop")) {
        rc_set(s->key, 0);
        o_s(s->name); o_s(": flag cleared; no daemon was running\n");
        return;
    }
    if (eqs(a1, "status")) {
        if (!s) { cmd_rcctl("ls", ""); return; }
        o_s(s->name); o_s("  "); o_s(rc_enabled(s->key) ? "enabled\n" : "disabled\n");
        return;
    }
    o_s("rcctl: unknown action\n");
}

static int g_pf_on = 1;

static void cmd_pfctl(const char *a1, const char *rest) {
    uint32_t n = 0;
    const char *rules = vfs_read("/etc/pf.conf", &n);
    (void)rest;
    if (!a1 || !a1[0] || eqs(a1, "-s") || eqs(a1, "status")) {
        o_s("pfctl: policy flag "); o_s(g_pf_on ? "ON" : "OFF");
        o_s("  (does not filter packets)\n");
        o_s("/etc/pf.conf is a text dump ("); o_d(n); o_s(" B), not a loaded ruleset\n");
        return;
    }
    if (eqs(a1, "-e") || eqs(a1, "enable")) {
        g_pf_on = 1; o_s("policy flag ON  (still not a packet filter)\n"); return;
    }
    if (eqs(a1, "-d") || eqs(a1, "disable")) {
        g_pf_on = 0; o_s("policy flag OFF  (still not a packet filter)\n"); return;
    }
    if (eqs(a1, "-f") || eqs(a1, "load")) {
        o_s("dump of /etc/pf.conf (not loaded into a filter)\n");
        if (rules && n) {
            uint32_t i, shown = n > 400 ? 400 : n;
            for (i = 0; i < shown; i++) {
                char c[2] = { rules[i], 0 };
                if (c[0] == '\n' || ((unsigned char)c[0] >= 0x20 && (unsigned char)c[0] != 0x7f)) o_s(c);
            }
            if (n > shown) o_s("\n...\n");
        }
        return;
    }
    if (eqs(a1, "-sr") || eqs(a1, "rules")) {
        if (rules && n) {
            uint32_t i;
            for (i = 0; i < n && o_len + 1 < o_max; i++) {
                char c = rules[i];
                if (c == '\n' || ((unsigned char)c >= 0x20 && (unsigned char)c != 0x7f)) {
                    char s[2] = { c, 0 }; o_s(s);
                }
            }
            if (o_len && o_buf[o_len - 1] != '\n') bsd_nl();
        }
        return;
    }
    o_s("pfctl -e|-d|-f|-s|-sr   (RAM boolean + dump; not a filter)\n");
}

static void cmd_disklabel(const char *dev) {
    blockdev_t *d = NULL;
    part_table_t t;
    int i;
    if (dev && dev[0]) d = blockdev_get(dev);
    if (!d) d = disk_dev();
    if (!d) { o_s("disklabel: no disk\n"); return; }
    o_s("# /dev/"); o_s(d->name); o_s("\n");
    o_s("type: EchOS\n");
    o_s("disk: "); o_s(d->name); bsd_nl();
    o_s("bytes/sector: 512\n");
    o_s("sectors: "); o_d(d->sectors); bsd_nl();
    if (!part_read(d, &t)) { o_s("disklabel: I/O error\n"); return; }
    o_s("label: "); o_s(t.gpt ? "gpt" : t.valid ? "mbr" : "none"); bsd_nl();
    o_s("16 partitions:\n");
    o_s("#        size   offset  fstype\n");
    for (i = 0; i < PART_MAX_PRIMARY; i++) {
        if (!t.e[i].used) continue;
        o_s("  "); o_s(d->name); o_d((uint64_t)(i + 1));
        o_s("   "); o_d(t.e[i].sectors);
        o_s("   "); o_d(t.e[i].start_lba);
        o_s("   "); o_s(part_type_name(t.e[i].type)); bsd_nl();
    }
}

static void cmd_gpt(const char *dev) {
    blockdev_t *d = (dev && dev[0]) ? blockdev_get(dev) : disk_dev();
    part_table_t t;
    if (!d) { o_s("gpt: no disk\n"); return; }
    if (!part_read(d, &t)) { o_s("gpt: I/O error\n"); return; }
    o_s("gpt "); o_s(d->name); o_s(": ");
    o_s(t.gpt ? "GPT present (EchOS install converts to MBR 0x7F)\n"
              : "no GPT header at LBA 1\n");
}

static void cmd_df(const char *a1) {
    int i, n;
    (void)a1;
    o_s("Filesystem      Size    Used   Avail  Mounted on\n");
    o_s("rxfs            ");
    o_d(RXFS_MAX_FILES * (uint64_t)RXFS_SIZE / 1024); o_s("K  ");
    o_d(vfs_used_bytes() / 1024); o_s("K  ");
    o_d((RXFS_MAX_FILES * (uint64_t)RXFS_SIZE - vfs_used_bytes()) / 1024);
    o_s("K  /\n");
    n = blockdev_count();
    for (i = 0; i < n; i++) {
        blockdev_t *d = blockdev_at(i);
        if (!d) continue;
        o_s("/dev/"); o_s(d->name);
        o_s("        ");
        o_d((uint64_t)d->sectors / 2); o_s("K      -      -  ");
        o_s(d->name); bsd_nl();
    }
}

static uint64_t du_walk(const char *path, int depth) {
    char names[RXFS_MAX_FILES][RXFS_NAME];
    int n, i;
    uint64_t total = 0;
    n = vfs_list_dir(path, names, RXFS_MAX_FILES);
    if (n < 0) {
        int sz = vfs_size(path);
        return sz > 0 ? (uint64_t)sz : 0;
    }
    for (i = 0; i < n; i++) {
        char full[RXFS_NAME];
        size_t k = 0, L = 0;
        const char *p = path;
        int isdir;
        while (names[i][L]) L++;
        isdir = (L && names[i][L - 1] == '/');
        if (path[0] == '/' && !path[1]) { full[k++] = '/'; }
        else {
            while (*p && k + 2 < sizeof(full)) full[k++] = *p++;
            if (k && full[k - 1] != '/') full[k++] = '/';
        }
        p = names[i];
        while (*p && *p != '/' && k + 1 < sizeof(full)) full[k++] = *p++;
        full[k] = '\0';
        if (isdir && depth < 8) {
            uint64_t sub = du_walk(full, depth + 1);
            total += sub;
            o_d(sub); o_s("\t"); o_s(full); bsd_nl();
        } else {
            int sz = vfs_size(full);
            if (sz < 0) sz = 0;
            total += (uint64_t)sz;
            o_d((uint64_t)sz); o_s("\t"); o_s(full); bsd_nl();
        }
    }
    return total;
}

static void cmd_du(const char *path) {
    const char *root = (path && path[0]) ? path : "/";
    uint64_t total = du_walk(root, 0);
    o_d(total); o_s("\t"); o_s(root); bsd_nl();
}

static void cmd_dysk(void) {
    int i, n = blockdev_count();
    o_s("NAME   SIZE     TYPE\n");
    for (i = 0; i < n; i++) {
        blockdev_t *d = blockdev_at(i);
        part_table_t t;
        if (!d) continue;
        o_s(d->name); o_s("   ");
        { uint64_t kib = (uint64_t)d->sectors / 2;
          if (kib >= 1024) { o_d(kib / 1024); o_s(" MiB   "); }
          else { o_d(kib); o_s(" KiB   "); } }
        if (is_ram(d)) o_s("ramdisk\n");
        else if (part_is_view(d)) o_s("partition\n");
        else if (part_read(d, &t) && t.gpt) o_s("disk (GPT)\n");
        else o_s("disk\n");
    }
}

static void cmd_less(const char *name) {
    uint32_t len = 0, i, shown = 0;
    const char *d;
    if (!name || !name[0]) { o_s("usage: less <file>\n"); return; }
    commands_v("dumping RXFS file (not GNU less, no pager keys)");
    d = vfs_read(name, &len);
    if (!d) { o_s("less: not found: "); o_s(name); bsd_nl(); return; }
    for (i = 0; i < len && shown < 900 && o_len + 1 < o_max; i++) {
        char c = d[i];
        if (c == '\n' || ((unsigned char)c >= 0x20 && (unsigned char)c != 0x7f)) {
            char s[2] = { c, 0 }; o_s(s); shown++;
        }
    }
    if (len > i) o_s("\n-- more --  (file truncated in pager)\n");
}

static void cmd_tail(const char *name) {
    uint32_t len = 0, i, lines = 0, start;
    const char *d;
    if (!name || !name[0]) { o_s("usage: tail <file>\n"); return; }
    d = vfs_read(name, &len);
    if (!d) { o_s("tail: not found\n"); return; }
    start = len;
    while (start > 0 && lines < 10) {
        start--;
        if (d[start] == '\n') lines++;
    }
    if (start && d[start] == '\n') start++;
    for (i = start; i < len && o_len + 1 < o_max; i++) {
        char c = d[i];
        if (c == '\n' || ((unsigned char)c >= 0x20 && (unsigned char)c != 0x7f)) {
            char s[2] = { c, 0 }; o_s(s);
        }
    }
    if (o_len && o_buf[o_len - 1] != '\n') bsd_nl();
}

static void cmd_grep(const char *pat, const char *file) {
    commands_v("search one RXFS file (not recursive rg)");
    uint32_t len = 0, i, line_start = 0, ln = 1;
    const char *d;
    size_t plen;
    if (!pat || !pat[0] || !file || !file[0]) {
        o_s("usage: grep <pattern> <file>\n"); return;
    }
    d = vfs_read(file, &len);
    if (!d) { o_s("grep: not found\n"); return; }
    plen = k_strlen(pat);
    for (i = 0; i <= len; i++) {
        if (i == len || d[i] == '\n') {
            uint32_t k;
            for (k = line_start; k + plen <= i; k++) {
                size_t p = 0;
                while (p < plen && d[k + p] == pat[p]) p++;
                if (p == plen) {
                    o_d(ln); o_s(":");
                    while (line_start < i && o_len + 1 < o_max) {
                        char c = d[line_start++];
                        char s[2] = { c, 0 };
                        if ((unsigned char)c >= 0x20 || c == '\t') o_s(s);
                    }
                    bsd_nl();
                    break;
                }
            }
            line_start = i + 1;
            ln++;
        }
    }
}

static void cmd_find(const char *path) {
    char names[RXFS_MAX_FILES][RXFS_NAME];
    const char *root = (path && path[0]) ? path : "/";
    int n, i;
    n = vfs_list_dir(root, names, RXFS_MAX_FILES);
    if (n < 0) { o_s("find: cannot open\n"); return; }
    o_s(root); bsd_nl();
    for (i = 0; i < n; i++) {
        o_s(root);
        if (root[0] && root[k_strlen(root) - 1] != '/') o_s("/");
        o_s(names[i]); bsd_nl();
    }
}

bool bsdtools_cmd(const char *name, const char *a1, const char *rest,
                  char *out, size_t outmax) {
    o_reset(out, outmax);
    if (eqs(name, "doas")) { cmd_doas(a1, rest); return true; }
    if (eqs(name, "rcctl")) { cmd_rcctl(a1, rest); return true; }
    if (eqs(name, "pfctl")) { cmd_pfctl(a1, rest); return true; }
    if (eqs(name, "disklabel")) { cmd_disklabel(a1); return true; }
    if (eqs(name, "gpt")) { cmd_gpt(a1); return true; }
    if (eqs(name, "df")) { cmd_df(a1); return true; }
    if (eqs(name, "du")) { cmd_du(a1[0] ? a1 : rest); return true; }
    if (eqs(name, "dysk")) { cmd_dysk(); return true; }
    if (eqs(name, "less")) { cmd_less(a1); return true; }
    if (eqs(name, "tail")) { cmd_tail(a1); return true; }
    if (eqs(name, "grep")) { cmd_grep(a1, rest); return true; }
    if (eqs(name, "find")) { cmd_find(a1); return true; }
    if (eqs(name, "htop")) {
        o_s("htop: not in this unikernel.  try: top\n");
        return true;
    }
    if (eqs(name, "tmux")) {
        o_s("tmux: not in this unikernel. one console.\n");
        return true;
    }
    if (eqs(name, "ncdu")) { cmd_du(a1); return true; }
    if (eqs(name, "ripgrep") || eqs(name, "rg")) {
        cmd_grep(a1, rest);
        return true;
    }
    return false;
}
