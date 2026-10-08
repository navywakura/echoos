/*
 * ui/dragdrop.c — global drag & drop for the Eclipse Shell.
 *
 * One state machine, owned by the shell's event loop, that ANY surface can
 * join: desktop icons, dock tiles, explorer items. Surfaces register drop
 * zones; the machine tracks the payload and reports where it landed.
 *
 *   dragdrop_begin()  ON_DRAG_START   (mouse press + move past threshold)
 *   dragdrop_update() ON_DRAG_MOVE    (ghost follows cursor)
 *   dragdrop_drop()   ON_DRAG_DROP    (resolve zone, deliver payload)
 *   dragdrop_cancel()                 (Esc or focus loss)
 *
 * Freestanding: integer-only, no allocation — one active drag at a time.
 */
#include "dragdrop.h"
#include "framebuffer.h"
#include "rogex.h"

#define DROP_ZONES_MAX 16
#define DRAG_THRESHOLD 6      /* px of travel before a press becomes a drag */

typedef struct {
    int              kind;            /* DRAG_ZONE_* */
    uint32_t         x, y, w, h;
    void            *ctx;             /* opaque: window, tile index... */
} drop_zone;

static struct {
    bool     active;
    bool     armed;                   /* press seen, threshold not crossed */
    uint32_t press_x, press_y;
    uint32_t x, y;                    /* current cursor                    */
    char     label[40];
    int      kind;                    /* DRAG_ICON_*                       */
    void    *payload;
    drop_zone zones[DROP_ZONES_MAX];
    int      zone_count;
} g;

void dragdrop_reset(void) {
    g.active = false; g.armed = false; g.zone_count = 0;
}

bool dragdrop_press(uint32_t x, uint32_t y, const char *label, int kind, void *payload)
{
    if (g.active || g.armed) return false;
    g.press_x = x; g.press_y = y;
    g.x = x; g.y = y;
    size_t n = k_strlen(label);
    if (n >= sizeof(g.label)) n = sizeof(g.label) - 1;
    k_memcpy(g.label, label, n);
    g.label[n] = '\0';
    g.kind    = kind;
    g.payload = payload;
    g.armed   = true;
    return true;
}

bool dragdrop_update(uint32_t x, uint32_t y)
{
    if (!g.active && !g.armed) return false;
    g.x = x; g.y = y;
    if (!g.active && g.armed) {
        uint32_t dx = (x > g.press_x) ? x - g.press_x : g.press_x - x;
        uint32_t dy = (y > g.press_y) ? y - g.press_y : g.press_y - y;
        if (dx * dx + dy * dy >= DRAG_THRESHOLD * DRAG_THRESHOLD)
            g.active = true;                     /* ON_DRAG_START fires here */
    }
    return g.active;
}

int dragdrop_add_zone(int kind, uint32_t x, uint32_t y, uint32_t w, uint32_t h, void *ctx)
{
    if (g.zone_count >= DROP_ZONES_MAX) return -1;
    drop_zone *z = &g.zones[g.zone_count];
    z->kind = kind; z->x = x; z->y = y; z->w = w; z->h = h; z->ctx = ctx;
    return g.zone_count++;
}

/* Drop zones are re-registered by each surface every frame. */
void dragdrop_clear_zones(void) { g.zone_count = 0; }

static drop_zone *zone_at(uint32_t x, uint32_t y)
{
    /* Topmost (last registered) wins. */
    for (int i = g.zone_count - 1; i >= 0; i--) {
        drop_zone *z = &g.zones[i];
        if (x >= z->x && x < z->x + z->w && y >= z->y && y < z->y + z->h) return z;
    }
    return 0;
}

bool dragdrop_active(void) { return g.active; }

bool dragdrop_drop(uint32_t x, uint32_t y, dragdrop_result *out)
{
    if (!g.active) { if (g.armed) dragdrop_reset(); return false; }  /* click, not drag */
    drop_zone *z = zone_at(x, y);
    if (out) {
        out->accepted = (z != 0);
        out->zone_kind = z ? z->kind : DRAG_ZONE_NONE;
        out->zone_ctx  = z ? z->ctx : 0;
        out->payload   = g.payload;
        out->x = x; out->y = y;
    }
    dragdrop_reset();
    return true;                                   /* ON_DRAG_DROP delivered */
}

void dragdrop_cancel(void) { dragdrop_reset(); }

/* Ghost: translucent tile + label trailing the cursor while dragging. */
void dragdrop_render(void)
{
    if (!g.active) return;
    uint32_t tw = (uint32_t)k_strlen(g.label) * FB_GLYPH_W + 16;
    uint32_t th = 22;
    uint32_t gx = g.x - tw / 2, gy = g.y - th - 10;
    fb_blend_rect(gx, gy, tw, th, 0xB0202028, 0xFFE8E8F0);
    fb_text(gx + 8, gy + 5, g.label, RGX_WHITE, 0);

    /* Highlight the zone under the ghost. */
    drop_zone *z = zone_at(g.x, g.y);
    if (z) fb_rect_outline(z->x, z->y, z->w, z->h, RGX_HIGHLIGHT);
}
