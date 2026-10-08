/* Monotonic microseconds from the ARM generic timer.
 *
 * CNTVCT_EL0 is architecturally a fixed-frequency counter and CNTFRQ_EL0
 * states that frequency, so there is nothing to calibrate and nothing to
 * discipline. The x86 side needed a watchdog against the PIT because its TSC
 * ran fifteen times fast under emulation; this counter cannot, by
 * definition, and saying so is the difference between the two files.
 */
#include "mono.h"

static uint64_t g_freq, g_start;
static bool     g_ok;
static char     g_source[48];

static inline uint64_t cntvct(void) {
    uint64_t v;
    __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

bool mono_init(void) {
    char *d = g_source;
    const char *p;
    char num[24];
    int n = 0;
    uint64_t mhz;

    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(g_freq));
    g_ok = g_freq >= 1000000ull;          /* below 1 MHz it cannot give us us */
    g_start = cntvct();

    p = g_ok ? "ARM generic timer, " : "generic timer unusable, ";
    while (*p) *d++ = *p++;
    mhz = g_freq / 1000000ull;
    if (!mhz) num[n++] = '0';
    while (mhz) { num[n++] = (char)('0' + (mhz % 10)); mhz /= 10; }
    while (n > 0) *d++ = num[--n];
    p = " MHz, fixed rate";
    while (*p) *d++ = *p++;
    *d = '\0';
    return g_ok;
}

uint64_t mono_us(void) {
    if (!g_ok) return 0;
    return ((cntvct() - g_start) * 1000000ull) / g_freq;
}

const char *mono_source(void) { return g_source[0] ? g_source : "uncalibrated"; }

uint32_t mono_resolution_ns(void) {
    if (!g_freq) return 0;
    return (uint32_t)(1000000000ull / g_freq) + 1u;
}

/* The counter does not drift against itself, so these are honest zeros
   rather than numbers nobody updates. */
uint32_t mono_resyncs(void)        { return 0; }
uint32_t mono_drift_permille(void) { return 0; }
