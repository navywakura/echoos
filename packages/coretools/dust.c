/* dust — superior du for EchOS.
 * Recursive disk usage printed as a largest-first tree with ASCII
 * percentage bars. -d DEPTH limits tree depth (root always shown).
 * EchOS coretools — proprietary, from scratch. C99.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static long opt_depth = 3;
static unsigned long long g_root_size = 1;

static unsigned long long file_size(const struct stat *st)
{
    return (unsigned long long)st->st_size; /* apparent size */
}

static void human(unsigned long long b, char *out, size_t n)
{
    static const char *u[] = { "B", "K", "M", "G", "T", "P" };
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, n, "%llu B", b);
    else snprintf(out, n, "%.1f %s", v, u[i]);
}

/* Recursively sum apparent sizes under path (including path itself). */
static unsigned long long scan(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *ent;
    unsigned long long total;

    if (stat(path, &st) != 0) return 0;
    total = file_size(&st);
    if (!S_ISDIR(st.st_mode)) return total;

    d = opendir(path);
    if (!d) return total;
    while ((ent = readdir(d)) != NULL) {
        char *sub;
        size_t need;
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        need = strlen(path) + strlen(ent->d_name) + 2;
        sub = malloc(need);
        if (!sub) continue;
        snprintf(sub, need, "%s/%s", path, ent->d_name);
        total += scan(sub);
        free(sub);
    }
    closedir(d);
    return total;
}

struct node {
    char *name;
    char *path;
    unsigned long long size;
};

static int cmp_node(const void *a, const void *b)
{
    const struct node *na = *(const struct node *const *)a;
    const struct node *nb = *(const struct node *const *)b;
    if (nb->size > na->size) return 1;
    if (nb->size < na->size) return -1;
    return strcmp(na->name, nb->name);
}

/* Collect immediate subdirectories of path sorted largest-first. */
static struct node **list_dirs(const char *path, size_t *count)
{
    DIR *d = opendir(path);
    struct dirent *ent;
    struct node **kids = NULL;
    size_t n = 0, cap = 0;

    *count = 0;
    if (!d) return NULL;
    while ((ent = readdir(d)) != NULL) {
        char *sub;
        size_t need;
        struct stat cst;
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        need = strlen(path) + strlen(ent->d_name) + 2;
        sub = malloc(need);
        if (!sub) continue;
        snprintf(sub, need, "%s/%s", path, ent->d_name);
        if (stat(sub, &cst) == 0 && S_ISDIR(cst.st_mode)) {
            struct node *nd = malloc(sizeof *nd);
            if (nd) {
                nd->name = strdup(ent->d_name);
                nd->path = strdup(sub);
                nd->size = scan(sub);
                if (n == cap) {
                    cap = cap ? cap * 2 : 16;
                    kids = realloc(kids, cap * sizeof *kids);
                }
                kids[n++] = nd;
            }
        }
        free(sub);
    }
    closedir(d);
    qsort(kids, n, sizeof *kids, cmp_node);
    *count = n;
    return kids;
}

static void bar_for(unsigned long long size, char *bar)
{
    int pct = (int)((size * 100) / g_root_size), filled, bi;
    if (pct > 100) pct = 100;
    filled = pct / 5;
    if (filled < 1 && size > 0) filled = 1;
    for (bi = 0; bi < 20; bi++) bar[bi] = bi < filled ? '#' : '-';
    bar[20] = '\0';
}

static void print_tree(const char *path, long depth)
{
    struct node **kids;
    size_t n, i;

    if (depth >= opt_depth) return;
    kids = list_dirs(path, &n);
    for (i = 0; i < n; i++) {
        char hs[32], bar[21];
        int pct = (int)((kids[i]->size * 100) / g_root_size);
        bar_for(kids[i]->size, bar);
        human(kids[i]->size, hs, sizeof hs);
        printf("%-8s [%s] %3d%%  %s\n", hs, bar, pct, kids[i]->name);
        /* descend only into children that hold >= 10% of the root */
        if (kids[i]->size >= g_root_size / 10)
            print_tree(kids[i]->path, depth + 1);
    }
    for (i = 0; i < n; i++) {
        free(kids[i]->name); free(kids[i]->path); free(kids[i]);
    }
    free(kids);
}

int main(int argc, char **argv)
{
    const char *root = ".";
    unsigned long long total;
    char hs[32];
    char bar[21];
    int i, bi;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            opt_depth = atol(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "usage: dust [-d DEPTH] [PATH]\n");
            return 2;
        } else {
            root = argv[i];
        }
    }

    total = scan(root);
    g_root_size = total ? total : 1;
    human(total, hs, sizeof hs);
    for (bi = 0; bi < 20; bi++) bar[bi] = '#';
    bar[20] = '\0';
    printf("%-8s [%s] 100%%  %s\n", hs, bar, root);
    print_tree(root, 0);
    return 0;
}
