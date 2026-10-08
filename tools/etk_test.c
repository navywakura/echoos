/* ETK host unit test: rect math, damage consolidation, sync pass.
   Built against ui/etk.c with fb_* stubbed out. */
#define ROGEX_HOST_TEST 1
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "etk.h"

/* ---- framebuffer stubs (etk.c links against these) ---- */
#define FB_GLYPH_W 8
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) { (void)x;(void)y;(void)w;(void)h;(void)c; }
void fb_round_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t c) { (void)x;(void)y;(void)w;(void)h;(void)r;(void)c; }
void fb_text(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg) { (void)x;(void)y;(void)s;(void)fg;(void)bg; }
void fb_blit_rgba(const uint32_t *src, uint32_t w, uint32_t h, uint32_t x, uint32_t y) { (void)src;(void)w;(void)h;(void)x;(void)y; }
void fb_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h) { (void)x;(void)y;(void)w;(void)h; }
void fb_damage_reset(void) {}
void fb_present(void) {}
void *k_memset(void *d, int v, size_t n) { memset(d, v, n); return d; }
size_t k_strlen(const char *s) { return strlen(s); }
#define size_t host_size_t

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } \
} while (0)

int main(void) {
    /* rect ops */
    etk_rect a = {0, 0, 10, 10}, b = {5, 5, 10, 10}, c = {50, 50, 2, 2};
    CHECK(etk_rect_intersect(&a, &b));
    CHECK(!etk_rect_intersect(&a, &c));
    etk_rect u; etk_rect_union(&a, &b, &u);
    CHECK(u.x == 0 && u.y == 0 && u.w == 15 && u.h == 15);
    CHECK(etk_rect_empty(&(etk_rect){0,0,0,5}));

    /* scene graph: root + dock + tile */
    etk_init();
    etk_node *root = etk_new(0, ETK_CONTAINER, "root");
    etk_set_bounds(root, 0, 0, 1280, 720);
    etk_node *dock = etk_new(root, ETK_CONTAINER, "dock");
    etk_set_bounds(dock, 473, 640, 334, 72);
    etk_node *tile = etk_new(dock, ETK_SOLID, "tile");
    etk_set_bounds(tile, 10, 18, 44, 44);
    etk_set_color(tile, 0xFF35506B);

    etk_frame_begin();
    etk_sync();
    CHECK(tile->global.x == 483 && tile->global.y == 658);

    etk_rect rects[16];
    etk_frame_begin();
    etk_sync();
    int n = etk_damage(rects, 16);
    CHECK(n >= 1);                       /* first frame paints something */
    etk_render_all_dirty();
    etk_frame_end();

    /* idle frame: nothing dirty → nothing to repaint */
    etk_frame_begin();
    etk_sync();
    n = etk_damage(rects, 16);
    CHECK(n == 0);                       /* idle = zero damage ✓ */

    /* move the tile → old + new footprints both damaged */
    etk_set_bounds(tile, 60, 18, 44, 44);
    etk_frame_begin();
    etk_sync();
    n = etk_damage(rects, 16);
    CHECK(n >= 1);
    /* damage must cover BOTH the old and the new position */
    int covers_old = 0, covers_new = 0;
    etk_rect oldr = { 483, 658, 44, 44 }, newr = { 533, 658, 44, 44 };
    for (int i = 0; i < n; i++) {
        etk_rect u2;
        etk_rect_union(&rects[i], &oldr, &u2);
        if (u2.w == rects[i].w + 0 && rects[i].x <= oldr.x) covers_old = 1;
        etk_rect_union(&rects[i], &newr, &u2);
        if (u2.w == rects[i].w + 0 && rects[i].x <= newr.x) covers_new = 1;
    }
    CHECK(covers_old && covers_new);
    etk_render_all_dirty();
    etk_frame_end();

    /* text mutation marks dirty */
    etk_node *lbl = etk_new(dock, ETK_TEXT, "label");
    etk_set_bounds(lbl, 0, 0, 60, 16);
    etk_set_text(lbl, "Files");
    etk_frame_begin(); etk_sync();
    etk_render_all_dirty(); etk_frame_end();
    etk_frame_begin(); etk_sync();
    n = etk_damage(rects, 16);
    etk_frame_begin();                    /* idle again */
    etk_sync();
    CHECK(etk_damage(rects, 16) == 0);
    etk_set_text(lbl, "Terminal");
    etk_sync();
    n = etk_damage(rects, 16);
    CHECK(n >= 1);                        /* text change → damage */

    printf("ETK host test: ALL OK\n");
    return 0;
}
