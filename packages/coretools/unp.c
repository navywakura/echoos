/* unp — intelligent unpacker for EchOS.
 * Inspects magic bytes (not extension) to detect:
 *   - POSIX tar (own reader, incl. ustar prefix + GNU longname 'L')
 *   - gzip (detection only; decompression is out of scope by design)
 *   - zip (own reader; stored/method-0 entries extracted)
 *   - EchOS .rxp packages (see SPEC.md)
 * Without -x prints the detected type and the action it would take;
 * with -x performs the extraction into the current directory (or -C DIR).
 * EchOS coretools — proprietary, from scratch. C99.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define TAR_BLOCK 512

static int opt_extract = 0;
static const char *opt_dir = NULL;
static int g_status = 0;

static void mkdir_all(char *path)
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

static void ensure_parents(const char *filepath)
{
    char tmp[4096];
    size_t i, len;
    char *slash;
    len = strlen(filepath);
    if (len >= sizeof tmp) return;
    memcpy(tmp, filepath, len + 1);
    slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return;
    *slash = '\0';
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

static int safe_path(const char *p)
{
    if (!p || !*p) return 0;
    if (p[0] == '/') return 0;
    if (strstr(p, "..") == p) return 0;
    if (strstr(p, "/..")) return 0;
    return 1;
}

static FILE *open_member(const char *dir, const char *name, unsigned mode,
                         int is_dir)
{
    char full[4096];
    (void)dir;
    if (!safe_path(name)) {
        fprintf(stderr, "unp: refusing unsafe member '%s'\n", name);
        return NULL;
    }
    if (opt_dir)
        snprintf(full, sizeof full, "%s/%s", opt_dir, name);
    else
        snprintf(full, sizeof full, "%s", name);
    ensure_parents(full); /* create parent dirs only */
    if (is_dir) {
        mkdir(full, mode & 0777 ? mode & 0777 : 0755);
        return NULL;
    }
    return fopen(full, "wb");
}

static long long parse_octal(const char *field, size_t n)
{
    long long v = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        char c = field[i];
        if (c == '\0' || c == ' ') {
            if (v == 0) continue; else break;
        }
        if (c < '0' || c > '7') break;
        v = v * 8 + (c - '0');
    }
    return v;
}

static int is_dir_name(const char *n)
{
    size_t l = strlen(n);
    return l > 0 && n[l - 1] == '/';
}

/* ---------------- tar ---------------- */

