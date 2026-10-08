/*
 * ui/dock.c — EchOS Eclipse Shell dock (macOS-style, not a Windows taskbar).
 *
 * Bottom-centred bar of app tiles with real-time vector magnification:
 * every tile's scale factor is f(distance from cursor), so icons swell
 * under the pointer and fall off smoothly, exactly like macOS.
 *
 *   scale(i) = 1 + (MAX_SCALE - 1) * falloff(d)
 *   falloff(d) = R*R / (R*R + d*d)          (rational bell — integer math only,
 *                                            no FPU: this file is freestanding)
 *
 * Tiles are pinned apps; running apps show an indicator dot. The dock owns
 * no policy: the shell registers pins via dock_pin()/dock_unpin() (the global
 * context menu calls these for "Pin to Dock") and marks running state.
 */
#include "dock.h"
#include "framebuffer.h"
#include "icon_pack.h"
#include "arch.h"
#include "../kernel/version.h"

static void dock_compute(uint32_t screen_w, uint32_t screen_h, uint32_t cursor_x);
#include "rogex.h"

/* ------------------------------ constants -------------------------------- */
#define DOCK_MAX_TILES  16
#define DOCK_TILE_BASE  44      /* resting icon size, px          */
#define DOCK_PAD        10      /* gap between tiles, px          */
#define DOCK_MARGIN     8       /* inset from screen bottom/edges */
#define DOCK_MAG_RADIUS 120     /* px falloff radius              */
#define DOCK_MAX_SCALE  114688  /* Q16.16 peak magnification (1.75x) */

/* Q16.16 helpers — integer-only, safe in kernel. */
#define Q16_ONE   65536u
static uint32_t q16_mul(uint32_t a, uint32_t b) { return (uint32_t)(((uint64_t)a * b) >> 16); }

static bool name_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a++ != *b++) return false; }
    return *a == *b;
}

typedef struct {
    char name[24];
    bool pinned;
    bool running;
} dock_tile;

static dock_tile g_tiles[DOCK_MAX_TILES];
static int       g_tile_count;
static bool      g_visible = true;
static int       g_hover = -1;        /* tile under the cursor          */
static int       g_bounce_idx = -1;   /* recently launched tile  */
static uint64_t  g_bounce_tick;       /* arch_timer_ticks() at launch   */

/* Cached layout computed by dock_layout() each frame. */
typedef struct { uint32_t x, y, w, h; } dock_rect;
static dock_rect g_layout[DOCK_MAX_TILES];
static uint32_t  g_bar_x, g_bar_y, g_bar_w, g_bar_h;

void dock_init(void)
{
    static const char *defaults[] = {
        "Files", "Nova", "Terminal", "Calculator", "Multimedia", "Settings",
    };
    g_tile_count = 0;
    for (int i = 0; i < (int)(sizeof(defaults) / sizeof(defaults[0])); i++)
        dock_pin(defaults[i]);
    g_hover = -1;
}

bool dock_pin(const char *app)
{
    /* Already present? Just pin it. */
    for (int i = 0; i < g_tile_count; i++) {
        if (name_eq(g_tiles[i].name, app)) { g_tiles[i].pinned = true; return true; }
    }
    if (g_tile_count >= DOCK_MAX_TILES) return false;
    dock_tile *t = &g_tiles[g_tile_count++];
    size_t n = k_strlen(app);
    if (n >= sizeof(t->name)) n = sizeof(t->name) - 1;
    k_memcpy(t->name, app, n);
    t->name[n] = '\0';
    t->pinned  = true;
    t->running = false;
    return true;
}

bool dock_unpin(const char *app)
{
    for (int i = 0; i < g_tile_count; i++) {
        if (name_eq(g_tiles[i].name, app)) {
            if (g_tiles[i].running) { g_tiles[i].pinned = false; return true; }
            for (int j = i; j < g_tile_count - 1; j++) g_tiles[j] = g_tiles[j + 1];
            g_tile_count--;
            return true;
        }
    }
    return false;
}

void dock_set_running(const char *app, bool running)
{
    for (int i = 0; i < g_tile_count; i++)
        if (name_eq(g_tiles[i].name, app)) { g_tiles[i].running = running; return; }
    if (running) dock_pin(app);      /* unpinned launch gets a transient tile */
}

void dock_set_visible(bool v) { g_visible = v; }

/* True when (x,y) is inside the dock's magnification influence band — the
   shell repaints only while this is true (or on the transition out). */
void dock_band(int *x, int *y, int *w, int *h)
{
    uint32_t sw, sh; fb_dims(&sw, &sh);
    if (!g_visible || g_tile_count == 0 || !g_bar_w) {
        *x = 0; *y = (int)sh - 120; *w = (int)sw; *h = 120;
        return;
    }
    *x = (g_bar_x > DOCK_MAG_RADIUS) ? (int)(g_bar_x - DOCK_MAG_RADIUS) : 0;
    *w = (int)g_bar_w + 2 * DOCK_MAG_RADIUS;
    if (*x + *w > (int)sw) *w = (int)sw - *x;
    *y = (g_bar_y > 26) ? (int)g_bar_y - 26 : 0;   /* hover label headroom */
    *h = (int)sh - *y;
}

