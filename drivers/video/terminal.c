/* Console output. Mirrors everything to serial — handy for QEMU -serial stdio. */
#include "rogex.h"
#include "framebuffer.h"
#include "font.h"
#include "rx_event.h"
#include "ports.h"

/* --- VGA text mode ------------------------------------------------------- */
#define VGA_MEM   ((volatile uint16_t *)0xb8000)
#define VGA_COLS  80
#define VGA_ROWS  25
#define VGA_ATTR  0x0f          /* white on black — CLI console */

static size_t vga_row = 0, vga_col = 0;
static uint8_t g_vga_attr = VGA_ATTR;
static int g_sgr_bold, g_sgr_fg, g_sgr_bg;

/* ANSI 16-colour → 0x00RRGGBB for the framebuffer console. */
static const uint32_t ANSI_RGB[16] = {
    0x00000000, 0x00CC4444, 0x0033AA55, 0x00D4B44A,
    0x004488CC, 0x00CC66AA, 0x0033CCCC, 0x00D0D0D0,
    0x00707070, 0x00FF6666, 0x0066FF99, 0x00FFE066,
    0x0066AAFF, 0x00FF99DD, 0x0066FFFF, 0x00FFFFFF
};

static void vga_cursor_sync(void) {
    uint16_t pos = (uint16_t)(vga_row * VGA_COLS + vga_col);
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)pos);
}
static void vga_cursor_enable(void) {
    /* Scanlines 13–15: underline caret. Bit 5 of 0x0A = 0 enables blink. */
    outb(0x3D4, 0x0A); outb(0x3D5, 13);
    outb(0x3D4, 0x0B); outb(0x3D5, 15);
}

static int g_th_fg = 7, g_th_bg = 0;
static const char *g_th_name = "night";

static void console_palette_apply(void) {
    int fg = (g_sgr_fg | g_sgr_bold) & 15;
    int bg = g_sgr_bg & 7;
    uint32_t rgb_fg = ANSI_RGB[fg], rgb_bg = ANSI_RGB[bg];
    if (g_th_bg == 7 && bg == 7) rgb_bg = 0x00E8E4D8;  /* paper */
    if (g_th_fg == 2 && fg == 2) rgb_fg = 0x0033FF77;  /* matrix */
    if (g_th_fg == 3 && fg == 3) rgb_fg = 0x00FFB038;  /* amber */
    g_vga_attr = (uint8_t)((bg << 4) | (fg & 0x0F));
    fb_console_set_color(rgb_fg, rgb_bg);
}

int console_set_theme(const char *name) {
    static const char *names[] = { "night", "matrix", "ocean", "amber", "paper" };
    static const int fgs[] = { 7, 2, 6, 3, 0 };
    static const int bgs[] = { 0, 0, 0, 0, 7 };
    int i, id = -1;
    if (!name || !name[0]) return 0;
    for (i = 0; i < 5; i++) {
        const char *a = names[i], *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*a && !*b) { id = i; break; }
    }
    if (id < 0) return 0;
    g_th_name = names[id];
    g_th_fg = fgs[id];
    g_th_bg = bgs[id];
    g_sgr_bold = 0;
    g_sgr_fg = g_th_fg;
    g_sgr_bg = g_th_bg;
    console_palette_apply();
    return 1;
}
const char *console_theme_name(void) { return g_th_name; }

void console_write_prompt(const char *user, const char *host, const char *cwd) {
    const char *u = user && user[0] ? user : "live";
    const char *h = host && host[0] ? host : "echos";
    const char *d = cwd && cwd[0] ? cwd : "/";
    if (g_th_fg == 2) {          /* matrix */
        console_write("\033[1;32m"); console_write(u);
        console_write("\033[0;32m@"); console_write(h);
        console_write(" \033[1;32m"); console_write(d);
        console_write("\033[0;32m# \033[0m");
    } else if (g_th_bg == 7) {   /* paper */
        console_write("\033[1;30m"); console_write(u);
        console_write("\033[0;30m@\033[1;34m"); console_write(h);
        console_write("\033[0m \033[1;34m"); console_write(d);
        console_write("\033[0;30m# \033[0m");
    } else if (g_th_fg == 3) {   /* amber */
        console_write("\033[1;33m"); console_write(u);
        console_write("\033[0;33m@\033[1;37m"); console_write(h);
        console_write("\033[0m \033[1;33m"); console_write(d);
        console_write("\033[0;33m# \033[0m");
    } else if (g_th_fg == 6) {   /* ocean */
        console_write("\033[1;36m"); console_write(u);
        console_write("\033[0;36m@\033[1;37m"); console_write(h);
        console_write("\033[0m \033[1;34m"); console_write(d);
        console_write("\033[0;36m# \033[0m");
    } else {                     /* night */
        console_write("\033[1;37m"); console_write(u);
        console_write("\033[0;37m@\033[1;36m"); console_write(h);
        console_write("\033[0m \033[1;34m"); console_write(d);
        console_write("\033[0;37m# \033[0m");
    }
}

