#ifndef ECHOS_NANO_H
#define ECHOS_NANO_H

#include "rogex.h"

/* Minimal TUI editor. path may be empty (untitled). Esc exits, Ctrl+W saves. */
void nano_run(const char *path);

#endif
