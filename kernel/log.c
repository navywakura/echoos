/* Simple kernel log lines. Always mirrored to COM1 by the terminal driver. */
#include "rogex.h"

void klog_info(const char *msg) {
    console_write("[echOS] ");
    console_write(msg);
    console_putc('\n');
}

void klog_warn(const char *msg) {
    console_write("[echOS] WARN: ");
    console_write(msg);
    console_putc('\n');
}

void klog_error(const char *msg) {
    console_write("[echOS] ERROR: ");
    console_write(msg);
    console_putc('\n');
}

/* Boot-banner status line, e.g.  "GDT: OK"  /  "Scheduler: STUB". */
void klog_status(const char *label, const char *status) {
    console_write(label);
    console_write(": ");
    console_write(status);
    console_putc('\n');
}

/* Unsigned decimal on the boot console. The banner needs counts (memory
   ranges, modules) and there is no printf down here. */
void klog_dec(uint64_t v) {
    char t[21];
    int i = 0;
    if (!v) { console_putc('0'); return; }
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) console_putc(t[--i]);
}
