/* eza — superior ls for EchOS. Columns, long mode, hidden, recursive, dirs first.
 * EchOS coretools — proprietary, from scratch. C99, host-libc freestanding style.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

static int opt_long = 0;
static int opt_all = 0;
static int opt_recurse = 0;

struct entry {
    char *name;
    struct stat st;
};

/* Own human-size formatter. */
static void human_size(unsigned long long bytes, char *out, size_t outsz)
{
    const char *units[] = { "B", "K", "M", "G", "T" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    if (u == 0) snprintf(out, outsz, "%lluB", bytes);
    else snprintf(out, outsz, "%.1f%s", v, units[u]);
}

static void mode_string(mode_t m, char *out)
{
    out[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : '-';
    out[1] = (m & S_IRUSR) ? 'r' : '-';
    out[2] = (m & S_IWUSR) ? 'w' : '-';
    out[3] = (m & S_IXUSR) ? 'x' : '-';
    out[4] = (m & S_IRGRP) ? 'r' : '-';
    out[5] = (m & S_IWGRP) ? 'w' : '-';
    out[6] = (m & S_IXGRP) ? 'x' : '-';
    out[7] = (m & S_IROTH) ? 'r' : '-';
    out[8] = (m & S_IWOTH) ? 'w' : '-';
    out[9] = (m & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static void format_mtime(time_t t, char *out, size_t outsz)
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, outsz, "%Y-%m-%d %H:%M", &tmv);
}

static int cmp_entry(const void *a, const void *b)
{
    const struct entry *ea = a, *eb = b;
    int da = S_ISDIR(ea->st.st_mode), db = S_ISDIR(eb->st.st_mode);
    if (da != db) return db - da; /* dirs first */
    return strcmp(ea->name, eb->name);
}

static void list_dir(const char *path, int depth)
{
    DIR *d;
    struct dirent *ent;
    struct entry *ents = NULL;
    size_t n = 0, cap = 0, i, maxlen = 0, cols, rows, r, c;

    if (depth > 16) return;
    d = opendir(path);
    if (!d) {
        fprintf(stderr, "eza: cannot open %s\n", path);
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        struct entry e;
        size_t need;
        char *full;
        if (!opt_all && ent->d_name[0] == '.') continue;
        need = strlen(path) + strlen(ent->d_name) + 2;
        full = malloc(need);
        if (!full) break;
        snprintf(full, need, "%s/%s", path, ent->d_name);
        memset(&e.st, 0, sizeof e.st);
        if (stat(full, &e.st) != 0) {
            /* keep zeroed stat on failure */
        }
        free(full);
        e.name = strdup(ent->d_name);
        if (!e.name) break;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            ents = realloc(ents, cap * sizeof *ents);
            if (!ents) break;
        }
        ents[n++] = e;
        if (strlen(e.name) > maxlen) maxlen = strlen(e.name);
    }
    closedir(d);

    qsort(ents, n, sizeof *ents, cmp_entry);

    printf("%s:\n", path);
    if (opt_long) {
        for (i = 0; i < n; i++) {
            char perms[11], mtime[32], size[32];
            mode_string(ents[i].st.st_mode, perms);
            human_size((unsigned long long)ents[i].st.st_size, size, sizeof size);
            format_mtime(ents[i].st.st_mtime, mtime, sizeof mtime);
            printf("%s %8ld %6s %s  %s\n", perms,
                   (long)ents[i].st.st_nlink,
                   size, mtime, ents[i].name);
        }
    } else {
        cols = maxlen ? 78 / (maxlen + 2) : 1;
        if (cols < 1) cols = 1;
        rows = (n + cols - 1) / cols;
        for (r = 0; r < rows; r++) {
            for (c = 0; c < cols; c++) {
                size_t idx = c * rows + r;
                if (idx < n)
                    printf("%-*s", (int)(maxlen + 2), ents[idx].name);
            }
            putchar('\n');
        }
    }

    if (opt_recurse) {
        for (i = 0; i < n; i++) {
            size_t need = strlen(path) + strlen(ents[i].name) + 2;
            char *sub = malloc(need);
            struct stat st;
            if (!sub) continue;
            snprintf(sub, need, "%s/%s", path, ents[i].name);
            if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode))
                list_dir(sub, depth + 1);
            free(sub);
        }
    }

    for (i = 0; i < n; i++) free(ents[i].name);
    free(ents);
}

static void usage(void)
{
    fprintf(stderr, "usage: eza [-laR] [PATH...]\n"
                    "  -l  long mode   -a  include hidden\n"
                    "  -R  recursive   dirs sort before files\n");
}

int main(int argc, char **argv)
{
    int i, paths = 0;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            const char *f;
            for (f = argv[i] + 1; *f; f++) {
                switch (*f) {
                case 'l': opt_long = 1; break;
                case 'a': opt_all = 1; break;
                case 'R': opt_recurse = 1; break;
                default:
                    fprintf(stderr, "eza: unknown option -%c\n", *f);
                    usage();
                    return 2;
                }
            }
        } else {
            paths++;
            list_dir(argv[i], 0);
        }
    }
    if (paths == 0)
        list_dir(".", 0);
    return 0;
}
