/*
 * man / manual — on-box pages for every shell command.
 *
 * No pager (the console already scrolls). `man -k word` searches NAME+blurb.
 */
#include "man.h"
#include "commands.h"
#include "version.h"

static int eqs(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int has_word(const char *hay, const char *needle) {
    int i, j;
    if (!hay || !needle || !needle[0]) return 0;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j] && hay[i + j] && hay[i + j] == needle[j]; j++) { }
        if (!needle[j]) return 1;
    }
    return 0;
}

static int in_list(const char *list, const char *name) {
    const char *p = list;
    int i;
    if (!list || !name) return 0;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        i = 0;
        while (p[i] && p[i] != ' ' && p[i] != ',') {
            if (p[i] != name[i]) { i = -1; break; }
            i++;
        }
        if (i > 0 && name[i] == '\0') return 1;
        while (*p && *p != ' ' && *p != ',') p++;
    }
    return 0;
}

typedef struct {
    const char *name;
    const char *also;
    const char *page;
} man_page_t;

/* Long pages first (curl/wdl/man/www/tls/ipconf). Short blurbs follow. */
static const man_page_t PAGES[] = {
    { "man", "manual apropos",
      "NAME\n"
      "  man, manual — show the manual page for a command\n"
      "\n"
      "SYNOPSIS\n"
      "  man [section] <command>\n"
      "  man -k <word>          (alias: apropos, man --apropos)\n"
      "  man                    list every documented command\n"
      "  manual <command>       same as man\n"
      "\n"
      "DESCRIPTION\n"
      "  Prints syntax, flags, and examples for a shell command. There is no\n"
      "  pager: use PgUp / PgDn or the mouse wheel to scroll. Section numbers\n"
      "  (man 1 curl) are accepted and ignored — every page lives in one book.\n"
      "\n"
      "EXAMPLES\n"
      "  man curl\n"
      "  man wdl\n"
      "  man -k download\n"
    },
    { "curl", "",
      "NAME\n"
      "  curl — transfer a URL (HTTP/1.0, HTTPS TLS 1.3 / 1.2)\n"
      "\n"
      "SYNOPSIS\n"
      "  curl [options] <url>\n"
      "  curl -X POST -d BODY <url>\n"
      "  curl search <query>\n"
      "\n"
      "DESCRIPTION\n"
      "  Client HTTP/1.0 over the www stack. https:// uses TLS 1.3 AES-128-GCM\n"
      "  (fallback TLS 1.2 ECDHE-RSA-AES128-GCM) with SNI. No CA pin yet.\n"
      "  Quoted arguments work:  -d \"usuario=juan&clave=123\"\n"
      "  A '$' between form fields is treated as '&' (shell does not expand $).\n"
      "\n"
      "OPTIONS\n"
      "  -X, --request <M>   method: GET POST HEAD PUT (default GET)\n"
      "  -d, --data BODY     POST body (x-www-form-urlencoded)\n"
      "  -I, --head          HEAD: print response headers only\n"
      "  -L, --location      follow 301/302/303/307/308 (ON by default)\n"
      "  -O, --remote-name   save as the basename of the URL\n"
      "  -o, --output PATH   save body to PATH\n"
      "  -b, --cookie STR    send Cookie: STR\n"
      "  -c, --cookie-jar F  write Set-Cookie name=value into F\n"
      "  -H, --header LINE   extra request header\n"
      "  --html              dump raw HTML instead of visible text\n"
      "  -dom                wait ~2s, re-GET, dump visible text + HTML\n"
      "                      (no JS engine: Vite SPAs stay as the shell HTML)\n"
      "\n"
      "EXAMPLES\n"
      "  curl https://example.com\n"
      "  curl -I -L https://www.rogexlaboratories.com\n"
      "  curl -X POST -d \"usuario=juan&clave=123\" https://ejemplo.com\n"
      "  curl -o /tmp/page.html https://google.com\n"
      "  curl -O https://example.com/file.bin\n"
      "  curl -dom https://www.rogexlaboratories.com\n"
      "  curl -b \"sid=1\" -c /tmp/jar https://ejemplo.com\n"
      "\n"
      "SEE ALSO\n"
      "  man wdl, man wget, man www, man tls, man ipconf\n"
    },
    { "wdl", "",
      "NAME\n"
      "  wdl — web download (wget-like, no browser)\n"
      "\n"
      "SYNOPSIS\n"
      "  wdl [options] <url>\n"
      "\n"
      "DESCRIPTION\n"
      "  Downloads a remote file onto RXFS. Default destination is\n"
      "  /tmp/<basename>. Follows redirects. Same HTTP/HTTPS stack as curl.\n"
      "  RXFS files are 64 KiB; larger bodies are truncated on write.\n"
      "\n"
      "OPTIONS\n"
      "  -O, -o PATH     write to PATH (instead of /tmp/<name>)\n"
      "  -c              continue: Range: bytes=<size>- if PATH exists\n"
      "  -b              \"background\": unikernel has no fork; the transfer\n"
      "                  still runs to completion, then the prompt returns\n"
      "  -L              follow redirects (already the default)\n"
      "\n"
      "EXAMPLES\n"
      "  wdl https://google.com\n"
      "  wdl -O /users/live/Downloads/index.html https://example.com\n"
      "  wdl -c -O /tmp/big.bin https://example.com/big.bin\n"
      "\n"
      "SEE ALSO\n"
      "  man curl, man wget, man www\n"
    },
    { "wget", "",
      "NAME\n"
      "  wget — HTTP download to a file\n"
      "\n"
      "SYNOPSIS\n"
      "  wget [options] <url> [path]\n"
      "\n"
      "DESCRIPTION\n"
      "  Same engine as wdl. Default path /tmp/<basename>. Options -O -o -c -b -L\n"
      "  match wdl. A bare second argument starting with / is still accepted as\n"
      "  the destination (legacy form).\n"
      "\n"
      "SEE ALSO\n"
      "  man wdl, man curl\n"
    },
    { "www", "",
      "NAME\n"
      "  www — bring up IPv4/DNS/TCP/HTTP(S)\n"
      "\n"
      "SYNOPSIS\n"
      "  www on | off | status\n"
      "  www config <ip> <mask> <gw> [dns]\n"
      "  www dns <host>\n"
      "  www get <url> | www post <url> [body]\n"
      "  www source <url>     dump HTML source\n"
      "  www channel [url]\n"
      "\n"
      "DESCRIPTION\n"
      "  WWW is OFF until `www on`. QEMU slirp defaults to 10.0.2.15/24.\n"
      "  HTTPS = TLS 1.3 AES-128-GCM, fallback TLS 1.2. No certificate pin.\n"
      "\n"
      "SEE ALSO\n"
      "  man curl, man tls, man ipconf, man dns\n"
    },
    { "tls", "",
      "NAME\n"
      "  tls — handshake probe against host:443\n"
      "\n"
      "SYNOPSIS\n"
      "  tls <host>\n"
      "\n"
      "DESCRIPTION\n"
      "  Offers TLS 1.3 (AES_128_GCM_SHA256) and TLS 1.2\n"
      "  (ECDHE_RSA_AES_128_GCM_SHA256) with SNI. Prints the negotiated\n"
      "  version. Does not fetch a page. Leaf RSA is used to check the 1.2\n"
      "  ServerKeyExchange; there is no CA store.\n"
      "\n"
      "EXAMPLE\n"
      "  www on\n"
      "  tls www.rogexlaboratories.com\n"
    },
    { "ipconf", "ipconfig ifconfig",
      "NAME\n"
      "  ipconf — network interface IP and MAC\n"
      "\n"
      "SYNOPSIS\n"
      "  ipconf\n"
      "\n"
      "DESCRIPTION\n"
      "  Lists the bound NIC (driver, MAC, IPv4, netmask, gateway, DNS) and\n"
      "  every PCI Ethernet/WiFi chip the firmware enumerated. Aliases:\n"
      "  ipconfig, ifconfig. For the PCI-only inventory see `nics`.\n"
      "\n"
      "SEE ALSO\n"
      "  man nics, man www, man hwprobe\n"
    },
    { "dns", "",
      "NAME\n"
      "  dns — resolve a hostname to IPv4\n"
      "SYNOPSIS\n"
      "  dns <host>\n"
      "DESCRIPTION\n"
      "  DNS A lookup through the www resolver (needs `www on`).\n"
      "EXAMPLE\n"
      "  dns google.com\n"
    },
    { "ping", "",
      "NAME\n"
      "  ping — ICMP echo, or Rogex Wired L2 ping\n"
      "SYNOPSIS\n"
      "  ping <host|ip|alias>\n"
      "DESCRIPTION\n"
      "  Dotted IP / hostname: ICMP (www on). A Wired alias: L2 0x88B5.\n"
      "  QEMU slirp often filters ICMP; a UDP/53 fallback is tried.\n"
    },
    { "nmap", "",
      "NAME\n"
      "  nmap — small TCP port probe\n"
      "SYNOPSIS\n"
      "  nmap <host|ip>\n"
      "DESCRIPTION\n"
      "  Connects to 21 22 23 25 53 80 110 143 443 445 3306 8080 8443.\n"
      "  Needs `www on` for DNS. Not a stealth scanner.\n"
    },
    { "traceroute", "trace",
      "NAME\n"
      "  traceroute, trace — hop-by-hop path\n"
      "SYNOPSIS\n"
      "  traceroute <host|ip>\n"
      "  trace <alias|ip>\n"
      "DESCRIPTION\n"
      "  ICMP TTL path when www is up; L2 hop when the target is a Wired alias.\n"
    },
    { "nano", "pico",
      "NAME\n"
      "  nano, pico — TUI editor\n"
      "SYNOPSIS\n"
      "  nano [file]\n"
      "  pico [file]\n"
      "DESCRIPTION\n"
      "  Line numbers, C/Python/JS/HTML/Rust colours. Ctrl+W save, Esc exit.\n"
    },
    { "tree", "",
      "NAME\n"
      "  tree — recursive coloured listing\n"
      "SYNOPSIS\n"
      "  tree [path]\n"
      "DESCRIPTION\n"
      "  Default: current directory. `tree /` walks the whole RXFS.\n"
    },
    { "history", "",
      "NAME\n"
      "  history — typed command ring\n"
      "SYNOPSIS\n"
      "  history\n"
      "DESCRIPTION\n"
      "  Up/Down arrows walk the same ring. Not written to disk.\n"
    },
    { "help", "",
      "NAME\n"
      "  help — short command list\n"
      "SYNOPSIS\n"
      "  help\n"
      "DESCRIPTION\n"
      "  One-liners. For syntax and flags use `man <command>`.\n"
    },
    { "echofetch", "fastfetch neofetch",
      "NAME\n"
      "  echofetch — system card + official Braille logo\n"
      "SYNOPSIS\n"
      "  echofetch\n"
    },
    { "epk", "epk_add epk_delete",
      "NAME\n"
      "  epk — RXFS notes for what this unikernel actually has. not an ELF installer.\n"
      "SYNOPSIS\n"
      "  epk list | list --lab | info [pkg] | install <pkg> | remove <pkg>\n"
      "  epk search <word> | apply <edition> | get <pkg>\n"
      "DESCRIPTION\n"
      "  `epk list` is kernel notes. `epk list --lab` is host toolchain / daemon\n"
      "  flags. Those names are not in-OS binaries. `epk get` needs `www on`.\n"
    },
    { "termtheme", "termcolor",
      "NAME\n"
      "  termtheme — CLI palette (not a desktop theme)\n"
      "SYNOPSIS\n"
      "  termtheme [night|matrix|ocean|amber|paper]\n"
    },
    { "clear", "cls",
      "NAME\n"
      "  clear, cls — wipe the console\n"
    },
    { "ls", "catalog dir",
      "NAME\n"
      "  ls, catalog — list a directory\n"
      "SYNOPSIS\n"
      "  ls [path]\n"
    },
    { "cd", "wander",
      "NAME\n"
      "  cd, wander — change directory\n"
      "SYNOPSIS\n"
      "  cd <path>\n"
    },
    { "pwd", "whereami",
      "NAME\n"
      "  pwd, whereami — print working directory\n"
    },
    { "cat", "unveil",
      "NAME\n"
      "  cat, unveil — print a file\n"
      "SYNOPSIS\n"
      "  cat <file>\n"
    },
    { "rm", "vanish del",
      "NAME\n"
      "  rm, vanish — delete a file\n"
      "SYNOPSIS\n"
      "  rm <file>\n"
    },
    { "cp", "forge copy",
      "NAME\n"
      "  cp, forge — copy a file\n"
      "SYNOPSIS\n"
      "  cp <src> <dst>\n"
    },
    { "mv", "relabel rename",
      "NAME\n"
      "  mv, relabel — rename / move\n"
      "SYNOPSIS\n"
      "  mv <old> <new>\n"
    },
    { "mkdir", "homestead",
      "NAME\n"
      "  mkdir, homestead — create a directory\n"
    },
    { "rmdir", "raze",
      "NAME\n"
      "  rmdir, raze — remove an empty directory\n"
    },
    { "write", "scribe",
      "NAME\n"
      "  write, scribe — overwrite a file with the rest of the line\n"
      "SYNOPSIS\n"
      "  write <file> <text...>\n"
    },
    { "head", "peep",
      "NAME\n"
      "  head, peep — first lines of a file\n"
    },
    { "tail", "",
      "NAME\n"
      "  tail — last lines of a file\n"
    },
    { "less", "",
      "NAME\n"
      "  less — page a file through the console scrollback\n"
    },
    { "grep", "",
      "NAME\n"
      "  grep — search file contents\n"
      "SYNOPSIS\n"
      "  grep <pattern> [path]\n"
    },
    { "find", "",
      "NAME\n"
      "  find — walk names under a path\n"
    },
    { "echo", "",
      "NAME\n"
      "  echo — print arguments\n"
    },
    { "whoami", "id",
      "NAME\n"
      "  whoami, id — current user\n"
    },
    { "uname", "hostname",
      "NAME\n"
      "  uname — " ECHOS_VERSION_NAME " kernel/os string\n"
      "  hostname — prints echos\n"
    },
    { "date", "time",
      "NAME\n"
      "  date, time — local wall clock (CMOS RTC is UTC)\n"
      "SYNOPSIS\n"
      "  date\n"
      "  date -u\n"
      "  date set YYYY-MM-DD HH:MM:SS\n"
      "The time you type is local. Spain in summer is Europe/Madrid (UTC+2).\n"
    },
    { "tz", "timezone",
      "NAME\n"
      "  tz — timezone (DST for Europe/*)\n"
      "SYNOPSIS\n"
      "  tz\n"
      "  tz list\n"
      "  tz Europe/Madrid\n"
      "  tz UTC\n"
      "  tz +2\n"
    },
    { "uptime", "",
      "NAME\n"
      "  uptime — time since PIT init\n"
    },
    { "mem", "",
      "NAME\n"
      "  mem — Heap-0 / PMM stats; `mem map` firmware map\n"
    },
    { "status", "about",
      "NAME\n"
      "  status — boot/subsystem card\n"
      "  about  — what echOS is\n"
    },
    { "reboot", "",
      "NAME\n"
      "  reboot — pulse 8042 reset. Does not return.\n"
    },
    { "halt", "exit poweroff power",
      "NAME\n"
      "  halt, poweroff — stop the machine\n"
      "  power [s] — RAPL idle sample when the CPU exposes it\n"
    },
    { "df", "vault du dysk",
      "NAME\n"
      "  df, vault — RXFS used/free\n"
      "  du [path]  — directory sizes\n"
      "  dysk       — disk inventory (human)\n"
    },
    { "nics", "lspci-net",
      "NAME\n"
      "  nics — PCI Ethernet/WiFi inventory\n"
      "DESCRIPTION\n"
      "  Vendor:device, class, bound driver. MAC is on the bound NIC.\n"
      "  For IP+MAC in one view use `ipconf`.\n"
    },
    { "hwprobe", "lspci",
      "NAME\n"
      "  hwprobe — PCI, storage, Akida AKD1000, NICs\n"
    },
    { "doas", "",
      "NAME\n"
      "  doas — one address space; does not isolate. re-dispatch if /etc/doas.conf permits.\n"
      "SYNOPSIS\n"
      "  doas <command> [args]\n"
      "DESCRIPTION\n"
      "  Unikernel: no second process, no privilege boundary. Elevation is already\n"
      "  the default. A permit line is policy text, not a sandbox.\n"
    },
    { "sudoers", "",
      "NAME\n"
      "  sudoers — print /etc/sudoers policy\n"
    },
    { "install", "reinstall echos-install echos_install",
      "NAME\n"
      "  install — guided disk install (ERASE- confirm)\n"
      "  reinstall <dev> — wipe leftover GPT/MBR and format\n"
      "  echos-install — TUI wizard from LIVE\n"
    },
    { "partition", "cfdisk fdisk gpt disklabel",
      "NAME\n"
      "  partition / cfdisk / fdisk / gpt / disklabel — disk layout tools\n"
      "DESCRIPTION\n"
      "  Destructive. Do not run on LIVE unless you intend to wipe a disk.\n"
    },
    { "format", "save load",
      "NAME\n"
      "  format <dev> yes — empty RXFS on a named disk\n"
      "  save | load — persist / restore the RXFS image\n"
    },
    { "env", "",
      "NAME\n"
      "  env — volatile environment (BSS; gone at power-off)\n"
      "SYNOPSIS\n"
      "  env\n"
      "  env <KEY> <value>\n"
    },
    { "apps", "run rx",
      "NAME\n"
      "  apps — list Roxenite apps\n"
      "  run <file.rxc> — Roxenite runtime\n"
      "  rx ... — legacy package / update channel\n"
    },
    { "browse", "nova",
      "NAME\n"
      "  browse, nova — retired in " ECHOS_VERSION_NAME " CLI\n"
      "SYNOPSIS\n"
      "  use curl / wdl instead\n"
    },
    { "wired", "mesh chat say",
      "NAME\n"
      "  wired — Rogex Wired L2 (ethertype 0x88B5)\n"
      "  chat <alias> <msg> | say <msg>\n"
    },
    { "kbd", "",
      "NAME\n"
      "  kbd — keyboard map  es | us | mac\n"
    },
    { "live", "",
      "NAME\n"
      "  live — LIVE image status (packages, FHS, edition)\n"
    },
    { "echlibc", "",
      "NAME\n"
      "  echlibc — C library status for userland builds\n"
    },
    { "prisma5", "bench-snn neurobench neuro neurocpu bench",
      "NAME\n"
      "  prisma5 — synthetic drive of the 64-cell Q6 cube (not EEG hardware)\n"
      "  bench-snn — sparsity / latency (mW only if RAPL)\n"
      "  neurocpu [software|akida] — akida is a PCI probe, not offload\n"
    },
    { "neuro", "neurocpu prisma5 bench-snn",
      "NAME\n"
      "  neuro — LIF / SNN explainer (how the spike stack works)\n"
      "  neurocpu [software|akida] — fabric backend (akida is PCI probe)\n"
      "  prisma5 — synthetic drive of the 64-cell Q6 cube; not EEG hardware\n"
      "  bench-snn — sparsity / latency (mW only if RAPL)\n"
    },
    { "top", "rxmon",
      "NAME\n"
      "  top, rxmon — in-kernel task / event card. not htop.\n"
    },
    { "update", "wallpaper",
      "NAME\n"
      "  update — file patches (check|download|apply|now|status)\n"
      "  wallpaper [name] — default|aurora|ember|midnight|solar\n"
    },
    { "devices", "dev",
      "NAME\n"
      "  devices — device registry\n"
      "  dev — /dev nodes (thermal, rapl, ...)\n"
    },
    { "rcctl", "pfctl",
      "NAME\n"
      "  rcctl — enable/disable flags. start does not spawn a daemon.\n"
      "  pfctl — RAM boolean + dump of /etc/pf.conf. not a packet filter.\n"
    },
    { "split", "vsplit hsplit unsplit",
      "NAME\n"
      "  split — not in this CLI (no window manager, one console)\n"
    },
    { "theme", "",
      "NAME\n"
      "  theme — desktop chrome (CLI uses termtheme)\n"
    },
    { "capture", "",
      "NAME\n"
      "  capture — 48x27 BMP under /screenshots (F12)\n"
    },
};

