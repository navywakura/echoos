/*
 * SPSC input ring. ISRs post; the UI pump pops. Keep posts short.
 */
#include "rx_event.h"

static rx_spsc_ring_t g_in;        /* 16 KiB, lives in BSS (not the stack)    */
static uint64_t       g_in_events; /* accepted events (ISR-written telemetry) */

static void input_post(uint8_t kind, char ch, int dx, int dy, int buttons) {
    rx_event_t ev;
    k_memset(&ev, 0, sizeof(ev));
    ev.type   = (kind == RX_INPUT_KEY) ? RX_EVENT_KBD : RX_EVENT_MOUSE;
    ev.source = RX_SRC_INPUT_ISR;
    /* No pit_ticks() here: keep the ISR path free of driver calls. Ordering,
       not wall-clock timing, is what input needs. */
    rx_input_t in = { kind, ch, (int16_t)dx, (int16_t)dy, (uint8_t)buttons };
    k_memcpy(ev.payload, &in, sizeof(in));
    ev.payload_len = sizeof(in);
    if (rx_ring_enqueue(&g_in, &ev)) g_in_events++;   /* full ring: drop+count */
}

void rx_input_post_key(char c) { input_post(RX_INPUT_KEY, c, 0, 0, 0); }
void rx_input_post_mouse(int dx, int dy, int buttons) {
    input_post(RX_INPUT_MOUSE, 0, dx, dy, buttons);
}

bool rx_input_pop(rx_input_t *out) {
    rx_event_t ev;
    if (!rx_ring_dequeue(&g_in, &ev)) return false;
    k_memcpy(out, ev.payload, sizeof(*out));
    return true;
}

uint64_t rx_input_events(void)  { return g_in_events; }
uint32_t rx_input_dropped(void) { return g_in.dropped; }

/* Boot self-test: a key and a mouse packet round-trip through the ring in
   order, and the ring reports empty afterward. Runs before IRQ input is
   enabled, so it only exercises the transport; it leaves the ring empty. */
bool rx_input_selftest(void) {
    rx_input_t r;
    rx_input_post_key('R');
    rx_input_post_mouse(3, -4, 1);
    if (!rx_input_pop(&r) || r.kind != RX_INPUT_KEY || r.ch != 'R') return false;
    if (!rx_input_pop(&r) || r.kind != RX_INPUT_MOUSE ||
        r.dx != 3 || r.dy != -4 || r.buttons != 1) return false;
    if (rx_input_pop(&r)) return false;               /* must be empty now     */
    return true;
}
