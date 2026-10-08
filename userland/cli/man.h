#ifndef ECHOS_MAN_H
#define ECHOS_MAN_H

#include "rogex.h"

/* `man` / `manual` — per-command pages (syntax, flags, examples).
   topic may be NULL / empty (index), a command name, or "-k" / "--apropos". */
void man_cmd(const char *topic, const char *rest);

#endif
