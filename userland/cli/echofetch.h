#ifndef ECHOS_ECHOFETCH_H
#define ECHOS_ECHOFETCH_H

#include "rogex.h"

/* Fill `out` with the coloured (ANSI) echofetch report. */
void echofetch_render(char *out, size_t outmax);

/* Paint the official logo + specs through the TUI (LIVE banner). */
void echofetch_banner(void);

#endif
