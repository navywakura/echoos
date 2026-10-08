/* Event fabric types: typed events for input and actors. */
#ifndef RXOS_RX_EVENT_H
#define RXOS_RX_EVENT_H

#include "rogex.h"

/* ------------------------- Q16.16 fixed point ----------------------------- */
/* Layout: bit 31 sign, bits 30..16 integer, bits 15..0 fraction.
   Range ±32767.9999; resolution 1/65536 ≈ 1.5e-5.
   Bounds: q16_mul multiplies through int64 (|a·b| < 2^62 for any two Q16
   values, no overflow); callers keep potentials below Q16_POTENTIAL_CAP so
   repeated integration cannot approach the 2^31 ceiling. */
#define Q16_SHIFT 16
#define Q16_ONE   ((int32_t)1 << Q16_SHIFT)
#define Q16_FROM_INT(n)   ((int32_t)((n) << Q16_SHIFT))
#define Q16_TO_INT(q)     ((int32_t)((q) >> Q16_SHIFT))
#define Q16_POTENTIAL_CAP Q16_FROM_INT(1024)   /* integration clamp          */

static inline int32_t q16_mul(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * (int64_t)b) >> Q16_SHIFT);
}
static inline int32_t q16_div(int32_t a, int32_t b) {
    if (b == 0) return 0;                       /* defined, not UB            */
    return (int32_t)(((int64_t)a << Q16_SHIFT) / (int64_t)b);
}

/* Lazy exponential decay, first-order rational approximation:
 *
 *      V(now) = V(last) · tau / (tau + dt)
 *
 * Chosen over exp() because it needs one 64-bit multiply and one divide, is
 * exact at dt=0, monotonically decreasing, and never underflows negative.
 * Error vs true exp(-dt/tau) stays under ~8% for dt <= tau (documented in
 * the blueprint's TECHNICAL_REPORT); for scheduling heuristics that is
 * ample — nothing correctness-critical reads a membrane potential. */
static inline int32_t calculate_lazy_decay(int32_t potential_q16,
                                           uint64_t dt_ticks,
                                           uint32_t tau_ticks) {
    if (dt_ticks == 0 || tau_ticks == 0) return potential_q16;
    uint64_t denominator = (uint64_t)tau_ticks + dt_ticks;
    return (int32_t)(((int64_t)potential_q16 * (int64_t)tau_ticks)
                     / (int64_t)denominator);
}

/* ------------------------------ events ------------------------------------ */
/* Event types (rx_event_t.type). */
#define RX_EVENT_NONE          0u
#define RX_EVENT_NET_RX_READY  1u   /* NIC ISR wake-hint: frames may wait    */
#define RX_EVENT_DEADLINE      2u   /* PIT-bounded fallback stimulus         */
#define RX_EVENT_SELFTEST      3u   /* used only by rx_event_selftest        */
#define RX_EVENT_KBD           4u   /* keyboard character (deterministic)    */
#define RX_EVENT_MOUSE         5u   /* mouse packet     (deterministic)      */
#define RX_EVENT_NOTIFY        6u   /* high-amplitude banner (soft actor)    */
#define RX_EVENT_AUDIO         7u   /* PCM / beep tick                       */
#define RX_EVENT_APP_WAKE      8u   /* background app axon                   */

/* Event sources (rx_event_t.source). */
#define RX_SRC_KERNEL     0u
#define RX_SRC_NET_ISR    1u        /* produced in interrupt context         */
#define RX_SRC_PUMP       2u        /* produced by the idle-loop pump        */
#define RX_SRC_INPUT_ISR  3u        /* IRQ1 keyboard / IRQ12 mouse handlers  */

/* One event descriptor: exactly one x86_64 cache line, so a descriptor
   never straddles two lines and ISR writes touch a single line. */
typedef struct __attribute__((aligned(64))) {
    uint32_t type;                  /* RX_EVENT_*                             */
    uint16_t source;                /* RX_SRC_*                               */
    uint16_t destination;           /* actor id (informational in MVP)        */
    uint8_t  priority;              /* 0 = highest; unused by the MVP pump    */
    uint8_t  flags;
    uint16_t payload_len;           /* valid bytes in payload[]               */
    int32_t  stimulus_q16;          /* Q16.16 contribution to the membrane    */
    uint64_t timestamp_ticks;       /* pit_ticks() at production              */
    uint8_t  payload[40];           /* small inline payload (descriptors)     */
} rx_event_t;

_Static_assert(sizeof(rx_event_t) == 64, "rx_event_t must be exactly 64 bytes");
_Static_assert(_Alignof(rx_event_t) == 64, "rx_event_t must be cache-line aligned");

