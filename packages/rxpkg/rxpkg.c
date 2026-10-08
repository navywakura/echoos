/* rx-pkg — the EchOS package manager.
 * Builds, installs, queries and removes .rxp packages (see SPEC.md).
 *
 *   rx-pkg build PKGDIR -o OUT.rxp     pack a directory into a package;
 *                                      PKGDIR/manifest holds Name/Version/
 *                                      Description; everything else is payload
 *   rx-pkg install FILE.rxp [--root R] extract under prefix R, record install
 *   rx-pkg unpack FILE.rxp [-C DIR]    raw extraction, no database update
 *   rx-pkg list                        list installed packages
 *   rx-pkg files NAME                  list files owned by an installed pkg
 *   rx-pkg info NAME|FILE.rxp          show metadata
 *   rx-pkg remove NAME                 delete recorded files + record
 *
 * EchOS packages — proprietary, from scratch. C99. Only stdio/stdlib/string,
 * ctype, dirent and sys/stat are used.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RX_MAXPATH 4096
#define RX_META_MAX 8192

/* ---------------- little-endian helpers ---------------- */

static void put_le16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}
static void put_le32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}
static void put_le64(unsigned char *p, unsigned long long v)
{
    int i;
    for (i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static unsigned get_le16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static unsigned long get_le32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static unsigned long long get_le64(const unsigned char *p)
{
    unsigned long long v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* checked writer (short-lived CLI: abort on hard I/O failure) */
static void xfwrite(const void *p, size_t n, FILE *f)
{
    if (n && fwrite(p, 1, n, f) != n) {
        fprintf(stderr, "rx-pkg: write error\n");
        exit(1);
    }
}

/* ---------------- environment ---------------- */

static const char *pkg_root(void)
{
    static char root[RX_MAXPATH];
    const char *env = getenv("ECHOS_ROOT");
    if (!root[0]) {
        if (env && *env)
            snprintf(root, sizeof root, "%s", env);
        else {
            const char *home = getenv("HOME");
            if (!home || !*home) home = "/tmp";
            snprintf(root, sizeof root, "%s/.echos/root", home);
        }
    }
    return root;
}

static const char *db_dir(void)
{
    static char db[RX_MAXPATH];
    const char *env = getenv("ECHOS_PKGDB");
    if (!db[0]) {
        if (env && *env)
            snprintf(db, sizeof db, "%s", env);
        else {
            const char *home = getenv("HOME");
            if (!home || !*home) home = "/tmp";
            snprintf(db, sizeof db, "%s/.echos/rxpkg", home);
        }
    }
    return db;
}

static void mkdir_p(char *path)
{
    size_t i, len = strlen(path);
    for (i = 1; i < len; i++) {
        if (path[i] == '/') {
            path[i] = '\0';
            mkdir(path, 0755);
            path[i] = '/';
        }
    }
    mkdir(path, 0755);
}

/* create every directory component of path except the last element */
static void ensure_parents(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) return;
    *slash = '\0';
    mkdir_p(path);
    *slash = '/';
}

static int safe_member(const char *p)
{
    if (!p || !*p) return 0;
    if (p[0] == '/') return 0;
    if (!strcmp(p, "..") || !strncmp(p, "../", 3)) return 0;
    if (strstr(p, "/../")) return 0;
    if (strstr(p, "//")) return 0;
    return 1;
}

/* ---------------- metadata ---------------- */

struct meta {
    char name[256];
    char version[128];
    char desc[1024];
};

static void meta_init(struct meta *m)
{
    m->name[0] = m->version[0] = m->desc[0] = '\0';
}

static void meta_parse(struct meta *m, const char *text)
{
    char buf[RX_META_MAX];
    char *line, *save;

    snprintf(buf, sizeof buf, "%s", text);
    for (line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(line, ':');
        char *key, *val;
        if (!colon) continue;
        *colon = '\0';
        key = line;
        val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;
        if (!strcmp(key, "Name"))             snprintf(m->name, sizeof m->name, "%s", val);
        else if (!strcmp(key, "Version"))     snprintf(m->version, sizeof m->version, "%s", val);
        else if (!strcmp(key, "Description")) snprintf(m->desc, sizeof m->desc, "%s", val);
    }
}

static int valid_pkg_name(const char *s)
{
    const char *c;
    if (!*s) return 0;
    for (c = s; *c; c++) {
        if (!islower((unsigned char)*c) && !isdigit((unsigned char)*c) &&
            *c != '.' && *c != '_' && *c != '-')
            return 0;
    }
    return 1;
}

/* ---------------- string list (install records) ---------------- */

struct strlist {
    char **v;
    size_t n, cap;
};

static void sl_add(struct strlist *sl, const char *s)
{
    if (sl->n == sl->cap) {
        sl->cap = sl->cap ? sl->cap * 2 : 32;
        sl->v = realloc(sl->v, sl->cap * sizeof *sl->v);
        if (!sl->v) { fprintf(stderr, "rx-pkg: out of memory\n"); exit(1); }
    }
    sl->v[sl->n++] = strdup(s);
}

static void sl_free(struct strlist *sl)
{
    size_t i;
    for (i = 0; i < sl->n; i++) free(sl->v[i]);
    free(sl->v);
    sl->v = NULL;
    sl->n = sl->cap = 0;
}

/* ---------------- reader core ---------------- */

static int rdfull_check(FILE *f, void *p, size_t n)
{
    return n == 0 || fread(p, 1, n, f) == n;
}

/* Reads and validates the 16-byte header, loads metadata into m,
 * returns the entry count. Leaves f positioned at the first entry. */
static unsigned long rxp_open(FILE *f, struct meta *m)
{
    unsigned char hdr[16];
    static char metabuf[RX_META_MAX + 1];
    unsigned long version, metalen;

    if (!rdfull_check(f, hdr, 16)) goto bad;
    if (memcmp(hdr, "RXP1", 4) != 0) goto bad;
    version = get_le32(hdr + 4);
    metalen = get_le32(hdr + 8);
    if (version != 1 || metalen > RX_META_MAX) goto bad;
    if (!rdfull_check(f, metabuf, metalen)) goto bad;
    metabuf[metalen] = '\0';
    meta_init(m);
    meta_parse(m, metabuf);
    return get_le32(hdr + 12);
bad:
    fprintf(stderr, "rx-pkg: not a valid RXP1 package\n");
    exit(1);
}

/* Extracts entries beginning at the current position of f into dest.
 * If rec != NULL, every written path is appended. Returns 0 on success. */
static int rxp_extract(FILE *f, const char *dest, unsigned long count,
                       struct strlist *rec)
{
    unsigned long i;
    static unsigned char pl[2], mh[12];
    static char name[RX_MAXPATH];

    for (i = 0; i < count; i++) {
        unsigned plen, mode;
        unsigned long long size, left;
        int isdir;

        if (!rdfull_check(f, pl, 2)) goto bad;
        plen = get_le16(pl);
        if (plen == 0 || plen >= sizeof name) goto bad;
        if (!rdfull_check(f, name, plen)) goto bad;
        name[plen] = '\0';
        if (!rdfull_check(f, mh, 12)) goto bad;
        mode = (unsigned)get_le32(mh);
        size = get_le64(mh + 4);
        if (!safe_member(name)) {
            fprintf(stderr, "rx-pkg: refusing unsafe member '%s'\n", name);
            goto bad;
        }

        isdir = (name[plen - 1] == '/');
        {
            char clean[RX_MAXPATH], full[RX_MAXPATH + RX_MAXPATH];
            snprintf(clean, sizeof clean, "%s", name);
            if (isdir && clean[0])
                clean[strlen(clean) - 1] = '\0';

            if (dest && *dest)
                snprintf(full, sizeof full, "%s%s/%s",
                         dest, dest[strlen(dest) - 1] == '/' ? "" : "/", clean);
            else
                snprintf(full, sizeof full, "%s", clean);

            if (isdir) {
                mkdir_p(full);
            } else {
                char tmp[RX_MAXPATH + RX_MAXPATH];
                FILE *out;
                snprintf(tmp, sizeof tmp, "%s", full);
                ensure_parents(tmp);
                out = fopen(full, "wb");
                if (!out) {
                    fprintf(stderr, "rx-pkg: cannot create %s\n", full);
                    goto bad;
                }
                left = size;
                while (left > 0) {
                    static char buf[65536];
                    size_t want = left > sizeof buf ? sizeof buf : (size_t)left;
                    if (!rdfull_check(f, buf, want)) { fclose(out); goto bad; }
                    xfwrite(buf, want, out);
                    left -= want;
                }
                fclose(out);
                chmod(full, mode & 0777 ? mode & 0777 : 0644);
            }
            if (rec) sl_add(rec, clean);
        }
    }
    return 0;
bad:
    fprintf(stderr, "rx-pkg: corrupt or truncated package\n");
    return -1;
}

/* ---------------- build ---------------- */

struct fentry {
    char rel[RX_MAXPATH]; /* exactly as stored ('/' suffix marks a dir) */
    unsigned mode;
    unsigned long long size;
};

static struct fentry *g_files;
static size_t g_nfiles, g_capfiles;

static void add_entry(const char *rel, unsigned mode, unsigned long long size)
{
    if (g_nfiles == g_capfiles) {
        g_capfiles = g_capfiles ? g_capfiles * 2 : 64;
        g_files = realloc(g_files, g_capfiles * sizeof *g_files);
        if (!g_files) { fprintf(stderr, "rx-pkg: out of memory\n"); exit(1); }
    }
    snprintf(g_files[g_nfiles].rel, RX_MAXPATH, "%s", rel);
    g_files[g_nfiles].mode = mode;
    g_files[g_nfiles].size = size;
    g_nfiles++;
}

static void collect_tree(const char *base, const char *rel)
{
    DIR *d;
    struct dirent *ent;
    char abs[RX_MAXPATH];
    char subabs[RX_MAXPATH * 2];

    snprintf(abs, sizeof abs, "%s/%s", base, rel);
    d = opendir(abs);
    if (!d) return;
    while ((ent = readdir(d)) != NULL) {
        char subrel[RX_MAXPATH], stored[RX_MAXPATH * 2];
        struct stat st;

        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;
        /* never ship the manifest itself */
        if (!strcmp(rel, ".") && !strcmp(ent->d_name, "manifest"))
            continue;

        if (!strcmp(rel, "."))
            snprintf(subrel, sizeof subrel, "%s", ent->d_name);
        else
            snprintf(subrel, sizeof subrel, "%s/%s", rel, ent->d_name);
        snprintf(subabs, sizeof subabs, "%s/%s", base, subrel);

        if (stat(subabs, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            snprintf(stored, sizeof stored, "%s/", subrel);
            add_entry(stored, 0755, 0);
            collect_tree(base, subrel);
        } else if (S_ISREG(st.st_mode)) {
            add_entry(subrel, (unsigned)(st.st_mode & 0777),
                      (unsigned long long)st.st_size);
        }
    }
    closedir(d);
}

static int cmp_fentry(const void *a, const void *b)
{
    return strcmp(((const struct fentry *)a)->rel,
                  ((const struct fentry *)b)->rel);
}

static void write_entry_payload(FILE *out, const struct fentry *e,
                                const char *base)
{
    static char buf[65536];
    unsigned long long left = e->size;

    if (left == 0 || e->rel[0] == '\0' ||
        e->rel[strlen(e->rel) - 1] == '/')
        return;
    {
        char abs[RX_MAXPATH * 2];
        FILE *in;
        snprintf(abs, sizeof abs, "%s/%s", base, e->rel);
        in = fopen(abs, "rb");
        if (!in) {
            fprintf(stderr, "rx-pkg: cannot read %s\n", abs);
            exit(1);
        }
        while (left > 0) {
            size_t want = left > sizeof buf ? sizeof buf : (size_t)left;
            size_t got = fread(buf, 1, want, in);
            if (got != want) {
                fprintf(stderr, "rx-pkg: %s shrank while packing\n", abs);
                exit(1);
            }
            xfwrite(buf, got, out);
            left -= got;
        }
        fclose(in);
    }
}

static int cmd_build(const char *pkgdir, const char *outpath)
{
    char mfpath[RX_MAXPATH];
    char line[2048];
    char metabuf[RX_META_MAX];
    FILE *mf, *out;
    struct meta m;
    unsigned long metalen;
    unsigned char hdr[16], pl[2], mh[12];
    size_t i;

    meta_init(&m);
    snprintf(mfpath, sizeof mfpath, "%s/manifest", pkgdir);
    mf = fopen(mfpath, "r");
    if (!mf) {
        fprintf(stderr, "rx-pkg: %s has no manifest\n", pkgdir);
        return 1;
    }
    {
        char text[RX_META_MAX];
        size_t used = 0;
        text[0] = '\0';
        while (fgets(line, sizeof line, mf) &&
               used + strlen(line) < sizeof text) {
            memcpy(text + used, line, strlen(line) + 1);
            used += strlen(line);
        }
        fclose(mf);
        meta_init(&m);
        meta_parse(&m, text);
    }

    if (!valid_pkg_name(m.name)) {
        fprintf(stderr,
                "rx-pkg: manifest needs a Name matching [a-z0-9._-]+\n");
        return 1;
    }

    g_nfiles = g_capfiles = 0;
    collect_tree(pkgdir, ".");
    qsort(g_files, g_nfiles, sizeof *g_files, cmp_fentry);

    metalen = (unsigned long)snprintf(
        metabuf, sizeof metabuf,
        "Name: %s\nVersion: %s\nDescription: %s\n",
        m.name, m.version, m.desc);

    out = fopen(outpath, "wb");
    if (!out) {
        fprintf(stderr, "rx-pkg: cannot write %s\n", outpath);
        return 1;
    }

    memcpy(hdr, "RXP1", 4);
    put_le32(hdr + 4, 1);
    put_le32(hdr + 8, metalen);
    put_le32(hdr + 12, (unsigned long)g_nfiles);
    xfwrite(hdr, 16, out);
    xfwrite(metabuf, metalen, out);

    for (i = 0; i < g_nfiles; i++) {
        const struct fentry *e = &g_files[i];
        size_t len = strlen(e->rel);

        put_le16(pl, (unsigned)len);
        xfwrite(pl, 2, out);
        xfwrite(e->rel, len, out);
        put_le32(mh, e->mode);
        put_le64(mh + 4, e->size);
        xfwrite(mh, 12, out);
        write_entry_payload(out, e, pkgdir);
    }
    if (fclose(out) != 0) {
        fprintf(stderr, "rx-pkg: close failed on %s\n", outpath);
        return 1;
    }
    printf("built %s (%lu entries)\n", outpath, (unsigned long)g_nfiles);
    free(g_files);
    g_files = NULL;
    return 0;
}

/* ---------------- install / unpack / remove ---------------- */

static int cmd_install(const char *file, const char *root_override)
{
    FILE *f = fopen(file, "rb");
    struct meta m;
    unsigned long count;
    struct strlist rec = { NULL, 0, 0 };
    char recpath[RX_MAXPATH * 2];
    FILE *rf;
    size_t i;
    const char *root = root_override ? root_override : pkg_root();

    if (!f) { fprintf(stderr, "rx-pkg: cannot open %s\n", file); return 1; }
    count = rxp_open(f, &m);
    if (!valid_pkg_name(m.name)) {
        fprintf(stderr, "rx-pkg: package has invalid Name\n");
        fclose(f);
        return 1;
    }
    if (rxp_extract(f, root, count, &rec) != 0) {
        fclose(f);
        sl_free(&rec);
        return 1;
    }
    fclose(f);

    /* database record */
    {
        char instdir[RX_MAXPATH];
        snprintf(instdir, sizeof instdir, "%s/installed", db_dir());
        mkdir_p(instdir);
        snprintf(recpath, sizeof recpath, "%s/installed/%s",
                 db_dir(), m.name);
        rf = fopen(recpath, "w");
        if (!rf) {
            fprintf(stderr, "rx-pkg: cannot update database (%s)\n", recpath);
            sl_free(&rec);
            return 1;
        }
        fprintf(rf, "Name: %s\n", m.name);
        fprintf(rf, "Version: %s\n", m.version);
        fprintf(rf, "Description: %s\n", m.desc);
        fprintf(rf, "Root: %s\n", root);
        for (i = 0; i < rec.n; i++)
            fprintf(rf, "File: %s\n", rec.v[i]);
        fclose(rf);
    }
    printf("installed %s %s (%lu files)\n",
           m.name, m.version, (unsigned long)rec.n);
    sl_free(&rec);
    return 0;
}

static int cmd_unpack(const char *file, const char *destdir)
{
    FILE *f = fopen(file, "rb");
    struct meta m;
    unsigned long count;

    if (!f) { fprintf(stderr, "rx-pkg: cannot open %s\n", file); return 1; }
    count = rxp_open(f, &m);
    if (rxp_extract(f, destdir ? destdir : ".", count, NULL) != 0) {
        fclose(f);
        return 1;
    }
    fclose(f);
    printf("unpacked %s into %s\n", m.name,
           destdir ? destdir : ".");
    return 0;
}

static void rmdir_parents(char *path, const char *stop)
{
    char *slash;
    while (strlen(path) > strlen(stop)) {
        slash = strrchr(path, '/');
        if (!slash) break;
        *slash = '\0';
        if (strlen(path) <= strlen(stop)) break;
        if (rmdir(path) != 0) break;
    }
}

static int record_path(char *out, size_t outsz, const char *name)
{
    const char *db = db_dir();
    size_t l1 = strlen(db), l2 = strlen(name);
    if (l1 + l2 + 16 >= outsz || l2 > 255) return -1;
    memcpy(out, db, l1);
    memcpy(out + l1, "/installed/", 11);
    memcpy(out + l1 + 11, name, l2 + 1);
    return 0;
}

static int cmd_remove(const char *name)
{
    char recpath[RX_MAXPATH * 2], line[RX_MAXPATH * 2];
    char rootbuf[RX_MAXPATH] = "";
    FILE *f;
    struct strlist files = { NULL, 0, 0 };
    size_t i;

    if (!valid_pkg_name(name)) {
        fprintf(stderr, "rx-pkg: invalid package name '%s'\n", name);
        return 1;
    }
    record_path(recpath, sizeof recpath, name);
    f = fopen(recpath, "r");
    if (!f) {
        fprintf(stderr, "rx-pkg: '%s' is not installed\n", name);
        return 1;
    }
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "File: ", 6))
            sl_add(&files, line + 6);
        else if (!strncmp(line, "Root: ", 6))
            snprintf(rootbuf, sizeof rootbuf, "%.*s",
                     (int)sizeof rootbuf - 1, line + 6);
    }
    fclose(f);

    {
        const char *root = rootbuf[0] ? rootbuf : pkg_root();
        for (i = 0; i < files.n; i++) {
            char full[RX_MAXPATH * 2];
            char parent[RX_MAXPATH * 2];
            snprintf(full, sizeof full, "%s/%s", root, files.v[i]);
            remove(full);
            snprintf(parent, sizeof parent, "%s", full);
            rmdir_parents(parent, root);
        }
    }
    sl_free(&files);
    remove(recpath);
    printf("removed %s\n", name);
    return 0;
}

