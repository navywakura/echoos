/*
 * Mini editor. Click a line to place the caret — no arrows/selection/undo yet.
 * Good enough to write .rxc apps. Missing features are not hidden.
 */
#include "edit.h"
#include "desktop.h"
#include "framebuffer.h"
#include "rxfs.h"
#include "roxenite.h"

#define E_LINES 48
#define E_COLS  110
#define TB_SAVE 1
#define TB_RUN  2
#define TB_NEW  3
#define TB_EXIT 4
#define LINE_ID 1000          /* line i click id = LINE_ID + i */

static char e_buf[E_LINES][E_COLS];
static int  e_len[E_LINES];
static int  e_nlines, e_cline, e_ccol, e_top;
static char e_name[RXFS_NAME];
static char e_status[64];

/* content geometry */
#define CX      220           /* == render CONTENT area x (NAV_W + 20) */
#define TEXT_X  (CX + 48)      /* text starts after the line-number gutter */
#define TOP_Y   96
#define LINE_H  18

static void put_status(const char *s) { int i = 0; for (; s[i] && i < 63; i++) e_status[i] = s[i]; e_status[i] = '\0'; }

static void num_str(int v, char *out) {
    char t[8]; int i = 0;
    if (v == 0) t[i++] = '0';
    while (v && i < 7) { t[i++] = '0' + v % 10; v /= 10; }
    int j = 0; while (i > 0) out[j++] = t[--i]; out[j] = '\0';
}

static int vis_lines(void) { uint32_t w, h; fb_dims(&w, &h); return (int)((h - TOP_Y - 44) / LINE_H); }

static void load(const char *name) {
    put_status("");
    { int i = 0; for (; name[i] && i < RXFS_NAME - 1; i++) e_name[i] = name[i]; e_name[i] = '\0'; }
    e_nlines = 0; e_cline = 0; e_ccol = 0; e_top = 0;
    for (int i = 0; i < E_LINES; i++) e_len[i] = 0;

    uint32_t len = 0;
    const char *data = rxfs_read(name, &len);
    if (!data) { e_nlines = 1; return; }
    int l = 0, c = 0;
    for (uint32_t i = 0; i < len && l < E_LINES; i++) {
        if (data[i] == '\n') { e_len[l] = c; l++; c = 0; }
        else if (c < E_COLS - 1) { e_buf[l][c++] = data[i]; }
    }
    e_len[l] = c;
    e_nlines = l + 1;
}

static uint32_t join(char *out, uint32_t max) {
    uint32_t n = 0;
    for (int i = 0; i < e_nlines; i++) {
        for (int c = 0; c < e_len[i] && n < max - 1; c++) out[n++] = e_buf[i][c];
        if (i < e_nlines - 1 && n < max - 1) out[n++] = '\n';
    }
    out[n] = '\0';
    return n;
}

static void draw(void) {
    fb_cursor_hide();
    char title[80]; int t = 0;
    const char *pfx = "Editor - ";
    for (int i = 0; pfx[i]; i++) title[t++] = pfx[i];
    for (int i = 0; e_name[i] && t < 78; i++) title[t++] = e_name[i];
    title[t] = '\0';
    render_begin_page(title, "rgx://editor");

    /* toolbar */
    uint32_t bx = CX, by = 54;
    struct { const char *l; int id; } tb[] = { {"Save", TB_SAVE}, {"Run", TB_RUN}, {"New", TB_NEW}, {"Exit", TB_EXIT} };
    for (int i = 0; i < 4; i++) {
        uint32_t bw = (uint32_t)k_strlen(tb[i].l) * 8 + 22;
        fb_fill_rect(bx, by, bw, 26, RGX_BTN);
        fb_rect_outline(bx, by, bw, 26, RGX_BAR);
        fb_text(bx + 11, by + 5, tb[i].l, RGX_BTN_TXT, RGX_BTN);
        render_add_hit(bx, by, bw, 26, tb[i].id);
        bx += bw + 12;
    }
    fb_text(bx + 10, by + 5, e_status, RGX_DIM, RGX_PANEL);

    /* lines */
    int vl = vis_lines();
    if (e_cline < e_top) e_top = e_cline;
    if (e_cline >= e_top + vl) e_top = e_cline - vl + 1;
    /* The selection bar spans the editor pane, whose width is the screen's -
       not a constant. A hardcoded 1280 leaves an unpainted strip on a 1366x768
       laptop panel and overshoots on anything narrower. */
    uint32_t sw, sh; fb_dims(&sw, &sh); (void)sh;
    uint32_t bar_w = sw > CX + 8 ? sw - CX - 8 : 0;
    for (int r = 0; r < vl; r++) {
        int i = e_top + r;
        if (i >= e_nlines) break;
        uint32_t y = TOP_Y + r * LINE_H;
        if (i == e_cline) fb_fill_rect(CX - 6, y - 2, bar_w, LINE_H, RGX_NAV_SEL);
        char ln[8]; num_str(i + 1, ln);
        fb_text(CX, y, ln, RGX_DIM, i == e_cline ? RGX_NAV_SEL : RGX_PANEL);
        /* line text (NUL-terminate a copy) */
        char row[E_COLS]; int c = 0; for (; c < e_len[i] && c < E_COLS - 1; c++) row[c] = e_buf[i][c]; row[c] = '\0';
        fb_text(TEXT_X, y, row, RGX_FG, i == e_cline ? RGX_NAV_SEL : RGX_PANEL);
        render_add_hit(CX - 6, y - 2, bar_w, LINE_H, LINE_ID + i);
        if (i == e_cline) {                        /* caret */
            uint32_t caretx = TEXT_X + (uint32_t)e_ccol * 8;
            fb_fill_rect(caretx, y, 2, 15, RGX_WHITE);
        }
    }
    /* hint */
    uint32_t w, h; fb_dims(&w, &h);
    fb_text(CX, h - 24, "Click a line to place the caret. Type to edit. Enter = new line.",
            RGX_DIM, RGX_PANEL);
    fb_cursor_show();
}