static const man_page_t *find_page(const char *name) {
    unsigned i;
    if (!name || !name[0]) return 0;
    for (i = 0; i < sizeof(PAGES) / sizeof(PAGES[0]); i++) {
        if (eqs(PAGES[i].name, name)) return &PAGES[i];
        if (PAGES[i].also && in_list(PAGES[i].also, name)) return &PAGES[i];
    }
    return 0;
}

static void print_index(void) {
    unsigned i, col = 0;
    o_str("What manual page do you want?\n");
    o_str("Usage: man <command>    man -k <word>    manual <command>\n \n");
    o_str("Documented commands:\n");
    for (i = 0; i < sizeof(PAGES) / sizeof(PAGES[0]); i++) {
        o_str("  ");
        o_str(PAGES[i].name);
        {
            int n = 0;
            const char *p = PAGES[i].name;
            while (*p++) n++;
            while (n < 14) { o_str(" "); n++; }
        }
        col++;
        if (col == 4) { o_nl(); col = 0; }
    }
    if (col) o_nl();
    o_str(" \nAlso: ");
    o_str(ECHOS_VERSION_NAME);
    o_str("  —  try  man curl  man wdl  man ipconf\n");
}

static void print_apropos(const char *word) {
    unsigned i, hits = 0;
    if (!word || !word[0]) { o_str("usage: man -k <word>\n"); return; }
    o_str("apropos "); o_str(word); o_str(":\n");
    for (i = 0; i < sizeof(PAGES) / sizeof(PAGES[0]); i++) {
        if (has_word(PAGES[i].name, word) ||
            has_word(PAGES[i].also, word) ||
            has_word(PAGES[i].page, word)) {
            o_str("  "); o_str(PAGES[i].name);
            if (PAGES[i].also && PAGES[i].also[0]) {
                o_str(" ("); o_str(PAGES[i].also); o_str(")");
            }
            o_nl();
            hits++;
        }
    }
    if (!hits) { o_str("  (nothing matches)\n"); }
}

void man_cmd(const char *topic, const char *rest) {
    const man_page_t *p;
    const char *name = topic;
    if (name && name[0] >= '0' && name[0] <= '9' && !name[1] && rest && rest[0])
        name = rest;
    if (!name || !name[0]) { print_index(); return; }
    if (eqs(name, "-k") || eqs(name, "--apropos") || eqs(name, "apropos")) {
        print_apropos(rest && rest[0] ? rest : 0);
        return;
    }
    p = find_page(name);
    if (!p) {
        commands_v("no embedded page for that name");
        o_str("No manual entry for "); o_str(name); o_str(".\n");
        o_str("Try: man            (index)\n");
        o_str("     man -k "); o_str(name); o_str("\n");
        o_str("     help\n");
        return;
    }
    o_str(p->page);
    if (p->page[0]) {
        const char *s = p->page;
        while (*s) s++;
        if (s > p->page && s[-1] != '\n') o_nl();
    }
}
