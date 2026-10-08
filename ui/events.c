/*
 * Input pump: fabric SPSC ring → UI. hlt when idle.
 * Falls back to polling if IRQ input never came up.
 */
#include "rogex.h"
#include "arch.h"
#include "pane_monitor.h"
#include "mavbridge.h"
#include "framebuffer.h"
#include "mouse.h"
#include "desktop.h"
#include "rx_event.h"
#include "scheduler.h"
#include "power.h"
#include "commands.h"
#include "arch.h"
#include "cli.h"

static bool g_irq_mode;                 /* consume IPC events + hlt idle    */

void events_use_irq(bool on) { g_irq_mode = on; }
bool events_irq_active(void) { return g_irq_mode; }

/* Idle-time service hook: called on every input-wait iteration so background
   work (e.g. the Rogex Wired network — answering PINGs while the shell sits
   at its prompt) gets serviced without its own thread. The PIT wakes the CPU
   from hlt ~every 10 ms, bounding the service latency. */
static void (*g_idle_hook)(void);
void events_set_idle_hook(void (*h)(void)) { g_idle_hook = h; }
static inline void run_idle_hook(void) {
    /* A monitor pane is redrawn from here: between keystrokes is
       exactly when the shell is not using the CPU. */
    /* The MAVLink link is pumped from here too: a control link cannot wait
       for the user to press a key. */
    mavbridge_poll();
    pane_monitor_tick();
    if (g_idle_hook) g_idle_hook();
}

/* One input event, normalized across both sources. */
typedef struct {
    int  kind;                          /* 0 none, 1 char, 2 mouse          */
    char ch;
    int  dx, dy, buttons;
} in_event;

/* One event of pushback. The coalescing reader below has to stop the moment it
   sees something that is not more of the same motion, and the event it stopped
   on must not be lost — so it goes here and the next read takes it first. */
static in_event g_pushback;
static bool     g_have_pushback;

/* Serial CSI: ESC [ ...  (PgUp, arrows, SGR mouse wheel). */
static int      g_esc;          /* 0 idle, 1 ESC, 2 CSI */
static char     g_escb[28];
static int      g_escn;
static uint64_t g_esc_t;

static int csi_int_at(int *pos) {
    int v = 0, n = *pos;
    while (n < g_escn && g_escb[n] >= '0' && g_escb[n] <= '9')
        v = v * 10 + (g_escb[n++] - '0');
    *pos = n;
    return v;
}

static bool csi_finish(in_event *ev, int term) {
    int pos = 0, btn;
    g_esc = 0;
    if (term == 'A') {
        ev->kind = 1;
        ev->ch = (g_escn >= 3 && g_escb[0]=='1' && g_escb[2]=='5')
                     ? (char)KEY_PGUP : (char)KEY_UP;
        return true;
    }
    if (term == 'B') {
        ev->kind = 1;
        ev->ch = (g_escn >= 3 && g_escb[0]=='1' && g_escb[2]=='5')
                     ? (char)KEY_PGDN : (char)KEY_DOWN;
        return true;
    }
    if (term == 'C') { ev->kind = 1; ev->ch = (char)KEY_RIGHT; return true; }
    if (term == 'D') { ev->kind = 1; ev->ch = (char)KEY_LEFT; return true; }
    if (term == '~') {
        int n = csi_int_at(&pos);
        if (n == 5) { ev->kind = 1; ev->ch = (char)KEY_PGUP; return true; }
        if (n == 6) { ev->kind = 1; ev->ch = (char)KEY_PGDN; return true; }
        return false;
    }
    /* SGR mouse: <btn;x;y M/m   64 = wheel up, 65 = wheel down */
    if ((term == 'M' || term == 'm') && g_escn > 0 && g_escb[0] == '<') {
        pos = 1;
        btn = csi_int_at(&pos);
        ev->kind = 2; ev->dx = 0; ev->dy = 0; ev->buttons = 0;
        if (btn == 64) { ev->buttons = 0x08; return true; }
        if (btn == 65) { ev->buttons = 0x10; return true; }
        return false;
    }
    return false;
}

/* Non-blocking: fetch the next input event from serial, the IPC queue (IRQ
   mode) or the raw drivers (polled mode). */
