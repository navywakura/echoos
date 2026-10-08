#include "mono.h"
#include "pit.h"

static uint64_t g_tsc0;
static uint64_t g_tsc_per_us;
static bool     g_ok;
static char     g_source[48];

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void describe(uint64_t mhz) {
    char *d = g_source;
    const char *e = g_source + sizeof(g_source) - 1;
    const char *p = "TSC calibrated against the PIT, ";
    char num[24];
    int n = 0;
    while (*p && d < e) *d++ = *p++;
    if (!mhz) num[n++] = '0';
    while (mhz) { num[n++] = (char)('0' + (mhz % 10)); mhz /= 10; }
    while (n > 0 && d < e) *d++ = num[--n];
    p = " MHz";
    while (*p && d < e) *d++ = *p++;
    *d = '\0';
}

/* Count TSC over a fixed number of PIT ticks. Ten ticks is 100 ms: long
   enough that a single missed tick does not move the result by percent, short
   enough not to be felt at boot. */
/* The TSC counts something; under an emulator it is not wall time. Measured
   here: 26 s of PIT time against 397 s of TSC-derived time, a factor of
   fifteen, which turned a 3-second link timeout into a 200 ms one and made
   the link flap two dozen times while heartbeats were arriving on schedule.
 *
 * So the PIT is the authority for elapsed time and the TSC only interpolates
 * inside a tick. The ratio is re-estimated at every re-anchor, the result is
 * clamped so it can never go backwards, and the drift is counted rather than
 * hidden: a clock that had to be corrected is a fact worth printing.
 */
#define RESYNC_US 100000ull        /* 10 PIT ticks between re-anchors      */

static uint64_t g_anchor_tsc, g_anchor_us, g_last_us;
static uint32_t g_resyncs;
static uint32_t g_last_err_permille;

bool mono_init(void) {
    uint64_t t0, t1, start, ticks;

    g_ok = false;
    g_tsc_per_us = 0;
    g_resyncs = 0;
    g_last_err_permille = 0;
    start = pit_ticks();
    if (start == pit_ticks()) {
        uint64_t guard = rdtsc();
        while (pit_ticks() == start)
            if (rdtsc() - guard > 4000000000ull) {   /* PIT is not ticking */
                const char *p = "PIT only, 10 ms granularity";
                char *d = g_source;
                while (*p) *d++ = *p++;
                *d = '\0';
                g_tsc0 = rdtsc();
                return false;
            }
    }
    start = pit_ticks();
    t0 = rdtsc();
    while (pit_ticks() - start < 10) { __asm__ volatile ("pause"); }
    t1 = rdtsc();
    ticks = pit_ticks() - start;

    if (t1 > t0 && ticks) {
        g_tsc_per_us = (t1 - t0) / (ticks * 10000ull);
        if (!g_tsc_per_us) g_tsc_per_us = 1;
    }
    g_tsc0 = t1;
    g_anchor_tsc = t1;
    g_anchor_us = pit_uptime_ms() * 1000ull;
    g_last_us = g_anchor_us;
    g_ok = g_tsc_per_us > 1;
    describe(g_ok ? g_tsc_per_us : 0);
    return g_ok;
}

uint64_t mono_us(void) {
    uint64_t tsc, pit_us, est;

    if (!g_ok) return pit_uptime_ms() * 1000ull;
    tsc = rdtsc();
    pit_us = pit_uptime_ms() * 1000ull;

    if (pit_us >= g_anchor_us + RESYNC_US) {
        uint64_t d_us = pit_us - g_anchor_us;
        uint64_t d_tsc = tsc - g_anchor_tsc;
        uint64_t ratio = d_us ? d_tsc / d_us : g_tsc_per_us;
        uint64_t predicted = g_anchor_us + (g_tsc_per_us ? d_tsc / g_tsc_per_us : d_us);
        /* How wrong the old ratio was over this window, in per mille. */
        uint64_t err = predicted > pit_us ? predicted - pit_us : pit_us - predicted;
        g_last_err_permille = (uint32_t)(d_us ? (err * 1000ull) / d_us : 0);
        if (ratio) g_tsc_per_us = ratio;
        g_anchor_tsc = tsc;
        g_anchor_us = pit_us;
        g_resyncs++;
        est = pit_us;
    } else {
        est = g_anchor_us + (tsc - g_anchor_tsc) / g_tsc_per_us;
        /* Interpolation may not run past the next tick's worth of time. */
        if (est > pit_us + RESYNC_US) est = pit_us + RESYNC_US;
    }
    if (est < g_last_us) est = g_last_us;     /* monotonic, always          */
    g_last_us = est;
    return est;
}

uint32_t mono_resyncs(void) { return g_resyncs; }
uint32_t mono_drift_permille(void) { return g_last_err_permille; }

const char *mono_source(void) { return g_source[0] ? g_source : "uncalibrated"; }

uint32_t mono_resolution_ns(void) {
    if (!g_ok) return 10000000u;              /* one PIT tick               */
    /* One TSC count, in nanoseconds, rounded up: at 2 GHz that is 1 ns, and
       saying "1 ns" is only honest because the counter really does step. */
    return (uint32_t)(1000u / (g_tsc_per_us ? g_tsc_per_us : 1)) + 1u;
}