/* ---------------- query commands ---------------- */

static int cmd_list(void)
{
    char instdir[RX_MAXPATH];
    DIR *d;
    struct dirent *ent;
    struct { char v[256]; } names[1024];
    size_t n = 0, i, j;

    snprintf(instdir, sizeof instdir, "%s/installed", db_dir());
    d = opendir(instdir);
    if (!d) return 0; /* nothing installed */
    while ((ent = readdir(d)) != NULL && n < 1024) {
        if (ent->d_name[0] == '.') continue;
        snprintf(names[n].v, sizeof names[n].v, "%s", ent->d_name);
        n++;
    }
    closedir(d);
    for (i = 0; i + 1 < n; i++)
        for (j = 0; j + 1 < n - i; j++)
            if (strcmp(names[j].v, names[j + 1].v) > 0) {
                char t[256];
                memcpy(t, names[j].v, sizeof t);
                memcpy(names[j].v, names[j + 1].v, sizeof t);
                memcpy(names[j + 1].v, t, sizeof t);
            }
    for (i = 0; i < n; i++) {
        char recpath[RX_MAXPATH * 2], line[2048];
        char ver[128] = "-", desc[512] = "-";
        FILE *f;
        record_path(recpath, sizeof recpath, names[i].v);
        f = fopen(recpath, "r");
        if (f) {
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = '\0';
                if (!strncmp(line, "Version: ", 9))
                    snprintf(ver, sizeof ver, "%.*s", (int)sizeof ver - 1, line + 9);
                else if (!strncmp(line, "Description: ", 13))
                    snprintf(desc, sizeof desc, "%.*s", (int)sizeof desc - 1, line + 13);
            }
            fclose(f);
        }
        printf("%-20s %-10s %s\n", names[i].v, ver, desc);
    }
    return 0;
}

