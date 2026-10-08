/*
 * System commands (help, status, ls, save, ...). Try not to crash on garbage
 * input; if you find a way, open an issue.
 */
#include "commands.h"
#include "report.h"
#include "arch.h"
#include "vfs.h"
#include "arch.h"
#include "heap.h"
#include "pmm.h"
#include "device_manager.h"
#include "scheduler.h"
#include "process.h"
#include "message.h"
#include "rx_event.h"
#include "power.h"
#include "thermal.h"
#include "ramdisk.h"
#include "disk.h"
#include "rxfs.h"
#include "init.h"
#include "netif.h"
#include "devfs.h"
#include "install.h"
#include "neurobench.h"
#include "gui.h"
#include "env.h"
#include "wired.h"
#include "framebuffer.h"
#include "font.h"
#include "pane_monitor.h"
#include "mono.h"
#include "robot.h"
#include "mavbridge.h"
#include "partition.h"
#include "rx_backend.h"
#include "pkg.h"
#include "clock.h"
#include "www.h"
#include "tls_min.h"
#include "tcp.h"
#include "window.h"
#include "ipstack.h"
#include "ipstack.h"
#include "update.h"
#include "version.h"
#include "handoff.h"
#include "browse.h"
#include "disks.h"
#include "gui.h"
#include "screen_capture.h"
#include "seb.h"
#include "heap0.h"
#include "wsp.h"
#include "cli.h"
#include "wizard.h"
#include "echofetch.h"
#include "epk.h"
#include "bsdtools.h"
#include "hwprobe.h"
#include "live.h"
#include "bench_neuromorphic.h"
#include "prisma5.h"
#include "echlibc.h"
#include "echos_arch.h"
#include "nano.h"
#include "man.h"
#include "netcli.h"

/* ------------------------- tiny output builder --------------------------- */
static char  *o_buf;
static size_t o_max, o_len;

static void o_reset(char *buf, size_t max) { o_buf = buf; o_max = max; o_len = 0; if (max) buf[0] = '\0'; }
/* Not static: userland/shell/install.c builds its wizard output through the
   same buffer, so the `install` command composes exactly like every other. */
void o_str(const char *s) {
    if (!o_buf || !o_max) return;
    while (*s && o_len + 1 < o_max) o_buf[o_len++] = *s++;
    o_buf[o_len] = '\0';
}
void o_dec(uint64_t v) {
    char t[21]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = '0' + (v % 10); v /= 10; }
    if (!o_buf || !o_max) return;
    while (i > 0 && o_len + 1 < o_max) o_buf[o_len++] = t[--i];
    o_buf[o_len] = '\0';
}
void o_nl(void) { o_str("\n"); }
static void o_pad(const char *s, int width) {
    int n = 0;
    o_str(s);
    for (const char *p = s; *p; p++) n++;
    for (; n < width && o_len + 1 < o_max; n++) o_buf[o_len++] = ' ';
    o_buf[o_len] = '\0';
}

static void o_dec_pad(uint64_t v, int width) {
    size_t start = o_len;
    o_dec(v);
    for (int n = (int)(o_len - start); n < width && o_len + 1 < o_max; n++)
        o_buf[o_len++] = ' ';
    o_buf[o_len] = '\0';
}

/* Bytes as MiB with one decimal. Integer division to whole MiB turned every
   small-but-real figure into a flat "0 MiB", which reads as "the reading is
   broken" and is indistinguishable from an actual zero. One decimal costs a
   multiply and removes the ambiguity: 4.1 MiB is obviously a measurement. */
static void o_mib(uint64_t bytes) {
    uint64_t whole = bytes / (1024 * 1024);
    uint64_t frac  = ((bytes % (1024 * 1024)) * 10) / (1024 * 1024);
    o_dec(whole); o_str("."); o_dec(frac); o_str(" MiB");
}

/* ---- verbose: what the kernel is doing --------------------------------- */
static int g_verbose = 1;

int commands_verbose(void) { return g_verbose; }

void commands_v(const char *s) {
    if (!g_verbose || !s || !s[0]) return;
    console_write("# ");
    console_write(s);
    console_write("\n");
}

static const char *v_blurb(const char *cmd) {
    static const struct { const char *n; const char *m; } T[] = {
        { "help", "command list for this unikernel" },
        { "about", "identity string from version.h" },
        { "status", "boot / subsystem card (OK only if that stage checked itself)" },
        { "mem", "Heap-0 regions + kmalloc 512 KiB arena + PMM" },
        { "limits", "compiled ceilings of this build; docs2.0/limites.md is the prose" },
        { "font", "typeface specimen: weights, blocks, box drawing" },
        { "pane", "split the console; one shell, several views" },
        { "robot", "sensor/intent pipeline, safety gate, queue counters" },
        { "px4", "MAVLink 2 link to a flight controller over UDP" },
        { "report", "the evidence block: every measured number, machine-readable" },
        { "uptime", "ticks since the periodic timer started" },
        { "date", "local wall clock (CMOS UTC + zone + DST)" },
        { "time", "alias of date" },
        { "tz", "read or set timezone; writes /etc/timezone" },
        { "timezone", "alias of tz" },
        { "power", "idle path + Intel RAPL sample if the MSRs exist" },
        { "devices", "probe table filled at boot (name / class / status)" },
        { "dev", "list /dev nodes (thermal, rapl, …)" },
        { "env", "volatile environment (lost on reboot unless saved)" },
        { "clear", "wipe framebuffer console + scrollback" },
        { "cls", "alias of clear" },
        { "reboot", "pulse 8042 reset — does not return" },
        { "halt", "stop the machine" },
        { "ls", "list RXFS directory (64 files × 64 KiB max)" },
        { "catalog", "alias of ls" },
        { "cd", "change RXFS working directory" },
        { "wander", "alias of cd" },
        { "pwd", "print RXFS working directory" },
        { "whereami", "alias of pwd" },
        { "cat", "read a file from RXFS (or a /dev node)" },
        { "unveil", "alias of cat" },
        { "write", "overwrite an RXFS file (truncated at 64 KiB)" },
        { "scribe", "alias of write" },
        { "rm", "delete an RXFS file" },
        { "vanish", "alias of rm" },
        { "cp", "copy one RXFS slot to another" },
        { "forge", "alias of cp" },
        { "copy", "alias of cp" },
        { "mv", "rename an RXFS file" },
        { "relabel", "alias of mv" },
        { "rename", "alias of mv" },
        { "mkdir", "create an RXFS directory" },
        { "homestead", "alias of mkdir" },
        { "rmdir", "remove an empty RXFS directory" },
        { "raze", "alias of rmdir" },
        { "tree", "walk RXFS from a path" },
        { "head", "first bytes of an RXFS file" },
        { "peep", "alias of head" },
        { "tail", "last lines of an RXFS file" },
        { "less", "dump an RXFS file (not GNU less)" },
        { "grep", "search one RXFS file" },
        { "find", "list one RXFS directory (not recursive POSIX find)" },
        { "du", "sum RXFS sizes under a path" },
        { "ncdu", "alias of du" },
        { "df", "RXFS used / slot count" },
        { "vault", "alias of df" },
        { "dysk", "block devices the kernel has bound" },
        { "nano", "TUI editor, 200×72, save with Ctrl+W — writes RXFS" },
        { "pico", "alias of nano" },
        { "history", "typed-command ring (RAM only)" },
        { "man", "embedded pages, not GNU man" },
        { "manual", "alias of man" },
        { "apropos", "search embedded man pages" },
        { "echofetch", "system card + Braille logo (free-to-use art)" },
        { "hwprobe", "PCI walk + disks + Akida 1e7c:bca1 probe" },
        { "live", "LIVE / RAM session card" },
        { "echlibc", "C library status for host-linked ELFs" },
        { "whoami", "USER env, else live" },
        { "id", "uid 0 in this unikernel (no ring-3)" },
        { "uname", "echOS version string" },
        { "hostname", "prints echos" },
        { "echo", "print arguments" },
        { "www", "IPv4 stack on/off (OFF by default)" },
        { "curl", "HTTP/1.0 client, one TCP; https needs WWW; body may be empty on TLS 1.3" },
        { "wget", "download URL into an RXFS file (64 KiB cap)" },
        { "wdl", "alias of wget" },
        { "dns", "DNS A lookup (needs www on)" },
        { "tls", "TLS handshake probe on :443 — not a full GET" },
        { "ping", "ICMP echo (slirp may drop it in QEMU)" },
        { "nics", "PCI network inventory; Wi-Fi chips are listed, not driven" },
        { "ipconf", "bound NIC: driver, MAC, IPv4, gateway, DNS" },
        { "ipconfig", "alias of ipconf" },
        { "ifconfig", "alias of ipconf" },
        { "trace", "ICMP traceroute or L2 hop" },
        { "traceroute", "alias of trace" },
        { "nmap", "TCP connect probe of a few ports (needs www on)" },
        { "wired", "Rogex Wired L2 (Ethertype 0x88B5)" },
        { "chat", "L2 chat frame to a peer" },
        { "say", "L2 broadcast chat" },
        { "epk", "RXFS package notes — not a user-process installer" },
        { "echos-install", "TUI wizard: LIVE or destructive disk install" },
        { "install", "write MBR + kernel + RXFS (needs ERASE- phrase)" },
        { "reinstall", "wipe leftover table and format" },
        { "partition", "read or write MBR type 0x7F" },
        { "cfdisk", "partition explorer" },
        { "fdisk", "alias of cfdisk" },
        { "disklabel", "print the bound disk geometry" },
        { "gpt", "GPT flag / table (writer is not a valid GPT yet)" },
        { "format", "empty RXFS on a named disk (needs yes)" },
        { "save", "serialize RXFS to the installed disk" },
        { "load", "replace RAM RXFS from disk" },
        { "doas", "re-dispatch if /etc/doas.conf permits; no isolation" },
        { "rcctl", "enable/disable flags in /etc/rc.conf.local; no daemons" },
        { "pfctl", "RAM boolean + dump /etc/pf.conf; not a packet filter" },
        { "sudoers", "print /etc/sudoers" },
        { "neuro", "how the in-kernel LIF / Q6 stack works" },
        { "neurocpu", "software LIF, or Akida if PCI 1e7c:bca1 is present" },
        { "prisma5", "synthetic drive of the 64-cell Q6 cube (not EEG hardware)" },
        { "bench-snn", "sparsity / latency; mW only if RAPL exists" },
        { "bench", "model vs kernel timing" },
        { "top", "scheduler tasks / event card" },
        { "rxmon", "alias of top" },
        { "capture", "tiny BMP under /screenshots" },
        { "kbd", "PS/2 layout es | us | mac" },
        { "termtheme", "console palette night|matrix|ocean|amber|paper" },
        { "termcolor", "alias of termtheme" },
        { "verbose", "toggle # trace lines (default on)" },
        { 0, 0 }
    };
    int i;
    if (!cmd) return 0;
    for (i = 0; T[i].n; i++) {
        const char *a = T[i].n, *b = cmd;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*a && !*b) return T[i].m;
    }
    return 0;
}

static bool eqs(const char *a, const char *b);

static int v_is_net(const char *cmd) {
    return eqs(cmd, "curl") || eqs(cmd, "wget") || eqs(cmd, "wdl") ||
           eqs(cmd, "dns") || eqs(cmd, "tls") || eqs(cmd, "ping") ||
           eqs(cmd, "nmap") || eqs(cmd, "trace") || eqs(cmd, "traceroute") ||
           eqs(cmd, "www");
}

static void v_announce(const char *cmd, const char *a1) {
    const char *msg;
    if (!g_verbose || !cmd || !cmd[0]) return;
    if (eqs(cmd, "verbose") || eqs(cmd, "clear") || eqs(cmd, "cls") ||
        eqs(cmd, "echo"))
        return;
    msg = v_blurb(cmd);
    if (!msg) return;
    console_write("# ");
    console_write(cmd);
    if (a1 && a1[0]) { console_write(" "); console_write(a1); }
    console_write(" — ");
    console_write(msg);
    console_write("\n");
    if (v_is_net(cmd) && !eqs(cmd, "www") && !www_enabled())
        commands_v("WWW is OFF. `www on` first or this will fail.");
    if ((eqs(cmd, "save") || eqs(cmd, "load") || eqs(cmd, "format") ||
         eqs(cmd, "install") || eqs(cmd, "reinstall") || eqs(cmd, "partition") ||
         eqs(cmd, "cfdisk") || eqs(cmd, "fdisk")) && !disk_present())
        commands_v("no persistent disk bound (LIVE is amnesic until install).");
}

static void cmd_verbose(const char *a1) {
    if (a1 && a1[0] && (eqs(a1, "off") || eqs(a1, "0") || eqs(a1, "quiet"))) {
        g_verbose = 0;
        o_str("verbose: off\n");
        return;
    }
    if (a1 && a1[0] && (eqs(a1, "on") || eqs(a1, "1"))) {
        g_verbose = 1;
        o_str("verbose: on  (# lines on the console before each command)\n");
        return;
    }
    o_str("verbose: "); o_str(g_verbose ? "on\n" : "off\n");
    o_str("  verbose on     # explain what the kernel is doing\n");
    o_str("  verbose off    # quiet\n");
}

/* 64-bit physical address in hex, zero-padded to 8 digits (wider if needed). */
static void o_addr(uint64_t v) {
    const char *hx = "0123456789ABCDEF";
    char b[19]; int n = 0;
    for (int sh = 60; sh >= 0; sh -= 4) {
        uint8_t nib = (uint8_t)((v >> sh) & 0xF);
        if (n || nib || sh <= 28) b[n++] = hx[nib];
    }
    b[n] = '\0';
    o_str("0x"); o_str(b);
}

