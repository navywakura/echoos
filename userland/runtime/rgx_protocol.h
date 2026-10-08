/* rgx:// protocol helpers. */
#ifndef RXOS_RGX_PROTOCOL_H
#define RXOS_RGX_PROTOCOL_H

#include "rogex.h"

typedef enum {
    RGX_ROUTE_APP,        /* an RXFS .rxc file — run it with Roxenite       */
    RGX_ROUTE_OTHER,      /* not an RXFS app; caller tries sites/built-ins  */
    RGX_ROUTE_BAD_URL,    /* not an rgx:// URL at all                       */
} rgx_route_t;

/* If `url` is rgx://<name> and <name>.rxc (or <name>) exists in the VFS,
   returns RGX_ROUTE_APP with the filename in file_out. */
rgx_route_t rgx_resolve(const char *url, char *file_out, size_t max);

/* True if the string starts with "rgx://". */
bool rgx_is_url(const char *s);

#endif /* RXOS_RGX_PROTOCOL_H */
