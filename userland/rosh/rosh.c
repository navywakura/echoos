/*
 * ROSH — line editor + shell bits used by the desktop terminal.
 * Still growing; report broken line-edit sequences.
 */
#include "rosh.h"
#include "commands.h"

#define HIST_MAX   32
#define LINE_MAX   ROSH_LINE_MAX

typedef struct {
    char hist[HIST_MAX][LINE_MAX];
    int  hist_n;
    int  hist_pos;
    char saved[LINE_MAX];
    char line[LINE_MAX];
    int  len;
    int  cur;
    bool ready;
} rosh_sess;

static rosh_sess g_s[ROSH_SESSIONS];
static int       g_sid;

#define g_hist     (g_s[g_sid].hist)
#define g_hist_n   (g_s[g_sid].hist_n)
#define g_hist_pos (g_s[g_sid].hist_pos)
#define g_saved    (g_s[g_sid].saved)
#define g_line     (g_s[g_sid].line)
#define g_len      (g_s[g_sid].len)
#define g_cur      (g_s[g_sid].cur)
#define g_ready    (g_s[g_sid].ready)

/* ------------------------------- utilities -------------------------------- */
static void cpy(char *d, const char *s, int max) {
    int i = 0;
    for (; s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = '\0';
}
static int len_of(const char *s) { int n = 0; while (s[n]) n++; return n; }
static bool prefix(const char *pre, const char *s) {
    while (*pre) { if (*pre != *s) return false; pre++; s++; }
    return true;
}

/* -------------------------------- history --------------------------------- */
/* Consecutive duplicates are not stored: walking back through five identical
   `status` lines is not history, it is noise. */
static void hist_push(const char *line) {
    if (!line[0]) return;
    if (g_hist_n > 0) {
        const char *last = g_hist[(g_hist_n - 1) % HIST_MAX];
        bool same = true;
        for (int i = 0; ; i++) {
            if (last[i] != line[i]) { same = false; break; }
            if (!last[i]) break;
        }
        if (same) return;
    }
    cpy(g_hist[g_hist_n % HIST_MAX], line, LINE_MAX);
    g_hist_n++;
}

int rosh_history_count(void) {
    return g_hist_n < HIST_MAX ? g_hist_n : HIST_MAX;
}

const char *rosh_history_at(int i) {
    int n = rosh_history_count();
    if (i < 0 || i >= n) return NULL;
    int base = g_hist_n - n;
    return g_hist[(base + i) % HIST_MAX];
}

/* Walk `delta` steps through history (-1 = older, +1 = newer). Position 0 is
   the oldest kept line; g_hist_pos == -1 means "back on the line I was
   typing", which is parked in g_saved while browsing. */
static void hist_walk(int delta) {
    int n = rosh_history_count();
    if (n == 0) return;

    if (g_hist_pos < 0) {
        if (delta > 0) return;                 /* already on the live line   */
        cpy(g_saved, g_line, LINE_MAX);
        g_hist_pos = n - 1;
    } else {
        g_hist_pos += delta;
        if (g_hist_pos < 0) g_hist_pos = 0;
        if (g_hist_pos >= n) {                 /* walked off the new end     */
            g_hist_pos = -1;
            cpy(g_line, g_saved, LINE_MAX);
            g_len = g_cur = len_of(g_line);
            return;
        }
    }
    cpy(g_line, rosh_history_at(g_hist_pos), LINE_MAX);
    g_len = g_cur = len_of(g_line);
}

/* ------------------------------- completion -------------------------------
   Completion is over the command table only — the first word of the line.
   Completing filenames would need the VFS here, and the shell that owns ROSH
   is better placed to decide what a second word means. */
static int complete(const char **match_out) {
    /* Only the first word, and only while still typing it. */
    for (int i = 0; i < g_cur; i++) if (g_line[i] == ' ') return 0;

    int n = commands_name_count(), hits = 0;
    const char *first = NULL;
    /* Longest common prefix across every hit, so a partial match still makes
       progress instead of doing nothing until the name is unambiguous. */
    char common[LINE_MAX]; int common_len = 0;

    for (int i = 0; i < n; i++) {
        const char *nm = commands_name_at(i);
        if (!nm || !prefix(g_line, nm)) continue;
        hits++;
        if (hits == 1) {
            first = nm;
            cpy(common, nm, LINE_MAX);
            common_len = len_of(common);
        } else {
            int k = 0;
            while (k < common_len && nm[k] && common[k] == nm[k]) k++;
            common_len = k;
            common[k] = '\0';
        }
    }
    if (hits == 0) return 0;

    if (hits == 1) {
        cpy(g_line, first, LINE_MAX);
        g_len = g_cur = len_of(g_line);
        if (g_len + 1 < LINE_MAX) { g_line[g_len++] = ' '; g_line[g_len] = '\0'; g_cur = g_len; }
    } else if (common_len > g_len) {
        cpy(g_line, common, LINE_MAX);
        g_len = g_cur = common_len;
    }
    if (match_out) *match_out = first;
    return hits;
}

/* --------------------------------- public --------------------------------- */
static void sess_wipe(rosh_sess *s) {
    int i, j;
    s->hist_n = 0;
    s->hist_pos = -1;
    s->saved[0] = '\0';
    s->line[0] = '\0';
    s->len = s->cur = 0;
    s->ready = false;
    for (i = 0; i < HIST_MAX; i++)
        for (j = 0; j < LINE_MAX; j++) s->hist[i][j] = 0;
}

void rosh_init_session(int id) {
    if (id < 0 || id >= ROSH_SESSIONS) return;
    sess_wipe(&g_s[id]);
}

void rosh_select(int id) {
    if (id < 0 || id >= ROSH_SESSIONS) return;
    g_sid = id;
}

int rosh_active(void) { return g_sid; }

void rosh_copy(int from, int to) {
    if (from < 0 || to < 0 || from >= ROSH_SESSIONS || to >= ROSH_SESSIONS) return;
    if (from == to) return;
    g_s[to] = g_s[from];
}

void rosh_init(void) {
    int i;
    for (i = 0; i < ROSH_SESSIONS; i++) sess_wipe(&g_s[i]);
    g_sid = 0;
}

const char *rosh_line(void)   { return g_line; }
int         rosh_cursor(void) { return g_cur; }

rosh_result rosh_key(int ch, const char **completions) {
    if (completions) *completions = NULL;

    switch (ch) {
    case '\n': case '\r':
        hist_push(g_line);
        g_hist_pos = -1;
        g_ready = true;
        return ROSH_SUBMIT;

    case '\b': case 0x7F:
        if (g_cur > 0) {
            for (int i = g_cur - 1; i < g_len; i++) g_line[i] = g_line[i + 1];
            g_cur--; g_len--;
            return ROSH_LINE_CHANGED;
        }
        return ROSH_IGNORED;

    case KEY_UP:    hist_walk(-1); return ROSH_LINE_CHANGED;
    case KEY_DOWN:  hist_walk(+1); return ROSH_LINE_CHANGED;
    case KEY_LEFT:  if (g_cur > 0)     { g_cur--; return ROSH_LINE_CHANGED; } return ROSH_IGNORED;
    case KEY_RIGHT: if (g_cur < g_len) { g_cur++; return ROSH_LINE_CHANGED; } return ROSH_IGNORED;
    case KEY_HOME:  if (g_cur != 0)    { g_cur = 0;     return ROSH_LINE_CHANGED; } return ROSH_IGNORED;
    case KEY_END:   if (g_cur != g_len){ g_cur = g_len; return ROSH_LINE_CHANGED; } return ROSH_IGNORED;

    case '\t': {
        const char *m = NULL;
        int hits = complete(&m);
        if (hits > 1 && completions) *completions = "";   /* caller may list  */
        return hits ? ROSH_LINE_CHANGED : ROSH_IGNORED;
    }

    /* Ctrl-X, not the traditional Ctrl-U: 0x15 is already KEY_HOME on this
       keyboard, and a shortcut that silently shadows a cursor key is worse
       than a slightly unusual one. */
    case 0x18:  /* Ctrl-X: clear the line                                     */
        if (g_len) { g_line[0] = '\0'; g_len = g_cur = 0; return ROSH_LINE_CHANGED; }
        return ROSH_IGNORED;

    default:
        if (ch >= 32 && ch < 127 && g_len + 1 < LINE_MAX) {
            for (int i = g_len; i > g_cur; i--) g_line[i] = g_line[i - 1];
            g_line[g_cur] = (char)ch;
            g_cur++; g_len++;
            g_line[g_len] = '\0';
            g_hist_pos = -1;          /* editing again: leave history behind  */
            return ROSH_LINE_CHANGED;
        }
        return ROSH_IGNORED;
    }
}

bool rosh_take(char *out, int max) {
    if (!g_ready) return false;
    cpy(out, g_line, max);
    g_line[0] = '\0'; g_len = g_cur = 0; g_ready = false;
    return true;
}

/* How many command names share this prefix, for a caller that wants to print
   the candidates on a double Tab. */
int rosh_completions(const char *pre, const char **out, int max) {
    int n = commands_name_count(), hits = 0;
    for (int i = 0; i < n && hits < max; i++) {
        const char *nm = commands_name_at(i);
        if (nm && prefix(pre, nm)) out[hits++] = nm;
    }
    return hits;
}