/* ------------------------------ helpers ---------------------------------- */
static bool eqs(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
static void cpn(char *d, const char *s, size_t max) {
    size_t i = 0;
    for (; s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = '\0';
}
/* Split "cmd arg1 rest of line": returns arg1 ('\0' if none) and rest. */
static void split3(char *line, char **arg1, char **rest) {
    char *p = line;
    while (*p && *p != ' ') p++;
    if (*p) { *p++ = '\0'; while (*p == ' ') p++; }
    *arg1 = p;
    while (*p && *p != ' ') p++;
    if (*p) { *p++ = '\0'; while (*p == ' ') p++; }
    *rest = p;
}
#if !ECHOS_CLI_ONLY
static bool ends_rxc(const char *s) {
    size_t n = k_strlen(s);
    return n > 4 && s[n-4] == '.' && s[n-3] == 'r' && s[n-2] == 'x' && s[n-1] == 'c';
}
#endif

/* The command names, in one place. Tab completion needs a list, and a list
   maintained separately from the dispatcher is a list that goes stale — so
   this is the list, and `help` and the dispatcher both read from the same
   truth. */
static const char *const CMD_NAMES[] = {
    "help", "about", "status", "mem", "limits", "font", "pane", "robot", "px4", "report", "uptime", "power", "bench", "capture",
    "devices", "dev", "env", "clear", "cls", "reboot", "halt",
    "ls", "catalog", "cd", "wander", "pwd", "whereami",
    "cat", "unveil", "write", "scribe", "rm", "vanish",
    "cp", "forge", "mv", "relabel", "rename", "copy",
    "mkdir", "homestead", "rmdir", "raze", "tree", "head", "peep", "tail",
    "less", "grep", "find", "du", "ncdu", "df", "vault", "dysk",
    "date", "time", "tz", "timezone", "kbd", "termtheme", "termcolor",
    "man", "manual", "apropos", "history", "nano", "pico",
    "echofetch", "hwprobe", "live", "echlibc", "whoami", "id", "uname",
    "hostname", "echo",
    "www", "curl", "wget", "wdl", "dns", "tls", "ping", "nics",
    "ipconf", "ipconfig", "ifconfig", "trace", "traceroute", "nmap",
    "wired", "chat", "say",
    "epk", "echos-install", "install", "reinstall", "partition",
    "cfdisk", "fdisk", "disklabel", "gpt", "format", "save", "load",
    "doas", "rcctl", "pfctl", "sudoers",
    "neuro", "neurocpu", "prisma5", "bench-snn", "top", "rxmon",
    "verbose",
};
int commands_name_count(void) {
    return (int)(sizeof(CMD_NAMES) / sizeof(CMD_NAMES[0]));
}
const char *commands_name_at(int i) {
    return (i >= 0 && i < commands_name_count()) ? CMD_NAMES[i] : NULL;
}

static int tab_common(char extra[][48], int n) {
    int k = 0, i;
    if (n < 1) return 0;
    for (;;) {
        char c = extra[0][k];
        if (!c) return k;
        for (i = 1; i < n; i++) if (extra[i][k] != c) return k;
        k++;
    }
}

int commands_tab_complete(const char *buf, size_t n,
                          char *insert, size_t imax,
                          char extra[][48], int extra_max) {
    size_t start = 0, i, plen;
    const char *pre;
    int nm = 0, first_tok = 1;
    if (insert && imax) insert[0] = '\0';
    if (!buf) return 0;
    for (i = 0; i < n; i++) if (buf[i] == ' ') { start = i + 1; first_tok = 0; }
    pre = buf + start;
    plen = n - start;

    if (first_tok) {
        int c;
        for (c = 0; c < commands_name_count(); c++) {
            const char *nmstr = commands_name_at(c);
            size_t j;
            if (!nmstr) continue;
            for (j = 0; j < plen && nmstr[j] && nmstr[j] == pre[j]; j++) { }
            if (j != plen) continue;
            if (nm < extra_max) {
                size_t t = 0;
                while (nmstr[t] && t < 47) { extra[nm][t] = nmstr[t]; t++; }
                extra[nm][t] = '\0';
            }
            nm++;
        }
        if (nm == 1 && insert && imax) {
            const char *s = extra[0] + plen;
            size_t t = 0;
            while (s[t] && t + 2 < imax) { insert[t] = s[t]; t++; }
            insert[t++] = ' ';
            insert[t] = '\0';
        } else if (nm > 1 && insert && imax) {
            int cp = tab_common(extra, nm > extra_max ? extra_max : nm);
            size_t t = 0;
            while (plen + t < (size_t)cp && extra[0][plen + t] && t + 1 < imax) {
                insert[t] = extra[0][plen + t];
                t++;
            }
            insert[t] = '\0';
        }
        return nm;
    }

    /* Path complete. */
    {
        char dir[RXFS_NAME], names[RXFS_MAX_FILES][RXFS_NAME];
        const char *slash = 0;
        size_t j, dlen;
        int listed, k;
        for (j = 0; j < plen; j++) if (pre[j] == '/') slash = pre + j;
        if (slash) {
            dlen = (size_t)(slash - pre);
            if (dlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
            else {
                if (dlen + 1 > sizeof(dir)) dlen = sizeof(dir) - 1;
                for (j = 0; j < dlen; j++) dir[j] = pre[j];
                dir[dlen] = '\0';
            }
            pre = slash + 1;
            plen = n - (size_t)(pre - buf);
        } else {
            const char *cwd = vfs_getcwd();
            j = 0;
            if (cwd) while (cwd[j] && j + 1 < sizeof(dir)) { dir[j] = cwd[j]; j++; }
            dir[j] = '\0';
            if (!dir[0]) { dir[0] = '/'; dir[1] = '\0'; }
        }
        listed = vfs_list_dir(dir, names, RXFS_MAX_FILES);
        for (k = 0; k < listed; k++) {
            size_t L = 0, t;
            while (names[k][L] && names[k][L] != '/') L++;
            if (plen > L) continue;
            for (t = 0; t < plen && names[k][t] == pre[t]; t++) { }
            if (t != plen) continue;
            if (nm < extra_max) {
                t = 0;
                while (names[k][t] && t < 47) { extra[nm][t] = names[k][t]; t++; }
                extra[nm][t] = '\0';
            }
            nm++;
        }
        if (nm == 1 && insert && imax) {
            const char *s = extra[0] + plen;
            size_t t = 0;
            while (s[t] && t + 1 < imax) { insert[t] = s[t]; t++; }
            insert[t] = '\0';
        } else if (nm > 1 && insert && imax) {
            int cp = tab_common(extra, nm > extra_max ? extra_max : nm);
            size_t t = 0;
            while (plen + t < (size_t)cp && extra[0][plen + t] && t + 1 < imax) {
                insert[t] = extra[0][plen + t];
                t++;
            }
            insert[t] = '\0';
        }
    }
    return nm;
}

#define HIST_MAX 64
#define HIST_LEN 96
static char g_hist[HIST_MAX][HIST_LEN];
static int  g_hist_n;

void commands_hist_add(const char *line) {
    int i;
    if (!line || !line[0]) return;
    if (g_hist_n > 0) {
        const char *p = g_hist[g_hist_n - 1], *q = line;
        while (*p && *q && *p == *q) { p++; q++; }
        if (!*p && !*q) return;
    }
    if (g_hist_n == HIST_MAX) {
        for (i = 1; i < HIST_MAX; i++) {
            int k = 0;
            while (g_hist[i][k] && k < HIST_LEN - 1) {
                g_hist[i - 1][k] = g_hist[i][k]; k++;
            }
            g_hist[i - 1][k] = '\0';
        }
        g_hist_n = HIST_MAX - 1;
    }
    { int k = 0; while (line[k] && k < HIST_LEN - 1) { g_hist[g_hist_n][k] = line[k]; k++; }
      g_hist[g_hist_n][k] = '\0'; }
    g_hist_n++;
}
int commands_hist_count(void) { return g_hist_n; }
const char *commands_hist_at(int i) {
    if (i < 0 || i >= g_hist_n) return "";
    return g_hist[i];
}
const char *commands_hist_from_end(int n) {
    if (n < 1 || n > g_hist_n) return "";
    return g_hist[g_hist_n - n];
}

/* ------------------------------ commands --------------------------------- */
static void cmd_help(void) {
    o_str(ECHOS_PRETTY_NAME); o_nl();
    o_str("scroll  PgUp/PgDn  Shift+Up/Down  mouse wheel\n");
    o_str("copy    drag + Ctrl+C     paste  Ctrl+V / right-click\n");
    o_str("Tab completes names on this list.  man <cmd> for flags.\n \n");
    o_str("system\n");
    o_str("  help about status mem limits font uptime date tz power devices\n");
    o_str("  echofetch hwprobe live whoami uname hostname echo env\n");
    o_str("  kbd  termtheme  clear  reboot  halt\n");
    o_str("  font  pane split|vsplit|next|close|monitor   (Ctrl+Tab cycles panes)\n");
    o_str("  robot status|run|abi   sensor -> intent -> safety gate pipeline\n");
    o_str("  px4 start|stop|status  MAVLink 2 to a flight controller (UDP)\n");
    o_str("  verbose on|off  (default on — # lines say what the kernel is doing)\n");
    o_str("  date set YYYY-MM-DD HH:MM:SS     tz Europe/Madrid | tz list\n \n");
    o_str("files  (RXFS: 64 slots x 64 KiB)\n");
    o_str("  ls cd pwd cat write rm cp mv mkdir rmdir tree\n");
    o_str("  head tail less grep find du df nano history\n \n");
    o_str("disk\n");
    o_str("  echos-install   TUI wizard (LIVE).  install / reinstall  ERASE-\n");
    o_str("  partition cfdisk format save load disklabel\n \n");
    o_str("net  (off until `www on`)\n");
    o_str("  ipconf nics ping dns tls curl wdl wget nmap trace\n");
    o_str("  www on|off|status|config     wired chat say\n \n");
    o_str("packages / policy\n");
    o_str("  epk list | list --lab | info | install | remove | apply\n");
    o_str("  doas <cmd>      /etc/doas.conf  (no isolation; one address space)\n");
    o_str("  rcctl           enable/disable flags only; no daemons in this kernel\n");
    o_str("  pfctl           RAM boolean + dump of /etc/pf.conf; not a filter\n \n");
    o_str("SNN in-kernel  (not Echo, not an LLM, not EEG hardware)\n");
    o_str("  neuro           explainer\n");
    o_str("  neurocpu        software | akida (PCI probe; no offload without a card)\n");
    o_str("  prisma5         synthetic drive of the 64-cell Q6 cube\n");
    o_str("  bench-snn       sparsity / latency; mW only if RAPL exists\n \n");
    o_str("not here  (names removed so Tab/help do not lie)\n");
    o_str("  no desktop, no chatbot, no LLM, no sshd/httpd, no python/tcc/git\n");
    o_str("  no browser, no htop/tmux, no Wi-Fi, no USB, no Loihi\n");
    o_str("  not a commercial OS; not for surfing the web. robotics / SNN.\n");
}

static void cmd_about(void) {
    char ver[24];
    update_runtime_version(ver, sizeof(ver));
    o_str(ECHOS_PRETTY_NAME); o_nl();
    o_str(ECHOS_SLOGAN); o_nl();
    o_str(ECHOS_VENDOR); o_nl(); o_nl();
    o_str("Robotic edge unikernel. Bare metal, Heap-0, JetBrains Mono, local epk.\n");
    o_str("Sensor -> intent -> safety gate, on static memory, with the numbers\n");
    o_str("measured (`robot`, `px4`). The autopilot stays the flight controller.\n");
    o_str("Not a browser. Not a chatbot. Not a commercial OS.\n");
    o_str("No WM, no desktop. GRUB/Multiboot2 -> long mode. C + Rust no_std + NASM.\n \n");
    o_str("Packages: `epk list`  (kernel notes).  `epk list --lab` = host notes.\n");
    o_str("Install:  `echos-install` from LIVE writes a disk.\n");
    o_str("ISO kernel: "); o_str(ECHOS_VERSION); o_str("  runtime: "); o_str(ver); o_nl();
    { char stamp[24]; clock_fmt_stamp(stamp, sizeof(stamp));
      o_str("Clock: "); o_str(stamp); o_nl(); }
}

static void cmd_status(void) {
    o_str(ECHOS_VERSION_NAME); o_str(" — status\n \n");
    o_pad("subsystem", 14); o_pad("state", 34); o_str("notes\n");
    o_pad("boot", 14);      o_pad("OK (reached kmain)", 34); o_str("boot/boot.asm\n");
    /* Which firmware actually booted us, from the handoff, not a build flag. */
    o_pad("firmware", 14);
    { const boot_handoff_t *bh = boot_handoff();
      char card[34]; int n = 0;
      const char *fw = boot_mode_string();
      while (fw[n] && n < 33) { card[n] = fw[n]; n++; }
      card[n] = '\0';
      o_pad(card, 34);
      o_str(bh->fw_config_table ? "EFI config table" : "no config table");
      o_str(bh->fw_services_active ? ", boot services STILL ACTIVE" : "");
      o_str("  "); o_dec(bh->mem_count); o_str(" memory ranges, ");
      o_dec(bh->module_count); o_str(" modules\n"); }
    o_pad("gdt", 14);       o_pad("OK (long mode running)", 34);  o_str("arch/x86_64/gdt.h\n");
    o_pad("idt", 14);
    if (arch_timer_ticks() > 0)
        o_pad("OK (IRQ0 delivered)", 34);
    else
        o_pad("FAIL (no PIT ticks)", 34);
    o_str("arch/x86_64/\n");
    o_pad("heap-0", 14);
    if (heap0_ready())
        o_pad("OK (WSP 64 KiB BSS)", 34);
    else
        o_pad("FAIL", 34);
    o_str("kmalloc 512 KiB still exists\n");
    o_pad("memory", 14);    o_str("OK  kernel ");
    o_mib(pmm_used_bytes());
    o_str(" in use / ");
    o_mib(pmm_total_bytes());
    o_str(" in this PC  heap ");
    o_dec(heap_total() / 1024); o_str(" KiB\n");
    o_pad("timer", 14);     o_pad("OK (PIT 100 Hz, IRQ0)", 34); o_str("ticks="); o_dec(arch_timer_ticks()); o_nl();
    if (events_irq_active()) {
        o_pad("keyboard", 14); o_pad("OK (PS/2, IRQ1 -> event fabric)", 34);  o_str("idle = hlt\n");
        o_pad("mouse", 14);    o_pad("OK (PS/2, IRQ12 -> event fabric)", 34); o_str("200 Hz\n");
    } else {
        o_pad("keyboard", 14); o_pad("OK (PS/2, polled fallback)", 34); o_nl();
        o_pad("mouse", 14);    o_pad("OK (PS/2, polled fallback)", 34); o_nl();
    }
    o_pad("vfs/rxfs", 14);  o_str("OK  ");
    o_dec((uint64_t)vfs_count()); o_str(" files, "); o_dec(vfs_used_bytes()); o_str(" bytes\n");
    o_pad("scheduler", 14); o_pad(scheduler_status(), 34);
    o_str("tasks="); o_dec((uint64_t)scheduler_task_count());
    o_str(" switches="); o_dec(sched_switches()); o_nl();
    /* Stack headroom is reported because running out of it corrupts unrelated
       globals and surfaces as a fault somewhere else entirely. */
    o_pad("task stacks", 14);
    { int used = sched_stack_worst_used();
      if (!sched_stacks_intact()) {
          o_pad("*** OVERFLOWED ***", 34);
          o_str("a task wrote past its stack - globals may be corrupt\n");
      } else {
          o_pad("guards intact", 34);
          o_str("deepest "); o_dec((uint64_t)used); o_str(" B of ");
          o_dec((uint64_t)SCHED_STACK_SIZE); o_str(" ("); o_str(sched_stack_worst_task());
          o_str(")\n");
      } }
    o_pad("idle power", 14); o_pad(power_idle_method(), 34);
    if (power_acpi_present()) { o_str("ACPI "); o_str(power_acpi_oem()); }
    else                      { o_str("no ACPI"); }
    o_str(", parks="); o_dec(power_idle_entries()); o_nl();
    o_pad("energy", 14);
    { uint64_t uj;
      if (power_rapl_pkg_uj(&uj)) { o_pad(power_rapl_status(), 34);
                                    o_dec(uj / 1000); o_str(" mJ so far - try `power`\n"); }
      else                        { o_pad(power_rapl_status(), 34); o_str("kernel/power/\n"); } }
    o_pad("syscalls", 14);  o_pad("STUB (direct dispatch)", 34); o_str("kernel/syscall/\n");
    /* The event fabric is now the input transport; report its real counters. */
    o_pad("event fabric", 14);
    if (events_irq_active()) {
        o_pad("in use (input + net actors)", 34);
        o_str("in="); o_dec(rx_input_events());
        o_str(" drop="); o_dec((uint64_t)rx_input_dropped()); o_nl();
    } else {
        o_pad("built, input polled instead", 34); o_str("kernel/event/\n");
    }
    o_pad("SNN", 14);
    o_pad(rx_backend_label(), 34);
    o_str(rx_backend_status()); o_str("  `neuro`\n");
    /* The legacy IPC queue is superseded: since the input migration nothing
       produces into it. Kept (self-tested at boot) as a general-purpose
       primitive, but it is no longer on any live path — say so. */
    o_pad("ipc (legacy)", 14);
    o_pad("superseded by the event fabric", 34); o_str("kernel/ipc/\n");
    o_pad("persistence", 14);
    if (disk_present()) { o_pad("OK", 34); o_str(disk_backend()); o_nl(); }
    else                { o_pad("no disk attached (amnesic)", 34); o_str("make run-disk\n"); }
    o_pad("network", 14);
    if (net_present()) { o_pad("OK", 34); o_str(net_driver_name()); o_str(" (Rogex Wired L2, 0x88B5)\n"); }
    else               { o_pad("no NIC attached", 34); o_str("-device virtio-net-pci | e1000\n"); }
    o_pad("updates", 14);
    { char ver[24]; update_runtime_version(ver, sizeof(ver));
      o_pad("file-patch channel", 34);
      o_str("runtime "); o_str(ver); o_str("  `rx update check`\n"); }
}

/* The firmware's own table, unsummarised. When a total looks wrong this is
   the only thing that settles it: either the firmware said something odd, or
   our arithmetic did, and the two are distinguishable here. */
static void cmd_mem_map(void) {
    int n = pmm_region_count(), seen = pmm_region_total_seen();
    o_str("firmware memory map ("); o_dec((uint64_t)seen);
    o_str(" regions, as Multiboot2 reported them)\n \n");
    o_pad("start", 20); o_pad("size", 14); o_str("type\n");
    for (int i = 0; i < n; i++) {
        const pmm_region_t *r = pmm_region(i);
        if (!r) continue;
        { size_t before = o_len; o_addr(r->base);
          for (size_t k = o_len - before; k < 20; k++) o_str(" "); }
        { size_t before = o_len; o_mib(r->len);
          for (size_t k = o_len - before; k < 14; k++) o_str(" "); }
        switch (r->type) {
        case 1:  o_str("available\n");         break;
        case 3:  o_str("ACPI reclaimable\n");  break;
        case 4:  o_str("ACPI NVS\n");          break;
        case 5:  o_str("bad RAM\n");           break;
        default: o_str("reserved\n");          break;
        }
    }
    if (seen > n) {
        o_str(" \n*** TABLE TRUNCATED: showing "); o_dec((uint64_t)n);
        o_str(" of "); o_dec((uint64_t)seen); o_str(" regions ***\n");
        o_str("The totals below still count every region - only this listing\n");
        o_str("is capped. Raise PMM_MAX_REGIONS if you need the rest.\n");
    }
    o_str(" \nOnly `available` regions count toward the totals in `mem`, and\n");
    o_str("only the part of them below 4 GiB is addressable today.\n");
}

static void cmd_mem(const char *arg) {
    if (eqs(arg, "map")) { cmd_mem_map(); return; }
    o_str("memory\n \n");
    o_str("kernel using (this is the only consumption figure)\n");
    o_str("  frames     "); o_mib(pmm_used_bytes());
    o_str("   bump allocator since boot\n");
    o_str("  heap       "); o_dec(heap_used()); o_str(" B / ");
    o_dec(heap_total() / 1024); o_str(" KiB\n");
    o_str("  console    "); o_mib(fb_backbuffer_bytes());
    o_str("   framebuffer backbuffer");
    if (fb_active()) {
        uint32_t w = 0, h = 0;
        fb_dims(&w, &h);
        o_str(" ("); o_dec(w); o_str("x"); o_dec(h); o_str("x32)\n");
    } else o_str(" (VGA text, no LFB)\n");
#if ECHOS_CLI_ONLY
    o_str("  desktop    none — " ECHOS_VERSION_NAME " CLI has no compositor\n");
#else
    o_str("  desktop    "); o_mib(gui_bytes());
    o_str("   wallpaper + scene cache\n");
#endif
    o_str(" \nthis PC (NOT used by the kernel — size of the machine)\n");
    o_str("  firmware   "); o_mib(pmm_total_bytes());
    o_str("   Multiboot2 available RAM\n");
    o_str("  we can map "); o_mib(pmm_usable_bytes());
    o_str("   below 4 GiB identity map\n");
    o_str("  still free "); o_mib(pmm_free_bytes());
    o_str("   bump pointer has not touched this\n \n");
    o_str("A 2 GiB line is the laptop's RAM under 4 GiB, not a 2 GiB leak.\n");
    o_str("`mem map` prints the firmware table if a total looks odd.\n \n");
    o_str("rxfs:      "); o_dec((uint64_t)vfs_count()); o_str(" files   ");
    o_dec(vfs_used_bytes()); o_str(" bytes used   (max ");
    o_dec(RXFS_MAX_FILES); o_str(" files x "); o_dec(RXFS_SIZE); o_str(" B)\n");
    o_str("ramdisk:   "); o_dec(RAMDISK_BYTES / 1024); o_str(" KiB block device (ram0)\n");
    o_str("SNN:       backend "); o_str(rx_backend_label());
    o_str("  "); o_str(rx_backend_status()); o_str("  (`neuro`)\n");
}





/* The PX4 link. Starting it binds a UDP port, attaches the bridge as the
   runtime's transport, and from then on telemetry becomes sensor records and
   approved intents leave as MAVLink. The autopilot keeps every authority it
   had: this end only asks. */
static void cmd_px4(const char *a1, const char *rest) {
    const mavbridge_stats_t *st;
    const mavlink_rx_stats_t *rx;
    uint8_t ip[4] = { 10, 0, 2, 2 };
    uint16_t port = 14580, local = 14550;

    if (eqs(a1, "start")) {
        char host[24];
        const char *p = rest;
        int n = 0;
        while (p && *p == ' ') p++;
        while (p && *p && *p != ' ' && n < 23) host[n++] = *p++;
        host[n] = '\0';
        if (n && !ip_parse(host, ip)) { o_str("px4: bad address\n"); return; }
        while (p && *p == ' ') p++;
        if (p && *p >= '0' && *p <= '9') {
            port = 0;
            while (*p >= '0' && *p <= '9') port = (uint16_t)(port * 10 + (*p++ - '0'));
        }
        if (!ipstack_configured()) {
            commands_v("no IPv4 yet: DHCP, then QEMU defaults if that fails");
            if (!ip_dhcp(4000)) ipstack_use_qemu_defaults();
        }
        if (!mavbridge_start(ip, port, local)) { o_str("px4: could not start\n"); return; }
        o_str("px4: MAVLink 2 over UDP to ");
        { char b[20]; ip_fmt(ip, b, sizeof(b)); o_str(b); }
        o_str(":"); o_dec(port); o_str(" from local port "); o_dec(local);
        o_str("\n     heartbeat 1 Hz, setpoints 20 Hz, link timeout 3 s\n");
        return;
    }
    if (eqs(a1, "offboard")) {
        if (!mavbridge_offboard()) { o_str("px4: link is not running\n"); return; }
        o_str("px4: asked PX4 to enter Offboard. It decides; watch the ack and\n");
        o_str("     its mode. From then on losing this stream is PX4's failsafe,\n");
        o_str("     not ours.\n");
        return;
    }
    if (eqs(a1, "stop")) {
        mavbridge_stop();
        o_str("px4: link closed, transport detached\n");
        return;
    }
    if (eqs(a1, "poll")) {
        uint32_t f = 0, i;
        for (i = 0; i < 200; i++) f += mavbridge_poll();
        o_str("px4: polled, "); o_dec(f); o_str(" frames this burst\n");
        return;
    }
    if (a1 && a1[0] && !eqs(a1, "status")) {
        o_str("usage: px4 [start [ip] [port]|offboard|stop|status|poll]\n");
        return;
    }

    st = mavbridge_stats();
    rx = mavbridge_rx_stats();
    o_str("\033[1mpx4 link\033[0m  ");
    o_str(mavbridge_running() ? "running" : "stopped");
    o_str("   state "); o_str(st->link_up ? "UP" : "DOWN");
    o_str("   drops to down "); o_dec(st->link_drops);
    o_nl();
    o_str("  peer      sysid "); o_dec(st->peer_sysid);
    o_str(" compid "); o_dec(st->peer_compid);
    o_str("   mode "); o_addr(st->autopilot_mode);
    o_str("   armed "); o_str(st->autopilot_armed ? "yes" : "no");
    o_nl();
    /* A link timeout is only as honest as the clock behind it: if the
       monotonic source runs fast, silence is declared that never happened. */
    o_str("  clock     mono "); o_dec(mono_us() / 1000);
    o_str(" ms   pit "); o_dec(arch_timer_uptime_ms());
    o_str(" ms   last heartbeat "); o_dec(st->last_heartbeat_us / 1000);
    o_str(" ms\n");
    o_str("  heartbeat gap  last "); o_dec(st->last_hb_gap_us / 1000);
    o_str(" ms   worst "); o_dec(st->max_hb_gap_us / 1000);
    o_str(" ms   (timeout 3000 ms)\n");
    o_str("  frames    rx "); o_dec(st->rx_frames);
    o_str("  tx "); o_dec(st->tx_frames);
    o_str("   heartbeats rx "); o_dec(st->heartbeats_rx);
    o_str(" tx "); o_dec(st->heartbeats_tx);
    o_nl();
    o_str("  telemetry heartbeat "); o_dec(st->rx_by_type[0]);
    o_str("  sys_status "); o_dec(st->rx_by_type[1]);
    o_str("  battery "); o_dec(st->rx_by_type[2]);
    o_str("  attitude "); o_dec(st->rx_by_type[3]);
    o_str("\n            local_pos "); o_dec(st->rx_by_type[4]);
    o_str("  global_pos "); o_dec(st->rx_by_type[5]);
    o_str("  distance "); o_dec(st->rx_by_type[6]);
    o_str("  command_ack "); o_dec(st->rx_by_type[7]);
    o_nl();
    o_str("  streams   silenced "); o_dec(mavbridge_quieted());
    o_str(" message types we cannot use (asked once each)\n");
    o_str("  errors    crc "); o_dec(rx->crc_errors);
    o_str("  unknown id "); o_dec(rx->unknown_msgid);
    o_str("  rejected incompat "); o_dec(rx->rejected_incompat);
    o_str("  stray bytes "); o_dec(rx->bad_stx);
    o_nl();
    o_str("  loss      sequence gaps "); o_dec(rx->seq_gaps);
    o_str("  frames missed "); o_dec(rx->lost_estimate);
    o_str("   our own ring drops "); o_dec(ip_udp_ring_drops());
    o_nl();
    o_str("  pipeline  sensor records "); o_dec(st->sensors_produced);
    o_str("   intents sent "); o_dec(st->intents_sent);
    o_str("   acks "); o_dec(st->command_acks);
    if (st->command_acks) {
        o_str(" (last cmd "); o_dec(st->last_ack_command);
        o_str(" result "); o_dec(st->last_ack_result); o_str(")");
    }
    o_str("\n \nPX4 keeps stabilisation, estimation, mixing, failsafes, geofence,\n");
    o_str("return and landing. This end sends high-level intent only.\n");
}

/* The robotic runtime, in the only terms that mean anything: what went in,
   what the gate did about it, and how deep the queues got. Nothing here is
   remembered — every number is read from the runtime when it prints. */
static void robot_queue_row(const char *label, const rqueue_t *q) {
    o_str("  "); o_pad(label, 20);
    o_str("cap "); o_dec_pad(q->capacity, 5);
    o_str("depth "); o_dec_pad(rqueue_depth(q), 5);
    o_str("high "); o_dec_pad(q->high_water, 5);
    o_str("in "); o_dec_pad(q->pushed, 6);
    o_str("out "); o_dec_pad(q->popped, 6);
    o_str("drops "); o_dec_pad(q->drops, 5);
    o_str("expired "); o_dec(q->expired);
    o_nl();
}

static void cmd_robot(const char *a1, const char *rest) {
    const robot_stats_t *st;
    const safety_stats_t *sf;
    const robot_latency_t *lat;
    const safety_limits_t *lim;
    (void)rest;

    if (eqs(a1, "run") || eqs(a1, "scenario")) {
        uint32_t n = robot_scenario();
        o_str("scenario: injected "); o_dec(n);
        o_str(" records through the pipeline\n \n");
    } else if (eqs(a1, "reset")) {
        robot_reset();
        o_str("runtime counters cleared\n");
        return;
    } else if (eqs(a1, "abi")) {
        o_str("Sensor ABI v"); o_dec(SENSOR_ABI_VERSION);
        o_str("   record "); o_dec(sizeof(sensor_event_t));
        o_str(" B fixed: version size type source quality flags seq t_us payload[40]\n");
        o_str("  types   ");
        for (uint8_t t = 1; t < SENSOR_TYPE_COUNT; t++) {
            o_str(sensor_type_name(t)); o_str(" ");
        }
        o_str("\n \nIntent ABI v"); o_dec(INTENT_ABI_VERSION);
        o_str("   record "); o_dec(sizeof(intent_t));
        o_str(" B fixed: version size kind reason confidence flags\n");
        o_str("            seq cause_seq cause_source cause_type issued_us deadline_us\n");
        o_str("            max_speed_mms max_range_mm x_mm y_mm z_mm\n");
        o_str("  kinds   ");
        for (uint8_t k = 0; k < INTENT_KIND_COUNT; k++) {
            o_str(intent_kind_name(k)); o_str(" ");
        }
        o_str("\n \nNo PWM, no servo, no throttle, no channel: an intent cannot\n");
        o_str("express a motor command. The autopilot owns the actuators.\n");
        return;
    } else if (a1 && a1[0] && !eqs(a1, "status")) {
        o_str("usage: robot [status|run|abi|reset]\n");
        return;
    }

    st = robot_stats();
    sf = safety_stats();
    lat = robot_latency();
    lim = safety_limits();

    o_str("\033[1mpipeline\033[0m  sensors -> runtime -> intents -> gate -> autopilot\n \n");
    robot_queue_row("sensor->runtime", robot_q_sensor());
    robot_queue_row("runtime->intents", robot_q_intent());
    robot_queue_row("intents->gate", robot_q_gate());
    robot_queue_row("gate->autopilot", robot_q_out());

    o_str(" \n\033[1mrecords\033[0m   accepted "); o_dec(st->sensors_in);
    o_str("   refused at the door "); o_dec(st->sensors_rejected);
    o_str("   intents made "); o_dec(st->intents_made);
    o_str("\n          sent "); o_dec(st->sent);
    o_str("   delivered to transport "); o_dec(st->delivered);
    o_str(" ("); o_str(robot_transport_attached() ? "attached" : "none attached");
    o_str(")");
    o_str("   expired before sending "); o_dec(st->intents_expired);
    o_str("   watchdog "); o_dec(st->watchdog_intents);

    o_str("\n \n\033[1mgate\033[0m      seen "); o_dec(sf->seen);
    o_str("   accepted unchanged "); o_dec(sf->accepted);
    o_str("   MODIFY "); o_dec(sf->modified);
    o_str("   BLOCK "); o_dec(sf->blocked);
    o_str("\n          reasons:");
    for (uint8_t r = 1; r < SAFETY_R_COUNT; r++) {
        if (!sf->by_reason[r]) continue;
        o_str(" "); o_str(safety_reason_name(r));
        o_str("="); o_dec(sf->by_reason[r]);
    }
    o_str("\n          sent as:");
    for (uint8_t k = 0; k < INTENT_KIND_COUNT; k++) {
        if (!sf->out_kind[k]) continue;
        o_str(" "); o_str(intent_kind_name(k));
        o_str("="); o_dec(sf->out_kind[k]);
    }
    o_str("\n          an intent the gate corrected is not credited to the producer\n");

    o_str(" \n\033[1mlatency\033[0m   samples "); o_dec(lat->count);
    if (lat->count) {
        o_str("   min "); o_dec(lat->min_us);
        o_str("  p50 <"); o_dec(robot_latency_percentile(500));
        o_str("  p95 <"); o_dec(robot_latency_percentile(950));
        o_str("  p99 <"); o_dec(robot_latency_percentile(990));
        o_str("  max "); o_dec(lat->max_us);
        o_str(" us\n          jitter (p95-p50) <");
        o_dec(robot_latency_percentile(950) - robot_latency_percentile(500));
        o_str(" us   clock: "); o_str(mono_source());
        o_str(", "); o_dec(mono_resolution_ns()); o_str(" ns\n");
        o_str("          percentiles are bucket upper bounds, not interpolated\n");
    } else {
        o_str("   (nothing has gone through yet: `robot run`)\n");
    }

    o_str(" \n\033[1mlimits\033[0m    battery return<="); o_dec(lim->battery_return_percent);
    o_str("%  land<="); o_dec(lim->battery_land_percent);
    o_str("%   input max age "); o_dec(lim->input_max_age_us / 1000); o_str(" ms\n");
    o_str("          link timeout "); o_dec(lim->link_timeout_us / 1000);
    o_str(" ms   watchdog "); o_dec(lim->watchdog_us / 1000);
    o_str(" ms   speed<="); o_dec(lim->max_speed_mms); o_str(" mm/s\n");
    o_str("          kmalloc calls since boot "); o_dec(heap_alloc_calls());
    o_str("   (this pipeline must not move that number)\n");
    o_str("          geofence "); o_str(lim->geofence_on ? "on" : "off");
    o_str("   producer: reflex policy (placeholder, not an intelligence)\n");
}

/* Split console. Terminator's model, with this system's honesty: there is
   one shell, and it types into the focused pane. A second shell would need
   the shell's own state to be per-pane, which it is not — so `pane` says
   what each pane is instead of implying two prompts. */
static void cmd_pane(const char *a1, const char *rest) {
    (void)rest;
    if (!a1 || !a1[0] || eqs(a1, "status")) {
        int i, cols, rows;
        o_str("panes  "); o_dec((uint64_t)console_pane_count());
        o_str(" of 4   focused #"); o_dec((uint64_t)console_pane_focused());
        o_nl();
        for (i = 0; i < 4; i++) {
            int kind = console_pane_kind(i);
            if (kind < 0) continue;
            console_pane_geometry(i, &cols, &rows);
            o_str(i == console_pane_focused() ? "  \033[1m>" : "   ");
            o_str(" #"); o_dec((uint64_t)i);
            o_str("  "); o_dec((uint64_t)cols); o_str("x"); o_dec((uint64_t)rows);
            o_str("  ");
            o_str(kind == CONSOLE_PANE_MONITOR ? "monitor (refreshes while you type)"
                                               : "shell output");
            o_str(i == console_pane_focused() ? "\033[0m\n" : "\n");
        }
        o_str("scroll  "); o_dec((uint64_t)console_scroll_offset());
        o_str(" back of "); o_dec((uint64_t)console_scroll_max());
        o_str(" rows   monitor refreshes "); o_dec((uint64_t)pane_monitor_refreshes());
        o_nl();
        o_str(" \n  pane split     side by side      pane vsplit   stacked\n");
        o_str("  pane next      Ctrl+Tab too      pane close    give the space back\n");
        o_str("  pane scroll up|down [rows]           PgUp / wheel do the same\n");
        o_str("  pane monitor   live system card in this pane\n");
        o_str("  pane shell     back to shell output\n \n");
        o_str("One shell task writes to the focused pane. The others keep what they\n");
        o_str("have; a monitor pane redraws itself while the shell waits for a key.\n");
        return;
    }
    if (eqs(a1, "split") || eqs(a1, "h")) {
        if (console_pane_split(1) < 0) o_str("pane: no room (4 maximum)\n");
        return;
    }
    if (eqs(a1, "vsplit") || eqs(a1, "v")) {
        if (console_pane_split(2) < 0) o_str("pane: no room (4 maximum)\n");
        return;
    }
    if (eqs(a1, "scroll")) {
        /* The same call PgUp and the wheel make, so this reports the real
           thing. Note that typing anything afterwards jumps back to the live
           tail — that is the terminal being right, not the scroll failing. */
        int rows = 8, sign = 1;
        const char *p = rest;
        while (p && *p == ' ') p++;
        if (p && (*p == 'd' || *p == 'D')) sign = -1;
        if (p) { while (*p && *p != ' ') p++; while (*p == ' ') p++;
                 if (*p >= '0' && *p <= '9') { rows = 0;
                     while (*p >= '0' && *p <= '9') rows = rows * 10 + (*p++ - '0'); } }
        console_scroll(sign * rows);
        o_str("scrolled: now "); o_dec((uint64_t)console_scroll_offset());
        o_str(" rows back of "); o_dec((uint64_t)console_scroll_max());
        o_str(" (typing returns to the live tail)\n");
        return;
    }
    if (eqs(a1, "next") || eqs(a1, "n")) { console_pane_next(); return; }
    if (eqs(a1, "close") || eqs(a1, "q")) {
        if (!console_pane_close()) o_str("pane: the last pane stays\n");
        return;
    }
    if (eqs(a1, "monitor")) {
        console_pane_set_kind(CONSOLE_PANE_MONITOR);
        pane_monitor_refresh();
        return;
    }
    if (eqs(a1, "shell")) { console_pane_set_kind(CONSOLE_PANE_SHELL); return; }
    o_str("usage: pane [split|vsplit|next|close|monitor|shell]\n");
}

/* What this console can actually draw. A specimen, not a claim: every line
   below is printed through the same path as any other output, so if a block
   is missing from the baked face it shows up here as U+FFFD. */
static int hex_nibble(char c);

/* `font check 00F1 2500 28FF` — is that codepoint drawable, does it have
   ink, and does each weight really differ from regular? Answered by the
   font module, not by this text. */
static void cmd_font_check(const char *rest) {
    const char *p = rest;
    if (!p || !p[0]) {
        o_str("usage: font check <hex codepoint> ...   e.g. font check 00F1 2500 28FF\n");
        return;
    }
    while (*p) {
        uint32_t cp = 0;
        int digits = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ') {
            int v = hex_nibble(*p++);
            if (v < 0) { digits = 0; break; }
            cp = (cp << 4) | (uint32_t)v;
            digits++;
        }
        if (!digits) { o_str("font check: not hex\n"); return; }
        o_str("U+");
        /* Four digits is the usual shape, but print every one a codepoint
           actually has: echoing U+F600 back at someone who typed 1F600 is a
           small lie about what was tested. */
        { const char H[] = "0123456789ABCDEF"; int sh = 12;
          while (sh < 28 && (cp >> (sh + 4))) sh += 4;
          for (; sh >= 0; sh -= 4) { char b[2] = { H[(cp >> sh) & 15], 0 }; o_str(b); } }
        o_str(font_has(cp) ? "  drawable" : "  MISSING ");
        o_str("  ink "); o_dec(font_ink(cp, FONT_REGULAR));
        o_str("  bold "); o_str(font_style_differs(cp, FONT_BOLD) ? "own" : "shared");
        o_str("  italic "); o_str(font_style_differs(cp, FONT_ITALIC) ? "own" : "shared");
        o_nl();
    }
}

static void cmd_font(const char *arg) {
    o_str("\033[1m"); o_str(font_name()); o_str("\033[0m  ");
    o_dec(font_cell_w()); o_str("x"); o_dec(font_cell_h());
    o_str(" px cell, "); o_dec(font_glyph_count());
    o_str(" glyphs, 4-bit antialiased\n");
    o_str("console "); o_dec((uint64_t)console_cols()); o_str("x");
    o_dec((uint64_t)console_rows()); o_str(" cells, scrollback ");
    o_dec((uint64_t)console_scroll_max() + (uint64_t)console_rows());
    o_str(" rows\n \n");

    o_str("  regular   ABCDEFGHIJKLM abcdefghijklm 0123456789 {}[]()<>/\\|\n");
    o_str("\033[1m  bold      ABCDEFGHIJKLM abcdefghijklm 0123456789 {}[]()<>/\\|\033[0m\n");
    o_str("\033[3m  italic    ABCDEFGHIJKLM abcdefghijklm 0123456789 {}[]()<>/\\|\033[0m\n");
    o_str("\033[1;3m  bold+it   ABCDEFGHIJKLM abcdefghijklm 0123456789\033[0m\n \n");

    o_str("  acentos   áéíóú àèìòù âêîôû äëïöü ñÑ çÇ ¿? ¡! ºª ßØøÅå ÆæŒœ\n");
    o_str("  moneda    € $ ¢ £ ¥ ₽ ₹ ₿    grados  ° ± × ÷ ‰ µ Ω\n");
    o_str("  puntuac.  – — ‘’ “” … · • ‹› «» † ‡ § ¶ ™ © ®\n");
    o_str("  flechas   ← ↑ → ↓ ↔ ↕ ⇐ ⇒ ⇔ ↰ ↳ ⟵ ⟶\n");
    o_str("  griego    α β γ δ ε θ λ μ π ρ σ τ φ ψ ω Γ Δ Θ Λ Ξ Π Σ Φ Ψ Ω\n");
    o_str("  mates     ∀ ∂ ∃ ∅ ∇ ∈ ∉ ∏ ∑ − ∗ √ ∝ ∞ ∧ ∨ ∩ ∪ ∫ ≈ ≠ ≡ ≤ ≥ ⊂ ⊕\n");
    o_str("  bloques   ░ ▒ ▓ █ ▄ ▀ ▌ ▐ ■ □ ▪ ▫ ● ○ ◆ ◇ ▲ ▼ ◀ ▶\n");
    o_str("  simbolos  ✓ ✔ ✗ ✘ ★ ☆ ♠ ♥ ♦ ♣ ⚠ ☺ ☻ ⌘ ⏻ ⏱ ⌛\n");
    o_str("  braille   ⠁⠃⠇⠏⠟⠿⡿⣿ ⣤⣴⣶⣾⣿ ⠈⠘⠸⢸⣸\n \n");

    o_str("  ┌─────────────┬─────────────┐   ╔═════════════╗\n");
    o_str("  │ box drawing │ U+2500-257F │   ║ double rule ║\n");
    o_str("  ├─────────────┼─────────────┤   ╟─────────────╢\n");
    o_str("  │ ╭─ rounded  │ ╱ ╲ ╳ diag  │   ║ ▀▄ blocks   ║\n");
    o_str("  └─────────────┴─────────────┘   ╚═════════════╝\n");

    if (eqs(arg, "check")) return;      /* handled before we get here */
    if (eqs(arg, "ranges")) {
        o_str(" \nA codepoint outside the baked ranges is drawn as \033[1m\357\277\275\033[0m\n");
        o_str("(U+FFFD), never as a blank: a missing glyph is information.\n");
    } else {
        o_str(" \n`font ranges` explains what happens outside those blocks.\n");
    }
}

/* The ceilings this ELF was compiled with, read from the same constants the
   subsystems use — not a table typed by hand next to them. A number here that
   disagrees with docs2.0/limites.md means one of the two is lying, which is
   exactly what the CERT-21 smoke test is asked to catch. */
static void cmd_limits(void) {
    o_str(ECHOS_VERSION_NAME); o_str(" — declared limits (compiled in, not aspirations)\n \n");
    o_pad("limit", 18); o_pad("value", 12); o_str("where\n");

    o_pad("tasks", 18);           o_dec_pad(SCHED_MAX_TASKS, 12);
    o_str("kernel/task/scheduler.h\n");
    o_pad("task stack", 18);      o_dec_pad(SCHED_STACK_SIZE, 12);
    o_str("bytes per task; IRQs run on it too\n");
    o_pad("seb ring", 18);        o_dec_pad(SEB_RING_CAPACITY, 12);
    o_str("spikes, kernel/event/seb.h\n");
    o_pad("seb channels", 18);    o_dec_pad(SEB_CH_COUNT, 12);
    o_str("timer kbd mouse pci net wsp sys\n");
    o_pad("wsp packet", 18);      o_dec_pad(WSP_PACKET_BYTES, 12);
    o_str("bytes, kernel/include/wsp.h\n");
    o_pad("heap-0 wsp pool", 18); o_dec_pad(HEAP0_WSP_POOL_SIZE, 12);
    o_str("bytes of BSS, never grows\n");
    o_pad("devices", 18);         o_dec_pad(HEAP0_DEVMGR_MAX, 12);
    o_str("static probe slots\n");
    o_pad("kmalloc arena", 18);   o_dec_pad(heap_total(), 12);
    o_str("bytes; the only dynamic allocator\n");
    o_pad("rxfs files", 18);      o_dec_pad(RXFS_MAX_FILES, 12);
    o_str("slots, fs/rxfs/rxfs.h\n");
    o_pad("rxfs file size", 18);  o_dec_pad(RXFS_SIZE, 12);
    o_str("bytes per file, fixed\n");
    o_pad("rxfs dirs", 18);       o_dec_pad(RXFS_MAX_DIRS, 12);
    o_str("directory entries\n");
    o_pad("rxfs path depth", 18); o_dec_pad(RXFS_MAX_DEPTH, 12);
    o_str("components\n");
    o_pad("ramdisk", 18);         o_dec_pad(RAMDISK_BYTES, 12);
    o_str("bytes of block device ram0\n");

    o_str(" \nReached one of these and it refuses; it does not grow. Prose and the\n");
    o_str("absent-hardware list: docs2.0/limites.md.\n");
}

static void cmd_uptime(void) {
    uint64_t ms = arch_timer_uptime_ms();
    o_str("uptime: "); o_dec(ms / 1000); o_str("."); o_dec((ms % 1000) / 100);
    o_str(" s   ("); o_dec(arch_timer_ticks()); o_str(" ticks @ ");
    o_dec(arch_timer_hz()); o_str(" Hz)\n");
    char stamp[24];
    clock_fmt_stamp(stamp, sizeof(stamp));
    o_str("clock:  "); o_str(stamp);
    o_str(clock_rtc_ok() ? "  (CMOS RTC)\n" : "  (software)\n");
}

/* Parse "YYYY-MM-DD" and optional "HH:MM:SS". Returns false on bad input. */
static bool parse_date_parts(const char *date, const char *time, clock_tm *out) {
    if (!date || !date[0] || !out) return false;
    /* YYYY-MM-DD */
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    int n = 0;
    for (const char *p = date; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            if (n < 4) y = y * 10 + (*p - '0');
            else if (n < 6) mo = mo * 10 + (*p - '0');
            else if (n < 8) d = d * 10 + (*p - '0');
            else return false;
            n++;
        } else if (*p == '-' && (n == 4 || n == 6)) {
            continue;
        } else return false;
    }
    if (n != 8) return false;
    if (time && time[0]) {
        int tn = 0;
        for (const char *p = time; *p; p++) {
            if (*p >= '0' && *p <= '9') {
                if (tn < 2) h = h * 10 + (*p - '0');
                else if (tn < 4) mi = mi * 10 + (*p - '0');
                else if (tn < 6) s = s * 10 + (*p - '0');
                else return false;
                tn++;
            } else if (*p == ':' && (tn == 2 || tn == 4)) {
                continue;
            } else return false;
        }
        if (tn != 6 && tn != 4) return false; /* HH:MM or HH:MM:SS */
    }
    out->year = y; out->month = mo; out->day = d;
    out->hour = h; out->min = mi; out->sec = s;
    return true;
}

static void cmd_date(const char *a1, const char *rest) {
    if (a1 && a1[0] && eqs(a1, "set")) {
        /* date set YYYY-MM-DD [HH:MM:SS] */
        char date[16], time[16];
        date[0] = time[0] = '\0';
        if (rest && rest[0]) {
            /* first token = date, optional second = time */
            size_t i = 0;
            while (rest[i] && rest[i] != ' ' && i + 1 < sizeof(date)) {
                date[i] = rest[i]; i++;
            }
            date[i] = '\0';
            while (rest[i] == ' ') i++;
            size_t j = 0;
            while (rest[i] && rest[i] != ' ' && j + 1 < sizeof(time)) {
                time[j++] = rest[i++];
            }
            time[j] = '\0';
        }
        clock_tm t;
        if (!parse_date_parts(date, time[0] ? time : NULL, &t) || !clock_set(&t)) {
            o_str("usage: date set YYYY-MM-DD [HH:MM:SS]\n");
            o_str("example: date set 2026-08-12 15:30:00\n");
            return;
        }
        char stamp[24];
        clock_fmt_stamp(stamp, sizeof(stamp));
        o_str("+ local time set to "); o_str(stamp); o_nl();
        return;
    }
    if (a1 && a1[0] && eqs(a1, "-u")) {
        clock_tm u; char s[24];
        clock_now_utc(&u);
        s[0] = (char)('0' + (u.year / 1000) % 10);
        s[1] = (char)('0' + (u.year / 100) % 10);
        s[2] = (char)('0' + (u.year / 10) % 10);
        s[3] = (char)('0' + u.year % 10);
        s[4] = '-';
        s[5] = (char)('0' + u.month / 10); s[6] = (char)('0' + u.month % 10);
        s[7] = '-';
        s[8] = (char)('0' + u.day / 10); s[9] = (char)('0' + u.day % 10);
        s[10] = ' ';
        s[11] = (char)('0' + u.hour / 10); s[12] = (char)('0' + u.hour % 10);
        s[13] = ':';
        s[14] = (char)('0' + u.min / 10); s[15] = (char)('0' + u.min % 10);
        s[16] = ':';
        s[17] = (char)('0' + u.sec / 10); s[18] = (char)('0' + u.sec % 10);
        s[19] = '\0';
        o_str(s); o_str(" UTC\n");
        return;
    }
    {
        char stamp[24], off[8];
        clock_fmt_stamp(stamp, sizeof(stamp));
        clock_tz_fmt_offset(off, sizeof(off));
        o_str(stamp); o_str("  "); o_str(clock_tz_name());
        o_str("  "); o_str(off); o_nl();
        o_str("source: "); o_str(clock_rtc_ok() ? "CMOS RTC (UTC) + PIT\n" : "software + PIT\n");
        o_str("set:  date set YYYY-MM-DD HH:MM:SS\n");
        o_str("zone: tz Europe/Madrid | tz UTC | tz +2 | tz list\n");
    }
}

static void cmd_tz(const char *a1, const char *rest) {
    const char *arg = (a1 && a1[0]) ? a1 : "";
    char off[8];
    int i;
    if (eqs(arg, "list") || eqs(arg, "ls")) {
        o_str("zones:\n");
        for (i = 0; clock_tz_list(i); i++) {
            o_str("  "); o_str(clock_tz_list(i)); o_nl();
        }
        o_str("or an offset:  tz +2   tz +01:00   tz -5\n");
        return;
    }
    if (arg[0] && !eqs(arg, "show") && !eqs(arg, "get")) {
        const char *id = arg;
        if (eqs(arg, "set") && rest && rest[0]) id = rest;
        if (clock_tz_apply(id) != 0) {
            o_str("unknown zone.  tz list\n");
            return;
        }
        vfs_write("/etc/timezone", clock_tz_name(), (uint32_t)k_strlen(clock_tz_name()));
    }
    clock_tz_fmt_offset(off, sizeof(off));
    o_str("timezone: "); o_str(clock_tz_name());
    o_str("  "); o_str(off); o_nl();
}

static void cmd_devices(void) {
    o_str("device registry\n \n");
    o_pad("device", 14); o_pad("class", 10); o_str("status\n");
    for (int i = 0; i < devmgr_count(); i++) {
        const device_info_t *d = devmgr_at(i);
        o_pad(d->name, 14); o_pad(d->class_, 10); o_str(d->status); o_nl();
    }
    if (devmgr_count() == 0) o_str("(none registered)\n");
}

/* ---- vault commands (creative BSD-style names) -------------------------- */
static void cmd_catalog(const char *path) {
    const char *dir = (path && path[0]) ? path : vfs_getcwd();
    char names[RXFS_MAX_FILES][RXFS_NAME];
    int n = vfs_list_dir(dir, names, RXFS_MAX_FILES);
    o_str("* catalog of "); o_str(dir ? dir : "/"); o_str("\n");
    o_str("  ("); o_dec(n < 0 ? 0 : (uint64_t)n); o_str(" entries, vault ");
    o_dec(vfs_used_bytes()); o_str(" B used)\n \n");
    if (n < 0) { o_str("! cannot open that path\n"); return; }
    for (int i = 0; i < n; i++) {
        size_t L = 0; while (names[i][L]) L++;
        bool isdir = L > 0 && names[i][L - 1] == '/';
        o_str(isdir ? "  ~ " : "    ");
        o_pad(names[i], 28);
        if (!isdir) {
            char full[RXFS_NAME];
            /* join cwd/path + name without trailing slash */
            char base[RXFS_NAME];
            size_t b = 0; for (; names[i][b] && names[i][b] != '/' && b + 1 < sizeof(base); b++)
                base[b] = names[i][b];
            base[b] = '\0';
            { char *d = full, *e = full + sizeof(full);
              const char *p = dir;
              if (p && p[0] == '/' && !p[1]) { *d++ = '/'; }
              else {
                  while (p && *p && d < e - 1) *d++ = *p++;
                  if (d > full && d[-1] != '/' && d < e - 1) *d++ = '/';
              }
              for (const char *q = base; *q && d < e - 1; q++) *d++ = *q;
              *d = '\0'; }
            int sz = vfs_size(full);
            o_dec(sz < 0 ? 0 : (uint64_t)sz); o_str(" B");
        } else o_str("dir");
        o_nl();
    }
    if (n == 0) o_str("  (empty)\n");
}

static void cmd_wander(const char *path) {
    if (!path || !path[0]) { o_str("usage: wander <path>\n"); return; }
    if (vfs_chdir(path)) {
        o_str("+ now at "); o_str(vfs_getcwd()); o_nl();
    } else {
        o_str("! cannot wander there: "); o_str(path); o_nl();
    }
}

static void cmd_whereami(void) {
    o_str("~ "); o_str(vfs_getcwd()); o_nl();
}

static void cmd_homestead(const char *path) {
    if (!path || !path[0]) { o_str("usage: homestead <dir>\n"); return; }
    o_str(vfs_mkdir(path) ? "+ homesteaded " : "! failed "); o_str(path); o_nl();
}

static void cmd_raze(const char *path) {
    if (!path || !path[0]) { o_str("usage: raze <empty-dir>\n"); return; }
    o_str(vfs_rmdir(path) ? "+ razed " : "! raze failed (not empty?) ");
    o_str(path); o_nl();
}

static void cmd_vault(void) {
    o_str("* vault (RXFS)\n");
    o_str("  cwd:   "); o_str(vfs_getcwd()); o_nl();
    o_str("  files: "); o_dec((uint64_t)vfs_count()); o_nl();
    o_str("  used:  "); o_dec(vfs_used_bytes()); o_str(" bytes\n");
    o_str("  fs:    "); o_str(vfs_fsname()); o_nl();
}

static void cmd_peep(const char *name) {
    if (!name || !name[0]) { o_str("usage: peep <file>\n"); return; }
    uint32_t len = 0;
    const char *d = vfs_read(name, &len);
    if (!d) { o_str("! peep: not found: "); o_str(name); o_nl(); return; }
    o_str("* peep "); o_str(name); o_str(" (first 400 B)\n");
    uint32_t n = len > 400 ? 400 : len;
    for (uint32_t i = 0; i < n; i++) {
        char c = d[i];
        if (c == '\n') o_nl();
        else if ((unsigned char)c >= 0x20 && (unsigned char)c != 0x7f) {
            char s[2] = { c, 0 }; o_str(s);
        } else o_str(".");
    }
    if (len > n) o_str("\n…\n");
    else o_nl();
}

static void tree_join(char *dst, size_t max, const char *dir, const char *name) {
    char *d = dst, *e = dst + max;
    const char *p = dir;
    size_t L = 0;
    while (name[L] && name[L] != '/') L++;
    if (p && p[0] == '/' && !p[1]) { *d++ = '/'; }
    else {
        while (p && *p && d < e - 1) *d++ = *p++;
        if (d > dst && d[-1] != '/' && d < e - 1) *d++ = '/';
    }
    { size_t i; for (i = 0; i < L && d < e - 1; i++) *d++ = name[i]; }
    *d = '\0';
}

static void tree_walk(const char *dir, const char *pref, int last, int depth) {
    char names[RXFS_MAX_FILES][RXFS_NAME];
    int n, i;
    if (depth > 12) return;
    n = vfs_list_dir(dir, names, RXFS_MAX_FILES);
    if (n < 0) return;
    (void)last;
    for (i = 0; i < n; i++) {
        int isdir, lastc = (i + 1 == n);
        size_t L = 0;
        char full[RXFS_NAME];
        while (names[i][L]) L++;
        isdir = (L && names[i][L - 1] == '/');
        o_str(pref);
        o_str(lastc ? "`-- " : "|-- ");
        if (isdir) { o_str("\033[1;34m"); o_str(names[i]); o_str("\033[0m"); }
        else {
            /* colour by extension */
            const char *nm = names[i];
            int e = (int)L, py = 0;
            if (e >= 2 && nm[e-2]=='.' && nm[e-1]=='c') o_str("\033[1;33m");
            else if (e >= 3 && nm[e-3]=='.' && nm[e-2]=='p' && nm[e-1]=='y') o_str("\033[1;32m");
            else if (e >= 3 && nm[e-3]=='.' && nm[e-2]=='j' && nm[e-1]=='s') o_str("\033[1;33m");
            else if (e >= 5 && nm[e-5]=='.' && nm[e-4]=='h' && nm[e-3]=='t') o_str("\033[1;35m");
            else if (e >= 3 && nm[e-3]=='.' && nm[e-2]=='r' && nm[e-1]=='s') o_str("\033[1;31m");
            else o_str("\033[0;37m");
            o_str(names[i]); o_str("\033[0m");
            (void)py;
        }
        o_nl();
        if (isdir) {
            char np[48];
            int k = 0;
            tree_join(full, sizeof(full), dir, names[i]);
            while (pref[k] && k < 40) { np[k] = pref[k]; k++; }
            np[k++] = lastc ? ' ' : '|';
            np[k++] = ' '; np[k++] = ' '; np[k++] = ' ';
            np[k] = '\0';
            tree_walk(full, np, lastc, depth + 1);
        }
    }
}

static void cmd_tree(const char *path) {
    const char *root = (path && path[0]) ? path : vfs_getcwd();
    if (!root || !root[0]) root = "/";
    o_str("\033[1;34m"); o_str(root); o_str("\033[0m\n");
    tree_walk(root, "", 1, 0);
}

static void cmd_history(void) {
    int i, n = commands_hist_count();
    if (!n) { o_str("(empty — type some commands first)\n"); return; }
    for (i = 0; i < n; i++) {
        o_str("  "); o_dec((uint64_t)(i + 1)); o_str("  ");
        o_str(commands_hist_at(i)); o_nl();
    }
}

/* These used to live only in the browser shell's own dispatcher, which meant
   the window manager's terminal could not run them — `theme`, `kbd`, `ping`
   and `wired` all answered "not a system command" there. They belong here:
   one dispatcher, both shells. */
static void cmd_theme(void) {
#if ECHOS_CLI_ONLY
    o_str("theme: white-on-black\n");
    o_str("CLI is JetBrains Mono (regular/bold/italic), white on black.\n");
    o_str("`termtheme` sets ROSH palettes, `font` prints the specimen,\n");
    o_str("`pane` splits the console.\n");
#else
    fb_set_theme((fb_theme() + 1) % RGX_THEME_COUNT);
    o_str("theme: "); o_str(fb_theme_name()); o_nl();
    o_str("\nNote: changing it from here does not touch your saved profile.\n");
    o_str("Use Settings in the browser shell to make it stick.\n");
#endif
}

/* Neuromorphic backend switch: software LIF today; Akida PCI probe for later. */
static void cmd_neurocpu(const char *arg) {
    o_str("Neuromorphic compute backend\n \n");
    o_str("active:  "); o_str(rx_backend_label()); o_nl();
    o_str("status:  "); o_str(rx_backend_status()); o_nl();
    o_str("spikes:  "); o_dec(rx_backend_spikes());
    o_str("   idle entries: "); o_dec(power_idle_entries()); o_nl();
    o_nl();
    o_str("available backends (2.1):\n");
    o_str("  software");
    o_str(rx_backend_available(RX_BACKEND_SOFTWARE) ? "  [ready]  " : "  [no]     ");
    o_str(rx_backend_avail_why(RX_BACKEND_SOFTWARE)); o_nl();
    o_str("  akida   ");
    o_str(rx_backend_available(RX_BACKEND_AKIDA) ? "  [ready]  " : "  [absent] ");
    o_str(rx_backend_avail_why(RX_BACKEND_AKIDA)); o_nl();
    o_nl();
    if (!arg || !arg[0]) {
        o_str("usage: neurocpu software | akida | cycle\n");
        o_str("akida is a PCI probe. no offload without a card. no Loihi in 2.1.\n");
#if !ECHOS_CLI_ONLY
        o_str("Desktop: press  b  to cycle backends.\n");
#endif
        return;
    }
    if (eqs(arg, "cycle")) {
        rx_backend_cycle(true);
        o_str("now: "); o_str(rx_backend_label()); o_str(" — ");
        o_str(rx_backend_status()); o_nl();
        return;
    }
    int id = -1;
    if (eqs(arg, "software") || eqs(arg, "cpu") || eqs(arg, "soft"))
        id = RX_BACKEND_SOFTWARE;
    else if (eqs(arg, "akida")) id = RX_BACKEND_AKIDA;
    else if (eqs(arg, "loihi")) {
        o_str("loihi: not a 2.1 backend (no interface in this tree)\n");
        return;
    } else {
        o_str("unknown backend. try: software | akida\n");
        return;
    }
    if (rx_backend_set(id)) {
        o_str("switched to "); o_str(rx_backend_label()); o_nl();
    } else {
        o_str("could not switch: "); o_str(rx_backend_status()); o_nl();
        o_str("("); o_str(rx_backend_avail_why(id)); o_str(")\n");
    }
}

static void cmd_kbd(const char *arg) {
    if (eqs(arg, "es") || eqs(arg, "es-es") || eqs(arg, "spanish"))
        kbd_set_layout(1);
    else if (eqs(arg, "us") || eqs(arg, "en") || eqs(arg, "windows"))
        kbd_set_layout(0);
    else if (eqs(arg, "mac") || eqs(arg, "us-mac") || eqs(arg, "osx"))
        kbd_set_layout(2);
    else if (arg && arg[0]) {
        o_str("usage: kbd es | us | mac\n");
        o_str("  es  — Spanish Windows QWERTY (AltGr+2 = @, ñ, dead keys)\n");
        o_str("  us  — English Windows QWERTY\n");
        o_str("  mac — English Mac QWERTY (Option = AltGr specials)\n");
        return;
    }
    o_str("keyboard layout: "); o_str(kbd_layout_name()); o_nl();
    if (kbd_layout() == 1)
        o_str("  tip: AltGr+2 = @   AltGr+3 = #   ñ on the ; key\n");
    else if (kbd_layout() == 2)
        o_str("  tip: Shift+2 = @   Option (AltGr) for Mac specials\n");
}

static void cmd_wired(void) {
    o_str("Rogex Wired - raw Ethernet, EtherType 0x88B5, no IP and no DNS\n \n");
    if (!wired_up()) {
        o_str("link: down (no NIC bound)\n \n");
        o_str("Drivers probed: virtio-net, Intel e1000, Realtek r8169\n");
        o_str("(RTL8106E — HP 15-ac195nl Ethernet), RTL8139.  `nics` lists PCI.\n");
        o_str("WiFi (RTL8188EE on the HP) has no driver — plug the cable.\n");
        return;
    }
    o_str("link:  up   driver "); o_str(net_driver_name()); o_nl();
    { char mac[24]; wired_mac_str(mac, sizeof(mac));
      o_str("mac:   "); o_str(mac); o_nl(); }
    o_str("alias: "); o_str(wired_my_alias()); o_nl();
    o_str("www:   "); o_str(www_enabled() ? "ON (opt-in)" : "OFF (default)"); o_nl();
    o_str(" \n[Mesh] ping/chat   [Browse] browse <url>   [Trace] trace <ip|alias>\n");
    int n = wired_reply_count();
    o_str(" \npeers who answered ("); o_dec((uint64_t)n); o_str("):\n");
    for (int i = 0; i < n; i++) {
        o_str("  * "); o_str(wired_reply_alias(i));
        o_str("  ACK  "); o_dec((uint64_t)wired_reply_rtt_ms(i)); o_str(" ms\n");
    }
    if (n == 0) o_str("  (none yet - try `ping <alias>`)\n");
    int c = wired_chat_count();
    o_str(" \nchat inbox ("); o_dec((uint64_t)c); o_str("):\n");
    for (int i = 0; i < c; i++) {
        o_str("  <"); o_str(wired_chat_from(i)); o_str("> ");
        o_str(wired_chat_text(i)); o_nl();
    }
    if (c == 0) o_str("  (empty — `chat <alias> hi` or `say hi`)\n");
    if (wired_chat_acked()) {
        o_str("  last say/chat: ACK from "); o_str(wired_chat_ack_from()); o_nl();
    }
    if (browse_last_url()[0] && browse_last_url()[0] != '(') {
        o_str(" \nlast page: "); o_str(browse_last_url()); o_nl();
    }
}

static bool looks_inet(const char *s) {
    int dots = 0, i;
    uint8_t ip[4];
    if (!s || !s[0]) return false;
    if (ip_parse(s, ip)) return true;
    for (i = 0; s[i]; i++) if (s[i] == '.') dots++;
    return dots >= 1;                 /* google.com, www.x.com, 1.2.3.4 */
}

static void cmd_iping(const char *target) {
    uint8_t ip[4];
    char ipb[20];
    uint32_t rtt = 0;
    int i, ok = 0;
    if (!target || !target[0]) {
        o_str("usage: ping <host|ip>     ICMP (www on first)\n");
        o_str("       ping <alias>       Rogex Wired L2\n");
        return;
    }
    if (!www_enable()) {
        o_str("! no IPv4 path: "); o_str(www_last_err()); o_nl();
        return;
    }
    if (!www_dns(target, ip, 3000)) {
        o_str("! dns failed: "); o_str(target); o_str(" — ");
        o_str(www_last_err()); o_nl();
        return;
    }
    ip_fmt(ip, ipb, sizeof(ipb));
    o_str("PING "); o_str(target); o_str(" ("); o_str(ipb); o_str(")\n");
    for (i = 0; i < 3; i++) {
        if (ip_icmp_ping_rtt(ip, 1500, &rtt)) {
            o_str("  pong  seq="); o_dec((uint64_t)(i + 1));
            o_str("  time="); o_dec((uint64_t)rtt); o_str(" ms\n");
            ok++;
        } else {
            o_str("  timeout seq="); o_dec((uint64_t)(i + 1)); o_nl();
        }
    }
    if (ok) {
        o_str("+ "); o_dec((uint64_t)ok); o_str("/3 ICMP echo replies\n");
        return;
    }
    /* QEMU user-net (slirp) often swallows ICMP to the internet.
       UDP/53 answering is still a real pong from that host's resolver path. */
    {
        uint8_t sip[4];
        uint16_t sp;
        uint8_t q[32], r[64];
        q[0]=0; q[1]=1; q[2]=1; q[3]=0; q[4]=0; q[5]=1;
        { int k; for (k = 6; k < 32; k++) q[k] = 0; }
        o_str("  (no ICMP — trying DNS pong to the resolver)\n");
        if (ip_udp_send(ipstack_cfg()->dns, 53, 53001, q, 12) &&
            ip_udp_recv(53001, sip, &sp, r, sizeof(r), 1500)) {
            o_str("+ pong  UDP/53 via ");
            ip_fmt(ipstack_cfg()->dns, ipb, sizeof(ipb));
            o_str(ipb); o_str("  (ICMP filtered, UDP works)\n");
        } else {
            o_str("! no pong. cable? QEMU: slirp filters ICMP; metal: DHCP?\n");
        }
    }
}

/* dns <host> — resolve A record through the www DNS client. */
static void cmd_dns(const char *host) {
    uint8_t ip[4];
    if (!host || !host[0]) { o_str("usage: dns <host>\n"); return; }
    if (!www_ready()) { o_str("www off — 'www on' first.\n"); return; }
    o_str("DNS "); o_str(host); o_str(" ...\n");
    if (!www_dns(host, ip, 3000)) { o_str("resolve failed\n"); return; }
    char ipb[16];
    ip_fmt(ip, ipb, sizeof(ipb));
    o_str(host); o_str(" -> "); o_str(ipb); o_nl();
}

/* tls <host> — handshake probe against :443 (offers 1.3 then 1.2). */
static void cmd_tls(const char *host) {
    char why[80];
    uint8_t ip[4];
    if (!host || !host[0]) { o_str("usage: tls <host>\n"); return; }
    if (!www_ready()) { o_str("www off — 'www on' first.\n"); return; }
    if (!www_dns(host, ip, 3000)) { o_str("dns failed\n"); return; }
    o_str("TLS "); o_str(host); o_str(":443 (offer 1.3 + 1.2) ...\n");
    if (!tcp_connect(ip, 443, 5000)) { o_str("tcp connect fail\n"); return; }
    if (!tls_handshake(host, why, (int)sizeof(why))) {
        o_str("handshake FAIL: "); o_str(why[0] ? why : "?"); o_nl();
        tcp_close();
        return;
    }
    o_str("handshake OK  "); o_str(tls_version_name());
    o_str("  AES-128-GCM P-256 SHA256, SNI (no CA pin)\n");
    tls_close();
}

static void cmd_ping(const char *target) {
    if (looks_inet(target)) { cmd_iping(target); return; }
    if (!wired_up()) {
        if (target && target[0]) { cmd_iping(target); return; }
        o_str("network down: no NIC. `nics` and `www on`.\n");
        return;
    }
    if (!target || !target[0]) { o_str("usage: ping <host|ip|alias>\n"); return; }

    wired_clear_replies();
    if (!wired_send_ping(target)) { o_str("ping: send failed\n"); return; }
    o_str("PING "); o_str(target); o_str(" by pseudonym (L2 Wired)...\n");

    uint64_t start = arch_timer_ticks();
    while (arch_timer_ticks() - start < 150) { wired_service(); sched_poll_relax(1); }

    int n = wired_reply_count();
    o_str(" \npeers who answered ("); o_dec((uint64_t)n); o_str("):\n");
    for (int i = 0; i < n; i++) {
        o_str("  * "); o_str(wired_reply_alias(i));
        o_str("  ACK confirmed  "); o_dec((uint64_t)wired_reply_rtt_ms(i));
        o_str(" ms\n");
    }
    if (n == 0) o_str("  (none on this wire — try ping google.com after www on)\n");
}

static void cmd_chat(const char *alias, const char *msg) {
    if (!wired_up()) { o_str("network down: no NIC. `wired` explains.\n"); return; }
    if (!msg || !msg[0]) {
        o_str("usage: chat <alias> <message>\n");
        o_str("       say <message>          (L2 broadcast)\n");
        return;
    }
    if (!wired_send_chat(alias && alias[0] ? alias : "", msg)) {
        o_str("chat: send failed\n"); return;
    }
    o_str("chat> ");
    o_str(alias && alias[0] ? alias : "*");
    o_str(": "); o_str(msg); o_nl();
    uint64_t start = arch_timer_ticks();
    while (arch_timer_ticks() - start < 80) { wired_service(); sched_poll_relax(1); }
    if (wired_chat_acked()) {
        o_str("  ACK from "); o_str(wired_chat_ack_from());
        o_str(" — delivered\n");
    } else {
        o_str("  (no ACK yet — peer offline or broadcast)\n");
    }
}

static void cmd_www(const char *a1, const char *rest) {
    if (!a1 || !a1[0] || eqs(a1, "status") || eqs(a1, "help")) {
        char buf[900];
        www_status_text(buf, sizeof(buf));
        o_str(buf);
        return;
    }
    if (eqs(a1, "on") || eqs(a1, "enable")) {
        commands_v("bringing up IPv4 (DHCP, then QEMU 10.0.2.15/24 if needed)");
        if (www_enable()) {
            char b[20];
            const ip_cfg_t *c = ipstack_cfg();
            o_str("+ WWW mode ON\n");
            o_str("  nic     "); o_str(net_driver_name()); o_nl();
            o_str("  ip      "); ip_fmt(c->ip, b, sizeof(b)); o_str(b); o_nl();
            o_str("  gateway "); ip_fmt(c->gw, b, sizeof(b)); o_str(b); o_nl();
            o_str("  dns     "); ip_fmt(c->dns, b, sizeof(b)); o_str(b); o_nl();
            o_str("  try: ping google.com   ping www.rogexlaboratories.com\n");
        } else {
            o_str("! www on failed: ");
            o_str(www_last_err()[0] ? www_last_err() : "need a NIC");
            o_nl();
            o_str("  `nics` lists PCI. HP 15-ac195nl: Ethernet cable, not WiFi.\n");
        }
        return;
    }
    if (eqs(a1, "off") || eqs(a1, "disable")) {
        commands_v("dropping IPv4 / DNS / TCP client");
        www_disable();
        o_str("+ WWW mode OFF\n");
        return;
    }
    if (eqs(a1, "config")) {
        char ip[16]={0}, mask[16]={0}, gw[16]={0}, dns[16]={0};
        const char *p = rest;
        int f = 0; char *outs[4] = {ip, mask, gw, dns};
        while (p && *p && f < 4) {
            while (*p == ' ') p++;
            if (!*p) break;
            int i = 0;
            while (*p && *p != ' ' && i < 15) outs[f][i++] = *p++;
            outs[f][i] = '\0'; f++;
        }
        if (f < 3) { o_str("usage: www config <ip> <mask> <gw> [dns]\n"); return; }
        if (www_config(ip, mask, gw, dns[0] ? dns : NULL))
            o_str("+ www address configured\n");
        else o_str("! bad address\n");
        return;
    }
    if (eqs(a1, "dns")) {
        if (!www_ready()) { o_str("www off — run `www on` first\n"); return; }
        if (!rest || !rest[0]) { o_str("usage: www dns <host>\n"); return; }
        uint8_t ip[4];
        if (!www_dns(rest, ip, 3000)) { o_str("! dns failed\n"); return; }
        char b[20]; ip_fmt(ip, b, sizeof(b));
        o_str(rest); o_str(" -> "); o_str(b); o_nl();
        return;
    }
    if (eqs(a1, "source") || eqs(a1, "src")) {
        if (!www_ready()) { o_str("www off — run `www on` first\n"); return; }
        if (!rest || !rest[0]) { o_str("usage: www source <http-url>\n"); return; }
        static char body[WWW_BODY_MAX];
        int st = 0;
        uint32_t n = www_http_get_follow(rest, body, sizeof(body), &st);
        o_str("HTTP "); o_dec((uint64_t)st); o_str("  "); o_dec(n);
        o_str(" bytes  source of "); o_str(rest); o_nl(); o_nl();
        if (n) {
            uint32_t i, show = n;
            if (show > o_max - o_len - 8) show = (uint32_t)(o_max - o_len - 8);
            for (i = 0; i < show; i++) {
                char c = body[i];
                if (c == '\n' || ((unsigned char)c >= 0x20 && (unsigned char)c != 0x7f)) {
                    char s[2] = { c, 0 }; o_str(s);
                } else if (c == '\t') o_str("    ");
            }
            if (n > show) o_str("\n... (truncated)\n");
            else o_nl();
        } else if (www_last_err()[0]) {
            o_str("! "); o_str(www_last_err()); o_nl();
        }
        return;
    }
    if (eqs(a1, "get")) {
        if (!www_ready()) { o_str("www off — run `www on` first\n"); return; }
        if (!rest || !rest[0]) { o_str("usage: www get <http-url>\n"); return; }
        static char body[WWW_BODY_MAX];
        int st = 0;
        uint32_t n = www_http_get_follow(rest, body, sizeof(body), &st);
        o_str("HTTP "); o_dec((uint64_t)st); o_str("  "); o_dec(n); o_str(" bytes\n");
        if (n) {
            uint32_t show = n < 400 ? n : 400;
            for (uint32_t i = 0; i < show; i++) {
                char s[2] = { body[i], 0 }; o_str(s);
            }
            if (n > show) o_str("\n... (truncated)\n");
            else o_nl();
        } else if (www_last_err()[0]) {
            o_str("! "); o_str(www_last_err()); o_nl();
        }
        return;
    }
    if (eqs(a1, "post")) {
        char url[196] = {0};
        const char *body = "";
        const char *p = rest;
        int i = 0;
        if (!www_ready()) { o_str("www off — run `www on` first\n"); return; }
        if (!rest || !rest[0]) { o_str("usage: www post <http-url> [body]\n"); return; }
        while (*p && *p != ' ' && i < 195) url[i++] = *p++;
        url[i] = 0;
        while (*p == ' ') p++;
        body = p;
        {
            static char resp[WWW_BODY_MAX];
            int st = 0;
            uint32_t n = www_http_post(url, body, NULL, resp, sizeof(resp), &st);
            o_str("HTTP "); o_dec((uint64_t)st); o_str("  "); o_dec(n); o_str(" bytes\n");
            if (n) {
                uint32_t show = n < 400 ? n : 400;
                for (uint32_t i2 = 0; i2 < show; i2++) {
                    char s[2] = { resp[i2], 0 }; o_str(s);
                }
                if (n > show) o_str("\n... (truncated)\n");
                else o_nl();
            } else if (www_last_err()[0]) {
                o_str("! "); o_str(www_last_err()); o_nl();
            }
        }
        return;
    }
    if (eqs(a1, "channel")) {
        if (rest && rest[0]) {
            www_set_channel_base(rest);
            o_str("+ channel base = "); o_str(www_channel_base()); o_nl();
        } else {
            o_str("channel: "); o_str(www_channel_base()); o_nl();
        }
        return;
    }
    o_str("usage: www on|off|status|config|dns|get|post|channel\n");
}

static void cmd_curl(const char *a1, const char *rest) { netcli_curl(a1, rest); }
static void cmd_wget(const char *a1, const char *rest) { netcli_wget(a1, rest); }

static void cmd_env(void) {
    o_str("environment (volatile - BSS only, gone at power-off)\n \n");
    for (int i = 0; i < env_count(); i++) {
        const char *k = NULL, *v = NULL;
        if (!env_at(i, &k, &v)) continue;
        o_pad(k, 10); o_str("= "); o_str(v); o_nl();
    }
    o_str(" \nSet one with:  env <KEY> <value>\n");
    o_str("Nothing is written to disk. EchOS is amnesic by design, and a\n");
    o_str("$PATH that quietly persisted would contradict that.\n");
}

static void cmd_env_set(const char *k, const char *v) {
    if (!k || !k[0]) { cmd_env(); return; }
    if (!v || !v[0]) {
        const char *cur = env_get(k);
        o_str(k); o_str(" = "); o_str(cur ? cur : "(unset)"); o_nl();
        return;
    }
    if (env_set(k, v)) { o_str(k); o_str(" = "); o_str(v); o_nl(); }
    else                 o_str("environment is full\n");
}

static void cmd_dev(void) {
    o_str("/dev - kernel state, generated the moment you read it\n \n");
    for (int i = 0; i < devfs_count(); i++) {
        o_pad(devfs_name(i), 16); o_str(devfs_describe(i)); o_nl();
    }
    o_str(" \nRead one with `cat`, e.g.  cat /dev/thermal\n \n");
    o_str("These are not files. RXFS stores bytes; these produce their\n");
    o_str("contents from the hardware at the instant you ask, so reading\n");
    o_str("/dev/rapl twice gives two different answers - which is the point.\n");
    o_str("No writes, no seeks, no permissions: EchOS has one address space\n");
    o_str("and one user, and inventing those layers would be ceremony.\n");
}

/* /dev nodes are generated on read, so they are consulted before RXFS. A file
   named /dev/rapl could not exist in RXFS anyway — the name would be stored
   bytes, and the point of these is that they are not. */
static bool cat_devfs(const char *name) {
    static char buf[1024];
    size_t n = 0;
    if (!devfs_read(name, buf, sizeof(buf), &n)) return false;
    o_str("-- "); o_str(name); o_str(" (generated on read) --\n");
    o_str(buf);
    return true;
}

static void cmd_cat(const char *name) {
    if (cat_devfs(name)) return;
    if (!name[0]) { o_str("usage: cat <file>\n"); return; }
    uint32_t len = 0;
    const char *d = vfs_read(name, &len);
    if (!d) { o_str("cat: not found: "); o_str(name); o_nl(); return; }
    o_str("-- "); o_str(name); o_str(" ("); o_dec(len); o_str(" B) --\n");
    for (uint32_t i = 0; i < len && o_len + 1 < o_max; i++) {
        char c = d[i];
        o_buf[o_len++] = (c == '\n' || ((unsigned char)c >= 0x20 && (unsigned char)c != 0x7f)) ? c : '.';
    }
    o_buf[o_len] = '\0';
    if (o_len && o_buf[o_len-1] != '\n') o_nl();
}

static void cmd_write(const char *name, const char *text) {
    if (!name[0]) { o_str("usage: write <file> <text>\n"); return; }
    uint32_t len = (uint32_t)k_strlen(text);
    if (vfs_write(name, text, len)) {
        o_str("wrote "); o_dec(len); o_str(" B to "); o_str(name); o_nl();
    } else o_str("write failed (filesystem full? bad name?)\n");
}

static void cmd_rm(const char *name) {
    if (!name[0]) { o_str("usage: rm <file>\n"); return; }
    o_str(vfs_delete(name) ? "deleted " : "rm: not found: "); o_str(name); o_nl();
}

static void cmd_rename(const char *a, const char *b) {
    if (!a[0] || !b[0]) { o_str("usage: rename <old> <new>\n"); return; }
    if (vfs_rename(a, b)) { o_str("renamed "); o_str(a); o_str(" -> "); o_str(b); o_nl(); }
    else o_str("rename failed (source missing or destination exists)\n");
}

static void cmd_copy(const char *a, const char *b) {
    if (!a[0] || !b[0]) { o_str("usage: copy <src> <dst>\n"); return; }
    if (vfs_copy(a, b)) { o_str("copied "); o_str(a); o_str(" -> "); o_str(b); o_nl(); }
    else o_str("copy failed (source missing or filesystem full)\n");
}

static void cmd_apps(void) {
#if ECHOS_CLI_ONLY
    o_str(ECHOS_VERSION_NAME " CLI — no Roxenite GUI apps.\n");
    o_str("Commands live in the shell. Try:  help  echofetch  epk list  hwprobe\n");
    return;
#else
    o_str("built-in apps (seeded into rxfs at boot):\n");
    for (int i = 0; i < init_app_count(); i++) {
        o_str("  "); o_str(init_app_name(i)); o_nl();
    }
    o_str(" \nrxfs .rxc files (run <file> or go rgx://<name>):\n");
    char names[RXFS_MAX_FILES][RXFS_NAME];
    int n = vfs_list(names, RXFS_MAX_FILES), shown = 0;
    for (int i = 0; i < n; i++)
        if (ends_rxc(names[i])) { o_str("  "); o_str(names[i]); o_nl(); shown++; }
    if (!shown) o_str("  (none)\n");
#endif
}

/* --- persistence: save/load/format against the attached disk (vda/hda) --- */
static struct blockdev *disk_or_msg(void) {
    if (!disk_present()) {
        o_str("no disk attached. Boot with one:  make disk && make run-disk\n");
        o_str("(EchOS stays amnesic without a disk - that is the default.)\n");
        return NULL;
    }
    return (struct blockdev *)disk_dev();
}

static void cmd_save(void) {
    commands_v("serializing the in-RAM RXFS image onto the bound disk");
    struct blockdev *d = disk_or_msg();
    if (!d) return;
    if (!rxfs_disk_has_image(d)) {
        o_str(disk_name()); o_str(" is not EchOS-formatted. If it holds nothing you care about:\n");
        o_str("    format "); o_str(disk_name()); o_str(" yes\n");
        o_str("(save refuses to write over an unformatted disk on purpose -\n");
        o_str(" it could be a real MBR/partition table.)\n");
        return;
    }
    if (rxfs_save(d)) {
        o_str("saved "); o_dec((uint64_t)vfs_count());
        o_str(" file(s) to "); o_str(disk_name()); o_str(". They will survive a reboot.\n");
    } else o_str("save FAILED (disk too small or I/O error).\n");
}

static void cmd_load(void) {
    commands_v("replacing RAM RXFS with the image on disk");
    struct blockdev *d = disk_or_msg();
    if (!d) return;
    if (!rxfs_disk_has_image(d)) { o_str(disk_name()); o_str(" has no RXFS image to load.\n"); return; }
    if (rxfs_load(d)) {
        o_str("loaded "); o_dec((uint64_t)vfs_count());
        o_str(" file(s) from "); o_str(disk_name()); o_str(" (replaced the in-RAM filesystem).\n");
    } else o_str("load FAILED (corrupt image or I/O error).\n");
}

static void cmd_format(const char *dev, const char *confirm) {
    blockdev_t *d = NULL;
    if (dev && dev[0] && !eqs(dev, "disk")) {
        d = blockdev_get(dev);
        if (!d) { o_str("no such disk: "); o_str(dev); o_nl(); return; }
    } else {
        d = (blockdev_t *)disk_or_msg();
        if (!d) return;
    }
    if (!eqs(confirm, "yes")) {
        o_str("format writes an empty RXFS image - DESTRUCTIVE.\n");
        o_str("Confirm with:  format "); o_str(d->name); o_str(" yes\n");
        return;
    }
    o_str(rxfs_disk_format(d) ? "disk formatted (empty RXFS). Now: save\n"
                              : "format FAILED (I/O error).\n");
}

/* Partition a disk for EchOS. Usage:
     partition              list whole disks
     partition <dev>        MBR + one bootable EchOS partition (type 0x7F)
     partition <dev> gpt    GPT + one Linux partition (advanced)
   After partitioning, use `install` or `format` + `save`. */
static void cmd_partition(const char *dev, const char *mode) {
    if (!dev || !dev[0]) {
        o_str("partition - prepare a disk for EchOS\n\n");
        o_str("usage:\n");
        o_str("  partition <disk>        MBR + one EchOS partition (type 0x7F, bootable)\n");
        o_str("  partition <disk> gpt    GPT + one Linux partition (type 0x83)\n\n");
        o_str("Then either:\n");
        o_str("  install <disk> ERASE-<disk>     full wizard (format RXFS + save)\n");
        o_str("  format <disk>1 yes && save      manual path after partition\n\n");
        o_str("Boot the OS itself from internal disk (no USB):\n");
        o_str("  on the host:  make hdd && sudo dd if=build/rxos-hdd.img of=/dev/sdX\n\n");
        o_str("Disks available:\n");
        int n = blockdev_count();
        for (int i = 0; i < n; i++) {
            blockdev_t *d = blockdev_at(i);
            if (!d || part_is_view(d)) continue;
            o_str("  "); o_str(d->name); o_str("  ");
            { uint64_t kib = d->sectors * BLOCKDEV_SECTOR / 1024;
              if (kib >= 1024 * 1024) { o_dec(kib / (1024 * 1024)); o_str(" GiB"); }
              else if (kib >= 1024)   { o_dec(kib / 1024);          o_str(" MiB"); }
              else                    { o_dec(kib);                 o_str(" KiB"); } }
            if (is_ram(d)) o_str("  (ram - skip)");
            o_nl();
        }
        return;
    }

    blockdev_t *d = blockdev_get(dev);
    if (!d) {
        o_str("no such disk: "); o_str(dev); o_nl();
        return;
    }
    if (is_ram(d)) {
        o_str("Cannot partition ram0 - it is temporary.\n");
        return;
    }
    if (part_is_view(d)) {
        o_str(dev); o_str(" is a partition, not a whole disk.\n");
        return;
    }

    bool want_gpt = mode && mode[0] &&
        ((mode[0] == 'g' || mode[0] == 'G') &&
         (mode[1] == 'p' || mode[1] == 'P') &&
         (mode[2] == 't' || mode[2] == 'T'));

    o_str("Partitioning "); o_str(d->name); o_str(want_gpt ? " (GPT)...\n" : " (MBR/EchOS)...\n");
    o_str("  !! This erases the partition table on this disk.\n");

    /* Always clear leftover GPT headers so the disk does not stay hybrid. */
    part_wipe_gpt(d);

    if (want_gpt) {
        if (!part_write_gpt_single(d, PART_TYPE_LINUX)) {
            o_str("  [FAIL] could not write GPT (I/O error).\n");
            return;
        }
        /* Written is not the same as valid. Read both copies back and
           check them the way firmware will before saying anything. */
        {
            const char *why = "";
            bool ok = part_verify_gpt(d, &why);
            o_str(ok ? "  [ok] wrote GPT: protective MBR, primary + backup headers,\n"
                     : "  [FAIL] GPT written but did not verify: ");
            if (ok) o_str("        128 entries, CRC32 and GUIDs verified on read-back\n");
            else  { o_str(why); o_nl(); return; }
        }
        o_str("  note: EchOS install uses MBR; for install run without 'gpt':\n");
        o_str("        partition "); o_str(d->name); o_nl();
        return;
    }

    if (!part_write_single_ex(d, PART_TYPE_RXOS, true)) {
        o_str("  [FAIL] could not write MBR (I/O error).\n");
        return;
    }
    o_str("  [ok] wrote MBR with one bootable EchOS partition (type 0x7F)\n");
    o_str("  [ok] first usable LBA "); o_dec((uint64_t)PART_FIRST_LBA);
    o_str(" (8 MiB gap for the bootloader + kernel)\n\n");
    o_str("Next steps:\n");
    o_str("  install "); o_str(d->name); o_str(" ERASE-"); o_str(d->name); o_nl();
    o_str("  or continue with the install wizard / format + save\n");
}

/* Not static any more: the desktop's Start menu reboots through exactly this
   path rather than growing a second one that could drift from it. */
ROGEX_NORETURN void system_reboot(void) {
    console_write("\n[echOS] rebooting...\n");
    /* Wait for the reset controller to be ready, then ask. What the pulse is
       made of belongs to the architecture, not to a shell command. */
    for (int i = 0; i < 100; i++) {
        if (!arch_reboot_input_busy()) break;
        arch_relax();
    }
    arch_reboot();
    arch_halt_forever();                /* if the machine refused, stop */
}

/* `power [seconds]` — the instrumented measurement, not a capability dump.
   We sample the RAPL energy counters, park the core in its deepest idle state
   for a whole number of PIT ticks, sample again, and divide. That yields real
   average idle power in milliwatts on any Intel part from Sandy Bridge on.
   Under QEMU/VirtualBox the MSRs are not implemented, the guarded read faults,
   and we say so instead of printing an invented number. */
static void cmd_power(const char *arg) {
    o_str("EchOS - instrumented power measurement (Intel RAPL)\n \n");
    o_pad("cpu", 12);   o_str(power_cpu_name()); o_nl();
    o_pad("idle via", 12); o_str(power_idle_method());
    if (power_acpi_present()) {
        o_str("  (ACPI "); o_str(power_acpi_oem());
        o_str(", C2 "); o_dec(power_c2_latency());
        o_str("us / C3 "); o_dec(power_c3_latency()); o_str("us)");
    }
    o_nl();
    o_pad("rapl", 12);  o_str(power_rapl_status()); o_nl();
    o_pad("msr guard", 12); o_str(power_msr_guard_status()); o_nl();

    if (!power_rapl_available()) {
        o_str(" \nNo energy counters on this machine, so there is no measured\n");
        o_str("figure to report. RAPL needs an Intel CPU (Sandy Bridge 2011 or\n");
        o_str("later) on bare metal - hypervisors do not emulate MSR 0x606.\n");
        o_str("Boot EchOS from the USB stick on real hardware and run `power`.\n");
        return;
    }
    { uint32_t c;
      o_pad("temp", 12);
      if (thermal_core_c(&c)) {
          o_dec(c); o_str(" C core");
          uint32_t pk;
          if (thermal_package_c(&pk)) { o_str(", "); o_dec(pk); o_str(" C package"); }
          o_str("   (TjMax "); o_dec(thermal_tjmax());
          o_str(thermal_tjmax_measured() ? " C, from the CPU)" : " C, assumed)");
          o_nl();
          o_str("             the digital sensor is accurate near TjMax and\n");
          o_str("             looser when cold - a few degrees of slop down here\n");
      } else { o_str(thermal_status()); o_nl(); } }

    if (power_rapl_tdp_mw()) {
        o_pad("tdp", 12); o_dec(power_rapl_tdp_mw()); o_str(" mW (package thermal spec)\n");
    }

    uint32_t secs = 1;
    if (arg && arg[0] >= '1' && arg[0] <= '9') secs = (uint32_t)(arg[0] - '0');

    uint32_t pkg = 0, core = 0, gpu = 0;
    if (!power_measure_mw(secs * arch_timer_hz(), &pkg, &core, &gpu)) {
        o_str(" \nno measurement: "); o_str(power_measure_status()); o_nl();
        return;
    }

    o_str(" \nidle for "); o_dec(secs); o_str(" s, core parked in ");
    o_str(power_idle_method()); o_str(":\n \n");
    o_pad("  package", 12); o_dec(pkg);  o_str(" mW\n");
    if (core) { o_pad("  cores", 12);    o_dec(core); o_str(" mW\n"); }
    if (gpu)  { o_pad("  graphics", 12); o_dec(gpu);  o_str(" mW\n"); }
    o_str(" \nMeasured, not estimated: energy delta over a known PIT interval.\n");
}

static void cmd_rxmon(void) {
    o_str("=== rxmon: Real-Time Event & System Monitor (" ECHOS_VERSION_NAME ") ===\n \n");

    /* 1. CPU Registers */
    /* Whatever this target calls its registers. The x86 names are not
       hard-coded here any more: on another architecture this prints that
       machine's own state instead of pretending it has an RAX. */
    {
        arch_reg_t regs[24];
        uint32_t n = arch_cpu_state(regs, 24), i;
        o_str("[CPU registers — "); o_str(arch_name()); o_str("]\n");
        for (i = 0; i < n; i++) {
            if ((i % 4) == 0) o_str("  ");
            o_pad(regs[i].name, 7); o_str(": "); o_addr(regs[i].value);
            o_str((i % 4) == 3 ? "\n" : "  ");
        }
        if (n % 4) o_nl();
    }
    o_nl();

    /* 2. Power / Thermal */
    o_str("[Power & Thermal State]\n");
    o_pad("  CPU Model", 16); o_str(power_cpu_name()); o_nl();
    o_pad("  Idle Method", 16); o_str(power_idle_method()); o_nl();
    o_pad("  RAPL State", 16); o_str(power_rapl_status()); o_nl();
    uint32_t tcore = 0;
    if (thermal_core_c(&tcore)) {
        o_pad("  Temperature", 16); o_dec(tcore); o_str(" C (TjMax "); o_dec(thermal_tjmax()); o_str(" C)\n");
    }

    /* 3. Clock & Uptime */
    o_str("\n[System Clock & Timers]\n");
    o_pad("  Uptime", 16); o_dec(arch_timer_uptime_ms() / 1000); o_str(" s ("); o_dec(arch_timer_uptime_ms() * 1000); o_str(" us)\n");
    o_pad("  PIT Ticks", 16); o_dec(arch_timer_ticks()); o_str(" ticks (100 Hz)\n");

    /* 4. Spiking Event Bus (SEB) */
    o_str("\n[Spiking Event Bus (SEB) - Akida Ready]\n");
    seb_stats_t st;
    seb_get_stats(&st);
    o_pad("  Total Spikes", 16); o_dec(st.total_spikes); o_str(" pulses\n");
    o_pad("  Spike Rate", 16); o_dec(st.spikes_per_sec); o_str(" Hz (pulses/sec)\n");
    o_pad("  Ring Queue", 16); o_dec(st.ring_occupancy); o_str(" / 1024 slots (0 overruns)\n");
    o_str("  Channels:\n");
    o_str("    Timer:    "); o_dec(st.channel_spikes[SEB_CH_TIMER]);
    o_str(" | Keyboard: "); o_dec(st.channel_spikes[SEB_CH_KEYBOARD]);
    o_str(" | Mouse: "); o_dec(st.channel_spikes[SEB_CH_MOUSE]); o_nl();
    o_str("    PCI/Dev:  "); o_dec(st.channel_spikes[SEB_CH_PCI]);
    o_str(" | Network:  "); o_dec(st.channel_spikes[SEB_CH_NET]);
    o_str(" | WSP:   "); o_dec(st.channel_spikes[SEB_CH_WSP]);
    o_str(" | Sys:   "); o_dec(st.channel_spikes[SEB_CH_SYS]); o_nl();

    /* 5. Memory Layout Heap-0 */
    o_str("\n[Memory Layout Heap-0 (Deterministic O(1))]\n");
    o_pad("  Static Pool", 16); o_dec(heap0_total_bytes() / 1024); o_str(" KiB reserved at compile time\n");
    o_pad("  Fragmentation", 16); o_str("0.00% (Heap-0 static page guarantees)\n");
    for (uint32_t i = 0; i < heap0_region_count(); i++) {
        const heap0_region_t *r = heap0_get_region(i);
        if (r) {
            o_str("   - "); o_pad(r->name, 18); o_dec(r->size_bytes / 1024); o_str(" KiB : "); o_str(r->purpose); o_nl();
        }
    }
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int wsp_parse_hex(const char *text, wsp_frame_t *out) {
    int n = 0;
    k_memset(out, 0, sizeof(*out));
    for (int i = 0; text[i] && n < WSP_PACKET_BYTES; ) {
        int hi, lo;
        if (text[i] == ' ') { i++; continue; }
        hi = hex_nibble(text[i++]);
        lo = text[i] ? hex_nibble(text[i++]) : -1;
        if (hi < 0 || lo < 0) return -1;
        ((uint8_t *)out)[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

static void wsp_print_hex(const uint8_t *b, int n) {
    static const char H[] = "0123456789ABCDEF";
    for (int i = 0; i < n; i++) {
        char t[4] = { H[b[i] >> 4], H[b[i] & 0x0f], ' ', 0 };
        o_str(t);
    }
}

/* Emit one frame on SEB_CH_WSP and read it back off the ring.
   Nothing in this kernel consumes the SEB ring today: the PIT produces a
   spike per tick, so a minute after boot the ring is full and every emit is
   an overrun. A loopback that ignored that would print PASS on a bus that
   dropped the frame, so this drains the backlog first and says how much it
   found. No turnaround figure is claimed: the SEB timestamp is milliseconds
   and the trip is far shorter than one tick. */
static void wsp_loopback(void) {
    wsp_frame_t tx;
    seb_spike_t got;
    seb_stats_t before, after;
    uint32_t id = 0x57535001u;
    uint64_t drained = 0;
    int passed = 0;

    seb_get_stats(&before);
    while (seb_poll_spike(&got)) drained++;

    k_memset(&tx, 0, sizeof(tx));
    tx.version  = WSP_VERSION;
    tx.opcode   = WSP_OP_PING;
    tx.src_node = 0;
    tx.dst_node = 1;
    wsp_seq_set(&tx, 1);
    for (int i = 0; i < WSP_PAYLOAD_BYTES; i++) tx.payload[i] = (uint8_t)(0xA0 + i);

    seb_emit_spike(SEB_CH_WSP, 500, id, tx.payload, WSP_PAYLOAD_BYTES);
    if (seb_poll_spike(&got) && got.channel == SEB_CH_WSP && got.event_id == id) {
        passed = 1;
        for (int i = 0; i < WSP_PAYLOAD_BYTES; i++)
            if (got.payload[i] != tx.payload[i]) passed = 0;
    }
    seb_get_stats(&after);

    o_str("wsp-tool loopback\n");
    o_str("  sent      "); wsp_print_hex((const uint8_t *)&tx, WSP_PACKET_BYTES); o_nl();
    o_str("  returned  ");
    if (passed) wsp_print_hex(got.payload, WSP_PAYLOAD_BYTES);
    else        o_str("nothing matching on SEB_CH_WSP");
    o_nl();
    o_str("  result    "); o_str(passed ? "PASS" : "FAIL"); o_nl();
    o_str("  ring      "); o_dec(drained);
    o_str(" stale spikes drained before the test; overruns so far ");
    o_dec((uint64_t)before.ring_overruns);
    if (after.ring_overruns != before.ring_overruns) o_str(" (grew during the test)");
    o_nl();
    o_str("  note      nothing consumes this ring yet; the PIT fills it\n");
}

static void cmd_wsp_tool(char *a1, char *rest) {
    wsp_frame_t f;

    if (!a1 || !a1[0] || eqs(a1, "status")) {
        size_t sz = 0;
        (void)heap0_get_wsp_pool(&sz);
        o_str("RogexWSP — 16-byte frame on the Spiking Event Bus\n \n");
        o_str("  frame     "); o_dec(WSP_PACKET_BYTES);
        o_str(" B fixed: version opcode src dst seq[4] payload[8]\n");
        o_str("  channel   SEB_CH_WSP, ring "); o_dec(SEB_RING_CAPACITY);
        o_str(" spikes, "); o_dec(WSP_PAYLOAD_BYTES); o_str(" payload bytes per spike\n");
        o_str("  pool      "); o_dec(sz / 1024); o_str(" KiB of Heap-0 BSS\n \n");
        o_str("  wsp-tool inject <hex>    put a frame on the bus\n");
        o_str("  wsp-tool decode <hex>    read a frame's fields\n");
        o_str("  wsp-tool loopback        emit one frame and read it back\n");
        o_str("  wsp-tool dump            first slots of the Heap-0 pool\n \n");
        o_str("It routes bytes. It does not mean anything by them.\n");
        return;
    }

    if (eqs(a1, "loopback")) { wsp_loopback(); return; }

    if (eqs(a1, "inject") || eqs(a1, "decode")) {
        int n;
        if (!rest || !rest[0]) {
            o_str("usage: wsp-tool "); o_str(a1);
            o_str(" <up to 16 hex bytes, e.g. 01010001000000010a0b0c0d0e0f1011>\n");
            return;
        }
        n = wsp_parse_hex(rest, &f);
        if (n < 0) { o_str("wsp-tool: not hex\n"); return; }
        if (eqs(a1, "inject")) {
            seb_emit_spike(SEB_CH_WSP, 600, 0, f.payload, WSP_PAYLOAD_BYTES);
            o_str("injected on SEB_CH_WSP ("); o_dec((uint64_t)n);
            o_str(" B parsed, "); o_dec(WSP_PAYLOAD_BYTES); o_str(" B carried)\n  ");
            wsp_print_hex((const uint8_t *)&f, WSP_PACKET_BYTES); o_nl();
            return;
        }
        o_str("decoded 16-byte frame\n");
        o_str("  version   0x"); o_addr(f.version);
        if (f.version != WSP_VERSION) o_str("  (not this build's version)");
        o_nl();
        o_str("  opcode    0x"); o_addr(f.opcode);
        o_str("  "); o_str(wsp_opcode_name(f.opcode)); o_nl();
        o_str("  route     node "); o_dec(f.src_node);
        o_str(" -> node "); o_dec(f.dst_node); o_nl();
        o_str("  seq       "); o_dec(wsp_seq_get(&f)); o_nl();
        o_str("  payload   "); wsp_print_hex(f.payload, WSP_PAYLOAD_BYTES); o_nl();
        return;
    }

    if (eqs(a1, "dump")) {
        size_t sz = 0;
        uint8_t *pool = (uint8_t *)heap0_get_wsp_pool(&sz);
        o_str("Heap-0 WSP pool\n");
        o_str("  base "); o_addr((uint64_t)(uintptr_t)pool);
        o_str("  size "); o_dec(sz / 1024); o_str(" KiB\n");
        for (int row = 0; row < 4; row++) {
            o_str("  ["); o_dec((uint64_t)row); o_str("] ");
            if (!pool) { o_str("(pool not bound)\n"); break; }
            wsp_print_hex(pool + row * WSP_PACKET_BYTES, WSP_PACKET_BYTES);
            o_nl();
        }
        return;
    }

    o_str("unknown wsp-tool option; `wsp-tool` lists them\n");
}

/* ------------------------------ dispatcher -------------------------------- */
cmd_result_t commands_dispatch(char *line, char *runfile, size_t runmax,
                               char *out, size_t outmax) {
    char *a1, *rest;
    split3(line, &a1, &rest);
    o_reset(out, outmax);
    if (eqs(line, "verbose")) { cmd_verbose(a1); return CMD_TEXT; }
    v_announce(line, a1);

    if (eqs(line, "help")) {
        static char hbuf[16384];
        o_reset(hbuf, sizeof(hbuf));
        cmd_help();
        console_page(hbuf);
        o_reset(out, outmax);
        return CMD_TEXT;
    }
    if (eqs(line, "echofetch") || eqs(line, "fastfetch") || eqs(line, "neofetch")) {
        echofetch_render(out, outmax); return CMD_TEXT;
    }
    if (eqs(line, "hwprobe") || eqs(line, "lspci")) {
        hwprobe_cmd(out, outmax); return CMD_TEXT;
    }
    if (eqs(line, "bench-snn") || eqs(line, "neurobench-snn")) {
        bench_neuromorphic(out, outmax); return CMD_TEXT;
    }
    if (eqs(line, "prisma5")) {
        if (eqs(a1, "stress")) { prisma5_stress(64, out, outmax); return CMD_TEXT; }
        { prisma5_run_t r; const char *k = a1[0] ? a1 : "alpha";
          prisma5_kind kind = PRISMA5_ALPHA;
          if (eqs(k, "beta")) kind = PRISMA5_BETA;
          else if (eqs(k, "gamma")) kind = PRISMA5_GAMMA;
          else if (eqs(k, "noise")) kind = PRISMA5_NOISE;
          else if (eqs(k, "null")) kind = PRISMA5_NULL;
          else if (eqs(k, "const")) kind = PRISMA5_CONST;
          prisma5_run(kind, &r);
          o_str("[PRISMA5] kind="); o_str(k);
          o_str(" spikes="); o_dec(r.spikes);
          o_str(" sparsity="); o_dec(r.sparsity_x10 / 10); o_str(".");
          o_dec(r.sparsity_x10 % 10); o_str("%\n");
          return CMD_TEXT; }
    }
    if (eqs(line, "live")) { live_status(out, outmax); return CMD_TEXT; }
    if (eqs(line, "echlibc")) { echlibc_status(out, outmax); return CMD_TEXT; }
    if (eqs(line, "echos-install") || eqs(line, "echos_install")) {
        echos_install();
        o_str("echos-install: wizard finished. LIVE session continues.\n");
        return CMD_TEXT;
    }
    if (eqs(line, "echo")) {
        if (a1[0]) { o_str(a1); if (rest[0]) { o_str(" "); o_str(rest); } }
        o_nl();
        return CMD_TEXT;
    }
    if (eqs(line, "whoami")) {
        { const char *u = env_get("USER"); o_str(u && u[0] ? u : "live"); o_nl(); }
        return CMD_TEXT;
    }
    if (eqs(line, "id")) {
        { const char *u = env_get("USER");
          o_str("uid=0("); o_str(u && u[0] ? u : "live");
          o_str(") gid=0(wheel) groups=wheel\n"); }
        return CMD_TEXT;
    }
    if (eqs(line, "uname")) {
        if (eqs(a1, "-a") || eqs(a1, "-srm")) {
            o_str("echOS "); o_str("echos "); o_str(ECHOS_VERSION);
            o_str(" "); o_str(ECHOS_ARCH); o_str(" "); o_str(ECHOS_CODENAME); o_nl();
        } else {
            o_str("echOS\n");
        }
        return CMD_TEXT;
    }
    if (eqs(line, "hostname")) { o_str("echos\n"); return CMD_TEXT; }
    if (eqs(line, "neuro")) {
        o_str("echOS SNN — how the spike stack works\n \n");
        o_str("A leaky integrate-and-fire (LIF) neuron is a capacitor with a\n");
        o_str("leak. Current charges the membrane V; when V crosses a threshold\n");
        o_str("the cell emits a spike (a 1-bit event) and resets. Between spikes\n");
        o_str("it is silent — that sparsity is the energy story.\n \n");
        o_str("In this kernel:\n");
        o_str("  event fabric   SPSC ring of typed events (key, mouse, spike)\n");
        o_str("  Q6 cube        64 LIF cells, 192 integer edges, Heap-0 BSS\n");
        o_str("  PRISMA 5       synthetic EEG → those 64 cells (`prisma5`)\n");
        o_str("  backend        "); o_str(rx_backend_label());
        o_str(" — "); o_str(rx_backend_status()); o_nl();
        o_str("  Akida          PCI 1e7c:bca1 probe. No offload without a card.\n \n");
        o_str("Nothing here is an LLM. Spikes are events. Weights are integers.\n \n");
        o_str("try:  neurocpu          software | akida\n");
        o_str("      prisma5 alpha     synthetic drive of the Q6 cube\n");
        o_str("      bench-snn         sparsity / latency (mW if RAPL)\n");
        return CMD_TEXT;
    }
    if (eqs(line, "doas")) {
        char sub[96];
        char *d = sub, *e = sub + sizeof(sub);
        const char *p;
        if (!a1[0]) {
            o_str("usage: doas <command>\n");
            o_str("one address space; does not isolate.\n");
            return CMD_TEXT;
        }
        if (eqs(a1, "doas")) { o_str("doas: nested doas ignored\n"); return CMD_TEXT; }
        if (!doas_permit("root", a1)) {
            o_str("doas: Operation not permitted\ncheck /etc/doas.conf\n");
            return CMD_TEXT;
        }
        commands_v("one address space — no isolation; re-dispatch in the kernel shell");
        p = a1; while (*p && d < e - 1) *d++ = *p++;
        if (rest[0] && d < e - 2) { *d++ = ' '; p = rest; while (*p && d < e - 1) *d++ = *p++; }
        *d = '\0';
        return commands_dispatch(sub, runfile, runmax, out, outmax);
    }
    if (eqs(line, "epk") || eqs(line, "epk_add") || eqs(line, "epk_delete")) {
        if (eqs(line, "epk_add")) { epk_cmd("install", a1, rest, out, outmax); return CMD_TEXT; }
        if (eqs(line, "epk_delete")) { epk_cmd("remove", a1, rest, out, outmax); return CMD_TEXT; }
        {
            char *a2 = rest, *a3 = rest;
            while (*a3 && *a3 != ' ') a3++;
            if (*a3) { *a3++ = '\0'; while (*a3 == ' ') a3++; }
            else a3 = (char *)"";
            epk_cmd(a1, a2, a3, out, outmax);
        }
        return CMD_TEXT;
    }
    if (bsdtools_cmd(line, a1, rest, out, outmax)) return CMD_TEXT;
    if (eqs(line, "rxmon") || eqs(line, "mon") || eqs(line, "top")) {
        cmd_rxmon(); return CMD_TEXT;
    }
    if (eqs(line, "wsp-tool") || eqs(line, "wsptool") || eqs(line, "wsp")) {
        cmd_wsp_tool(a1, rest); return CMD_TEXT;
    }
    if (eqs(line, "about"))    { cmd_about();   return CMD_TEXT; }
    if (eqs(line, "status"))   { cmd_status();  return CMD_TEXT; }
    if (eqs(line, "mem"))      { cmd_mem(a1);   return CMD_TEXT; }
    if (eqs(line, "limits"))   { cmd_limits();  return CMD_TEXT; }
    if (eqs(line, "font")) {
        if (eqs(a1, "check")) cmd_font_check(rest);
        else cmd_font(a1);
        return CMD_TEXT;
    }
    if (eqs(line, "pane"))     { cmd_pane(a1, rest); return CMD_TEXT; }
    if (eqs(line, "robot"))    { cmd_robot(a1, rest); return CMD_TEXT; }
    if (eqs(line, "px4") || eqs(line, "mavlink")) { cmd_px4(a1, rest); return CMD_TEXT; }
    if (eqs(line, "report"))   { report_emit();  return CMD_TEXT; }
    if (eqs(line, "uptime"))   { cmd_uptime();  return CMD_TEXT; }
    if (eqs(line, "date") || eqs(line, "time")) {
        cmd_date(a1, rest); return CMD_TEXT;
    }
    if (eqs(line, "tz") || eqs(line, "timezone")) {
        cmd_tz(a1, rest); return CMD_TEXT;
    }
    if (eqs(line, "power"))   { cmd_power(a1); return CMD_TEXT; }
    if (eqs(line, "bench"))   { neurobench_run(); return CMD_TEXT; }
    if (eqs(line, "capture")) {
        capture_screen(0, 0);
        o_str(capture_last_status()); o_nl();
        return CMD_TEXT;
    }
    if (eqs(line, "neurocpu") || eqs(line, "backend")) {
        cmd_neurocpu(a1); return CMD_TEXT;
    }
    if (eqs(line, "devices")) { cmd_devices(); return CMD_TEXT; }
    if (eqs(line, "dev"))     { cmd_dev();     return CMD_TEXT; }
    if (eqs(line, "env"))     { cmd_env_set(a1, rest); return CMD_TEXT; }
    if (eqs(line, "theme"))   { cmd_theme();   return CMD_TEXT; }
    if (eqs(line, "split") || eqs(line, "vsplit") || eqs(line, "hsplit") ||
        eqs(line, "unsplit") || eqs(line, "only")) {
#if ECHOS_CLI_ONLY
        o_str("split: no window manager in this CLI. one console.\n");
#else
        o_str("ROSH split view (desktop Terminal, key t)\n");
        o_str("  split / vsplit     vertical   (Ctrl+E)\n");
        o_str("  hsplit             horizontal (Ctrl+O)\n");
        o_str("  unsplit            one pane   (Ctrl+W)\n");
        o_str("  Ctrl+N             next pane\n");
        o_str("  PgUp / PgDn        scroll lists (also Ctrl+Up / Ctrl+Down)\n");
#endif
        return CMD_TEXT;
    }
    if (eqs(line, "termtheme") || eqs(line, "termcolor")) {
        static const char *names[] = { "night", "matrix", "ocean", "amber", "paper" };
        int i, id = -1;
        if (a1[0]) {
            for (i = 0; i < 5; i++) if (eqs(a1, names[i])) { id = i; break; }
        }
        if (id < 0) {
            const char *cur = env_get("TERM_THEME");
            id = 0;
            if (cur) for (i = 0; i < 5; i++) if (eqs(cur, names[i])) { id = (i + 1) % 5; break; }
        }
        env_set("TERM_THEME", names[id]);
        if (!console_set_theme(names[id])) {
            o_str("termtheme: unknown palette\n");
            return CMD_TEXT;
        }
        console_clear();
        o_str("termtheme: "); o_str(names[id]); o_str("  (CLI palette applied)\n");
        o_str("palettes: night  matrix  ocean  amber  paper\n");
        return CMD_TEXT;
    }
    if (eqs(line, "kbd"))     { cmd_kbd(a1);   return CMD_TEXT; }
    if (eqs(line, "wired") || eqs(line, "mesh")) { cmd_wired(); return CMD_TEXT; }
    if (eqs(line, "ping"))    { cmd_ping(a1);  return CMD_TEXT; }
    if (eqs(line, "chat"))    { cmd_chat(a1, rest); return CMD_TEXT; }
    if (eqs(line, "say")) {
        /* broadcast: say <message...> — a1 is first word, rest follows */
        if (a1[0] && rest[0]) {
            /* rejoin a1 + rest roughly: put message as "a1 rest" */
            char msg[220];
            char *d = msg; char *e = msg + sizeof(msg) - 1;
            const char *p = a1; while (*p && d < e) *d++ = *p++;
            if (rest[0] && d < e) { *d++ = ' '; p = rest; while (*p && d < e) *d++ = *p++; }
            *d = '\0';
            cmd_chat("", msg);
        } else cmd_chat("", a1);
        return CMD_TEXT;
    }
    if (eqs(line, "www"))     { cmd_www(a1, rest); return CMD_TEXT; }
    if (eqs(line, "curl"))    { cmd_curl(a1, rest); return CMD_TEXT; }
    if (eqs(line, "dns"))     { cmd_dns(a1[0] ? a1 : rest); return CMD_TEXT; }
    if (eqs(line, "tls"))     { cmd_tls(a1[0] ? a1 : rest); return CMD_TEXT; }
    if (eqs(line, "wget"))    { cmd_wget(a1, rest); return CMD_TEXT; }
    if (eqs(line, "wdl"))     { netcli_wdl(a1, rest); return CMD_TEXT; }
    if (eqs(line, "ipconf") || eqs(line, "ipconfig") || eqs(line, "ifconfig")) {
        netcli_ipconf(); return CMD_TEXT;
    }
    if (eqs(line, "man") || eqs(line, "manual")) {
        man_cmd(a1, rest); return CMD_TEXT;
    }
    if (eqs(line, "apropos")) {
        man_cmd("-k", a1[0] ? a1 : rest); return CMD_TEXT;
    }
    if (eqs(line, "browse") || eqs(line, "nova")) {
        o_str("browse / nova: not in this CLI.  try: curl <url>\n");
        return CMD_TEXT;
    }
    if (eqs(line, "trace") || eqs(line, "traceroute")) {
        trace_cmd(a1, out, outmax); return CMD_TEXT;
    }
    if (eqs(line, "nmap")) {
        nmap_cmd(a1[0] ? a1 : rest, out, outmax); return CMD_TEXT;
    }
    if (eqs(line, "nics") || eqs(line, "lspci-net")) {
        nics_cmd(out, outmax); return CMD_TEXT;
    }
    if (eqs(line, "update"))  { update_cmd("update", a1, rest, out, outmax); return CMD_TEXT; }
    if (eqs(line, "wallpaper")) {
        update_cmd("wallpaper", a1, rest, out, outmax); return CMD_TEXT;
    }
    if (eqs(line, "catalog") || eqs(line, "ls") || eqs(line, "dir")) {
        cmd_catalog(a1[0] ? a1 : NULL); return CMD_TEXT;
    }
    if (eqs(line, "wander") || eqs(line, "cd")) { cmd_wander(a1); return CMD_TEXT; }
    if (eqs(line, "whereami") || eqs(line, "pwd")) { cmd_whereami(); return CMD_TEXT; }
    if (eqs(line, "unveil") || eqs(line, "cat")) { cmd_cat(a1); return CMD_TEXT; }
    if (eqs(line, "vanish") || eqs(line, "rm") || eqs(line, "del")) {
        cmd_rm(a1); return CMD_TEXT;
    }
    if (eqs(line, "relabel") || eqs(line, "rename") || eqs(line, "mv")) {
        cmd_rename(a1, rest); return CMD_TEXT;
    }
    if (eqs(line, "forge") || eqs(line, "copy") || eqs(line, "cp")) {
        cmd_copy(a1, rest); return CMD_TEXT;
    }
    if (eqs(line, "homestead") || eqs(line, "mkdir")) {
        cmd_homestead(a1); return CMD_TEXT;
    }
    if (eqs(line, "raze") || eqs(line, "rmdir")) { cmd_raze(a1); return CMD_TEXT; }
    if (eqs(line, "vault") || eqs(line, "df")) { cmd_vault(); return CMD_TEXT; }
    if (eqs(line, "peep") || eqs(line, "head")) { cmd_peep(a1); return CMD_TEXT; }
    if (eqs(line, "tree")) { cmd_tree(a1[0] ? a1 : NULL); return CMD_TEXT; }
    if (eqs(line, "history")) { cmd_history(); return CMD_TEXT; }
    if (eqs(line, "nano") || eqs(line, "pico")) {
        nano_run(a1[0] ? a1 : "");
        o_str("nano: closed.\n");
        return CMD_TEXT;
    }
    if (eqs(line, "scribe")) {
        cmd_write(a1, rest); return CMD_TEXT;
    }
    if (eqs(line, "apps"))    { cmd_apps();    return CMD_TEXT; }
    if (eqs(line, "save"))    { cmd_save();    return CMD_TEXT; }
    if (eqs(line, "load"))    { cmd_load();    return CMD_TEXT; }
    if (eqs(line, "format"))  { cmd_format(a1, rest); return CMD_TEXT; }
    if (eqs(line, "install")) { install_wizard(a1, rest); return CMD_TEXT; }
    if (eqs(line, "reinstall")) {
        if (!a1[0]) { o_str("usage: reinstall <disk>\n"); return CMD_TEXT; }
        char ph[48]; int n = 0;
        const char *p = "REINSTALL-";
        while (*p && n < 47) ph[n++] = *p++;
        p = a1; while (*p && n < 47) ph[n++] = *p++;
        ph[n] = '\0';
        install_wizard(a1, ph);
        return CMD_TEXT;
    }
    if (eqs(line, "cfdisk") || eqs(line, "fdisk")) {
        char *op = rest, *arg2 = rest;
        while (*arg2 && *arg2 != ' ') arg2++;
        if (*arg2) { *arg2++ = '\0'; while (*arg2 == ' ') arg2++; }
        else arg2 = (char *)"";
        disks_cmd(a1, op, arg2, out, outmax);
        return CMD_TEXT;
    }
    if (eqs(line, "partition")) { cmd_partition(a1, rest); return CMD_TEXT; }
    if (eqs(line, "clear") || eqs(line, "Clear") || eqs(line, "cls")) {
        return CMD_CLEAR;
    }
    if (eqs(line, "sudoers")) {
        uint32_t n = 0;
        const char *s = vfs_read("/etc/sudoers", &n);
        if (s && n) {
            uint32_t i;
            for (i = 0; i < n && o_len + 1 < o_max; i++) {
                char c = s[i];
                if (c == '\n' || ((unsigned char)c >= 0x20 && (unsigned char)c != 0x7f)) {
                    char b[2] = { c, 0 }; o_str(b);
                }
            }
            if (o_len && o_buf[o_len - 1] != '\n') o_nl();
        } else o_str("no /etc/sudoers\n");
        return CMD_TEXT;
    }
    if (eqs(line, "reboot"))  { system_reboot(); }
    if (eqs(line, "halt") || eqs(line, "exit") || eqs(line, "poweroff"))
        return CMD_HALT;
    if (eqs(line, "write")) {
        /* re-join: write <file> <rest of line as content> */
        cmd_write(a1, rest);
        return CMD_TEXT;
    }
    if (eqs(line, "run")) {
        if (!a1[0]) { o_str("usage: run <file.rxc>\n"); return CMD_TEXT; }
        /* Prefer the given path; also try /bin/<name> for installed packages. */
        if (vfs_size(a1) < 0) {
            char alt[RXFS_NAME];
            char *d = alt, *e = alt + sizeof(alt);
            const char *p = "/bin/";
            while (*p && d < e - 1) *d++ = *p++;
            p = a1; while (*p && d < e - 1) *d++ = *p++;
            *d = '\0';
            if (vfs_size(alt) >= 0) cpn(runfile, alt, runmax);
            else {
                /* also /bin/<name>.rxc from `rx app add` */
                char rxc[RXFS_NAME];
                char *d2 = rxc, *e2 = rxc + sizeof(rxc);
                const char *q = "/bin/";
                while (*q && d2 < e2 - 1) *d2++ = *q++;
                q = a1; while (*q && d2 < e2 - 5) *d2++ = *q++;
                *d2++ = '.'; *d2++ = 'r'; *d2++ = 'x'; *d2++ = 'c'; *d2 = '\0';
                if (vfs_size(rxc) >= 0) cpn(runfile, rxc, runmax);
                else {
                    o_str("run: not found: "); o_str(a1); o_nl();
                    o_str("try: rx app add <name>   or   fortune   (no run needed)\n");
                    return CMD_TEXT;
                }
            }
        } else {
            cpn(runfile, a1, runmax);
        }
        return CMD_RUN_FILE;
    }
    if (eqs(line, "rx")) {
        /* line was tokenized: a1 = first arg, rest = remainder.
           Examples:  rx app add hellopkg
                      rx search fortune
                      rx demo */
        char *a2 = rest, *a3 = rest;
        while (*a3 && *a3 != ' ') a3++;
        if (*a3) { *a3++ = '\0'; while (*a3 == ' ') a3++; }
        else a3 = (char *)"";
        /* When `rx app add X`: a1=app, a2=add, a3=X
           When `rx add X`:     a1=add, a2=X,  a3=""  — pkg_cmd_rx handles both */
        if (eqs(a1, "update")) {
            update_cmd("update", a2, a3, out, outmax);
            return CMD_TEXT;
        }
        pkg_cmd_rx(a1, a2, a3, out, outmax);
        return CMD_TEXT;
    }
    /* LIVE / installed epk catalog entries (tcc, python3.14, qjs, ...). */
    if (epk_run_command(line, a1, rest, out, outmax))
        return CMD_TEXT;
    /* Installed packages (`rx app add fortune`) are real terminal commands. */
    if (pkg_try_command(line, a1[0] ? a1 : rest, out, outmax))
        return CMD_TEXT;
    return CMD_NOT_HANDLED;
}
