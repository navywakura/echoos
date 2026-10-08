/*
 * Roxenite (RX-C) interpreter. Sandboxed: UI, rgx:// nav, own state — no raw
 * memory/kernel/crypto access. Parser errors try to be readable.
 * Expect bugs in edge-case syntax; send a failing snippet if you hit one.
 */
#include "roxenite.h"
#include "desktop.h"
#include "framebuffer.h"

/* ------------------------------ tiny helpers ----------------------------- */
static bool eqs(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
static void cpn(char *d, const char *s, size_t max) {
    size_t i = 0;
    for (; s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = '\0';
}
static char *apps(char *d, const char *s, char *end) {
    while (*s && d < end - 1) *d++ = *s++;
    *d = '\0';
    return d;
}
static char *appn(char *d, long v, char *end) {
    char tmp[24]; int i = 0;
    bool neg = v < 0; unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) tmp[i++] = '0';
    while (u) { tmp[i++] = '0' + (u % 10); u /= 10; }
    if (neg && d < end - 1) *d++ = '-';
    while (i-- > 0 && d < end - 1) *d++ = tmp[i];
    *d = '\0';
    return d;
}
static long parse_int(const char *s, bool *ok) {
    long v = 0; int i = 0; bool neg = false;
    if (s[0] == '-') { neg = true; i = 1; }
    if (!s[i]) { *ok = false; return 0; }
    for (; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') { *ok = false; return 0; }
        v = v * 10 + (s[i] - '0');
    }
    *ok = true;
    return neg ? -v : v;
}

static void serr3(rxc_program *p, const char *a, const char *b, const char *c) {
    p->ok = false;
    char *d = p->error, *end = p->error + RXC_ERR_LEN;
    d = apps(d, "RX-C error: ", end);
    d = apps(d, a, end); d = apps(d, b, end); apps(d, c, end);
}
#define ERR(p, msg)        serr3(p, msg, "", "")
#define ERR3(p, a, b, c)   serr3(p, a, b, c)

/* ------------------------------- tokenizer ------------------------------- */
#define MAXTOK 8
typedef struct { char text[RXC_STR]; bool is_str; } tok;

static int tokenize(const char *line, tok *toks) {
    int n = 0; const char *p = line;
    while (*p && n < MAXTOK) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (p[0] == '/' && p[1] == '/') break;          /* // comment */
        if (*p == '"') {
            p++; int i = 0;
            while (*p && *p != '"' && i < RXC_STR - 1) toks[n].text[i++] = *p++;
            toks[n].text[i] = '\0'; toks[n].is_str = true;
            if (*p == '"') p++;
            n++;
        } else {
            int i = 0;
            /* a lone ':' is its own token so `button "x":` splits cleanly */
            if (*p == ':') { toks[n].text[0] = ':'; toks[n].text[1] = '\0'; toks[n].is_str = false; p++; n++; continue; }
            while (*p && *p != ' ' && *p != '\t' && *p != ':' && i < RXC_STR - 1) toks[n].text[i++] = *p++;
            toks[n].text[i] = '\0'; toks[n].is_str = false;
            n++;
        }
    }
    return n;
}

