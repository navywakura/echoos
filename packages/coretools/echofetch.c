/*
 * Host-side echofetch — official echOS droplet + host specs.
 * Kernel counterpart: userland/cli/echofetch.c
 * Typeface contract: Liberation Mono.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#endif

static const char *LOGO[] = {
    "\033[36m           \xE2\xA0\x87\xE2\xA1\xA6\033[0m",
    "\033[36m   \xE2\xA0\x87\xE2\xA1\xBB\xE2\xA1\xBB\xE2\xA1\xBD\xE2\xA3\xB4\xE2\xA3\xA2\xE2\xA3\xB0\xE2\xA3\xB4\xE2\xA3\xA0\xE2\xA2\x87\xE2\xA1\xBB\xE2\xA3\xB4\xE2\xA3\xA4\xE2\xA0\x87\xE2\xA0\x87\xE2\xA3\xB4\xE2\xA3\xA0\xE2\xA3\xB2\xE2\xA3\xA2\xE2\xA3\xA0\xE2\xA3\xB4\xE2\xA3\xA2\xE2\xA3\xA0\xE2\xA3\xB4\xE2\xA3\xA0\xE2\xA2\x87\xE2\xA2\x9B\xE2\xA0\x87\xE2\xA0\x87\033[0m",
    0
};

/* Official art as specified (UTF-8 Braille droplet). */
static const char OFFICIAL[] =
"\033[36m"
"           \xE2\xA0\x87\xE2\xA1\xA6\n"
"\033[0m";

/* The exact glyph block from the echOS droplet spec — kept as UTF-8 source. */
static void print_official(void)
{
    fputs("\033[36m", stdout);
    fputs(
        "           ⢰⣇\n"
        "   ⢤⣤⣤⣴⡿⠿⢷⣶⣶⡾⢃⣤⣶⣾⣿⣿⡿⠿⠟⠛⠛⠛⠓⠦⠄⡀\n"
        "        ⠙⣿⠏     ⠘⣿⣿⣿⣿⡿⠟⠋⠁\n"
        "    ⢀⣠⣴⣿⣆    ⢀⣼⣿⣿⡿⠋\n"
        "    ⠉ ⠉⠉⣿⣿⣶⣿⣿⡿⠋\n"
        "          ⡿⢫⣿⣿⣿⠁\n"
        "         ⠈ ⣼⣿⣿⠃\n"
        "           ⣿⣿⡿\n"
        "           ⣿⣿⡇\n"
        "           ⢸⣿⣇\n"
        "            ⢿⣿\n"
        "            ⠈⢻⣧\n"
        "              ⠙⢆\n"
        "                ⠑\n",
        stdout);
    fputs("\033[0m\n", stdout);
}

static void kv(const char *k, const char *v)
{
    printf("  \033[1;33m%-10s\033[0m %s\n", k, v);
}

int main(void)
{
    char host[256];
    print_official();
    printf("  \033[1;36mechOS 2.1 Universal\033[0m\n");
    printf("  \033[2mOne kernel. Four editions. Zero graphics.\033[0m\n\n");
    kv("OS", "echOS 2.1.0-honest (Universal · Heap-0 · CLI)");
    kv("Kernel", "2.0.0  Heap-0  UNIVERSAL");
#ifdef __linux__
    {
        struct utsname u;
        struct sysinfo si;
        char buf[192];
        if (uname(&u) == 0) {
            snprintf(buf, sizeof buf, "%s %s", u.sysname, u.release);
            kv("Hostkern", buf);
        }
        if (sysinfo(&si) == 0) {
            snprintf(buf, sizeof buf, "%lu MiB  (%lu MiB free)",
                     (unsigned long)(si.totalram / 1024 / 1024),
                     (unsigned long)(si.freeram / 1024 / 1024));
            kv("RAM", buf);
            snprintf(buf, sizeof buf, "%ld s", (long)si.uptime);
            kv("Uptime", buf);
        }
    }
#endif
    if (gethostname(host, sizeof host) == 0) kv("Host", host);
    kv("Font", "Liberation Mono");
    kv("Shell", "ROSH  (doas rcctl pfctl epk)");
    kv("Vendor", "Rogex Laboratories / Knights Labs");
    (void)LOGO;
    (void)OFFICIAL;
    putchar('\n');
    return 0;
}