/* -------- panes: split console, one scrollback each --------
 *
 * A cell is a Unicode codepoint plus its colours and its style, not a byte:
 * the console decodes UTF-8 on the way in, so an accent, a box corner and a
 * Braille dot are ordinary cells. Everything is static — a terminal that
 * allocates while printing is a terminal that dies while printing.
 *
 * The screen is a binary tree of splits, like Terminator: splitting the
 * focused pane replaces it with a node holding the old pane and a new one,
 * closing a pane gives its space back to its sibling. Each pane keeps its own
 * ring, cursor and scroll position, so scrolling one does not move another.
 *
 * What this is not: two shells. There is one shell task, and it writes to the
 * focused pane. An unfocused shell pane keeps what it had; a monitor pane
 * keeps updating, because the idle hook redraws it while the shell waits for
 * a key. `pane` says so in its own output.
 */
#define SB_ROWS   300
#define SB_COLS   208               /* 1920/10 + margin; visible width is
                                       taken from the framebuffer at runtime */
#define PANE_MAX  4
#define NODE_MAX  8

#define ST_BOLD   0x01
#define ST_ITALIC 0x02

#define CONS_X0 4
#define CONS_Y0 2

enum { PANE_SHELL = 0, PANE_MONITOR = 1 };

typedef struct {
    uint16_t cp[SB_ROWS][SB_COLS];
    uint8_t  at[SB_ROWS][SB_COLS];
    uint8_t  st[SB_ROWS][SB_COLS];
    uint16_t used[SB_ROWS];
    int  row, col, fill, off;
    int  px0, py0, cols, rows;      /* geometry, in pixels and cells        */
    uint8_t live, kind;
} pane_t;

/* Layout tree. A leaf names a pane; a node splits its box in half. */
typedef struct {
    uint8_t used, leaf, dir;        /* dir: 1 = side by side, 2 = stacked   */
    uint8_t child[2], pane, parent;
} node_t;

static pane_t g_pane[PANE_MAX];
static node_t g_node[NODE_MAX];
static int    g_root, g_focus_pane, g_pane_count = 1;
static int    sb_lock;
static int    g_sgr_style;
static char   g_clip[1024];
static int    sel_on, sel_drag;
static int    sel_a_r, sel_a_c, sel_b_r, sel_b_c;

#define CUR (&g_pane[g_focus_pane])

static uint32_t decode_byte(unsigned char u);


static void pane_clear_row(pane_t *p, int r) {
    int i;
    for (i = 0; i < SB_COLS; i++) {
        p->cp[r][i] = ' ';
        p->at[r][i] = g_vga_attr;
        p->st[r][i] = 0;
    }
    p->used[r] = 0;
}

static void pane_reset(pane_t *p) {
    int r;
    p->row = p->col = p->fill = p->off = 0;
    for (r = 0; r < SB_ROWS; r++) pane_clear_row(p, r);
}

static int pane_rows_used(const pane_t *p) {
    int n = p->fill + 1;
    if (n > SB_ROWS) n = SB_ROWS;
    return n < 1 ? 1 : n;
}
static int pane_vis_used(const pane_t *p) {
    int n = pane_rows_used(p);
    if (n > p->rows) n = p->rows;
    return n < 1 ? 1 : n;
}

/* ---- layout ------------------------------------------------------------- */

static void layout_node(int id, int px, int py, int pw, int ph) {
    node_t *n = &g_node[id];
    if (n->leaf) {
        pane_t *p = &g_pane[n->pane];
        p->px0 = px; p->py0 = py;
        p->cols = pw / (int)FB_GLYPH_W;
        p->rows = ph / (int)FB_GLYPH_H;
        if (p->cols > SB_COLS) p->cols = SB_COLS;
        if (p->cols < 1) p->cols = 1;
        if (p->rows < 1) p->rows = 1;
        return;
    }
    if (n->dir == 1) {                       /* side by side, 1 px rule      */
        int half = pw / 2;
        layout_node(n->child[0], px, py, half - 1, ph);
        layout_node(n->child[1], px + half + 1, py, pw - half - 1, ph);
    } else {                                 /* stacked                      */
        int half = ph / 2;
        layout_node(n->child[0], px, py, pw, half - 1);
        layout_node(n->child[1], px, py + half + 1, pw, ph - half - 1);
    }
}

static void layout_all(void) {
    uint32_t w = 0, h = 0;
    if (!fb_active()) {
        g_pane[0].px0 = CONS_X0; g_pane[0].py0 = CONS_Y0;
        g_pane[0].cols = 80; g_pane[0].rows = 25;
        return;
    }
    fb_dims(&w, &h);
    layout_node(g_root, CONS_X0, CONS_Y0,
                (int)w - CONS_X0 * 2, (int)h - CONS_Y0 * 2);
}

