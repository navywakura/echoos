/* The generic timer, programmed as a periodic tick.
 *
 * CNTFRQ_EL0 tells us the counter's frequency, so unlike the x86 TSC there
 * is nothing to calibrate and nothing to discipline: the counter is defined
 * to be constant-rate. That is the whole reason the monotonic clock on this
 * target is three lines instead of a watchdog.
 */
#include "atimer.h"
#include "gic.h"

#define TIMER_IRQ 30                   /* PPI 14 -> INTID 30, EL1 physical  */

static uint64_t g_freq;
static volatile uint64_t g_ticks;
static uint64_t g_interval;

static inline uint64_t cntpct(void) {
    uint64_t v;
    __asm__ volatile ("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline void set_tval(uint64_t v) {
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(v));
}

void atimer_irq(void) {
    g_ticks++;
    set_tval(g_interval);              /* re-arm for the next period       */
}

bool atimer_init(void) {
    uint64_t start;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(g_freq));
    if (!g_freq) return false;
    g_interval = g_freq / ATIMER_HZ;
    g_ticks = 0;

    gic_register(TIMER_IRQ, atimer_irq);
    set_tval(g_interval);
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(1ull));   /* enable     */
    gic_enable(TIMER_IRQ);

    /* Same contract as the x86 side: report OK only if it really advances. */
    start = g_ticks;
    for (uint64_t guard = 0; guard < 200000000ull; guard++) {
        if (g_ticks != start) return true;
        __asm__ volatile ("yield");
    }
    return false;
}

uint64_t atimer_ticks(void) { return g_ticks; }
uint64_t atimer_freq(void)  { return g_freq; }

uint64_t atimer_uptime_ms(void) {
    /* From the counter, not from the tick count: an interrupt that was
       masked for a while must not make time stand still. */
    if (!g_freq) return 0;
    return (cntpct() * 1000ull) / g_freq;
}
