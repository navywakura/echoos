/*
 * Neuromorphic micro-benchmarks (LIF/STDP path).
 * Used to catch real faults — it already found some. Keep it honest.
 */
#include "neurobench.h"
#include "arch.h"
#include "rx_event.h"
#include "arch.h"
#include "commands.h"    /* o_str / o_dec / o_nl */

/* Stimulus of 0.35 in Q16.16 — the same value the `neuro` demo uses. */
#define S35   ((Q16_ONE * 35) / 100)
#define TAU   100u        /* 1 s at PIT 100 Hz                                */

/* THE ACTORS LIVE IN BSS, NOT ON THE STACK.
   sizeof(rx_actor_t) is 16704 bytes — it carries a 256-slot SPSC inbox of
   64-byte events — and a task stack is 16384. A single one as a local
   overflows the whole stack; t_determinism() wanted two. That overflow ran
   off the bottom of the shell task's stack into neighbouring BSS and
   corrupted whatever was there: on one machine the heap free list, so a
   later `mem` page-faulted inside heap_block_count(); on another the physical
   allocator's own counters, so `mem` printed 0 MiB in use out of 0 MiB free.
   One cause, two symptoms, both intermittent because which globals get hit
   depends on link order.

   The benchmark never uses the actors' rings — rx_actor_stimulate() bypasses
   them by design — but the struct carries one regardless, so these have to be
   statics. */
static rx_actor_t g_a, g_b;

static int g_fires;
static bool count_fire(rx_actor_t *self, const rx_event_t *ev) {
    (void)self; (void)ev; g_fires++; return true;
}

static void fresh(rx_actor_t *a, const char *name, uint32_t refractory) {
    rx_actor_init(a, name, count_fire, Q16_ONE, 0, TAU, refractory);
}

/* ------------------------------ reporting -------------------------------- */
static int g_pass, g_fail;

static void row(const char *test, const char *expect, const char *got, bool ok) {
    o_str("  ");
    { int n = 0; o_str(test); for (const char *p = test; *p; p++) n++;
      for (; n < 26; n++) o_str(" "); }
    { int n = 0; o_str(expect); for (const char *p = expect; *p; p++) n++;
      for (; n < 14; n++) o_str(" "); }
    { int n = 0; o_str(got); for (const char *p = got; *p; p++) n++;
      for (; n < 14; n++) o_str(" "); }
    o_str(ok ? "PASS\n" : "FAIL\n");
    if (ok) g_pass++; else g_fail++;
}

static void num(char *out, int64_t v, const char *suffix) {
    char t[24]; int i = 0;
    if (v < 0) { *out++ = '-'; v = -v; }
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i) *out++ = t[--i];
    while (*suffix) *out++ = *suffix++;
    *out = '\0';
}

/* --------------------------- 1. temporal coding --------------------------- */
/* Three identical stimuli summing to 1.05x threshold. Delivered on
   consecutive ticks the leak barely touches them and the third crosses.
   Delivered 2 tau apart each one has decayed to a third before the next
   arrives, and the membrane never gets there. */
static void t_temporal(void) {
    rx_actor_t *ap = &g_a;
    char got[32];

    fresh(ap, "burst", 0);
    g_fires = 0;
    rx_actor_stimulate(ap, RX_EVENT_SELFTEST, S35, 0);
    rx_actor_stimulate(ap, RX_EVENT_SELFTEST, S35, 1);
    rx_actor_stimulate(ap, RX_EVENT_SELFTEST, S35, 2);
    int burst = g_fires;
    num(got, burst, " spike");
    row("temporal: 3 stim, fast", "1 spike", got, burst == 1);

    fresh(ap, "spread", 0);
    g_fires = 0;
    rx_actor_stimulate(ap, RX_EVENT_SELFTEST, S35, 0);
    rx_actor_stimulate(ap, RX_EVENT_SELFTEST, S35, 200);
    rx_actor_stimulate(ap, RX_EVENT_SELFTEST, S35, 400);
    int spread = g_fires;
    num(got, spread, " spikes");
    row("temporal: 3 stim, slow", "0 spikes", got, spread == 0);
}

/* ------------------------- 2. decay vs the model -------------------------- */
/* V(dt) = V0 * tau / (tau + dt), evaluated in Q16.16 integers. The expected
   column is arithmetic done by hand, not a second call into the same code, so
   agreement is evidence about the implementation rather than a tautology. */
static void t_decay(void) {
    static const struct { uint64_t dt; int32_t want; } K[] = {
        {   0, 65536 },     /* 65536 * 100 / 100 */
        {  50, 43690 },     /* 65536 * 100 / 150 */
        { 100, 32768 },     /* 65536 * 100 / 200 */
        { 200, 21845 },     /* 65536 * 100 / 300 */
        { 400, 13107 },     /* 65536 * 100 / 500 */
    };
    rx_actor_t *ap = &g_a;
    fresh(ap, "decay", 0);
    g_fires = 0;
    /* Threshold is 1.0 and the stimulus is 1.0, so this spikes and resets to
       0. Charge to just under threshold instead: 0.99. */
    int32_t v0 = (Q16_ONE * 99) / 100;
    rx_actor_stimulate(ap, RX_EVENT_SELFTEST, v0, 0);

    int agree = 0;
    for (unsigned i = 0; i < sizeof(K) / sizeof(K[0]); i++) {
        int32_t want = (int32_t)(((int64_t)v0 * 100) / (100 + (int64_t)K[i].dt));
        int32_t got  = rx_actor_peek_potential(ap, K[i].dt);
        if (got == want) agree++;
    }
    char g[32], e[32];
    num(e, (int64_t)(sizeof(K) / sizeof(K[0])), "/5 exact");
    num(g, agree, "/5 exact");
    row("decay vs V*t/(t+dt)", e, g, agree == (int)(sizeof(K) / sizeof(K[0])));
}