static int node_alloc(void) {
    int i;
    for (i = 0; i < NODE_MAX; i++)
        if (!g_node[i].used) { g_node[i].used = 1; return i; }
    return -1;
}
static int pane_alloc(void) {
    int i;
    for (i = 0; i < PANE_MAX; i++)
        if (!g_pane[i].live) { g_pane[i].live = 1; return i; }
    return -1;
}
static int leaf_of_pane(int pane) {
    int i;
    for (i = 0; i < NODE_MAX; i++)
        if (g_node[i].used && g_node[i].leaf && g_node[i].pane == pane) return i;
    return -1;
}

static void panes_init_once(void) {
    static int done;
    if (done) return;
    done = 1;
    g_pane[0].live = 1;
    g_pane[0].kind = PANE_SHELL;
    pane_reset(&g_pane[0]);
    g_node[0].used = 1; g_node[0].leaf = 1; g_node[0].pane = 0; g_node[0].parent = 0xFF;
    g_root = 0;
    layout_all();
}

/* ---- painting ----------------------------------------------------------- */

static void paint_cell(pane_t *p, int idx, int col, int screen_row) {
    uint8_t at = p->at[idx][col];
    uint32_t fg = ANSI_RGB[at & 15], bg = ANSI_RGB[(at >> 4) & 7];
    uint32_t cp = p->cp[idx][col] ? p->cp[idx][col] : ' ';
    int style = (p->st[idx][col] & ST_BOLD)   ? 1 :
                (p->st[idx][col] & ST_ITALIC) ? 2 : 0;
    if (sel_on && p == CUR) {
        int ia = sel_a_r * SB_COLS + sel_a_c;
        int ib = sel_b_r * SB_COLS + sel_b_c;
        int ic = idx * SB_COLS + col;
        if (ia > ib) { int t = ia; ia = ib; ib = t; }
        if (ic >= ia && ic <= ib) { uint32_t x = fg; fg = bg; bg = x ^ 0x00404040u; }
    }
    fb_glyph_cp((uint32_t)(p->px0 + col * (int)FB_GLYPH_W),
                (uint32_t)(p->py0 + screen_row * (int)FB_GLYPH_H),
                cp, style, fg, bg);
}

static void pane_live_cursor(pane_t *p) {
    int used = pane_vis_used(p);
    fb_set_cursor_px((uint32_t)(p->px0 + p->col * (int)FB_GLYPH_W),
                     (uint32_t)(p->py0 + (used - 1) * (int)FB_GLYPH_H));
}

static void pane_paint(pane_t *p) {
    int used = pane_vis_used(p), r, c, idx, n;
    if (!fb_active() || !p->live) return;
    fb_fill_rect((uint32_t)p->px0, (uint32_t)p->py0,
                 (uint32_t)(p->cols * (int)FB_GLYPH_W),
                 (uint32_t)(p->rows * (int)FB_GLYPH_H), ANSI_RGB[g_th_bg & 7]);
    for (r = 0; r < used; r++) {
        idx = (p->row - (used - 1 - r) - p->off + SB_ROWS * 4) % SB_ROWS;
        n = sel_on ? p->cols : p->used[idx];
        if (n > p->cols) n = p->cols;
        for (c = 0; c < n; c++) paint_cell(p, idx, c, r);
    }
}

static void paint_borders(void) {
    int i;
    uint32_t w = 0, h = 0;
    if (!fb_active() || g_pane_count < 2) return;
    fb_dims(&w, &h);
    for (i = 0; i < PANE_MAX; i++) {
        pane_t *p = &g_pane[i];
        uint32_t col;
        if (!p->live) continue;
        col = (i == g_focus_pane) ? 0x0066AAFFu : 0x00303038u;
        /* One-pixel frame just outside the cell box. */
        fb_fill_rect((uint32_t)(p->px0 - 1), (uint32_t)(p->py0 - 1),
                     (uint32_t)(p->cols * (int)FB_GLYPH_W + 2), 1, col);
        fb_fill_rect((uint32_t)(p->px0 - 1),
                     (uint32_t)(p->py0 + p->rows * (int)FB_GLYPH_H),
                     (uint32_t)(p->cols * (int)FB_GLYPH_W + 2), 1, col);
        fb_fill_rect((uint32_t)(p->px0 - 1), (uint32_t)(p->py0 - 1), 1,
                     (uint32_t)(p->rows * (int)FB_GLYPH_H + 2), col);
        fb_fill_rect((uint32_t)(p->px0 + p->cols * (int)FB_GLYPH_W),
                     (uint32_t)(p->py0 - 1), 1,
                     (uint32_t)(p->rows * (int)FB_GLYPH_H + 2), col);
    }
}

static void sb_repaint(void) {
    int i;
    if (sb_lock) return;
    sb_lock = 1;
    panes_init_once();
    if (fb_active()) {
        uint32_t w = 0, h = 0;
        fb_dims(&w, &h);
        fb_fill_rect(0, 0, w, h, ANSI_RGB[g_th_bg & 7]);
        for (i = 0; i < PANE_MAX; i++)
            if (g_pane[i].live) pane_paint(&g_pane[i]);
        paint_borders();
        if (CUR->off == 0) pane_live_cursor(CUR);
        fb_present();
    }
    sb_lock = 0;
}

