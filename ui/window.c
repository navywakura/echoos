/*
 * Window manager + desktop integration. Hybrid tiling, drag-to-float.
 * Rough edges remain — PRs welcome.
 */
#include "window.h"
#include "arch.h"
#include "framebuffer.h"
#include "mouse.h"
#include "vfs.h"
#include "heap.h"
#include "pmm.h"
#include "arch.h"
#include "clock.h"
#include "netif.h"
#include "rx_event.h"
#include "commands.h"      /* the terminal window runs the same shell commands */
#include "rosh.h"          /* line editing, history, completion               */
#include "env.h"
#include "gui.h"           /* wallpaper + the scene cache that makes drag cheap */
#include "calc.h"
#include "power.h"
#include "rx_backend.h"
#include "disks.h"
#include "wallpaper_pack.h"
#include "imgdec.h"
#include "www.h"
#include "browse.h"        /* Nova browser: page fetch + last-page text */
#include "dock.h"
#include "etk.h"          /* Eclipse Shell: macOS-style dock */
#include "../kernel/version.h"  /* edition capability gates */
#include "icon_pack.h"
#include "screen_capture.h"

/* ------------------------------ tiny helpers ----------------------------- */
static int uitoa(uint64_t v, char *out) {         /* -> length */
    char t[24]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = '0' + (int)(v % 10); v /= 10; }
    int n = i, j = 0;
    while (i) out[j++] = t[--i];
    out[j] = '\0';
    return n;
}
static char *sapp(char *d, const char *s, char *e) { while (*s && d < e - 1) *d++ = *s++; *d = '\0'; return d; }
/* Does this command line start with `name` as a whole word? Arguments are
   allowed to follow; `neurology` must not match `neuro`. */
static bool cmd_is(const char *line, const char *name) {
    int i = 0;
    for (; name[i]; i++) if (line[i] != name[i]) return false;
    return line[i] == '\0' || line[i] == ' ';
}
static char *napp(char *d, uint64_t v, char *e) { char b[24]; uitoa(v, b); return sapp(d, b, e); }

/* ------------------------------- WM state -------------------------------- */
static wm_window g_win[WM_MAX_WINDOWS];
static int  g_focus = -1;          /* focused window id, or -1                */
static int  g_ws;                  /* current workspace                       */
static bool g_dirty;
static int  g_next_id = 1;
static int  g_screen_w, g_screen_h;

/* drag state */
static int  g_drag = -1;           /* window id being dragged, or -1          */

/* Forward declarations */
static void draw_context_menu(void);
static void term_end_select(wm_window *w);
static void win_maximize_toggle(wm_window *w);
static bool g_prev_left;

/* Global clipboard (Ctrl+C / Ctrl+V). Shared across windows so paste works
   after you focus another app — per-window clipboards felt broken. */
static char g_clip[512];
static bool g_clip_has;
static uint64_t g_last_title_click_ms;
static int      g_last_title_click_id = -1;

static void clip_append_line(const char *s) {
    int n = 0; while (g_clip[n]) n++;
    if (n && n < (int)sizeof(g_clip) - 1) g_clip[n++] = '\n';
    for (; *s && n < (int)sizeof(g_clip) - 1; s++) g_clip[n++] = *s;
    g_clip[n] = '\0';
    g_clip_has = (n > 0);
}

/* status-bar workspace tag hitboxes (filled by draw_bar) */
static int  g_tag_x[WM_WORKSPACES], g_tag_w[WM_WORKSPACES];

/* Set when the desktop asks the shell to leave the machine. */
static wm_action g_exit;
static bool      g_started;

bool      wm_started(void)     { return g_started; }
wm_action wm_last_action(void) { return g_exit; }

static const char *WS_NAME[WM_WORKSPACES] = { "1:Lab", "2:Sys", "3:Net" };

static wm_window *win_by_id(int id) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (g_win[i].used && g_win[i].id == id) return &g_win[i];
    return NULL;
}

/* ------------------------------ layout engine ---------------------------- */
/* Master/stack: first tiled window on this workspace takes the left ~55%,
   the rest stack vertically on the right. Floating windows keep their own
   geometry and are excluded here. A uniform WM_GAP separates everything. */
static void retile(void) {
    int idx[WM_MAX_WINDOWS], n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_win[i].used && g_win[i].ws == g_ws && !g_win[i].floating && !g_win[i].maximized)
            idx[n++] = i;
    if (n == 0) return;

    int ax = WM_GAP, ay = WM_BAR_H + WM_GAP;
    int aw = g_screen_w - 2 * WM_GAP;
    int ah = g_screen_h - WM_BAR_H - WM_TASK_H - 2 * WM_GAP;

    if (n == 1) {
        wm_window *w = &g_win[idx[0]];
        w->x = ax; w->y = ay; w->w = aw; w->h = ah;
        return;
    }
    int mw = (aw - WM_GAP) * 55 / 100;              /* master width          */
    g_win[idx[0]].x = ax; g_win[idx[0]].y = ay; g_win[idx[0]].w = mw; g_win[idx[0]].h = ah;

    int sx = ax + mw + WM_GAP;
    int sw = aw - mw - WM_GAP;
    int sn = n - 1;
    int sh = (ah - (sn - 1) * WM_GAP) / sn;
    for (int k = 0; k < sn; k++) {
        wm_window *w = &g_win[idx[k + 1]];
        w->x = sx; w->y = ay + k * (sh + WM_GAP); w->w = sw; w->h = sh;
    }
}

/* =============================== terminal ================================
   ROSH inside the WM. One window, up to two Terminator-style panes, each
   with its own scrollback, scroll offset and line editor. */
#define TERM_LINES 160
#define TERM_COLS  160
#define TERM_PANES 2
#define TERM_SASH  5

typedef struct {
    char tl[TERM_LINES][TERM_COLS];
    int  n;                        /* lines stored                         */
    int  view;                     /* 0 = follow tail; >0 scrolled up      */
} term_pane;

static term_pane g_pane[TERM_PANES];
static int  g_npane = 1;           /* 1 or 2                               */
static int  g_fpane;               /* focused pane 0..g_npane-1            */
static int  g_split;               /* 0 none  1 vertical  2 horizontal     */
static int  g_term_theme;          /* 0 night 1 matrix 2 ocean 3 amber 4 paper */
static int  g_term_win = -1;
static bool g_term_halt;
static bool g_term_want_neuro;

/* Compatibility aliases so selection / copy keep compiling. */
#define g_tl   (g_pane[g_fpane].tl)
#define g_tl_n (g_pane[g_fpane].n)

typedef struct {
    uint32_t bg, fg, dim, prompt, path, cmd, err, ok, warn, acc, sel, bar, caret, sash;
} term_pal;

static const term_pal TERM_PAL[] = {
    /* night  */ {0x000A0C10,0x00E6E6E4,0x00686870,0x0055CDFF,0x0055FF88,0x00FFFFFF,0x00FF5555,0x0055FF88,0x00FFD866,0x00C792EA,0x003C6EA5,0x0014181C,0x00FFB020,0x0030A0C0},
    /* matrix */ {0x00020800,0x0033FF66,0x00106030,0x0066FF99,0x00A0FF70,0x00C8FFC0,0x00FF4444,0x0033FF66,0x00C8FF40,0x0020C060,0x00104020,0x00041000,0x00A0FF40,0x00208040},
    /* ocean  */ {0x00081422,0x00D0E8FF,0x00507088,0x0040C8FF,0x0040E0C0,0x00F0F6FF,0x00FF6B6B,0x0040E0A0,0x00FFC14D,0x0080A0FF,0x00205080,0x000C1828,0x0040C8FF,0x003080C0},
    /* amber  */ {0x00120800,0x00FFB020,0x00806020,0x00FFD060,0x00FFC040,0x00FFE8A0,0x00FF6060,0x00C0FF40,0x00FFA000,0x00FFCC66,0x00604010,0x00181000,0x00FFE080,0x00C08020},
    /* paper  */ {0x00F0EEE6,0x0011110F,0x00707068,0x000A5A8C,0x001A6B3A,0x00000000,0x00C43C24,0x001A6B3A,0x00B58900,0x007A3D8C,0x00C8D4E8,0x00E4E0D4,0x00C43C24,0x00808080},
};
#define TERM_THEME_N 5
static const char *const TERM_THEME_NAME[] = { "night", "matrix", "ocean", "amber", "paper" };

static const term_pal *term_palette(void) {
    int i = g_term_theme;
    if (i < 0 || i >= TERM_THEME_N) i = 0;
    return &TERM_PAL[i];
}

static int term_theme_by_name(const char *s) {
    int i;
    if (!s || !s[0]) return -1;
    for (i = 0; i < TERM_THEME_N; i++) {
        const char *n = TERM_THEME_NAME[i];
        int k = 0;
        while (n[k] && s[k] && n[k] == s[k]) k++;
        if (n[k] == 0 && (s[k] == 0 || s[k] == ' ')) return i;
    }
    return -1;
}

static void pane_clear(term_pane *p) {
    int i, c;
    p->n = 0;
    p->view = 0;
    for (i = 0; i < TERM_LINES; i++)
        for (c = 0; c < TERM_COLS; c++) p->tl[i][c] = 0;
}

static void pane_line(term_pane *p, const char *s) {
    int c = 0;
    if (p->n == TERM_LINES) {
        int i;
        for (i = 1; i < TERM_LINES; i++)
            for (c = 0; c < TERM_COLS; c++) p->tl[i - 1][c] = p->tl[i][c];
        p->n--;
    }
    c = 0;
    while (s[c] && c < TERM_COLS - 1) { p->tl[p->n][c] = s[c]; c++; }
    p->tl[p->n][c] = '\0';
    p->n++;
    if (p->view) {
        /* stay parked unless we were following the tail */
    } else {
        p->view = 0;
    }
}

static void term_line(const char *s) {
    pane_line(&g_pane[g_fpane], s);
    g_pane[g_fpane].view = 0;      /* a new line snaps the active pane down  */
}

/* Append a block of shell output, splitting on newlines and hard-wrapping
   anything longer than the buffer so nothing is silently dropped. */
static void term_write(const char *s) {
    char buf[TERM_COLS];
    int n = 0;
    for (; *s; s++) {
        if (*s == '\n') { buf[n] = '\0'; term_line(buf); n = 0; continue; }
        if (*s == '\r') continue;
        if (n >= TERM_COLS - 1) { buf[n] = '\0'; term_line(buf); n = 0; }
        buf[n++] = *s;
    }
    if (n) { buf[n] = '\0'; term_line(buf); }
}

static void term_banner(term_pane *p) {
    char stamp[24], hello[96];
    char *d, *e;
    clock_fmt_stamp(stamp, sizeof(stamp));
    d = hello; e = hello + sizeof(hello);
    d = sapp(d, "EchOS 1.0  ROSH  ", e); sapp(d, stamp, e);
    pane_line(p, hello);
    pane_line(p, "PgUp/PgDn scroll   Ctrl+Up/Down   Up/Down history   Tab completes");
    pane_line(p, "Ctrl+E vsplit   Ctrl+O hsplit   Ctrl+W unsplit   Ctrl+N next pane");
    pane_line(p, "`termtheme` night|matrix|ocean|amber|paper   `split` / `unsplit`");
    pane_line(p, "`help` lists commands.  Esc returns the keyboard to the desktop.");
    pane_line(p, " ");
}

static void term_reset(void) {
    int i;
    g_term_halt = false;
    g_npane = 1;
    g_fpane = 0;
    g_split = 0;
    {
        const char *tn = env_get("TERM_THEME");
        int id = term_theme_by_name(tn);
        if (id >= 0) g_term_theme = id;
    }
    rosh_init();
    for (i = 0; i < TERM_PANES; i++) pane_clear(&g_pane[i]);
    term_banner(&g_pane[0]);
}

static void term_do_split(int mode) {     /* 1 = vertical, 2 = horizontal   */
    if (mode != 1 && mode != 2) return;
    g_split = mode;
    g_npane = 2;
    if (g_pane[1].n == 0) {
        rosh_init_session(1);
        pane_line(&g_pane[1], "pane 2  ·  ROSH");
        pane_line(&g_pane[1], "Ctrl+N switches panes.  Type `unsplit` to close.");
        pane_line(&g_pane[1], " ");
    }
}

static void term_unsplit(void) {
    if (g_npane < 2) return;
    if (g_fpane == 1) {
        g_pane[0] = g_pane[1];
        rosh_copy(1, 0);
    }
    g_npane = 1;
    g_split = 0;
    g_fpane = 0;
    rosh_select(0);
}

static void term_focus_pane(int i) {
    if (i < 0 || i >= g_npane) return;
    g_fpane = i;
    rosh_select(i);
}

static void term_scroll(int delta) {
    term_pane *p = &g_pane[g_fpane];
    int maxv = p->n;
    p->view += delta;
    if (p->view < 0) p->view = 0;
    if (p->view > maxv) p->view = maxv;
}

static int term_set_theme(int id) {
    if (id < 0 || id >= TERM_THEME_N) id = (g_term_theme + 1) % TERM_THEME_N;
    g_term_theme = id;
    env_set("TERM_THEME", TERM_THEME_NAME[id]);
    return id;
}

/* BSD-flavoured prompt: [HH:MM:SS] user@echos:path % (cwd from the vault). */
static void term_prompt(char *d, char *e) {
    char t[12];
    clock_fmt_time(t, sizeof(t));
    d = sapp(d, "[", e); d = sapp(d, t, e); d = sapp(d, "] ", e);
    const char *u = env_get("USER");
    const char *cwd = vfs_getcwd();
    d = sapp(d, u ? u : "guest", e);
    d = sapp(d, "@echos:", e);
    d = sapp(d, cwd && cwd[0] ? cwd : "/", e);
    d = sapp(d, " % ", e);
}

static void term_exec(void) {
    static char cmd[ROSH_LINE_MAX];
    if (!rosh_take(cmd, sizeof(cmd))) return;

    char shown[TERM_COLS];
    { char *d = shown, *e = shown + sizeof(shown);
      term_prompt(d, e);
      sapp(shown + k_strlen(shown), cmd, e); }
    term_line(shown);

    if (!cmd[0]) return;

    if (cmd_is(cmd, "split") || cmd_is(cmd, "vsplit") || cmd_is(cmd, "hsplit")) {
        int mode = 1;
        if (cmd_is(cmd, "hsplit") || (cmd[5] == ' ' && (cmd[6] == 'h' || cmd[6] == 'H')))
            mode = 2;
        term_do_split(mode);
        term_line(mode == 2 ? "split: horizontal (Ctrl+O). Ctrl+N next pane."
                            : "split: vertical (Ctrl+E). Ctrl+N next pane.");
        term_line(" ");
        return;
    }
    if (cmd_is(cmd, "unsplit") || cmd_is(cmd, "only")) {
        term_unsplit();
        term_line("unsplit: one pane.");
        term_line(" ");
        return;
    }
    if (cmd_is(cmd, "termtheme") || cmd_is(cmd, "termcolor")) {
        const char *arg = cmd;
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
        {
            int id = term_theme_by_name(arg);
            id = term_set_theme(id);
            term_line("termtheme:");
            { char m[64]; char *d = m, *e = m + sizeof(m);
              d = sapp(d, "  ", e); sapp(d, TERM_THEME_NAME[id], e);
              term_line(m); }
            term_line("  night  matrix  ocean  amber  paper");
            term_line(" ");
        }
        return;
    }

    /* `neuro` is interactive, not a command that prints and returns: it wants
       the keyboard for as long as you look at it. In a window manager that is
       a window, so the terminal opens one rather than reporting a command it
       cannot run. The browser shell keeps its own full-screen version. */
    if (cmd_is(cmd, "neuro")) {
        g_term_want_neuro = true;
        term_line("opening the Neuro window - any key there is a stimulus.");
        term_line(" ");
        return;
    }

    /* commands_dispatch tokenizes in place, so hand it a scratch copy. */
    static char scratch[ROSH_LINE_MAX], runfile[64], out[3000];
    { char *d = scratch, *e = scratch + sizeof(scratch); sapp(d, cmd, e); }
    out[0] = '\0';

    switch (commands_dispatch(scratch, runfile, sizeof(runfile), out, sizeof(out))) {
    case CMD_TEXT:  term_write(out); break;
    case CMD_CLEAR: pane_clear(&g_pane[g_fpane]); break;
    case CMD_HALT:  term_line("halting..."); g_term_halt = true; break;
    case CMD_RUN_FILE:
        /* Roxenite apps paint the whole screen, so they cannot run inside a
           window. Say that instead of pretending the command did nothing. */
        term_line("Roxenite apps take over the display: leave the WM with Esc,");
        term_line("then `run <file.rxc>` from the shell.");
        break;
    case CMD_NOT_HANDLED: {
        /* Distinguish "no such command" from "that command exists but is a
           view of the browser shell". Answering `not a command: settings`
           when `settings` plainly is one is the kind of small lie that makes
           a shell feel broken. */
        static const char *const BROWSER_ONLY[] = {
            "go", "home", "you", "account", "id", "whoami", "settings",
            "editor", "edit", "rx", "report", "bans",
        };
        bool view = false;
        for (unsigned i = 0; i < sizeof(BROWSER_ONLY)/sizeof(BROWSER_ONLY[0]); i++)
            if (cmd_is(cmd, BROWSER_ONLY[i])) { view = true; break; }

        char m[TERM_COLS]; char *d = m, *e = m + sizeof(m);
        if (view) {
            d = sapp(d, "`", e); d = sapp(d, cmd, e);
            sapp(d, "` is a browser-shell view, not a window.", e);
            term_line(m);
            term_line("Press Esc to leave the WM, then type it there.");
        } else {
            d = sapp(d, "not a command: ", e); sapp(d, cmd, e);
            term_line(m);
            term_line("`help` lists what works. Tab completes command names.");
        }
        break; }
    }
    term_line(" ");
}

/* How much of the screen a key changed. Typing only ever edits the prompt
   strip; running a command rewrites the scrollback. Telling the caller which
   it was is what keeps a keystroke from costing a full-screen repaint. */
typedef enum { TERM_IGNORED, TERM_INPUT_ONLY, TERM_FULL } term_key_result;

static term_key_result term_key(int ch) {
    const char *comp = NULL;
    if (ch == KEY_PGUP) { term_scroll(8);  return TERM_FULL; }
    if (ch == KEY_PGDN) { term_scroll(-8); return TERM_FULL; }
    if (ch == 5)  { term_do_split(1); return TERM_FULL; }   /* Ctrl+E vsplit */
    if (ch == 15) { term_do_split(2); return TERM_FULL; }   /* Ctrl+O hsplit */
    if (ch == 23) { term_unsplit();   return TERM_FULL; }   /* Ctrl+W only   */
    if (ch == 14) {
        if (g_npane > 1) term_focus_pane((g_fpane + 1) % g_npane);
        return TERM_FULL;
    }
    switch (rosh_key(ch, &comp)) {
    case ROSH_SUBMIT:       term_exec(); return TERM_FULL;
    case ROSH_LINE_CHANGED: return TERM_INPUT_ONLY;
    default:                return TERM_IGNORED;
    }
}

static void term_prompt(char *d, char *e);

static int has_sub(const char *s, const char *k) {
    int i, j;
    if (!s || !k || !k[0]) return 0;
    for (i = 0; s[i]; i++) {
        for (j = 0; k[j] && s[i + j] && s[i + j] == k[j]; j++) {}
        if (!k[j]) return 1;
    }
    return 0;
}

static uint32_t term_row_fg(const char *row, const term_pal *p) {
    if (has_sub(row, "@echos:")) return p->prompt;
    if (has_sub(row, "not a command") || has_sub(row, "FAIL") ||
        has_sub(row, "error") || row[0] == '!')
        return p->err;
    if (has_sub(row, "PASS") || (row[0] == 'o' && row[1] == 'k') || row[0] == '+')
        return p->ok;
    if (row[0] == '#' || (row[0] == '/' && row[1] == '/')) return p->dim;
    if (row[0] == '*' || row[0] == '~') return p->warn;
    if (row[0] == ' ' && row[1] == ' ') return p->acc;   /* indented lists   */
    if (row[0] >= 'A' && row[0] <= 'Z' && row[1] == ':') return p->acc;
    if (row[0] >= '0' && row[0] <= '9') return p->warn;
    return p->fg;
}

