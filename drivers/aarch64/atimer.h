/* ARM Generic Timer: the architectural counter every AArch64 core has. */
#ifndef ECHOS_ATIMER_H
#define ECHOS_ATIMER_H
#include "rogex.h"

#define ATIMER_HZ 100                  /* the tick rate we programme        */

bool     atimer_init(void);
uint64_t atimer_ticks(void);
uint64_t atimer_uptime_ms(void);
uint64_t atimer_freq(void);            /* CNTFRQ_EL0, counts per second     */
void     atimer_irq(void);             /* called from the IRQ dispatcher    */
#endif
