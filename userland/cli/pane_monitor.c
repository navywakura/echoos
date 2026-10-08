/*
 * The live pane: a system card that redraws itself while the shell waits.
 *
 * There is one shell task, so a second pane cannot hold a second prompt.
 * What it can hold is something the system already knows how to update: the
 * event loop calls the idle hook between keystrokes, and this paints a
 * monitor into whichever pane asked to be one. That is the multitasking this
 * architecture can honestly do today — a view that keeps moving while you
 * type in another pane, not two shells.
 */
#include "pane_monitor.h"
#include "rogex.h"
#include "arch.h"
#include "clock.h"
#include "seb.h"
#include "scheduler.h"
#include "power.h"
#include "netif.h"
#include "disk.h"
#include "rx_backend.h"

static uint64_t g_last_tick;
static uint32_t g_refreshes;

static char *put(char *d, const char *e, const char *s) {
    while (*s && d < e - 1) *d++ = *s++;
    *d = '\0';
    return d;
}

static char *put_dec(char *d, const char *e, uint64_t v) {
    char t[21];
    int i = 0;
    if (!v) return put(d, e, "0");
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0 && d < e - 1) *d++ = t[--i];
    *d = '\0';
    return d;
}

/* Everything on this card is read at the moment of drawing. Nothing here is
   remembered from the last refresh, so a frozen number means a frozen
   counter, not a stale cache. */
void pane_monitor_refresh(void) {
    int id = console_pane_first_of_kind(CONSOLE_PANE_MONITOR);
    char buf[1024];
    char *d = buf, *e = buf + sizeof(buf);
    clock_tm now;
    seb_stats_t seb;

    if (id < 0) return;
    console_pane_clear(id);
    clock_now(&now);
    seb_get_stats(&seb);

    d = put(d, e, "\033[1;36m echOS monitor\033[0m\n");
    d = put(d, e, " \033[1mclock\033[0m   ");
    d = put_dec(d, e, (uint64_t)now.hour); d = put(d, e, ":");
    if (now.min < 10) d = put(d, e, "0");
    d = put_dec(d, e, (uint64_t)now.min); d = put(d, e, ":");
    if (now.sec < 10) d = put(d, e, "0");
    d = put_dec(d, e, (uint64_t)now.sec);
    d = put(d, e, "\n \033[1muptime\033[0m  ");
    d = put_dec(d, e, arch_timer_uptime_ms() / 1000); d = put(d, e, " s\n");
    d = put(d, e, " \033[1mticks\033[0m   ");
    d = put_dec(d, e, arch_timer_ticks());
    d = put(d, e, "\n \033[1mspikes\033[0m  ");
    d = put_dec(d, e, seb.total_spikes);
    d = put(d, e, "  ring ");
    d = put_dec(d, e, (uint64_t)seb.ring_occupancy);
    d = put(d, e, "\n \033[1mtasks\033[0m   ");
    d = put_dec(d, e, (uint64_t)scheduler_task_count());
    d = put(d, e, "  switches ");
    d = put_dec(d, e, sched_switches());
    d = put(d, e, "\n \033[1mmemory\033[0m  ");
    d = put_dec(d, e, pmm_used_bytes() / (1024 * 1024));
    d = put(d, e, " / ");
    d = put_dec(d, e, pmm_total_bytes() / (1024 * 1024));
    d = put(d, e, " MiB\n \033[1midle\033[0m    ");
    d = put_dec(d, e, power_idle_entries());
    d = put(d, e, " parks\n \033[1mdisk\033[0m    ");
    d = put(d, e, disk_present() ? disk_name() : "none");
    d = put(d, e, "\n \033[1mnet\033[0m     ");
    d = put(d, e, net_present() ? net_driver_name() : "none");
    d = put(d, e, "\n \033[1msnn\033[0m     ");
    d = put(d, e, rx_backend_label());
    d = put(d, e, "\n \n\033[2m Ctrl+Tab to focus  ·  pane shell to stop\033[0m\n");

    console_pane_write(id, buf);
    g_last_tick = arch_timer_ticks();
    g_refreshes++;
}

/* Called from the input idle path. Ten times a second is enough to look
   alive and cheap enough not to compete with the shell for the CPU. */
void pane_monitor_tick(void) {
    if (console_pane_first_of_kind(CONSOLE_PANE_MONITOR) < 0) return;
    if (arch_timer_ticks() - g_last_tick < 10) return;
    pane_monitor_refresh();
}

uint32_t pane_monitor_refreshes(void) { return g_refreshes; }
