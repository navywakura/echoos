/*
 * Buffered TUI. Cells go into a grid; tui_end() paints framebuffer + one
 * compact serial frame. Per-cell ANSI was flooding COM1 (~100 KiB/frame)
 * and showing as garbage.
 */
#include "tui.h"
#include "framebuffer.h"

#define VGA_MEM  ((volatile uint16_t *)0xB8000)
#define VGA_COLS 80
#define VGA_ROWS 25

/* Codepoints, not bytes. Every string in this tree is UTF-8, and a grid of
   chars gives each byte of a multi-byte character its own cell: the middle
   dot in the installer's subtitle came out as two Latin-1 letters on the
   first screen of the product. The console proper decodes UTF-8; this layer
   now does too, so both agree about what a column is. */
static uint32_t g_ch[TUI_ROWS][TUI_COLS];
static uint32_t g_fg[TUI_ROWS][TUI_COLS];
static uint32_t g_bg[TUI_ROWS][TUI_COLS];
static uint32_t g_last_ch;
static int      g_ox, g_oy;

static uint8_t rgb_to_vga(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    uint8_t c = 0;
    if (r > 80) c |= 4;
    if (g > 80) c |= 2;
    if (b > 80) c |= 1;
    if (r + g + b > 360) c |= 8;
    return c;
}

static void serial_puts(const char *s) { console_serial_write(s); }

static void serial_sgr(uint32_t fg, uint32_t bg) {
    /* 16-colour ANSI, not 24-bit. One short sequence per run. */
    uint8_t f = rgb_to_vga(fg), b = rgb_to_vga(bg);
    char buf[24];
    int n = 0;
    buf[n++] = '\033'; buf[n++] = '[';
    buf[n++] = '0'; buf[n++] = ';';
    if (f & 8) { buf[n++] = '1'; buf[n++] = ';'; }
    buf[n++] = '3'; buf[n++] = (char)('0' + (f & 7)); buf[n++] = ';';
    buf[n++] = '4'; buf[n++] = (char)('0' + (b & 7)); buf[n++] = 'm';
    buf[n] = '\0';
    serial_puts(buf);
}

void tui_cell(int col, int row, uint32_t cp, uint32_t fg, uint32_t bg) {
    if (col < 0 || row < 0 || col >= TUI_COLS || row >= TUI_ROWS) return;
    if (cp < 0x20 || cp == 0x7f) cp = ' ';
    g_ch[row][col] = cp;
    g_fg[row][col] = fg;
    g_bg[row][col] = bg;
}

/* One UTF-8 sequence, one column. Malformed input is drawn as the byte it
   is rather than swallowed, so a bad string looks wrong instead of moving
   everything after it. */
static uint32_t utf8_next(const char **pp) {
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t cp = *p++;
    int extra = 0;
    if (cp >= 0xF0)      { cp &= 0x07; extra = 3; }
    else if (cp >= 0xE0) { cp &= 0x0F; extra = 2; }
    else if (cp >= 0xC0) { cp &= 0x1F; extra = 1; }
    while (extra--) {
        if ((*p & 0xC0) != 0x80) { *pp = (const char *)p; return '?'; }
        cp = (cp << 6) | (*p++ & 0x3F);
    }
    *pp = (const char *)p;
    return cp;
}

void tui_text(int col, int row, const char *s, uint32_t fg, uint32_t bg) {
    if (!s) return;
    while (*s && col < TUI_COLS) {
        tui_cell(col, row, utf8_next(&s), fg, bg);
        col++;
    }
}

void tui_fill(int col, int row, int w, int h, uint32_t c, uint32_t fg, uint32_t bg) {
    int r, k;
    for (r = 0; r < h; r++)
        for (k = 0; k < w; k++)
            tui_cell(col + k, row + r, c, fg, bg);
}

static void serial_itoa(char *b, int *n, int v) {
    char d[8]; int nd = 0;
    if (v < 0) v = 0;
    if (!v) d[nd++] = 0;
    while (v && nd < 8) { d[nd++] = (char)(v % 10); v /= 10; }
    while (nd) b[(*n)++] = (char)('0' + d[--nd]);
}

static void serial_cup(int row1, int col1) {
    char b[24];
    int n = 0;
    b[n++] = '\033'; b[n++] = '[';
    serial_itoa(b, &n, row1);
    b[n++] = ';';
    serial_itoa(b, &n, col1);
    b[n++] = 'H';
    b[n] = '\0';
    serial_puts(b);
}

