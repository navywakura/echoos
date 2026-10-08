/*
 * Neuromorphic bench harness. Format is a contract — do not change the tags.
 *
 *   [BENCH] Neuromorphic Sparsity: XX.X %
 *   [BENCH] Avg Spike Latency:    XX.X us
 *   [BENCH] Power Consumption:    XX.X mW
 *   [BENCH] Heap-0 Static RAM:    XXXX KB (Delta: 0 B)
 */
#include "bench_neuromorphic.h"
#include "prisma5.h"
#include "akida.h"
#include "heap0.h"
#include "heap.h"
#include "power.h"
#include "version.h"

static char  *o_buf;
static size_t o_max, o_len;
static void o_reset(char *b, size_t m) {
    o_buf = b; o_max = m; o_len = 0; if (m) b[0] = '\0';
}
static void o_s(const char *s) {
    if (!s) return;
    while (*s && o_len + 1 < o_max) o_buf[o_len++] = *s++;
    if (o_max) o_buf[o_len] = '\0';
}
static void o_d(uint64_t v) {
    char t[24]; int i = 0;
    if (!v) t[i++] = '0';
    while (v && i < 20) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) { char c[2] = { t[--i], 0 }; o_s(c); }
}
static void o_nl(void) { o_s("\n"); }

static void o_x10(uint32_t x10) {
    o_d(x10 / 10); o_s("."); o_d(x10 % 10);
}

void bench_neuromorphic(char *out, size_t outmax) {
    prisma5_run_t alpha, nil;
    size_t h0 = heap0_total_bytes();
    size_t used0 = heap_used();
    uint32_t mw = 0, rapl = 0, core = 0, gpu = 0;
    bool ak_power, rapl_ok;

    o_reset(out, outmax);
    prisma5_run(PRISMA5_ALPHA, &alpha);
    prisma5_run(PRISMA5_NULL, &nil);

    o_s("[BENCH] Neuromorphic Sparsity: ");
    o_x10(alpha.sparsity_x10); o_s(" %\n");
    o_s("[BENCH] Avg Spike Latency:    ");
    /* latency_ns → us with one decimal */
    {
        uint32_t us10 = alpha.latency_ns / 100;   /* 0.1 us */
        o_x10(us10); o_s(" us\n");
    }

    ak_power = false;   /* no register contract for the AKD1000 exists here */
    rapl_ok = power_measure_mw(10, &rapl, &core, &gpu);
    o_s("[BENCH] Power Consumption:    ");
    if (rapl_ok) {
        o_d(rapl / 10); o_s("."); o_d(rapl % 10);
        o_s(" mW  (CPU RAPL; no AKD1000 — NPU budget < 100 mW)\n");
    } else {
        o_s("n/a mW  (no AKD1000, no RAPL; NPU budget < 100 mW)\n");
    }

    o_s("[BENCH] Heap-0 Static RAM:    ");
    o_d(h0 / 1024); o_s(" KB (Delta: ");
    {
        size_t used1 = heap_used();
        if (used1 > used0) { o_d((uint64_t)(used1 - used0)); o_s(" B)\n"); }
        else { o_s("0 B)\n"); }
    }

    o_s("[BENCH] Null-input activity:  ");
    o_d(nil.spikes); o_s(" spikes / "); o_d(nil.slots);
    o_s(nil.spikes == 0 ? "  PASS (silent)\n" : "  FAIL (should be 0)\n");
    o_s("[BENCH] Backend:              ");
    o_s("software LIF (CPU)");
    o_s("  "); o_s(akida_status()); o_nl();
    o_s("[BENCH] Edition:              "); o_s(ECHOS_EDITION); o_nl();
}