/* Geometry of pane `pi` inside the terminal client rect. */
static void term_pane_rect(int cx, int cy, int cw, int ch, int pi,
                           int *ox, int *oy, int *ow, int *oh) {
    *ox = cx; *oy = cy; *ow = cw; *oh = ch;
    if (g_npane < 2 || g_split == 0) return;
    if (g_split == 1) {
        int half = (cw - TERM_SASH) / 2;
        if (pi == 0) { *ow = half; }
        else { *ox = cx + half + TERM_SASH; *ow = cw - half - TERM_SASH; }
    } else {
        int half = (ch - TERM_SASH) / 2;
        if (pi == 0) { *oh = half; }
        else { *oy = cy + half + TERM_SASH; *oh = ch - half - TERM_SASH; }
    }
}

static int term_hit_pane(int cx, int cy, int cw, int ch, int px, int py) {
    int i, ox, oy, ow, oh;
    if (g_npane < 2) return 0;
    for (i = 0; i < g_npane; i++) {
        term_pane_rect(cx, cy, cw, ch, i, &ox, &oy, &ow, &oh);
        if (px >= ox && px < ox + ow && py >= oy && py < oy + oh) return i;
    }
    return g_fpane;
}

static void draw_term_input_at(int cx, int cy, int cw, int ch, int sess) {
    const term_pal *p = term_palette();
    const int lh = 16, pad = 8;
    int maxc = (cw - 2 * pad) / 8;
    int iy, plen, caret, prev;
    char line[TERM_COLS];
    if (maxc < 8) return;
    if (maxc > TERM_COLS - 1) maxc = TERM_COLS - 1;
    iy = cy + ch - pad - lh;
    fb_fill_rect((uint32_t)(cx + 3), (uint32_t)(iy - 2),
                 (uint32_t)(cw - 6), (uint32_t)(lh + 2), p->bar);
    prev = rosh_active();
    rosh_select(sess);
    { char *d = line, *e = line + sizeof(line);
      term_prompt(d, e);
      sapp(line + k_strlen(line), rosh_line(), e); }
    plen = 0;
    { char pr[TERM_COLS]; term_prompt(pr, pr + sizeof(pr)); plen = (int)k_strlen(pr); }
    line[maxc] = '\0';
    fb_text((uint32_t)(cx + pad), (uint32_t)iy, line,
            sess == g_fpane ? p->cmd : p->dim, p->bar);
    if (sess == g_fpane) {
        caret = cx + pad + (plen + rosh_cursor()) * 8;
        if (caret < cx + cw - 4)
            fb_fill_rect((uint32_t)caret, (uint32_t)iy, 8, 14, p->caret);
    }
    rosh_select(prev);
}

static void draw_one_pane(wm_window *win, int cx, int cy, int cw, int ch, int pi) {
    const term_pal *p = term_palette();
    term_pane *pane = &g_pane[pi];
    const int lh = 16, pad = 8;
    int rows, maxc, body, first, last, y, i;
    if (cw < 24 || ch < 36) return;
    rows = (ch - 2 * pad) / lh;
    if (rows < 2) return;
    maxc = (cw - 2 * pad) / 8;
    if (maxc < 8) return;
    if (maxc > TERM_COLS - 1) maxc = TERM_COLS - 1;

    fb_fill_rect((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)ch, p->bg);
    if (pi == g_fpane)
        fb_rect_outline((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)ch, p->sash);

    body = rows - 1;
    last = pane->n - pane->view;
    if (last < 0) last = 0;
    if (last > pane->n) last = pane->n;
    first = last > body ? last - body : 0;
    y = cy + pad;
    for (i = first; i < last; i++, y += lh) {
        char row[TERM_COLS];
        int c = 0;
        uint32_t fg, bg;
        bool selected = false;
        for (; pane->tl[i][c] && c < maxc; c++) row[c] = pane->tl[i][c];
        row[c] = '\0';
        if (win->selecting && pi == g_fpane) {
            int y0 = win->sel_start_y < win->sel_end_y ? win->sel_start_y : win->sel_end_y;
            int y1 = win->sel_start_y < win->sel_end_y ? win->sel_end_y : win->sel_start_y;
            selected = (y + lh > y0 && y < y1);
        }
        fg = term_row_fg(row, p);
        bg = p->bg;
        if (selected) { fg = 0x00FFFFFF; bg = p->sel; }
        fb_fill_rect((uint32_t)(cx + pad), (uint32_t)y,
                     (uint32_t)(maxc * 8), (uint32_t)lh, bg);
        fb_text((uint32_t)(cx + pad), (uint32_t)y, row, fg, bg);
    }

    /* scrollbar when the list is taller than the pane */
    if (pane->n > body) {
        int track = ch - 2 * pad - lh;
        int thumb = track * body / pane->n;
        int maxoff = pane->n - body;
        int off = maxoff - pane->view;
        int ty;
        if (thumb < 8) thumb = 8;
        if (off < 0) off = 0;
        if (off > maxoff) off = maxoff;
        ty = cy + pad + (maxoff ? (track - thumb) * off / maxoff : 0);
        fb_fill_rect((uint32_t)(cx + cw - 5), (uint32_t)(cy + pad),
                     3, (uint32_t)track, p->dim);
        fb_fill_rect((uint32_t)(cx + cw - 6), (uint32_t)ty,
                     5, (uint32_t)thumb, p->acc);
    }
    draw_term_input_at(cx, cy, cw, ch, pi);
}

static void draw_terminal(wm_window *win, int cx, int cy, int cw, int ch) {
    const term_pal *p = term_palette();
    int i, ox, oy, ow, oh;
    fb_fill_rect((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)ch, p->bg);
    if (g_npane > 1 && g_split == 1) {
        int half = (cw - TERM_SASH) / 2;
        fb_fill_rect((uint32_t)(cx + half), (uint32_t)cy,
                     TERM_SASH, (uint32_t)ch, p->sash);
    } else if (g_npane > 1 && g_split == 2) {
        int half = (ch - TERM_SASH) / 2;
        fb_fill_rect((uint32_t)cx, (uint32_t)(cy + half),
                     (uint32_t)cw, TERM_SASH, p->sash);
    }
    for (i = 0; i < g_npane; i++) {
        term_pane_rect(cx, cy, cw, ch, i, &ox, &oy, &ow, &oh);
        draw_one_pane(win, ox, oy, ow, oh, i);
    }
}

static void term_refresh_input(wm_window *w) {
    int cx = w->x + 2, cy = w->y + WM_TITLE_H;
    int cw = w->w - 4, ch = w->h - WM_TITLE_H - 2;
    int ox, oy, ow, oh;
    fb_cursor_hide();
    fb_damage_reset();
    term_pane_rect(cx, cy, cw, ch, g_fpane, &ox, &oy, &ow, &oh);
    draw_term_input_at(ox, oy, ow, oh, g_fpane);
    fb_cursor_show();
    fb_present();
}

/* ============================ File Explorer ==============================
   Vault browser for RXFS: sidebar roots, path bar, list + preview, drag into
   folders, clipboard cut/copy/paste, right-click actions. */
#define FM_MAX 48
#define FM_SIDE 92
#define FM_ROW  18
static char g_fm_path[RXFS_NAME] = "/";
static char g_fm_names[FM_MAX][RXFS_NAME];
static bool g_fm_isdir[FM_MAX];
static int  g_fm_n;
static int  g_fm_scroll;
static char g_fm_clip[RXFS_NAME];
static bool g_fm_clip_has;
static bool g_fm_clip_cut;          /* true = vanish after paste            */
static int  g_fm_drag = -1;         /* index being dragged, or -1           */
static char g_fm_status[64];

static void fm_set_status(const char *s) {
    char *d = g_fm_status, *e = g_fm_status + sizeof(g_fm_status);
    if (!s) { g_fm_status[0] = '\0'; return; }
    sapp(d, s, e);
}

static bool fm_name_is_dir(const char *n) {
    if (!n || !n[0]) return false;
    size_t L = k_strlen(n);
    return L > 0 && n[L - 1] == '/';
}

static void fm_basename_clean(const char *n, char *out, size_t max) {
    size_t i = 0;
    for (; n[i] && n[i] != '/' && i + 1 < max; i++) out[i] = n[i];
    out[i] = '\0';
}

static void fm_join(char *out, size_t max, const char *dir, const char *name) {
    char base[RXFS_NAME];
    fm_basename_clean(name, base, sizeof(base));
    char *d = out, *e = out + max;
    if (!dir || !dir[0] || (dir[0] == '/' && !dir[1])) {
        *d++ = '/';
        for (const char *p = base; *p && d < e - 1; p++) *d++ = *p;
        *d = '\0';
        return;
    }
    for (const char *p = dir; *p && d < e - 1; p++) *d++ = *p;
    if (d > out && d[-1] != '/' && d < e - 1) *d++ = '/';
    for (const char *p = base; *p && d < e - 1; p++) *d++ = *p;
    *d = '\0';
}

static void fm_parent(char *path) {
    if (!path[0] || (path[0] == '/' && !path[1])) { path[0] = '/'; path[1] = '\0'; return; }
    size_t L = k_strlen(path);
    while (L > 1 && path[L - 1] == '/') path[--L] = '\0';
    while (L > 1 && path[L - 1] != '/') path[--L] = '\0';
    if (L > 1 && path[L - 1] == '/') path[--L] = '\0';
    if (L == 0) { path[0] = '/'; path[1] = '\0'; }
}

static void fm_refresh(void) {
    g_fm_n = vfs_list_dir(g_fm_path, g_fm_names, FM_MAX);
    if (g_fm_n < 0) g_fm_n = 0;
    for (int i = 0; i < g_fm_n; i++) g_fm_isdir[i] = fm_name_is_dir(g_fm_names[i]);
    if (g_fm_scroll > g_fm_n) g_fm_scroll = 0;
    if (g_fm_scroll < 0) g_fm_scroll = 0;
}

static void fm_go(const char *path) {
    if (!path || !path[0]) path = "/";
    char *d = g_fm_path, *e = g_fm_path + sizeof(g_fm_path);
    if (path[0] == '/' && !path[1]) { g_fm_path[0] = '/'; g_fm_path[1] = '\0'; }
    else {
        d = g_fm_path;
        for (const char *p = path; *p && d < e - 1; p++) *d++ = *p;
        *d = '\0';
    }
    g_fm_scroll = 0;
    fm_refresh();
    fm_set_status("browsing vault");
}

static int  fm_name_is_image(const char *n);
static bool photo_load_path(const char *path);
static int  spawn(wm_kind kind, int ws, bool floating);
static void fm_open_sel(wm_window *win) {
    if (win->sel < 0 || win->sel >= g_fm_n) return;
    if (g_fm_isdir[win->sel]) {
        char next[RXFS_NAME];
        fm_join(next, sizeof(next), g_fm_path, g_fm_names[win->sel]);
        fm_go(next);
        win->sel = 0;
        return;
    }
    /* Image files open in the Multimedia viewer, like a file manager should. */
    if (fm_name_is_image(g_fm_names[win->sel])) {
        char full[RXFS_NAME];
        fm_join(full, sizeof(full), g_fm_path, g_fm_names[win->sel]);
        if (photo_load_path(full)) {
            int id = spawn(WK_PHOTOS, g_ws, true);
            if (id >= 0) g_focus = id;
            g_dirty = true;
        }
    }
}

static void fm_delete_sel(wm_window *win) {
    if (win->sel < 0 || win->sel >= g_fm_n) return;
    char full[RXFS_NAME];
    fm_join(full, sizeof(full), g_fm_path, g_fm_names[win->sel]);
    /* Papelera: move into /trash when possible; hard-delete as fallback. */
    if (!has_sub(full, "/trash/")) {
        char dst[RXFS_NAME];
        fm_join(dst, sizeof(dst), "/trash", g_fm_names[win->sel]);
        if (vfs_rename(full, dst)) {
            fm_set_status("moved to /trash (papelera)");
            fm_refresh();
            if (win->sel >= g_fm_n) win->sel = g_fm_n - 1;
            return;
        }
    }
    bool ok = g_fm_isdir[win->sel] ? vfs_rmdir(full) : vfs_delete(full);
    fm_set_status(ok ? "vanished" : "could not vanish (dir not empty?)");
    fm_refresh();
    if (win->sel >= g_fm_n) win->sel = g_fm_n - 1;
}

/* Properties: inspect the selected item into the explorer status line. */
static void fm_properties(wm_window *win) {
    if (win->sel < 0 || win->sel >= g_fm_n) return;
    char full[RXFS_NAME];
    fm_join(full, sizeof(full), g_fm_path, g_fm_names[win->sel]);
    char info[96]; char *d = info, *e = info + sizeof(info);
    d = sapp(d, g_fm_names[win->sel], e);
    if (g_fm_isdir[win->sel]) {
        sapp(d, "  [dir]  ", e);
    } else {
        uint32_t len = 0;
        vfs_read(full, &len);
        d = sapp(d, "  ", e);
        d = napp(d, len, e);
        d = sapp(d, " B  ", e);
        sapp(d, fm_name_is_image(g_fm_names[win->sel]) ? "image" : "file", e);
    }
    fm_set_status(info);
    g_dirty = true;
}

/* Inline rename: the selected row becomes an edit field; typing replaces
   the name, Enter confirms (vfs_rename), Esc cancels. */
static char g_fm_rename[RXFS_NAME];
static bool g_fm_renaming;

static void fm_rename_begin(wm_window *win) {
    if (win->sel < 0 || win->sel >= g_fm_n) return;
    size_t n = 0;
    const char *s = g_fm_names[win->sel];
    while (s[n] && n < sizeof(g_fm_rename) - 1) { g_fm_rename[n] = s[n]; n++; }
    g_fm_rename[n] = '\0';
    g_fm_renaming = true;
    fm_set_status("rename: type, Enter confirms, Esc cancels");
    g_dirty = true;
}

static void fm_rename_key(wm_window *win, char ch) {
    if (!g_fm_renaming) return;
    if (ch == '\n' || ch == '\r') {
        if (g_fm_rename[0]) {
            char src[RXFS_NAME], dst[RXFS_NAME];
            fm_join(src, sizeof(src), g_fm_path, g_fm_names[win->sel]);
            fm_join(dst, sizeof(dst), g_fm_path, g_fm_rename);
            fm_set_status(vfs_rename(src, dst) ? "renamed" : "rename failed");
        }
        g_fm_renaming = false;
        fm_refresh();
        g_dirty = true;
        return;
    }
    if (ch == 27) { g_fm_renaming = false; fm_set_status("rename cancelled"); g_dirty = true; return; }
    size_t L = k_strlen(g_fm_rename);
    if ((ch == 0x7F || ch == '\b')) {
        if (L) g_fm_rename[L - 1] = '\0';
    } else if (ch >= 32 && ch < 127 && L < sizeof(g_fm_rename) - 1) {
        g_fm_rename[L] = ch; g_fm_rename[L + 1] = '\0';
    }
    g_dirty = true;
}

static void fm_copy_sel(wm_window *win, bool cut) {
    if (win->sel < 0 || win->sel >= g_fm_n) return;
    fm_join(g_fm_clip, sizeof(g_fm_clip), g_fm_path, g_fm_names[win->sel]);
    g_fm_clip_has = true;
    g_fm_clip_cut = cut;
    fm_set_status(cut ? "cut — paste to move" : "copied — paste to duplicate");
}

static void fm_paste(void) {
    if (!g_fm_clip_has || !g_fm_clip[0]) { fm_set_status("clipboard empty"); return; }
    char base[RXFS_NAME];
    fm_basename_clean(g_fm_clip, base, sizeof(base));
    /* if clip was a dir path ending with name only */
    { size_t L = k_strlen(g_fm_clip);
      if (L && g_fm_clip[L-1] == '/') { /* strip */ g_fm_clip[L-1] = '\0';
        fm_basename_clean(g_fm_clip, base, sizeof(base)); } }
    char dst[RXFS_NAME];
    fm_join(dst, sizeof(dst), g_fm_path, base);
    bool ok = false;
    if (g_fm_clip_cut) {
        ok = vfs_rename(g_fm_clip, dst);
        if (ok) { g_fm_clip_has = false; g_fm_clip_cut = false; }
        fm_set_status(ok ? "moved into vault folder" : "move failed");
    } else {
        ok = vfs_copy(g_fm_clip, dst);
        fm_set_status(ok ? "forged a copy here" : "copy failed (dir?)");
    }
    fm_refresh();
}

static void fm_new_note(void) {
    char name[32];
    char *d = name, *e = name + sizeof(name);
    d = sapp(d, "note-", e);
    d = napp(d, arch_timer_ticks() % 10000, e);
    sapp(d, ".txt", e);
    char full[RXFS_NAME];
    fm_join(full, sizeof(full), g_fm_path, name);
    static const char body[] = "New note from the explorer.\n";
    if (vfs_write(full, body, (uint32_t)(sizeof(body) - 1)))
        fm_set_status("scribed a new note");
    else
        fm_set_status("could not create note");
    fm_refresh();
}

static void fm_new_folder(void) {
    char name[32];
    char *d = name, *e = name + sizeof(name);
    d = sapp(d, "folder-", e);
    napp(d, arch_timer_ticks() % 1000, e);
    char full[RXFS_NAME];
    fm_join(full, sizeof(full), g_fm_path, name);
    fm_set_status(vfs_mkdir(full) ? "homesteaded a folder" : "mkdir failed");
    fm_refresh();
}

