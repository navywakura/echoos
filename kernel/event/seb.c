/*
 * Spiking Event Bus (SEB) — Implementation
 * Low-overhead, lock-free interrupt-to-spike translation engine.
 */
#include "seb.h"
#include "arch.h"
#include "arch.h"

/* Memory fence helper */
static inline void seb_mfence(void) {
    arch_barrier_full();
}

/* Static ring buffer in Heap-0 BSS */
static seb_spike_t g_seb_ring[SEB_RING_CAPACITY];
static volatile uint32_t g_seb_head = 0;
static volatile uint32_t g_seb_tail = 0;

static seb_stats_t g_seb_stats;
static uint32_t g_seq_counter = 0;

/* Waveform history for the top-bar Pulse Visualizer */
static int8_t g_waveform[SEB_WAVEFORM_LEN];
static uint32_t g_waveform_idx = 0;

static uint64_t g_last_rate_ticks = 0;
static uint32_t g_spikes_in_window = 0;
static uint32_t g_current_rate = 0;

/* Emit one spike and read it back. Only meaningful on a quiet bus: once the
   input and network actors are producing, the next spike out of the ring is
   whoever's turn it was, not ours, and this returns false for a bus that is
   working perfectly. So the boot-time answer is latched — that is the one
   worth reporting — and a later caller is told what it really measured. */
static int g_selftest_at_boot = -1;

bool seb_selftest(void) {
    seb_spike_t got;
    uint8_t pay[1] = { 0xA5 };
    bool ok;
    seb_emit_spike(SEB_CH_SYS, 1, 0, pay, 1);
    ok = seb_poll_spike(&got) && got.payload[0] == 0xA5;
    if (g_selftest_at_boot < 0) g_selftest_at_boot = ok ? 1 : 0;
    return ok;
}

int seb_selftest_at_boot(void) { return g_selftest_at_boot; }

void seb_init(void) {
    g_seb_head = 0;
    g_seb_tail = 0;
    g_seq_counter = 0;
    g_last_rate_ticks = arch_timer_ticks();
    g_spikes_in_window = 0;
    g_current_rate = 0;
    
    for (int i = 0; i < (int)SEB_CH_COUNT; i++) {
        g_seb_stats.channel_spikes[i] = 0;
    }
    g_seb_stats.total_spikes = 0;
    g_seb_stats.spikes_per_sec = 0;
    g_seb_stats.ring_overruns = 0;
    g_seb_stats.ring_occupancy = 0;

    for (int i = 0; i < (int)SEB_WAVEFORM_LEN; i++) {
        g_waveform[i] = 0;
    }
}

void seb_emit_spike(uint16_t channel, uint16_t amplitude, uint32_t event_id,
                    const uint8_t *payload, uint8_t len) {
    uint32_t h = g_seb_head;
    uint32_t t = g_seb_tail;

    if (h - t >= SEB_RING_CAPACITY) {
        g_seb_stats.ring_overruns++;
        return;
    }

    uint32_t slot = h & (SEB_RING_CAPACITY - 1);
    seb_spike_t *spk = &g_seb_ring[slot];

    spk->timestamp_us = arch_timer_uptime_ms() * 1000ULL;
    spk->channel = channel;
    spk->amplitude = amplitude ? amplitude : 100;
    spk->event_id = event_id ? event_id : ++g_seq_counter;

    for (int i = 0; i < 8; i++) {
        spk->payload[i] = (i < len && payload) ? payload[i] : 0;
    }

    seb_mfence();
    g_seb_head = h + 1;

    /* Update stats */
    g_seb_stats.total_spikes++;
    if (channel < SEB_CH_COUNT) {
        g_seb_stats.channel_spikes[channel]++;
    }
    g_spikes_in_window++;

    /* Update live waveform for pulse visualizer */
    int8_t wave_val = (int8_t)((amplitude > 127) ? 127 : (amplitude ? amplitude : 40));
    if ((g_seb_stats.total_spikes & 1) == 0) {
        wave_val = -wave_val; /* Alternating polarity for oscillatory wave */
    }
    g_waveform[g_waveform_idx & (SEB_WAVEFORM_LEN - 1)] = wave_val;
    g_waveform_idx++;

    /* Calculate rolling spike rate every 10 PIT ticks (~100ms) */
    uint64_t now_ticks = arch_timer_ticks();
    if (now_ticks >= g_last_rate_ticks + 10) {
        uint64_t dt = now_ticks - g_last_rate_ticks;
        if (dt > 0) {
            g_current_rate = (uint32_t)((uint64_t)g_spikes_in_window * 100ULL / dt);
        }
        g_seb_stats.spikes_per_sec = g_current_rate;
        g_spikes_in_window = 0;
        g_last_rate_ticks = now_ticks;
    }
}

bool seb_poll_spike(seb_spike_t *out) {
    uint32_t t = g_seb_tail;
    if (t == g_seb_head) {
        return false;
    }

    seb_mfence();
    uint32_t slot = t & (SEB_RING_CAPACITY - 1);
    if (out) {
        *out = g_seb_ring[slot];
    }
    seb_mfence();
    g_seb_tail = t + 1;
    return true;
}

uint64_t seb_total_spikes(void) {
    return g_seb_stats.total_spikes;
}

uint32_t seb_spike_rate_hz(void) {
    /* Auto decay if no spikes recently */
    uint64_t now_ticks = arch_timer_ticks();
    if (now_ticks > g_last_rate_ticks + 20) {
        g_current_rate = g_current_rate / 2;
        g_last_rate_ticks = now_ticks;
    }
    return g_current_rate;
}

void seb_get_stats(seb_stats_t *out) {
    if (!out) return;
    *out = g_seb_stats;
    out->spikes_per_sec = seb_spike_rate_hz();
    out->ring_occupancy = g_seb_head - g_seb_tail;
}

void seb_get_waveform(int8_t *out_samples, uint32_t count) {
    if (!out_samples || count == 0) return;
    uint32_t cur = g_waveform_idx;
    for (uint32_t i = 0; i < count; i++) {
        if (i < SEB_WAVEFORM_LEN) {
            uint32_t idx = (cur - count + i) & (SEB_WAVEFORM_LEN - 1);
            out_samples[i] = g_waveform[idx];
            /* Slow damping for idle state */
            if (g_waveform[idx] > 0) g_waveform[idx] = (int8_t)(g_waveform[idx] * 9 / 10);
            else if (g_waveform[idx] < 0) g_waveform[idx] = (int8_t)(g_waveform[idx] * 9 / 10);
        } else {
            out_samples[i] = 0;
        }
    }
}

void seb_reset_stats(void) {
    g_seb_stats.total_spikes = 0;
    g_seb_stats.ring_overruns = 0;
    for (int i = 0; i < (int)SEB_CH_COUNT; i++) {
        g_seb_stats.channel_spikes[i] = 0;
    }
}
