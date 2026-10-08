/* fd — superior find for EchOS. Recursive filename glob match (* ?).
 * EchOS coretools — proprietary, from scratch. C99, host-libc freestanding style.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static int opt_type_dir = 0;
static int opt_type_file = 0;
static int opt_hidden = 0;          /* -H include hidden entries */
static const char *opt_ext = NULL;  /* -e EXT filter */
static const char *g_pat = ".*";    /* default: everything */
static long g_found = 0;

/* Own iterative glob matcher: supports '*' and '?'. */
static int glob_match(const char *pat, const char *str)
{
    const char *p = pat, *s = str;
    const char *star_p = NULL, *star_s = NULL;

    while (*s) {
        if (*p == '?' || *p == *s) {
            p++; s++;
        } else if (*p == '*') {
            star_p = p++;
            star_s = s;
        } else if (star_p) {
            p = star_p + 1;
            s = ++star_s;
        } else {
            return 0;
        }
    }
    while (*p == '*') p++;
    return *p == '\0';
}

static int name_matches(const char *name)
{
    /* "." is accepted as find-style "match everything" */
    int m = strcmp(g_pat, ".") == 0 ? 1 : glob_match(g_pat, name);
    if (!m) return 0;
    if (!opt_hidden && name[0] == '.') return 0;
    if (opt_ext) {
        size_t nl = strlen(name), el = strlen(opt_ext);
        if (nl <= el || name[nl - el - 1] != '.' ||
            strcmp(name + nl - el, opt_ext) != 0)
            return 0;
    }
    return 1;
}

static void walk(const char *path, int depth)
{
    DIR *d;
    struct dirent *ent;
    struct stat st;

    if (depth > 128) return;

    /* Report the entry itself when it matches. */
    {
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (stat(path, &st) == 0 && name_matches(base)) {
            int ok = 1;
            if (opt_type_file && !S_ISREG(st.st_mode)) ok = 0;
            if (opt_type_dir && !S_ISDIR(st.st_mode)) ok = 0;
            if (ok) { printf("%s\n", path); g_found++; }
        }
    }

    d = opendir(path);
    if (!d) return;
    while ((ent = readdir(d)) != NULL) {
        char *sub;
        size_t need;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        need = strlen(path) + strlen(ent->d_name) + 2;
        sub = malloc(need);
        if (!sub) break;
        snprintf(sub, need, "%s/%s", path, ent->d_name);
        walk(sub, depth + 1); /* walk() handles files and dirs alike */
        free(sub);
    }
    closedir(d);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: fd [options] [PATTERN] [PATH]\n"
        "  -t f   files only\n"
        "  -t d   directories only\n"
        "  -H     include hidden entries\n"
        "  -e EXT match extension EXT (e.g. -e c)\n"
        "PATTERN is a shell-like glob with * and ?; default matches all.\n");
}

int main(int argc, char **argv)
{
    const char *root = ".";
    const char *pat_arg = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            i++;
            if (argv[i][0] == 'f') { opt_type_file = 1; opt_type_dir = 0; }
            else if (argv[i][0] == 'd') { opt_type_dir = 1; opt_type_file = 0; }
            else { usage(); return 2; }
        } else if (strcmp(argv[i], "-H") == 0) {
            opt_hidden = 1;
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            opt_ext = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "fd: unknown option %s\n", argv[i]);
            usage();
            return 2;
        } else if (!pat_arg) {
            pat_arg = argv[i];
        } else {
            root = argv[i];
        }
    }
    if (pat_arg) g_pat = pat_arg;

    walk(root, 0);
    return g_found > 0 ? 0 : 1;
}
