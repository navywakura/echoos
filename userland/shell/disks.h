#ifndef RXOS_DISKS_H
#define RXOS_DISKS_H

#include "rogex.h"

void disks_cmd(const char *dev, const char *op, const char *arg,
               char *out, size_t outmax);

/* One-line summary for the GUI disk manager (index 0..n-1). */
int         disks_gui_count(void);
const char *disks_gui_line(int i);   /* static buffer, overwritten each call */

#endif