/* ---- writing ------------------------------------------------------------ */

static void pane_putcp(pane_t *p, uint32_t cp) {
    if (cp == '\r') { p->col = 0; return; }
    if (cp == '\n' || p->col >= p->cols) {
        p->row = (p->row + 1) % SB_ROWS;
        p->col = 0;
        if (p->fill < SB_ROWS) p->fill++;
        pane_clear_row(p, p->row);
        if (cp == '\n') return;
    }
    if (cp == '\b') {
        if (p->col > 0) p->col--;
        else if (p->fill > 0) {
            p->row = (p->row + SB_ROWS - 1) % SB_ROWS;
            p->col = p->cols - 1;
        }
        return;
    }
    if (cp < 0x20) return;
    p->cp[p->row][p->col] = (uint16_t)(cp > 0xFFFF ? 0xFFFD : cp);
    p->at[p->row][p->col] = g_vga_attr;
    p->st[p->row][p->col] = (uint8_t)g_sgr_style;
    p->col++;
    if (p->col > p->used[p->row]) p->used[p->row] = (uint16_t)p->col;
}

void console_caret_sync(int on) {
    if (!fb_active()) return;
    panes_init_once();
    if (CUR->off) { fb_caret(0); return; }
    pane_live_cursor(CUR);
    fb_caret(on);
}

void console_scroll(int rows) {
    pane_t *p;
    panes_init_once();
    p = CUR;
    {
        int maxoff = pane_rows_used(p) > p->rows ? pane_rows_used(p) - p->rows : 0;
        p->off += rows;
        if (p->off < 0) p->off = 0;
        if (p->off > maxoff) p->off = maxoff;
    }
    sb_repaint();
}
void console_scroll_follow(void) {
    panes_init_once();
    if (CUR->off) { CUR->off = 0; sb_repaint(); }
}
int console_scroll_offset(void) { panes_init_once(); return CUR->off; }
int console_scroll_max(void) {
    pane_t *p;
    panes_init_once();
    p = CUR;
    return pane_rows_used(p) > p->rows ? pane_rows_used(p) - p->rows : 0;
}
int console_cols(void) { panes_init_once(); return CUR->cols; }
int console_rows(void) { panes_init_once(); return CUR->rows; }

/* ---- pane management (public) ------------------------------------------- */

int console_pane_count(void) { panes_init_once(); return g_pane_count; }
int console_pane_focused(void) { panes_init_once(); return g_focus_pane; }
int console_pane_kind(int id) {
    panes_init_once();
    return (id >= 0 && id < PANE_MAX && g_pane[id].live) ? g_pane[id].kind : -1;
}
void console_pane_geometry(int id, int *cols, int *rows) {
    panes_init_once();
    if (id < 0 || id >= PANE_MAX || !g_pane[id].live) { if (cols) *cols = 0;
                                                        if (rows) *rows = 0; return; }
    if (cols) *cols = g_pane[id].cols;
    if (rows) *rows = g_pane[id].rows;
}

/* dir: 1 = side by side, 2 = stacked. Returns the new pane id or -1. */
int console_pane_split(int dir) {
    int leaf, a, b, np;
    panes_init_once();
    if (g_pane_count >= PANE_MAX) return -1;
    leaf = leaf_of_pane(g_focus_pane);
    if (leaf < 0) return -1;
    np = pane_alloc();
    if (np < 0) return -1;
    a = node_alloc();
    b = node_alloc();
    if (a < 0 || b < 0) { g_pane[np].live = 0; return -1; }

    /* The focused leaf becomes the split node; its pane moves into child a. */
    g_node[a].used = 1; g_node[a].leaf = 1; g_node[a].pane = (uint8_t)g_focus_pane;
    g_node[a].parent = (uint8_t)leaf;
    g_node[b].used = 1; g_node[b].leaf = 1; g_node[b].pane = (uint8_t)np;
    g_node[b].parent = (uint8_t)leaf;
    g_node[leaf].leaf = 0;
    g_node[leaf].dir = (uint8_t)(dir == 2 ? 2 : 1);
    g_node[leaf].child[0] = (uint8_t)a;
    g_node[leaf].child[1] = (uint8_t)b;

    g_pane[np].kind = PANE_SHELL;
    pane_reset(&g_pane[np]);
    g_pane_count++;
    layout_all();
    sb_repaint();
    return np;
}

bool console_pane_close(void) {
    int leaf, parent, sib, i;
    panes_init_once();
    if (g_pane_count < 2) return false;
    leaf = leaf_of_pane(g_focus_pane);
    if (leaf < 0) return false;
    parent = g_node[leaf].parent;
    if (parent == 0xFF) return false;
    sib = g_node[parent].child[0] == leaf ? g_node[parent].child[1]
                                          : g_node[parent].child[0];
    /* The sibling takes the parent's box; the parent slot becomes it. */
    {
        node_t keep = g_node[sib];
        uint8_t up = g_node[parent].parent;
        g_node[parent] = keep;
        g_node[parent].parent = up;
        if (!keep.leaf) {
            g_node[keep.child[0]].parent = (uint8_t)parent;
            g_node[keep.child[1]].parent = (uint8_t)parent;
        }
    }
    g_node[sib].used = 0;
    g_node[leaf].used = 0;
    g_pane[g_focus_pane].live = 0;
    g_pane_count--;
    for (i = 0; i < PANE_MAX; i++)
        if (g_pane[i].live) { g_focus_pane = i; break; }
    layout_all();
    sb_repaint();
    return true;
}

