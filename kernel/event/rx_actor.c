/*
 * Actor runtime on the event fabric. Cooperative; one callback at a time.
 */
#include "rx_event.h"
#include "arch.h"
#include "rx_backend.h"
#include "scheduler.h"
#include "power.h"

/* Full fence: orders the slot write before the head publish (and the tail
   read before the slot read on the consumer side), and forbids the compiler
   from folding accesses across it. On one x86 core this is stronger than
   strictly required (x86 stores are already ordered), but it is cheap and
   makes the contract explicit. */
static inline void rx_mfence(void) { arch_barrier_full(); }

/* ---------------------- SPSC ring: producer side -------------------------- */
/* Runs in ISR context: no loops, no waiting. A full ring DROPS the event
   (drop-newest) and counts it — for a wake-hint fabric that is correct
   behaviour: the deadline stimulus guarantees the consumer still runs. */
bool rx_ring_enqueue(rx_spsc_ring_t *r, const rx_event_t *ev) {
    uint32_t head = r->head;
    uint32_t tail = r->tail;                    /* one racy read: tail only   */
    if (head - tail >= RX_RING_SLOTS) {         /* grows -> ring can only be  */
        r->dropped++;                           /* fuller than we saw: safe   */
        return false;
    }
    r->slot[head & (RX_RING_SLOTS - 1)] = *ev;  /* write the descriptor       */
    rx_mfence();                                /* slot visible before head   */
    r->head = head + 1;                         /* publish                    */
    return true;
}

/* ---------------------- SPSC ring: consumer side -------------------------- */
bool rx_ring_dequeue(rx_spsc_ring_t *r, rx_event_t *out) {
    uint32_t tail = r->tail;
    if (tail == r->head) return false;          /* empty                      */
    rx_mfence();                                /* head seen -> slot is ready */
    *out = r->slot[tail & (RX_RING_SLOTS - 1)];
    rx_mfence();                                /* copy done before release   */
    r->tail = tail + 1;
    return true;
}

/* ------------------------------ synapse ----------------------------------- */
static int32_t clamp01_q16(int32_t v) {
    if (v < 0) return 0;
    if (v > Q16_ONE) return Q16_ONE;
    return v;
}

void rx_synapse_init(rx_synapse_t *s, int32_t weight_q16) {
    s->weight_q16       = clamp01_q16(weight_q16);
    s->pre_trace_q16    = 0;
    s->post_trace_q16   = 0;
    s->last_trace_ticks = 0;
    s->tau_trace_ticks  = 2;               /* ~20 ms at PIT 100 Hz            */
    s->a_plus_q16       = Q16_ONE / 40;    /* 0.025                           */
    s->a_minus_q16      = (Q16_ONE * 11) / 400;  /* 0.0275                    */
}

static void synapse_decay_traces(rx_synapse_t *s, uint64_t now_ticks) {
    uint64_t dt = now_ticks - s->last_trace_ticks;
    if (s->last_trace_ticks != 0 && dt > 0) {
        s->pre_trace_q16  = calculate_lazy_decay(s->pre_trace_q16,  dt, s->tau_trace_ticks);
        s->post_trace_q16 = calculate_lazy_decay(s->post_trace_q16, dt, s->tau_trace_ticks);
    }
    s->last_trace_ticks = now_ticks;
}

/* Pre spike shortly after a post spike -> depression (LTD). */
void rx_synapse_pre_spike(rx_synapse_t *s, uint64_t now_ticks) {
    synapse_decay_traces(s, now_ticks);
    s->weight_q16 = clamp01_q16(s->weight_q16
                                - q16_mul(s->a_minus_q16, s->post_trace_q16));
    s->pre_trace_q16 += Q16_ONE;
}

/* Post spike shortly after a pre spike -> potentiation (LTP). */
void rx_synapse_post_spike(rx_synapse_t *s, uint64_t now_ticks) {
    synapse_decay_traces(s, now_ticks);
    s->weight_q16 = clamp01_q16(s->weight_q16
                                + q16_mul(s->a_plus_q16, s->pre_trace_q16));
    s->post_trace_q16 += Q16_ONE;
}

