/*
 * echOS Universal TUI installer.
 * Steps: language -> timezone -> keyboard -> locale -> edition
 *        -> LIVE | disk confirm + install.
 * `echos-install` from the LIVE shell restarts this wizard in-place.
 */
#include "wizard.h"
#include "tui.h"
#include "epk.h"
#include "setup.h"
#include "install.h"
#include "i18n.h"
#include "blockdev.h"
#include "partition.h"
#include "clock.h"
#include "vfs.h"
#include "version.h"
#include "disk.h"

typedef struct { const char *id; const char *en; const char *es; int off; } tz_ent;

static const tz_ent ZONES[] = {
    { "UTC",                   "UTC",            "UTC",             0 },
    { "Europe/London",         "London",         "Londres",         0 },
    { "Europe/Madrid",         "Madrid",         "Madrid",         60 }, /* +DST */
    { "Europe/Paris",          "Paris",          "Paris",          60 },
    { "Atlantic/Canary",       "Canary Islands", "Islas Canarias",  0 },
    { "America/New_York",      "New York",       "Nueva York",   -300 },
    { "America/Mexico_City",   "Mexico City",    "Ciudad de Mexico", -360 },
    { "America/Bogota",        "Bogota",         "Bogota",       -300 },
    { "America/Argentina/Buenos_Aires", "Buenos Aires", "Buenos Aires", -180 },
    { "America/Sao_Paulo",     "Sao Paulo",      "Sao Paulo",    -180 },
    { "Asia/Tokyo",            "Tokyo",          "Tokio",         540 },
};
#define NZONES ((int)(sizeof(ZONES) / sizeof(ZONES[0])))

static const char *LOC_EN[] = { "en_US", "en_GB", 0 };
static const char *LOC_ES[] = { "es_ES", "es_MX", "es_AR", "es_CO", 0 };

enum { ST_LANG, ST_TZ, ST_KBD, ST_LOC, ST_ED, ST_DISK, ST_GO, ST_DONE, ST_LIVE };

static int  g_lang;          /* 0 = en, 1 = es */
static int  g_zi, g_ki, g_li, g_ei, g_di;
static char g_langc[8]  = "en";
static char g_loc[16]   = "en_US";
static char g_tz[40]    = "Europe/Madrid";
static char g_kbd[8]    = "en";
static epk_edition_t g_ed = EPK_ED_MINIMAL;

static const char *S(const char *en, const char *es) { return g_lang ? es : en; }

static void apply_lang(int es) {
    g_lang = es ? 1 : 0;
    g_langc[0] = es ? 'e' : 'e';
    g_langc[1] = es ? 's' : 'n';
    g_langc[2] = '\0';
    i18n_set(es ? LANG_ES : LANG_EN);
    kbd_set_layout(es ? 1 : 0);
    g_kbd[0] = es ? 'e' : 'e';
    g_kbd[1] = es ? 's' : 'n';
    g_kbd[2] = '\0';
    g_ki = es ? 1 : 0;
}

static int menu(const char *title, const char *hint,
                const char **items, int n, int *sel) {
    int i;
    tui_begin();
    tui_box(2, 1, 76, 22, title);
    tui_text(5, 3, S("JetBrains Mono  ·  Heap-0 shared kernel  ·  local epk",
                     "JetBrains Mono  ·  kernel Heap-0 compartido  ·  epk local"),
             TUI_DIM, TUI_NAVY);
    for (i = 0; i < n && i < 14; i++)
        tui_item(6, 6 + i, 68, items[i], i == *sel);
    tui_hint(21, hint);
    tui_end();
    for (;;) {
        tui_key k = tui_read_key();
        if (k == TUI_KEY_UP)   { if (*sel > 0) (*sel)--; else *sel = n - 1; break; }
        if (k == TUI_KEY_DOWN) { if (*sel + 1 < n) (*sel)++; else *sel = 0; break; }
        if (k == TUI_KEY_ENTER) return 1;
        if (k == TUI_KEY_ESC || k == TUI_KEY_Q) return -1;
        if (k == TUI_KEY_CHAR) {
            char c = tui_last_char();
            if (c >= '1' && c < '1' + n) { *sel = c - '1'; return 1; }
        }
        if (k == TUI_KEY_UP || k == TUI_KEY_DOWN) break;
        return 0;
    }
    return 0;
}