/* -------------------------------- parser --------------------------------- */
static void parse_action(rxc_program *p, int cur_button, tok *t, int nt) {
    if (p->nactions >= RXC_MAX_ACTIONS) { ERR(p, "too many actions"); return; }
    const char *w = t[0].text;
    rxc_action *a = &p->actions[p->nactions];

    if (eqs(w, "add")) {                       /* add <n> to <state> */
        if (nt < 4 || !eqs(t[2].text, "to")) { ERR(p, "expected: add <n> to <state>"); return; }
        bool ok; long v = parse_int(t[1].text, &ok);
        if (!ok) { ERR(p, "'add' needs a number"); return; }
        a->kind = RXA_ADD; a->ival = v; cpn(a->arg, t[3].text, RXC_STR);
    } else if (eqs(w, "set")) {                /* set <state> to <value> */
        if (nt < 4 || !eqs(t[2].text, "to")) { ERR(p, "expected: set <state> to <value>"); return; }
        cpn(a->arg, t[1].text, RXC_STR);
        if (t[3].is_str) { a->kind = RXA_SET_STR; cpn(a->sval, t[3].text, RXC_STR); }
        else { bool ok; long v = parse_int(t[3].text, &ok); if (!ok) { ERR(p, "'set' needs a number or string"); return; } a->kind = RXA_SET_INT; a->ival = v; }
    } else if (eqs(w, "open")) {
        if (nt < 2 || !t[1].is_str) { ERR(p, "expected: open \"rgx://...\""); return; }
        a->kind = RXA_OPEN; cpn(a->arg, t[1].text, RXC_STR);
    } else if (eqs(w, "print")) {
        if (nt < 2 || !t[1].is_str) { ERR(p, "expected: print \"...\""); return; }
        a->kind = RXA_PRINT; cpn(a->arg, t[1].text, RXC_STR);
    } else if (eqs(w, "clear")) {
        a->kind = RXA_CLEAR;
    } else {
        ERR3(p, "unknown action '", w, "'"); return;
    }
    p->nactions++;
    p->buttons[cur_button].action_count++;
}

void rxc_compile(const char *src, rxc_program *p) {
    k_memset(p, 0, sizeof(*p));
    p->ok = true;
    cpn(p->app_name, "untitled", RXC_STR);

    int cur_page = -1, cur_button = -1, button_indent = 0;
    const char *s = src;
    char line[192];

    while (*s) {
        int len = 0;
        while (*s && *s != '\n' && len < (int)sizeof(line) - 1) line[len++] = *s++;
        line[len] = '\0';
        if (*s == '\n') s++;

        int indent = 0;
        while (line[indent] == ' ' || line[indent] == '\t') indent++;

        tok t[MAXTOK];
        int nt = tokenize(line, t);
        if (nt == 0) continue;                 /* blank / comment */
        const char *w = t[0].text;
        if (eqs(w, "note")) continue;          /* note "..." comment */

        /* Inside a button's indented action block? */
        if (cur_button >= 0 && indent > button_indent) {
            parse_action(p, cur_button, t, nt);
            if (!p->ok) return;
            continue;
        }
        cur_button = -1;                       /* action block ended */

        if (eqs(w, "app")) {
            if (nt < 2 || !t[1].is_str) { ERR(p, "expected app name in quotes after 'app'"); return; }
            cpn(p->app_name, t[1].text, RXC_STR);
        } else if (eqs(w, "state")) {
            if (nt < 4 || !eqs(t[2].text, "is")) { ERR(p, "expected: state <name> is <value>"); return; }
            if (p->nstates >= RXC_MAX_STATES) { ERR(p, "too many states"); return; }
            rxc_state *st = &p->states[p->nstates++];
            cpn(st->name, t[1].text, RXC_STR);
            if (t[3].is_str) { st->type = RXV_STR; cpn(st->sval, t[3].text, RXC_STR); }
            else { bool ok; long v = parse_int(t[3].text, &ok); if (!ok) { ERR3(p, "state '", st->name, "' needs a number or string"); return; } st->type = RXV_INT; st->ival = v; }
        } else if (eqs(w, "page")) {
            if (nt < 2) { ERR(p, "expected page name after 'page'"); return; }
            if (p->npages >= RXC_MAX_PAGES) { ERR(p, "too many pages"); return; }
            cur_page = p->npages++;
            cpn(p->pages[cur_page].name, t[1].text, RXC_STR);
            p->pages[cur_page].node_start = p->nnodes;
            p->pages[cur_page].node_count = 0;
        } else if (eqs(w, "title") || eqs(w, "text") || eqs(w, "value") || eqs(w, "button")) {
            if (cur_page < 0) { ERR3(p, "'", w, "' must be inside a page"); return; }
            if (p->nnodes >= RXC_MAX_NODES) { ERR(p, "too many UI nodes"); return; }
            rxc_node *nd = &p->nodes[p->nnodes];
            if (eqs(w, "title") || eqs(w, "text")) {
                if (nt < 2 || !t[1].is_str) { ERR3(p, "expected text in quotes after '", w, "'"); return; }
                nd->kind = eqs(w, "title") ? RXN_TITLE : RXN_TEXT;
                cpn(nd->text, t[1].text, RXC_STR);
            } else if (eqs(w, "value")) {
                if (nt < 2) { ERR(p, "expected state name after 'value'"); return; }
                nd->kind = RXN_VALUE; cpn(nd->text, t[1].text, RXC_STR);
            } else { /* button */
                if (nt < 2 || !t[1].is_str) { ERR(p, "expected button label in quotes"); return; }
                if (p->nbuttons >= RXC_MAX_BUTTONS) { ERR(p, "too many buttons"); return; }
                nd->kind = RXN_BUTTON; nd->button = p->nbuttons;
                cpn(nd->text, t[1].text, RXC_STR);
                rxc_button *b = &p->buttons[p->nbuttons++];
                cpn(b->label, t[1].text, RXC_STR);
                b->open_url[0] = '\0'; b->action_start = p->nactions; b->action_count = 0;
                if (nt >= 4 && eqs(t[2].text, "opens") && t[3].is_str) {
                    cpn(b->open_url, t[3].text, RXC_STR);
                } else if (nt >= 3 && eqs(t[2].text, ":")) {
                    cur_button = nd->button; button_indent = indent;   /* action block follows */
                }
            }
            p->nnodes++;
            p->pages[cur_page].node_count++;
        } else {
            ERR3(p, "unknown word '", w, "'"); return;
        }
    }
    if (p->npages == 0) ERR(p, "app has no pages");
}

