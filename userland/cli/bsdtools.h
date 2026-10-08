#ifndef ECHOS_BSDTOOLS_H
#define ECHOS_BSDTOOLS_H

#include "rogex.h"

void bsdtools_init(void);

/* True if `name` is an OpenBSD-style tool we handle. Fills out. */
bool bsdtools_cmd(const char *name, const char *a1, const char *rest,
                  char *out, size_t outmax);

/* doas policy: permit if /etc/doas.conf contains a matching permit line. */
bool doas_permit(const char *user, const char *cmd);

#endif
