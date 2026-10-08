#ifndef ECHOS_LIVE_H
#define ECHOS_LIVE_H

#include "rogex.h"

/* Print the LIVE landing card: tree, packages, commands, disks. */
void live_enter(void);
void live_status(char *out, size_t outmax);

#endif
