/* z — zoxide-like directory jumper for EchOS.
 * Maintains a frequency/recency database at ~/.echos/z.db of visited dirs.
 *   z add PATH       record a visit (scored by frecency)
 *   z query TERM     print best-ranked dir whose path contains TERM
 *   z query          dump ranked table
 * EchOS coretools — proprietary, from scratch. C99, host-libc freestanding style.
 *
 * Shell integration (bash/zsh):
 *   z() { local d; d=$(~/.echos/bin/z query "${1:-}"); [ -n "$d" ] && cd "$d"; }
 * and add `z add "$PWD"` to your PROMPT_COMMAND / precmd hook.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

#define Z_DB_LINE_MAX 4096

struct zrec {
    char *path;
    double freq;
    double recency; /* unix seconds of last visit */
    double rank;    /* computed frecency score */
};

static char *z_db_path(void)
{
    const char *home = getenv("HOME");
    char *p;
    if (!home || !*home) home = "/tmp";
    p = malloc(strlen(home) + 32);
    if (!p) return NULL;
    sprintf(p, "%s/.echos/z.db", home);
    return p;
}

/* Load db lines: "freq<TAB>recency<TAB>path". */
static struct zrec *z_load(const char *dbpath, size_t *count)
{
    FILE *f = fopen(dbpath, "r");
    struct zrec *recs = NULL;
    size_t n = 0, cap = 0;
    char line[Z_DB_LINE_MAX];

    *count = 0;
    if (!f) return NULL;
    while (fgets(line, sizeof line, f)) {
        char *tab1, *tab2, *end;
        double fr, rc;
        line[strcspn(line, "\r\n")] = '\0';
        tab1 = strchr(line, '\t');
        if (!tab1) continue;
        tab2 = strchr(tab1 + 1, '\t');
        if (!tab2) continue;
        *tab1 = '\0'; *tab2 = '\0';
        fr = strtod(line, &end);
        if (end == line) continue;
        rc = strtod(tab1 + 1, &end);
        if (end == tab1 + 1) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            recs = realloc(recs, cap * sizeof *recs);
            if (!recs) break;
        }
        recs[n].path = strdup(tab2 + 1);
        if (!recs[n].path) break;
        recs[n].freq = fr;
        recs[n].recency = rc;
        n++;
    }
    fclose(f);
    *count = n;
    return recs;
}

static int z_save(const char *dbpath, const struct zrec *recs, size_t n)
{
    FILE *f = fopen(dbpath, "w");
    size_t i;
    if (!f) return -1;
    for (i = 0; i < n; i++)
        fprintf(f, "%.4f\t%.0f\t%s\n", recs[i].freq, recs[i].recency, recs[i].path);
    fclose(f);
    return 0;
}

static void z_mkdir_db(void)
{
    const char *home = getenv("HOME");
    char dir[4096];
    if (!home || !*home) home = "/tmp";
    snprintf(dir, sizeof dir, "%s/.echos", home);
    mkdir(dir, 0755);
}

static void z_add(const char *path)
{
    char *db = z_db_path();
    struct zrec *recs;
    size_t n, i;
    time_t now = time(NULL);
    char resolved[4096];

    if (!db) return;
    z_mkdir_db();
    /* canonicalize relative path */
    if (realpath(path, resolved))
        path = resolved;

    recs = z_load(db, &n);
    for (i = 0; i < n; i++) {
        if (strcmp(recs[i].path, path) == 0) {
            recs[i].freq += 1.0;
            recs[i].recency = (double)now;
            z_save(db, recs, n);
            goto done;
        }
    }
    recs = realloc(recs, (n + 1) * sizeof *recs);
    if (recs) {
        recs[n].path = strdup(path);
        recs[n].freq = 1.0;
        recs[n].recency = (double)now;
        z_save(db, recs, n + 1);
    }
done:
    for (i = 0; i < n; i++) free(recs[i].path);
    free(recs);
    free(db);
}

