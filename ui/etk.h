/* ETK — EchOS Toolkit. Retained-mode scene graph + damage tracking.
 *
 * Architecture (GTK4/GSK-inspired, from scratch, freestanding C17):
 *
 *   [scene graph]  retained node tree: container / solid / text / texture
 *   [sync pass]    local → global bounds; dirtiness = bounds delta + flags
 *   [damage pass]  old_global_bounds ∪ new_global_bounds per dirty node,
 *                  consolidated (AABB merge, count/area threshold → full)
 *   [render pass]  per dirty rect: walk tree, AABB-test, emit clipped draws
 *   [present]      fb_present() already copies only damaged scanlines
 *
 * Migration: the desktop layer (wallpaper, icons, dock) is ETK-managed today;
 * window contents migrate node-by-node. Until then the WM renders windows
 * immediately and marks their union as damage (ETK_DIRTY_ALL scope).
 */
#ifndef ECHOS_ETK_H
#define ECHOS_ETK_H

#include "rogex.h"

typedef struct { int x, y, w, h; } etk_rect;

typedef enum {
    ETK_CONTAINER = 0,   /* clip/transform only                       */
    ETK_SOLID,           /* fill_rect(color[, radius])                */
    ETK_TEXT,            /* fb_text(text, color)                      */
    ETK_TEXTURE,         /* fb_blit_rgba(rgba, 48, 48)                */
} etk_type;

typedef enum {
    ETK_ALIGN_LEFT = 0, ETK_ALIGN_CENTER, ETK_ALIGN_RIGHT,
} etk_align;

/* Node — fixed pool, first-child/next-sibling links (no allocation). */
typedef struct etk_node etk_node;
struct etk_node {
    uint32_t   id;
    etk_type   type;
    bool       visible;

    etk_rect   bounds;            /* relative to parent                    */
    etk_rect   global;            /* absolute, recomputed each sync        */
    etk_rect   old_global;        /* where we were last frame (damage)     */
    bool       dirty;             /* explicit invalidation                 */
    bool       moved;             /* global bounds changed since last sync */

    /* styling / content */
    uint32_t   color;             /* solid fill / text color               */
    uint32_t   bg;                /* text background (0 = transparent)     */
    int        radius;            /* solid corner radius                   */
    char       text[96];
    etk_align  align;
    const uint32_t *texture;      /* 48x48 RGBA for ETK_TEXTURE            */

    etk_node  *first_child;
    etk_node  *next;              /* next sibling                          */
    etk_node  *parent;
};

/* Lifecycle ---------------------------------------------------------------- */
void  etk_init(void);
void  etk_reset(void);                     /* drop the whole tree             */
etk_node *etk_new(etk_node *parent, etk_type type, const char *name);
etk_node *etk_find(uint32_t id);

/* Node mutation (each marks dirty + damages old bounds) -------------------- */
void etk_set_bounds(etk_node *n, int x, int y, int w, int h);
void etk_set_visible(etk_node *n, bool v);
void etk_set_color(etk_node *n, uint32_t color);
void etk_set_text(etk_node *n, const char *text);
void etk_set_align(etk_node *n, etk_align a);
void etk_set_texture(etk_node *n, const uint32_t *rgba);
void etk_invalidate(etk_node *n);          /* force repaint next frame        */

/* Frame pipeline ----------------------------------------------------------- */
void etk_frame_begin(void);                /* clear per-frame damage list     */
void etk_sync(void);                       /* transform + dirtiness pass      */
int  etk_damage(etk_rect *out, int max);   /* consolidated dirty rects        */
void etk_render(const etk_rect *clip);     /* clipped render of one rect      */
void etk_render_all_dirty(void);           /* render every consolidated rect  */
void etk_frame_end(void);                  /* retire dirty flags              */

/* Frame scope for the WM fast path ----------------------------------------- */
typedef enum {
    ETK_SCOPE_ALL = 0,   /* full compose (windows changed)                   */
    ETK_SCOPE_SCENE,     /* only the ETK tree changed (dock hover, icons)    */
} etk_scope;
void etk_set_scope(etk_scope s);
etk_scope etk_get_scope(void);

/* Rect helpers (exposed for tests) ----------------------------------------- */
bool etk_rect_intersect(const etk_rect *a, const etk_rect *b);
void etk_rect_union(const etk_rect *a, const etk_rect *b, etk_rect *out);
bool etk_rect_empty(const etk_rect *r);

/* Stats (self-check) ------------------------------------------------------- */
int  etk_node_count(void);
int  etk_last_rect_count(void);

#endif /* ECHOS_ETK_H */