static int cmd_files(const char *name)
{
    char recpath[RX_MAXPATH * 2], line[RX_MAXPATH * 2];
    FILE *f;
    int any = 0;

    if (!valid_pkg_name(name)) {
        fprintf(stderr, "rx-pkg: invalid package name '%s'\n", name);
        return 1;
    }
    record_path(recpath, sizeof recpath, name);
    f = fopen(recpath, "r");
    if (!f) {
        fprintf(stderr, "rx-pkg: '%s' is not installed\n", name);
        return 1;
    }
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!strncmp(line, "File: ", 6)) {
            printf("%s\n", line + 6);
            any++;
        }
    }
    fclose(f);
    if (!any)
        fprintf(stderr, "rx-pkg: no files recorded for %s\n", name);
    return 0;
}

static void print_meta(const struct meta *m)
{
    printf("Name: %s\n", m->name);
    printf("Version: %s\n", m->version[0] ? m->version : "-");
    printf("Description: %s\n", m->desc[0] ? m->desc : "-");
}

/* info NAME        -> show installed record
 * info FILE.rxp    -> show package header metadata */
static int cmd_info(const char *arg)
{
    struct stat st;
    FILE *f;

    if (stat(arg, &st) == 0 && S_ISREG(st.st_mode)) {
        struct meta m;
        f = fopen(arg, "rb");
        if (!f) {
            fprintf(stderr, "rx-pkg: cannot open %s\n", arg);
            return 1;
        }
        rxp_open(f, &m); /* validates + parses; exits on bad format */
        fclose(f);
        print_meta(&m);
        return 0;
    }

    {
        char recpath[RX_MAXPATH * 2], line[2048];
        char ver[128] = "-", desc[512] = "-";
        int found = 0;
        record_path(recpath, sizeof recpath, arg);
        f = fopen(recpath, "r");
        if (f) {
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = '\0';
                if (!strncmp(line, "Version: ", 9))
                    snprintf(ver, sizeof ver, "%.*s", (int)sizeof ver - 1, line + 9);
                else if (!strncmp(line, "Description: ", 13))
                    snprintf(desc, sizeof desc, "%.*s", (int)sizeof desc - 1, line + 13);
            }
            fclose(f);
            found = 1;
        }
        if (!found) {
            fprintf(stderr, "rx-pkg: '%s' not installed\n", arg);
            return 1;
        }
        {
            struct meta m;
            meta_init(&m);
            snprintf(m.name, sizeof m.name, "%s", arg);
            snprintf(m.version, sizeof m.version, "%s", ver);
            snprintf(m.desc, sizeof m.desc, "%s", desc);
            print_meta(&m);
        }
        return 0;
    }
}

