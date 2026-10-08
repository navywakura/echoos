/* Path normalization + cwd resolution for the VFS. */
#include "vfs.h"

/* Join components into an absolute path starting with '/'. */
static void build_abs(char *out, size_t max,
                      char parts[][RXFS_DIR_NAME_LEN], int n) {
    if (!out || max == 0) return;
    if (n <= 0) {
        if (max >= 2) { out[0] = '/'; out[1] = '\0'; }
        else out[0] = '\0';
        return;
    }
    size_t pos = 0;
    if (pos + 1 < max) out[pos++] = '/';
    for (int i = 0; i < n && pos + 1 < max; i++) {
        if (i > 0 && pos + 1 < max) out[pos++] = '/';
        for (size_t j = 0; parts[i][j] && pos + 1 < max; j++)
            out[pos++] = parts[i][j];
    }
    out[pos] = '\0';
}

/* Split path into components, apply . and .., write absolute form. */
bool path_normalize(const char *in, char *out, size_t max) {
    if (!in || !out || max < 2) return false;

    /* Empty -> fail (caller should pass cwd or "/"). */
    if (!in[0]) return false;

    char parts[RXFS_MAX_DEPTH][RXFS_DIR_NAME_LEN];
    int n = 0;
    const char *p = in;

    /* Absolute paths keep root; relative normalization still produces
       absolute form only when path_resolve joins cwd first. Here a relative
       input is normalized component-wise without a leading slash until the
       end — but we always emit absolute paths so the rest of RXFS can treat
       every stored path the same way. Relative inputs without cwd context
       are treated as from root (legacy callers like "profile"). */
    if (*p == '/') p++;

    while (*p && n < RXFS_MAX_DEPTH) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len == 0) continue;

        if (len == 1 && start[0] == '.') {
            continue; /* skip . */
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (n > 0) n--;
            continue;
        }

        if (len >= RXFS_DIR_NAME_LEN) len = RXFS_DIR_NAME_LEN - 1;
        for (size_t i = 0; i < len; i++) parts[n][i] = start[i];
        parts[n][len] = '\0';
        n++;
    }

    build_abs(out, max, parts, n);
    return out[0] != '\0';
}

/* Resolve relative path against cwd, then normalize to absolute. */
bool path_resolve(const char *in, char *out, size_t max) {
    if (!in || !out || max < 2) return false;

    if (!in[0]) {
        const char *cwd = vfs_getcwd();
        return path_normalize(cwd && cwd[0] ? cwd : "/", out, max);
    }

    /* Absolute: normalize only. */
    if (in[0] == '/')
        return path_normalize(in, out, max);

    /* Relative: join with cwd. */
    const char *cwd = vfs_getcwd();
    if (!cwd || !cwd[0]) cwd = "/";

    char joined[RXFS_NAME];
    size_t pos = 0;
    if (cwd[0] == '/' && !cwd[1]) {
        /* cwd is root: "/name" */
        if (pos + 1 < sizeof(joined)) joined[pos++] = '/';
        for (const char *q = in; *q && pos + 1 < sizeof(joined); q++)
            joined[pos++] = *q;
        joined[pos] = '\0';
    } else {
        for (const char *q = cwd; *q && pos + 1 < sizeof(joined); q++)
            joined[pos++] = *q;
        if (pos > 0 && joined[pos - 1] != '/' && pos + 1 < sizeof(joined))
            joined[pos++] = '/';
        for (const char *q = in; *q && pos + 1 < sizeof(joined); q++)
            joined[pos++] = *q;
        joined[pos] = '\0';
    }
    return path_normalize(joined, out, max);
}
