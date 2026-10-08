/*
 * LIVE installer (language, locale, timezone, CMOS clock, account, disk)
 * and the lock-screen chrome (wallpaper + glass card).
 */
#include "setup.h"
#include "arch.h"
#include "i18n.h"
#include "install.h"
#include "hdboot.h"
#include "framebuffer.h"
#include "gui.h"
#include "clock.h"
#include "disk.h"
#include "blockdev.h"
#include "partition.h"
#include "vfs.h"
#include "version.h"

enum { ST_LANG, ST_LOCALE, ST_TZ, ST_CLOCK, ST_USER, ST_DISK, ST_GO, ST_DONE };

typedef struct { const char *id; const char *en; const char *es; int off; } tz_ent;

static const tz_ent ZONES[] = {
    { "UTC",                            "UTC",                 "UTC",                   0 },
    { "Europe/London",                  "London",              "Londres",               0 },
    { "Europe/Madrid",                  "Madrid",              "Madrid",               60 },
    { "Europe/Paris",                   "Paris",               "París",                60 },
    { "Europe/Berlin",                  "Berlin",              "Berlín",               60 },
    { "Atlantic/Canary",                "Canary Islands",      "Islas Canarias",        0 },
    { "America/New_York",               "New York",            "Nueva York",         -300 },
    { "America/Chicago",                "Chicago",             "Chicago",            -360 },
    { "America/Denver",                 "Denver",              "Denver",             -420 },
    { "America/Los_Angeles",            "Los Angeles",         "Los Ángeles",        -480 },
    { "America/Mexico_City",            "Mexico City",         "Ciudad de México",   -360 },
    { "America/Bogota",                 "Bogota",              "Bogotá",             -300 },
    { "America/Argentina/Buenos_Aires", "Buenos Aires",        "Buenos Aires",       -180 },
    { "America/Sao_Paulo",              "Sao Paulo",           "São Paulo",          -180 },
    { "America/Santiago",               "Santiago",            "Santiago",           -240 },
    { "Asia/Tokyo",                     "Tokyo",               "Tokio",               540 },
    { "Asia/Shanghai",                  "Shanghai",            "Shanghái",            480 },
    { "Australia/Sydney",               "Sydney",              "Sídney",              600 },
};
#define NZONES ((int)(sizeof(ZONES) / sizeof(ZONES[0])))

static const char *LOC_EN[] = { "en_US", "en_GB", 0 };
static const char *LOC_ES[] = { "es_ES", "es_MX", "es_AR", "es_CO", 0 };

static char g_lang[8]  = "en";
static char g_locale[16] = "en_US";
static char g_tz[40]   = "UTC";
static int  g_tzoff    = 0;

static void cpy(char *d, const char *s, uint32_t max) {
    uint32_t i = 0;
    if (!d || !max) return;
    if (!s) { d[0] = '\0'; return; }
    for (; s[i] && i + 1 < max; i++) d[i] = s[i];
    d[i] = '\0';
}

static bool eq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

const char *setup_lang(void)     { return g_lang; }
const char *setup_locale(void)   { return g_locale; }
const char *setup_timezone(void) { return g_tz; }
int         setup_tzoff(void)    { return g_tzoff; }

void setup_apply_locale(const char *lang, const char *locale,
                        const char *tz, int tzoff) {
    if (lang && lang[0]) {
        cpy(g_lang, lang, sizeof(g_lang));
        i18n_set_code(g_lang);
        kbd_set_layout(i18n_get() == LANG_ES ? 1 : 0);
    }
    if (locale && locale[0]) cpy(g_locale, locale, sizeof(g_locale));
    if (tz && tz[0])         cpy(g_tz, tz, sizeof(g_tz));
    g_tzoff = tzoff;
    if (g_tz[0] && clock_tz_apply(g_tz) == 0)
        g_tzoff = clock_tz_std_minutes();
    else
        clock_tz_set(tzoff, g_tz);
}

bool setup_should_run(void) {
    if (!disk_present()) return false;
    if (console_serial_has_input()) return false;
    /* A disk that already has a profile is an installed system, not LIVE. */
    uint32_t n = 0;
    if (vfs_read("/etc/profile", &n) || vfs_read("profile", &n)) return false;
    return true;
}