static bool next_event(in_event *ev) {
    if (g_have_pushback) { *ev = g_pushback; g_have_pushback = false; return true; }

    if (g_esc && arch_timer_ticks() - g_esc_t > 8) {   /* ~80 ms: lone ESC */
        g_esc = 0;
        ev->kind = 1; ev->ch = 0x1b;
        return true;
    }

    int c = console_serial_getc_nb();
    if (c >= 0) {
        if (g_esc == 0) {
            if (c == 0x1b) { g_esc = 1; g_esc_t = arch_timer_ticks(); return false; }
            ev->kind = 1; ev->ch = (char)c; return true;
        }
        if (g_esc == 1) {
            if (c == '[') { g_esc = 2; g_escn = 0; g_esc_t = arch_timer_ticks(); return false; }
            g_esc = 0;
            ev->kind = 1; ev->ch = (char)c;
            return true;
        }
        if (c >= 0x40 && c <= 0x7e) return csi_finish(ev, c);
        if (g_escn < (int)sizeof(g_escb) - 1) g_escb[g_escn++] = (char)c;
        g_esc_t = arch_timer_ticks();
        return false;
    }

    if (g_irq_mode) {
        rx_input_t in;
        if (!rx_input_pop(&in)) return false;         /* fabric input ring    */
        if (in.kind == RX_INPUT_KEY) {
            ev->kind = 1; ev->ch = in.ch;
            return true;
        }
        if (in.kind == RX_INPUT_MOUSE) {
            ev->kind = 2; ev->dx = in.dx; ev->dy = in.dy; ev->buttons = in.buttons;
            return true;
        }
        return false;
    }

    /* Polled fallback. */
    c = kbd_poll_char();
    if (c >= 0) { ev->kind = 1; ev->ch = (char)c; return true; }
    if (fb_active() && mouse_present()) {
        int dx, dy, btn;
        if (mouse_poll(&dx, &dy, &btn)) {
            ev->kind = 2; ev->dx = dx; ev->dy = dy; ev->buttons = btn;
            return true;
        }
    }
    return false;
}

/* Idle until something can have happened.
   Order matters: first give the CPU to any other runnable task (the
   cooperative scheduler — this is what makes the shell "a task among
   others"); only when nobody else can run do we park the core in the
   deepest idle state the machine supports (power_idle: MWAIT with a
   C-state hint where available, otherwise HLT). Any interrupt wakes it —
   input immediately, the PIT within 10 ms, which also bounds serial-poll
   latency. In polled mode we just yield the pipeline. */
static void idle_wait(void) {
    /* The one place the OS goes to sleep waiting for input, and therefore the
       correct frame boundary: anything drawn into the backbuffer has to reach
       the screen BEFORE we park the core, or it sits in RAM until something
       else happens to present.

       This was the `neuro` bug on real hardware. neuro_render() drew a full
       frame and then called ui_poll(), which idled here without presenting,
       so the screen never changed — the demo looked frozen and only "woke up"
       on Esc, because returning to the shell presented. The editor and every
       other ui_poll/ui_line screen had the same fault. Presenting here fixes
       all of them at once instead of per caller. */
    fb_present();
    sched_yield();
    if (g_irq_mode) power_idle();
    else            arch_relax();
}

/* Apply a mouse event to the cursor; returns a clicked hit id (>=0) on a
   left-button release over a hitbox, else -1. */
static bool g_prev_left, g_prev_right;

static int mouse_apply(const in_event *ev, bool clicks) {
    uint32_t cx, cy;
    fb_cursor_get(&cx, &cy);
    if (ev->dx || ev->dy) fb_cursor_moveto((int)cx + ev->dx, (int)cy + ev->dy);
    bool left = (ev->buttons & 0x01) != 0;
    int id = -1;
    if (clicks && g_prev_left && !left) {          /* click = press+release  */
        fb_cursor_get(&cx, &cy);
        id = render_hit_test(cx, cy);
    }
    g_prev_left = left;
    return id;
}

/* Unified line reader: echo + backspace; while waiting, the mouse cursor
   moves. With `allow_click`, a click over a button returns its id (>0);
   otherwise 0 with the line in `buf`. Cursor visible only during the wait. */