void console_pane_next(void) {
    int i, id;
    panes_init_once();
    if (g_pane_count < 2) return;
    for (i = 1; i <= PANE_MAX; i++) {
        id = (g_focus_pane + i) % PANE_MAX;
        if (g_pane[id].live) { g_focus_pane = id; break; }
    }
    sb_repaint();
}

void console_pane_set_kind(int kind) {
    panes_init_once();
    CUR->kind = (uint8_t)(kind == PANE_MONITOR ? PANE_MONITOR : PANE_SHELL);
    if (CUR->kind == PANE_MONITOR) { pane_reset(CUR); sb_repaint(); }
}

/* Write into a pane that is not the focused one (the live monitor). Colours
   and style come from whatever the shell last set, which is why the monitor
   sets its own each refresh. */
void console_pane_write(int id, const char *s) {
    pane_t *p;
    uint8_t save_attr = g_vga_attr;
    int save_bold = g_sgr_bold, save_fg = g_sgr_fg, save_bg = g_sgr_bg;
    int save_style = g_sgr_style;

    panes_init_once();
    if (id < 0 || id >= PANE_MAX || !g_pane[id].live || !s) return;
    p = &g_pane[id];
    /* Its own colours, then the shell's back: writing the monitor must not
       repaint the prompt in whatever the monitor ended on. */
    g_sgr_bold = 0; g_sgr_fg = g_th_fg; g_sgr_bg = g_th_bg; g_sgr_style = 0;
    console_palette_apply();
    for (; *s; s++) {
        uint32_t cp = decode_byte((unsigned char)*s);
        if (cp) pane_putcp(p, cp);
    }
    g_vga_attr = save_attr;
    g_sgr_bold = save_bold; g_sgr_fg = save_fg; g_sgr_bg = save_bg;
    g_sgr_style = save_style;
    if (fb_active()) { pane_paint(p); paint_borders(); fb_present(); }
}

void console_pane_clear(int id) {
    panes_init_once();
    if (id < 0 || id >= PANE_MAX || !g_pane[id].live) return;
    pane_reset(&g_pane[id]);
}

int console_pane_first_of_kind(int kind) {
    int i;
    panes_init_once();
    for (i = 0; i < PANE_MAX; i++)
        if (g_pane[i].live && g_pane[i].kind == kind) return i;
    return -1;
}

/* Swallow CSI so ESC bytes never become glyphs (boot/TUI garbage). */
static int a_st;           /* 0 normal, 1 ESC, 2 CSI */
static char a_buf[32];
static int a_n;

static void sgr_apply(void) {
    int i = 0;
    a_buf[a_n] = '\0';
    if (a_n == 0) {
        g_sgr_bold = 0; g_sgr_fg = g_th_fg; g_sgr_bg = g_th_bg;
        console_palette_apply();
        return;
    }
    while (i <= a_n) {
        int v = 0;
        while (i < a_n && a_buf[i] >= '0' && a_buf[i] <= '9')
            v = v * 10 + (a_buf[i++] - '0');
        if (v == 0) {
            g_sgr_bold = 0; g_sgr_fg = g_th_fg; g_sgr_bg = g_th_bg;
            g_sgr_style = 0;
        }
        /* Bold picks the heavier face *and* keeps the bright colour a
           terminal user expects; italic is a real slanted face. */
        else if (v == 1)  { g_sgr_bold = 8; g_sgr_style |= ST_BOLD; }
        else if (v == 3)  g_sgr_style |= ST_ITALIC;
        else if (v == 22) { g_sgr_bold = 0; g_sgr_style &= ~ST_BOLD; }
        else if (v == 23) g_sgr_style &= ~ST_ITALIC;
        else if (v >= 30 && v <= 37) g_sgr_fg = v - 30;
        else if (v >= 90 && v <= 97) g_sgr_fg = (v - 90) | 8;
        else if (v >= 40 && v <= 47) g_sgr_bg = v - 40;
        else if (v == 39) g_sgr_fg = g_th_fg;
        else if (v == 49) g_sgr_bg = g_th_bg;
        if (i < a_n && a_buf[i] == ';') i++;
        else i++;
    }
    console_palette_apply();
}

static bool ansi_eat(unsigned char c) {
    if (a_st == 0) {
        if (c == 0x1b) { a_st = 1; return true; }
        return false;
    }
    if (a_st == 1) {
        if (c == '[') { a_st = 2; a_n = 0; return true; }
        a_st = 0;
        return true;
    }
    if (c >= 0x40 && c <= 0x7e) {
        if (c == 'm') sgr_apply();
        a_st = 0;
        return true;
    }
    if (a_n < (int)sizeof(a_buf) - 1) a_buf[a_n++] = (char)c;
    return true;
}