/* menu() redraws on every arrow by returning 0 so the caller loops. */

static int pick(const char *title, const char *hint,
                const char **items, int n, int *sel) {
    int r;
    if (*sel < 0 || *sel >= n) *sel = 0;
    for (;;) {
        r = menu(title, hint, items, n, sel);
        if (r == 1) return 1;
        if (r < 0) return -1;
    }
}

const char *wizard_lang(void)     { return g_langc; }
const char *wizard_locale(void)   { return g_loc; }
const char *wizard_timezone(void) { return g_tz; }
const char *wizard_kbd(void)      { return g_kbd; }
epk_edition_t wizard_edition(void){ return g_ed; }

static int list_disks(blockdev_t **out, int max) {
    int i, n = 0, tot = blockdev_count();
    for (i = 0; i < tot && n < max; i++) {
        blockdev_t *d = blockdev_at(i);
        if (!d || part_is_view(d) || is_ram(d)) continue;
        out[n++] = d;
    }
    return n;
}

static void apply_zone(void) {
    if (clock_tz_apply(g_tz) != 0)
        clock_tz_set(ZONES[g_zi].off, g_tz);
    vfs_write("/etc/timezone", g_tz, (uint32_t)k_strlen(g_tz));
}

static wizard_result_t commit_live(void) {
    apply_zone();
    setup_apply_locale(g_langc, g_loc, g_tz, clock_tz_std_minutes());
    epk_live_all();
    vfs_write("/etc/echos-live", "1", 1);
    account_create("live", "", g_langc, g_loc, g_tz, clock_tz_std_minutes());
    return WIZARD_LIVE;
}

static wizard_result_t commit_install(blockdev_t *d) {
    char err[80];
    apply_zone();
    setup_apply_locale(g_langc, g_loc, g_tz, clock_tz_std_minutes());
    if (!epk_apply_edition(g_ed, err, sizeof(err))) {
        tui_begin();
        tui_box(2, 1, 76, 10, "epk");
        tui_text(5, 4, err[0] ? err : "apply failed", TUI_RED, TUI_NAVY);
        tui_end();
        tui_read_key();
        return WIZARD_CANCEL;
    }
    tui_begin();
    tui_box(2, 1, 76, 10, S("Installing", "Instalando"));
    tui_text(5, 4, S("Writing MBR, bootloader, kernel, RXFS...",
                     "Escribiendo MBR, bootloader, kernel, RXFS..."),
             TUI_FG, TUI_NAVY);
    tui_end();
    if (!install_to_disk(d, err, sizeof(err))) {
        tui_begin();
        tui_box(2, 1, 76, 10, S("Install failed", "Fallo la instalacion"));
        tui_text(5, 4, err[0] ? err : "unknown", TUI_RED, TUI_NAVY);
        tui_end();
        tui_read_key();
        return WIZARD_CANCEL;
    }
    account_create("echos", "", g_langc, g_loc, g_tz, ZONES[g_zi].off);
    tui_begin();
    tui_box(2, 1, 76, 12, S("Installation complete", "Instalacion terminada"));
    tui_text(5, 4, S("Remove the USB and reboot. EchOS will start from this disk.",
                     "Retira el USB y reinicia. EchOS arrancara desde este disco."),
             TUI_ACCENT, TUI_NAVY);
    tui_text(5, 6, S("Edition:", "Edicion:"), TUI_DIM, TUI_NAVY);
    tui_text(16, 6, epk_edition_name(g_ed), TUI_GOLD, TUI_NAVY);
    tui_hint(10, S("Enter to continue on LIVE  |  this session stays in RAM",
                   "Enter para seguir en LIVE  |  esta sesion sigue en RAM"));
    tui_end();
    tui_read_key();
    return WIZARD_INSTALLED;
}

