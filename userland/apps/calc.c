/* Simple calculator window app. */
#include "calc.h"

#define SCALE      1000000LL
#define DECIMALS   6
#define VAL_MAX    (4000000000000LL)     /* ~4e6 after scaling: past this we
                                            stop accepting digits rather than
                                            let the 128-bit products drift    */

static int64_t g_acc;             /* the value on the left of the pending op */
static int64_t g_entry;           /* the number being typed, scaled          */
static int     g_frac;            /* digits typed after the point, 0 = none  */
static bool    g_point;           /* a decimal point has been typed          */
static bool    g_fresh;           /* next digit starts a new entry           */
static char    g_op;              /* pending operator, 0 = none              */
static bool    g_error;
static char    g_disp[32];
static char    g_note[40];

/* ------------------------------- formatting ------------------------------- */
static char *app(char *d, const char *s, char *e) { while (*s && d < e - 1) *d++ = *s++; *d = '\0'; return d; }

static char *num(char *d, uint64_t v, char *e) {
    char t[24]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i && d < e - 1) *d++ = t[--i];
    *d = '\0';
    return d;
}

/* Scaled int64 -> "-12.34". Trailing zeros are trimmed, and so is the point
   itself when nothing survives it, so 2.000000 reads as 2. */
static void format(char *out, int max, int64_t v) {
    char *d = out, *e = out + max;
    if (v < 0) { d = app(d, "-", e); v = -v; }
    d = num(d, (uint64_t)(v / SCALE), e);

    uint64_t frac = (uint64_t)(v % SCALE);
    if (!frac) return;

    char fbuf[DECIMALS + 1];
    for (int i = DECIMALS - 1; i >= 0; i--) { fbuf[i] = (char)('0' + (int)(frac % 10)); frac /= 10; }
    fbuf[DECIMALS] = '\0';
    int n = DECIMALS;
    while (n > 0 && fbuf[n - 1] == '0') n--;
    fbuf[n] = '\0';
    d = app(d, ".", e);
    app(d, fbuf, e);
}

static void refresh(void) {
    if (g_error) {
        g_disp[0] = 'e'; g_disp[1] = 'r'; g_disp[2] = 'r'; g_disp[3] = 'o';
        g_disp[4] = 'r'; g_disp[5] = '\0';
        return;
    }
    format(g_disp, (int)sizeof(g_disp), g_entry);
    /* A point that has been typed but not yet followed by a digit has to show,
       or "3." looks exactly like "3" and the next keystroke is a surprise. */
    if (g_point && g_frac == 0) {
        int n = 0; while (g_disp[n]) n++;
        if (n + 2 < (int)sizeof(g_disp)) { g_disp[n] = '.'; g_disp[n + 1] = '\0'; }
    }
}

static void note(void) {
    char *d = g_note, *e = g_note + sizeof(g_note);
    if (!g_op) { g_note[0] = '\0'; return; }
    char buf[24]; format(buf, (int)sizeof(buf), g_acc);
    d = app(d, buf, e);
    const char sym[2] = { g_op, '\0' };
    d = app(d, " ", e); d = app(d, sym, e);
}

/* -------------------------------- arithmetic ------------------------------ */
static int64_t apply(int64_t a, char op, int64_t b, bool *bad) {
    __int128 r;
    switch (op) {
    case '+': r = (__int128)a + b; break;
    case '-': r = (__int128)a - b; break;
    case '*': r = ((__int128)a * b) / SCALE; break;
    case '/':
        if (b == 0) { *bad = true; return 0; }
        r = ((__int128)a * SCALE) / b;
        break;
    default:  return b;
    }
    if (r > (__int128)VAL_MAX * SCALE || r < -(__int128)VAL_MAX * SCALE) { *bad = true; return 0; }
    return (int64_t)r;
}

/* --------------------------------- public --------------------------------- */
void calc_reset(void) {
    g_acc = 0; g_entry = 0; g_frac = 0; g_point = false;
    g_fresh = true; g_op = 0; g_error = false;
    refresh(); note();
}