static void vga_scroll(void) {
    for (size_t r = 1; r < VGA_ROWS; r++)
        for (size_t c = 0; c < VGA_COLS; c++)
            VGA_MEM[(r - 1) * VGA_COLS + c] = VGA_MEM[r * VGA_COLS + c];
    for (size_t c = 0; c < VGA_COLS; c++)
        VGA_MEM[(VGA_ROWS - 1) * VGA_COLS + c] = (uint16_t)((g_vga_attr << 8) | ' ');
    vga_row = VGA_ROWS - 1;
}

static void vga_putc(char c) {
    if (c == '\r') { vga_col = 0; vga_cursor_sync(); return; }
    if (c == '\b') {
        if (vga_col > 0) vga_col--;
        else if (vga_row > 0) { vga_row--; vga_col = VGA_COLS - 1; }
        vga_cursor_sync();
        return;
    }
    if (c == '\n') { vga_col = 0; vga_row++; }
    else {
        VGA_MEM[vga_row * VGA_COLS + vga_col] = (uint16_t)((g_vga_attr << 8) | (uint8_t)c);
        if (++vga_col >= VGA_COLS) { vga_col = 0; vga_row++; }
    }
    if (vga_row >= VGA_ROWS) vga_scroll();
    vga_cursor_sync();
}