static int fm_name_is_image(const char *n) {
    int i = 0, k;
    char ext[8];
    if (!n) return 0;
    while (n[i]) i++;
    while (i > 0 && n[i - 1] != '.') i--;
    if (!i) return 0;
    for (k = 0; n[i] && k < 7; i++, k++) {
        char c = n[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        ext[k] = c;
    }
    ext[k] = 0;
    return (ext[0]=='j' && ext[1]=='p' && ext[2]=='g') ||
           (ext[0]=='j' && ext[1]=='p' && ext[2]=='e' && ext[3]=='g') ||
           (ext[0]=='p' && ext[1]=='n' && ext[2]=='g') ||
           (ext[0]=='q' && ext[1]=='o' && ext[2]=='i');
}

/* Sidebar shortcut roots */
static const char *FM_ROOTS[] = { "/", "/bin", "/etc", "/home", "/var", "/tmp", "/trash" };
#define FM_ROOT_N 7

static void draw_files(wm_window *win, int cx, int cy, int cw, int ch) {
    if (g_fm_n == 0 && g_fm_path[0] == '/' && !g_fm_path[1]) fm_refresh();

    /* Sidebar */
    fb_fill_rect((uint32_t)cx, (uint32_t)cy, FM_SIDE, (uint32_t)ch, 0x00141418);
    fb_fill_rect((uint32_t)(cx + FM_SIDE), (uint32_t)cy, 1, (uint32_t)ch, RGX_BORDER);
    fb_text(cx + 8, cy + 6, "Vault", 0x00C8C8D0, 0x00141418);
    for (int i = 0; i < FM_ROOT_N; i++) {
        int y = cy + 28 + i * FM_ROW;
        bool on = false;
        { const char *a = g_fm_path, *b = FM_ROOTS[i];
          on = true; while (*a && *b) { if (*a != *b) { on = false; break; } a++; b++; }
          if (on && *b) on = false;
          if (!on && FM_ROOTS[i][0] == '/' && !FM_ROOTS[i][1])
              on = (g_fm_path[0] == '/' && !g_fm_path[1]);
        }
        if (on) fb_fill_rect((uint32_t)(cx + 2), (uint32_t)(y - 1), FM_SIDE - 4, FM_ROW - 2, RGX_NAV_SEL);
        fb_text(cx + 10, y, FM_ROOTS[i], on ? 0x00FFFFFF : 0x00C0C0C8,
                on ? RGX_NAV_SEL : 0x00141418);
    }

    int lx = cx + FM_SIDE + 6;
    int lw = cw - FM_SIDE - 10;
    if (lw < 80) lw = 80;

    /* Path bar */
    fb_fill_rect((uint32_t)lx, (uint32_t)(cy + 4), (uint32_t)lw, 20, 0x00101014);
    fb_rect_outline((uint32_t)lx, (uint32_t)(cy + 4), (uint32_t)lw, 20, RGX_BORDER);
    { char bar[80]; char *d = bar, *e = bar + sizeof(bar);
      d = sapp(d, "  ", e); sapp(d, g_fm_path, e);
      fb_text(lx + 4, cy + 6, bar, 0x00E0E0E4, 0x00101014); }
    /* Up button */
    fb_aero_button((uint32_t)(lx + lw - 44), (uint32_t)(cy + 5), 40, 18, false, false);
    fb_text_trans((uint32_t)(lx + lw - 36), (uint32_t)(cy + 7), "Up", 0x00E8E8EA);

    /* List (left of preview) */
    int list_w = lw * 45 / 100;
    if (list_w < 120) list_w = 120;
    int list_top = cy + 30;
    int list_h = ch - 54;
    int rows = list_h / FM_ROW;
    if (rows < 1) rows = 1;

    fb_fill_rect((uint32_t)lx, (uint32_t)list_top, (uint32_t)list_w, (uint32_t)list_h, 0x00101014);
    fb_rect_outline((uint32_t)lx, (uint32_t)list_top, (uint32_t)list_w, (uint32_t)list_h, 0x00404048);

    if (g_fm_scroll > 0 && g_fm_n > rows) {
        /* keep selection visible */
        if (win->sel < g_fm_scroll) g_fm_scroll = win->sel;
        if (win->sel >= g_fm_scroll + rows) g_fm_scroll = win->sel - rows + 1;
    }

    for (int r = 0; r < rows; r++) {
        int i = g_fm_scroll + r;
        if (i >= g_fm_n) break;
        int y = list_top + 2 + r * FM_ROW;
        bool seld = (i == win->sel);
        bool drag = (i == g_fm_drag);
        if (seld || drag)
            fb_fill_rect((uint32_t)(lx + 1), (uint32_t)y, (uint32_t)(list_w - 2), FM_ROW - 1,
                         drag ? 0x00FFD080 : RGX_NAV_SEL);
        char line[72]; char *d = line, *e = line + sizeof(line);
        d = sapp(d, g_fm_isdir[i] ? "[dir] " : "      ", e);
        if (g_fm_renaming && seld) {
            d = sapp(d, g_fm_rename, e); sapp(d, "_", e);
        } else {
            d = sapp(d, g_fm_names[i], e);
        }
        if (!g_fm_isdir[i]) {
            char full[RXFS_NAME];
            fm_join(full, sizeof(full), g_fm_path, g_fm_names[i]);
            int sz = vfs_size(full);
            while (d < line + 28 && d < e - 1) *d++ = ' ';
            *d = '\0'; d = line + k_strlen(line);
            d = napp(d, sz < 0 ? 0 : (uint64_t)sz, e); sapp(d, " B", e);
        }
        fb_text(lx + 6, y + 1, line,
                seld || drag ? 0x00FFFFFF : 0x00D0D0D4,
                seld || drag ? (drag ? 0x00605020 : RGX_NAV_SEL) : 0x00101014);
    }
    if (g_fm_n == 0)
        fb_text(lx + 10, list_top + 20, "(empty folder)", RGX_DIM, 0x00101014);

    /* Preview pane */
    int px = lx + list_w + 6;
    int pw = lx + lw - px;
    if (pw > 40) {
        fb_fill_rect((uint32_t)px, (uint32_t)list_top, (uint32_t)pw, (uint32_t)list_h, 0x00121216);
        fb_rect_outline((uint32_t)px, (uint32_t)list_top, (uint32_t)pw, (uint32_t)list_h, 0x00404048);
        fb_text(px + 8, list_top + 6, "Preview", 0x00C8C8D0, 0x00121216);
        if (win->sel >= 0 && win->sel < g_fm_n && !g_fm_isdir[win->sel]) {
            char full[RXFS_NAME];
            fm_join(full, sizeof(full), g_fm_path, g_fm_names[win->sel]);
            uint32_t len = 0;
            const char *data = vfs_read(full, &len);
            char hdr[64]; char *d = hdr, *e = hdr + sizeof(hdr);
            d = sapp(d, g_fm_names[win->sel], e); d = sapp(d, "  (", e);
            d = napp(d, len, e); sapp(d, " B)", e);
            fb_text(px + 8, list_top + 24, hdr, RGX_DIM, 0x00121216);
            if (data && len >= 8 && fm_name_is_image(g_fm_names[win->sel])) {
                img_info_t inf;
                if (img_info((const uint8_t *)data, len, &inf) && inf.width && inf.height) {
                    static uint32_t *g_fm_prev;
                    static uint32_t  g_fm_pw, g_fm_ph;
                    if (!g_fm_prev || g_fm_pw != inf.width || g_fm_ph != inf.height) {
                        uint64_t bytes = (uint64_t)inf.width * inf.height * 4;
                        uint32_t pages = (uint32_t)((bytes + 4095) / 4096);
                        uint64_t p = pmm_alloc_contig(pages);
                        if (p) {
                            g_fm_prev = (uint32_t *)(uintptr_t)p;
                            g_fm_pw = inf.width; g_fm_ph = inf.height;
                        }
                    }
                    if (g_fm_prev &&
                        img_decode((const uint8_t *)data, len, g_fm_prev,
                                   inf.width * inf.height, &inf) == inf.width * inf.height) {
                        int ih = list_h - 48;
                        if (ih > 24)
                            gui_blit(g_fm_prev, g_fm_pw, g_fm_ph,
                                     px + 8, list_top + 42, pw - 16, ih);
                    }
                }
            } else {
                int ly = list_top + 44, col = 0, maxc = (pw - 16) / 8;
                if (data) for (uint32_t i = 0; i < len && ly < list_top + list_h - 12; i++) {
                    char c = data[i];
                    if (c == '\n' || col >= maxc) { ly += 14; col = 0; if (c == '\n') continue; }
                    char s[2] = { (c >= 0x20 && (unsigned char)c != 0x7f) ? c : '.', 0 };
                    fb_text(px + 8 + col * 8, ly, s, 0x00D0D0D4, 0x00121216);
                    col++;
                }
            }
        } else if (win->sel >= 0 && win->sel < g_fm_n && g_fm_isdir[win->sel]) {
            fb_text(px + 8, list_top + 28, "Directory — double-click to enter", RGX_DIM, 0x00121216);
        } else {
            fb_text(px + 8, list_top + 28, "Select a file", RGX_DIM, 0x00121216);
        }
    }

    /* Status strip */
    fb_fill_rect((uint32_t)lx, (uint32_t)(cy + ch - 18), (uint32_t)lw, 16, 0x0018181C);
    { char st[96]; char *d = st, *e = st + sizeof(st);
      d = napp(d, (uint64_t)g_fm_n, e); d = sapp(d, " entries  ", e);
      if (g_fm_clip_has) d = sapp(d, g_fm_clip_cut ? "[cut] " : "[clip] ", e);
      sapp(d, g_fm_status, e);
      fb_text(lx + 4, cy + ch - 16, st, 0x00C0C0C8, 0x0018181C); }
}

/* Hit-test explorer: returns 1=sidebar root i via *out, 2=list row, 3=up, 0=none */
static int fm_hit(wm_window *w, int px, int py, int *out) {
    int cx = w->x + 2, cy = w->y + WM_TITLE_H, cw = w->w - 4, chh = w->h - WM_TITLE_H - 2;
    if (px < cx || py < cy || px >= cx + cw || py >= cy + chh) return 0;
    /* sidebar */
    if (px < cx + FM_SIDE) {
        for (int i = 0; i < FM_ROOT_N; i++) {
            int y = cy + 28 + i * FM_ROW;
            if (py >= y - 1 && py < y + FM_ROW - 2) { if (out) *out = i; return 1; }
        }
        return 0;
    }
    int lx = cx + FM_SIDE + 6;
    int lw = cw - FM_SIDE - 10;
    if (py >= cy + 5 && py < cy + 23 && px >= lx + lw - 44 && px < lx + lw - 4) return 3;
    int list_w = lw * 45 / 100; if (list_w < 120) list_w = 120;
    int list_top = cy + 30;
    int list_h = chh - 54;
    int rows = list_h / FM_ROW;
    if (px >= lx && px < lx + list_w && py >= list_top && py < list_top + list_h) {
        int r = (py - list_top - 2) / FM_ROW;
        int i = g_fm_scroll + r;
        if (i >= 0 && i < g_fm_n && r < rows) { if (out) *out = i; return 2; }
    }
    return 0;
}

static void draw_monitor(wm_window *win, int cx, int cy, int cw, int ch) {
    (void)win; (void)cw; (void)ch;
    char b[96], *d, *e;
    int y = cy + 8;
    #define LINE(...) do { d = b; e = b + sizeof(b); __VA_ARGS__; fb_text(cx + 10, y, b, RGX_FG, RGX_PANEL); y += 20; } while (0)
    LINE( d = sapp(d, "EchOS 1.0 - System Monitor", e) );
    y += 6;
    LINE( d = sapp(d, "uptime:  ", e); d = napp(d, arch_timer_uptime_ms()/1000, e); sapp(d, " s", e) );
    LINE( d = sapp(d, "ticks:   ", e); napp(d, arch_timer_ticks(), e) );
    LINE( d = sapp(d, "heap:    ", e); d = napp(d, heap_used()/1024, e); d = sapp(d, " / ", e);
          d = napp(d, heap_total()/1024, e); sapp(d, " KiB", e) );
    /* "free / total" read as though the difference were in use. It is not:
       the gap is RAM above the 4 GiB identity map plus the PCI hole. Report
       what is actually consumed, and the addressable window separately. */
    LINE( d = sapp(d, "ram used:", e); d = napp(d, pmm_used_bytes()/(1024*1024), e);
          d = sapp(d, " MiB of ", e); d = napp(d, pmm_usable_bytes()/(1024*1024), e);
          sapp(d, " MiB usable", e) );
    LINE( d = sapp(d, "ram fitted: ", e); d = napp(d, pmm_total_bytes()/(1024*1024), e);
          sapp(d, " MiB (>4 GiB not addressed yet)", e) );
    /* Named rather than left as unexplained growth: the desktop's wallpaper
       and scene cache are most of what "ram used" counts on a fresh boot. */
    LINE( d = sapp(d, "desktop: ", e); d = napp(d, gui_bytes()/(1024*1024), e);
          sapp(d, " MiB (wallpaper + scene cache)", e) );
    LINE( d = sapp(d, "rxfs:    ", e); d = napp(d, (uint64_t)vfs_count(), e); sapp(d, " files", e) );
    LINE( d = sapp(d, "network: ", e); sapp(d, net_present() ? "up (rgx:// L2)" : "down", e) );
    LINE( d = sapp(d, "neuro:   ", e); d = sapp(d, rx_backend_label(), e) );
    LINE( d = sapp(d, "spikes:  ", e); d = napp(d, rx_backend_spikes(), e);
          d = sapp(d, "   idle entries: ", e); napp(d, power_idle_entries(), e) );
    LINE( d = sapp(d, "idle via ", e); sapp(d, power_idle_method(), e) );
    /* a heap-usage bar (the one honestly-measurable "load" figure) */
    y += 6;
    int bw = cw - 40, bx = cx + 10;
    fb_rect_outline(bx, y, bw, 14, RGX_BAR);
    uint64_t used = heap_used(), tot = heap_total() ? heap_total() : 1;
    int fill = (int)((uint64_t)(bw - 2) * used / tot);
    fb_fill_rect(bx + 1, y + 1, fill < 1 ? 1 : fill, 12, RGX_FG);
    fb_text(bx, y + 20, "heap usage", RGX_DIM, RGX_PANEL);
    #undef LINE
}

/* ================================ neuro ==================================
   The LIF demo as a window instead of a full-screen takeover.

   `neuro` used to exist only in the browser shell's dispatcher, where it ran
   its own blocking input loop and painted the entire display. That is why it
   answered "not a command" in the WM terminal: there is no way to hand a
   window's keyboard to a loop that owns the screen. So the demo is rebuilt
   here against the same fabric primitives — one rx_actor_t, stimulated by
   keystrokes, read back with rx_actor_peek_potential() — and drawn inside its
   frame like any other window.

   The actor is static, not a local. sizeof(rx_actor_t) is ~16 KB (it carries
   a 256-slot inbox); putting one on a 64 KiB task stack costs a quarter of it
   per instance, and six of them once cost the whole thing plus the page below
   it. Actors live in BSS. */
static rx_actor_t g_neuro;
static int        g_neuro_win = -1;      /* window id, or -1                 */

static int spawn(wm_kind kind, int ws, bool floating);

static uint32_t   g_neuro_spikes;

/* A rolling trace of the membrane, sampled once per repaint. This is the
   part worth seeing: the sawtooth of integrate-and-fire, and the exponential
   sag between keystrokes that no polling loop would show you. */
#define NEURO_TRACE 640                  /* one sample per pixel column      */
static uint8_t g_trace[NEURO_TRACE];     /* 0..255, scaled to threshold      */
static int     g_trace_n;

static bool neuro_fire(rx_actor_t *self, const rx_event_t *ev) {
    (void)self; (void)ev; g_neuro_spikes++; return true;
}

static void neuro_reset(void) {
    rx_actor_init(&g_neuro, "wm_lif", neuro_fire,
                  /* threshold  */ Q16_ONE,
                  /* reset      */ 0,
                  /* tau        */ 100,   /* 1 s leak at PIT 100 Hz          */
                  /* refractory */ 30);   /* 300 ms                          */
    g_neuro_spikes = 0;
    g_trace_n = 0;
}

/* Push the current potential onto the trace ring. Called from the draw path
   so the graph advances exactly as fast as the window is repainted — the
   x axis is "frames", which is honest, rather than a time axis the WM does
   not actually sample at a fixed rate. */
static void neuro_sample(int32_t v) {
    int scaled = (int)(((int64_t)v * 200) / g_neuro.threshold_q16);
    if (scaled < 0)   scaled = 0;
    if (scaled > 255) scaled = 255;
    if (g_trace_n < NEURO_TRACE) {
        g_trace[g_trace_n++] = (uint8_t)scaled;
    } else {
        for (int i = 1; i < NEURO_TRACE; i++) g_trace[i - 1] = g_trace[i];
        g_trace[NEURO_TRACE - 1] = (uint8_t)scaled;
    }
}

static void neuro_stimulate(void) {
    rx_actor_stimulate(&g_neuro, RX_EVENT_SELFTEST, (Q16_ONE * 35) / 100,
                       arch_timer_ticks());
}

/* Q16.16 -> "d.dd", two decimals, no float anywhere on the path. */
static char *q16app(char *d, int32_t v, char *e) {
    if (v < 0) { d = sapp(d, "-", e); v = -v; }
    d = napp(d, (uint64_t)(v >> 16), e);
    d = sapp(d, ".", e);
    uint32_t frac = (uint32_t)(((uint64_t)(v & 0xFFFF) * 100) >> 16);
    char t[4] = { (char)('0' + frac / 10), (char)('0' + frac % 10), '\0', '\0' };
    return sapp(d, t, e);
}

static void draw_neuro(wm_window *win, int cx, int cy, int cw, int ch) {
    (void)win;
    uint64_t now = arch_timer_ticks();
    int32_t  v   = rx_actor_peek_potential(&g_neuro, now);
    neuro_sample(v);

    char b[112], *d, *e;
    int y = cy + 8;
    #define NLINE(...) do { d = b; e = b + sizeof(b); __VA_ARGS__; \
                            fb_text(cx + 10, y, b, RGX_FG, RGX_PANEL); y += 18; } while (0)

    NLINE( d = sapp(d, "Live LIF neuron - kernel event fabric", e) );
    fb_text(cx + 10, y, "Any key is a stimulus (+0.35). Type slowly: the leak",
            RGX_DIM, RGX_PANEL); y += 16;
    fb_text(cx + 10, y, "wins. Type fast: it crosses 1.00 and fires.  [q] close",
            RGX_DIM, RGX_PANEL); y += 20;

    /* membrane bar, clamped: the potential can sit above threshold for the
       instant before dispatch resets it, and a bar wider than its box would
       paint over the window border. */
    int bw = cw - 40; if (bw < 40) bw = 40;
    int bx = cx + 10;
    fb_rect_outline(bx, y, bw, 16, RGX_BAR);
    int fill = (int)(((int64_t)v * (bw - 2)) / g_neuro.threshold_q16);
    if (fill < 0)      fill = 0;
    if (fill > bw - 2) fill = bw - 2;
    if (fill) fb_fill_rect(bx + 1, y + 1, fill, 14, RGX_FG);
    y += 22;
    NLINE( d = sapp(d, "V = ", e); d = q16app(d, v, e);
           d = sapp(d, "   threshold 1.00   tau ", e);
           d = napp(d, g_neuro.tau_ticks, e);
           d = sapp(d, "   refractory ", e);
           d = napp(d, g_neuro.refractory_ticks, e); sapp(d, " ticks", e) );

    NLINE( d = sapp(d, "spikes ", e);      d = napp(d, g_neuro_spikes, e);
           d = sapp(d, "   suppressed ", e); d = napp(d, g_neuro.stats.suppressed, e);
           d = sapp(d, "   events ", e);     napp(d, g_neuro.stats.received, e) );
    NLINE( d = sapp(d, "STDP weight ", e); d = q16app(d, g_neuro.synapse.weight_q16, e);
           sapp(d, "  (moves on every event/spike pair)", e) );
    y += 6;

    /* The trace. Height is whatever is left; if the window is too short to
       show a graph, the numbers above already said everything. */
    int gh = cy + ch - y - 24;
    if (gh >= 24) {
        int gw = cw - 20; if (gw > NEURO_TRACE) gw = NEURO_TRACE;
        int gx = cx + 10;
        fb_fill_rect(gx, y, gw, gh, RGX_NAV);
        /* threshold line at V = 1.00, which the trace scales to 200/255.
           Dashed, so it reads as a reference rather than as data. */
        int ty = y + gh - (gh * 200) / 255;
        for (int dx = 0; dx + 3 < gw; dx += 8) fb_fill_rect(gx + dx, ty, 4, 1, RGX_DIM);
        /* newest sample at the right edge */
        int first = g_trace_n > gw ? g_trace_n - gw : 0;
        for (int i = first; i < g_trace_n; i++) {
            int col = gx + (i - first);
            int hgt = (g_trace[i] * gh) / 255;
            if (hgt > gh) hgt = gh;
            if (hgt) fb_fill_rect(col, y + gh - hgt, 1, hgt, RGX_FG);
        }
        fb_text(gx, y + gh + 4, "membrane over time (dotted = threshold)",
                RGX_DIM, RGX_PANEL);
    }
    #undef NLINE
}

static void draw_about(wm_window *win, int cx, int cy, int cw, int ch) {
    (void)win; (void)cw; (void)ch;
    const char *L[] = {
        "EchOS 1.0  ECLIPSE  (Dark Aero)",
        "Rogex Laboratories / Knights Labs", "",
        "Glass desktop: rounded frames, reflective chrome.",
        "Right-click desktop: Ajustes / cambiar fondo.",
        "Multimedia: next does NOT change wallpaper.", "",
        "Keys:",
        "  t term  e explorer  b nova  y neuro  k calc",
        "  a ajustes  i photos  s Start  m maximize",
        "  Ctrl+C/V  1/2/3 ws  Esc shell", "",
        "www on  then  curl / wget <http-url-exacta>",
        "Explorer previews JPEG/PNG. Tray: clock.",
    };
    int y = cy + 10;
    for (unsigned i = 0; i < sizeof(L)/sizeof(L[0]); i++) { fb_text(cx + 10, y, L[i], RGX_FG, RGX_CLIENT); y += 18; }
}

/* ============================== calculator ===============================
   The arithmetic lives in userland/apps/calc.c; this is only its face. The
   keypad geometry is computed here and hit-tested against the same table the
   key handler uses, so a button and the key it stands for cannot drift. */
static int g_calc_win = -1;

/* Where button `i` sits inside a content box. Returns false if the window is
   too small to hold a usable keypad. */
static bool calc_cell(int i, int cx, int cy, int cw, int ch,
                      int *bx, int *by, int *bw, int *bh) {
    const int pad = 8, disp_h = 54;
    int gw = cw - 2 * pad;
    int gh = ch - disp_h - 2 * pad;
    if (gw < CALC_COLS * 28 || gh < CALC_ROWS * 20) return false;
    int cwid = gw / CALC_COLS, chgt = gh / CALC_ROWS;
    int r = i / CALC_COLS, c = i % CALC_COLS;
    *bx = cx + pad + c * cwid;
    *by = cy + disp_h + pad + r * chgt;
    *bw = cwid - 3;
    *bh = chgt - 3;
    return true;
}

