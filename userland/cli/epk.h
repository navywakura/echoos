/*
 * epk — echOS Package. Local ISO catalog plus the public channel
 * https://www.rogexlaboratories.com/packages  (`epk get` needs `www on`).
 *
 * Commands (OpenBSD-inspired names accepted as aliases):
 *   epk install <pkg>     /  epk_add
 *   epk remove  <pkg>     /  epk_delete
 *   epk info [pkg]
 *   epk list
 *   epk apply <edition>   install every package in <edition>.manifest
 */
#ifndef ECHOS_EPK_H
#define ECHOS_EPK_H

#include "rogex.h"

#define EPK_MEDIA_DIR      "/epk"
#define EPK_DB_DIR         "/epk/db"
#define EPK_MANIFEST_DIR   "/epk/manifests"
#define EPK_BIN_DIR        "/bin"

typedef enum {
    EPK_ED_MINIMAL = 0,
    EPK_ED_COMPLETE,
    EPK_ED_EDGE,
    EPK_ED_SERVER,
    EPK_ED_COUNT
} epk_edition_t;

void epk_init(void);
void epk_cmd(const char *sub, const char *arg, const char *rest,
             char *out, size_t outmax);

/* Apply an edition manifest from the local ISO catalog. */
bool epk_apply_edition(epk_edition_t ed, char *err, uint32_t errmax);
const char *epk_edition_name(epk_edition_t ed);
epk_edition_t epk_edition_from_name(const char *s);
const char *epk_runtime_edition(void);
bool        epk_is_installed(const char *name);
const char *epk_pkg_for_command(const char *cmd);
void        epk_live_all(void);   /* LIVE: public catalog only (not --lab notes) */

/* If `cmd` maps to an installed (or LIVE-world) package, print its stub
   banner into `out` and return true. Unknown / not-installed → false. */
bool        epk_run_command(const char *cmd, const char *a1, const char *rest,
                            char *out, size_t outmax);

#endif