static void vga_clear(void) {
    for (size_t i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_MEM[i] = (uint16_t)((g_vga_attr << 8) | ' ');
    vga_row = vga_col = 0;
    vga_cursor_sync();
}

/* --- COM1 serial (16550 UART) -------------------------------------------- */
#define COM1 0x3f8

/* True only when a real UART answers at COM1. This matters because VirtualBox
   VMs ship with NO serial port by default: reads of an absent I/O port float
   the ISA bus to 0xFF, so the LSR "data ready" bit reads 1 forever and the
   data register hands back an endless stream of 0xFF ('ÿ') bytes. The UI event
   loop polls serial before the keyboard, so that phantom stream floods the
   prompt and starves PS/2 input — the frozen cursor + "ÿÿÿ…" seen on
   VirtualBox. We probe once at init and, if nothing is there, disable the
   serial path entirely. QEMU always emulates a real 16550, so headless/serial
   workflows (smoke test, net-test serial logs) are unaffected. */
static bool g_serial;

/* 16550 presence probe: the scratch register (COM1+7) holds whatever we write;
   an absent/floating port reads back something else (0xFF on VirtualBox). Two
   distinct values guard against a bus that happens to float to one of them. */
static bool serial_probe(void) {
    outb(COM1 + 7, 0xae);
    if (inb(COM1 + 7) != 0xae) return false;
    outb(COM1 + 7, 0x51);
    if (inb(COM1 + 7) != 0x51) return false;
    return true;
}

static void serial_init(void) {
    g_serial = serial_probe();
    if (!g_serial) return;                    /* no UART: leave the port alone */
    outb(COM1 + 1, 0x00);   /* disable interrupts            */
    outb(COM1 + 3, 0x80);   /* enable DLAB                   */
    outb(COM1 + 0, 0x03);   /* divisor low  (38400 baud)     */
    outb(COM1 + 1, 0x00);   /* divisor high                  */
    outb(COM1 + 3, 0x03);   /* 8 bits, no parity, one stop   */
    outb(COM1 + 2, 0xc7);   /* enable + clear FIFO           */
    outb(COM1 + 4, 0x0b);   /* IRQs enabled, RTS/DSR set     */
}

static void serial_putc(char c) {
    if (!g_serial) return;
    /* Bounded wait for THR empty so a wedged UART can't hang the console. */
    for (int i = 0; i < 100000 && (inb(COM1 + 5) & 0x20) == 0; i++) { }
    outb(COM1, (uint8_t)c);
}

static bool serial_has_input(void) {
    if (!g_serial) return false;
    uint8_t lsr = inb(COM1 + 5);
    if (lsr == 0xff) return false;            /* floating bus, not a real byte */
    return (lsr & 0x01) != 0;                 /* LSR: data ready */
}

/* --- public API ---------------------------------------------------------- */
void console_init(void) {
    serial_init();
    g_vga_attr = VGA_ATTR;
    g_sgr_bold = 0; g_sgr_fg = 7; g_sgr_bg = 0;
    a_st = 0;
    vga_clear();
    vga_cursor_enable();
    /* Reset host terminal: black bg, white fg, blinking caret.
       1000h/1006h: X10 + SGR mouse so a serial wheel reaches the guest. */
    console_serial_write("\033[0m\033[37;40m\033[?25h\033[5 q\033[?1000h\033[?1006h");
}

/* Decode UTF-8 as it arrives. The serial side gets the raw bytes (a real
   terminal on the other end decodes them itself); the framebuffer side gets
   codepoints, because that is what the face is indexed by. */
static uint32_t u8_cp;
static int      u8_left;

/* Returns the codepoint to store, or 0 when the byte was consumed by an
   escape sequence or is a partial UTF-8 sequence. Shared by every pane so a
   monitor pane understands the same colours and accents as the shell. */
static uint32_t decode_byte(unsigned char u) {
    uint32_t cp;
    if (ansi_eat(u)) { u8_left = 0; return 0; }

    if (u < 0x80) {
        u8_left = 0;
        cp = u;
    } else if ((u & 0xC0) == 0x80) {          /* continuation byte          */
        if (u8_left <= 0) return 0;           /* stray: drop it, not a glyph */
        u8_cp = (u8_cp << 6) | (u & 0x3F);
        if (--u8_left) return 0;
        cp = u8_cp;
    } else {                                  /* leading byte               */
        if ((u & 0xE0) == 0xC0)      { u8_cp = u & 0x1F; u8_left = 1; }
        else if ((u & 0xF0) == 0xE0) { u8_cp = u & 0x0F; u8_left = 2; }
        else if ((u & 0xF8) == 0xF0) { u8_cp = u & 0x07; u8_left = 3; }
        else { u8_left = 0; return 0; }       /* 0xFE/0xFF are not UTF-8    */
        return 0;
    }

    /* A codepoint the baked face has no glyph for is drawn as U+FFFD, and
       that is deliberate: an empty cell would hide the fact that the text
       said something this build cannot show. */
    if (cp >= 0x20 && !font_has(cp)) cp = 0xFFFD;
    return cp;
}

void console_putc(char c) {
    unsigned char u = (unsigned char)c;
    uint32_t cp;

    if (c == '\n') serial_putc('\r');
    serial_putc(c);
    cp = decode_byte(u);
    if (!cp) return;

    {
        pane_t *p;
        int row0;
        panes_init_once();
        p = CUR;
        row0 = p->row;
        pane_putcp(p, cp);
        if (p->off) return;        /* viewing history: don't clobber the view */
        if (!fb_active()) { vga_putc((char)(cp < 0x80 ? cp : '?')); return; }
        fb_caret(0);
        if (p->row != row0) {
            if (pane_vis_used(p) >= p->rows) {
                /* The pane is full: everything in it moves up by one row. */
                pane_paint(p);
                paint_borders();
                pane_live_cursor(p);
                fb_present();
                return;
            }
            fb_fill_rect((uint32_t)p->px0,
                         (uint32_t)(p->py0 + (pane_vis_used(p) - 1) * (int)FB_GLYPH_H),
                         (uint32_t)(p->cols * (int)FB_GLYPH_W), FB_GLYPH_H,
                         ANSI_RGB[g_th_bg & 7]);
        }
        if (cp >= 0x20 && p->col > 0) {
            paint_cell(p, p->row, p->col - 1, pane_vis_used(p) - 1);
            fb_present();
        }
        pane_live_cursor(p);
    }
}

void console_write(const char *s) {
    for (; *s; s++) console_putc(*s);
}

void console_clear(void) {
    int r, c;
    g_sgr_bold = 0; g_sgr_fg = g_th_fg; g_sgr_bg = g_th_bg;
    console_palette_apply();
    g_sgr_style = 0;
    panes_init_once();
    pane_reset(CUR);
    (void)r; (void)c;
    console_serial_write("\033[2J\033[H\033[0m\033[?25h");
    if (fb_active()) fb_clear();
    else vga_clear();
}

void console_clipboard_set(const char *s) {
    int i = 0;
    if (!s) { g_clip[0] = '\0'; return; }
    while (s[i] && i + 1 < (int)sizeof(g_clip)) { g_clip[i] = s[i]; i++; }
    g_clip[i] = '\0';
}
const char *console_clipboard(void) { return g_clip; }

void console_sel_copy(void) {
    int ar = sel_a_r, ac = sel_a_c, br = sel_b_r, bc = sel_b_c;
    int da, db, n = 0, last_r = -1, r, c;
    if (!sel_on) return;
    /* Age from the live row: larger = older. Keeps wrap-around selections
       from dumping the whole 240-row ring. */
    da = (CUR->row - ar + SB_ROWS) % SB_ROWS;
    db = (CUR->row - br + SB_ROWS) % SB_ROWS;
    if (da < db || (da == db && ac > bc)) {
        int t = ar; ar = br; br = t;
        t = ac; ac = bc; bc = t;
    }
    r = ar; c = ac;
    for (;;) {
        uint32_t cp = CUR->cp[r][c] ? CUR->cp[r][c] : ' ';
        if (last_r >= 0 && r != last_r && n + 2 < (int)sizeof(g_clip))
            g_clip[n++] = '\n';
        /* Back to UTF-8: the clipboard is bytes, and pasting a box-drawing
           character has to give back the same bytes it came from. */
        if (cp < 0x80) {
            if (n + 1 < (int)sizeof(g_clip)) g_clip[n++] = (char)cp;
        } else if (cp < 0x800) {
            if (n + 2 < (int)sizeof(g_clip)) {
                g_clip[n++] = (char)(0xC0 | (cp >> 6));
                g_clip[n++] = (char)(0x80 | (cp & 0x3F));
            }
        } else {
            if (n + 3 < (int)sizeof(g_clip)) {
                g_clip[n++] = (char)(0xE0 | (cp >> 12));
                g_clip[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                g_clip[n++] = (char)(0x80 | (cp & 0x3F));
            }
        }
        last_r = r;
        if (r == br && c == bc) break;
        c++;
        if (c >= SB_COLS) { c = 0; r = (r + 1) % SB_ROWS; }
    }
    while (n > 0 && (g_clip[n - 1] == ' ' || g_clip[n - 1] == '\n')) n--;
    g_clip[n] = '\0';
}

void console_sel_pointer(int px, int py, int buttons) {
    pane_t *p;
    int used, col, vrow, sbr, left = buttons & 1;
    panes_init_once();
    p = CUR;
    used = pane_vis_used(p);
    col = (px - p->px0) / (int)FB_GLYPH_W;
    vrow = (py - p->py0) / (int)FB_GLYPH_H;
    if (col < 0) col = 0;
    if (col >= p->cols) col = p->cols - 1;
    if (vrow < 0) vrow = 0;
    if (vrow >= used) vrow = used - 1;
    sbr = (p->row - (used - 1 - vrow) - p->off + SB_ROWS * 4) % SB_ROWS;
    if (left && !sel_drag) {
        sel_drag = 1; sel_on = 1;
        sel_a_r = sel_b_r = sbr;
        sel_a_c = sel_b_c = col;
        sb_repaint();
    } else if (left && sel_drag) {
        sel_b_r = sbr; sel_b_c = col;
        sb_repaint();
    } else if (!left && sel_drag) {
        sel_drag = 0;
        sel_b_r = sbr; sel_b_c = col;
        console_sel_copy();
        sb_repaint();
    }
}

void console_page(const char *text) {
    int vis = console_rows();
    int page = vis > 4 ? vis - 2 : 20;
    int lines = 0;
    const char *p = text ? text : "";
    console_write("\n");
    while (*p) {
        while (*p && *p != '\n') console_putc(*p++);
        if (*p == '\n') console_putc(*p++);
        else console_putc('\n');
        lines++;
        if (lines >= page && *p) {
            console_write("\033[1;33m-- more --\033[0m  space next   b back   q quit\n");
            for (;;) {
                unsigned char u = (unsigned char)console_getc();
                if (u == 'q' || u == 'Q' || u == 0x1b) {
                    console_write("\n");
                    return;
                }
                if (u == ' ' || u == '\n' || u == '\r' || u == KEY_PGDN || u == KEY_DOWN)
                    break;
                if (u == 'b' || u == 'B' || u == KEY_PGUP || u == KEY_UP) {
                    console_scroll(page);
                }
            }
            lines = 0;
        }
    }
}

void console_write_hex(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    console_write("0x");
    for (int i = 60; i >= 0; i -= 4)
        console_putc(digits[(v >> i) & 0xf]);
}

void console_write_dec(uint64_t v) {
    char buf[21];
    int i = 20;
    buf[i--] = '\0';
    if (v == 0) { console_putc('0'); return; }
    while (v && i >= 0) { buf[i--] = '0' + (v % 10); v /= 10; }
    console_write(&buf[i + 1]);
}

/* Serial-only output: used to mirror graphical pages to a serial terminal
   (so `-nographic` / headless users still see page content as text). */
void console_serial_write(const char *s) {
    for (; *s; s++) { if (*s == '\n') serial_putc('\r'); serial_putc(*s); }
}

/* Non-blocking serial read for the UI event loop: -1 if no byte waiting. */
bool console_serial_has_input(void) { return serial_has_input(); }
int  console_serial_getc_nb(void) {
    return serial_has_input() ? (int)(unsigned char)inb(COM1) : -1;
}

bool console_has_input(void) { return serial_has_input() || kbd_has_input(); }

/* Read one character from whichever input is ready: COM1 serial OR the PS/2
   keyboard. Works in both input modes: with IRQ input active the keyboard
   ISR posts chars to the fabric input ring (mouse events are skipped here —
   this is a raw char API); in polled mode it reads the driver directly. */
char console_getc(void) {
    /* Blocking on input is the frame boundary for a text UI: whatever was
       drawn has to be on screen before we go to sleep waiting for a key. */
    fb_present();
    for (;;) {
        if (serial_has_input()) return (char)inb(COM1);
        if (events_irq_active()) {
            rx_input_t in;
            while (rx_input_pop(&in))
                if (in.kind == RX_INPUT_KEY) return in.ch;   /* skip mouse */
            __asm__ volatile ("hlt");
        } else {
            int k = kbd_poll_char();
            if (k >= 0) return (char)k;
            __asm__ volatile ("pause");
        }
    }
}