static int extract_tar(FILE *f)
{
    unsigned char hdr[TAR_BLOCK];
    char pending_longname[4096];
    pending_longname[0] = '\0';

    for (;;) {
        size_t got = fread(hdr, 1, TAR_BLOCK, f);
        long long size;
        unsigned type;
        char name[512], prefix[156];

        if (got == 0) break;
        if (got < TAR_BLOCK) { fprintf(stderr, "unp: truncated tar\n"); return -1; }
        {
            int allzero = 1;
            size_t i;
            for (i = 0; i < TAR_BLOCK; i++)
                if (hdr[i]) { allzero = 0; break; }
            if (allzero) continue; /* end-of-archive or padding */
        }
        memcpy(name, hdr, 100);      name[100] = '\0';
        memcpy(prefix, hdr + 345, 155); prefix[155] = '\0';
        size = parse_octal((const char *)hdr + 124, 12);
        type = hdr[156];

        if (type == 'L') { /* GNU long name */
            FILE *lf;
            size_t n = 0;
            char full[4096];
            const char *base = opt_dir ? opt_dir : "";
            snprintf(full, sizeof full, "%s%s.unp-longname.tmp",
                     base, (*base && base[strlen(base)-1] != '/') ? "/" : "");
            lf = fopen(full, "wb");
            if (!lf) return -1;
            {
                char buf[4096];
                long long left = size;
                while (left > 0) {
                    size_t chunk = left > (long long)sizeof buf
                                       ? sizeof buf : (size_t)left;
                    if (fread(buf, 1, chunk, f) != chunk) { fclose(lf); return -1; }
                    fwrite(buf, 1, chunk, lf);
                    left -= (long long)chunk;
                }
                /* consume block padding */
                {
                    long long padded = ((size + TAR_BLOCK - 1) / TAR_BLOCK)
                                           * TAR_BLOCK;
                    left = padded - size;
                    while (left > 0) {
                        size_t chunk = left > (long long)sizeof buf
                                           ? sizeof buf : (size_t)left;
                        if (fread(buf, 1, chunk, f) != chunk) {
                            fclose(lf);
                            return -1;
                        }
                        left -= (long long)chunk;
                    }
                }
            }
            fclose(lf);
            lf = fopen(full, "rb");
            if (lf) {
                n = fread(pending_longname, 1, sizeof pending_longname - 1, lf);
                pending_longname[n] = '\0';
                while (n > 0 && pending_longname[n-1] == '\n')
                    pending_longname[--n] = '\0';
                fclose(lf);
            }
            remove(full);
            continue;
        }
        if (pending_longname[0]) {
            size_t ln = strlen(pending_longname);
            if (ln >= sizeof name) ln = sizeof name - 1;
            memcpy(name, pending_longname, ln);
            name[ln] = '\0';
            pending_longname[0] = '\0';
        } else if (prefix[0]) {
            char joined[1024];
            snprintf(joined, sizeof joined, "%s/%s", prefix, name);
            memcpy(name, joined, strlen(joined) < sizeof name
                       ? strlen(joined) + 1 : sizeof name - 1);
            name[sizeof name - 1] = '\0';
        }

        if (type == '5' || (type == '0' && name[0] &&
                            name[strlen(name)-1] == '/')) {
            /* directory */
            char clean[256];
            snprintf(clean, sizeof clean, "%s", name);
            if (clean[0] && clean[strlen(clean)-1] == '/')
                clean[strlen(clean)-1] = '\0';
            if (opt_extract) open_member(opt_dir, clean, 0755, 1);
            printf("  d %s\n", name);
            continue;
        }
        if (type == '0' || type == '\0' || type == '7') {
            FILE *out = NULL;
            char buf[8192];
            long long left = size;
            long long padded = ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
            if (opt_extract) {
                if (!is_dir_name(name)) {
                    out = open_member(opt_dir, name, 0644, 0);
                    if (!out && size > 0) {
                        fprintf(stderr, "unp: cannot write %s\n", name);
                        g_status = 1;
                    }
                }
            }
            printf("  f %s (%lld bytes)\n", name, size);
            while (left > 0) {
                size_t chunk = left > (long long)sizeof buf
                                   ? sizeof buf : (size_t)left;
                if (fread(buf, 1, chunk, f) != chunk) {
                    fprintf(stderr, "unp: truncated file data in tar\n");
                    if (out) fclose(out);
                    return -1;
                }
                if (out) fwrite(buf, 1, chunk, out);
                left -= (long long)chunk;
            }
            /* consume block padding so the next header stays aligned */
            left = padded - size;
            while (left > 0) {
                size_t chunk = left > (long long)sizeof buf
                                   ? sizeof buf : (size_t)left;
                if (fread(buf, 1, chunk, f) != chunk) {
                    fprintf(stderr, "unp: truncated padding in tar\n");
                    if (out) fclose(out);
                    return -1;
                }
                left -= (long long)chunk;
            }
            if (out) fclose(out);
            continue;
        }
        /* link / special: skip payload */
        {
            long long left = ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
            char buf[512];
            while (left > 0) {
                size_t chunk = left > (long long)sizeof buf
                                   ? sizeof buf : (size_t)left;
                if (fread(buf, 1, chunk, f) != chunk) return -1;
                left -= (long long)chunk;
            }
            printf("  s %s (special, skipped)\n", name);
        }
    }
    return 0;
}

/* ---------------- zip (stored entries only) ---------------- */