int ui_line(char *buf, size_t max, bool allow_click) {
    size_t n = 0;
    bool cli = echos2_cli_only();
    bool cursor = fb_active() && mouse_present();
    uint64_t caret_t = arch_timer_ticks();
    int caret_on = 1;
    int hist_pos = 0;                   /* 0 = live line, 1 = last cmd, ... */
    g_prev_left = false;
    g_prev_right = false;
    if (cursor) fb_cursor_show();
    else if (fb_active()) fb_cursor_hide();
    if (fb_active()) console_caret_sync(1);

    for (;;) {
        in_event ev;
        if (!next_event(&ev)) {
            uint64_t now = arch_timer_ticks();
            if (now - caret_t >= 50) {          /* 500 ms at PIT 100 Hz */
                caret_t = now;
                caret_on = !caret_on;
                if (fb_active()) console_caret_sync(caret_on);
            }
            run_idle_hook();
            idle_wait();
            continue;
        }

        if (ev.kind == 2) {
            /* Wheel: bit3 = up, bit4 = down. */
            if (ev.buttons & 0x08) { console_scroll(3); continue; }
            if (ev.buttons & 0x10) { console_scroll(-3); continue; }
            if (cli && cursor) {
                uint32_t cx, cy;
                int right = (ev.buttons & 0x02) != 0;
                fb_cursor_get(&cx, &cy);
                if (ev.dx || ev.dy) fb_cursor_moveto((int)cx + ev.dx, (int)cy + ev.dy);
                fb_cursor_get(&cx, &cy);
                console_sel_pointer((int)cx, (int)cy, ev.buttons);
                if (right && !g_prev_right) {
                    const char *p = console_clipboard();
                    while (p && *p && n + 1 < max) {
                        if (*p == '\n' || *p == '\r') break;
                        buf[n++] = *p;
                        console_putc(*p++);
                    }
                    buf[n] = '\0';
                    caret_on = 1; caret_t = arch_timer_ticks();
                    if (fb_active()) console_caret_sync(1);
                }
                g_prev_right = right;
                continue;
            }
            if (!cli && cursor) {
                int id = mouse_apply(&ev, allow_click);
                if (id >= 0) { fb_cursor_hide(); if (fb_active()) fb_caret(0); buf[0] = '\0'; return id; }
            }
            continue;
        }

        if (ev.kind == 1) {
            char c = ev.ch;
            unsigned char uc = (unsigned char)c;
            if (cursor) fb_cursor_hide();
            if (fb_active()) console_caret_sync(0);
            if (uc == KEY_ALTTAB) { console_pane_next(); continue; }
            if (uc == KEY_PGUP) { console_scroll(8); continue; }
            if (uc == KEY_PGDN) { console_scroll(-8); continue; }
            if (uc == 3) { console_sel_copy(); continue; }          /* Ctrl+C copy */
            if (uc == 22) {                                        /* Ctrl+V paste */
                const char *p = console_clipboard();
                while (p && *p && n + 1 < max) {
                    if (*p == '\n' || *p == '\r') break;
                    buf[n++] = *p;
                    console_putc(*p++);
                }
                buf[n] = '\0';
                caret_on = 1; caret_t = arch_timer_ticks();
                if (fb_active()) console_caret_sync(1);
                continue;
            }
            if (console_scroll_offset()) console_scroll_follow();
            if (uc == KEY_UP || uc == KEY_DOWN) {
                int count = commands_hist_count();
                const char *h;
                size_t t;
                if (uc == KEY_UP) {
                    if (hist_pos < count) hist_pos++;
                } else {
                    if (hist_pos > 0) hist_pos--;
                }
                h = hist_pos ? commands_hist_from_end(hist_pos) : "";
                while (n > 0) { n--; console_write("\b \b"); }
                if (h) for (t = 0; h[t] && n + 1 < max; t++) {
                    buf[n++] = h[t];
                    console_putc(h[t]);
                }
                buf[n] = '\0';
                caret_on = 1; caret_t = arch_timer_ticks();
                if (fb_active()) console_caret_sync(1);
                continue;
            }
            if (c == '\r' || c == '\n') { console_putc('\n'); buf[n] = '\0'; return 0; }
            if (c == 0x7f || uc == 0x08) {
                if (n > 0) { n--; console_write("\b \b"); }
            } else if (c == '\t') {
                char ins[64];
                char extra[16][48];
                int nm = commands_tab_complete(buf, n, ins, sizeof(ins), extra, 16);
                size_t t;
                for (t = 0; ins[t] && n + 1 < max; t++) {
                    buf[n++] = ins[t];
                    console_putc(ins[t]);
                }
                if (nm > 1 && !ins[0]) {
                    int i;
                    console_putc('\n');
                    for (i = 0; i < nm && i < 16; i++) {
                        console_write(extra[i]);
                        console_write("  ");
                    }
                    console_putc('\n');
                    for (t = 0; t < n; t++) console_putc(buf[t]);
                }
            } else if (uc >= 0x20 && uc != 0x7f && n + 1 < max) {
                buf[n++] = c;
                console_putc(c);
            }
            caret_on = 1;
            caret_t = arch_timer_ticks();
            if (fb_active()) console_caret_sync(1);
            if (cursor) fb_cursor_show();
            continue;
        }
        if (ev.kind == 2 && cursor) {
            int id = mouse_apply(&ev, allow_click);
            if (id >= 0) { fb_cursor_hide(); if (fb_active()) fb_caret(0); buf[0] = '\0'; return id; }
        }
    }
}