/* ------------------------------ interpreter ------------------------------ */
static rxc_state *find_state(rxc_program *p, const char *name) {
    for (int i = 0; i < p->nstates; i++) if (eqs(p->states[i].name, name)) return &p->states[i];
    return NULL;
}
static int find_page(rxc_program *p, const char *name) {
    for (int i = 0; i < p->npages; i++) if (eqs(p->pages[i].name, name)) return i;
    return -1;
}
/* "rgx://<name>" -> "<name>" (local app page lookup). */
static const char *url_local(const char *url) {
    const char *pfx = "rgx://";
    const char *u = url;
    for (int i = 0; pfx[i]; i++) { if (u[i] != pfx[i]) return NULL; }
    return u + 6;
}

static void render_page_rxc(rxc_program *p, int page) {
    rxc_page *pg = &p->pages[page];
    const char *title = p->app_name;
    for (int i = 0; i < pg->node_count; i++) {
        rxc_node *nd = &p->nodes[pg->node_start + i];
        if (nd->kind == RXN_TITLE) { title = nd->text; break; }
    }
    char origin[RXC_STR]; char *d = origin, *e = origin + sizeof(origin);
    d = apps(d, "rgx://", e); d = apps(d, p->app_name, e); d = apps(d, "/", e); apps(d, pg->name, e);
    render_begin_page(title, origin);

    int bnum = 0;
    for (int i = 0; i < pg->node_count; i++) {
        rxc_node *nd = &p->nodes[pg->node_start + i];
        char buf[128]; char *b = buf, *be = buf + sizeof(buf);
        switch (nd->kind) {
        case RXN_TITLE: break;                 /* shown in the title bar */
        case RXN_TEXT:  render_line(nd->text); break;
        case RXN_VALUE: {
            rxc_state *st = find_state(p, nd->text);
            b = apps(b, nd->text, be); b = apps(b, ": ", be);
            if (!st) b = apps(b, "(undefined)", be);
            else if (st->type == RXV_INT) b = appn(b, st->ival, be);
            else b = apps(b, st->sval, be);
            render_line(buf);
            break;
        }
        case RXN_BUTTON: {
            bnum++;
            rxc_button *bt = &p->buttons[nd->button];
            b = apps(b, "[", be); b = appn(b, bnum, be); b = apps(b, "] ", be); apps(b, bt->label, be);
            render_button(buf, bt->open_url[0] ? bt->open_url : "action", bnum);
            break;
        }
        }
    }
    render_end_page();
}

