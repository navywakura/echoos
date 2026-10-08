/* The evidence block §7 of the Astra brief asks for, emitted by the board
   that measured it. See kernel/report.c for what may and may not appear. */
#ifndef ECHOS_REPORT_H
#define ECHOS_REPORT_H

#include "rogex.h"

/* Runs the robotic scenario and prints one machine-readable block on the
   console. Safe to call more than once; counters accumulate, which is why
   the parser reads the last block. */
void report_emit(void);

#endif /* ECHOS_REPORT_H */