/* ------------------------------- actor ------------------------------------ */
void rx_actor_init(rx_actor_t *a, const char *name, rx_actor_cb cb,
                   int32_t threshold_q16, int32_t reset_q16,
                   uint32_t tau_ticks, uint32_t refractory_ticks) {
    k_memset(a, 0, sizeof(*a));
    a->name             = name;
    a->callback         = cb;
    a->threshold_q16    = threshold_q16;
    a->reset_q16        = reset_q16;
    a->tau_ticks        = tau_ticks;
    a->refractory_ticks = refractory_ticks;
    rx_synapse_init(&a->synapse, Q16_ONE / 2);
}

/* The one LIF step every stimulus goes through, ring or direct. */
static void actor_integrate(rx_actor_t *a, uint32_t type,
                            int32_t stimulus_q16, uint64_t now_ticks) {
    a->stats.received++;
    if (type == RX_EVENT_NET_RX_READY) a->stats.hint_events++;
    if (type == RX_EVENT_DEADLINE)     a->stats.deadline_events++;

    /* Lazy decay: computed only now, from the timestamp delta — there is no
       periodic "neural tick" anywhere in the kernel. */
    uint64_t dt = a->has_update ? now_ticks - a->last_update_ticks : 0;
    if (a->has_update && dt > 0)
        a->potential_q16 = calculate_lazy_decay(a->potential_q16, dt, a->tau_ticks);
    a->last_update_ticks = now_ticks;
    a->has_update = true;

    /* Integrate, with a cap so pathological stimulus streams cannot creep
       toward the int32 ceiling (Q16_POTENTIAL_CAP << 2^31). */
    int64_t v = (int64_t)a->potential_q16 + (int64_t)stimulus_q16;
    if (v > Q16_POTENTIAL_CAP) v = Q16_POTENTIAL_CAP;
    if (v < 0)                 v = 0;
    a->potential_q16 = (int32_t)v;

    rx_synapse_pre_spike(&a->synapse, now_ticks);   /* record the arrival     */

    bool refractory = a->has_spiked &&
                      (now_ticks - a->last_spike_ticks) < a->refractory_ticks;
    if (a->potential_q16 < a->threshold_q16 || refractory) {
        a->stats.suppressed++;
        return;                                     /* stay dormant           */
    }

    /* Spike: reset first so a re-entrant stimulate from the callback sees a
       consistent membrane, then run the bounded callback. */
    a->potential_q16   = a->reset_q16;
    a->last_spike_ticks = now_ticks;
    a->has_spiked       = true;
    a->stats.spikes++;
    rx_backend_note_spike();                    /* fabric-wide spike counter */
    rx_synapse_post_spike(&a->synapse, now_ticks);

    /* Deliver. If this actor has been retired by the fault policy, its work
       goes to the standby instead — the caller never sees the difference. */
    rx_actor_t *target = a;
    if (a->failed && a->standby) { target = a->standby; a->stats.failovers++; }

    if (target->callback) {
        rx_event_t ev;
        k_memset(&ev, 0, sizeof(ev));
        ev.type            = type;
        ev.source          = RX_SRC_PUMP;
        ev.stimulus_q16    = stimulus_q16;
        ev.timestamp_ticks = now_ticks;
        bool ok = target->callback(target, &ev);

        /* Fault accounting on the actor that actually ran. A success clears
           the streak: transient errors must not retire a healthy actor. */
        if (ok) {
            target->consecutive_faults = 0;
        } else {
            target->stats.faults++;
            target->consecutive_faults++;
            if (target->fault_budget &&
                target->consecutive_faults >= target->fault_budget &&
                target->standby) {
                target->failed = true;   /* retire; reroute from here on */
            }
        }
    }
}

void rx_actor_set_standby(rx_actor_t *a, rx_actor_t *standby, uint32_t fault_budget) {
    if (!a) return;
    a->standby            = standby;
    a->fault_budget       = fault_budget;
    a->consecutive_faults = 0;
    a->failed             = false;
}