const char *calc_display(void) { return g_disp; }
const char *calc_pending(void) { return g_note; }

/* The keypad. Labels and the character each one sends are one table, so a
   button and the key it stands for cannot disagree. */
typedef struct { const char *label; char key; } calc_btn;
static const calc_btn BTN[CALC_ROWS * CALC_COLS] = {
    { "C", 'c' }, { "+/-", '~' }, { "<", '\b' }, { "/", '/' },
    { "7", '7' }, { "8",   '8' }, { "9", '9'  }, { "*", '*' },
    { "4", '4' }, { "5",   '5' }, { "6", '6'  }, { "-", '-' },
    { "1", '1' }, { "2",   '2' }, { "3", '3'  }, { "+", '+' },
    { "0", '0' }, { ".",   '.' }, { "=", '=' },  { "=", '=' },
};

int         calc_button_count(void)    { return CALC_ROWS * CALC_COLS; }
const char *calc_button_label(int i)   { return (i >= 0 && i < CALC_ROWS * CALC_COLS) ? BTN[i].label : ""; }
char        calc_button_key(int i)     { return (i >= 0 && i < CALC_ROWS * CALC_COLS) ? BTN[i].key   : 0; }

void calc_key(int ch) {
    if (ch >= 'A' && ch <= 'Z') ch += 32;

    if (ch == 'c') { calc_reset(); return; }
    if (g_error && ch != 'c') { return; }          /* only C leaves the error */

    if (ch >= '0' && ch <= '9') {
        if (g_fresh) { g_entry = 0; g_frac = 0; g_point = false; g_fresh = false; }
        int digit = ch - '0';
        if (g_point) {
            if (g_frac < DECIMALS) {
                int64_t place = SCALE;
                for (int i = 0; i <= g_frac; i++) place /= 10;
                g_entry += (g_entry < 0 ? -digit : digit) * place;
                g_frac++;
            }
        } else if (g_entry < VAL_MAX / 10) {
            g_entry = g_entry * 10 + digit * SCALE * (g_entry < 0 ? -1 : 1);
        }
        refresh();
        return;
    }

    switch (ch) {
    case '.':
        if (g_fresh) { g_entry = 0; g_frac = 0; g_fresh = false; }
        g_point = true;
        refresh();
        return;

    case '~':                                   /* sign flip                 */
        g_entry = -g_entry;
        refresh();
        return;

    case '\b': case 0x7F:                       /* one digit back            */
        if (g_fresh) return;
        if (g_point && g_frac > 0) {
            int64_t place = SCALE;
            for (int i = 0; i < g_frac; i++) place /= 10;
            g_entry -= g_entry % (place * 10);
            g_frac--;
            if (g_frac == 0) { /* keep the point: the user is still editing */ }
        } else if (g_point) {
            g_point = false;
        } else {
            g_entry = (g_entry / SCALE) / 10 * SCALE;
        }
        refresh();
        return;

    case '+': case '-': case '*': case '/': case 'x': case ':': {
        char op = (ch == 'x') ? '*' : (ch == ':') ? '/' : (char)ch;
        /* Chaining: 2 + 3 + shows 5 before taking the second +, which is what
           a calculator does and what makes a long sum readable. */
        if (g_op && !g_fresh) {
            bool bad = false;
            int64_t r = apply(g_acc, g_op, g_entry, &bad);
            if (bad) { g_error = true; refresh(); return; }
            g_acc = r; g_entry = r;
        } else {
            g_acc = g_entry;
        }
        g_op = op; g_fresh = true; g_point = false; g_frac = 0;
        refresh(); note();
        return; }

    case '=': case '\n': case '\r':
        if (!g_op) { g_fresh = true; return; }
        {
            bool bad = false;
            int64_t r = apply(g_acc, g_op, g_entry, &bad);
            if (bad) { g_error = true; g_op = 0; g_note[0] = '\0'; refresh(); return; }
            g_entry = r; g_acc = r;
        }
        g_op = 0; g_fresh = true; g_point = false; g_frac = 0;
        refresh(); note();
        return;

    default:
        return;
    }
}