bool dock_near(uint32_t x, uint32_t y)
{
    if (!g_visible || g_tile_count == 0) return false;
    uint32_t sw, sh; fb_dims(&sw, &sh);
    if (y + 40 < g_bar_y) return false;               /* above the band */
    if (y > g_bar_y + g_bar_h + 20) return false;     /* below the bar  */
    uint32_t x0 = (g_bar_x > DOCK_MAG_RADIUS) ? g_bar_x - DOCK_MAG_RADIUS : 0;
    uint32_t x1 = g_bar_x + g_bar_w + DOCK_MAG_RADIUS;
    return x >= x0 && x <= x1;
}

bool dock_animating(void)
{
    return g_bounce_idx >= 0 && arch_timer_ticks() - g_bounce_tick < 40;
}

/* Launch animation: the tile pulses for ~40 PIT ticks (0.4 s at 100 Hz). */
void dock_bounce(const char *app)
{
    for (int i = 0; i < g_tile_count; i++) {
        if (name_eq(g_tiles[i].name, app)) {
            g_bounce_idx = i;
            g_bounce_tick = arch_timer_ticks();
            return;
        }
    }
}

int dock_hit_test(uint32_t x, uint32_t y)
{
    if (!g_visible) return -1;
    if (g_tile_count == 0) dock_init();     /* self-heal */
    /* Recompute geometry for THIS click instead of trusting the last
       compose's cached rects — the dock must stay clickable even if the
       render path skipped a frame. */
    uint32_t sw, sh; fb_dims(&sw, &sh);
    dock_compute(sw, sh, x);
    for (int i = 0; i < g_tile_count; i++) {
        dock_rect *r = &g_layout[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return i;
    }
    return -1;
}

const char *dock_app_at(int index)
{
    return (index >= 0 && index < g_tile_count) ? g_tiles[index].name : 0;
}

bool dock_name_is(const char *app, const char *name)
{
    while (*app && *name) { if (*app++ != *name++) return false; }
    return *app == *name;
}

/*
 * Recompute geometry for this frame given the cursor X. Magnification is
 * purely positional — call once per frame before dock_render().
 */
void dock_layout(uint32_t screen_w, uint32_t screen_h, uint32_t cursor_x,
                 uint32_t cursor_y)
{
    if (!g_visible) return;
    if (g_tile_count == 0) dock_init();     /* self-heal: state must never die */
    /* macOS behaviour: tiles only magnify while the pointer is INSIDE the
       bar. Passing near the dock across the desktop must not move it. */
    bool inside = cursor_y >= g_bar_y && cursor_y < g_bar_y + g_bar_h &&
                  cursor_x >= g_bar_x && cursor_x < g_bar_x + g_bar_w;
    if (!inside) {
        /* pointer outside the bar: every tile at base scale, centred layout */
        g_hover = -1;
        uint32_t x = g_bar_x + DOCK_PAD;
        uint32_t total_w = DOCK_PAD;
        for (int i = 0; i < g_tile_count; i++) {
            g_layout[i].x = x;
            g_layout[i].y = g_bar_y + g_bar_h - DOCK_PAD - DOCK_TILE_BASE;
            g_layout[i].w = DOCK_TILE_BASE;
            g_layout[i].h = DOCK_TILE_BASE;
            x += DOCK_TILE_BASE + DOCK_PAD;
            total_w += DOCK_TILE_BASE + DOCK_PAD;
        }
        (void)total_w;
        return;
    }
    dock_compute(screen_w, screen_h, cursor_x);
}

static void dock_compute(uint32_t screen_w, uint32_t screen_h, uint32_t cursor_x)
{
    /* Per-tile Q16.16 scale factors from cursor distance. */
    uint32_t scale[DOCK_MAX_TILES];
    uint32_t widths[DOCK_MAX_TILES];

    /* Tile centres depend on total width, which depends on scales — one
       pass with base positions is enough visually (macOS does two; we keep
       one to stay O(n) per frame). */
    uint32_t total_w = DOCK_PAD;
    for (int i = 0; i < g_tile_count; i++) {
        uint32_t centre = g_bar_x + total_w + DOCK_TILE_BASE / 2;  /* first estimate */
        uint32_t d = (centre > cursor_x) ? centre - cursor_x : cursor_x - centre;
        uint32_t rr = DOCK_MAG_RADIUS * DOCK_MAG_RADIUS;
        uint32_t dd = d * d;
        /* falloff = R²/(R²+d²), scaled into Q16.16 */
        uint32_t f = (dd + rr) ? (uint32_t)(((uint64_t)rr << 16) / (dd + rr)) : Q16_ONE;
        uint32_t s = Q16_ONE + q16_mul((DOCK_MAX_SCALE - Q16_ONE) / 2, f);
        /* Launch bounce: triangle wave over the first ~40 ticks after a
           launch adds up to +35% scale — integer math only. */
        if ((int)i == g_bounce_idx && arch_timer_ticks() - g_bounce_tick < 40) {
            uint32_t dt = (uint32_t)(arch_timer_ticks() - g_bounce_tick);
            uint32_t tri = (dt < 20) ? (20 - dt) : (dt - 20);   /* 20..0..20 */
            s += q16_mul(23000u, tri * Q16_ONE / 20);           /* up to ~+0.35x */
        }
        scale[i]  = s;
        widths[i] = q16_mul(DOCK_TILE_BASE * Q16_ONE, s) >> 16;
        total_w  += widths[i] + DOCK_PAD;
    }

    g_bar_w = total_w + DOCK_PAD;
    g_bar_h = DOCK_TILE_BASE + DOCK_PAD * 2 + 8;   /* headroom for magnified tops */
    g_bar_x = (screen_w > g_bar_w) ? (screen_w - g_bar_w) / 2 : 0;
    g_bar_y = screen_h - g_bar_h - DOCK_MARGIN;

    uint32_t x = g_bar_x + DOCK_PAD;
    g_hover = -1;
    for (int i = 0; i < g_tile_count; i++) {
        uint32_t w = widths[i];
        uint32_t h = w;
        g_layout[i].x = x;
        g_layout[i].y = g_bar_y + g_bar_h - DOCK_PAD - h;   /* grow upward */
        g_layout[i].w = w;
        g_layout[i].h = h;
        if ((int)cursor_x >= (int)x && (int)cursor_x < (int)(x + w))
            g_hover = i;                                     /* real hover */
        x += w + DOCK_PAD;
    }
}

static uint32_t tile_color(int i)
{
    /* Flat/metal palette: deterministic hue per app name, dark theme. */
    static const uint32_t palette[] = {
        0xFF35506B, 0xFF5B3B28, 0xFF2E4A33, 0xFF4A2E45,
        0xFF274752, 0xFF52321F, 0xFF31395C, 0xFF46325A,
    };
    uint32_t acc = 0;
    for (const char *p = g_tiles[i].name; *p; p++) acc += (uint8_t)*p;
    return palette[acc % (sizeof(palette) / sizeof(palette[0]))];
}

/* Dock app name -> icon-pack asset name (48x48 RGBA). */
static const char *tile_icon_name(int i)
{
    static const struct { const char *app, *icon; } map[] = {
        { "Files",      "explorer" },
        { "Nova",       "browser"  },
        { "Terminal",   "terminal" },
        { "Calculator", "calc"     },
        { "Multimedia", "photos"   },
        { "Settings",   "ajustes"  },
    };
    for (unsigned k = 0; k < sizeof(map) / sizeof(map[0]); k++)
        if (dock_name_is(g_tiles[i].name, map[k].app)) return map[k].icon;
    return 0;
}

void dock_render(void)
{
    if (!g_visible) return;
    if (g_tile_count == 0) dock_init();     /* self-heal */

    /* Bar: dark translucent slab with metal top edge. */
    fb_round_blend(g_bar_x, g_bar_y, g_bar_w, g_bar_h, 14, 0xFF16161E, 225);
    fb_rect_outline(g_bar_x, g_bar_y, g_bar_w, g_bar_h, RGX_BAR);
    fb_hgradient_rect(g_bar_x + 1, g_bar_y + 1, g_bar_w - 2, 2, 0xFF3A3A44, 0xFF22222A);

    int hover = g_hover;                /* tile under the cursor (from layout) */
    for (int i = 0; i < g_tile_count; i++) {
        dock_rect *r = &g_layout[i];
        fb_round_fill(r->x, r->y, r->w, r->h, 8, tile_color(i));
        fb_rect_outline(r->x, r->y, r->w, r->h, RGX_BORDER);
        /* White rim so dark icon glyphs read on the metal tiles. */
        fb_round_outline(r->x - 1, r->y - 1, r->w + 2, r->h + 2, 9, RGX_HIGHLIGHT);

        /* Real app icon from the icon pack, centred in the tile. */
        const char *iname = tile_icon_name(i);
        int id = iname ? icon_index_by_name(iname) : -1;
        if (id >= 0 && icon_rgba_at(id))
            fb_blit_rgba(icon_rgba_at(id), 48, 48,
                         r->x + (r->w > 48 ? (r->w - 48) / 2 : 0),
                         r->y + (r->h > 48 ? (r->h - 48) / 2 : 0));

        /* macOS-style running indicator dot. */
        if (g_tiles[i].running)
            fb_fill_rect(r->x + r->w / 2 - 2, g_bar_y + g_bar_h - 5, 4, 3, RGX_HIGHLIGHT);

    }

    /* Hovered/magnified tile gets its name above the bar. */
    if (hover >= 0) {
        uint32_t tw = (uint32_t)k_strlen(g_tiles[hover].name) * FB_GLYPH_W + 12;
        uint32_t cx = g_layout[hover].x + g_layout[hover].w / 2;
        uint32_t lx = (cx > tw / 2) ? cx - tw / 2 : 2;
        fb_round_blend(lx, g_bar_y - 22, tw, 18, 5, 0x00000000, 160);
        fb_text(lx + 6, g_bar_y - 18, g_tiles[hover].name, RGX_WHITE, 0);
    }
}