/* ---------------------- SPSC lock-free ring buffer ------------------------ */
/* Single producer (one ISR), single consumer (the pump/main flow), one core.
 *
 * Contract: `head` is written ONLY by the producer, `tail` ONLY by the
 * consumer; both indices grow monotonically and are used modulo size (a
 * power of two). The producer publishes the slot BEFORE bumping head
 * (mfence between them), so a consumer that observes the new head always
 * observes a fully-written descriptor. On one x86 core this ordering is
 * guaranteed even against IRQ preemption; mfence also stops the compiler
 * from reordering across the publish. No cli/sti, no loops in the ISR.
 *
 * IMPORTANT: exactly ONE producer context per ring. Same-context stimuli
 * (e.g. the pump's own deadline events) must NOT enqueue here — an ISR
 * preempting the pump mid-enqueue would make two concurrent producers.
 * Use rx_actor_stimulate() for same-context injection instead. */
#define RX_RING_SLOTS 256           /* power of two; 256 * 64 B = 16 KiB      */

typedef struct {
    rx_event_t        slot[RX_RING_SLOTS];
    volatile uint32_t head;         /* producer-owned: next write index       */
    volatile uint32_t tail;         /* consumer-owned: next read index        */
    volatile uint32_t dropped;      /* producer-side drops on full ring       */
} rx_spsc_ring_t;

bool rx_ring_enqueue(rx_spsc_ring_t *r, const rx_event_t *ev);  /* producer  */
bool rx_ring_dequeue(rx_spsc_ring_t *r, rx_event_t *out);       /* consumer  */
static inline bool rx_ring_empty(const rx_spsc_ring_t *r) {
    return r->head == r->tail;
}

/* --------------------- input events (deterministic) ----------------------- */
/* Keyboard and mouse are migrated OFF the legacy IPC message queue onto the
   fabric's typed transport. They use the raw SPSC ring, NOT a thresholded
   actor: input must never be suppressed (the critical boundary — a keystroke
   is a correctness event, not a soft hint). Both the IRQ1 (keyboard) and
   IRQ12 (mouse) handlers produce here; on this single core the interrupt
   gates keep IF=0 throughout a handler, so the two ISRs never nest and act as
   ONE serialized producer — the SPSC contract holds. The consumer is the UI
   event pump (ui/events.c) and, when used, console_getc (terminal.c). */
typedef struct {
    uint8_t  kind;                  /* RX_INPUT_KEY | RX_INPUT_MOUSE          */
    char     ch;                    /* key: Latin-1 char                      */
    int16_t  dx, dy;                /* mouse: screen deltas                   */
    uint8_t  buttons;               /* mouse: bit0 L, bit1 R, bit2 M          */
} rx_input_t;
#define RX_INPUT_KEY   1
#define RX_INPUT_MOUSE 2

void     rx_input_post_key(char c);                        /* ISR producer    */
void     rx_input_post_mouse(int dx, int dy, int buttons); /* ISR producer    */
bool     rx_input_pop(rx_input_t *out);                    /* consumer        */
uint64_t rx_input_events(void);                            /* telemetry       */
uint32_t rx_input_dropped(void);
bool     rx_input_selftest(void);   /* ring round-trip; PASS = true           */

/* ---------------------- background actor pump registry -------------------- */
/* The shared dispatch core. Background services register a pump function; the
   idle path (ui/events.c) and rx_kernel_event_loop() both drive them through
   fabric_pump_all(). This is the mechanical core of "event-loop inversion":
   the SAME dispatch runs whether the shell calls it during an input wait or
   the kernel loop owns the machine outright. Full shell-as-actor inversion
   (the shell becoming a suspendable actor) needs the Level 2 scheduler — a
   blocking line reader cannot be an actor without cooperative suspension. */
typedef void (*rx_pump_fn)(void);
void fabric_register_pump(rx_pump_fn fn);   /* up to 8 pumps                  */
void fabric_pump_all(void);                 /* drive every registered pump    */

/* --------------------- memristive software synapse ------------------------ */
/* Normalized conductive state x in [0,1] as Q16.16 (weight), with pair-based
   STDP traces that decay lazily by the same rational rule. Ported from the
   validated Python MemristorSynapse; no timers, state moves only on events. */
typedef struct {
    int32_t  weight_q16;            /* x in [0, Q16_ONE]                      */
    int32_t  pre_trace_q16;
    int32_t  post_trace_q16;
    uint64_t last_trace_ticks;
    uint32_t tau_trace_ticks;       /* trace decay constant                   */
    int32_t  a_plus_q16;            /* LTP amplitude                          */
    int32_t  a_minus_q16;           /* LTD amplitude                          */
} rx_synapse_t;

void rx_synapse_init(rx_synapse_t *s, int32_t weight_q16);
void rx_synapse_pre_spike(rx_synapse_t *s, uint64_t now_ticks);
void rx_synapse_post_spike(rx_synapse_t *s, uint64_t now_ticks);

