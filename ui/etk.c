/* ETK — EchOS Toolkit implementation. See etk.h for the architecture.
 *
 * Freestanding C17. Fixed node pool (no allocation). Integer-only: the
 * kernel compiles with -mno-sse, so SIMD blending is a documented roadmap
 * item behind SSE state saving — not a silent FPU hazard.
 */
#include "etk.h"
#ifdef ROGEX_HOST_TEST
/* host test: framebuffer calls are stubbed in the test file */
void fb_fill_rect(unsigned, unsigned, unsigned, unsigned, unsigned);
void fb_round_fill(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
void fb_text(unsigned, unsigned, const char *, unsigned, unsigned);
void fb_blit_rgba(const unsigned *, unsigned, unsigned, unsigned, unsigned);
void fb_damage(unsigned, unsigned, unsigned, unsigned);
void fb_damage_reset(void);
void fb_present(void);
#define FB_GLYPH_W 8
#else
#include "framebuffer.h"
#include "icon_pack.h"
#endif

/* ------------------------------- rect ops -------------------------------- */
bool etk_rect_empty(const etk_rect *r) {
    return !r || r->w <= 0 || r->h <= 0;
}

bool etk_rect_intersect(const etk_rect *a, const etk_rect *b) {
    if (etk_rect_empty(a) || etk_rect_empty(b)) return false;
    return a->x < b->x + b->w && a->x + a->w > b->x &&
           a->y < b->y + b->h && a->y + a->h > b->y;
}

void etk_rect_union(const etk_rect *a, const etk_rect *b, etk_rect *out) {
    int ax0 = a->x,        ay0 = a->y;
    int ax1 = a->x + a->w, ay1 = a->y + a->h;
    int bx0 = b->x,        by0 = b->y;
    int bx1 = b->x + b->w, by1 = b->y + b->h;
    out->x = ax0 < bx0 ? ax0 : bx0;
    out->y = ay0 < by0 ? ay0 : by0;
    out->w = (ax1 > bx1 ? ax1 : bx1) - out->x;
    out->h = (ay1 > by1 ? ay1 : by1) - out->y;
}

/* ------------------------------ node pool -------------------------------- */
#define ETK_MAX_NODES 512
static etk_node  g_pool[ETK_MAX_NODES];
static int       g_pool_n;
static etk_node *g_root;
static uint32_t  g_next_id = 1;
static int       g_last_rects;
static etk_scope g_scope = ETK_SCOPE_ALL;

void etk_init(void) {
    g_pool_n = 0;
    g_next_id = 1;
    g_root = 0;
    g_last_rects = 0;
    g_scope = ETK_SCOPE_ALL;
}

void etk_reset(void) { etk_init(); }

int etk_node_count(void) { return g_pool_n; }
int etk_last_rect_count(void) { return g_last_rects; }
void etk_set_scope(etk_scope s) { g_scope = s; }
etk_scope etk_get_scope(void) { return g_scope; }

etk_node *etk_new(etk_node *parent, etk_type type, const char *name) {
    if (g_pool_n >= ETK_MAX_NODES) return 0;
    etk_node *n = &g_pool[g_pool_n++];
    k_memset(n, 0, sizeof(*n));
    n->id = g_next_id++;
    n->type = type;
    n->visible = true;
    n->color = 0xFFFFFFFF;
    if (name) {
        int i = 0;
        while (name[i] && i < 15) { n->text[i] = name[i]; i++; }
        n->text[i] = 0;
    }
    n->parent = parent;
    if (parent) {
        /* append to parent's child list */
        if (!parent->first_child) parent->first_child = n;
        else {
            etk_node *c = parent->first_child;
            while (c->next) c = c->next;
            c->next = n;
        }
    } else if (!g_root) {
        g_root = n;
    }
    return n;
}

etk_node *etk_find(uint32_t id) {
    for (int i = 0; i < g_pool_n; i++)
        if (g_pool[i].id == id) return &g_pool[i];
    return 0;
}

/* --------------------------- node invalidation --------------------------- */
static void node_dirty(etk_node *n) {
    n->dirty = true;
    /* the old footprint must be repainted wherever it was */
    if (!etk_rect_empty(&n->global)) n->moved = true;
}

void etk_set_bounds(etk_node *n, int x, int y, int w, int h) {
    if (!n) return;
    if (n->bounds.x == x && n->bounds.y == y &&
        n->bounds.w == w && n->bounds.h == h) return;
    n->bounds.x = x; n->bounds.y = y; n->bounds.w = w; n->bounds.h = h;
    node_dirty(n);
}

void etk_set_visible(etk_node *n, bool v) {
    if (!n || n->visible == v) return;
    n->visible = v;
    node_dirty(n);
}

void etk_set_color(etk_node *n, uint32_t color) {
    if (!n || n->color == color) return;
    n->color = color;
    node_dirty(n);
}

void etk_set_text(etk_node *n, const char *text) {
    if (!n || !text) return;
    int i = 0;
    while (text[i] && i < (int)sizeof(n->text) - 1) { n->text[i] = text[i]; i++; }
    n->text[i] = 0;
    node_dirty(n);
}

void etk_set_align(etk_node *n, etk_align a) {
    if (!n || (int)n->align == (int)a) return;
    n->align = a;
    node_dirty(n);
}

void etk_set_texture(etk_node *n, const uint32_t *rgba) {
    if (!n || n->texture == rgba) return;
    n->texture = rgba;
    node_dirty(n);
}

void etk_invalidate(etk_node *n) { if (n) node_dirty(n); }

/* ------------------------------ sync pass -------------------------------- */
static void dmg_add(const etk_rect *r);
static void sync_rec(etk_node *n, int px, int py, bool parent_dirty) {
    if (!n->visible) {
        /* a hidden node that was visible last frame still needs its old
           footprint repainted */
        if (!etk_rect_empty(&n->global) && (n->dirty || n->moved))
            dmg_add(&n->global);
        return;
    }
    int gx = px + n->bounds.x, gy = py + n->bounds.y;
    bool changed = parent_dirty || n->dirty;
    if (n->global.x != gx || n->global.y != gy ||
        n->global.w != n->bounds.w || n->global.h != n->bounds.h) {
        changed = true;
    }
    if (changed) {
        /* damage the OLD footprint (where it used to be) … */
        if (!etk_rect_empty(&n->global) &&
            (n->global.x != gx || n->global.y != gy ||
             n->global.w != n->bounds.w || n->global.h != n->bounds.h))
            dmg_add(&n->global);
        n->global.x = gx; n->global.y = gy;
        n->global.w = n->bounds.w; n->global.h = n->bounds.h;
        /* … and the NEW one (where it is now) */
        dmg_add(&n->global);
        node_dirty(n);
    }
    for (etk_node *c = n->first_child; c; c = c->next)
        sync_rec(c, gx, gy, changed && n->type == ETK_CONTAINER);
}

void etk_sync(void) {
    if (g_root) sync_rec(g_root, 0, 0, false);
}

/* --------------------------- damage consolidation ------------------------ */
#define ETK_MAX_DAMAGE 16
#define ETK_MERGE_GAP  8          /* px — merge near-neighbours            */

static etk_rect g_dmg[ETK_MAX_DAMAGE];
static int      g_dmg_n;
static bool     g_full;

void etk_frame_begin(void) {
    g_dmg_n = 0;
    g_full = false;
    /* last frame's geometry becomes the old footprint for every node */
    for (int i = 0; i < g_pool_n; i++) {
        if (g_pool[i].moved || g_pool[i].dirty) {
            if (!etk_rect_empty(&g_pool[i].global))
                g_pool[i].old_global = g_pool[i].global;
        }
    }
}

static void dmg_add(const etk_rect *r) {
    if (etk_rect_empty(r) || g_full) return;
    etk_rect rc = *r;
    if (rc.x < 0) { rc.w += rc.x; rc.x = 0; }
    if (rc.y < 0) { rc.h += rc.y; rc.y = 0; }
    if (etk_rect_empty(&rc)) return;
    if (g_dmg_n >= ETK_MAX_DAMAGE) { g_full = true; g_dmg_n = 1;
        g_dmg[0].x = -1; g_dmg[0].y = -1; g_dmg[0].w = 1 << 20; g_dmg[0].h = 1 << 20;
        return;
    }
    g_dmg[g_dmg_n++] = rc;
}

int etk_damage(etk_rect *out, int max) {
    if (g_full) {
        if (max > 0) { out[0] = g_dmg[0]; return 1; }
        return 0;
    }
    /* AABB merge pass: fold overlapping / near-neighbouring rects */
    bool merged = true;
    while (merged && g_dmg_n > 1) {
        merged = false;
        for (int i = 0; i < g_dmg_n && !merged; i++) {
            for (int j = i + 1; j < g_dmg_n && !merged; j++) {
                etk_rect a = g_dmg[i], b = g_dmg[j];
                etk_rect wide = { a.x - ETK_MERGE_GAP, a.y - ETK_MERGE_GAP,
                                  a.w + 2 * ETK_MERGE_GAP, a.h + 2 * ETK_MERGE_GAP };
                etk_rect wide2 = { b.x - ETK_MERGE_GAP, b.y - ETK_MERGE_GAP,
                                   b.w + 2 * ETK_MERGE_GAP, b.h + 2 * ETK_MERGE_GAP };
                if (etk_rect_intersect(&wide, &wide2)) {
                    etk_rect_union(&a, &b, &g_dmg[i]);
                    g_dmg[j] = g_dmg[g_dmg_n - 1];
                    g_dmg_n--;
                    merged = true;
                }
            }
        }
    }
    /* density threshold: too many rects → one full-screen redraw */
    if (g_dmg_n > ETK_MAX_DAMAGE / 2) {
        etk_rect all = g_dmg[0];
        for (int i = 1; i < g_dmg_n; i++) etk_rect_union(&all, &g_dmg[i], &all);
        g_dmg[0] = all;
        g_dmg_n = 1;
    }
    g_last_rects = g_dmg_n;
    int n = 0;
    for (int i = 0; i < g_dmg_n && n < max; i++) out[n++] = g_dmg[i];
    return n;
}

void etk_render_all_dirty(void) {
    etk_rect rects[ETK_MAX_DAMAGE];
    int n = etk_damage(rects, ETK_MAX_DAMAGE);
    for (int i = 0; i < n; i++) etk_render(&rects[i]);
}

/* ------------------------------ render pass ------------------------------ */
static void clip_rect(const etk_rect *clip, etk_rect *r) {
    int x0 = r->x < clip->x ? clip->x : r->x;
    int y0 = r->y < clip->y ? clip->y : r->y;
    int x1 = r->x + r->w > clip->x + clip->w ? clip->x + clip->w : r->x + r->w;
    int y1 = r->y + r->h > clip->y + clip->h ? clip->y + clip->h : r->y + r->h;
    r->x = x0; r->y = y0; r->w = x1 - x0; r->h = y1 - y0;
}

static void render_rec(etk_node *n, const etk_rect *clip, bool parent_dirty) {
    if (!n->visible) return;
    bool paint = parent_dirty || n->dirty || n->moved;
    if (!etk_rect_intersect(&n->global, clip)) return;   /* subtree culled */

    if (paint) {
        switch (n->type) {
        case ETK_SOLID: {
            etk_rect r = n->global;
            clip_rect(clip, &r);
            if (!etk_rect_empty(&r))
                n->radius > 0 ? fb_round_fill((uint32_t)r.x, (uint32_t)r.y,
                                              (uint32_t)r.w, (uint32_t)r.h,
                                              (uint32_t)n->radius, n->color)
                              : fb_fill_rect((uint32_t)r.x, (uint32_t)r.y,
                                             (uint32_t)r.w, (uint32_t)r.h, n->color);
            break;
        }
        case ETK_TEXT: {
            if (!n->text[0]) break;
            int tw = (int)k_strlen(n->text) * FB_GLYPH_W;
            int tx = n->global.x;
            if (n->align == ETK_ALIGN_CENTER) tx = n->global.x + (n->global.w - tw) / 2;
            else if (n->align == ETK_ALIGN_RIGHT) tx = n->global.x + n->global.w - tw;
            if (tx < clip->x - tw && tx + tw > clip->x + clip->w) break;
            if (n->bg) fb_fill_rect((uint32_t)n->global.x, (uint32_t)n->global.y,
                                    (uint32_t)n->global.w, (uint32_t)n->global.h, n->bg);
            fb_text((uint32_t)tx, (uint32_t)n->global.y, n->text, n->color, n->bg);
            break;
        }
        case ETK_TEXTURE: {
            const uint32_t *rgba = n->texture;
            if (!rgba) break;
            fb_blit_rgba(rgba, 48, 48, (uint32_t)n->global.x, (uint32_t)n->global.y);
            break;
        }
        case ETK_CONTAINER:
        default:
            break;
        }
    }
    for (etk_node *c = n->first_child; c; c = c->next)
        render_rec(c, clip, paint && n->type == ETK_CONTAINER);
}

void etk_render(const etk_rect *clip) {
    if (!clip || etk_rect_empty(clip) || !g_root) return;
    fb_damage_reset();
    render_rec(g_root, clip, false);
    fb_damage((uint32_t)clip->x, (uint32_t)clip->y, (uint32_t)clip->w, (uint32_t)clip->h);
    fb_present();
}

/* after rendering, retire dirty flags */
void etk_frame_end(void) {
    for (int i = 0; i < g_pool_n; i++) {
        g_pool[i].dirty = false;
        g_pool[i].moved = false;
        g_pool[i].old_global = g_pool[i].global;
    }
}