/* Frecency rank: frequency weighted by recency decay (14-day half-life,
 * computed with our own exponential to avoid libm). */
static double z_exp_neg(double x) /* e^-x, x >= 0 */
{
    /* range-reduce: e^-x = (e^-1)^k * e^-r */
    double result = 1.0, term = 1.0;
    int i;
    if (x > 32.0) return 0.0;
    for (i = 1; i <= 24; i++) {
        term *= -x / i;
        result += term;
        if (result + term == result) break;
    }
    return result;
}

static double z_rank(const struct zrec *r, double now)
{
    double age_days = (now - r->recency) / 86400.0;
    if (age_days < 0) age_days = 0;
    return r->freq * z_exp_neg(age_days * 0.0495106); /* ln2/14 */
}

static int cmp_rank(const void *a, const void *b)
{
    const struct zrec *ra = *(const struct zrec *const *)a;
    const struct zrec *rb = *(const struct zrec *const *)b;
    if (rb->rank > ra->rank) return 1;
    if (rb->rank < ra->rank) return -1;
    return strcmp(ra->path, rb->path);
}

/* case-insensitive substring search */
static int ci_contains(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) return 1;
    for (; *hay; hay++) {
        size_t j = 0;
        while (j < nlen &&
               tolower((unsigned char)hay[j]) == tolower((unsigned char)needle[j]))
            j++;
        if (j == nlen) return 1;
    }
    return 0;
}

static void z_query(const char *term)
{
    char *db = z_db_path();
    struct zrec *recs;
    struct zrec **view;
    size_t n, i, vn = 0;
    double now = (double)time(NULL);

    if (!db) return;
    recs = z_load(db, &n);
    view = malloc((n ? n : 1) * sizeof *view);
    if (!view) goto done;
    for (i = 0; i < n; i++) {
        recs[i].rank = z_rank(&recs[i], now);
        if (*term && !ci_contains(recs[i].path, term))
            continue;
        view[vn++] = &recs[i];
    }
    qsort(view, vn, sizeof *view, cmp_rank);
    if (vn > 0)
        printf("%s\n", view[0]->path);
    else
        fprintf(stderr, "z: no match for '%s'\n", term ? term : "");
done:
    for (i = 0; i < n; i++) free(recs ? recs[i].path : NULL);
    free(recs);
    free(view);
    free(db);
}

static void z_dump(void)
{
    char *db = z_db_path();
    struct zrec *recs;
    struct zrec **view;
    size_t n, i, vn = 0;
    double now = (double)time(NULL);

    if (!db) return;
    recs = z_load(db, &n);
    view = malloc((n ? n : 1) * sizeof *view);
    if (!view) goto done;
    for (i = 0; i < n; i++) {
        recs[i].rank = z_rank(&recs[i], now);
        view[vn++] = &recs[i];
    }
    qsort(view, vn, sizeof *view, cmp_rank);
    for (i = 0; i < vn; i++)
        printf("%8.3f %8.1f %s\n", view[i]->rank, view[i]->freq, view[i]->path);
done:
    for (i = 0; i < n; i++) free(recs ? recs[i].path : NULL);
    free(recs);
    free(view);
    free(db);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: z add PATH | z query TERM | z list\n"
            "shell function: z() { local d=$(z query \"$*\"); [ -n \"$d\" ] && cd \"$d\"; }\n");
        return 2;
    }
    if (strcmp(argv[1], "add") == 0) {
        z_add(argc > 2 ? argv[2] : ".");
        return 0;
    }
    if (strcmp(argv[1], "query") == 0) {
        z_query(argc > 2 ? argv[2] : "");
        return 0;
    }
    if (strcmp(argv[1], "list") == 0) {
        z_dump();
        return 0;
    }
    fprintf(stderr, "z: unknown command %s\n", argv[1]);
    return 2;
}
