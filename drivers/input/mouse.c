/*
 * PS/2 mouse. IRQ12 posts into the event fabric ring; polled path as backup.
 * Shares i8042 with the keyboard (status bit 5). Scroll wheel supported
 * via the Intellimouse 4-byte packet mode (buttons bit3=up, bit4=down).
 */
#include "mouse.h"
#include "irq.h"
#include "rx_event.h"
#include "seb.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

static inline uint8_t inb(uint16_t p) { uint8_t r; __asm__ volatile ("inb %1,%0" : "=a"(r) : "Nd"(p)); return r; }
static inline void    outb(uint16_t p, uint8_t v) { __asm__ volatile ("outb %0,%1" : : "a"(v), "Nd"(p)); }

static bool g_present;
static uint8_t g_pkt[4];
static int g_idx;
static int g_wheel;                 /* 0 = 3-byte mode, 1 = 4-byte wheel mode */
static int g_desync;                /* consecutive bad byte0 at idx 0         */

/* Wait until the controller can accept a byte (input buffer empty). */
static void wait_in(void)  { for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & 0x02)) return; }
/* Wait until there is a byte to read (output buffer full). */
static void wait_out(void) { for (int i = 0; i < 100000; i++) if (inb(PS2_STATUS) & 0x01) return; }

/* Send a command byte to the mouse (aux) and swallow its ACK (0xFA). */
static void mouse_write(uint8_t v) {
    wait_in(); outb(PS2_CMD, 0xD4);      /* next byte targets the aux device */
    wait_in(); outb(PS2_DATA, v);
    wait_out(); (void)inb(PS2_DATA);      /* ACK */
}

void mouse_init(void) {
    g_present = false; g_idx = 0;

    wait_in(); outb(PS2_CMD, 0xA8);       /* enable the auxiliary device */

    /* Enable IRQ12 + aux clock in the controller config byte (we still poll,
       but this makes the controller route mouse bytes to the output buffer). */
    wait_in(); outb(PS2_CMD, 0x20);       /* read config byte */
    wait_out();
    uint8_t cfg = inb(PS2_DATA);
    cfg |= 0x02;                          /* enable IRQ12 (bit 1)            */
    cfg &= ~0x20;                         /* clear aux clock disable (bit 5) */
    wait_in(); outb(PS2_CMD, 0x60);       /* write config byte */
    wait_in(); outb(PS2_DATA, cfg);

    mouse_write(0xF6);                    /* set defaults */
    /* IMPS/2 (Intellimouse) enable: sample rates 200, 100, 80, then Get
       Device ID (0xF2 — not 0xF9). 0x03 = 4-byte wheel packets. A 3-byte
       mouse stays at 0x00; forcing 4-byte mode on those desyncs the stream. */
    mouse_write(0xF3); mouse_write(200);
    mouse_write(0xF3); mouse_write(100);
    mouse_write(0xF3); mouse_write(80);
    for (int i = 0; i < 16 && (inb(PS2_STATUS) & 0x01); i++) (void)inb(PS2_DATA);
    mouse_write(0xF2);                    /* Get Device ID; mouse_write ate ACK */
    wait_out();
    {
        uint8_t id = inb(PS2_DATA);
        if (id == 0xFA) { wait_out(); id = inb(PS2_DATA); }
        g_wheel = (id == 3 || id == 4 || id == 5) ? 1 : 0;
    }
    mouse_write(0xF4);                    /* enable data reporting (streaming) */
    g_present = true;
}

bool mouse_has_wheel(void) { return g_present && g_wheel; }

bool mouse_present(void) { return g_present; }

/* Feed one aux byte into the 3-byte packet assembler. Returns true when a
   complete packet was decoded into dx/dy/buttons. Shared by both paths. */
static bool mouse_feed(uint8_t b, int *dx, int *dy, int *buttons) {
    if (g_idx == 0 && !(b & 0x08)) {
        /* bit3 missing at packet start: either a plain 3-byte mouse while we
           are in wheel mode (stream desync — phantom clicks, random app
           launches) or controller noise. Fall back to 3-byte after a few. */
        if (g_wheel && ++g_desync > 8) g_wheel = 0;
        return false;
    }
    g_desync = 0;
    g_pkt[g_idx++] = b;
    int want = g_wheel ? 4 : 3;
    if (g_idx < want) return false;
    g_idx = 0;

    uint8_t f = g_pkt[0];
    int mx = g_pkt[1];
    int my = g_pkt[2];
    if (f & 0x10) mx -= 256;              /* sign-extend from the flag bits */
    if (f & 0x20) my -= 256;
    if (dx) *dx = mx;
    if (dy) *dy = -my;                    /* PS/2 y is up-positive; screen is down */
    int btn = f & 0x07;
    if (g_wheel) {
        int w = (int8_t)g_pkt[3];
        if (w >= 8 && w <= 15) w -= 16;   /* IMPS/2 nibble-only (no high bits) */
        if (w > 0) btn |= 0x08;           /* wheel up   -> older scrollback    */
        else if (w < 0) btn |= 0x10;      /* wheel down -> follow live         */
    }
    if (buttons) *buttons = btn;
    return true;
}

/* Polling path (fallback; also used before IRQ setup). */
bool mouse_poll(int *dx, int *dy, int *buttons) {
    uint8_t st = inb(PS2_STATUS);
    if (!(st & 0x01)) return false;       /* no data              */
    if (!(st & 0x20)) return false;       /* keyboard data — leave for kbd */
    return mouse_feed(inb(PS2_DATA), dx, dy, buttons);
}

/* IRQ12 path: assemble the packet in interrupt context, post a typed event
   into the fabric input ring. Minimal on purpose — no logging, no drawing. */
static void mouse_irq(void) {
    uint8_t st = inb(PS2_STATUS);
    if (!(st & 0x01) || !(st & 0x20)) return;     /* spurious / kbd byte    */
    int dx, dy, btn;
    if (mouse_feed(inb(PS2_DATA), &dx, &dy, &btn)) {
        uint8_t payload[8] = { (uint8_t)dx, (uint8_t)dy, (uint8_t)btn, 0 };
        seb_emit_spike(SEB_CH_MOUSE, 150, 0, payload, 4);
        rx_input_post_mouse(dx, dy, btn);         /* full ring: packet dropped */
    }
}

/* Switch to interrupt-driven mouse input. mouse_init() already set the aux-
   interrupt enable bit in the i8042 config byte; this registers the IRQ12
   handler (which unmasks PIC line 12 + the cascade). */
bool mouse_enable_irq(void) {
    if (!g_present) return false;
    g_idx = 0;
    irq_register(12, mouse_irq);
    return true;
}
