/* GICv2 distributor + CPU interface, and the exception vectors that feed it. */
#ifndef ECHOS_GIC_H
#define ECHOS_GIC_H
#include "rogex.h"

bool gic_init(void);
bool gic_register(int intid, void (*handler)(void));
void gic_enable(int intid);
void gic_disable(int intid);
uint32_t gic_spurious(void);      /* interrupts with nobody to take them   */
uint64_t gic_taken(void);         /* how many we actually dispatched        */
#endif