static void run_actions(rxc_program *p, rxc_button *bt) {
    for (int k = 0; k < bt->action_count; k++) {
        rxc_action *a = &p->actions[bt->action_start + k];
        rxc_state *st;
        switch (a->kind) {
        case RXA_ADD:     st = find_state(p, a->arg); if (st && st->type == RXV_INT) st->ival += a->ival; break;
        case RXA_SET_INT: st = find_state(p, a->arg); if (st) { st->type = RXV_INT; st->ival = a->ival; } break;
        case RXA_SET_STR: st = find_state(p, a->arg); if (st) { st->type = RXV_STR; cpn(st->sval, a->sval, RXC_STR); } break;
        case RXA_PRINT:   console_write("  [rx] "); console_write(a->arg); console_putc('\n'); break;
        case RXA_CLEAR:   break;               /* re-render happens next loop */
        case RXA_OPEN:    break;               /* handled by caller for nav   */
        }
    }
}

bool rxc_run(rxc_program *p, char *open_out, size_t max) {
    if (open_out && max) open_out[0] = '\0';
    if (!p->ok) {
        console_write(p->error); console_putc('\n');
        return false;
    }
    int cur = 0;
    int h = find_page(p, "home");
    if (h >= 0) cur = h;

    char line[64];
    for (;;) {
        fb_cursor_hide();                     /* hide before the redraw */
        render_page_rxc(p, cur);
        console_write("  rx> click a button, type its number, or 'back': ");

        /* Event loop: a typed number/word (returns 0) OR a clicked button id. */
        int num = ui_line(line, sizeof(line), true);
        if (num == 0) {
            if (line[0] == '\0') continue;
            if (eqs(line, "back") || eqs(line, "exit")) return false;
            bool ok; long v = parse_int(line, &ok);
            if (!ok) { console_write("  [rx] click a button, type a number, or 'back'\n"); continue; }
            num = (int)v;
        }

        /* find the num-th button on the current page (1-based) */
        rxc_page *pg = &p->pages[cur];
        int seen = 0, chosen = -1;
        for (int i = 0; i < pg->node_count; i++) {
            rxc_node *nd = &p->nodes[pg->node_start + i];
            if (nd->kind == RXN_BUTTON && ++seen == num) { chosen = nd->button; break; }
        }
        if (chosen < 0) { console_write("  [rx] no such button\n"); continue; }

        rxc_button *bt = &p->buttons[chosen];
        if (bt->open_url[0]) {
            const char *local = url_local(bt->open_url);
            int lp = local ? find_page(p, local) : -1;
            if (lp >= 0) { cur = lp; }                    /* in-app page switch */
            else if (open_out && max) { cpn(open_out, bt->open_url, max); return true; } /* ask shell to navigate */
        } else {
            run_actions(p, bt);                            /* modify own state */
        }
    }
}

/* ------------------------ app state persistence -------------------------- */
/* Serialize live state to "name=value\n" lines (strings quoted). */
uint32_t rxc_state_save(const rxc_program *p, char *out, uint32_t max) {
    char *d = out, *e = out + max;
    for (int i = 0; i < p->nstates; i++) {
        const rxc_state *s = &p->states[i];
        d = apps(d, s->name, e);
        d = apps(d, "=", e);
        if (s->type == RXV_STR) { d = apps(d, "\"", e); d = apps(d, s->sval, e); d = apps(d, "\"", e); }
        else                    { d = appn(d, s->ival, e); }
        d = apps(d, "\n", e);
    }
    return (uint32_t)(d - out);
}

