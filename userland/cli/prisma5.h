#ifndef ECHOS_PRISMA5_H
#define ECHOS_PRISMA5_H

#include "rogex.h"

#define PRISMA5_WIN     256
#define PRISMA5_TICKS   16

typedef enum {
    PRISMA5_ALPHA = 0,   /* ~10 Hz */
    PRISMA5_BETA,        /* ~20 Hz */
    PRISMA5_GAMMA,       /* ~40 Hz */
    PRISMA5_NOISE,
    PRISMA5_NULL,        /* zeros — activity must fall to 0 */
    PRISMA5_CONST,       /* constant DC, below threshold */
    PRISMA5_KIND_COUNT
} prisma5_kind;

typedef struct {
    prisma5_kind kind;
    uint32_t     spikes;
    uint32_t     slots;        /* neurons * ticks */
    uint32_t     sparsity_x10; /* 0.1 % units: 1000 = 100.0 % silent */
    uint32_t     latency_ns;   /* average step, nanoseconds (TSC) */
} prisma5_run_t;

void prisma5_synth(prisma5_kind k, int16_t *out, uint32_t n);
void prisma5_run(prisma5_kind k, prisma5_run_t *out);
void prisma5_stress(uint32_t bursts, char *out, size_t outmax);

#endif