static void draw_calc(wm_window *win, int cx, int cy, int cw, int ch) {
    (void)win;
    const int pad = 8;
    /* display: inset panel, expression dim on top, value big and bright */
    fb_fill_rect(cx + pad, cy + pad, cw - 2 * pad, 52, RGX_NAV);
    fb_rect_outline(cx + pad, cy + pad, cw - 2 * pad, 52, RGX_BORDER);
    const char *pend = calc_pending();
    if (pend[0]) {
        int pw = (int)k_strlen(pend) * 8;
        int px = cx + cw - pad - 8 - pw;
        if (px < cx + pad + 6) px = cx + pad + 6;
        fb_text((uint32_t)px, cy + pad + 5, pend, RGX_DIM, RGX_NAV);
    }
    const char *d = calc_display();
    /* right-aligned: a calculator whose digits crawl rightwards as you type
       is a calculator you have to re-read after every keystroke. */
    int dw = (int)k_strlen(d) * 8;
    int dx = cx + cw - pad - 8 - dw;
    if (dx < cx + pad + 8) dx = cx + pad + 8;
    /* double-strike for a heavier digit weight (no scalable font yet) */
    fb_text((uint32_t)dx, cy + pad + 22, d, RGX_WHITE, RGX_NAV);
    fb_text((uint32_t)(dx + 1), cy + pad + 22, d, RGX_WHITE, RGX_NAV);

    int n = calc_button_count();
    for (int i = 0; i < n; i++) {
        int bx, by, bw, bh;
        if (!calc_cell(i, cx, cy, cw, ch, &bx, &by, &bw, &bh)) return;
        const char *lb = calc_button_label(i);
        /* The bottom-right pair is one wide `=`; drawing it twice would leave
           a seam down the middle of a single button. */
        if (i == CALC_ROWS * CALC_COLS - 1 && calc_button_key(i) == '=' &&
            calc_button_key(i - 1) == '=') continue;
        if (i == CALC_ROWS * CALC_COLS - 2 && calc_button_key(i) == '=') {
            int bx2, by2, bw2, bh2;
            if (calc_cell(i + 1, cx, cy, cw, ch, &bx2, &by2, &bw2, &bh2))
                bw = bx2 + bw2 - bx;
        }
        bool op = !(lb[0] >= '0' && lb[0] <= '9') && lb[0] != '.';
        fb_fill_rect(bx, by, bw, bh, op ? RGX_BTN : RGX_BAR);
        fb_text(bx + bw / 2 - (int)k_strlen(lb) * 4, by + bh / 2 - 8, lb,
                op ? RGX_BTN_TXT : RGX_WHITE, op ? RGX_BTN : RGX_BAR);
    }
}

/* A click inside the calculator's content box: find the button under it and
   feed its key to the same handler the keyboard uses. */
static void calc_click(wm_window *w, int px, int py) {
    int cx = w->x, cy = w->y + WM_TITLE_H, cw = w->w, chh = w->h - WM_TITLE_H;
    int n = calc_button_count();
    for (int i = 0; i < n; i++) {
        int bx, by, bw, bh;
        if (!calc_cell(i, cx, cy, cw, chh, &bx, &by, &bw, &bh)) return;
        if (px >= bx && px < bx + bw && py >= by && py < by + bh) {
            calc_key(calc_button_key(i));
            return;
        }
    }
}

/* ============================= desktop icons =============================
   Icons are how a desktop says what it can do without you having to already
   know. They live on the wallpaper in the top-left column and open a window
   on a single click — single, not double, because there is no double-click
   timing anywhere else in this system and inventing one here would be a
   convention only this row of pictures knows about. */
#define ICON_W   88
#define ICON_H   72
#define ICON_X   14
#define ICON_Y0  (WM_BAR_H + 10)

typedef struct { const char *label; const char *iname; wm_kind kind; } desk_icon;
static const desk_icon ICONS[] = {
    { "Ajustes",     "ajustes",  WK_SETTINGS },
    { "Terminal",    "terminal", WK_TERMINAL },
    { "Explorer",    "explorer", WK_FILES    },
    { "Neuro",       "neuro",    WK_NEURO    },
    { "Calculator",  "calc",     WK_CALC     },
    { "Disks",       "disks",    WK_DISKS    },
    { "Multimedia",  "photos",   WK_PHOTOS   },
    { "About",       "about",    WK_ABOUT    },
};
#define ICON_COUNT ((int)(sizeof(ICONS)/sizeof(ICONS[0])))

/* Desktop icon slots: icons live in grid slots, and global drag & drop
   reorders them. g_icon_slot[icon] = slot index; identity at boot. */
static int g_icon_slot[ICON_COUNT];
static bool g_icon_removed[ICON_COUNT];   /* session-only "Delete" from desktop */
static int g_icon_drag  = -1;   /* icon held under the pointer, else -1 */
static bool g_icon_moving;      /* press crossed the drag threshold     */
static bool g_marquee;          /* rubber-band selection active         */
static uint32_t g_marquee_x0, g_marquee_y0, g_marquee_x1, g_marquee_y1;
static bool g_icon_selected[ICON_COUNT];   /* multi-select set          */
static uint32_t g_icon_press_x, g_icon_press_y;

static void icon_slots_reset(void) {
    for (int i = 0; i < ICON_COUNT; i++) { g_icon_slot[i] = i; g_icon_removed[i] = false; }
}

#define ICON_SLOTS_PER_COL 5

static int icon_slot_xy(int slot, int *x, int *y) {
    int col = slot / ICON_SLOTS_PER_COL;
    int row = slot % ICON_SLOTS_PER_COL;
    *x = ICON_X + col * (ICON_W + 8);
    *y = ICON_Y0 + row * ICON_H;
    return 1;
}

static int icon_slot_at(int px, int py) {
    int col = (px - ICON_X) / (ICON_W + 8);
    int row = (py - ICON_Y0) / ICON_H;
    if (col < 0 || row < 0 || row >= ICON_SLOTS_PER_COL) return -1;
    return col * ICON_SLOTS_PER_COL + row;
}

static void icon_box(int i, int *x, int *y) {
    /* Slots (not indices) position icons so dragging reorders the grid. */
    icon_slot_xy(g_icon_slot[i], x, y);
}

static void draw_icons(void) {
    int dim = icon_dim();
    if (dim <= 0) dim = 48;
    for (int i = 0; i < ICON_COUNT; i++) {
        if (g_icon_removed[i]) continue;
        int x, y, id; icon_box(i, &x, &y);
        if (y + ICON_H > g_screen_h - WM_TASK_H) return;
        /* White glow so dark-outlined glyphs read against the eclipse
           wallpaper: soft outer halo, then a bright backing plate. */
        fb_round_blend((uint32_t)(x + 8), (uint32_t)y, 52, 52, 12, 0x38FFFFFF, 90);
        fb_round_blend((uint32_t)(x + 11), (uint32_t)(y + 3), 46, 46, 10, 0xB8E8ECF0, 200);
        fb_round_fill((uint32_t)(x + 13), (uint32_t)(y + 5), 42, 42, 9, 0xFFF2F4F6);
        id = icon_index_by_name(ICONS[i].iname);
        if (id >= 0 && icon_rgba_at(id))
            gui_blit(icon_rgba_at(id), 48, 48, x + 14, y + 7, 36, 36);
        else {
            fb_round_fill((uint32_t)(x + 16), (uint32_t)(y + 10), 36, 30, 6, RGX_PANEL);
            fb_text_trans((uint32_t)(x + 20), (uint32_t)(y + 16), ICONS[i].label, 0x00101018);
        }
        {
            /* Label with a hard black shadow — never camouflages. */
            int lw = (int)k_strlen(ICONS[i].label) * 8;
            int lx = x + 40 - lw / 2;
            fb_text_trans((uint32_t)(lx + 1), (uint32_t)(y + 51), ICONS[i].label, 0xFF000000);
            fb_text_trans((uint32_t)lx, (uint32_t)(y + 50), ICONS[i].label, 0xFFFFFFFF);
        }
    }
}

static void draw_disks(wm_window *win, int cx, int cy, int cw, int ch);
static void draw_photos(wm_window *win, int cx, int cy, int cw, int ch);
static void draw_settings(wm_window *win, int cx, int cy, int cw, int ch);
static void draw_browser(wm_window *win, int cx, int cy, int cw, int ch);

static int icon_at(int px, int py) {
    for (int i = 0; i < ICON_COUNT; i++) {
        if (g_icon_removed[i]) continue;
        int x, y; icon_box(i, &x, &y);
        if (px >= x && px < x + ICON_W && py >= y && py < y + ICON_H) return i;
    }
    return -1;
}

/* --- taskbar and Start menu (Windows 7 Aero style) --- */
#define START_W  72

typedef enum {
    SM_SETTINGS, SM_TERMINAL, SM_FILES, SM_MONITOR, SM_NEURO, SM_CALC,
    SM_DISKS, SM_PHOTOS, SM_ABOUT,
    SM_SHELL, SM_REBOOT, SM_OFF, SM_HALT, SM_COUNT
} start_item;

static const char *SM_LABEL[SM_COUNT] = {
    "Ajustes", "Terminal", "Explorer", "System Monitor", "Neuro",
    "Calculator", "Disks", "Multimedia", "About",
    "Classic shell", "Reboot", "Shutdown", "Halt",
};
/* Rows the menu draws a separator above. */
static bool sm_rule(int i) { return i == SM_SHELL || i == SM_REBOOT; }

static bool g_start_open;
static bool g_help_open;      /* '?' — keyboard shortcuts cheat sheet */
static int  g_start_x = -1;   /* Start button hit box (top bar)       */
static int  g_clock_x = -1, g_clock_w = 0;

/* Keyboard shortcuts cheat sheet (toggle with ?). */
static const char *const HELP_LINES[] = {
    "EchOS 1.0 — Eclipse Shell shortcuts",
    "",
    "  t  Terminal        e  Explorer (files)",
    "  b  Nova browser    v  Echo assistant",
    "  i  Multimedia      k  Calculator",
    "  d  Disks           a  Ajustes (settings)",
    "  y  Neuro           s  Start menu",
    "  1/2/3  workspaces  n  new window",
    "  f  tile/float      q  close focused",
    "  Tab  cycle focus   ?  this panel (Esc closes)",
    "",
    "Mouse: drag desktop icons, drop on dock to pin;",
    "right-click anything for Open/Rename/Delete/Pin.",
};

static void draw_help_panel(void) {
    if (!g_help_open) return;
    uint32_t sw, sh; fb_dims(&sw, &sh);
    uint32_t pw = 460, ph = (uint32_t)(sizeof(HELP_LINES) / sizeof(HELP_LINES[0])) * 20 + 24;
    uint32_t x = (sw > pw) ? (sw - pw) / 2 : 2;
    uint32_t y = (sh > ph + WM_TASK_H) ? ((sh - ph - WM_TASK_H) / 2) : WM_BAR_H + 2;
    fb_shadow_soft(x, y, pw, ph);
    fb_round_blend(x, y, pw, ph, 12, 0xF814141C, 250);
    fb_rect_outline(x, y, pw, ph, RGX_HIGHLIGHT);
    for (unsigned i = 0; i < sizeof(HELP_LINES) / sizeof(HELP_LINES[0]); i++)
        fb_text(x + 18, y + 16 + i * 20, HELP_LINES[i],
                i == 0 ? RGX_HIGHLIGHT : RGX_FG, 0);
}

#define SM_ROW_H 24
#define SM_LEFT_W 36   /* Win7-style left blue strip */
static int start_windows(void) {   /* open windows across all workspaces */
    int n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_win[i].used && g_win[i].ws == g_ws) n++;
    return n;
}

static void start_menu_box(int *x, int *y, int *w, int *h) {
    *w = 260;
    *h = SM_COUNT * SM_ROW_H + 48 + start_windows() * SM_ROW_H;
    *x = g_screen_w - *w - 6;        /* drops from the top-right button */
    *y = WM_BAR_H + 2;
}

