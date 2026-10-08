/* CPU helpers (halt, interrupts, etc.). */
#ifndef RXOS_CPU_H
#define RXOS_CPU_H

#include "rogex.h"

static inline uint64_t cpu_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpu_pause(void)   { __asm__ volatile ("pause"); }
static inline void cpu_halt(void)    { __asm__ volatile ("hlt"); }
static inline void cpu_cli(void)     { __asm__ volatile ("cli"); }
static inline void cpu_sti(void)     { __asm__ volatile ("sti"); }

#endif /* RXOS_CPU_H */