static unsigned rd16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static unsigned rd32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static int extract_zip(FILE *f)
{
    /* Walk local file headers sequentially. */
    for (;;) {
        unsigned char lh[30];
        unsigned method, namelen, extralen, csize;
        char name[1024];
        size_t got = fread(lh, 1, 30, f);
        long sig;

        if (got == 0) break;
        if (got < 30 || rd32(lh) != 0x04034b50u) {
            sig = (got >= 4) ? (long)rd32(lh) : -1;
            if (sig == 0x02014b50 || sig == 0x06054b50 || sig == 0x08064b50)
                break; /* central directory / end reached */
            fprintf(stderr, "unp: malformed zip local header\n");
            return -1;
        }
        method  = rd16(lh + 8);
        csize   = rd32(lh + 18);
        namelen = rd16(lh + 26);
        extralen= rd16(lh + 28);
        if (namelen >= sizeof name) { fprintf(stderr, "unp: name too long\n"); return -1; }
        if (fread(name, 1, namelen, f) != namelen) return -1;
        name[namelen] = '\0';
        if (fseek(f, (long)extralen, SEEK_CUR) != 0) return -1;

        if (method != 0) {
            char buf[4096];
            unsigned left = csize;
            printf("  z %s (compressed entry, skipped)\n", name);
            while (left) {
                size_t chunk = left > sizeof buf ? sizeof buf : left;
                if (fread(buf, 1, chunk, f) != chunk) return -1;
                left -= (unsigned)chunk;
            }
            continue;
        }
        {
            FILE *out = NULL;
            char buf[8192];
            unsigned left = csize;
            int isdir = (name[0] && name[strlen(name)-1] == '/');
            char clean[1024];
            snprintf(clean, sizeof clean, "%s", name);
            if (isdir && clean[0]) clean[strlen(clean)-1] = '\0';
            if (opt_extract) {
                if (isdir) open_member(opt_dir, clean, 0755, 1);
                else {
                    out = open_member(opt_dir, clean, 0644, 0);
                    if (!out) { fprintf(stderr, "unp: cannot write %s\n", clean); g_status = 1; }
                }
            }
            printf("  %c %s (%u bytes)\n", isdir ? 'd' : 'f', name, csize);
            while (left) {
                size_t chunk = left > sizeof buf ? sizeof buf : left;
                if (fread(buf, 1, chunk, f) != chunk) {
                    if (out) fclose(out);
                    return -1;
                }
                if (out) fwrite(buf, 1, chunk, out);
                left -= (unsigned)chunk;
            }
            if (out) fclose(out);
        }
    }
    return 0;
}

/* ---------------- rxp (EchOS package) ---------------- */

static unsigned le32(const unsigned char *p)
{ return rd32(p); }

static int extract_rxp(FILE *f)
{
    unsigned char hdr[16];
    unsigned version, metalen, count, i;

    if (fread(hdr, 1, 16, f) != 16) return -1;
    version = le32(hdr + 4);
    metalen = le32(hdr + 8);
    count   = le32(hdr + 12);
    if (version != 1) { fprintf(stderr, "unp: rxp version %u unsupported\n", version); return -1; }
    if (metalen) fseek(f, (long)metalen, SEEK_CUR);
    for (i = 0; i < count; i++) {
        unsigned char pl[2], mh[12];
        unsigned plen, mode;
        unsigned long long size;
        char name[4096];
        if (fread(pl, 1, 2, f) != 2) return -1;
        plen = rd16(pl);
        if (plen >= sizeof name) return -1;
        if (fread(name, 1, plen, f) != plen) return -1;
        name[plen] = '\0';
        if (fread(mh, 1, 12, f) != 12) return -1;
        mode = le32(mh);
        size = (unsigned long long)le32(mh + 4) |
               ((unsigned long long)le32(mh + 8) << 32);
        {
            int isdir = (plen > 0 && name[plen-1] == '/');
            FILE *out = NULL;
            char clean[4096], buf[8192];
            snprintf(clean, sizeof clean, "%s", name);
            if (isdir && clean[0]) clean[strlen(clean)-1] = '\0';
            if (opt_extract) {
                if (isdir) open_member(opt_dir, clean,
                                       mode & 0777 ? mode & 0777 : 0755, 1);
                else {
                    out = open_member(opt_dir, clean, mode & 0777 ? mode & 0777 : 0644, 0);
                    if (!out) { fprintf(stderr, "unp: cannot write %s\n", clean); g_status = 1; }
                }
            }
            printf("  %c %s (%llu bytes)\n", isdir ? 'd' : 'f', name, size);
            while (size) {
                size_t chunk = size > sizeof buf ? sizeof buf : (size_t)size;
                if (fread(buf, 1, chunk, f) != chunk) return -1;
                if (out) fwrite(buf, 1, chunk, out);
                size -= chunk;
            }
            if (out) fclose(out);
        }
    }
    return 0;
}