static void draw_start_menu(void) {
    if (!g_start_open) return;
    int x, y, w, h; start_menu_box(&x, &y, &w, &h);

    fb_shadow_soft((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
    fb_glass_panel((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, 8,
                   0x00101014, 210, true);
    fb_gradient_rect(x + 2, y + 2, SM_LEFT_W, h - 4, 0x00282830, 0x00101014);
    fb_text_trans(x + SM_LEFT_W + 10, y + 8, "EchOS 1.0", 0x00F0F0F4);
    int list_y = y + 32;
    for (int i = 0; i < SM_COUNT; i++) {
        int ry = list_y + i * SM_ROW_H;
        if (sm_rule(i))
            fb_fill_rect(x + SM_LEFT_W + 8, ry - 2, w - SM_LEFT_W - 16, 1, 0x00404048);
        bool dead = (i == SM_OFF) && !power_soft_off_available();
        uint32_t fg = dead ? 0x00606068 : 0x00E8E8EA;
        fb_text_trans(x + SM_LEFT_W + 12, ry + 4, SM_LABEL[i], fg);
    }
    /* Open windows (multitask list): title + workspace. */
    int wy = list_y + SM_COUNT * SM_ROW_H + 8;
    fb_text_trans(x + SM_LEFT_W + 10, wy, "VENTANAS", 0x00909098);
    wy += SM_ROW_H;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        wm_window *w = &g_win[i];
        if (!w->used || w->ws != g_ws) continue;
        char row[64]; char *d = row, *e = row + sizeof(row);
        d = sapp(d, w->title, e);
        d = sapp(d, "  ·  ws ", e);
        napp(d, (uint64_t)(w->ws + 1), e);
        bool hot = (w->id == g_focus);
        fb_text_trans(x + SM_LEFT_W + 12, wy + 4, row,
                      hot ? 0x00FFFFFF : 0x00C0C0C8);
        wy += SM_ROW_H;
    }
}

static int start_item_at(int px, int py) {
    if (!g_start_open) return -1;
    int x, y, w, h; start_menu_box(&x, &y, &w, &h);
    int list_y = y + 32;
    if (px < x + SM_LEFT_W || px >= x + w || py < list_y || py >= y + h) return -1;
    int i = (py - list_y) / SM_ROW_H;
    if (i >= 0 && i < SM_COUNT) return i;
    /* window rows: 1000 + window index */
    int row = i - SM_COUNT - 1;
    if (row >= 0) {
        int seen = 0;
        for (int j = 0; j < WM_MAX_WINDOWS; j++) {
            if (g_win[j].used && g_win[j].ws == g_ws) {
                if (seen == row) return 1000 + j;
                seen++;
            }
        }
    }
    return -1;
}

/* Clock tick used to dirty the whole desktop. Typing then waited on a
   full compose every second — that is the lag when you write slowly. */
static void paint_clock_only(void) {
    char clk[12];
    clock_fmt_time(clk, sizeof(clk));
    if (g_clock_x < 0) return;
    fb_cursor_hide();
    fb_damage_reset();
    fb_text_trans((uint32_t)g_clock_x, 4, clk, 0x00E8E8EA);
    fb_cursor_show();
    fb_present();
}

/* ------------------------------- compositor ------------------------------ */
static void draw_window(wm_window *w) {
    bool focus = (w->id == g_focus);

    fb_shadow_soft((uint32_t)w->x, (uint32_t)w->y, (uint32_t)w->w, (uint32_t)w->h);
    fb_glass_panel((uint32_t)w->x, (uint32_t)w->y, (uint32_t)w->w, (uint32_t)w->h,
                   WM_CORNER, focus ? 0x0018181C : 0x00101014,
                   focus ? 210 : 180, focus);

    fb_aero_titlebar((uint32_t)(w->x + 1), (uint32_t)(w->y + 1),
                     (uint32_t)(w->w - 2), WM_TITLE_H - 1, focus);

    uint32_t tfg = focus ? 0x00F4F4F6 : 0x00A0A0A8;
    fb_text_trans((uint32_t)(w->x + 10), (uint32_t)(w->y + 6), w->title, tfg);
    if (w->maximized)
        fb_text_trans((uint32_t)(w->x + w->w - 78), (uint32_t)(w->y + 6), "[]", tfg);
    else if (w->floating)
        fb_text_trans((uint32_t)(w->x + w->w - 78), (uint32_t)(w->y + 6), "~", tfg);

    fb_aero_caption((uint32_t)w->x, (uint32_t)w->y, (uint32_t)w->w,
                    WM_TITLE_H, focus, true);

    if (w->floating && !w->maximized) {
        int hx = w->x + w->w - 10, hy = w->y + w->h - 10;
        fb_fill_rect((uint32_t)hx, (uint32_t)(hy + 6), 8, 2, RGX_DIM);
        fb_fill_rect((uint32_t)(hx + 3), (uint32_t)(hy + 3), 5, 2, RGX_DIM);
        fb_fill_rect((uint32_t)(hx + 6), (uint32_t)hy, 2, 2, RGX_DIM);
    }

    int cx = w->x + 2, cy = w->y + WM_TITLE_H, cw = w->w - 4, chh = w->h - WM_TITLE_H - 2;
    if (cw < 1) cw = 1;
    if (chh < 1) chh = 1;
    if (w->kind == WK_TERMINAL)
        fb_fill_rect((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)chh, 0x00080A0Cu);
    else
        fb_blend_rect((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)chh, RGX_CLIENT, 220);

    if (w->resizing) {
        int handle_size = 8;
        fb_fill_rect((uint32_t)(w->x + w->w - handle_size),
                     (uint32_t)(w->y + w->h - handle_size),
                     (uint32_t)handle_size, (uint32_t)handle_size, RGX_NAV_SEL);
    }

    switch (w->kind) {
    case WK_FILES:    draw_files(w, cx, cy, cw, chh); break;
    case WK_MONITOR:  draw_monitor(w, cx, cy, cw, chh); break;
    case WK_ABOUT:    draw_about(w, cx, cy, cw, chh); break;
    case WK_NEURO:    draw_neuro(w, cx, cy, cw, chh); break;
    case WK_CALC:     draw_calc(w, cx, cy, cw, chh); break;
    case WK_DISKS:    draw_disks(w, cx, cy, cw, chh); break;
    case WK_PHOTOS:   draw_photos(w, cx, cy, cw, chh); break;
    case WK_SETTINGS: draw_settings(w, cx, cy, cw, chh); break;
    case WK_BROWSER:  draw_browser(w, cx, cy, cw, chh); break;
    case WK_TERMINAL: draw_terminal(w, cx, cy, cw, chh); break;
    }
}

static void draw_bar(void) {
    /* Slim top bar: Aero glass strip with workspace tags (not a second taskbar) */
    fb_aero_taskbar(0, 0, (uint32_t)g_screen_w, WM_BAR_H);
    int x = 8;
    for (int i = 0; i < WM_WORKSPACES; i++) {
        bool act = (i == g_ws);
        int tw = (int)k_strlen(WS_NAME[i]) * 8 + 12;
        if (act)
            fb_aero_button((uint32_t)(x - 2), 2, (uint32_t)tw, WM_BAR_H - 4, true, false);
        fb_text_trans((uint32_t)(x + 4), 4, WS_NAME[i],
                      act ? 0x00FFFFFF : 0x00B0B0B8);
        g_tag_x[i] = x - 2; g_tag_w[i] = tw;
        x += tw + 6;
    }
    wm_window *f = win_by_id(g_focus);
    if (f)
        fb_text_trans((uint32_t)(g_screen_w / 2 - (int)k_strlen(f->title) * 4),
                      4, f->title, 0x00FFFFFF);
    char t[96], *d = t, *e = t + sizeof(t);
    d = sapp(d, "up ", e); d = napp(d, arch_timer_uptime_ms()/1000, e); d = sapp(d, "s  heap ", e);
    d = napp(d, heap_used()/1024, e); d = sapp(d, "K  ram ", e);
    d = napp(d, pmm_used_bytes()/(1024*1024), e); d = sapp(d, "M  net ", e);
    d = sapp(d, net_present() ? "up" : "--", e);
    int tx = g_screen_w - (int)k_strlen(t) * 8 - 8;
    fb_text_trans((uint32_t)tx, 4, t, 0x00E0F0FF);

    /* Wall clock, then the Start button — top-right, GNOME style. */
    {
        char clk[12];
        clock_fmt_time(clk, sizeof(clk));
        int cw = (int)k_strlen(clk) * 8 + 16;
        int cx = tx - cw - 12;
        if (cx < 0) cx = 0;
        fb_text_trans((uint32_t)cx, 4, clk, 0x00E8E8EA);
        g_clock_x = cx; g_clock_w = cw;

        int sx = cx - START_W - 10;
        if (sx < 0) sx = 0;
        fb_aero_button((uint32_t)sx, 2, START_W, WM_BAR_H - 4, g_start_open, true);
        fb_text_trans((uint32_t)(sx + 12), 4, "Start", 0x00101014);
        g_start_x = sx;
    }
}

/* Damage a window-sized box in screen coordinates. Window x/y are signed and a
   dragged window may hang off the left or top edge, so clamp before handing
   the rectangle to the unsigned damage API — a negative coordinate cast to
   uint32 would look like a huge one and get silently discarded. */
static void damage_box(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    fb_damage((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}

/* The scene cache excludes exactly one window, the one being dragged. -1 when
   there is no valid cache. Every full compose invalidates it, because after a
   full compose the cache would contain the dragged window baked into the
   background and restoring from it would smear a copy across the desktop. */
static int g_scene_for = -1;

static bool visible(const wm_window *w) {
    return w->used && w->ws == g_ws && !w->hidden;
}

static void wm_draw(void) {
    g_scene_for = -1;
    gui_background(0, 0, g_screen_w, g_screen_h);
    draw_icons();
    /* tiled windows first, floating on top */
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (visible(&g_win[i]) && !g_win[i].floating && g_win[i].id != g_focus) draw_window(&g_win[i]);
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (visible(&g_win[i]) && g_win[i].floating && g_win[i].id != g_focus) draw_window(&g_win[i]);
    wm_window *f = win_by_id(g_focus);              /* focused window on top  */
    if (f && visible(f)) draw_window(f);
    draw_bar();

    /* Eclipse Shell dock (macOS-style) — magnification tracks the cursor. */
    {
        uint32_t mx, my; fb_cursor_get(&mx, &my);
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            wm_window *w = &g_win[i];
            if (!w->used || w->ws != g_ws) continue;
            if (w->kind == WK_FILES)         dock_set_running("Files", true);
            else if (w->kind == WK_TERMINAL) dock_set_running("Terminal", true);
            else if (w->kind == WK_SETTINGS) dock_set_running("Settings", true);
        }
        dock_layout((uint32_t)g_screen_w, (uint32_t)g_screen_h, mx, my);
        dock_render();
    }

    /* Rubber-band selection rectangle (translucent, like every desktop). */
    if (g_marquee) {
        uint32_t mx0 = g_marquee_x0 < g_marquee_x1 ? g_marquee_x0 : g_marquee_x1;
        uint32_t my0 = g_marquee_y0 < g_marquee_y1 ? g_marquee_y0 : g_marquee_y1;
        uint32_t mw = g_marquee_x0 < g_marquee_x1 ? g_marquee_x1 - g_marquee_x0
                                                  : g_marquee_x0 - g_marquee_x1;
        uint32_t mh = g_marquee_y0 < g_marquee_y1 ? g_marquee_y1 - g_marquee_y0
                                                  : g_marquee_y0 - g_marquee_y1;
        fb_blend_rect(mx0, my0, mw, mh, 0xFF9D45, 70);   /* translucent band */
        fb_rect_outline(mx0, my0, mw, mh, RGX_HIGHLIGHT);
        for (int j = 0; j < ICON_COUNT; j++) {
            if (!g_icon_selected[j] || g_icon_removed[j]) continue;
            int ix, iy; icon_box(j, &ix, &iy);
            fb_rect_outline((uint32_t)ix + 4, (uint32_t)iy - 6,
                            (uint32_t)ICON_W, (uint32_t)ICON_H + 8, RGX_HIGHLIGHT);
        }
    }

    /* Dragged desktop icon ghost follows the pointer. */
    if (g_icon_drag >= 0 && g_icon_moving) {
        uint32_t mx, my; fb_cursor_get(&mx, &my);
        int lw = (int)k_strlen(ICONS[g_icon_drag].label) * 8 + 16;
        fb_round_blend(mx - (uint32_t)lw / 2, my - 34, (uint32_t)lw, 20, 4,
                       0x00000000, 170);
        fb_text_trans(mx - (uint32_t)lw / 2 + 8, my - 30,
                      ICONS[g_icon_drag].label, 0x00E8E8EA);
    }

    draw_start_menu();
    draw_help_panel();
    draw_context_menu();  /* Context menu on top of everything */
}

static void draw_disks(wm_window *win, int cx, int cy, int cw, int ch) {
    (void)cw;
    fb_text(cx + 10, cy + 8, "Disks  (cfdisk / reinstall)", RGX_FG, RGX_PANEL);
    fb_text(cx + 10, cy + 26, "Click a row. Enter in Terminal:", RGX_DIM, RGX_PANEL);
    fb_text(cx + 10, cy + 42, "  reinstall <disk>     or   cfdisk <disk>", RGX_DIM, RGX_PANEL);
    int n = disks_gui_count();
    int y = cy + 66;
    for (int i = 0; i < n && y < cy + ch - 20; i++) {
        bool sel = (win->sel == i);
        if (sel) fb_fill_rect(cx + 8, y - 2, (uint32_t)(cw - 16), 18, RGX_NAV_SEL);
        fb_text(cx + 12, y, disks_gui_line(i), sel ? RGX_WHITE : RGX_FG,
                sel ? RGX_NAV_SEL : RGX_PANEL);
        y += 20;
    }
    if (!n) fb_text(cx + 12, y, "(no disks — amnesic session)", RGX_DIM, RGX_PANEL);
}

static uint32_t *g_photo;
static uint32_t  g_photo_w, g_photo_h;
static int       g_photo_idx = -1;

static void photo_load(int idx) {
    int n = wp_count();
    if (n <= 0) return;
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    if (idx == g_photo_idx && g_photo) return;
    uint32_t len = 0;
    const uint8_t *data = wp_data_at(idx, &len);
    img_info_t inf;
    if (!data || !img_info(data, len, &inf)) return;
    if (!g_photo || g_photo_w != inf.width || g_photo_h != inf.height) {
        uint64_t bytes = (uint64_t)inf.width * inf.height * 4;
        uint32_t pages = (uint32_t)((bytes + 4095) / 4096);
        uint64_t p = pmm_alloc_contig(pages);
        if (!p) return;
        g_photo = (uint32_t *)(uintptr_t)p;
        g_photo_w = inf.width; g_photo_h = inf.height;
    }
    if (img_decode(data, len, g_photo, inf.width * inf.height, &inf) == 0) return;
    g_photo_idx = idx;
}

/* File mode: the viewer shows an arbitrary image from the vault instead of
   the wallpaper pack (Explorer 'Open' routes here). */
static char g_photo_path[RXFS_NAME];

static bool photo_load_path(const char *path) {
    uint32_t len = 0;
    const char *data = vfs_read(path, &len);
    img_info_t inf;
    if (!data || len < 8 || !img_info((const uint8_t *)data, len, &inf)) return false;
    if (!g_photo || g_photo_w != inf.width || g_photo_h != inf.height) {
        uint64_t bytes = (uint64_t)inf.width * inf.height * 4;
        uint32_t pages = (uint32_t)((bytes + 4095) / 4096);
        uint64_t q = pmm_alloc_contig(pages);
        if (!q) return false;
        g_photo = (uint32_t *)(uintptr_t)q;
        g_photo_w = inf.width; g_photo_h = inf.height;
    }
    if (img_decode((const uint8_t *)data, len, g_photo, inf.width * inf.height, &inf)
        != inf.width * inf.height) return false;
    size_t n = 0;
    while (path[n] && n < sizeof(g_photo_path) - 1) { g_photo_path[n] = path[n]; n++; }
    g_photo_path[n] = '\0';
    g_photo_idx = -1;                    /* pack mode off */
    return true;
}

static void draw_photos(wm_window *win, int cx, int cy, int cw, int ch) {
    int n = wp_count();
    char line[72];
    int iy, ih;
    if (win->sel < 0) win->sel = 0;
    if (n && win->sel >= n) win->sel = n - 1;
    if (g_photo_path[0]) {
        /* file mode: an image opened from the Explorer */
        { char *d = line, *e = line + sizeof(line);
          d = sapp(d, "Multimedia  ", e); d = sapp(d, g_photo_path, e); }
    } else {
        photo_load(win->sel);
        { char *d = line, *e = line + sizeof(line);
          d = sapp(d, "Multimedia  ", e);
          d = sapp(d, (n && win->sel < n) ? wp_name_at(win->sel) : "(none)", e);
          d = sapp(d, "  ", e);
          d = napp(d, (uint64_t)(n ? win->sel + 1 : 0), e);
          d = sapp(d, "/", e);
          napp(d, (uint64_t)n, e); }
    }
    fb_text(cx + 10, cy + 6, line, RGX_FG, RGX_CLIENT);
    fb_text(cx + 10, cy + 22, "Ver no cambia el fondo. Usa el boton o Enter.", RGX_DIM, RGX_CLIENT);
    fb_round_blend((uint32_t)(cx + 10), (uint32_t)(cy + 40), 56, 22, 4, 0x00282830, 200);
    fb_text_trans((uint32_t)(cx + 22), (uint32_t)(cy + 44), "< prev", 0x00E8E8EA);
    fb_round_blend((uint32_t)(cx + 74), (uint32_t)(cy + 40), 56, 22, 4, 0x00282830, 200);
    fb_text_trans((uint32_t)(cx + 86), (uint32_t)(cy + 44), "next >", 0x00E8E8EA);
    fb_round_blend((uint32_t)(cx + 140), (uint32_t)(cy + 40), 144, 22, 4, 0x00383840, 210);
    fb_text_trans((uint32_t)(cx + 148), (uint32_t)(cy + 44), "Usar como fondo", 0x00E8E8EA);
    iy = cy + 70; ih = ch - 78;
    if (ih > 20 && g_photo)
        gui_blit(g_photo, g_photo_w, g_photo_h, cx + 8, iy, cw - 16, ih);
}

static int photos_hit(int cx, int cy, int px, int py) {
    if (py < cy + 40 || py >= cy + 62) return 0;
    if (px >= cx + 10 && px < cx + 66) return 1;   /* prev */
    if (px >= cx + 74 && px < cx + 130) return 2;  /* next */
    if (px >= cx + 140 && px < cx + 284) return 3; /* set wallpaper */
    return 0;
}

/* ------------------------------ Nova browser ------------------------------
 * GUI front-end over the wired browse stack: URL bar + page text. Honest
 * scope: rgx:// pages and plain http:// URLs today; https:// needs the TLS
 * body transfer that is still staged (browse says so itself).
 * ------------------------------------------------------------------------ */
#define NOVA_TEXT_MAX 3072
/* Per-instance browser state — multitasking: several Novas coexist. */
typedef struct {
    char url[128];
    char text[NOVA_TEXT_MAX];
    char entry[128];
    bool editing;
    bool used;
} nova_inst;
static nova_inst g_nova_st[WM_MAX_WINDOWS];

static nova_inst *nova_st(int win_id) {
    if (win_id < 0 || win_id >= WM_MAX_WINDOWS) win_id = 0;
    return &g_nova_st[win_id];
}

/* Browser history: back / forward / reload — like every browser. */
#define NOVA_HIST_MAX 24
static char g_nova_hist[NOVA_HIST_MAX][128];
static int  g_nova_hist_n;
static int  g_nova_hist_cur;
static int  g_nova_btn_back_x, g_nova_btn_fwd_x, g_nova_btn_rel_x, g_nova_btn_y, g_nova_btn_sz;

static void nova_load(int win_id, const char *url, bool push_hist);

bool wm_active(void) { return g_started && fb_active(); }

void wm_open_nova(const char *url)
{
    if (!url || !url[0]) url = "https://www.rogexlaboratories.com/";
    int id = spawn(WK_BROWSER, g_ws, true);
    if (id < 0) return;
    wm_window *w = win_by_id(id);
    if (w && w->ws != g_ws) w->ws = g_ws;
    if (w) {
        w->floating = true;
        if (!w->maximized) {
            w->rest_x = w->x; w->rest_y = w->y;
            w->rest_w = w->w; w->rest_h = w->h;
            w->x = 0; w->y = WM_BAR_H;
            w->w = g_screen_w;
            w->h = g_screen_h - WM_BAR_H - WM_TASK_H;
            if (w->h < WM_TITLE_H + 40) w->h = WM_TITLE_H + 40;
            w->maximized = true;
        }
    }
    nova_inst *st = nova_st(id);
    st->used = true;
    size_t n = 0;
    while (url[n] && n < sizeof(st->url) - 1) { st->url[n] = url[n]; n++; }
    st->url[n] = '\0';
    if (g_nova_hist_n == 0) {
        k_memcpy(g_nova_hist[0], st->url,
                 k_strlen(st->url) + 1 < sizeof(g_nova_hist[0]) ? k_strlen(st->url) + 1 : sizeof(g_nova_hist[0]) - 1);
        g_nova_hist_n = 1; g_nova_hist_cur = 0;
        nova_load(id, st->url, false);
    } else {
        nova_load(id, st->url, true);
    }
    g_focus = id;
    g_dirty = true;
}

static void nova_load(int win_id, const char *url, bool push_hist)
{
    nova_inst *st = nova_st(win_id);
    size_t n = 0;
    if (!url || !url[0]) url = "https://www.rogexlaboratories.com/";
    while (url[n] && n < sizeof(st->url) - 1) { st->url[n] = url[n]; n++; }
    st->url[n] = '\0';
    browse_cmd(st->url, st->text, sizeof(st->text) - 1);
    if (push_hist) {
        if (g_nova_hist_cur + 1 < NOVA_HIST_MAX) {
            g_nova_hist_cur++;
            g_nova_hist_n = g_nova_hist_cur + 1;
        } else if (g_nova_hist_n < NOVA_HIST_MAX) {
            g_nova_hist_n++;
            g_nova_hist_cur = g_nova_hist_n - 1;
        } else {
            for (int j = 1; j < NOVA_HIST_MAX; j++)
                k_memcpy(g_nova_hist[j - 1], g_nova_hist[j], sizeof(g_nova_hist[0]));
            g_nova_hist_cur = NOVA_HIST_MAX - 1;
        }
        k_memcpy(g_nova_hist[g_nova_hist_cur], st->url, sizeof(st->url));
    }
    g_dirty = true;
}

void nova_back(void)
{
    wm_window *nf = win_by_id(g_focus);
    if (!nf || nf->kind != WK_BROWSER) return;
    if (g_nova_hist_cur > 0) { g_nova_hist_cur--; nova_load(nf->id, g_nova_hist[g_nova_hist_cur], false); }
}

void nova_forward(void)
{
    wm_window *nf = win_by_id(g_focus);
    if (!nf || nf->kind != WK_BROWSER) return;
    if (g_nova_hist_cur + 1 < g_nova_hist_n) {
        g_nova_hist_cur++;
        nova_load(nf->id, g_nova_hist[g_nova_hist_cur], false);
    }
}

typedef struct {
    char     text[112];
    uint32_t color;
    uint8_t  size;
    uint8_t  bold;
    uint8_t  align;
} nova_line_pub;

static void draw_browser(wm_window *win, int cx, int cy, int cw, int ch) {
    (void)win;
    fb_fill_rect((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)ch, RGX_CLIENT);
    /* Nav buttons: back / forward / reload — the trio every browser has. */
    {
        uint32_t by2 = (uint32_t)(cy + 8), sz = 24;
        g_nova_btn_sz  = (int)sz;
        g_nova_btn_y   = (int)by2;
        g_nova_btn_back_x = cx + 10;
        g_nova_btn_fwd_x  = cx + 10 + (int)sz + 6;
        g_nova_btn_rel_x  = cx + 10 + 2 * ((int)sz + 6);
        bool canB = g_nova_hist_cur > 0;
        bool canF = g_nova_hist_cur + 1 < g_nova_hist_n;
        for (int b = 0; b < 3; b++) {
            uint32_t bx2 = (uint32_t)(g_nova_btn_back_x + b * ((int)sz + 6));
            fb_round_fill(bx2, by2, sz, sz, 6, 0xFF141A22);
            fb_rect_outline(bx2, by2, sz, sz,
                            (b == 0 && !canB) ? RGX_BORDER
                            : (b == 1 && !canF) ? RGX_BORDER : RGX_HIGHLIGHT);
            const char *lbl = b == 0 ? "<" : b == 1 ? ">" : "R";
            fb_text(bx2 + 9, by2 + 5, lbl, 0xFFE8E8F0, 0);
        }
    }
    /* URL bar */
    uint32_t bx = (uint32_t)(g_nova_btn_rel_x + g_nova_btn_sz + 8), by = (uint32_t)(cy + 8);
    uint32_t bw = (uint32_t)(cx + cw - 10 - (int)bx), bh = 24;
    if ((int)bw < 120) bw = 120;
    fb_round_fill(bx, by, bw, bh, 6, 0xFF0C1016);
    fb_rect_outline(bx, by, bw, bh, RGX_BORDER);
    fb_text(bx + 8, by + 5, "Nova", RGX_HIGHLIGHT, 0);
    {
        nova_inst *st = nova_st(win->id);
        const char *shown = st->editing ? st->entry
                            : (st->url[0] ? st->url : "https://www.rogexlaboratories.com/");
        fb_text(bx + 46, by + 5, shown, 0xFFE8E8F0, 0);
        if (st->editing) {
            uint32_t tw = (uint32_t)k_strlen(shown) * FB_GLYPH_W;
            fb_fill_rect(bx + 46 + tw, by + 4, 8, 16, RGX_HIGHLIGHT);
        }
        fb_text_trans(bx + bw - 88, by + 5, "type a URL, Enter", RGX_DIM);
    }

    /* Page body: styled HTML render (CSS subset) when available; plain text
       fallback for rgx:// local pages. */
    int y = cy + bh + 22;
    if (browse_line_count() > 0) {
        const nova_line_pub *L = (const nova_line_pub *)browse_lines();
        int n = browse_line_count();
        for (int i = 0; i < n && y < cy + ch - 12; i++) {
            if (!L[i].text[0]) { y += 10; continue; }
            int lh = L[i].size == 2 ? 30 : L[i].size == 1 ? 26 : 18;
            uint32_t color = L[i].color;
            int tx = cx + 12;
            int maxc = (cw - 24) / FB_GLYPH_W;
            int len = 0; while (L[i].text[len]) len++;
            if (L[i].align == 1) tx = cx + (cw - len * FB_GLYPH_W) / 2;
            else if (L[i].align == 2) tx = cx + cw - 12 - len * FB_GLYPH_W;
            if (tx < cx + 12) tx = cx + 12;
            if (len > maxc) len = maxc;
            char line[112];
            k_memcpy(line, L[i].text, (size_t)len); line[len] = '\0';
            if (L[i].bold) fb_text((uint32_t)(tx + 1), (uint32_t)y, line, 0xFF000000u | (color & 0xFFFFFF), RGX_CLIENT);
            fb_text((uint32_t)tx, (uint32_t)y, line, color, RGX_CLIENT);
            y += lh;
        }
    } else {
        const char *p = nova_st(win->id)->text;
        while (*p && y < cy + ch - 12) {
            char line[110];
            int n = 0;
            while (*p && *p != '\n' && n < (int)sizeof(line) - 1
                   && n < (cw - 24) / FB_GLYPH_W) line[n++] = *p++;
            line[n] = '\0';
            if (*p == '\n') p++;
            fb_text((uint32_t)(cx + 12), (uint32_t)y, line, RGX_FG, RGX_CLIENT);
            y += 18;
        }
    }
}

static void draw_settings(wm_window *win, int cx, int cy, int cw, int ch) {
    int y = cy + 10, i, n;
    char b[80], *d, *e;
    (void)win; (void)cw; (void)ch;
    fb_text(cx + 10, y, "Ajustes  ·  EchOS 1.0 ECLIPSE", RGX_FG, RGX_CLIENT); y += 22;
    fb_text(cx + 10, y, "Tema (click):", RGX_DIM, RGX_CLIENT); y += 20;
    for (i = 0; i < RGX_THEME_COUNT; i++) {
        int bx = cx + 10 + i * 72;
        bool on = (fb_theme() == i);
        fb_round_blend((uint32_t)bx, (uint32_t)y, 66, 22, 4,
                       on ? 0x00484850 : 0x00202026, 210);
        fb_text_trans((uint32_t)(bx + 8), (uint32_t)(y + 4), fb_theme() == i
                      ? (i==0?"smoke*": i==1?"green*": i==2?"amber*": i==3?"ice*":"mono*")
                      : (i==0?"smoke": i==1?"green": i==2?"amber": i==3?"ice":"mono"),
                      0x00E8E8EA);
    }
    y += 36;
    fb_text(cx + 10, y, "Fondo (click aplica):", RGX_DIM, RGX_CLIENT); y += 20;
    n = wp_count();
    for (i = 0; i < n && i < 12; i++) {
        int row = i / 6, col = i % 6;
        int bx = cx + 10 + col * 88;
        int by2 = y + row * 28;
        bool on = (gui_wallpaper_index() == i);
        fb_round_blend((uint32_t)bx, (uint32_t)by2, 80, 22, 4,
                       on ? 0x00484850 : 0x00202026, 210);
        fb_text_trans((uint32_t)(bx + 8), (uint32_t)(by2 + 4), wp_name_at(i), 0x00E8E8EA);
    }
    y += 28 * ((n > 0 ? (n < 12 ? n : 12) : 1) - 1) / 6 * 28 + 36;
    d = b; e = b + sizeof(b);
    d = sapp(d, "www ", e);
    sapp(d, www_ready() ? "on" : "off", e);
    fb_text(cx + 10, y, b, RGX_FG, RGX_CLIENT); y += 20;
    d = b; e = b + sizeof(b);
    d = sapp(d, "net ", e); d = sapp(d, net_present() ? "up" : "--", e);
    d = sapp(d, "   theme=", e); sapp(d, fb_theme_name(), e);
    fb_text(cx + 10, y, b, RGX_DIM, RGX_CLIENT); y += 22;
    fb_text(cx + 10, y, "Escritorio: click derecho = Ajustes / fondo.", RGX_DIM, RGX_CLIENT);
}

static int settings_hit(int cx, int cy, int px, int py) {
    int i, n;
    if (py >= cy + 52 && py < cy + 74) {
        for (i = 0; i < RGX_THEME_COUNT; i++) {
            int bx = cx + 10 + i * 72;
            if (px >= bx && px < bx + 66) return 10 + i;
        }
    }
    {
        n = wp_count();
        if (n > 12) n = 12;
        for (i = 0; i < n; i++) {
            int row = i / 6, col = i % 6;
            int bx = cx + 10 + col * 88;
            int by2 = cy + 108 + row * 28;
            if (py >= by2 && py < by2 + 22 && px >= bx && px < bx + 80) return 20 + i;
        }
    }
    return 0;
}

/* Freeze the desktop with `id` taken out of it, so a drag can repair the
   pixels that window uncovers by copying them back instead of recomposing
   the whole scene for every mouse report. Costs one full compose plus one
   screen-sized copy, once per drag, and saves both on every frame after. */
static void scene_prepare(int id) {
    wm_window *w = win_by_id(id);
    if (!w || !gui_scene_ready()) { g_scene_for = -1; return; }
    fb_cursor_hide();
    w->hidden = true;
    wm_draw();                       /* everything except the dragged window */
    w->hidden = false;
    bool ok = gui_scene_capture();
    draw_window(w);                  /* put it back for the frame we present */
    fb_cursor_show();
    fb_damage_all();
    fb_present();
    g_scene_for = ok ? id : -1;
}

/* ------------------------------ window ops ------------------------------- */
static int spawn(wm_kind kind, int ws, bool floating) {
    /* The terminal and the neuron are singletons: both are one piece of live
       state (a scrollback, an actor) that a second window would draw twice
       while stealing the first window's identity. Hand back the existing one
       instead of duplicating it. */
    if (kind == WK_NEURO) { wm_window *n = win_by_id(g_neuro_win); if (n) return n->id; }
    if (kind == WK_CALC)  { wm_window *n = win_by_id(g_calc_win);  if (n) return n->id; }

    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (!g_win[i].used) {
        wm_window *w = &g_win[i];
        w->used = true; w->id = g_next_id++; w->ws = ws; w->kind = kind;
        w->floating = floating; w->sel = 0; w->hidden = false;
        w->maximized = false;
        w->rest_x = w->rest_y = w->rest_w = w->rest_h = 0;
        w->resizing = false; w->resize_edge = 0; w->orig_w = 0; w->orig_h = 0;
        w->selecting = false; w->sel_start_x = 0; w->sel_start_y = 0;
        w->sel_end_x = 0; w->sel_end_y = 0;
        w->grad_top = RGX_GRAD_5;
        w->grad_bottom = RGX_GRAD_1;
        
        const char *nm = kind == WK_FILES    ? "Explorer"
                       : kind == WK_MONITOR  ? "System Monitor"
                       : kind == WK_NEURO    ? "Neuro"
                       : kind == WK_CALC     ? "Calculator"
                       : kind == WK_DISKS    ? "Disks"
                       : kind == WK_PHOTOS   ? "Multimedia"
                       : kind == WK_SETTINGS ? "Ajustes"
                       : kind == WK_BROWSER  ? "Nova"
                       : kind == WK_TERMINAL ? "Terminal" : "About";
        if (kind == WK_FILES) { fm_go(g_fm_path[0] ? g_fm_path : "/"); }
        { char *d = w->title, *ee = w->title + sizeof(w->title); sapp(d, nm, ee); }
        if (floating) {
            /* Cascade from the top-left of the free desktop, clear of the
               icon column, and clamp so a window never opens off-screen on a
               small panel. */
            int step = (g_next_id % 6) * 26;
            w->w = 460; w->h = 320;
            if (kind == WK_CALC) { w->w = 300; w->h = 380; }
            if (kind == WK_BROWSER) { w->w = 620; w->h = 430; }
            if (w->w > g_screen_w - 40) w->w = g_screen_w - 40;
            if (w->h > g_screen_h - WM_BAR_H - WM_TASK_H - 20) w->h = g_screen_h - WM_BAR_H - WM_TASK_H - 20;
            w->x = ICON_X + ICON_W + 24 + step;
            w->y = WM_BAR_H + 20 + step;
            if (w->x + w->w > g_screen_w - 8) w->x = g_screen_w - 8 - w->w;
            if (w->y + w->h > g_screen_h - WM_TASK_H - 8) w->y = g_screen_h - WM_TASK_H - 8 - w->h;
            if (w->x < 8) w->x = 8;
            /* Apps open fullscreen (maximized) by default — Calculator keeps
               its phone-sized cascade window. */
            if (kind != WK_CALC) {
                w->rest_x = w->x; w->rest_y = w->y;
                w->rest_w = w->w; w->rest_h = w->h;
                w->x = 0; w->y = WM_BAR_H;
                w->w = g_screen_w;
                w->h = g_screen_h - WM_BAR_H - WM_TASK_H;
                if (w->h < WM_TITLE_H + 40) w->h = WM_TITLE_H + 40;
                w->maximized = true;
            }
            if (w->y < WM_BAR_H + 4) w->y = WM_BAR_H + 4;
        }
        if (kind == WK_TERMINAL) g_term_win = w->id;
        if (kind == WK_NEURO) { g_neuro_win = w->id; neuro_reset(); }
        if (kind == WK_CALC)  { g_calc_win  = w->id; calc_reset(); }
        return w->id;
    }
    return -1;
}

static void focus_first(void) {
    g_focus = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_win[i].used && g_win[i].ws == g_ws) { g_focus = g_win[i].id; break; }
}

static void cycle_focus(void) __attribute__((unused));
static void cycle_focus(void) {
    int ids[WM_MAX_WINDOWS], n = 0, cur = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (g_win[i].used && g_win[i].ws == g_ws) { if (g_win[i].id == g_focus) cur = n; ids[n++] = g_win[i].id; }
    if (n == 0) { g_focus = -1; return; }
    g_focus = ids[(cur + 1) % n];
}

static void close_focused(void) {
    wm_window *f = win_by_id(g_focus);
    if (!f) return;
    if (f->id == g_term_win)  g_term_win  = -1;
    if (f->id == g_neuro_win) g_neuro_win = -1;
    if (f->id == g_calc_win)  g_calc_win  = -1;
    f->used = false;
    retile();
    focus_first();
}

/* `t` opens the terminal. One window (split lives *inside* it), so a second
   spawn focuses the existing session instead of drawing the same ROSH twice.

   It opens floating and generously sized rather than tiled: shell output is
   line-oriented and a stacked tile is only ~40 columns wide, which truncates
   most of what `status` or `devices` print. Floating also means it can be
   dragged out of the way without disturbing the layout underneath. */
static void open_terminal(void) {
    wm_window *t = win_by_id(g_term_win);
    if (t) { if (t->ws != g_ws) t->ws = g_ws; g_focus = t->id; retile(); return; }
    term_reset();
    int id = spawn(WK_TERMINAL, g_ws, true);
    wm_window *w = win_by_id(id);
    if (!w) return;
    if (!w->maximized) {                 /* spawn already opened it fullscreen */
        int avail_h = g_screen_h - WM_BAR_H - WM_TASK_H - 2 * WM_GAP;
        w->w = g_screen_w - 2 * WM_GAP > 860 ? 860 : g_screen_w - 2 * WM_GAP;
        w->h = avail_h > 460 ? 460 : avail_h;
        w->x = (g_screen_w - w->w) / 2;
        w->y = WM_BAR_H + (avail_h - w->h) / 2 + WM_GAP;
    }
    g_focus = id;
}

/* `neuro` from the terminal, the `y` key, or the desktop icon. Wide, because
   the membrane trace is the point and a narrow window shows a few seconds of
   it. */
static void open_neuro(void) {
    wm_window *n = win_by_id(g_neuro_win);
    if (n) { if (n->ws != g_ws) n->ws = g_ws; g_focus = n->id; return; }
    int id = spawn(WK_NEURO, g_ws, true);
    wm_window *w = win_by_id(id);
    if (!w) return;
    if (!w->maximized) {                 /* respect the fullscreen default */
        int maxw = g_screen_w - ICON_X - ICON_W - 48;
        w->w = maxw > 700 ? 700 : maxw;
        w->h = 380;
    }
    g_focus = id;
}

/* Open a window of `kind`, or focus the one that already exists. This is what
   a desktop icon, a Start-menu row and the keyboard shortcuts all go through,
   so the three cannot disagree about what "open the Calculator" means. */
static void open_kind(wm_kind kind) {
    if (kind == WK_TERMINAL) { open_terminal(); return; }
    if (kind == WK_NEURO)    { open_neuro();    return; }
    int id = spawn(kind, g_ws, true);
    if (id < 0) return;
    wm_window *w = win_by_id(id);
    if (w && w->ws != g_ws) w->ws = g_ws;
    g_focus = id;
}

/* Topmost window on the current workspace at (px,py): floating first (they
   are drawn on top), focused floating highest. Returns id or -1. */
static int window_at(int px, int py) {
    int hit = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        wm_window *w = &g_win[i];
        if (!w->used || w->ws != g_ws) continue;
        if (px >= w->x && px < w->x + w->w && py >= w->y && py < w->y + w->h) {
            if (hit < 0) hit = w->id;
            if (w->floating) hit = w->id;            /* floating wins        */
            if (w->id == g_focus) return w->id;      /* focused wins outright */
        }
    }
    return hit;
}

/* ------------------------------- event loop ------------------------------ */
static void start_action(int item) {
    if (item >= 1000) {
        wm_window *w = &g_win[item - 1000];
        if (w->used) { g_ws = w->ws; retile(); focus_first(); g_focus = w->id; }
        g_start_open = false;
        g_dirty = true;
        return;
    }
    g_start_open = false;
    switch (item) {
    case SM_SETTINGS: open_kind(WK_SETTINGS); break;
    case SM_TERMINAL: open_kind(WK_TERMINAL); break;
    case SM_FILES:    open_kind(WK_FILES);    break;
    case SM_MONITOR:  open_kind(WK_MONITOR);  break;
    case SM_NEURO:    open_kind(WK_NEURO);    break;
    case SM_CALC:     open_kind(WK_CALC);     break;
    case SM_DISKS:    open_kind(WK_DISKS);    break;
    case SM_PHOTOS:   open_kind(WK_PHOTOS);   break;
    case SM_ABOUT:    open_kind(WK_ABOUT);    break;
    case SM_SHELL:    g_exit = WM_ACTION_SHELL;  break;
    case SM_REBOOT:   g_exit = WM_ACTION_REBOOT; break;
    case SM_OFF:
        /* Silently doing nothing when the firmware has no usable S5 would
           look like a broken button. The row is already drawn greyed; this
           is the other half of that promise. */
        if (power_soft_off_available()) g_exit = WM_ACTION_OFF;
        break;
    case SM_HALT:     g_exit = WM_ACTION_HALT;   break;
    default: break;
    }
    g_dirty = true;
}

/* Hit-test resize edges/corners.
   0=none, 1=N, 2=S, 3=E, 4=W, 5=NE, 6=NW, 7=SE, 8=SW */
static int hit_test_resize(wm_window *w, int px, int py) {
    if (!w->floating || w->maximized || w->resizing) return 0;
    const int hs = 8;
    bool on_l = px >= w->x && px < w->x + hs;
    bool on_r = px >= w->x + w->w - hs && px < w->x + w->w;
    bool on_t = py >= w->y && py < w->y + hs;
    bool on_b = py >= w->y + w->h - hs && py < w->y + w->h;
    /* Prefer corners so diagonal resize works. */
    if (on_t && on_r) return 5;
    if (on_t && on_l) return 6;
    if (on_b && on_r) return 7;
    if (on_b && on_l) return 8;
    if (on_t) return 1;
    if (on_b) return 2;
    if (on_r) return 3;
    if (on_l) return 4;
    return 0;
}

/* Fill the free desktop (between top bar and taskbar). Double-click title,
   maximize button, or F11. Stores previous geometry for restore. */
static void win_maximize_toggle(wm_window *w) {
    if (!w) return;
    if (w->maximized) {
        w->x = w->rest_x; w->y = w->rest_y;
        w->w = w->rest_w; w->h = w->rest_h;
        w->maximized = false;
        w->floating = true;
    } else {
        if (!w->floating) { w->floating = true; retile(); }
        w->rest_x = w->x; w->rest_y = w->y;
        w->rest_w = w->w; w->rest_h = w->h;
        w->x = 0;
        w->y = WM_BAR_H;
        w->w = g_screen_w;
        w->h = g_screen_h - WM_BAR_H - WM_TASK_H;
        if (w->h < WM_TITLE_H + 40) w->h = WM_TITLE_H + 40;
        w->maximized = true;
    }
    g_dirty = true;
}

/* Caption button hit boxes (must match fb_aero_caption layout). */
static int caption_hit(wm_window *w, int px, int py) {
    /* 0=none, 1=close, 2=max */
    if (py < w->y || py >= w->y + WM_TITLE_H) return 0;
    int bw = 16;
    int close_x = w->x + w->w - bw - 4;
    int max_x = close_x - bw - 2;
    if (px >= close_x && px < close_x + bw) return 1;
    if (px >= max_x && px < max_x + bw) return 2;
    return 0;
}

static void clamp_win_size(wm_window *w) {
    if (w->w < 160) w->w = 160;
    if (w->h < WM_TITLE_H + 60) w->h = WM_TITLE_H + 60;
    if (w->x + w->w > g_screen_w) w->w = g_screen_w - w->x;
    if (w->y + w->h > g_screen_h - WM_TASK_H) w->h = g_screen_h - WM_TASK_H - w->y;
    if (w->x < 0) { w->w += w->x; w->x = 0; }
    if (w->y < WM_BAR_H) { w->h -= (WM_BAR_H - w->y); w->y = WM_BAR_H; }
    if (w->w < 160) w->w = 160;
    if (w->h < WM_TITLE_H + 60) w->h = WM_TITLE_H + 60;
}

/* Right-click context menu — window chrome, File Explorer, or desktop */
static bool g_ctx_open = false;
static int  g_ctx_x = -1, g_ctx_y = -1;
static int  g_ctx_win_id = -1;
static int  g_ctx_mode = 0;   /* 0 = window, 1 = file explorer, 2 = desktop */
#define CTX_WIN_N 6
#define CTX_FM_N  11
#define CTX_DESK_N 5
static const char *CTX_WIN[] = {
    "Copy text", "Paste text", "Select All", "Maximize", "Close Window", "Tile/Float",
    "To ws 2", "To ws 3"
};
static const char *CTX_FM[] = {
    "Open", "Rename", "Copy", "Cut", "Paste here", "To Trash", "Properties",
    "New note", "New folder", "Go up", "Refresh"
};
static const char *CTX_DESK[] = {
    "Ajustes", "Cambiar fondo...", "Siguiente fondo", "Terminal", "Explorer"
};
/* Global right-click targets: dock tiles (mode 3) and desktop icons (mode 4). */
static const char *CTX_DOCK[] = {
    "Open", "Unpin from Dock", "Properties"
};
static const char *CTX_ICON[] = {
    "Open", "Rename", "Delete", "Properties", "Pin to Dock"
};
#define CTX_DOCK_N ((int)(sizeof(CTX_DOCK)/sizeof(CTX_DOCK[0])))
#define CTX_ICON_N ((int)(sizeof(CTX_ICON)/sizeof(CTX_ICON[0])))

static int ctx_count(void) {
    if (g_ctx_mode == 1) return CTX_FM_N;
    if (g_ctx_mode == 2) return CTX_DESK_N;
    if (g_ctx_mode == 3) return CTX_DOCK_N;
    if (g_ctx_mode == 4) return CTX_ICON_N;
    return CTX_WIN_N;
}
static const char *const *ctx_labels(void) {
    if (g_ctx_mode == 1) return CTX_FM;
    if (g_ctx_mode == 2) return CTX_DESK;
    if (g_ctx_mode == 3) return CTX_DOCK;
    if (g_ctx_mode == 4) return CTX_ICON;
    return CTX_WIN;
}

static void draw_context_menu(void) {
    if (!g_ctx_open) return;
    int n = ctx_count();
    int cw = 160, ch = n * 20 + 8;
    int cx = g_ctx_x, cy = g_ctx_y;
    if (cx + cw > g_screen_w) cx = g_screen_w - cw - 4;
    if (cy + ch > g_screen_h) cy = g_screen_h - ch - 4;
    fb_shadow_soft((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)ch);
    fb_glass_panel((uint32_t)cx, (uint32_t)cy, (uint32_t)cw, (uint32_t)ch, 6,
                   0x00141418, 220, false);
    const char *const *L = ctx_labels();
    for (int i = 0; i < n; i++) {
        int y = cy + 4 + i * 20;
        fb_text_trans(cx + 8, y, L[i], 0x00E8E8EA);
    }
}

static int ctx_item_at(int px, int py) {
    if (!g_ctx_open) return -1;
    int n = ctx_count();
    int cw = 160, ch = n * 20 + 8;
    int cx = g_ctx_x, cy = g_ctx_y;
    if (cx + cw > g_screen_w) cx = g_screen_w - cw - 4;
    if (cy + ch > g_screen_h) cy = g_screen_h - ch - 4;
    if (px < cx || px >= cx + cw || py < cy || py >= cy + ch) return -1;
    int idx = (py - cy - 4) / 20;
    return (idx >= 0 && idx < n) ? idx : -1;
}

static void close_context_menu(void) {
    g_ctx_open = false;
    g_ctx_win_id = -1;
    g_dirty = true;
}

static void handle_context_action(int item) {
    wm_window *w;
    /* Mode 3: dock tile. g_ctx_win_id carries the tile index. */
    if (g_ctx_mode == 3) {
        const char *app = dock_app_at(g_ctx_win_id);
        if (app && item == 0) {
            dock_bounce(app);
            if      (dock_name_is(app, "Files"))     open_kind(WK_FILES);
            else if (dock_name_is(app, "Terminal"))  open_kind(WK_TERMINAL);
            else if (dock_name_is(app, "Nova"))      wm_open_nova("rgx://home");
            else if (dock_name_is(app, "Calculator"))open_kind(WK_CALC);
            else if (dock_name_is(app, "Multimedia"))open_kind(WK_PHOTOS);
            else if (dock_name_is(app, "Settings"))  open_kind(WK_SETTINGS);
        } else if (app && item == 1) {
            dock_unpin(app);
        }
        /* item 2 Properties → About */
        if (item == 2) open_kind(WK_ABOUT);
        close_context_menu();
        g_dirty = true;
        return;
    }
    /* Mode 4: desktop icon. g_ctx_win_id carries the icon index. */
    if (g_ctx_mode == 4) {
        int ic = g_ctx_win_id;
        if (ic >= 0 && ic < ICON_COUNT) {
            switch (item) {
            case 0: open_kind(ICONS[ic].kind); break;
            case 1: fm_rename_begin(win_by_id(g_focus)); break;
            case 2: g_icon_removed[ic] = true; break;    /* removed from desktop this session */
            case 3: open_kind(WK_ABOUT); break;
            case 4: dock_pin(ICONS[ic].label); break;
            }
        }
        close_context_menu();
        g_dirty = true;
        return;
    }
    if (g_ctx_mode == 2) {
        switch (item) {
        case 0: open_kind(WK_SETTINGS); break;
        case 1: open_kind(WK_SETTINGS); break;
        case 2: {
            int n = wp_count();
            static int desk_wp;
            if (n > 0) {
                desk_wp = (desk_wp + 1) % n;
                gui_wallpaper_load_index(desk_wp);
            }
            break;
        }
        case 3: open_kind(WK_TERMINAL); break;
        case 4: open_kind(WK_FILES); break;
        }
        close_context_menu();
        g_dirty = true;
        return;
    }
    w = win_by_id(g_ctx_win_id);
    if (!w) { close_context_menu(); return; }
    g_focus = w->id;

    if (g_ctx_mode == 1 && w->kind == WK_FILES) {
        switch (item) {
        case 0: fm_open_sel(w); break;
        case 1: fm_rename_begin(w); break;
        case 2: fm_copy_sel(w, false); break;
        case 3: fm_copy_sel(w, true); break;
        case 4: fm_paste(); break;
        case 5: fm_delete_sel(w); break;      /* moves to /trash (papelera) */
        case 6: fm_new_note(); break;
        case 7: fm_new_folder(); break;
        case 8: fm_properties(w); break;
        case 9: fm_parent(g_fm_path); fm_go(g_fm_path); w->sel = 0; break;
        case 10: fm_refresh(); fm_set_status("refreshed"); break;
        }
        g_dirty = true;
        close_context_menu();
        return;
    }

    switch (item) {
    case 0:
        if (w->kind == WK_TERMINAL) {
            if (w->selecting) term_end_select(w);
            else {
                g_clip[0] = '\0'; g_clip_has = false;
                for (int i = 0; i < g_tl_n; i++) clip_append_line(g_tl[i]);
            }
        }
        break;
    case 1:
        if (g_clip_has && w->kind == WK_TERMINAL) {
            for (const char *p = g_clip; *p; p++) rosh_key(*p, NULL);
            g_dirty = true;
        }
        break;
    case 2:
        if (w->kind == WK_TERMINAL) {
            w->selecting = true;
            w->sel_start_x = w->x;
            w->sel_start_y = w->y + WM_TITLE_H;
            w->sel_end_x = w->x + w->w;
            w->sel_end_y = w->y + w->h;
            term_end_select(w);
            g_dirty = true;
        }
        break;
    case 3: win_maximize_toggle(w); break;
    case 4: close_focused(); g_dirty = true; break;
    case 5:
        if (w->maximized) win_maximize_toggle(w);
        w->floating = !w->floating;
        retile();
        g_dirty = true;
        break;
    case 6:
    case 7: {
        int target = item - 6 + 1;            /* rows 6/7 → ws 2/3 (0-based) */
        if (target >= WM_WORKSPACES) target = WM_WORKSPACES - 1;
        w->ws = target;
        if (!w->floating && !w->maximized) retile();
        g_ws = target;                        /* follow the window */
        retile(); focus_first();
        g_dirty = true;
        break;
    }
    }
    close_context_menu();
}

/* Text selection hit test for terminal window */
static void term_begin_select(wm_window *w, int px, int py) {
    if (w->kind != WK_TERMINAL) return;
    w->selecting = true;
    w->sel_start_x = px;
    w->sel_start_y = py;
    w->sel_end_x = px;
    w->sel_end_y = py;
}

static void term_update_select(wm_window *w, int px, int py) {
    if (!w->selecting || w->kind != WK_TERMINAL) return;
    w->sel_end_x = px;
    w->sel_end_y = py;
}

static void term_end_select(wm_window *w) {
    if (!w->selecting || w->kind != WK_TERMINAL) return;

    if (w->sel_end_x < w->sel_start_x) {
        int tmp = w->sel_end_x; w->sel_end_x = w->sel_start_x; w->sel_start_x = tmp;
    }
    if (w->sel_end_y < w->sel_start_y) {
        int tmp = w->sel_end_y; w->sel_end_y = w->sel_start_y; w->sel_start_y = tmp;
    }

    const int lh = 16, pad = 8;
    int cx = w->x + 2, cy = w->y + WM_TITLE_H;
    int cw = w->w - 4, chh = w->h - WM_TITLE_H - 2;
    int ox, oy, ow, oh;
    term_pane *pane = &g_pane[g_fpane];
    int start_row, end_row, body = 20, first, last;
    term_pane_rect(cx, cy, cw, chh, g_fpane, &ox, &oy, &ow, &oh);
    start_row = (w->sel_start_y - (oy + pad)) / lh;
    end_row   = (w->sel_end_y   - (oy + pad)) / lh;
    if (start_row < 0) start_row = 0;
    if (end_row < start_row) end_row = start_row;
    { int rows = (oh - 2 * pad) / lh;
      if (rows > 2) body = rows - 1; }
    last = pane->n - pane->view;
    if (last < 0) last = 0;
    if (last > pane->n) last = pane->n;
    first = last > body ? last - body : 0;
    start_row += first;
    end_row   += first;
    if (end_row >= pane->n) end_row = pane->n > 0 ? pane->n - 1 : 0;

    g_clip[0] = '\0'; g_clip_has = false;
    for (int row = start_row; row <= end_row && row < g_tl_n; row++)
        clip_append_line(g_tl[row]);
    w->selecting = false;
    g_dirty = true;
}

/* Multitasking: an app with a window on the CURRENT workspace is focused;
   the same app on another workspace (or closed) spawns a new instance here.
   Terminal/Neuro/Echo/Calculator stay single-instance (live shared state). */
static void focus_or_open(wm_kind kind) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        wm_window *w = &g_win[i];
        if (w->used && w->ws == g_ws && w->kind == kind) {
            if (kind == WK_TERMINAL) { open_terminal(); return; }
            g_focus = w->id;
            g_dirty = true;
            return;
        }
    }
    open_kind(kind);
}