void tui_begin(void) {
    int r, c;
    uint32_t w = 0, h = 0;
    g_ox = g_oy = 0;
    if (fb_active()) fb_caret(0);
    for (r = 0; r < TUI_ROWS; r++)
        for (c = 0; c < TUI_COLS; c++) {
            g_ch[r][c] = ' ';
            g_fg[r][c] = TUI_FG;
            g_bg[r][c] = TUI_BG;
        }
    if (fb_active()) {
        fb_dims(&w, &h);
        g_ox = ((int)w / (int)FB_GLYPH_W - TUI_COLS) / 2;
        g_oy = ((int)h / (int)FB_GLYPH_H - TUI_ROWS) / 2;
        if (g_ox < 0) g_ox = 0;
        if (g_oy < 0) g_oy = 0;
        fb_fill_rect(0, 0, w, h, TUI_BG);
    }
}

void tui_end(void) {
    int r, c;
    uint32_t last_fg = 1, last_bg = 1;

    if (fb_active()) {
        uint32_t w, h;
        fb_dims(&w, &h);
        fb_fill_rect(0, 0, w, h, TUI_BG);
        for (r = 0; r < TUI_ROWS; r++) {
            int c0 = 0;
            uint32_t y = (uint32_t)((g_oy + r) * (int)FB_GLYPH_H);
            /* One call per cell rather than per run: a run painter takes
               bytes, and this grid holds codepoints. Two thousand glyphs on
               a screen that repaints when a key is pressed is not a cost
               worth trading correctness for. */
            for (c0 = 0; c0 < TUI_COLS; c0++) {
                uint32_t x = (uint32_t)((g_ox + c0) * (int)FB_GLYPH_W);
                fb_glyph_cp(x, y, g_ch[r][c0], 0, g_fg[r][c0], g_bg[r][c0]);
            }
        }
        fb_present();
    } else {
        for (r = 0; r < TUI_ROWS; r++)
            for (c = 0; c < TUI_COLS; c++) {
                uint32_t cp = g_ch[r][c];
                uint8_t attr = (uint8_t)((rgb_to_vga(g_bg[r][c]) << 4) |
                                         (rgb_to_vga(g_fg[r][c]) & 0x0F));
                /* VGA text mode has one byte per cell and no room for a
                   codepoint; say so with a question mark rather than
                   truncating into a different letter. */
                if (cp < 0x20 || cp > 0x7e) cp = (cp == ' ') ? ' ' : '?';
                VGA_MEM[r * VGA_COLS + c] =
                    (uint16_t)((attr << 8) | (uint8_t)cp);
            }
    }

    /* Absolute CUP per row. A raw 80-char line + CR on an 80-col host
       terminal wraps then \r returns to the SAME visual row, so nano
       painted every file line on top of the first. */
    serial_puts("\033[?25l\033[2J");
    for (r = 0; r < TUI_ROWS; r++) {
        serial_cup(r + 1, 1);
        for (c = 0; c < TUI_COLS; c++) {
            uint32_t cp = g_ch[r][c];
            if (cp < 0x20 || cp == 0x7f) cp = ' ';
            if (g_fg[r][c] != last_fg || g_bg[r][c] != last_bg) {
                serial_sgr(g_fg[r][c], g_bg[r][c]);
                last_fg = g_fg[r][c];
                last_bg = g_bg[r][c];
            }
            { char s[5]; int n = 0;
              /* Back to UTF-8 on the wire: the terminal on the other end
                 expects the same encoding the source was written in. */
              if (cp < 0x80) s[n++] = (char)cp;
              else if (cp < 0x800) {
                  s[n++] = (char)(0xC0 | (cp >> 6));
                  s[n++] = (char)(0x80 | (cp & 0x3F));
              } else if (cp < 0x10000) {
                  s[n++] = (char)(0xE0 | (cp >> 12));
                  s[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                  s[n++] = (char)(0x80 | (cp & 0x3F));
              } else {
                  s[n++] = (char)(0xF0 | (cp >> 18));
                  s[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                  s[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                  s[n++] = (char)(0x80 | (cp & 0x3F));
              }
              s[n] = 0; serial_puts(s); }
        }
    }
    serial_puts("\033[0m");
}

void tui_hbar(int col, int row, int w, uint32_t fg) {
    int i;
    for (i = 0; i < w; i++) tui_cell(col + i, row, '-', fg, TUI_BG);
}

void tui_box(int col, int row, int w, int h, const char *title) {
    int x, y;
    if (w < 4 || h < 3) return;
    tui_cell(col, row, '+', TUI_BORDER, TUI_BG);
    tui_cell(col + w - 1, row, '+', TUI_BORDER, TUI_BG);
    tui_cell(col, row + h - 1, '+', TUI_BORDER, TUI_BG);
    tui_cell(col + w - 1, row + h - 1, '+', TUI_BORDER, TUI_BG);
    for (x = 1; x < w - 1; x++) {
        tui_cell(col + x, row, '-', TUI_BORDER, TUI_BG);
        tui_cell(col + x, row + h - 1, '-', TUI_BORDER, TUI_BG);
    }
    for (y = 1; y < h - 1; y++) {
        tui_cell(col, row + y, '|', TUI_BORDER, TUI_BG);
        tui_cell(col + w - 1, row + y, '|', TUI_BORDER, TUI_BG);
        for (x = 1; x < w - 1; x++)
            tui_cell(col + x, row + y, ' ', TUI_FG, TUI_BG);
    }
    if (title && title[0]) {
        int tx = col + 2;
        tui_text(tx, row, " ", TUI_GOLD, TUI_BG);
        tui_text(tx + 1, row, title, TUI_GOLD, TUI_BG);
        tui_text(tx + 1 + (int)k_strlen(title), row, " ", TUI_GOLD, TUI_BG);
    }
}

void tui_item(int col, int row, int w, const char *label, bool selected) {
    uint32_t fg = selected ? TUI_HI_FG : TUI_FG;
    uint32_t bg = selected ? TUI_HI_BG : TUI_BG;
    int i, lab = (int)k_strlen(label ? label : "");
    tui_fill(col, row, w, 1, ' ', fg, bg);
    tui_cell(col + 1, row, selected ? '>' : ' ', fg, bg);
    tui_text(col + 3, row, label ? label : "", fg, bg);
    for (i = 3 + lab; i < w - 1; i++)
        tui_cell(col + i, row, ' ', fg, bg);
}

void tui_hint(int row, const char *s) {
    tui_fill(2, row, TUI_COLS - 4, 1, ' ', TUI_DIM, TUI_BG);
    tui_text(4, row, s ? s : "", TUI_DIM, TUI_BG);
}

char tui_last_char(void) { return g_last_ch; }

static int map_key(int c) {
    g_last_ch = 0;
    if (c == KEY_UP)    return TUI_KEY_UP;
    if (c == KEY_DOWN)  return TUI_KEY_DOWN;
    if (c == KEY_LEFT)  return TUI_KEY_LEFT;
    if (c == KEY_RIGHT) return TUI_KEY_RIGHT;
    if (c == '\n' || c == '\r') return TUI_KEY_ENTER;
    if (c == 0x1b)      return TUI_KEY_ESC;
    if (c == '\b' || c == 0x7f) return TUI_KEY_BACK;
    if (c == '\t')      return TUI_KEY_TAB;
    if (c == 'q' || c == 'Q') { g_last_ch = (char)c; return TUI_KEY_Q; }
    if (c >= 32 && c < 127) { g_last_ch = (char)c; return TUI_KEY_CHAR; }
    return TUI_KEY_NONE;
}

tui_key tui_read_key(void) {
    for (;;) {
        int c = (int)(unsigned char)console_getc();
        if (c == 0x1b) {
            int n = 0;
            while (n++ < 4000 && !console_serial_has_input() && !kbd_has_input())
                { }
            if (console_serial_has_input()) {
                int n1 = console_serial_getc_nb();
                if (n1 == '[') {
                    int n2 = -1, w = 0;
                    while (w++ < 4000 && n2 < 0) n2 = console_serial_getc_nb();
                    if (n2 == 'A') return TUI_KEY_UP;
                    if (n2 == 'B') return TUI_KEY_DOWN;
                    if (n2 == 'C') return TUI_KEY_RIGHT;
                    if (n2 == 'D') return TUI_KEY_LEFT;
                }
            }
            return TUI_KEY_ESC;
        }
        {
            tui_key k = (tui_key)map_key(c);
            if (k != TUI_KEY_NONE) return k;
        }
    }
}
