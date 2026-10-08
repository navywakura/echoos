/* EchOS package manager — Portage-inspired .rxc app/command installs.
   Channel: https://www.rogexlaboratories.com/rx-os/packages */
#ifndef RXOS_PKG_H
#define RXOS_PKG_H

#include "rogex.h"

/* Verified package mirror root (no TCP stack yet: local verified index +
   future HTTP client against this host). */
#define PKG_CHANNEL_HOST "www.rogexlaboratories.com"
#define PKG_CHANNEL_PATH "/packages"
#define PKG_CHANNEL_URL  "https://www.rogexlaboratories.com/packages"

/* Dispatch: rx app <sub> [args]. Writes human text into out. */
void pkg_cmd_rx(const char *sub, const char *arg, const char *rest,
                char *out, size_t outmax);

/* Boot: seed package DB dirs and the offline verified index. */
void pkg_init(void);

/* Run an installed package as a terminal command (`fortune`, `echo hi`).
   Looks in /bin/<name>.rxc and <name>.rxc. Returns true if it handled `name`. */
bool pkg_try_command(const char *name, const char *args, char *out, size_t outmax);

#endif /* RXOS_PKG_H */