static void on_press(int px, int py) {
    /* Close context menu on any click */
    if (g_ctx_open) {
        int item = ctx_item_at(px, py);
        if (item >= 0) { handle_context_action(item); return; }
        close_context_menu();
        /* fall through to handle normal click */
    }

    /* The Start menu is modal in the only way a menu should be: a click
       anywhere else closes it, and only a click on a row acts. */
    if (g_start_open) {
        int item = start_item_at(px, py);
        if (item >= 0) { start_action(item); return; }
        bool on_start_btn = (py < WM_BAR_H && g_start_x >= 0 &&
                             px >= g_start_x && px < g_start_x + START_W);
        g_start_open = false;
        g_dirty = true;
        if (on_start_btn) return;              /* the button toggles it shut */
        /* fall through: the click also means whatever it would normally mean */
    }

    /* Eclipse Shell dock first: it floats above every window, so clicks on
       tiles must open/raise apps even when a fullscreen browser is up. */
    {
        int dtk = dock_hit_test((uint32_t)px, (uint32_t)py);
        if (dtk >= 0) {
            const char *dapp = dock_app_at(dtk);
            if (dapp) {
                dock_bounce(dapp);
                if      (dock_name_is(dapp, "Files"))     focus_or_open(WK_FILES);
                else if (dock_name_is(dapp, "Terminal"))  focus_or_open(WK_TERMINAL);
                else if (dock_name_is(dapp, "Nova"))      focus_or_open(WK_BROWSER);
                else if (dock_name_is(dapp, "Calculator"))focus_or_open(WK_CALC);
                else if (dock_name_is(dapp, "Multimedia"))focus_or_open(WK_PHOTOS);
                else if (dock_name_is(dapp, "Settings"))  focus_or_open(WK_SETTINGS);
            }
            g_dirty = true;
            return;
        }
    }

    /* Nova nav buttons (back / forward / reload) + URL-bar edit focus. */
    {
        wm_window *nf = win_by_id(g_focus);
        if (nf && nf->kind == WK_BROWSER) {
            /* Click on the URL bar row -> start editing (caret shows). */
            if (g_nova_btn_y > 0 && py >= g_nova_btn_y && py < g_nova_btn_y + g_nova_btn_sz) {
                int urlx = g_nova_btn_rel_x + g_nova_btn_sz + 8 + 46;
                if (px >= urlx && px < nf->x + nf->w - 10) { nova_st(nf->id)->editing = true; g_dirty = true; return; }
            }
            if (nova_st(nf->id)->editing) { nova_st(nf->id)->editing = false; }   /* click away ends edit */
        }
        if (nf && nf->kind == WK_BROWSER && g_nova_btn_y > 0) {
            int bx0 = g_nova_btn_back_x, by0 = g_nova_btn_y, sz = g_nova_btn_sz;
            if (py >= by0 && py < by0 + sz &&
                px >= bx0 && px < g_nova_btn_rel_x + sz) {
                if (px < bx0 + sz)                       nova_back();
                else if (px < g_nova_btn_fwd_x + sz)     nova_forward();
                else                                     nova_load(nf->id, nova_st(nf->id)->url, false);
                g_dirty = true;
                return;
            }
        }
    }

    /* Eclipse Shell dock: click = open/raise the app. */
    {
        int dt = dock_hit_test((uint32_t)px, (uint32_t)py);
        if (dt >= 0) {
            const char *app = dock_app_at(dt);
            if (app) {
                dock_bounce(app);
                if      (dock_name_is(app, "Files"))     open_kind(WK_FILES);
                else if (dock_name_is(app, "Terminal"))  open_kind(WK_TERMINAL);
                else if (dock_name_is(app, "Nova"))      wm_open_nova("rgx://home");
                else if (dock_name_is(app, "Calculator"))open_kind(WK_CALC);
                else if (dock_name_is(app, "Multimedia"))open_kind(WK_PHOTOS);
                else if (dock_name_is(app, "Settings"))  open_kind(WK_SETTINGS);
            }
            g_dirty = true;
            return;
        }
    }

    /* top bar: workspace tags + Start button (top-right) */
    if (py < WM_BAR_H) {
        if (g_start_x >= 0 && px >= g_start_x && px < g_start_x + START_W) {
            g_start_open = !g_start_open; g_dirty = true; return;
        }
        for (int i = 0; i < WM_WORKSPACES; i++)
            if (px >= g_tag_x[i] && px < g_tag_x[i] + g_tag_w[i]) { g_ws = i; retile(); focus_first(); g_dirty = true; return; }
        return;
    }

    int id = window_at(px, py);
    if (id < 0) {
        /* Nothing but desktop under the pointer: an icon, or empty space.
           A press ARMS a drag; open fires on release if it never moved
           (global drag & drop contract: ON_DRAG_START / ON_DRAG_DROP). */
        int ic = icon_at(px, py);
        if (ic >= 0) {
            for (int j = 0; j < ICON_COUNT; j++) g_icon_selected[j] = false;
            g_icon_drag   = ic;
            g_icon_moving = false;
            g_icon_press_x = (uint32_t)px;
            g_icon_press_y = (uint32_t)py;
        } else {
            /* Rubber-band multi-select: press on empty desktop starts a
               marquee; icons inside the band end up selected. */
            g_marquee = true;
            g_marquee_x0 = g_marquee_x1 = (uint32_t)px;
            g_marquee_y0 = g_marquee_y1 = (uint32_t)py;
            for (int j = 0; j < ICON_COUNT; j++) g_icon_selected[j] = false;
        }
        return;
    }
    g_focus = id;
    wm_window *w = win_by_id(id);

    /* Resize handles (edges/corners) — auto-float if tiled. */
    int edge = hit_test_resize(w, px, py);
    if (edge == 0 && !w->floating && !w->maximized) {
        /* Also allow resize on tiled windows by floating first at edges. */
        const int hs = 8;
        bool near = (px >= w->x && px < w->x + hs) ||
                    (px >= w->x + w->w - hs && px < w->x + w->w) ||
                    (py >= w->y + w->h - hs && py < w->y + w->h);
        if (near) {
            w->floating = true;
            retile();
            edge = hit_test_resize(w, px, py);
        }
    }
    if (edge != 0) {
        w->resizing = true;
        w->resize_edge = edge;
        w->orig_w = w->w;
        w->orig_h = w->h;
        g_drag = id;
        return;
    }

    /* Caption buttons: close / maximize */
    int cap = caption_hit(w, px, py);
    if (cap == 1) { close_focused(); g_dirty = true; return; }
    if (cap == 2) { win_maximize_toggle(w); return; }

    /* Title bar: double-click maximizes; single-click starts drag. */
    if (py < w->y + WM_TITLE_H) {
        uint64_t now = arch_timer_uptime_ms();
        if (g_last_title_click_id == w->id &&
            now - g_last_title_click_ms < 400) {
            win_maximize_toggle(w);
            g_last_title_click_id = -1;
            return;
        }
        g_last_title_click_ms = now;
        g_last_title_click_id = w->id;
        if (w->maximized) return; /* drag disabled while maximized */
        if (!w->floating) { w->floating = true; retile(); }
        g_drag = id;
        return;
    }

    if (w->kind == WK_FILES) {
        int which = -1;
        int hit = fm_hit(w, px, py, &which);
        static uint64_t last_fm_ms; static int last_fm_sel = -1;
        if (hit == 1 && which >= 0 && which < FM_ROOT_N) {
            fm_go(FM_ROOTS[which]);
            w->sel = 0;
        } else if (hit == 3) {
            fm_parent(g_fm_path);
            fm_go(g_fm_path);
            w->sel = 0;
        } else if (hit == 2 && which >= 0) {
            uint64_t now = arch_timer_uptime_ms();
            if (which == last_fm_sel && now - last_fm_ms < 400) {
                fm_open_sel(w);
                last_fm_sel = -1;
            } else {
                w->sel = which;
                last_fm_sel = which;
                last_fm_ms = now;
                /* Start file drag if not a pure click (release without move cancels) */
                if (!g_fm_isdir[which]) g_fm_drag = which;
            }
        }
    } else if (w->kind == WK_CALC) {
        calc_click(w, px, py);
    } else if (w->kind == WK_PHOTOS) {
        int cx2 = w->x + 2, cy2 = w->y + WM_TITLE_H;
        int hit = photos_hit(cx2, cy2, px, py);
        int n = wp_count();
        if (hit == 1 && n) { w->sel = (w->sel + n - 1) % n; photo_load(w->sel); }
        else if (hit == 2 && n) { w->sel = (w->sel + 1) % n; photo_load(w->sel); }
        else if (hit == 3 && n) { gui_wallpaper_load_index(w->sel); }
        /* click on the picture only browses? no — it must not change wallpaper */
    } else if (w->kind == WK_SETTINGS) {
        int cx2 = w->x + 2, cy2 = w->y + WM_TITLE_H;
        int hit = settings_hit(cx2, cy2, px, py);
        if (hit >= 10 && hit < 20) fb_set_theme(hit - 10);
        else if (hit >= 20) {
            w->sel = hit - 20;
            gui_wallpaper_load_index(w->sel);
        }
    } else if (w->kind == WK_DISKS) {
        int n = disks_gui_count();
        if (n) w->sel = (w->sel + 1) % n;
    } else if (w->kind == WK_TERMINAL) {
        int cx = w->x + 2, cy = w->y + WM_TITLE_H;
        int cw = w->w - 4, ch = w->h - WM_TITLE_H - 2;
        term_focus_pane(term_hit_pane(cx, cy, cw, ch, px, py));
        term_begin_select(w, px, py);
    }
    g_dirty = true;
}