/* ------------------------------ drawing -------------------------------- */
static void paint_bg(void) {
    uint32_t w, h; fb_dims(&w, &h);
#if !ECHOS_CLI_ONLY
    if (!gui_wallpaper_ready()) gui_init();
    if (gui_wallpaper_ready()) gui_background(0, 0, (int)w, (int)h);
    else
#endif
    fb_fill_rect(0, 0, w, h, 0x00101828);
}

static void glass_card(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    fb_shadow_soft(x, y, w, h);
    fb_blend_rect(x, y, w, h, 0x00F4F8FC, 168);
    fb_rect_outline(x, y, w, h, 0x00A8C4DC);
    fb_fill_rect(x, y, w, 36, 0x001B3F7A);
}

static void btn(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                const char *lab, bool sel) {
    fb_aero_button(x, y, w, h, false, sel);
    uint32_t tw = (uint32_t)k_strlen(lab) * FB_GLYPH_W;
    uint32_t tx = x + (w > tw ? (w - tw) / 2 : 4);
    uint32_t ty = y + (h > FB_GLYPH_H ? (h - FB_GLYPH_H) / 2 : 0);
    fb_text_trans(tx, ty, lab, sel ? 0x00FFFFFF : 0x00101828);
}

static bool hit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, int px, int py) {
    return px >= (int)x && py >= (int)y && px < (int)(x + w) && py < (int)(y + h);
}

static void field(uint32_t x, uint32_t y, uint32_t w, const char *text,
                  bool focus, bool secret) {
    fb_fill_rect(x, y, w, 28, 0x00FFFFFF);
    fb_rect_outline(x, y, w, 28, focus ? 0x001B7AC8 : 0x00395E8B);
    char show[64];
    if (secret) {
        uint32_t n = 0;
        while (text[n] && n + 1 < sizeof(show)) { show[n] = '*'; n++; }
        show[n] = '\0';
        text = show;
    }
    fb_text(x + 8, y + 6, text[0] ? text : " ", 0x00202020, 0x00FFFFFF);
}

/* ------------------------------ disks ---------------------------------- */
static int list_disks(blockdev_t **out, int max) {
    int n = 0, tot = blockdev_count();
    for (int i = 0; i < tot && n < max; i++) {
        blockdev_t *d = blockdev_at(i);
        if (!d || part_is_view(d) || is_ram(d)) continue;
        out[n++] = d;
    }
    return n;
}

/* ------------------------------ serial wizard -------------------------- */
static void sputs(const char *s) { console_write(s); }

static void prompt_line(const char *q, char *buf, uint32_t max) {
    sputs(q);
    console_readline(buf, max);
}

