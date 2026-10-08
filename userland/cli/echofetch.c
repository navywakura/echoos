/*
 * echofetch — echOS system card (neofetch/fastfetch replacement).
 * Official logo is the Rogex droplet in Unicode Braille; the framebuffer
 * The console face now carries Braille (synthesised from the codepoint), so
 * the droplet draws as dots rather than as an ASCII stand-in.
 */
#include "echofetch.h"
#include "tui.h"
#include "bootmode.h"
#include "version.h"
#include "heap0.h"
#include "vfs.h"
#include "arch.h"
#include "power.h"
#include "framebuffer.h"
#include "echofetch_logo.h"

static char  *o_buf;
static size_t o_max, o_len;

static void o_reset(char *buf, size_t max) {
    o_buf = buf; o_max = max; o_len = 0;
    if (max) buf[0] = '\0';
}
static void o_s(const char *s) {
    if (!s || !o_buf) return;
    while (*s && o_len + 1 < o_max) o_buf[o_len++] = *s++;
    if (o_max) o_buf[o_len] = '\0';
}
static void o_nl(void) { o_s("\n"); }

static char g_ed[32];
static const char *runtime_edition(void) {
    uint32_t n = 0, i = 0;
    const char *s = vfs_read("/etc/echos-edition", &n);
    if (!s || !n) return ECHOS_EDITION;
    while (i < n && i + 1 < sizeof(g_ed) && s[i] != '\n' && s[i] != '\r') {
        g_ed[i] = s[i];
        i++;
    }
    g_ed[i] = '\0';
    return g_ed[0] ? g_ed : ECHOS_EDITION;
}

/* Console is 80 cells. Logo + gap + spec must stay on one row or the
   remainder wraps under the droplet and shears it. */
#define FETCH_COLS 80

static int disp_w(const char *s) {
    int w = 0;
    while (s && *s) {
        unsigned char c = (unsigned char)*s;
        if (c == 0x1b) {                 /* skip CSI so pad is visual */
            s++;
            if (*s == '[') {
                s++;
                while (*s && !(*s >= 0x40 && *s <= 0x7e)) s++;
                if (*s) s++;
            }
            continue;
        }
        if (c < 0x80) { w++; s++; }
        else if ((c & 0xE0) == 0xC0) { w++; s += 2; }
        else if ((c & 0xF0) == 0xE0) { w++; s += 3; }
        else if ((c & 0xF8) == 0xF0) { w++; s += 4; }
        else { w++; s++; }
    }
    return w;
}

static void spec_line(char *d, int max, const char *k, const char *v, int vmax) {
    int n = 0, vis = 0, kw = 0, room;
    const char *p = "  \033[1;33m";
    while (*p && n + 1 < max) d[n++] = *p++;
    vis += 2;
    p = k; while (*p && n + 1 < max) { d[n++] = *p++; kw++; vis++; }
    p = "\033[0m  ";
    while (*p && n + 1 < max) d[n++] = *p++;
    vis += 2;
    while (kw < 8 && n + 1 < max) { d[n++] = ' '; kw++; vis++; }
    room = vmax - vis;
    if (room < 1) room = 1;
    p = v;
    {
        int got = 0;
        while (p && *p && n + 1 < max && got < room) { d[n++] = *p++; got++; }
        if (p && *p && got >= 3) {
            d[n - 3] = '.'; d[n - 2] = '.'; d[n - 1] = '.';
        }
    }
    d[n] = '\0';
}

static void title_line(char *d, int max, const char *ansi, const char *t, int vmax) {
    int n = 0, vis = 2, room;
    const char *p = ansi;
    while (*p && n + 1 < max) d[n++] = *p++;
    d[n++] = ' '; d[n++] = ' ';
    room = vmax - vis;
    if (room < 1) room = 1;
    p = t;
    {
        int got = 0;
        while (p && *p && n + 1 < max && got < room) { d[n++] = *p++; got++; }
        if (p && *p && got >= 3) {
            d[n - 3] = '.'; d[n - 2] = '.'; d[n - 1] = '.';
        }
    }
    p = "\033[0m";
    while (*p && n + 1 < max) d[n++] = *p++;
    d[n] = '\0';
}

