#ifndef ECHOS_NETCLI_H
#define ECHOS_NETCLI_H

#include "rogex.h"

/* curl / wdl / wget / ipconf. a1 is first token after the verb; rest is the
   remainder of the line (may be empty). */
void netcli_curl(const char *a1, const char *rest);
void netcli_wdl(const char *a1, const char *rest);
void netcli_wget(const char *a1, const char *rest);
void netcli_ipconf(void);

#endif
