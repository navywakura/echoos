/* Desktop / page render API. */
#ifndef ROGEX_RENDER_H
#define ROGEX_RENDER_H

#include "rogex.h"

typedef enum {
    RENDER_TEXT = 0,   /* CLI text renderer (implemented)                    */
    RENDER_GPU  = 1,   /* framebuffer/WebGPU "Liquid Glass" (Phase 3, TODO)  */
} render_mode;

/* A minimal page model. A future graphical backend can extend this (nodes,
   styles, shaders) behind the same render_page() call. */
typedef struct {
    const char *url;    /* rgx:// address        */
    const char *title;  /* page title            */
    const char *body;   /* text content          */
} rogex_page;

void        render_init(void);
render_mode render_current_mode(void);
const char *render_mode_name(render_mode m);
void        render_page(const rogex_page *page);

/* Page-building API used by both built-in pages and .rgxsite content. Draws a
   graphical panel+buttons when the framebuffer is active, or framed text
   otherwise — callers are backend-agnostic. */
void render_begin_page(const char *title, const char *origin);
void render_line(const char *text);
void render_link(const char *label, const char *url);
/* `id` >= 0 registers a click hitbox for this button (see render_hit_test). */
void render_button(const char *label, const char *url, int id);
void render_end_page(void);

/* Hit-test a framebuffer point against the current page's buttons.
   Returns the id of the button at (x,y), or -1. */
int  render_hit_test(uint32_t x, uint32_t y);

/* Left navigation bar. Tabs return ids >= RENDER_NAV_BASE from render_hit_test. */
#define RENDER_NAV_BASE 900
enum { NAV_HOME, NAV_SOCIAL, NAV_LAWS, NAV_YOU, NAV_EDITOR, NAV_SETTINGS, NAV_HALT, NAV_COUNT };
void render_set_nav(int active);   /* which tab is highlighted (-1 for none) */

/* Register a custom clickable region (used by the editor for lines/toolbar). */
void render_add_hit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, int id);
void render_clear_hits(void);

#endif /* ROGEX_RENDER_H */
