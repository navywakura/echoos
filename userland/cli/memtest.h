#ifndef ECHOS_MEMTEST_H
#define ECHOS_MEMTEST_H

#include "rogex.h"

/* Destructive-on-unused-frames RAM diagnostic. Does not return on success
   until the operator hits a key; then the caller typically reboots. */
void memtest_run(void);

#endif