/* ---------------- detection ---------------- */

enum kind { K_UNKNOWN, K_TAR, K_GZ, K_ZIP, K_RXP };

static enum kind detect(FILE *f, const char **desc)
{
    unsigned char b[262];
    size_t got = fread(b, 1, sizeof b, f);
    rewind(f);

    if (got >= 262 && memcmp(b + 257, "ustar", 5) == 0) {
        *desc = "tar archive (ustar)";
        return K_TAR;
    }
    /* checksum heuristic for old-style tars without magic */
    if (got >= 512) {
        unsigned sum = 0, stored;
        size_t i;
        for (i = 0; i < 512; i++)
            sum += (i >= 148 && i < 156) ? 32 : b[i];
        stored = (unsigned)parse_octal((const char *)b + 148, 8);
        if (stored > 0 && sum == stored) {
            *desc = "tar archive (pre-POSIX, checksum match)";
            return K_TAR;
        }
    }
    if (got >= 2 && b[0] == 0x1f && b[1] == 0x8b) {
        *desc = "gzip compressed data";
        return K_GZ;
    }
    if (got >= 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 0x03 && b[3] == 0x04) {
        *desc = "zip archive";
        return K_ZIP;
    }
    if (got >= 4 && memcmp(b, "RXP1", 4) == 0) {
        *desc = "echos package (.rxp)";
        return K_RXP;
    }
    *desc = "unknown format";
    return K_UNKNOWN;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: unp [-x] [-C DIR] FILE\n"
        "  inspect magic bytes and print what would be done,\n"
        "  or perform it with -x (into current dir or -C DIR)\n"
        "recognized: tar, gzip (detect only), zip (stored), .rxp\n");
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int i;
    FILE *f;
    const char *desc = NULL;
    enum kind k;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-x") == 0) opt_extract = 1;
        else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) opt_dir = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1] != '\0') { usage(); return 2; }
        else if (!path) path = argv[i];
        else { usage(); return 2; }
    }
    if (!path) { usage(); return 2; }
    if (opt_dir && opt_extract) {
        char tmp[4096];
        snprintf(tmp, sizeof tmp, "%s", opt_dir);
        mkdir_all(tmp);
    }

    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "unp: cannot open %s\n", path); return 1; }
    k = detect(f, &desc);
    printf("%s: %s\n", path, desc);

    switch (k) {
    case K_TAR:
        printf(opt_extract ? "extracting tar...\n" : "(run with -x to extract)\n");
        if (opt_extract && extract_tar(f) != 0) { fprintf(stderr, "unp: tar extraction failed\n"); g_status = 1; }
        break;
    case K_ZIP:
        printf(opt_extract ? "extracting zip (stored entries)...\n" : "(run with -x to extract stored entries)\n");
        if (opt_extract && extract_zip(f) != 0) { fprintf(stderr, "unp: zip extraction failed\n"); g_status = 1; }
        break;
    case K_RXP:
        printf(opt_extract ? "unpacking echos package...\n" : "(run with -x to unpack)\n");
        if (opt_extract && extract_rxp(f) != 0) { fprintf(stderr, "unp: rxp unpack failed\n"); g_status = 1; }
        break;
    case K_GZ:
        printf("action: gunzip required (decompressor not built in; pipe through an external tool)\n");
        break;
    default:
        printf("action: none\n");
        g_status = 1;
    }
    fclose(f);
    return g_status;
}