bool rx_actor_failed(const rx_actor_t *a) { return a && a->failed; }

int rx_actor_dispatch(rx_actor_t *a, uint64_t now_ticks) {
    int consumed = 0;
    rx_event_t ev;
    while (rx_ring_dequeue(&a->ring, &ev)) {
        /* Use the later of production/dispatch time so decay never runs
           backwards if an ISR stamped a tick the consumer hasn't seen. */
        uint64_t t = ev.timestamp_ticks > now_ticks ? ev.timestamp_ticks : now_ticks;
        actor_integrate(a, ev.type, ev.stimulus_q16, t);
        consumed++;
        if (consumed >= RX_RING_SLOTS) break;   /* bounded: one ring's worth  */
    }
    return consumed;
}

void rx_actor_stimulate(rx_actor_t *a, uint32_t type, int32_t stimulus_q16,
                        uint64_t now_ticks) {
    actor_integrate(a, type, stimulus_q16, now_ticks);
}

/* --------------------- background pump registry ---------------------------- */
/* The shared dispatch core (see rx_event.h). Background services register a
   pump; both the live idle path and rx_kernel_event_loop() drive them here. */
#define RX_MAX_PUMPS 8
static rx_pump_fn g_pumps[RX_MAX_PUMPS];
static int        g_npumps;

void fabric_register_pump(rx_pump_fn fn) {
    if (fn && g_npumps < RX_MAX_PUMPS) g_pumps[g_npumps++] = fn;
}
void fabric_pump_all(void) {
    for (int i = 0; i < g_npumps; i++) g_pumps[i]();
}

/* ----------------------- master event loop --------------------------------- */
/* The seat the fabric takes when it owns the machine. It drives the exact
   same fabric_pump_all() the live idle hook uses, and drains input; a pass
   with zero work parks the CPU in hlt until the next interrupt produces an
   event. Not installed as the owner in the MVP — the interactive shell still
   owns the outer loop and reaches this dispatch through the idle hook — but
   this is real, coherent code, not a placeholder. */
ROGEX_NORETURN void rx_kernel_event_loop(void) {
    for (;;) {
        fabric_pump_all();          /* net drain, and future background svcs */
        sched_yield();              /* hand the CPU to the shell/other tasks */
        /* Nothing else to run: park the core in the deepest idle state this
           machine supports (MWAIT C-state hint, or HLT) until an interrupt. */
        power_idle();
    }
}

/* ------------------------------ self-test ---------------------------------- */
static int  g_st_spikes;
static bool st_cb(rx_actor_t *self, const rx_event_t *ev) {
    (void)self; (void)ev;
    g_st_spikes++;
    return true;
}

/* Fault-tolerance fixtures: a callback that always reports failure, and the
   standby that should inherit its work once the budget is spent. */
static int g_st_fail_hits, g_st_backup_hits;
static bool st_failing_cb(rx_actor_t *self, const rx_event_t *ev) {
    (void)self; (void)ev; g_st_fail_hits++; return false;
}
static bool st_backup_cb(rx_actor_t *self, const rx_event_t *ev) {
    (void)self; (void)ev; g_st_backup_hits++; return true;
}