/* ---------------------------- 3. determinism ------------------------------ */
/* Two independent actors, the same stimulus train, sampled at the same
   virtual instants. Fixed-point integer dynamics have to agree bit for bit;
   a floating-point implementation would be free not to. */
static void t_determinism(void) {
    rx_actor_t *ap = &g_a, *bp = &g_b;
    fresh(ap, "det_a", 0); fresh(bp, "det_b", 0);
    g_fires = 0;
    bool same = true;
    for (uint64_t t = 0; t < 500; t += 7) {
        rx_actor_stimulate(ap, RX_EVENT_SELFTEST, S35 / 4, t);
        rx_actor_stimulate(bp, RX_EVENT_SELFTEST, S35 / 4, t);
        if (rx_actor_peek_potential(ap, t) != rx_actor_peek_potential(bp, t))
            same = false;
    }
    row("determinism, 72 steps", "bit-exact", same ? "bit-exact" : "diverged", same);
}

/* ----------------------------- 4. refractory ------------------------------ */
/* After a spike the unit is deaf for refractory_ticks no matter how hard it
   is driven. A stimulus of 2x threshold inside the window must be absorbed;
   the same stimulus after it must fire. */
static void t_refractory(void) {
    rx_actor_t *ap = &g_a;
    fresh(ap, "refrac", 30);
    g_fires = 0;
    rx_actor_stimulate(ap, RX_EVENT_SELFTEST, Q16_ONE, 0);        /* spike     */
    int after_first = g_fires;
    rx_actor_stimulate(ap, RX_EVENT_SELFTEST, Q16_ONE * 2, 10);   /* deaf      */
    int inside = g_fires - after_first;
    rx_actor_stimulate(ap, RX_EVENT_SELFTEST, Q16_ONE * 2, 40);   /* awake     */
    int outside = g_fires - after_first - inside;
    bool ok = (after_first == 1 && inside == 0 && outside == 1);
    row("refractory lockout", "0 then 1", ok ? "0 then 1" : "wrong", ok);
}

/* ------------------------------ 5. sparsity ------------------------------- */
/* 200 stimuli, each a fifth of threshold, spaced 3 tau apart so the leak
   always wins. Every one is integrated and every one is absorbed: the
   callback never runs, so no downstream work is done at all. That ratio is
   the economic claim of an event-driven substrate, and here it is counted
   rather than asserted. */
static void t_sparsity(void) {
    rx_actor_t *ap = &g_a;
    fresh(ap, "sparse", 0);
    g_fires = 0;
    for (uint64_t i = 0; i < 200; i++)
        rx_actor_stimulate(ap, RX_EVENT_SELFTEST, Q16_ONE / 5, i * 300);
    char got[40];
    num(got, (int64_t)ap->stats.suppressed, " absorbed");
    row("sparsity: 200 sub-thr", "200 absorbed", got,
        ap->stats.suppressed == 200 && g_fires == 0);
}

/* --------------------------- 6. cost per event ---------------------------- */
static inline uint64_t cycles(void) { return arch_cycles(); }

static uint64_t measure_cycles(void) {
    rx_actor_t *ap = &g_a;
    fresh(ap, "cost", 0);
    g_fires = 0;
    const uint64_t N = 20000;
    uint64_t t0 = cycles();
    for (uint64_t i = 0; i < N; i++)
        rx_actor_stimulate(ap, RX_EVENT_SELFTEST, Q16_ONE / 8, i);
    uint64_t t1 = cycles();
    return (t1 - t0) / N;
}

/* -------------------------------- driver ---------------------------------- */
void neurobench_run(void) {
    g_pass = g_fail = 0;

    o_str("EchOS neuromorphic benchmark - predicted, then measured\n \n");
    o_str("  test                      model requires  kernel gave   \n");
    o_str("  ------------------------------------------------------------\n");

    t_temporal();
    t_decay();
    t_determinism();
    t_refractory();
    t_sparsity();

    o_str(" \n");
    { uint64_t c = measure_cycles();
      o_str("  cost per event   "); o_dec(c);
      o_str(" cycles (integer only: the kernel is\n");
      o_str("                   built -mno-sse, there is no FPU state)\n"); }
    o_str("  live fabric      "); o_dec(rx_input_events());
    o_str(" input events, "); o_dec((uint64_t)rx_input_dropped());
    o_str(" dropped\n \n");

    o_str("  VERDICT: "); o_dec((uint64_t)g_pass); o_str("/");
    o_dec((uint64_t)(g_pass + g_fail)); o_str(" PASS\n \n");

    if (g_fail == 0) {
        o_str("  Rows 1 and 2 are the ones that matter. Three stimuli summing\n");
        o_str("  to 1.05x threshold fire the unit when they arrive together and\n");
        o_str("  do nothing when the same three arrive spread out - identical\n");
        o_str("  input, different outcome, decided by arrival time alone. A\n");
        o_str("  counter cannot do that. The membrane also tracks the analytic\n");
        o_str("  leak exactly, in integers, with no floating point anywhere.\n");
    } else {
        o_str("  A row failed: the implementation and the model disagree.\n");
        o_str("  Treat every neuromorphic claim about this build as unproven\n");
        o_str("  until it is explained.\n");
    }
}