static void insert_char(char ch) {
    if (e_len[e_cline] >= E_COLS - 1) return;
    for (int c = e_len[e_cline]; c > e_ccol; c--) e_buf[e_cline][c] = e_buf[e_cline][c - 1];
    e_buf[e_cline][e_ccol] = ch;
    e_len[e_cline]++; e_ccol++;
}

static void newline(void) {
    if (e_nlines >= E_LINES) return;
    for (int i = e_nlines; i > e_cline + 1; i--) {
        k_memcpy(e_buf[i], e_buf[i - 1], E_COLS); e_len[i] = e_len[i - 1];
    }
    int tail = e_len[e_cline] - e_ccol;
    for (int c = 0; c < tail; c++) e_buf[e_cline + 1][c] = e_buf[e_cline][e_ccol + c];
    e_len[e_cline + 1] = tail;
    e_len[e_cline] = e_ccol;
    e_nlines++; e_cline++; e_ccol = 0;
}

static void backspace(void) {
    if (e_ccol > 0) {
        for (int c = e_ccol - 1; c < e_len[e_cline] - 1; c++) e_buf[e_cline][c] = e_buf[e_cline][c + 1];
        e_len[e_cline]--; e_ccol--;
    } else if (e_cline > 0) {
        int prev = e_cline - 1, at = e_len[prev];
        for (int c = 0; c < e_len[e_cline] && at + c < E_COLS - 1; c++) e_buf[prev][at + c] = e_buf[e_cline][c];
        e_len[prev] = at + e_len[e_cline];
        for (int i = e_cline; i < e_nlines - 1; i++) { k_memcpy(e_buf[i], e_buf[i + 1], E_COLS); e_len[i] = e_len[i + 1]; }
        e_nlines--; e_cline = prev; e_ccol = at;
    }
}

static void do_save(void) {
    static char out[E_LINES * E_COLS];
    uint32_t n = join(out, sizeof(out));
    put_status(rxfs_write(e_name, out, n) ? "saved." : "save failed (fs full).");
}

static void do_run(void) {
    static char out[E_LINES * E_COLS];
    join(out, sizeof(out));
    static rxc_program prog;
    rxc_compile(out, &prog);
    char open_url[64];
    rxc_run(&prog, open_url, sizeof(open_url));   /* interactive; returns on 'back' */
    put_status("returned from run.");
}

int edit_run(const char *name) {
    if (!fb_active()) { console_write("  [edit] the editor needs the graphical framebuffer.\n"); return -1; }
    load(name);
    put_status("new file. click a line, type, then Save.");
    draw();

    for (;;) {
        char ch; int click;
        int e = ui_poll(&ch, &click);
        if (e == 0) continue;
        if (e == 2) {
            /* Only the actual left-nav tab range is a nav click. This range
               check is essential: LINE_ID (1000) is > RENDER_NAV_BASE (900),
               so a bare `click >= RENDER_NAV_BASE` would treat every line
               click as a nav tab, make edit_run return, and the shell would
               re-enter the editor (reloading an empty file) — i.e. "clicking
               a line wipes everything". */
            if (click >= RENDER_NAV_BASE && click < RENDER_NAV_BASE + NAV_COUNT)
                return click - RENDER_NAV_BASE;                 /* nav tab */
            if (click == TB_EXIT) return -1;
            if (click == TB_SAVE) { do_save(); draw(); continue; }
            if (click == TB_RUN)  { do_run();  draw(); continue; }
            if (click == TB_NEW)  { e_nlines = 1; e_len[0] = 0; e_cline = e_ccol = e_top = 0; put_status("cleared."); draw(); continue; }
            if (click >= LINE_ID) {
                int i = click - LINE_ID;
                if (i < e_nlines) {
                    e_cline = i;
                    uint32_t cx, cy; ui_cursor_xy(&cx, &cy);
                    int col = ((int)cx - TEXT_X) / 8;
                    if (col < 0) col = 0;
                    if (col > e_len[i]) col = e_len[i];
                    e_ccol = col;
                }
                draw();
            }
            continue;
        }
        /* e == 1: keyboard char */
        if (ch == '\n' || ch == '\r') newline();
        else if (ch == 0x08 || ch == 0x7f) backspace();
        else { unsigned char uc = (unsigned char)ch; if (uc >= 0x20 && uc != 0x7f) insert_char(ch); }
        draw();
    }
}