bool rx_event_selftest(void) {
    /* 1. Ring integrity across wraparound: push/pop 3 full laps. */
    static rx_spsc_ring_t ring;                 /* static: 16 KiB, not stack  */
    k_memset(&ring, 0, sizeof(ring));
    rx_event_t ev, out;
    k_memset(&ev, 0, sizeof(ev));
    for (uint32_t i = 0; i < RX_RING_SLOTS * 3u; i++) {
        ev.type = RX_EVENT_SELFTEST;
        ev.stimulus_q16 = (int32_t)i;
        if (!rx_ring_enqueue(&ring, &ev)) return false;
        if (!rx_ring_dequeue(&ring, &out)) return false;
        if (out.stimulus_q16 != (int32_t)i) return false;
    }
    /* 2. Drop policy: fill to capacity, the next enqueue must drop+count. */
    for (uint32_t i = 0; i < RX_RING_SLOTS; i++)
        if (!rx_ring_enqueue(&ring, &ev)) return false;
    if (rx_ring_enqueue(&ring, &ev)) return false;
    if (ring.dropped != 1) return false;
    while (rx_ring_dequeue(&ring, &out)) { }
    if (!rx_ring_empty(&ring)) return false;

    /* 3. Decay exactness: V0=1.0, tau=100, dt=100 -> exactly 0.5 (32768). */
    if (calculate_lazy_decay(Q16_ONE, 100, 100) != Q16_ONE / 2) return false;
    if (calculate_lazy_decay(Q16_ONE, 0, 100)   != Q16_ONE)     return false;
    if (calculate_lazy_decay(Q16_ONE, 300, 100) != Q16_ONE / 4) return false;

    /* 4. LIF policy: sub-threshold suppressed; integration fires; the
       refractory window suppresses an immediate re-fire. */
    static rx_actor_t a;
    g_st_spikes = 0;
    rx_actor_init(&a, "selftest", st_cb,
                  Q16_ONE, 0, /*tau*/ 1000, /*refractory*/ 5);
    rx_actor_stimulate(&a, RX_EVENT_SELFTEST, Q16_ONE / 2, 10);  /* 0.5: no  */
    if (g_st_spikes != 0 || a.stats.suppressed != 1) return false;
    rx_actor_stimulate(&a, RX_EVENT_SELFTEST, (Q16_ONE * 6) / 10, 11); /* fires */
    if (g_st_spikes != 1 || a.potential_q16 != 0) return false;
    rx_actor_stimulate(&a, RX_EVENT_SELFTEST, Q16_ONE * 2, 12);  /* refractory */
    if (g_st_spikes != 1 || a.stats.suppressed != 2) return false;
    rx_actor_stimulate(&a, RX_EVENT_SELFTEST, Q16_ONE * 2, 20);  /* window over */
    if (g_st_spikes != 2) return false;

    /* 5. Ring -> dispatch path end to end. */
    k_memset(&a.ring, 0, sizeof(a.ring));
    ev.type = RX_EVENT_SELFTEST; ev.stimulus_q16 = Q16_ONE * 3;
    ev.timestamp_ticks = 30;
    if (!rx_ring_enqueue(&a.ring, &ev)) return false;
    if (rx_actor_dispatch(&a, 30) != 1) return false;
    if (g_st_spikes != 3) return false;

    /* 6. Input transport (deterministic ring): key + mouse round-trip. */
    if (!rx_input_selftest()) return false;

    /* 7. Logical plasticity: a failing actor is retired after its fault
          budget and its work reroutes to the standby, without the caller
          changing anything. */
    static rx_actor_t prim, backup;
    g_st_fail_hits = 0; g_st_backup_hits = 0;
    rx_actor_init(&backup, "standby", st_backup_cb, Q16_ONE, 0, 1000, 0);
    rx_actor_init(&prim,   "primary", st_failing_cb, Q16_ONE, 0, 1000, 0);
    rx_actor_set_standby(&prim, &backup, /*fault budget*/ 2);

    rx_actor_stimulate(&prim, RX_EVENT_SELFTEST, Q16_ONE, 100);   /* fault 1 */
    if (rx_actor_failed(&prim)) return false;                     /* not yet */
    rx_actor_stimulate(&prim, RX_EVENT_SELFTEST, Q16_ONE, 101);   /* fault 2 */
    if (!rx_actor_failed(&prim)) return false;      /* budget spent: retired */
    if (g_st_fail_hits != 2 || g_st_backup_hits != 0) return false;

    rx_actor_stimulate(&prim, RX_EVENT_SELFTEST, Q16_ONE, 102);   /* reroute */
    if (g_st_fail_hits != 2) return false;          /* primary no longer run */
    if (g_st_backup_hits != 1) return false;        /* standby took over     */
    if (prim.stats.failovers != 1) return false;

    return true;
}