wizard_result_t wizard_run(void) {
    int step = ST_LANG;
    g_zi = 2; /* Madrid as a reasonable default for ES */
    g_ki = 0;
    g_li = 0;
    g_ei = 0;
    g_di = 0;
    apply_lang(0);

    while (step != ST_DONE && step != ST_LIVE) {
        if (step == ST_LANG) {
            const char *it[] = { "English", "Español" };
            int r = pick(ECHOS_VERSION_NAME,
                         S("Enter confirm  |  1-2 jump  |  q LIVE (Minimal)",
                           "Enter confirma  |  1-2 salta  |  q LIVE (Minimal)"),
                         it, 2, &g_lang);
            if (r < 0) { commit_live(); step = ST_LIVE; continue; }
            apply_lang(g_lang);
            g_zi = g_lang ? 2 : 0;
            step = ST_TZ;
        } else if (step == ST_TZ) {
            const char *it[NZONES];
            int i, r;
            for (i = 0; i < NZONES; i++) it[i] = g_lang ? ZONES[i].es : ZONES[i].en;
            r = pick(S("Timezone", "Zona horaria"),
                     S("Enter next  |  Esc back", "Enter sigue  |  Esc atras"),
                     it, NZONES, &g_zi);
            if (r < 0) { step = ST_LANG; continue; }
            { const char *p = ZONES[g_zi].id; int n = 0;
              while (*p && n + 1 < (int)sizeof(g_tz)) g_tz[n++] = *p++;
              g_tz[n] = '\0'; }
            clock_tz_set(ZONES[g_zi].off, g_tz);
            vfs_write("/etc/timezone", g_tz, (uint32_t)k_strlen(g_tz));
            step = ST_KBD;
        } else if (step == ST_KBD) {
            const char *it[] = { "en  (US QWERTY)", "es  (Spanish)" };
            int r = pick(S("Keyboard", "Teclado"),
                         S("Enter next  |  Esc back", "Enter sigue  |  Esc atras"),
                         it, 2, &g_ki);
            if (r < 0) { step = ST_TZ; continue; }
            g_kbd[0] = 'e'; g_kbd[1] = g_ki ? 's' : 'n'; g_kbd[2] = '\0';
            kbd_set_layout(g_ki ? 1 : 0);
            step = ST_LOC;
        } else if (step == ST_LOC) {
            const char **locs = g_lang ? LOC_ES : LOC_EN;
            const char *it[8];
            int n = 0, r;
            while (locs[n] && n < 8) { it[n] = locs[n]; n++; }
            if (g_li >= n) g_li = 0;
            r = pick(S("Locale", "Configuracion regional"),
                     S("Enter next  |  Esc back", "Enter sigue  |  Esc atras"),
                     it, n, &g_li);
            if (r < 0) { step = ST_KBD; continue; }
            { const char *p = locs[g_li]; int k = 0;
              while (*p && k + 1 < (int)sizeof(g_loc)) g_loc[k++] = *p++;
              g_loc[k] = '\0'; }
            step = ST_ED;
        } else if (step == ST_ED) {
            const char *it_en[] = {
                "1. Minimal   kernel + shell + Heap-0 + epk",
                "2. Complete  extended CLI + compilers + diagnostics",
                "3. Edge      Akida AKD1000 SNN runtime + static I/O",
                "4. Server    net stack + httpd/ftpd/sshd + ROGEX-PQC",
                "5. LIVE Mode Minimal in RAM (no disk write)",
            };
            const char *it_es[] = {
                "1. Minimal   kernel + shell + Heap-0 + epk",
                "2. Complete  CLI extendida + compiladores + diagnostico",
                "3. Edge      runtime NPU Akida AKD1000 + I/O estatico",
                "4. Server    red + httpd/ftpd/sshd + tunel ROGEX-PQC",
                "5. Modo LIVE Minimal en RAM (sin escribir disco)",
            };
            int r = pick(S("Edition", "Edicion a instalar"),
                         S("Enter next  |  Esc back  |  5 = LIVE",
                           "Enter sigue  |  Esc atras  |  5 = LIVE"),
                         g_lang ? it_es : it_en, 5, &g_ei);
            if (r < 0) { step = ST_LOC; continue; }
            if (g_ei == 4) { commit_live(); step = ST_LIVE; continue; }
            g_ed = (epk_edition_t)g_ei;
            step = ST_DISK;
        } else if (step == ST_DISK) {
            blockdev_t *ds[8];
            const char *it[8];
            static char labels[8][48];
            int n = list_disks(ds, 8), i, r;
            if (n == 0) {
                const char *msg[] = {
                    S("No persistent disk (AHCI / virtio / IDE).",
                      "No hay disco persistente (AHCI / virtio / IDE)."),
                    S("Continue in LIVE Mode", "Continuar en modo LIVE"),
                };
                int dummy = 1;
                pick(S("No disk", "Sin disco"),
                     S("Enter LIVE", "Enter LIVE"), msg, 2, &dummy);
                commit_live();
                step = ST_LIVE;
                continue;
            }
            for (i = 0; i < n; i++) {
                char *d = labels[i];
                const char *p = ds[i]->name;
                const char *kind = "disk";
                uint64_t mib = (uint64_t)ds[i]->sectors / 2 / 1024;
                int k = 0;
                const char *be = disk_backend();
                if (be && be[0] == 'v') kind = "virtio";
                else if (be && (be[0] == 'A' || be[0] == 'a')) kind = "AHCI-SSD/HDD";
                else if (be && (be[0] == 'I' || be[0] == 'i')) kind = "IDE-HDD";
                while (*p && k < 12) d[k++] = *p++;
                d[k++] = ' ';
                { char t[12]; int ti = 0; uint64_t v = mib;
                  if (!v) t[ti++] = '0';
                  while (v && ti < 10) { t[ti++] = (char)('0' + v % 10); v /= 10; }
                  while (ti && k < 20) d[k++] = t[--ti]; }
                d[k++] = 'M'; d[k++] = 'i'; d[k++] = 'B'; d[k++] = ' ';
                p = kind; while (*p && k < 46) d[k++] = *p++;
                d[k] = '\0';
                it[i] = labels[i];
            }
            if (g_di >= n) g_di = 0;
            r = pick(S("Install disk  (ERASES the whole disk)",
                       "Disco  (BORRA el disco entero)"),
                     S("Enter to confirm phrase  |  Esc back",
                       "Enter para la frase de confirmacion  |  Esc atras"),
                     it, n, &g_di);
            if (r < 0) { step = ST_ED; continue; }
            {
                blockdev_t *d = ds[g_di];
                const char *yes[] = {
                    S("Cancel", "Cancelar"),
                    S("ERASE this disk and install echOS " ECHOS_VERSION,
                      "BORRAR este disco e instalar echOS " ECHOS_VERSION),
                };
                int yn = 0;
                char title[64];
                int k = 0;
                const char *p = S("Confirm erase of ", "Confirmar borrado de ");
                while (*p && k < 40) title[k++] = *p++;
                p = d->name; while (*p && k < 60) title[k++] = *p++;
                title[k] = '\0';
                r = pick(title,
                         S("Select the second line to write the disk",
                           "Elige la segunda linea para escribir el disco"),
                         yes, 2, &yn);
                if (r < 0 || yn == 0) { step = ST_DISK; continue; }
                return commit_install(d);
            }
        } else {
            break;
        }
    }
    if (step == ST_LIVE) return WIZARD_LIVE;
    return WIZARD_CANCEL;
}

wizard_result_t echos_install(void) {
    return wizard_run();
}
