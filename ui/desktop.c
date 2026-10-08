/*
 * Desktop chrome + page renderer (used to live in kernel/render.c).
 * Framebuffer path + text fallback. Real WM is ui/window.c.
 */
#include "desktop.h"
#include "framebuffer.h"

void render_init(void) { /* backend chosen dynamically by fb_active() */ }
render_mode render_current_mode(void) { return fb_active() ? RENDER_GPU : RENDER_TEXT; }
const char *render_mode_name(render_mode m) {
    return m == RENDER_GPU ? fb_mode_string() : "text (serial/vga)";
}

/* ------------------------------- layout ---------------------------------- */
#define NAV_W     200          /* left navigation column width */
#define CONTENT_X (NAV_W + 20)
#define BAR_H     44
static uint32_t g_cy;

static int g_nav_active = -1;
void render_set_nav(int a) { g_nav_active = a; }

/* Click hitboxes + button widgets live in ui/widgets.c (render_add_hit,
   render_clear_hits, render_hit_test). */
static void hit_add(uint32_t x, uint32_t y, uint32_t w, uint32_t h, int id) {
    render_add_hit(x, y, w, h, id);
}

static void rule(char ch) { for (int i = 0; i < 64; i++) console_putc(ch); console_putc('\n'); }
static void smirror3(const char *a, const char *b, const char *c) {
    if (!fb_active()) return;
    console_serial_write(a); console_serial_write(b); console_serial_write(c);
    console_serial_write("\n");
}

/* ------------------------------- left nav -------------------------------- */
static const char *NAV_LABELS[NAV_COUNT] = {
    "Home", "Social", "Laws", "You", "Editor", "Settings", "Halt",
};

static void draw_nav(void) {
    uint32_t w, h; fb_dims(&w, &h);
    fb_fill_rect(0, 0, NAV_W, h, RGX_NAV);
    fb_fill_rect(NAV_W - 2, 0, 2, h, RGX_BAR);           /* divider */
    fb_text(18, 14, "EchOS", RGX_FG, RGX_NAV);
    fb_text(18 + 5 * FB_GLYPH_W, 14, "1.0", RGX_DIM, RGX_NAV);
    fb_fill_rect(12, 40, NAV_W - 24, 1, RGX_BTN);

    uint32_t y = 60;
    for (int i = 0; i < NAV_COUNT; i++) {
        bool sel = (i == g_nav_active);
        if (sel) fb_fill_rect(8, y - 5, NAV_W - 16, 26, RGX_NAV_SEL);
        fb_text(20, y, NAV_LABELS[i], sel ? RGX_WHITE : RGX_FG, sel ? RGX_NAV_SEL : RGX_NAV);
        hit_add(8, y - 5, NAV_W - 16, 26, RENDER_NAV_BASE + i);
        y += 34;
    }
}

/* ------------------------------- page API -------------------------------- */
void render_begin_page(const char *title, const char *origin) {
    render_clear_hits();
    if (fb_active()) {
        uint32_t w, h; fb_dims(&w, &h);
        fb_clear();
        draw_nav();
        fb_fill_rect(NAV_W, 0, w - NAV_W, BAR_H, RGX_BAR);
        fb_text(NAV_W + 16, 14, title, RGX_BG, RGX_BAR);
        uint32_t ow = (uint32_t)k_strlen(origin) * FB_GLYPH_W;
        fb_text(w - ow - 16, 14, origin, RGX_BG, RGX_BAR);
        fb_fill_rect(NAV_W, BAR_H, w - NAV_W, h - BAR_H, RGX_PANEL);
        g_cy = BAR_H + 18;
        smirror3("\n== ", title, " ==");
        smirror3("   [ ", origin, " ]");
    } else {
        console_putc('\n'); rule('=');
        console_write("  "); console_write(title);
        console_write("   [ "); console_write(origin); console_write(" ]\n");
        rule('-');
    }
}

void render_line(const char *text) {
    if (fb_active()) {
        fb_text(CONTENT_X, g_cy, text, RGX_FG, RGX_PANEL);
        g_cy += 22;
        smirror3("  ", text, "");
    } else {
        console_write("  "); console_write(text); console_putc('\n');
    }
}

void render_link(const char *label, const char *url) {
    if (fb_active()) {
        fb_text(CONTENT_X, g_cy, ">", RGX_BAR, RGX_PANEL);
        fb_text(CONTENT_X + 20, g_cy, label, RGX_WHITE, RGX_PANEL);
        fb_text(CONTENT_X + 20 + (uint32_t)(k_strlen(label) + 2) * FB_GLYPH_W, g_cy, url, RGX_DIM, RGX_PANEL);
        g_cy += 24;
        smirror3("  > ", label, "");
        console_serial_write("      "); console_serial_write(url); console_serial_write("\n");
    } else {
        console_write("  > "); console_write(label);
        console_write("   "); console_write(url); console_putc('\n');
    }
}

void render_button(const char *label, const char *url, int id) {
    if (fb_active()) {
        uint32_t bw = (uint32_t)k_strlen(label) * FB_GLYPH_W + 28;
        uint32_t bx = CONTENT_X, by = g_cy;
        fb_fill_rect(bx, by, bw, 28, RGX_BTN);
        fb_rect_outline(bx, by, bw, 28, RGX_BAR);
        fb_text(bx + 14, by + 6, label, RGX_BTN_TXT, RGX_BTN);
        if (url && url[0] && url[0] != '\0')
            fb_text(bx + bw + 16, by + 6, url, RGX_DIM, RGX_PANEL);
        hit_add(bx, by, bw, 28, id);
        g_cy += 38;
        smirror3("  [ ", label, " ]"); smirror3("      ", url ? url : "", "");
    } else {
        console_write("  [ "); console_write(label); console_write(" ]   ");
        console_write(url ? url : ""); console_putc('\n');
    }
}

void render_end_page(void) {
    if (fb_active()) {
        fb_set_cursor_px(CONTENT_X - 4, g_cy + 12);
    } else {
        rule('='); console_putc('\n');
    }
}

void render_page(const rogex_page *page) {
    render_begin_page(page->title, page->url);
    const char *s = page->body;
    char line[160];
    while (*s) {
        size_t n = 0;
        while (*s && *s != '\n' && n < sizeof(line) - 1) line[n++] = *s++;
        line[n] = '\0';
        if (*s == '\n') s++;
        render_line(line);
    }
    render_end_page();
}