static const char *tool_name(const char *argv0)
{
#ifdef EPK_CLI
    (void)argv0;
    return "epk";
#else
    (void)argv0;
    return "rx-pkg";
#endif
}

static const char *base_name(const char *p)
{
    const char *s = p;
    if (!p) return "";
    while (*p) {
        if (*p == '/') s = p + 1;
        p++;
    }
    return s;
}

int main(int argc, char **argv)
{
    const char *tool = tool_name(argv[0]);
    const char *base = base_name(argv[0]);

    /* OpenBSD-style helper binaries: epk_add FILE.rxp, epk_delete NAME */
    if (!strcmp(base, "epk_add")) {
        if (argc < 2) {
            fprintf(stderr, "usage: epk_add FILE.rxp [--root R]\n");
            return 2;
        }
        return cmd_install(argv[1], argc > 3 && !strcmp(argv[2], "--root") ? argv[3] : NULL);
    }
    if (!strcmp(base, "epk_delete")) {
        if (argc < 2) {
            fprintf(stderr, "usage: epk_delete NAME\n");
            return 2;
        }
        return cmd_remove(argv[1]);
    }

    if (argc < 2) {
        fprintf(stderr,
            "usage:\n"
            "  %s build PKGDIR -o OUT.rxp\n"
            "  %s install FILE.rxp [--root R]   (alias: add, epk_add)\n"
            "  %s unpack FILE.rxp [-C DIR]\n"
            "  %s list | files NAME | info [NAME|FILE.rxp] | remove NAME\n"
            "  %s delete NAME                   (alias: epk_delete)\n"
            "Local media only when EPK_MEDIA is set (default /media/epk).\n",
            tool, tool, tool, tool, tool);
        return 2;
    }
    if (!strcmp(argv[1], "add") || !strcmp(argv[1], "epk_add")) {
        if (argc < 3) {
            fprintf(stderr, "%s: install requires FILE.rxp\n", tool);
            return 2;
        }
        argv[1] = "install";
    }
    if (!strcmp(argv[1], "delete") || !strcmp(argv[1], "epk_delete")) {
        if (argc < 3) {
            fprintf(stderr, "%s: remove requires NAME\n", tool);
            return 2;
        }
        argv[1] = "remove";
    }
    if (!strcmp(argv[1], "build") && argc >= 4) {
        const char *out = NULL;
        int i;
        for (i = 2; i < argc; i++)
            if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        if (!out) {
            fprintf(stderr, "rx-pkg: build requires -o OUT.rxp\n");
            return 2;
        }
        return cmd_build(argv[2], out);
    }
    if (!strcmp(argv[1], "install") && argc >= 3) {
        const char *root = NULL;
        int i;
        for (i = 3; i < argc; i++)
            if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        return cmd_install(argv[2], root);
    }
    if (!strcmp(argv[1], "unpack") && argc >= 3) {
        const char *dest = NULL;
        int i;
        for (i = 3; i < argc; i++)
            if (!strcmp(argv[i], "-C") && i + 1 < argc) dest = argv[++i];
        return cmd_unpack(argv[2], dest);
    }
    if (!strcmp(argv[1], "list"))                 return cmd_list();
    if (!strcmp(argv[1], "remove") && argc >= 3)  return cmd_remove(argv[2]);
    if (!strcmp(argv[1], "files") && argc >= 3)   return cmd_files(argv[2]);
    if (!strcmp(argv[1], "info") && argc < 3) {
        printf("Name: epk\nFormat: RXP1\nMedia: local (%s)\n",
               getenv("EPK_MEDIA") ? getenv("EPK_MEDIA") : "/media/epk");
        return 0;
    }
    if (!strcmp(argv[1], "info") && argc >= 3)    return cmd_info(argv[2]);

    fprintf(stderr, "rx-pkg: unknown command '%s'\n", argc > 1 ? argv[1] : "");
    return 2;
}
