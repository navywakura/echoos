/*
 * nano / pico — line-numbered TUI editor with light syntax colouring.
 * Languages: C, Python, JavaScript, HTML, Rust. Heap-0: static buffers.
 */
#include "nano.h"
#include "tui.h"
#include "vfs.h"
#include "rogex.h"
#include "framebuffer.h"
#include "commands.h"

#define NL  200
#define NC  72

static char  g_lines[NL][NC + 1];
static int   g_n, g_cx, g_cy, g_top, g_dirty;
static char  g_path[RXFS_NAME];
static int   g_lang;   /* 0 none 1 c 2 py 3 js 4 html 5 rust */

static int slen(const char *s) { int n = 0; if (s) while (s[n]) n++; return n; }
static void scpy(char *d, const char *s, int max) {
    int i = 0; if (!d || max < 1) return;
    if (s) while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static int detect_lang(const char *p) {
    int n = slen(p);
    if (n >= 2 && p[n-2]=='.' && p[n-1]=='c') return 1;
    if (n >= 2 && p[n-2]=='.' && p[n-1]=='h') return 1;
    if (n >= 3 && p[n-3]=='.' && p[n-2]=='p' && p[n-1]=='y') return 2;
    if (n >= 3 && p[n-3]=='.' && p[n-2]=='j' && p[n-1]=='s') return 3;
    if (n >= 4 && p[n-4]=='.' && p[n-3]=='h' && p[n-2]=='t' && p[n-1]=='m') return 4;
    if (n >= 5 && p[n-5]=='.' && p[n-4]=='h' && p[n-3]=='t' && p[n-2]=='m' && p[n-1]=='l') return 4;
    if (n >= 3 && p[n-3]=='.' && p[n-2]=='r' && p[n-1]=='s') return 5;
    return 0;
}

static int is_kw(const char *s, int n, const char *const *kw) {
    int i;
    for (i = 0; kw[i]; i++) {
        int k = 0; const char *w = kw[i];
        while (w[k]) k++;
        if (k != n) continue;
        { int j = 0; while (j < n && s[j] == w[j]) j++; if (j == n) return 1; }
    }
    return 0;
}

static const char *const KW_C[] = {
    "if","else","for","while","return","int","char","void","const","static",
    "struct","typedef","enum","sizeof","include","define","bool","unsigned",0
};
static const char *const KW_PY[] = {
    "def","class","if","elif","else","for","while","return","import","from",
    "as","try","except","with","lambda","True","False","None","and","or","not",0
};
static const char *const KW_JS[] = {
    "function","const","let","var","if","else","for","while","return","class",
    "import","export","new","this","true","false","null","async","await",0
};
static const char *const KW_RS[] = {
    "fn","let","mut","const","if","else","for","while","return","struct","enum",
    "impl","pub","use","mod","match","true","false","self","Self","i32","u32",0
};

static uint32_t tok_color(const char *s, int n, int lang) {
    int i, allnum = 1;
    if (n <= 0) return TUI_FG;
    if (s[0] == '"' || s[0] == '\'') return TUI_GOLD;
    if (s[0] == '#') return TUI_ACCENT;
    if (s[0] == '/' && n > 1 && (s[1] == '/' || s[1] == '*')) return TUI_DIM;
    if (s[0] == '<' && lang == 4) return TUI_ACCENT;
    for (i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') allnum = 0;
    if (allnum) return TUI_GOLD;
    if (lang == 1 && is_kw(s, n, KW_C))  return TUI_ACCENT;
    if (lang == 2 && is_kw(s, n, KW_PY)) return TUI_ACCENT;
    if (lang == 3 && is_kw(s, n, KW_JS)) return TUI_ACCENT;
    if (lang == 5 && is_kw(s, n, KW_RS)) return TUI_ACCENT;
    return TUI_FG;
}

static void paint_code(int col, int row, const char *s, int lang) {
    int i = 0, x = col;
    while (s[i] && x < TUI_COLS - 1) {
        int j, q;
        char c = s[i];
        if (c == ' ' || c == '\t') {
            tui_cell(x++, row, ' ', TUI_FG, TUI_BG);
            i++; continue;
        }
        if (c == '"' || c == '\'') {
            q = c; j = i + 1;
            while (s[j] && s[j] != q) j++;
            if (s[j] == q) j++;
            while (i < j && x < TUI_COLS - 1)
                tui_cell(x++, row, s[i++], TUI_GOLD, TUI_BG);
            continue;
        }
        if (c == '/' && s[i+1] == '/') {
            while (s[i] && x < TUI_COLS - 1)
                tui_cell(x++, row, s[i++], TUI_DIM, TUI_BG);
            break;
        }
        if (c == '#') {
            while (s[i] && x < TUI_COLS - 1)
                tui_cell(x++, row, s[i++], TUI_ACCENT, TUI_BG);
            break;
        }
        if (c == '<' && lang == 4) {
            while (s[i] && s[i] != '>' && x < TUI_COLS - 1)
                tui_cell(x++, row, s[i++], TUI_ACCENT, TUI_BG);
            if (s[i] == '>' && x < TUI_COLS - 1)
                tui_cell(x++, row, s[i++], TUI_ACCENT, TUI_BG);
            continue;
        }
        j = i;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
            j++;
            while ((s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= 'a' && s[j] <= 'z') ||
                   (s[j] >= '0' && s[j] <= '9') || s[j] == '_') j++;
        } else if (c >= '0' && c <= '9') {
            while (s[j] >= '0' && s[j] <= '9') j++;
        } else j = i + 1;
        {
            uint32_t colr = tok_color(s + i, j - i, lang);
            while (i < j && x < TUI_COLS - 1)
                tui_cell(x++, row, s[i++], colr, TUI_BG);
        }
    }
}

static void load_file(const char *path) {
    uint32_t len = 0, i;
    const char *d;
    int ln = 0, col = 0;
    g_n = 1; g_cx = g_cy = g_top = 0; g_dirty = 0;
    g_lines[0][0] = '\0';
    if (!path || !path[0]) return;
    d = vfs_read(path, &len);
    if (!d) return;
    for (i = 0; i < len && ln < NL; i++) {
        char c = d[i];
        if (c == '\n' && i > 0 && d[i - 1] == '\r') continue;
        if (c == '\n' || c == '\r') {
            g_lines[ln][col] = '\0';
            ln++; col = 0;
            if (ln < NL) g_lines[ln][0] = '\0';
        } else if (col < NC) {
            g_lines[ln][col++] = c;
            g_lines[ln][col] = '\0';
        }
    }
    g_n = ln + (col > 0 || ln == 0 ? 1 : 0);
    if (g_n < 1) g_n = 1;
}

static void save_file(void) {
    static char body[RXFS_SIZE];
    int i, o = 0;
    if (!g_path[0]) return;
    for (i = 0; i < g_n && o + 2 < (int)sizeof(body); i++) {
        int k = 0;
        while (g_lines[i][k] && o + 1 < (int)sizeof(body))
            body[o++] = g_lines[i][k++];
        if (i + 1 < g_n) body[o++] = '\n';
    }
    vfs_write(g_path, body, (uint32_t)o);
    g_dirty = 0;
}

static void draw(void) {
    int r, vis = TUI_ROWS - 3;
    char title[64];
    tui_begin();
    scpy(title, "nano  ", 64);
    { int n = slen(title); const char *p = g_path[0] ? g_path : "(untitled)";
      while (*p && n < 50) title[n++] = *p++;
      if (g_dirty && n < 62) { title[n++] = ' '; title[n++] = '*'; }
      title[n] = '\0'; }
    tui_box(0, 0, TUI_COLS, TUI_ROWS, title);
    if (g_cy < g_top) g_top = g_cy;
    if (g_cy >= g_top + vis) g_top = g_cy - vis + 1;
    for (r = 0; r < vis; r++) {
        int li = g_top + r;
        int row = 1 + r;
        char nb[8]; int v, k = 0;
        tui_fill(1, row, TUI_COLS - 2, 1, ' ', TUI_FG, TUI_BG);
        if (li >= g_n) continue;
        v = li + 1;
        if (!v) nb[k++] = '0';
        { char t[8]; int ti = 0; int vv = v;
          if (!vv) t[ti++] = '0';
          while (vv) { t[ti++] = (char)('0' + vv % 10); vv /= 10; }
          while (ti) nb[k++] = t[--ti]; }
        nb[k] = '\0';
        { int pad = 4 - k; int x = 2; int p;
          for (p = 0; p < pad; p++) tui_cell(x++, row, ' ', TUI_DIM, TUI_BG);
          tui_text(x, row, nb, TUI_DIM, TUI_BG);
          tui_cell(x + k, row, '|', TUI_DIM, TUI_BG);
          paint_code(x + k + 2, row, g_lines[li], g_lang);
        }
        if (li == g_cy) {
            int cx = 2 + 4 + 2 + g_cx;
            if (cx < TUI_COLS - 2)
                tui_cell(cx, row,
                         g_lines[li][g_cx] ? g_lines[li][g_cx] : ' ',
                         TUI_HI_FG, TUI_HI_BG);
        }
    }
    tui_hint(TUI_ROWS - 2, "Ctrl+W save   Esc exit   arrows move   Enter newline");
    tui_end();
}

static int read_key(int *ch_out) {
    for (;;) {
        int c = (int)(unsigned char)console_getc();
        *ch_out = c;
        if (c == 0x1b) {
            int n = 0;
            while (n++ < 4000 && !console_serial_has_input() && !kbd_has_input()) { }
            if (console_serial_has_input()) {
                int n1 = console_serial_getc_nb();
                if (n1 == '[') {
                    int n2 = -1, w = 0;
                    while (w++ < 4000 && n2 < 0) n2 = console_serial_getc_nb();
                    if (n2 == 'A') return KEY_UP;
                    if (n2 == 'B') return KEY_DOWN;
                    if (n2 == 'C') return KEY_RIGHT;
                    if (n2 == 'D') return KEY_LEFT;
                    if (n2 == 'H') return KEY_HOME;
                    if (n2 == 'F') return KEY_END;
                }
            }
            return 0x1b;
        }
        return c;
    }
}

static void clamp_cx(void) {
    int L = slen(g_lines[g_cy]);
    if (g_cx > L) g_cx = L;
}

void nano_run(const char *path) {
    scpy(g_path, path ? path : "", RXFS_NAME);
    g_lang = detect_lang(g_path);
    if (commands_verbose()) {
        if (g_path[0]) commands_v("opening RXFS file (Ctrl+W save, Esc quit, 64 KiB cap)");
        else commands_v("new buffer (Ctrl+W save, Esc quit)");
    }
    load_file(g_path[0] ? g_path : NULL);
    if (fb_active()) fb_caret(0);
    for (;;) {
        int ch, k;
        draw();
        k = read_key(&ch);
        if (k == 0x1b) break;
        if (k == 0x17) { save_file(); continue; }          /* Ctrl+W */
        if (k == KEY_UP)   { if (g_cy > 0) { g_cy--; clamp_cx(); } continue; }
        if (k == KEY_DOWN) { if (g_cy + 1 < g_n) { g_cy++; clamp_cx(); } continue; }
        if (k == KEY_LEFT) { if (g_cx > 0) g_cx--; else if (g_cy > 0) { g_cy--; g_cx = slen(g_lines[g_cy]); } continue; }
        if (k == KEY_RIGHT) {
            int L = slen(g_lines[g_cy]);
            if (g_cx < L) g_cx++;
            else if (g_cy + 1 < g_n) { g_cy++; g_cx = 0; }
            continue;
        }
        if (k == KEY_HOME) { g_cx = 0; continue; }
        if (k == KEY_END)  { g_cx = slen(g_lines[g_cy]); continue; }
        if (k == KEY_PGUP) { g_cy -= 10; if (g_cy < 0) g_cy = 0; clamp_cx(); continue; }
        if (k == KEY_PGDN) { g_cy += 10; if (g_cy >= g_n) g_cy = g_n - 1; clamp_cx(); continue; }
        if (k == '\n' || k == '\r') {
            if (g_n >= NL) continue;
            { int i;
              for (i = g_n; i > g_cy + 1; i--) scpy(g_lines[i], g_lines[i-1], NC + 1);
              scpy(g_lines[g_cy + 1], g_lines[g_cy] + g_cx, NC + 1);
              g_lines[g_cy][g_cx] = '\0';
              g_n++; g_cy++; g_cx = 0; g_dirty = 1;
            }
            continue;
        }
        if (k == '\b' || k == 0x7f) {
            if (g_cx > 0) {
                int i, L = slen(g_lines[g_cy]);
                for (i = g_cx - 1; i < L; i++) g_lines[g_cy][i] = g_lines[g_cy][i+1];
                g_cx--; g_dirty = 1;
            } else if (g_cy > 0) {
                int L = slen(g_lines[g_cy - 1]);
                int L2 = slen(g_lines[g_cy]);
                int i;
                if (L + L2 < NC) {
                    int p = L;
                    while (g_lines[g_cy][p - L] && p < NC) {
                        g_lines[g_cy - 1][p] = g_lines[g_cy][p - L];
                        p++;
                    }
                    g_lines[g_cy - 1][p] = '\0';
                    for (i = g_cy; i + 1 < g_n; i++) scpy(g_lines[i], g_lines[i+1], NC + 1);
                    g_n--; g_cy--; g_cx = L; g_dirty = 1;
                }
            }
            continue;
        }
        if (ch >= 32 && ch < 127) {
            int L = slen(g_lines[g_cy]);
            if (L >= NC) continue;
            { int i;
              for (i = L; i >= g_cx; i--) g_lines[g_cy][i+1] = g_lines[g_cy][i];
              g_lines[g_cy][g_cx] = (char)ch;
              g_cx++; g_dirty = 1;
            }
        }
    }
    tui_begin(); tui_end();
    console_clear();
}
