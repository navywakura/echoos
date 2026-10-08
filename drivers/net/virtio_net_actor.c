/*
 * virtio-net wired as an event-fabric actor.
 * Same hardware path, async-friendly posting.
 */
#include "netif.h"
#include "rx_event.h"
#include "wired.h"
#include "pit.h"
#include "rogex.h"

static rx_actor_t g_net_actor;         /* .ring is 16 KiB -> BSS, not stack   */
static uint64_t   g_last_deadline_tick;
static bool       g_online;

/* Spike callback: bounded drain. wired_service() clears the legacy pending
   hint, polls up to a 64-frame burst, answers PINGs addressed to our alias
   and records PONGs — the exact code path verified by make net-test. */
static bool net_actor_fire(rx_actor_t *self, const rx_event_t *ev) {
    (void)self; (void)ev;
    wired_service();
    return true;                    /* drain never reports failure           */
}

/* ISR-context producer (virtio INTx only): construct one descriptor and
   enqueue it. No loops, no waiting; a full ring drops the hint, which is
   safe because the deadline stimulus guarantees a drain within one tick. */
static void net_actor_isr_produce(void) {
    rx_event_t ev;
    k_memset(&ev, 0, sizeof(ev));
    ev.type            = RX_EVENT_NET_RX_READY;
    ev.source          = RX_SRC_NET_ISR;
    ev.stimulus_q16    = Q16_ONE;               /* == threshold: always fires */
    ev.timestamp_ticks = pit_ticks();
    rx_ring_enqueue(&g_net_actor.ring, &ev);
}

/* Idle-loop pump, installed as the UI idle hook. Runs on every input-wait
   iteration with the CPU already awake (the PIT bounds iteration spacing to
   10 ms even when no input arrives). Cheap when idle: one tick compare and
   one empty-ring check. */
static void net_actor_pump(void) {
    uint64_t now = pit_ticks();

    /* Deadline fallback: at most one stimulus per PIT tick. Direct
       stimulation (not the ring) — see the SPSC note in the header. */
    if (now != g_last_deadline_tick && wired_up()) {
        g_last_deadline_tick = now;
        rx_actor_stimulate(&g_net_actor, RX_EVENT_DEADLINE, Q16_ONE, now);
    }

    /* Drain ISR-produced hint events (virtio INTx, when it delivers). */
    rx_actor_dispatch(&g_net_actor, now);
}

void net_actor_online(void) {
    if (g_online) return;
    rx_actor_init(&g_net_actor, "net_rx", net_actor_fire,
                  /* threshold  */ Q16_ONE,
                  /* reset      */ 0,
                  /* tau        */ 1000,     /* decay is irrelevant: stimulus  */
                  /* refractory */ 0);       /* == threshold, so always fires  */
    virtio_net_set_irq_event_hook(net_actor_isr_produce);
    /* Register with the fabric's shared dispatch core instead of grabbing the
       idle hook directly: the idle path now drives every registered pump via
       fabric_pump_all(), the same core rx_kernel_event_loop() uses. */
    fabric_register_pump(net_actor_pump);
    events_set_idle_hook(fabric_pump_all);
    g_online = true;
}

void net_actor_stats(uint64_t *received, uint64_t *hints,
                     uint64_t *deadlines, uint64_t *spikes,
                     uint32_t *dropped) {
    if (received)  *received  = g_net_actor.stats.received;
    if (hints)     *hints     = g_net_actor.stats.hint_events;
    if (deadlines) *deadlines = g_net_actor.stats.deadline_events;
    if (spikes)    *spikes    = g_net_actor.stats.spikes;
    if (dropped)   *dropped   = g_net_actor.ring.dropped;
}