/* ------------------------------ actors ------------------------------------ */
struct rx_actor;
/* Return false to report that this activation failed. Consecutive failures
   past an actor's fault budget mark it FAILED and reroute its work to a
   registered standby (rx_actor_set_standby) — the software analogue of the
   manifesto's "route around the defect instead of stopping at it". Without a
   physical crossbar this is classical redundancy expressed in neuromorphic
   vocabulary, and it is labeled as such rather than oversold. */
typedef bool (*rx_actor_cb)(struct rx_actor *self, const rx_event_t *ev);

typedef struct {
    uint64_t received;              /* events consumed                        */
    uint64_t suppressed;            /* below threshold / refractory           */
    uint64_t spikes;                /* callback invocations                   */
    uint64_t hint_events;           /* type == NET_RX_READY                   */
    uint64_t deadline_events;       /* type == DEADLINE                       */
    uint64_t faults;                /* callback reported failure              */
    uint64_t failovers;             /* times work was rerouted to the standby */
} rx_actor_stats_t;

typedef struct rx_actor {
    const char     *name;
    int32_t         potential_q16;  /* membrane potential V                   */
    int32_t         threshold_q16;  /* fire when V >= threshold               */
    int32_t         reset_q16;      /* V after a spike                        */
    uint32_t        tau_ticks;      /* membrane decay constant (PIT ticks)    */
    uint32_t        refractory_ticks;
    uint64_t        last_update_ticks;
    bool            has_update;     /* last_update_ticks is valid             */
    uint64_t        last_spike_ticks;
    bool            has_spiked;     /* last_spike_ticks is valid              */
    rx_actor_cb     callback;       /* bounded work; runs in pump context     */
    rx_spsc_ring_t  ring;           /* inbox (ISR producer -> pump consumer)  */
    rx_synapse_t    synapse;        /* local plasticity record                */
    rx_actor_stats_t stats;
    /* --- logical plasticity: fault tolerance by rerouting ---------------- */
    struct rx_actor *standby;       /* takes over when this actor fails       */
    uint32_t        fault_budget;   /* consecutive failures tolerated (0=off) */
    uint32_t        consecutive_faults;
    bool            failed;         /* retired; work goes to the standby      */
} rx_actor_t;

/* Register a redundant path. When `a` exceeds its fault budget it is retired
   and its activations are delivered to `standby` instead. */
void rx_actor_set_standby(rx_actor_t *a, rx_actor_t *standby, uint32_t fault_budget);
bool rx_actor_failed(const rx_actor_t *a);

/* Initialize an actor control block (all Q16 fields already scaled). */
void rx_actor_init(rx_actor_t *a, const char *name, rx_actor_cb cb,
                   int32_t threshold_q16, int32_t reset_q16,
                   uint32_t tau_ticks, uint32_t refractory_ticks);

/* Drain the actor's ring completely: lazy-decay, integrate each stimulus,
   apply refractory/threshold policy, spike -> reset + callback. Returns the
   number of events consumed (0 = no work was pending). */
int rx_actor_dispatch(rx_actor_t *a, uint64_t now_ticks);

/* Same-context stimulus injection (bypasses the SPSC ring on purpose — see
   the single-producer contract above). Follows the identical LIF path. */
void rx_actor_stimulate(rx_actor_t *a, uint32_t type, int32_t stimulus_q16,
                        uint64_t now_ticks);

/* Pure read of the membrane potential as it stands at `now` (applies the
   lazy decay to the returned value WITHOUT mutating the actor) — for live
   telemetry/demo views that redraw between events. */
static inline int32_t rx_actor_peek_potential(const rx_actor_t *a,
                                              uint64_t now_ticks) {
    /* has_update, not last_update_ticks != 0: an actor whose first activity
       lands on tick 0 has a perfectly valid timestamp of zero, and testing the
       value conflated that with "never updated" — such an actor then never
       leaked. Found by the `bench` decay row, which disagreed with the model
       at every dt but zero. */
    uint64_t dt = (a->has_update && now_ticks > a->last_update_ticks)
                      ? now_ticks - a->last_update_ticks : 0;
    return calculate_lazy_decay(a->potential_q16, dt, a->tau_ticks);
}

/* Master event loop for a fabric-owned machine: drive every registered pump
   (fabric_pump_all) and drain input; hlt when a full pass finds no work. The
   MVP does NOT install this as the machine owner — the desktop shell still
   owns the interactive outer loop and embeds the SAME dispatch core through
   the idle hook — but the loop is real, uses fabric_pump_all like the live
   path, and is the seat the fabric takes once the Level 2 scheduler can
   suspend a blocking reader. Never returns. */
ROGEX_NORETURN void rx_kernel_event_loop(void);

/* Deterministic boot self-test (ring integrity + wraparound + drop policy,
   decay exactness, integrate/fire/suppress/refractory). True = PASS. */
bool rx_event_selftest(void);

#endif /* RXOS_RX_EVENT_H */
