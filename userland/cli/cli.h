#ifndef ECHOS_CLI_H
#define ECHOS_CLI_H

#include "rogex.h"

/* Parse GRUB cmdline, run memtest/recovery/LIVE wizard, seed epk. */
void echos2_boot(void);

/* Skip the graphical desktop (2.0 Universal, LIVE, recovery). */
bool echos2_cli_only(void);

/* Recovery banner already printed. */
bool echos2_recovery(void);

#endif
