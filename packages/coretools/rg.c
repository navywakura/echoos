/* rg — superior grep for EchOS. Recursive search with line numbers.
 * EchOS coretools — proprietary, from scratch. C99, host-libc freestanding style.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define RG_MAXLINE 65536

static const char *g_pattern = NULL;
static size_t g_plen = 0;
static int opt_ignore_case = 0;
static int opt_invert = 0;
static int opt_number = 1; /* line numbers on by default; -n is explicit */
static long g_matches = 0;
static long g_files_matched = 0;
static long g_lines_scanned = 0;

/* Case-insensitive substring search (own implementation). */
static const char *ci_strstr(const char *hay, const char *needle, size_t nlen)
{
    if (nlen == 0) return hay;
    for (; *hay; hay++) {
        size_t j = 0;
        while (j < nlen &&
               tolower((unsigned char)hay[j]) == tolower((unsigned char)needle[j]))
            j++;
        if (j == nlen) return hay;
    }
    return NULL;
}

static int line_matches(const char *line)
{
    int m;
    if (opt_ignore_case)
        m = ci_strstr(line, g_pattern, g_plen) != NULL;
    else
        m = strstr(line, g_pattern) != NULL;
    return opt_invert ? !m : m;
}

/* Search one file; returns number of matching lines. */
static long search_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char buf[RG_MAXLINE];
    long lineno = 0, hits = 0;

    if (!f) {
        fprintf(stderr, "rg: cannot open %s\n", path);
        return -1;
    }
    while (fgets(buf, sizeof buf, f)) {
        size_t len = strlen(buf);
        lineno++;
        g_lines_scanned++;
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';
        if (line_matches(buf)) {
            hits++;
            printf("%s:", path);
            if (opt_number) printf("%ld:", lineno);
            printf("%s\n", buf);
        }
    }
    fclose(f);
    return hits;
}

/* Search stdin; returns number of matching lines. */
static long search_stdin(void)
{
    char buf[RG_MAXLINE];
    long lineno = 0, hits = 0;

    while (fgets(buf, sizeof buf, stdin)) {
        size_t len = strlen(buf);
        lineno++;
        g_lines_scanned++;
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';
        if (line_matches(buf)) {
            hits++;
            printf("<stdin>:");
            if (opt_number) printf("%ld:", lineno);
            printf("%s\n", buf);
        }
    }
    return hits;
}

static int path_is_hidden(const char *name)
{
    return name[0] == '.';
}

static void search_path(const char *path, int depth)
{
    struct stat st;

    if (depth > 64) return;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "rg: cannot stat %s\n", path);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *ent;
        if (!d) {
            fprintf(stderr, "rg: cannot read dir %s\n", path);
            return;
        }
        while ((ent = readdir(d)) != NULL) {
            char *sub;
            size_t need;
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            if (path_is_hidden(ent->d_name))
                continue;
            need = strlen(path) + strlen(ent->d_name) + 2;
            sub = malloc(need);
            if (!sub) { fprintf(stderr, "rg: out of memory\n"); closedir(d); return; }
            snprintf(sub, need, "%s/%s", path, ent->d_name);
            search_path(sub, depth + 1);
            free(sub);
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode)) {
        long h = search_file(path);
        if (h > 0) {
            g_matches += h;
            g_files_matched++;
        }
    }
}

static void usage(void)
{
    fprintf(stderr,
        "usage: rg [options] PATTERN [PATH...]\n"
        "  -i   ignore case\n"
        "  -v   invert match (print non-matching lines)\n"
        "  -n   prefix line numbers (default on)\n"
        "  -N   suppress line numbers\n"
        "PATH may be a file or directory (recursed). Default PATH: .\n");
}

int main(int argc, char **argv)
{
    int i, first_path = -1;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            const char *f;
            for (f = argv[i] + 1; *f; f++) {
                switch (*f) {
                case 'i': opt_ignore_case = 1; break;
                case 'v': opt_invert = 1; break;
                case 'n': opt_number = 1; break;
                case 'N': opt_number = 0; break;
                default:
                    fprintf(stderr, "rg: unknown option -%c\n", *f);
                    usage();
                    return 2;
                }
            }
        } else {
            first_path = i;
            break;
        }
    }
    if (first_path < 0 || first_path >= argc) {
        usage();
        return 2;
    }
    g_pattern = argv[first_path];
    g_plen = strlen(g_pattern);

    if (first_path + 1 == argc) {
        /* no explicit path: recurse current dir */
        search_path(".", 0);
    } else {
        for (i = first_path + 1; i < argc; i++)
            if (strcmp(argv[i], "-") == 0) {
                long h = search_stdin();
                if (h > 0) { g_matches += h; g_files_matched++; }
            } else {
                search_path(argv[i], 0);
            }
    }

    fprintf(stderr, "rg: %ld match(es) in %ld file(s), %ld line(s) scanned\n",
            g_matches, g_files_matched, g_lines_scanned);
    return g_matches > 0 ? 0 : 1;
}
