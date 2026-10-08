/* Neuromorphic compute backend: software LIF today, NPU plugs tomorrow. */
#ifndef RXOS_RX_BACKEND_H
#define RXOS_RX_BACKEND_H

#include "rogex.h"

/* Which silicon (or software stand-in) runs the event fabric's LIF math.
   Software is always available. Akida / Loihi are real product lines we
   intend to drive; until a board is probed they stay honest stubs so the
   user can switch and see the OS refuse gracefully. */
typedef enum {
    RX_BACKEND_SOFTWARE = 0,   /* host CPU, Q16.16 LIF (what ships today) */
    RX_BACKEND_AKIDA    = 1,   /* BrainChip Akida — PCI/USB NPU (stub)     */
    RX_BACKEND_LOIHI    = 2,   /* Intel Loihi research NPU (stub)         */
    RX_BACKEND_COUNT
} rx_backend_id;

void        rx_backend_init(void);
int         rx_backend_get(void);
const char *rx_backend_name(void);          /* short id: "software", "akida" */
const char *rx_backend_label(void);         /* human: "Software LIF (CPU)"   */
const char *rx_backend_status(void);        /* one-line health / why         */
bool        rx_backend_available(int id);   /* true if we can activate it    */
const char *rx_backend_avail_why(int id);   /* why available or not          */

/* Select backend. Returns true only if the switch stuck. Requesting a
   missing NPU leaves the previous backend and explains itself in status. */
bool        rx_backend_set(int id);
/* Cycle software → akida → loihi → software (skips unavailable only when
   skip_missing is true; otherwise lands on the stub and reports). */
int         rx_backend_cycle(bool skip_missing);

/* Fabric accounting the UI can show so the user *feels* event-driven idle. */
uint64_t    rx_backend_spikes(void);        /* total LIF fires observed      */
uint64_t    rx_backend_idle_ticks(void);    /* PIT ticks spent in power_idle */
void        rx_backend_note_spike(void);
void        rx_backend_note_idle(void);

#endif /* RXOS_RX_BACKEND_H */
