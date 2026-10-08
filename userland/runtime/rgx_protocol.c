/* rgx:// routing. Local only — no DHT/network fetch yet. */
#include "rgx_protocol.h"
#include "vfs.h"

static char *apps(char *d, const char *s, char *e) {
    while (*s && d < e - 1) *d++ = *s++;
    *d = '\0';
    return d;
}
static void cpn(char *d, const char *s, size_t max) {
    size_t i = 0;
    for (; s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = '\0';
}

bool rgx_is_url(const char *s) {
    const char *pfx = "rgx://";
    for (int i = 0; pfx[i]; i++) if (s[i] != pfx[i]) return false;
    return true;
}

rgx_route_t rgx_resolve(const char *url, char *file_out, size_t max) {
    if (!rgx_is_url(url)) return RGX_ROUTE_BAD_URL;
    const char *name = url + 6;
    if (!name[0]) return RGX_ROUTE_OTHER;

    /* Search order (apps can live in the cwd, vault root, or /bin after
       `rx app add`): relative, /bin/<name>.rxc, /<name>.rxc, bare name. */
    char cand[RXFS_NAME];
    char *d, *e;

    d = cand; e = cand + sizeof(cand);
    d = apps(d, name, e); apps(d, ".rxc", e);
    if (vfs_size(cand) >= 0) { cpn(file_out, cand, max); return RGX_ROUTE_APP; }

    d = cand; e = cand + sizeof(cand);
    d = apps(d, "/bin/", e); d = apps(d, name, e); apps(d, ".rxc", e);
    if (vfs_size(cand) >= 0) { cpn(file_out, cand, max); return RGX_ROUTE_APP; }

    d = cand; e = cand + sizeof(cand);
    d = apps(d, "/", e); d = apps(d, name, e); apps(d, ".rxc", e);
    if (vfs_size(cand) >= 0) { cpn(file_out, cand, max); return RGX_ROUTE_APP; }

    if (vfs_size(name) >= 0) { cpn(file_out, name, max); return RGX_ROUTE_APP; }
    return RGX_ROUTE_OTHER;
}
