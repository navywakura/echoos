/* rgxsite packages. */
#ifndef ROGEX_RGXSITE_H
#define ROGEX_RGXSITE_H

#include "rogex.h"

typedef enum {
    RGXSITE_OK = 0,
    RGXSITE_BAD_MAGIC,
    RGXSITE_BAD_VERSION,
    RGXSITE_TRUNCATED,
    RGXSITE_INTEGRITY_FAIL,   /* an asset's SHA3-256 did not match         */
    RGXSITE_SITEID_FAIL,      /* content-address (site_id) did not match    */
    RGXSITE_NO_ENTRY,
} rgxsite_status;

const char *rgxsite_status_str(rgxsite_status s);

/* Verify + render a package in place. Does NOT trust anything until every
   asset hash and the site_id check out. */
rgxsite_status rgxsite_open_and_render(const uint8_t *pkg, uint32_t len);

/* If `url` is served by a built-in embedded site (matched by origin), verify +
   render it and return true; otherwise false (caller falls back to built-ins). */
bool rgxsite_try(const char *url);

/* Boot self-test: embedded site verifies, and a 1-byte tamper is rejected. */
bool rgxsite_selftest(void);

#endif /* ROGEX_RGXSITE_H */