/* How fast does something on this workspace change on its own? Returns the
   per-tick stimulus for the refresh actor, or 0 when the screen is static —
   in which case the actor gets nothing, never spikes, and the WM does zero
   periodic work. The Neuro window needs a faster rate than the monitor: its
   whole point is watching the membrane leak, and a 1 Hz sample of an
   exponential with tau = 1 s is a straight line. */
static int32_t refresh_stimulus(void) {
    bool monitor = false, neuro = false;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!g_win[i].used || g_win[i].ws != g_ws) continue;
        if (g_win[i].kind == WK_MONITOR) monitor = true;
        if (g_win[i].kind == WK_NEURO)   neuro   = true;
    }
    if (neuro)   return Q16_ONE / 10;    /* ~10 Hz: the decay is visible     */
    if (monitor) return Q16_ONE / 100;   /* ~1 Hz: telemetry is slow news    */
    return 0;
}

/* Threshold-driven UI refresh: a real LIF actor on the fabric. It integrates
   a small stimulus per PIT tick ONLY while a live-telemetry window is on
   screen; when the accumulated potential crosses threshold (~once a second)
   it spikes and requests a repaint. On a static workspace it gets zero
   stimulus, never spikes, and the WM does zero periodic work — this is the
   manifesto's "thresholded activation for soft, deferrable services", and the
   suppression here is genuine, not decorative. */
static rx_actor_t g_ui_refresh;
static bool       g_ui_refresh_fired;
static bool ui_refresh_cb(rx_actor_t *self, const rx_event_t *ev) {
    (void)self; (void)ev; g_ui_refresh_fired = true; return true;
}

