/* The live monitor pane. See pane_monitor.c for what it is and is not. */
#ifndef ECHOS_PANE_MONITOR_H
#define ECHOS_PANE_MONITOR_H

void pane_monitor_refresh(void);   /* draw it now                          */
void pane_monitor_tick(void);      /* from the idle hook; rate-limited     */

/* How many times it has actually redrawn. The proof that a pane keeps
   living while the shell is used is this number moving on its own. */
#include "rogex.h"
uint32_t pane_monitor_refreshes(void);

#endif
