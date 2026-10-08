/*
 * Neuromorphic backend switch.
 *
 * Today every spike is computed in software (Q16.16 LIF on the host CPU).
 * The switch is real plumbing: UI, shell, and fabric all read one active
 * backend id. When Akida or Loihi hardware is present we will probe PCI/USB
 * here and route stimulate/fire through their drivers. Until then the stubs
 * say so — never pretend an NPU is running.
 */
#include "rx_backend.h"
#include "akida.h"

static int         g_id = RX_BACKEND_SOFTWARE;
static uint64_t    g_spikes;
static uint64_t    g_idle_ticks;
static const char *g_status = "software LIF on host CPU (active)";

static const char *NAMES[RX_BACKEND_COUNT] = {
    "software", "akida", "loihi"
};
static const char *LABELS[RX_BACKEND_COUNT] = {
    "Software LIF (CPU)",
    "BrainChip Akida",
    "Intel Loihi",
};

void rx_backend_init(void) {
    g_id = RX_BACKEND_SOFTWARE;
    g_spikes = 0;
    g_idle_ticks = 0;
    g_status = "software LIF on host CPU (active)";
}

int         rx_backend_get(void)          { return g_id; }
const char *rx_backend_name(void)         { return NAMES[g_id]; }
const char *rx_backend_label(void)        { return LABELS[g_id]; }
const char *rx_backend_status(void)       { return g_status; }
uint64_t    rx_backend_spikes(void)       { return g_spikes; }
uint64_t    rx_backend_idle_ticks(void)   { return g_idle_ticks; }
void        rx_backend_note_spike(void)   { g_spikes++; }
void        rx_backend_note_idle(void)    { g_idle_ticks++; }

bool rx_backend_available(int id) {
    if (id < 0 || id >= RX_BACKEND_COUNT) return false;
    if (id == RX_BACKEND_SOFTWARE) return true;
    if (id == RX_BACKEND_AKIDA) return akida_present();
    return false;
}

const char *rx_backend_avail_why(int id) {
    if (id < 0 || id >= RX_BACKEND_COUNT) return "unknown backend id";
    if (id == RX_BACKEND_SOFTWARE)
        return "always available — Q16.16 LIF actors on the event fabric";
    if (id == RX_BACKEND_AKIDA)
        return akida_present()
            ? "AKD1000 PCI 1e7c:bca1 mapped (probe only; LIF still on CPU)"
            : akida_status();
    if (id == RX_BACKEND_LOIHI)
        return "Loihi is not a 2.1 backend (no interface in this tree)";
    return "unavailable";
}

bool rx_backend_set(int id) {
    if (id < 0 || id >= RX_BACKEND_COUNT) {
        g_status = "rejected: unknown backend id";
        return false;
    }
    if (!rx_backend_available(id)) {
        if (id == RX_BACKEND_AKIDA)
            g_status = "akida requested but not present — still on software";
        else if (id == RX_BACKEND_LOIHI)
            g_status = "loihi requested but not present — still on software";
        else
            g_status = "backend not available — still on software";
        return false;
    }
    g_id = id;
    if (id == RX_BACKEND_SOFTWARE)
        g_status = "software LIF on host CPU (active)";
    else if (id == RX_BACKEND_AKIDA)
        g_status = "akida PCI mapped (no spike offload yet; LIF on CPU)";
    else
        g_status = "loihi is not wired in 2.1";
    return true;
}

int rx_backend_cycle(bool skip_missing) {
    int start = g_id;
    for (int step = 1; step <= RX_BACKEND_COUNT; step++) {
        int next = (start + step) % RX_BACKEND_COUNT;
        if (skip_missing && !rx_backend_available(next)) continue;
        if (rx_backend_set(next) || !skip_missing) return g_id;
    }
    return g_id;
}
