/* PIT at 100 Hz on IRQ0. uptime lives here. */
#include "pit.h"
#include "irq.h"
#include "ports.h"

#define PIT_CH0   0x40
#define PIT_MODE  0x43
#define PIT_INPUT 1193182u         /* PIT input clock in Hz */

#include "seb.h"

static volatile uint64_t g_ticks;

static void pit_irq(void) {
    g_ticks++;
    seb_emit_spike(SEB_CH_TIMER, 50, (uint32_t)g_ticks, (const uint8_t *)&g_ticks, 8);
}

uint64_t pit_ticks(void)     { return g_ticks; }
uint64_t pit_uptime_ms(void) { return g_ticks * (1000u / PIT_HZ); }

bool pit_init(void) {
    uint16_t divisor = (uint16_t)(PIT_INPUT / PIT_HZ);
    outb(PIT_MODE, 0x36);                       /* ch0, lo/hi, mode 3 */
    outb(PIT_CH0, (uint8_t)(divisor & 0xff));
    outb(PIT_CH0, (uint8_t)(divisor >> 8));
    irq_register(0, pit_irq);

    /* Verify the tick actually advances (bounded by a TSC-based timeout so a
       broken interrupt path reports FAIL instead of hanging the boot). */
    uint64_t start = g_ticks;
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc0 = ((uint64_t)hi << 32) | lo;
    for (;;) {
        if (g_ticks != start) return true;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t tsc = ((uint64_t)hi << 32) | lo;
        if (tsc - tsc0 > 2000000000ull) return false;   /* ~1 s at 2 GHz */
        __asm__ volatile ("pause");
    }
}
