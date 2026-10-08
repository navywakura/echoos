/* bat — superior cat for EchOS. Line numbers, gutter, headers, page ranges.
 * EchOS coretools — proprietary, from scratch. C99, host-libc freestanding style.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define BAT_MAXLINE 65536

static long opt_first = 1;   /* -r first:last (1-based) */
static long opt_last = -1;   /* -1 = to end of file */
static int multiple_files = 0;
static int g_status = 0;

static void print_header(const char *name, const char *tag)
{
    size_t n;
    printf("\n  %s%s %s ", tag, name, tag);
    for (n = strlen(name) + strlen(tag) * 2 + 4; n < 72; n++)
        putchar('-');
    putchar('\n');
}

static void cat_file(FILE *f)
{
    char buf[BAT_MAXLINE];
    long lineno = 0;

    while (fgets(buf, sizeof buf, f)) {
        size_t len = strlen(buf);
        lineno++;
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';
        if (lineno < opt_first) continue;
        if (opt_last >= 0 && lineno > opt_last) break;
        printf("%6ld | %s\n", lineno, buf);
    }
}

static void do_file(const char *path)
{
    FILE *f = stdin;

    if (strcmp(path, "-") != 0) {
        f = fopen(path, "r");
        if (!f) {
            fprintf(stderr, "bat: cannot open %s\n", path);
            g_status = 1;
            return;
        }
    } else if (multiple_files) {
        /* stdin among many files: no header needed but keep order */
    }
    if (multiple_files && path[0] != '\0')
        print_header(strcmp(path, "-") == 0 ? "<stdin>" : path, "==");
    cat_file(f);
    if (f != stdin) fclose(f);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: bat [options] [FILE...]\n"
        "  -r N:M   print lines N through M only (1-based)\n"
        "  '-' reads stdin.\n");
}

int main(int argc, char **argv)
{
    int i, nfiles = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            char *arg = argv[++i];
            char *colon = strchr(arg, ':');
            if (colon) {
                *colon = '\0';
                opt_first = atol(arg);
                opt_last = atol(colon + 1);
            } else {
                opt_first = atol(arg);
                opt_last = atol(arg); /* single line */
            }
            if (opt_first < 1) opt_first = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0' &&
                   strcmp(argv[i], "-") != 0) {
            fprintf(stderr, "bat: unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    /* count file args */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) { i++; continue; }
        if (argv[i][0] == '-' && argv[i][1] != '\0' && strcmp(argv[i], "-") != 0)
            continue;
        nfiles++;
    }
    if (nfiles == 0) {
        cat_file(stdin);
        return g_status;
    }
    multiple_files = nfiles > 1;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) { i++; continue; }
        if (argv[i][0] == '-' && argv[i][1] != '\0' && strcmp(argv[i], "-") != 0)
            continue;
        do_file(argv[i]);
    }
    return g_status;
}