void echofetch_render(char *out, size_t outmax) {
    char spec[16][120];
    char extra[64];
    int nspec = 0, nlogo = 0, rows, i, pad, logo_w = 0, spec_max;
    uint64_t ms = arch_timer_uptime_ms();
    uint64_t ram = pmm_total_bytes();
    const char *ed = runtime_edition();

    o_reset(out, outmax);
    for (nlogo = 0; ECHOS_LOGO_OFFICIAL[nlogo]; nlogo++) {
        int w = disp_w(ECHOS_LOGO_OFFICIAL[nlogo]);
        if (w > logo_w) logo_w = w;
    }
    pad = logo_w + 2;
    spec_max = FETCH_COLS - pad;
    if (spec_max < 12) spec_max = 12;

    title_line(spec[nspec++], 120, "\033[1;36m", ECHOS_VERSION_NAME, spec_max);
    title_line(spec[nspec++], 120, "\033[2m", ECHOS_SLOGAN, spec_max);
    spec[nspec][0] = '\0'; nspec++;

    spec_line(spec[nspec++], 120, "OS", ECHOS_PRETTY_NAME, spec_max);
    { char v[80]; int n = 0; const char *p = ECHOS_VERSION;
      while (*p) v[n++] = *p++;
      p = "  Heap-0  "; while (*p) v[n++] = *p++;
      p = ECHOS_CODENAME; while (*p) v[n++] = *p++;
      v[n] = 0;
      spec_line(spec[nspec++], 120, "Kernel", v, spec_max); }
    { char v[80]; int n = 0; const char *p = ed;
      while (*p) v[n++] = *p++;
      p = "  (shared kernel "; while (*p) v[n++] = *p++;
      p = ECHOS_SHARED_KERNEL ? "yes)" : "no)"; while (*p) v[n++] = *p++;
      v[n] = 0;
      spec_line(spec[nspec++], 120, "Edition", v, spec_max); }
    spec_line(spec[nspec++], 120, "Host", "echos", spec_max);
    spec_line(spec[nspec++], 120, "CPU", power_cpu_name(), spec_max);
    { char v[64]; int n = 0;
      extra[0] = 0;
      { uint64_t x = ram / (1024 * 1024); char t[16]; int ti = 0;
        if (!x) t[ti++] = '0'; while (x) { t[ti++] = (char)('0' + x % 10); x /= 10; }
        while (ti) v[n++] = t[--ti]; }
      { const char *p = " MiB  ("; while (*p) v[n++] = *p++; }
      { uint64_t x = pmm_free_bytes() / (1024 * 1024); char t[16]; int ti = 0;
        if (!x) t[ti++] = '0'; while (x) { t[ti++] = (char)('0' + x % 10); x /= 10; }
        while (ti) v[n++] = t[--ti]; }
      { const char *p = " MiB free)"; while (*p) v[n++] = *p++; }
      v[n] = 0;
      spec_line(spec[nspec++], 120, "RAM", v, spec_max); }
    { char v[32]; int n = 0; uint64_t x = ms / 1000; char t[16]; int ti = 0;
      if (!x) t[ti++] = '0'; while (x) { t[ti++] = (char)('0' + x % 10); x /= 10; }
      while (ti) v[n++] = t[--ti];
      v[n++] = ' '; v[n++] = 's'; v[n] = 0;
      spec_line(spec[nspec++], 120, "Uptime", v, spec_max); }
    { char v[48]; int n = 0; uint64_t x = heap0_total_bytes() / 1024; char t[16]; int ti = 0;
      if (!x) t[ti++] = '0'; while (x) { t[ti++] = (char)('0' + x % 10); x /= 10; }
      while (ti) v[n++] = t[--ti];
      { const char *p = " KiB static  0.00% frag"; while (*p) v[n++] = *p++; }
      v[n] = 0;
      spec_line(spec[nspec++], 120, "Heap-0", v, spec_max); }
    spec_line(spec[nspec++], 120, "Font", ECHOS_FONT, spec_max);
    spec_line(spec[nspec++], 120, "Shell", "ROSH  (doas rcctl pfctl epk)", spec_max);
    spec_line(spec[nspec++], 120, "Boot", bootmode_name(), spec_max);
    spec_line(spec[nspec++], 120, "Vendor", ECHOS_VENDOR, spec_max);

    rows = nlogo > nspec ? nlogo : nspec;
    o_s("\n");
    for (i = 0; i < rows; i++) {
        int w = 0, s;
        o_s("\033[1;36m");
        if (i < nlogo) {
            o_s(ECHOS_LOGO_OFFICIAL[i]);
            w = disp_w(ECHOS_LOGO_OFFICIAL[i]);
        }
        o_s("\033[0m");
        for (s = w; s < pad; s++) o_s(" ");
        if (i < nspec) o_s(spec[i]);
        o_nl();
    }
    o_nl();
    (void)extra;
}

void echofetch_banner(void) {
    char buf[4096];
    echofetch_render(buf, sizeof(buf));
    console_write(buf);
}
