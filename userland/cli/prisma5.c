/*
 * PRISMA 5 SNN — synthetic EEG -> spike trains on the Q6 LIF cube.
 * All buffers are BSS (Heap-0). No malloc.
 */
#include "prisma5.h"
#include "arch.h"
#include "snn_q6.h"
#include "arch.h"

/* Quarter-wave of sin(pi/2 * i/63) * 127, integer. */
static const int8_t SIN64[64] = {
      0,   3,   6,   9,  13,  16,  19,  22,  25,  28,  31,  34,  37,  40,  43,  46,
     49,  52,  54,  57,  60,  62,  65,  67,  70,  72,  74,  77,  79,  81,  83,  85,
     87,  89,  91,  92,  94,  96,  97,  98, 100, 101, 102, 103, 104, 105, 106, 106,
    107, 107, 108, 108, 108, 108, 108, 108, 108, 107, 107, 106, 106, 105, 104, 103
};

static snn_q6_t g_net;                 /* Heap-0 BSS, never on the stack */
static int16_t   g_eeg[PRISMA5_WIN];

static int16_t isin(unsigned phase) {
    unsigned q = (phase / 64) & 3;
    unsigned i = phase & 63;
    int16_t v = SIN64[q & 1 ? 63 - i : i];
    return (q == 1 || q == 2) ? (int16_t)(-v) : v;
}

static uint32_t lcg(uint32_t *s) {
    *s = (*s * 1664525u) + 1013904223u;
    return *s;
}

/* The CPU's own counter, whatever it is on this target. Used for a
   benchmark ratio, never as wall time. */
static uint64_t cycles(void) { return arch_cycles(); }

static uint64_t tsc_per_us(void) {
    static uint64_t cached;
    uint64_t t0, t1, start;
    if (cached) return cached;
    start = arch_timer_ticks();
    while (arch_timer_ticks() == start) { }
    t0 = cycles();
    start = arch_timer_ticks();
    while (arch_timer_ticks() - start < 10) { }   /* 100 ms at 100 Hz */
    t1 = cycles();
    cached = (t1 - t0) / 100000ull;        /* ticks / us */
    if (!cached) cached = 1;
    return cached;
}

void prisma5_synth(prisma5_kind k, int16_t *out, uint32_t n) {
    uint32_t i, s = 0xA5A5u;
    unsigned step = 4; /* alpha ~10 Hz at 256 Hz fs, 64-sample circle */
    if (k == PRISMA5_BETA)  step = 8;
    if (k == PRISMA5_GAMMA) step = 16;
    for (i = 0; i < n; i++) {
        if (k == PRISMA5_NULL)       out[i] = 0;
        else if (k == PRISMA5_CONST) out[i] = 12;          /* below SNN_Q6_VTH current */
        else if (k == PRISMA5_NOISE) out[i] = (int16_t)((lcg(&s) % 255) - 127);
        else                         out[i] = (int16_t)(isin((i * step) & 255) * 1);
    }
}

void prisma5_run(prisma5_kind k, prisma5_run_t *out) {
    uint32_t i, spikes = 0, t;
    uint64_t tsc0, tsc1, us, lat_acc = 0;
    uint64_t per_us = tsc_per_us();

    if (!out) return;
    prisma5_synth(k, g_eeg, PRISMA5_WIN);
    snn_q6_init_cube(&g_net);

    for (t = 0; t < PRISMA5_TICKS; t++) {
        unsigned stim = 0;
        int stim_on = 0;
        int16_t sample = g_eeg[(t * (PRISMA5_WIN / PRISMA5_TICKS)) & (PRISMA5_WIN - 1)];
        if (sample > 40 || sample < -40) {
            stim_on = 1;
            stim = (unsigned)((sample < 0 ? -sample : sample) & (SNN_Q6_N - 1));
        }
        tsc0 = cycles();
        snn_q6_step(&g_net, stim, stim_on, 0);
        tsc1 = cycles();
        spikes += (uint32_t)snn_popcount64(g_net.spikes);
        lat_acc += (tsc1 - tsc0);
    }
    us = (lat_acc / PRISMA5_TICKS) / (per_us ? per_us : 1);
    out->kind = k;
    out->spikes = spikes;
    out->slots = (uint32_t)SNN_Q6_N * PRISMA5_TICKS;
    {
        uint32_t silent = out->slots - spikes;
        out->sparsity_x10 = (silent * 1000u) / out->slots;
    }
    out->latency_ns = (uint32_t)(us * 1000ull);
    (void)i;
}

void prisma5_stress(uint32_t bursts, char *out, size_t outmax) {
    uint32_t b, hung = 0;
    prisma5_run_t r;
    char *d = out;
    size_t n = 0;
    if (bursts > 256) bursts = 256;
    if (!bursts) bursts = 64;
    for (b = 0; b < bursts; b++) {
        prisma5_run((prisma5_kind)(b % PRISMA5_KIND_COUNT), &r);
        hung += r.spikes; /* touch the result so the compiler keeps the loop */
    }
    if (!out || !outmax) return;
    {
        const char *s = "[STRESS] EEG bursts=";
        while (*s && n + 1 < outmax) out[n++] = *s++;
    }
    {
        char t[12]; int i = 0; uint32_t v = bursts;
        if (!v) t[i++] = '0';
        while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
        while (i && n + 1 < outmax) out[n++] = t[--i];
    }
    {
        const char *s = "  completed  (no deadlock)  spikes_seen=";
        while (*s && n + 1 < outmax) out[n++] = *s++;
    }
    {
        char t[12]; int i = 0; uint32_t v = hung;
        if (!v) t[i++] = '0';
        while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
        while (i && n + 1 < outmax) out[n++] = t[--i];
    }
    if (n + 1 < outmax) out[n++] = '\n';
    out[n] = '\0';
    (void)d;
}