size_t console_readline(char *buf, size_t max) {
    ui_line(buf, max, false);
    return k_strlen(buf);
}

/* Non-blocking single poll for interactive apps (the editor). Moves the
   cursor. Returns: 0 nothing, 1 = char typed (in *ch), 2 = button clicked
   (in *click). The caller manages cursor show/hide around its redraws. */
int ui_poll(char *ch, int *click) {
    in_event ev;
    if (!next_event(&ev)) { run_idle_hook(); idle_wait(); return 0; }
    if (ev.kind == 1) { if (ch) *ch = ev.ch; return 1; }
    if (ev.kind == 2 && fb_active() && mouse_present()) {
        int id = mouse_apply(&ev, true);
        if (id >= 0) { if (click) *click = id; return 2; }
    }
    return 0;
}

/* Cursor pixel position (for the editor to map clicks to text columns). */
void ui_cursor_xy(uint32_t *x, uint32_t *y) { fb_cursor_get(x, y); }

/* Raw non-blocking input for the window manager, which does its own cursor
   movement and hit-testing (drag needs raw mouse deltas, not hitbox clicks).
   Works in both IRQ and polled modes. Returns 0 nothing / 1 char (in *ch) /
   2 mouse (raw *dx,*dy,*buttons). Also runs the idle hook so the network is
   still serviced from inside the WM. */
int ui_next_raw(int *ch, int *dx, int *dy, int *buttons) {
    in_event ev;
    /* About to report "nothing pending", i.e. the caller is going to idle:
       flush whatever it drew so the frame is complete before we sleep. */
    if (!next_event(&ev)) { fb_present(); run_idle_hook(); return 0; }
    if (ev.kind == 1) { if (ch) *ch = (int)(unsigned char)ev.ch; return 1; }
    if (ev.kind == 2) {
        /* Coalesce the motion burst. The mouse streams at 200 Hz, and the
           window manager answers a motion event by recomposing the scene —
           so one physical flick of the wrist asked for dozens of full frames
           the machine could not deliver, which is what made dragging stutter
           and the pointer lag behind the hand.

           Consecutive motion carrying the SAME button state is summed into
           one event: the endpoint is identical and the intermediate frames
           were never going to be shown. Coalescing stops at anything else —
           a key, or any change of button state — so no press or release is
           ever folded away and a click is never lost. */
        int adx = ev.dx, ady = ev.dy;
        const int btn = ev.buttons;
        for (;;) {
            in_event nx;
            if (!next_event(&nx)) break;
            if (nx.kind != 2 || nx.buttons != btn) {
                g_pushback = nx; g_have_pushback = true;
                break;
            }
            adx += nx.dx; ady += nx.dy;
        }
        if (dx) *dx = adx;
        if (dy) *dy = ady;
        if (buttons) *buttons = btn;
        return 2;
    }
    return 0;
}

int ui_next_key_nb(int *ch) {
    in_event ev;
    if (!next_event(&ev)) return 0;
    if (ev.kind == 1) {
        if (ch) *ch = (int)(unsigned char)ev.ch;
        return 1;
    }
    g_pushback = ev;
    g_have_pushback = true;
    return 0;
}
