/* Serial console for AArch64.
 *
 * The PC console is a framebuffer with panes, a scrollback and a bitmap
 * face. None of that exists on this target yet, and pretending otherwise
 * would mean stubs that lie. This is what the board actually has: a PL011,
 * eighty columns by convention, and the same console_* contract the rest of
 * the kernel calls — so klog, the self-tests and the shell work unchanged.
 */
#include "rogex.h"
#include "pl011.h"
#include "scheduler.h"

static bool g_ready;

void console_init(void) {
    pl011_init();
    g_ready = true;
}

void console_putc(char c) {
    if (!g_ready) pl011_init();
    if (c == '\n') pl011_putc('\r');
    pl011_putc(c);
}

void console_write(const char *s) { while (s && *s) console_putc(*s++); }
void console_serial_write(const char *s) { console_write(s); }

void console_write_dec(uint64_t v) {
    char t[21];
    int i = 0;
    if (!v) { console_putc('0'); return; }
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) console_putc(t[--i]);
}

void console_write_hex(uint64_t v) {
    static const char H[] = "0123456789abcdef";
    int sh;
    console_write("0x");
    for (sh = 60; sh >= 0; sh -= 4) console_putc(H[(v >> sh) & 0xF]);
}

void console_clear(void) { console_write("\033[2J\033[H"); }

bool console_serial_has_input(void) { return pl011_has_input(); }
int  console_serial_getc_nb(void)   { return pl011_getc_nb(); }
bool console_has_input(void)        { return pl011_has_input(); }

/* Waiting for a key is not a reason for the rest of the system to stop.
   The scheduler is cooperative, so a spin here starves every other task —
   which is exactly what made a PX4 link accumulate losses at the prompt
   while the machine looked idle. Yielding hands the CPU to whoever else is
   runnable and comes back when they are done. */
char console_getc(void) {
    int c;
    for (;;) {
        c = pl011_getc_nb();
        if (c >= 0) return (char)c;
        sched_yield();
        __asm__ volatile ("wfe");
    }
}

/* Geometry: a serial terminal's size is the terminal's business, not ours.
   Eighty by twenty-five is the honest assumption, and it is stated rather
   than measured. */
int console_cols(void) { return 80; }
int console_rows(void) { return 25; }

/* No scrollback on a serial line: the terminal on the other end has one. */
void console_scroll(int rows)     { (void)rows; }
void console_scroll_follow(void)  { }
int  console_scroll_offset(void)  { return 0; }
int  console_scroll_max(void)     { return 0; }

void console_write_prompt(const char *user, const char *host, const char *cwd) {
    console_write(user ? user : "live");
    console_putc('@');
    console_write(host ? host : "echos");
    console_putc(' ');
    console_write(cwd ? cwd : "/");
    console_write("# ");
}

int console_set_theme(const char *name) { (void)name; return 0; }
const char *console_theme_name(void) { return "serial"; }