static int prompt_choice(const char *q, int nopt) {
    char b[8];
    prompt_line(q, b, sizeof(b));
    if (!b[0]) return 0;
    int v = 0;
    for (const char *p = b; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    if (v < 1 || v > nopt) return 0;
    return v - 1;
}

static bool serial_wizard(char *user, char *pass) {
    sputs("\n  EchOS LIVE installer\n");
    sputs("  1) English\n  2) Español\n  language> ");
    char b[8];
    console_readline(b, sizeof(b));
    i18n_set((b[0] == '2') ? LANG_ES : LANG_EN);
    cpy(g_lang, i18n_code(), sizeof(g_lang));
    kbd_set_layout(i18n_get() == LANG_ES ? 1 : 0);

    const char **locs = i18n_get() == LANG_ES ? LOC_ES : LOC_EN;
    sputs("  locale:\n");
    int nl = 0;
    for (; locs[nl]; nl++) {
        sputs("    "); console_write_dec((uint64_t)(nl + 1));
        sputs(") "); sputs(locs[nl]); sputs("\n");
    }
    int li = prompt_choice("  locale> ", nl);
    cpy(g_locale, locs[li], sizeof(g_locale));

    sputs("  timezone:\n");
    for (int i = 0; i < NZONES; i++) {
        sputs("    "); console_write_dec((uint64_t)(i + 1));
        sputs(") "); sputs(i18n_get() == LANG_ES ? ZONES[i].es : ZONES[i].en);
        sputs("\n");
    }
    int zi = prompt_choice("  timezone> ", NZONES);
    cpy(g_tz, ZONES[zi].id, sizeof(g_tz));
    g_tzoff = ZONES[zi].off;
    clock_tz_set(g_tzoff, g_tz);

    char stamp[24];
    clock_fmt_stamp(stamp, sizeof(stamp));
    sputs("  clock now: "); sputs(stamp); sputs("\n");
    sputs("  new date YYYY-MM-DD (empty=keep)> ");
    char ds[16], ts[16];
    console_readline(ds, sizeof(ds));
    if (ds[0]) {
        sputs("  time HH:MM:SS> ");
        console_readline(ts, sizeof(ts));
        clock_tm t;
        if (clock_parse(ds, ts[0] ? ts : NULL, &t)) clock_set(&t);
    }

    prompt_line("  username> ", user, 33);
    if (!user[0]) cpy(user, "RogexUser", 33);
    for (;;) {
        sputs("  password> ");
        /* reuse console_readline; serial tests type visible passwords */
        console_readline(pass, 49);
        if (!pass[0]) { sputs("  ! empty\n"); continue; }
        char p2[49];
        sputs("  confirm> ");
        console_readline(p2, sizeof(p2));
        if (eq(pass, p2)) break;
        sputs("  ! mismatch\n");
    }

    blockdev_t *disks[8];
    int nd = list_disks(disks, 8);
    if (nd == 0) { sputs("  no disk.\n"); return true; }
    sputs("  disks:\n");
    for (int i = 0; i < nd; i++) {
        sputs("    "); console_write_dec((uint64_t)(i + 1));
        sputs(") "); sputs(disks[i]->name); sputs("\n");
    }
    int di = prompt_choice("  disk (empty=skip install)> ", nd);
    account_create(user, pass, g_lang, g_locale, g_tz, g_tzoff);
    (void)di;
    sputs("  install? type ERASE to write the disk, or Enter to skip\n  > ");
    char conf[16];
    console_readline(conf, sizeof(conf));
    if (eq(conf, "ERASE") && nd) {
        int pick = prompt_choice("  disk number> ", nd);
        if (pick < 0 || pick >= nd) pick = 0;
        char err[80];
        if (!install_to_disk(disks[pick], err, sizeof(err))) {
            sputs("  install failed: "); sputs(err); sputs("\n");
        } else {
            sputs("  installed. Remove USB and reboot.\n");
        }
    }
    return true;
}

/* ------------------------------ GUI wizard ----------------------------- */
static void append_ch(char *buf, uint32_t max, int ch) {
    uint32_t n = (uint32_t)k_strlen(buf);
    if (ch == '\b' || ch == 0x7f) {
        if (n) buf[n - 1] = '\0';
        return;
    }
    if (ch < 32 || ch == 0x7f) return;
    if (n + 1 >= max) return;
    buf[n] = (char)ch;
    buf[n + 1] = '\0';
}

bool setup_run(void) {
    char user[33] = "", pass[49] = "", pass2[49] = "";
    char date[16] = "", time[16] = "";
    int step = ST_LANG, sel = 0, loc_i = 0, tz_i = 2; /* Madrid default-ish */
    int disk_i = 0, focus = 0;
    blockdev_t *disks[8];
    int nd = list_disks(disks, 8);
    char status[96] = "";
    bool installed = false;

    /* Sensible defaults from CMOS so the clock page is already "real". */
    {
        clock_tm now; clock_now(&now);
        date[0] = (char)('0' + (now.year / 1000) % 10);
        date[1] = (char)('0' + (now.year / 100) % 10);
        date[2] = (char)('0' + (now.year / 10) % 10);
        date[3] = (char)('0' + now.year % 10);
        date[4] = '-';
        date[5] = (char)('0' + now.month / 10);
        date[6] = (char)('0' + now.month % 10);
        date[7] = '-';
        date[8] = (char)('0' + now.day / 10);
        date[9] = (char)('0' + now.day % 10);
        date[10] = '\0';
        time[0] = (char)('0' + now.hour / 10);
        time[1] = (char)('0' + now.hour % 10);
        time[2] = ':';
        time[3] = (char)('0' + now.min / 10);
        time[4] = (char)('0' + now.min % 10);
        time[5] = ':';
        time[6] = (char)('0' + now.sec / 10);
        time[7] = (char)('0' + now.sec % 10);
        time[8] = '\0';
    }

    if (!fb_active()) return serial_wizard(user, pass);

#if !ECHOS_CLI_ONLY
    gui_init();
#endif
    fb_cursor_show();

    console_write("\n  EchOS LIVE installer — language / locale / clock / account\n");

    for (;;) {
        uint32_t sw, sh; fb_dims(&sw, &sh);
        paint_bg();
        uint32_t cw = sw > 720 ? 640 : (sw > 80 ? sw - 40 : sw);
        uint32_t card_h = sh > 480 ? 420 : (sh > 80 ? sh - 40 : sh);
        uint32_t cx = (sw - cw) / 2, cy = (sh - card_h) / 2;
        glass_card(cx, cy, cw, card_h);
        fb_text(cx + 16, cy + 10, T(S_INSTALL_TITLE), 0x00FFFFFF, 0x001B3F7A);

        uint32_t bx = cx + 24, by = cy + 52;

        if (step == ST_LANG) {
            fb_text_trans(bx, by, T(S_LANGUAGE), 0x00101828);
            btn(bx, by + 36, 240, 40, "English", sel == 0);
            btn(bx + 260, by + 36, 240, 40, "Espanol", sel == 1);
        } else if (step == ST_LOCALE) {
            fb_text_trans(bx, by, T(S_LOCALE), 0x00101828);
            const char **locs = i18n_get() == LANG_ES ? LOC_ES : LOC_EN;
            for (int i = 0; locs[i]; i++)
                btn(bx, by + 32 + (uint32_t)i * 36, 200, 30, locs[i], loc_i == i);
        } else if (step == ST_TZ) {
            fb_text_trans(bx, by, T(S_TIMEZONE), 0x00101828);
            int top = tz_i > 4 ? tz_i - 4 : 0;
            for (int i = 0; i < 8 && top + i < NZONES; i++) {
                int zi = top + i;
                const char *lab = i18n_get() == LANG_ES ? ZONES[zi].es : ZONES[zi].en;
                btn(bx, by + 28 + (uint32_t)i * 32, 360, 28, lab, tz_i == zi);
            }
        } else if (step == ST_CLOCK) {
            fb_text_trans(bx, by, T(S_CLOCK), 0x00101828);
            fb_text_trans(bx, by + 22, T(S_CLOCK_HINT), 0x00304050);
            char stamp[24];
            clock_fmt_stamp(stamp, sizeof(stamp));
            fb_text_trans(bx, by + 52, stamp, 0x000A5A9A);
            fb_text_trans(bx, by + 80, "YYYY-MM-DD", 0x00404040);
            field(bx, by + 98, 200, date, focus == 0, false);
            fb_text_trans(bx + 220, by + 80, "HH:MM:SS", 0x00404040);
            field(bx + 220, by + 98, 160, time, focus == 1, false);
            btn(bx, by + 150, 220, 32, T(S_KEEP_TIME), focus == 2);
        } else if (step == ST_USER) {
            fb_text_trans(bx, by, T(S_ACCOUNT), 0x00101828);
            fb_text_trans(bx, by + 28, T(S_USERNAME), 0x00404040);
            field(bx, by + 46, 360, user, focus == 0, false);
            fb_text_trans(bx, by + 86, T(S_PASSWORD), 0x00404040);
            field(bx, by + 104, 360, pass, focus == 1, true);
            fb_text_trans(bx, by + 144, T(S_PASS_CONFIRM), 0x00404040);
            field(bx, by + 162, 360, pass2, focus == 2, true);
            if (status[0]) fb_text_trans(bx, by + 200, status, 0x00A03030);
        } else if (step == ST_DISK) {
            fb_text_trans(bx, by, T(S_DISK), 0x00101828);
            if (nd == 0) {
                fb_text_trans(bx, by + 36, T(S_NO_DISK), 0x00A03030);
            } else {
                for (int i = 0; i < nd; i++) {
                    char lab[48];
                    uint32_t kib = disks[i]->sectors / 2;
                    char *d = lab, *e = lab + sizeof(lab);
                    const char *p = disks[i]->name;
                    while (*p && d < e - 1) *d++ = *p++;
                    *d++ = ' ';
                    /* MiB */
                    uint32_t mib = kib / 1024;
                    char tmp[12]; int t = 0, v = (int)mib;
                    if (!v) tmp[t++] = '0';
                    char r[12]; int ri = 0;
                    while (v) { r[ri++] = (char)('0' + v % 10); v /= 10; }
                    while (ri) tmp[t++] = r[--ri];
                    tmp[t] = '\0';
                    p = tmp; while (*p && d < e - 1) *d++ = *p++;
                    p = " MiB"; while (*p && d < e - 1) *d++ = *p++;
                    *d = '\0';
                    btn(bx, by + 32 + (uint32_t)i * 36, 360, 30, lab, disk_i == i);
                }
            }
            uint32_t plen = 0;
            if (!hdboot_payload(&plen))
                fb_text_trans(bx, by + 280, T(S_NO_PAYLOAD), 0x00A06020);
        } else if (step == ST_GO) {
            fb_text_trans(bx, by, T(S_INSTALLING), 0x00101828);
            if (status[0]) fb_text_trans(bx, by + 40, status, 0x00202020);
        } else if (step == ST_DONE) {
            fb_text_trans(bx, by, T(S_DONE), 0x00101828);
            fb_text_trans(bx, by + 36, T(S_REBOOT_HINT), 0x00202020);
            btn(bx, by + 100, 360, 36, T(S_CONTINUE), true);
        }

        if (step != ST_GO && step != ST_DONE) {
            btn(cx + 24, cy + card_h - 52, 140, 32, T(S_BACK), false);
            btn(cx + cw - 164, cy + card_h - 52, 140, 32, T(S_NEXT), true);
            if (step == ST_DISK)
                btn(cx + cw / 2 - 90, cy + card_h - 52, 180, 32, T(S_SKIP), false);
        }
        fb_present();

        int ch = 0, dx = 0, dy = 0, btnst = 0;
        int ev = ui_next_raw(&ch, &dx, &dy, &btnst);
        if (ev == 0) continue;
        if (ev == 2) {
            uint32_t mx, my; fb_cursor_get(&mx, &my);
            if (dx || dy) fb_cursor_moveto((int)mx + dx, (int)my + dy);
            /* click on release-less press */
            if (!(btnst & 1)) continue;
            fb_cursor_get(&mx, &my);
            int px = (int)mx, py = (int)my;
            if (step == ST_LANG) {
                if (hit(bx, by + 36, 240, 40, px, py)) sel = 0;
                if (hit(bx + 260, by + 36, 240, 40, px, py)) sel = 1;
            }
            if (step == ST_LOCALE) {
                const char **locs = i18n_get() == LANG_ES ? LOC_ES : LOC_EN;
                for (int i = 0; locs[i]; i++)
                    if (hit(bx, by + 32 + (uint32_t)i * 36, 200, 30, px, py)) loc_i = i;
            }
            if (step == ST_TZ) {
                int top = tz_i > 4 ? tz_i - 4 : 0;
                for (int i = 0; i < 8 && top + i < NZONES; i++)
                    if (hit(bx, by + 28 + (uint32_t)i * 32, 360, 28, px, py))
                        tz_i = top + i;
            }
            if (step == ST_CLOCK) {
                if (hit(bx, by + 98, 200, 28, px, py)) focus = 0;
                if (hit(bx + 220, by + 98, 160, 28, px, py)) focus = 1;
            }
            if (step == ST_USER) {
                if (hit(bx, by + 46, 360, 28, px, py)) focus = 0;
                if (hit(bx, by + 104, 360, 28, px, py)) focus = 1;
                if (hit(bx, by + 162, 360, 28, px, py)) focus = 2;
            }
            if (step == ST_DISK) {
                for (int i = 0; i < nd; i++)
                    if (hit(bx, by + 32 + (uint32_t)i * 36, 360, 30, px, py))
                        disk_i = i;
            }
            if (step != ST_GO && step != ST_DONE) {
                if (hit(cx + 24, cy + card_h - 52, 140, 32, px, py)) {
                    if (step > ST_LANG) step--;
                    continue;
                }
                if (hit(cx + cw - 164, cy + card_h - 52, 140, 32, px, py))
                    goto do_next;
                if (step == ST_DISK && hit(cx + cw / 2 - 90, cy + card_h - 52, 180, 32, px, py)) {
                    if (!user[0]) cpy(user, "RogexUser", sizeof(user));
                    if (!pass[0]) { cpy(status, T(S_EMPTY_PASS), sizeof(status)); continue; }
                    account_create(user, pass, g_lang, g_locale, g_tz, g_tzoff);
                    return true;
                }
            }
            if (step == ST_DONE && hit(bx, by + 100, 360, 36, px, py))
                return true;
            continue;
        }
        /* keyboard */
        if (ch == KEY_LEFT || ch == KEY_UP) {
            if (step == ST_LANG) sel = 0;
            else if (step == ST_LOCALE && loc_i) loc_i--;
            else if (step == ST_TZ && tz_i) tz_i--;
            else if (step == ST_DISK && disk_i) disk_i--;
            else if (step == ST_CLOCK || step == ST_USER) {
                if (focus) focus--;
            }
            continue;
        }
        if (ch == KEY_RIGHT || ch == KEY_DOWN) {
            if (step == ST_LANG) sel = 1;
            else if (step == ST_LOCALE) loc_i++;
            else if (step == ST_TZ && tz_i + 1 < NZONES) tz_i++;
            else if (step == ST_DISK && disk_i + 1 < nd) disk_i++;
            else if (step == ST_CLOCK && focus < 2) focus++;
            else if (step == ST_USER && focus < 2) focus++;
            continue;
        }
        if (ch == '\t') {
            if (step == ST_CLOCK) focus = (focus + 1) % 3;
            if (step == ST_USER)  focus = (focus + 1) % 3;
            continue;
        }
        if (ch == 27) { /* Esc = back */
            if (step > ST_LANG) step--;
            continue;
        }
        if (ch == '\n' || ch == '\r') goto do_next;

        if (step == ST_CLOCK) {
            if (focus == 0) append_ch(date, sizeof(date), ch);
            else if (focus == 1) append_ch(time, sizeof(time), ch);
        } else if (step == ST_USER) {
            if (focus == 0) append_ch(user, sizeof(user), ch);
            else if (focus == 1) append_ch(pass, sizeof(pass), ch);
            else append_ch(pass2, sizeof(pass2), ch);
        }
        continue;

    do_next:
        if (step == ST_LANG) {
            i18n_set(sel == 1 ? LANG_ES : LANG_EN);
            cpy(g_lang, i18n_code(), sizeof(g_lang));
            kbd_set_layout(i18n_get() == LANG_ES ? 1 : 0);
            loc_i = 0;
            step = ST_LOCALE;
        } else if (step == ST_LOCALE) {
            const char **locs = i18n_get() == LANG_ES ? LOC_ES : LOC_EN;
            int n = 0; while (locs[n]) n++;
            if (loc_i >= n) loc_i = n ? n - 1 : 0;
            cpy(g_locale, locs[loc_i], sizeof(g_locale));
            step = ST_TZ;
        } else if (step == ST_TZ) {
            cpy(g_tz, ZONES[tz_i].id, sizeof(g_tz));
            g_tzoff = ZONES[tz_i].off;
            clock_tz_set(g_tzoff, g_tz);
            step = ST_CLOCK;
            focus = 0;
        } else if (step == ST_CLOCK) {
            clock_tm t;
            if (clock_parse(date, time, &t)) clock_set(&t);
            step = ST_USER;
            focus = 0;
            status[0] = '\0';
        } else if (step == ST_USER) {
            if (!user[0]) cpy(user, "RogexUser", sizeof(user));
            if (!pass[0]) { cpy(status, T(S_EMPTY_PASS), sizeof(status)); continue; }
            if (!eq(pass, pass2)) { cpy(status, T(S_MISMATCH), sizeof(status)); continue; }
            step = ST_DISK;
        } else if (step == ST_DISK) {
            account_create(user, pass, g_lang, g_locale, g_tz, g_tzoff);
            if (nd == 0) return true;
            if (disks[disk_i]->sectors <= PART_FIRST_LBA + 64) {
                cpy(status, T(S_TOO_SMALL), sizeof(status));
                continue;
            }
            step = ST_GO;
            /* fall through and install on next frame? do it now */
            {
                char err[80];
                cpy(status, T(S_INSTALLING), sizeof(status));
                paint_bg();
                glass_card(cx, cy, cw, card_h);
                fb_text(cx + 16, cy + 10, T(S_INSTALL_TITLE), 0x00FFFFFF, 0x001B3F7A);
                fb_text_trans(bx, by, T(S_INSTALLING), 0x00101828);
                fb_present();
                if (!install_to_disk(disks[disk_i], err, sizeof(err)))
                    cpy(status, err, sizeof(status));
                else {
                    installed = true;
                    cpy(status, T(S_DONE), sizeof(status));
                }
                (void)installed;
                step = ST_DONE;
            }
        } else if (step == ST_DONE) {
            return true;
        }
    }
}

/* ------------------------------ lock screen ---------------------------- */
void setup_lock_screen(const char *user, bool (*ok)(const char *pass)) {
    char pass[49];
    pass[0] = '\0';
    if (fb_active()) {
#if !ECHOS_CLI_ONLY
        gui_init();
#endif
        fb_cursor_show();
    }
    for (;;) {
        if (fb_active()) {
            uint32_t w, h; fb_dims(&w, &h);
            paint_bg();
            uint32_t cw = 440, ch = 200;
            if (cw + 40 > w) cw = w > 40 ? w - 40 : w;
            uint32_t cx = (w - cw) / 2, cy = (h - ch) / 2;
            glass_card(cx, cy, cw, ch);
            fb_text(cx + 16, cy + 10, T(S_LOCKED), 0x00FFFFFF, 0x001B3F7A);
            fb_text_trans(cx + 24, cy + 52, T(S_USER), 0x00202830);
            fb_text_trans(cx + 120, cy + 52, user ? user : "", 0x000A5A9A);
            fb_text_trans(cx + 24, cy + 80, T(S_UNLOCK), 0x00304050);
            field(cx + 24, cy + 112, cw - 48, pass, true, true);
            fb_present();
        }

        console_write("\n  ================================================================\n");
        console_write("   EchOS locked  —  ");
        console_write(user ? user : "");
        console_write("\n  ================================================================\n");
        console_write("   password> ");

        /* Collect a password, updating the glass field as we go. */
        uint32_t n = 0;
        pass[0] = '\0';
        for (;;) {
            int raw = 0, dx = 0, dy = 0, btn = 0;
            int sc = console_serial_getc_nb();
            int kind = 0, ch = 0;
            if (sc >= 0) { kind = 1; ch = sc; }
            else {
                kind = ui_next_raw(&raw, &dx, &dy, &btn);
                if (kind == 1) ch = raw;
                else if (kind == 2) {
                    uint32_t mx, my; fb_cursor_get(&mx, &my);
                    fb_cursor_moveto((int)mx + dx, (int)my + dy);
                    continue;
                } else continue;
            }
            if (ch == '\n' || ch == '\r') {
                console_putc('\n');
                break;
            }
            if (ch == 0x7f || ch == '\b') {
                if (n) { n--; pass[n] = '\0'; console_write("\b \b"); }
            } else if (ch >= 32 && ch != 0x7f && n + 1 < sizeof(pass)) {
                pass[n++] = (char)ch;
                pass[n] = '\0';
                console_putc('*');
            }
            if (fb_active()) {
                uint32_t w, h; fb_dims(&w, &h);
                paint_bg();
                uint32_t cw = 440, chh = 200;
                if (cw + 40 > w) cw = w > 40 ? w - 40 : w;
                uint32_t cx = (w - cw) / 2, cy = (h - chh) / 2;
                glass_card(cx, cy, cw, chh);
                fb_text(cx + 16, cy + 10, T(S_LOCKED), 0x00FFFFFF, 0x001B3F7A);
                fb_text_trans(cx + 24, cy + 52, T(S_USER), 0x00202830);
                fb_text_trans(cx + 120, cy + 52, user ? user : "", 0x000A5A9A);
                fb_text_trans(cx + 24, cy + 80, T(S_UNLOCK), 0x00304050);
                field(cx + 24, cy + 112, cw - 48, pass, true, true);
                fb_present();
            }
        }
        if (ok && ok(pass)) {
            console_write("   unlocked.\n");
            return;
        }
        console_write("   ! ");
        console_write(T(S_WRONG));
        console_write("\n");
        pass[0] = '\0';
        for (volatile int i = 0; i < 4000000; i++) arch_relax();
    }
}
