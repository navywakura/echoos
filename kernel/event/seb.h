/*
 * Spiking Event Bus (SEB) — EchOS 1.0
 * Event-driven hardware interrupt translation into binary pulse packets (Spikes).
 * Prepares the OS for neuromorphic/Akida hardware acceleration while running
 * bare-metal event loops with zero polling latency.
 */
#ifndef RXOS_SEB_H
#define RXOS_SEB_H

#include "rogex.h"

#define SEB_CH_TIMER     0x01u  /* PIT / APIC / timer tick spikes */
#define SEB_CH_KEYBOARD  0x02u  /* PS/2 & USB keyboard pulse events */
#define SEB_CH_MOUSE     0x03u  /* Mouse coordinate/button spikes */
#define SEB_CH_PCI       0x04u  /* PCI bus & device interrupts */
#define SEB_CH_NET       0x05u  /* VirtIO / E1000 network frame spikes */
#define SEB_CH_WSP       0x06u  /* RogexWSP 16-byte protocol spikes */
#define SEB_CH_SYS       0x07u  /* Scheduler / power / thermal events */
#define SEB_CH_COUNT     8u

#define SEB_RING_CAPACITY 1024u
#define SEB_WAVEFORM_LEN  64u

/* 16-byte aligned binary spike packet */
typedef struct __attribute__((packed, aligned(16))) {
    uint64_t timestamp_us;    /* Kernel timestamp in microseconds */
    uint16_t channel;         /* SEB_CH_* source identifier */
    uint16_t amplitude;       /* Spike energy / amplitude in millivolts */
    uint32_t event_id;        /* Monotonic spike sequence number */
    uint8_t  payload[8];      /* Fast 8-byte inline pulse payload */
} seb_spike_t;

/* SEB Statistics */
typedef struct {
    uint64_t total_spikes;
    uint32_t spikes_per_sec;
    uint64_t channel_spikes[SEB_CH_COUNT];
    uint32_t ring_overruns;
    uint32_t ring_occupancy;
} seb_stats_t;

/* Public SEB API */
void     seb_init(void);
bool     seb_selftest(void);  /* emit + poll one SYS spike; consumes it */
/* What the self-test said the first time it ran, which is at boot on an idle
   bus. -1 if it has never run. Re-running it on a live bus proves nothing:
   another producer's spike comes out of the ring first. */
int      seb_selftest_at_boot(void);
void     seb_emit_spike(uint16_t channel, uint16_t amplitude, uint32_t event_id,
                        const uint8_t *payload, uint8_t len);
bool     seb_poll_spike(seb_spike_t *out);
uint64_t seb_total_spikes(void);
uint32_t seb_spike_rate_hz(void);
void     seb_get_stats(seb_stats_t *out);
void     seb_get_waveform(int8_t *out_samples, uint32_t count);
void     seb_reset_stats(void);

#endif /* RXOS_SEB_H */