void wm_run(void) {
    if (!fb_active()) { console_write("  [wm] needs the graphical framebuffer.\n"); return; }
    uint32_t w, h; fb_dims(&w, &h);
    g_screen_w = (int)w; g_screen_h = (int)h;

    /* Built once. Coming back from the classic shell finds the same windows
       where they were left, which is the difference between a desktop and a
       screen that is rebuilt every time you look away. */
    if (!g_started) {
        for (int i = 0; i < WM_MAX_WINDOWS; i++) { g_win[i].used = false; g_win[i].hidden = false; }
        g_ws = 0; g_next_id = 1;
        g_focus = -1;
        /* An empty desktop on purpose: the wallpaper and the icons ARE the
           first screen now. Opening things is the user's first move, not
           something the system did on their behalf before they arrived. */
        gui_init();
        g_started = true;
    }
    g_drag = -1; g_prev_left = false; g_start_open = false;
    icon_slots_reset();
    dock_init();
    etk_init();          /* scene graph foundation (desktop layer migrates here) */
    g_exit = WM_ACTION_SHELL;
    g_dirty = true;

    /* threshold 1.0, no leak (tau huge), fires after ~100 ticks of stimulus */
    rx_actor_init(&g_ui_refresh, "ui_refresh", ui_refresh_cb,
                  /*threshold*/ Q16_ONE, /*reset*/ 0,
                  /*tau*/ 1000000, /*refractory*/ 0);
    g_ui_refresh_fired = false;
    uint64_t last_tick = arch_timer_ticks();
    uint64_t clock_seen = 0;
    (void)clock_second_changed(&clock_seen);   /* prime so we don't paint twice */

    for (;;) {
        /* Someone is typing on the serial console. That conversation belongs
           to the classic shell — it is line-oriented, and this is a desktop —
           so hand over instead of shredding their command into hotkeys. It is
           also what lets a machine driven entirely down COM1 (a serial cable,
           or the automated tests) still reach a prompt now that boot lands
           here. The byte itself is left in the UART for the shell to read. */
        if (console_serial_has_input()) { fb_clear(); fb_present(); return; }

        if (dock_animating()) g_dirty = true;   /* launch pulse repaints */
        if (g_dirty) {
            fb_cursor_hide(); wm_draw(); fb_cursor_show();
            fb_present();               /* one blit, no half-drawn frame */
            g_dirty = false;
        }
        /* Threshold-driven telemetry refresh (replaces the old 1 Hz timer
           repaint): feed the actor ~0.01 per tick ONLY while the monitor is
           visible, so it spikes ~1 Hz then; on a static workspace it never
           spikes and we never repaint. */
        uint64_t now = arch_timer_ticks();
        if (now != last_tick) {
            last_tick = now;
            /* Not while dragging. A telemetry repaint mid-drag would recompose
               the scene, and the scene cache the drag is copying from would
               then have the dragged window baked into it — every subsequent
               frame would smear a copy of the window across the desktop. */
            int32_t s = (g_drag < 0) ? refresh_stimulus() : 0;
            if (s) rx_actor_stimulate(&g_ui_refresh, RX_EVENT_DEADLINE, s, now);
            /* Wall-clock second: paint the tray clock only. A full compose
               here is what made typing feel lagged if you paused between keys. */
            if (g_drag < 0 && clock_second_changed(&clock_seen))
                paint_clock_only();
        }
        if (g_ui_refresh_fired) {
            g_ui_refresh_fired = false;
            if (g_drag < 0) {
                wm_window *rf = win_by_id(g_focus);
                /* Don't steal a keystroke's frame to refresh telemetry. */
                if (!rf || rf->kind != WK_TERMINAL)
                    g_dirty = true;
            }
        }

        int ch = 0, dx = 0, dy = 0, btn = 0;
        int ev = ui_next_raw(&ch, &dx, &dy, &btn);
        if (ev == 0) { arch_idle_wait(); continue; }

        if (ev == 1) {                               /* keyboard             */
            /* The Terminal and the Neuro window GRAB the keyboard: every key
               belongs to them, because a shell that cannot type Tab has no
               completion and a neuron demo that reserves half the alphabet is
               not "any key is a stimulus".

               Tab used to be taken by the WM before the terminal saw it, which
               is why completion silently cycled the focus instead. So the one
               reserved key is Esc, and it is layered: Esc hands the keyboard
               back to the window manager, and Esc again (or Esc with nothing
               else on the workspace) leaves for the shell. */
            wm_window *kf = win_by_id(g_focus);
            bool grab = kf && (kf->kind == WK_TERMINAL || kf->kind == WK_NEURO);

            if (ch == KEY_F12) {
                capture_screen(0, 0);
                g_dirty = true;
                continue;
            }

            if (ch == 27) {
                if (g_start_open) { g_start_open = false; g_dirty = true; continue; }
                if (g_help_open) { g_help_open = false; g_dirty = true; continue; }
                /* Nova owns ESC first: it navigates back, like every browser. */
                if (kf && kf->kind == WK_BROWSER &&
                (nova_st(kf->id)->editing || !ECHOS_DEV_SHORTCUTS)) { nova_back(); g_dirty = true; continue; }
                if (grab) { g_focus = -1; g_dirty = true; continue; }
                /* Desktop ESC no longer dumps the user into the text console —
                   the classic shell stays reachable from the Start menu. */
                g_focus = -1; g_dirty = true; continue;
            }

#if ECHOS_DEV_SHORTCUTS
            /* Alt+Tab / Ctrl+Tab — dev edition only. */
            if (ch == KEY_ALTTAB) {
                cycle_focus();
                g_dirty = true;
                continue;
            }
#endif

            /* Nova URL bar owns every key while the browser is focused.
               User editions have no letter hotkeys, so typing goes straight
               to the bar (caret visible); Dev keeps click-to-edit. */
            nova_inst *st = nova_st(kf ? kf->id : 0);
            if (kf && kf->kind == WK_BROWSER &&
                (st->editing || !ECHOS_DEV_SHORTCUTS)) {
                if (ch == '\n' || ch == '\r') {
                    if (st->entry[0]) {
                        char target[128];
                        size_t k = 0;
                        const char *src = st->entry;
                        if (!has_sub(st->entry, "://") && !has_sub(st->entry, "rgx://")) {
                            k_memcpy(target, "https://", 8); k = 8;
                        }
                        while (src[0] && k < sizeof(target) - 1) {
                            if (src[0] == ' ') { src++; continue; }
                            target[k++] = *src++;
                        }
                        target[k] = '\0';
                        st->entry[0] = '\0';
                        st->editing = false;
                        nova_load(kf->id, target, true);
                    }
                    g_dirty = true; continue;
                }
                if (ch == 0x7F || ch == '\b') {
                    size_t L = k_strlen(st->entry);
                    if (L) st->entry[L - 1] = '\0';
                    st->editing = true;
                    g_dirty = true; continue;
                }
                if (ch >= 32 && ch < 127) {
                    size_t L = k_strlen(st->entry);
                    if (L < sizeof(st->entry) - 1) {
                        st->entry[L] = ch; st->entry[L + 1] = '\0';
                    }
                    st->editing = true;
                    g_dirty = true; continue;
                }
                if (ch == 27) { st->editing = false; g_dirty = true; continue; }
            }

            /* Ctrl+C / Ctrl+V — before terminal grab, so they never type. */
            if (ch == 3) {   /* Ctrl+C copy */
                if (kf && kf->kind == WK_TERMINAL) {
                    if (kf->selecting) term_end_select(kf);
                    else {
                        g_clip[0] = '\0'; g_clip_has = false;
                        for (int i = 0; i < g_tl_n; i++) clip_append_line(g_tl[i]);
                        g_dirty = true;
                    }
                }
                continue;
            }
            if (ch == 22) {  /* Ctrl+V paste */
                if (g_clip_has && kf && kf->kind == WK_TERMINAL) {
                    for (const char *p = g_clip; *p; p++) {
                        if (*p == '\n' || *p == '\r') rosh_key('\n', NULL);
                        else rosh_key(*p, NULL);
                    }
                    g_dirty = true;
                }
                continue;
            }

            /* F11 (scancode path may not map — also accept 'F' with no grab
               via maximize key `m` when not typing). Maximize focused win. */
#if ECHOS_DEV_SHORTCUTS
            if (!grab && (ch == 'm' || ch == 'M')) {
                if (kf) win_maximize_toggle(kf);
                g_dirty = true;
                continue;
            }
#endif
            /* File Explorer keys when focused (not typing in terminal). */
            if (kf && kf->kind == WK_FILES && !grab) {
                if (ch == '\n' || ch == '\r') { fm_open_sel(kf); g_dirty = true; continue; }
                if (ch == 0x7F || ch == '\b') { /* Delete / Backspace: up if nothing */
                    if (kf->sel >= 0) fm_delete_sel(kf);
                    g_dirty = true; continue;
                }
                if (ch == 'c' || ch == 'C') { fm_copy_sel(kf, false); g_dirty = true; continue; }
                if (ch == 'x' || ch == 'X') { fm_copy_sel(kf, true); g_dirty = true; continue; }
                if (ch == 'v' || ch == 'V') { fm_paste(); g_dirty = true; continue; }
                if (ch == 'n' || ch == 'N') { fm_new_note(); g_dirty = true; continue; }
                if (ch == 'u' || ch == 'U') {
                    fm_parent(g_fm_path); fm_go(g_fm_path); kf->sel = 0; g_dirty = true; continue;
                }
                if (ch == 3) { fm_copy_sel(kf, false); g_dirty = true; continue; } /* Ctrl+C */
                if (ch == 22) { fm_paste(); g_dirty = true; continue; }            /* Ctrl+V */
            }

            if (grab && kf->kind == WK_TERMINAL) {
                term_key_result r = term_key(ch);
                /* Drain a burst of keys into one present — one compose per
                   character was the other half of the typing lag. */
                while (r == TERM_INPUT_ONLY) {
                    int nch = 0;
                    if (!ui_next_key_nb(&nch)) break;
                    r = term_key(nch);
                }
                if (g_term_halt) { g_exit = WM_ACTION_HALT; fb_clear(); fb_present(); return; }
                if (g_term_want_neuro) { g_term_want_neuro = false; open_neuro(); g_dirty = true; continue; }
                if (r == TERM_INPUT_ONLY) term_refresh_input(kf);
                else if (r == TERM_FULL)  g_dirty = true;
                continue;
            }
            if (grab && kf->kind == WK_NEURO) {
                if (ch != 'q') { neuro_stimulate(); g_dirty = true; continue; }
            }
            if (kf && kf->kind == WK_FILES && g_fm_renaming) {
                fm_rename_key(kf, ch);
                continue;
            }
            if (kf && kf->kind == WK_CALC) {
                bool mine = (ch >= '0' && ch <= '9') || ch == '.' || ch == '+' || ch == '-' ||
                            ch == '*' || ch == '/' || ch == '=' || ch == '\n' || ch == '\r' ||
                            ch == '\b' || ch == 0x7F || ch == 'c' || ch == 'C';
                if (mine) { calc_key(ch); g_dirty = true; continue; }
            }
            if (kf && kf->kind == WK_PHOTOS) {
                int n = wp_count();
                if (n && (ch == ']' || ch == '.' || ch == ' ')) {
                    kf->sel = (kf->sel + 1) % n; photo_load(kf->sel); g_dirty = true; continue;
                }
                if (n && (ch == '[' || ch == ',')) {
                    kf->sel = (kf->sel + n - 1) % n; photo_load(kf->sel); g_dirty = true; continue;
                }
#if ECHOS_DEV_SHORTCUTS
                if (ch == '\n' || ch == '\r') {
                    gui_wallpaper_load_index(kf->sel); g_dirty = true; continue;
                }
#endif
            }

#if ECHOS_DEV_SHORTCUTS
            /* Developer edition: the keyboard drives the shell. User editions
               (Minimal, Edge) have NO letter shortcuts — stray keys must never
               open apps or change state. Ctrl+C/V and mouse only. */
            if (ch == '\t') { cycle_focus(); g_dirty = true; continue; }
            if (ch == '1' || ch == '2' || ch == '3') { g_ws = ch - '1'; retile(); focus_first(); g_dirty = true; }
            else if (ch == 'n') { spawn((wm_kind)(g_next_id % WK_TERMINAL), g_ws, false); retile(); g_dirty = true; }
            else if (ch == 't') { open_terminal(); g_dirty = true; }
            else if (ch == 'e') { open_kind(WK_FILES); g_dirty = true; }
            else if (ch == 'y') { open_neuro();    g_dirty = true; }
            else if (ch == 'k') { open_kind(WK_CALC); g_dirty = true; }
            else if (ch == 'd') { open_kind(WK_DISKS); g_dirty = true; }
            else if (ch == 'i') { open_kind(WK_PHOTOS); g_dirty = true; }
            else if (ch == 'a') { open_kind(WK_SETTINGS); g_dirty = true; }
            else if (ch == 's') { g_start_open = !g_start_open; g_dirty = true; }
            else if (ch == 'b') { wm_open_nova(0); g_dirty = true; }
            else if (ch == '?') { g_help_open = !g_help_open; g_dirty = true; }
            else if (ch == 'f') {
                wm_window *ff = win_by_id(g_focus);
                if (ff) {
                    if (ff->maximized) win_maximize_toggle(ff);
                    ff->floating = !ff->floating;
                    retile();
                }
                g_dirty = true;
            }
            else if (ch == 'q') { close_focused(); g_dirty = true; }
#endif /* ECHOS_DEV_SHORTCUTS */
            if (g_exit != WM_ACTION_SHELL) { fb_clear(); fb_present(); return; }
            continue;
        }
        /* mouse */
        uint32_t cx, cy; fb_cursor_get(&cx, &cy);
        if (dx || dy) {
            /* Clamp FIRST: every consumer (marquee, dock, hit tests) gets
               on-screen coordinates. Unclamped drag past an edge used to
               build giant damage rects → out-of-bounds writes → crash. */
            int nx = (int)cx + dx, ny = (int)cy + dy;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx > g_screen_w - 1) nx = g_screen_w - 1;
            if (ny > g_screen_h - 1) ny = g_screen_h - 1;

            /* Dock magnification is positional. Fast path: repaint ONLY the
               dock band (wallpaper restore + dock) instead of composing the
               whole screen — this is what makes hover 60fps-friendly. */
            {
                static bool was_near;
                bool near_dock = dock_near((uint32_t)nx, (uint32_t)ny);
                if (near_dock || was_near) {
                    int bx, by, bw, bh;
                    dock_band(&bx, &by, &bw, &bh);
                    fb_cursor_hide();
                    fb_cursor_moveto((uint32_t)nx, (uint32_t)ny);
                    fb_damage_reset();
                    gui_background(bx, by, bw, bh);   /* wallpaper restore */
                    dock_layout((uint32_t)g_screen_w, (uint32_t)g_screen_h,
                                (uint32_t)nx, (uint32_t)ny);
                    dock_render();
                    fb_cursor_show();
                    fb_present();
                    was_near = near_dock;
                    continue;                          /* no full compose */
                }
                was_near = near_dock;
            }

            /* Rubber-band selection follows the cursor. */
            if (g_marquee) {
                g_marquee_x1 = (uint32_t)nx;
                g_marquee_y1 = (uint32_t)ny;
                for (int j = 0; j < ICON_COUNT; j++) {
                    if (g_icon_removed[j]) { g_icon_selected[j] = false; continue; }
                    int ix, iy; icon_box(j, &ix, &iy);
                    uint32_t mx0 = g_marquee_x0 < g_marquee_x1 ? g_marquee_x0 : g_marquee_x1;
                    uint32_t my0 = g_marquee_y0 < g_marquee_y1 ? g_marquee_y0 : g_marquee_y1;
                    uint32_t mx1 = g_marquee_x0 < g_marquee_x1 ? g_marquee_x1 : g_marquee_x0;
                    uint32_t my1 = g_marquee_y0 < g_marquee_y1 ? g_marquee_y1 : g_marquee_y0;
                    bool inside = (uint32_t)ix + ICON_W > mx0 && (uint32_t)ix < mx1 &&
                                  (uint32_t)iy + ICON_H > my0 && (uint32_t)iy < my1;
                    g_icon_selected[j] = inside;
                }
                fb_cursor_moveto((uint32_t)nx, (uint32_t)ny);
                wm_draw();
                fb_present();
                continue;
            }

            /* Desktop icon drag: once past the threshold, the ghost follows. */
            if (g_icon_drag >= 0 && !g_icon_moving) {
                int ddx = nx - (int)g_icon_press_x, ddy = ny - (int)g_icon_press_y;
                if (ddx * ddx + ddy * ddy > 36) { g_icon_moving = true; g_dirty = true; }
            } else if (g_icon_drag >= 0) {
                fb_cursor_moveto((uint32_t)nx, (uint32_t)ny);
                wm_draw();          /* cheap enough; icons are few */
                fb_present();
                continue;
            }

            wm_window *d = (g_drag >= 0) ? win_by_id(g_drag) : NULL;

            if (d && d->resizing) {
                /* Resize only — never also translate as a move. */
                fb_cursor_moveto(nx, ny);
                switch (d->resize_edge) {
                    case 1: d->y += dy; d->h -= dy; break;
                    case 2: d->h += dy; break;
                    case 3: d->w += dx; break;
                    case 4: d->x += dx; d->w -= dx; break;
                    case 5: d->y += dy; d->h -= dy; d->w += dx; break;
                    case 6: d->x += dx; d->w -= dx; d->y += dy; d->h -= dy; break;
                    case 7: d->w += dx; d->h += dy; break;
                    case 8: d->x += dx; d->w -= dx; d->h += dy; break;
                }
                clamp_win_size(d);
                g_dirty = true;
            } else if (d) {
                /* Move floating window with scene-cache path. */
                if (g_scene_for != g_drag) scene_prepare(g_drag);
                int ox = d->x, oy = d->y, ow = d->w, oh = d->h;
                fb_cursor_hide();
                fb_cursor_moveto(nx, ny);
                d->x += dx; d->y += dy;
                if (d->y < WM_BAR_H) d->y = WM_BAR_H;
                if (d->y > g_screen_h - WM_TASK_H - WM_TITLE_H)
                    d->y = g_screen_h - WM_TASK_H - WM_TITLE_H;
                fb_damage_reset();
                if (g_scene_for == g_drag) {
                    gui_scene_restore(ox, oy, ow, oh);
                    draw_window(d);
                    if (d->y < WM_BAR_H + 4) draw_bar();
                } else {
                    wm_draw();
                    damage_box(ox, oy, ow, oh);
                    damage_box(d->x, d->y, d->w, d->h);
                }
                fb_cursor_show();
                fb_present();
            } else {
                fb_cursor_moveto(nx, ny);
            }
        }
        /* Mouse wheel (buttons bit3 up / bit4 down): always consumed — it
           must never leak into click/drag logic (that is how scrolling ended
           up mutating other apps). Scrolls whichever terminal is under the
           cursor, focused or not. */
        if (btn & 0x18) {
            int delta = (btn & 0x08) ? 6 : -6;
            wm_window *fw = win_by_id(g_focus);
            if (fw && fw->kind == WK_TERMINAL) {
                term_scroll(delta);
            } else {
                uint32_t mx, my; fb_cursor_get(&mx, &my);
                int hit = window_at((int)mx, (int)my);
                wm_window *hw = win_by_id(hit);
                if (hw && hw->kind == WK_TERMINAL) term_scroll(delta);
            }
            g_dirty = true;
            g_prev_left = false;
            continue;
        }
        bool left = (btn & 0x01) != 0;
        bool right = (btn & 0x02) != 0;

        /* Update text selection while the button is held. */
        if (left && g_drag < 0) {
            wm_window *f = win_by_id(g_focus);
            if (f && f->selecting) {
                uint32_t mx, my; fb_cursor_get(&mx, &my);
                term_update_select(f, (int)mx, (int)my);
                g_dirty = true;
            }
        }
        
        /* Right-click: dock tile / desktop icon / window / explorer / desktop */
        if (right && !g_prev_left && !g_ctx_open) {
            fb_cursor_get(&cx, &cy);
            int dt = dock_hit_test(cx, cy);
            if (dt >= 0) {                       /* mode 3: dock tile */
                g_ctx_open = true; g_ctx_mode = 3;
                g_ctx_x = (int)cx; g_ctx_y = (int)cy;
                g_ctx_win_id = dt;
                g_dirty = true;
            } else if ((int)cy < g_screen_h - WM_TASK_H) {
                int ic = icon_at((int)cx, (int)cy);
                if (ic >= 0) {                   /* mode 4: desktop icon */
                    g_ctx_open = true; g_ctx_mode = 4;
                    g_ctx_x = (int)cx; g_ctx_y = (int)cy;
                    g_ctx_win_id = ic;
                    g_dirty = true;
                } else {
                    int id = window_at((int)cx, (int)cy);
                    if (id >= 0) {
                        wm_window *rw = win_by_id(id);
                        g_ctx_open = true;
                        g_ctx_x = (int)cx;
                        g_ctx_y = (int)cy;
                        g_ctx_win_id = id;
                        g_ctx_mode = 0;
                        if (rw && rw->kind == WK_FILES) {
                            g_ctx_mode = 1;
                            g_focus = id;
                            int which = -1;
                            if (fm_hit(rw, (int)cx, (int)cy, &which) == 2 && which >= 0)
                                rw->sel = which;
                        }
                        g_dirty = true;
                    } else if ((int)cy >= WM_BAR_H) {
                        g_ctx_open = true;       /* mode 2: empty desktop */
                        g_ctx_x = (int)cx;
                        g_ctx_y = (int)cy;
                        g_ctx_win_id = -1;
                        g_ctx_mode = 2;
                        g_dirty = true;
                    }
                }
            }
        }
        
        if (left && !g_prev_left) {
            fb_cursor_get(&cx, &cy);
            on_press((int)cx, (int)cy);
            if (g_exit != WM_ACTION_SHELL) { fb_clear(); fb_present(); return; }
        }
        if (!left && g_prev_left) {                   /* drop                 */
            if (g_marquee) {
                bool moved = g_marquee_x0 != g_marquee_x1 || g_marquee_y0 != g_marquee_y1;
                g_marquee = false;
                if (!moved) for (int j = 0; j < ICON_COUNT; j++) g_icon_selected[j] = false;
                g_dirty = true;
            }
            /* Desktop icon: release. No movement = open; moved = drop
               onto another icon's slot (swap), an empty slot, or the dock. */
            if (g_icon_drag >= 0) {
                uint32_t mx, my; fb_cursor_get(&mx, &my);
                int ic = g_icon_drag;
                g_icon_drag = -1;
                if (!g_icon_moving) {
                    open_kind(ICONS[ic].kind);
                } else {
                    int dt = dock_hit_test(mx, my);
                    if (dt >= 0 && dock_app_at(dt)) {
                        /* Dropped on the dock: launch + pin it there. */
                        dock_pin(ICONS[ic].label);
                        open_kind(ICONS[ic].kind);
                    } else {
                        int slot = icon_slot_at((int)mx, (int)my);
                        if (slot >= 0) {
                            int occupant = -1;
                            for (int j = 0; j < ICON_COUNT; j++)
                                if (g_icon_slot[j] == slot && j != ic) { occupant = j; break; }
                            if (occupant >= 0)
                                g_icon_slot[occupant] = g_icon_slot[ic];  /* swap */
                            g_icon_slot[ic] = slot;
                        }
                    }
                }
                g_dirty = true;
            }
            /* File explorer: drop onto a folder = move */
            if (g_fm_drag >= 0) {
                wm_window *fw = win_by_id(g_focus);
                if (fw && fw->kind == WK_FILES) {
                    uint32_t mx, my; fb_cursor_get(&mx, &my);
                    int target = -1;
                    if (fm_hit(fw, (int)mx, (int)my, &target) == 2 &&
                        target >= 0 && g_fm_isdir[target] && target != g_fm_drag) {
                        char src[RXFS_NAME], dst[RXFS_NAME], base[RXFS_NAME];
                        fm_join(src, sizeof(src), g_fm_path, g_fm_names[g_fm_drag]);
                        fm_basename_clean(g_fm_names[g_fm_drag], base, sizeof(base));
                        char folder[RXFS_NAME];
                        fm_join(folder, sizeof(folder), g_fm_path, g_fm_names[target]);
                        fm_join(dst, sizeof(dst), folder, base);
                        fm_set_status(vfs_rename(src, dst) ? "moved by drag" : "drag-move failed");
                        fm_refresh();
                    }
                }
                g_fm_drag = -1;
                g_dirty = true;
            }
            if (g_drag >= 0) {
                wm_window *d = win_by_id(g_drag);
                if (d && d->resizing) {
                    d->resizing = false;
                    d->resize_edge = 0;
                } else if (d) {
                    wm_window *f = win_by_id(g_focus);
                    if (f && f->selecting) term_end_select(f);
                }
                g_drag = -1;
                g_scene_for = -1;
                g_dirty = true;
            }
            wm_window *f = win_by_id(g_focus);
            if (f && f->selecting) term_end_select(f);
        }
        g_prev_left = left;
    }
}
