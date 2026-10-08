/* Programmable interval timer. */
#ifndef RXOS_PIT_H
#define RXOS_PIT_H

#include "rogex.h"

#define PIT_HZ 100                 /* tick rate we program (10 ms per tick) */

/* Program channel 0 and register the IRQ0 handler (irq.c must be installed
   first). Returns true if ticks were observed advancing afterwards. */
bool pit_init(void);

uint64_t pit_ticks(void);          /* raw tick count since boot            */
uint64_t pit_uptime_ms(void);      /* milliseconds since pit_init          */

#endif /* RXOS_PIT_H */
