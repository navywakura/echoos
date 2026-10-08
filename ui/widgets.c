/* Buttons + click hitboxes. Small and dumb — keep it that way. */
#include "widgets.h"
#include "desktop.h"

static struct { uint32_t x, y, w, h; int id; } g_hits[WIDGETS_MAX_HITS];
static int g_nhits;

int render_hit_test(uint32_t x, uint32_t y) {
    for (int i = 0; i < g_nhits; i++)
        if (x >= g_hits[i].x && x < g_hits[i].x + g_hits[i].w &&
            y >= g_hits[i].y && y < g_hits[i].y + g_hits[i].h)
            return g_hits[i].id;
    return -1;
}

void render_add_hit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, int id) {
    if (id < 0 || g_nhits >= WIDGETS_MAX_HITS) return;
    g_hits[g_nhits].x = x; g_hits[g_nhits].y = y;
    g_hits[g_nhits].w = w; g_hits[g_nhits].h = h; g_hits[g_nhits].id = id;
    g_nhits++;
}

void render_clear_hits(void) { g_nhits = 0; }