/* Re-apply saved "name=value" lines onto matching states (unknown names are
   ignored, so editing the app's state list never breaks an old sidecar). */
void rxc_state_apply(rxc_program *p, const char *data, uint32_t len) {
    uint32_t i = 0;
    char line[RXC_STR * 2];
    while (i < len) {
        uint32_t n = 0;
        while (i < len && data[i] != '\n' && n < sizeof(line) - 1) line[n++] = data[i++];
        line[n] = '\0';
        while (i < len && data[i] != '\n') i++;   /* skip overflow tail */
        if (i < len) i++;                          /* skip '\n'          */

        int eq = -1;
        for (int k = 0; line[k]; k++) if (line[k] == '=') { eq = k; break; }
        if (eq <= 0) continue;
        line[eq] = '\0';
        const char *val = &line[eq + 1];
        rxc_state *st = find_state(p, line);
        if (!st) continue;
        if (val[0] == '"') {
            st->type = RXV_STR;
            char *w = st->sval; const char *r = val + 1; int c = 0;
            while (*r && *r != '"' && c < RXC_STR - 1) { *w++ = *r++; c++; }
            *w = '\0';
        } else {
            bool ok; long v = parse_int(val, &ok);
            if (ok) { st->type = RXV_INT; st->ival = v; }
        }
    }
}

/* ------------------------------ built-ins -------------------------------- */
static const char *DEMO_COUNTER =
    "app \"Roxenite Demo\"\n"
    "state counter is 0\n"
    "page home:\n"
    "  title \"Roxenite RX-C\"\n"
    "  text \"A human-readable language for RogexOS.\"\n"
    "  text \"Counter value:\"\n"
    "  value counter\n"
    "  button \"Increase\":\n"
    "    add 1 to counter\n"
    "  button \"Reset\":\n"
    "    set counter to 0\n"
    "  button \"Open Laws\" opens \"rgx://laws\"\n";

static const char *DEMO_HELLO =
    "app \"hello\"\n"
    "page home:\n"
    "  title \"Rogex OS\"\n"
    "  text \"Welcome to RXwired.\"\n"
    "  button \"Open Social\" opens \"rgx://social\"\n"
    "  button \"Settings\" opens \"rgx://settings\"\n";

/* Intentionally malformed — demonstrates the human-readable error path. */
static const char *DEMO_BROKEN =
    "app \"Oops\"\n"
    "page home:\n"
    "  title \"Broken\"\n"
    "  wobble \"this is not a real word\"\n";

const char *rxc_demo_source(const char *name) {
    if (!name || !name[0] || eqs(name, "demo") || eqs(name, "counter")) return DEMO_COUNTER;
    if (eqs(name, "hello")) return DEMO_HELLO;
    if (eqs(name, "broken")) return DEMO_BROKEN;
    return NULL;
}

void rxc_list(void) {
    console_write("  built-in RX-C apps:  demo (counter)   hello\n");
    console_write("  run one with:  rx run demo   |   rx run hello\n");
}

void rxc_help(void) {
    console_write(
        "  Roxenite (RX-C) - a human-readable language for RogexOS apps.\n"
        "  A person speaking to the machine: words over symbols, safe by default.\n\n"
        "  app \"My App\"\n"
        "  state counter is 0\n"
        "  page home:\n"
        "    title \"Title\"\n"
        "    text  \"Some text\"\n"
        "    value counter\n"
        "    button \"Increase\":\n"
        "        add 1 to counter\n"
        "    button \"Open\" opens \"rgx://laws\"\n\n"
        "  actions:  add <n> to <state>   set <state> to <value>\n"
        "            open \"rgx://...\"   print \"...\"   clear\n"
        "  comments: // ...   or   note \"...\"\n\n"
        "  shell:  rx help | rx list | rx demo | rx run <name>\n");
}
