/* Compositor / GUI core. */
#ifndef RXOS_GUI_H
#define RXOS_GUI_H

#include "rogex.h"

/* --- wallpaper -----------------------------------------------------------
   Decoded once from the QOI image linked into the kernel. The image is kept
   at its native size and sampled through precomputed row/column tables, so
   there is one copy of it in RAM instead of a second, screen-sized, scaled
   one — and the scaling costs a table lookup per pixel rather than a divide. */
bool gui_wallpaper_init(void);          /* decode; false = flat colour desktop */
bool gui_wallpaper_ready(void);
const char *gui_wallpaper_status(void); /* why it is or is not there           */
const char *gui_wallpaper_name(void);   /* default | aurora | solar | …        */
void        gui_wallpaper_set_name(const char *name);

/* Replace the decoded image with a generated gradient (update recipes).
   Integer-only; keeps the existing scale tables. */
bool gui_wallpaper_fill_gradient(uint32_t from, uint32_t to,
                                 uint32_t accent, uint32_t grain);
bool gui_wallpaper_restore_default(void);  /* first packed JPEG          */
int         gui_wp_count(void);
const char *gui_wp_name_at(int i);
bool        gui_wallpaper_load_index(int i);
int         gui_wallpaper_index(void);
bool        gui_wallpaper_load_mem(const uint8_t *data, uint32_t len, const char *name);

/* Paint a region of the desktop background into the backbuffer: the wallpaper
   if there is one, otherwise the theme's background colour. Coordinates are
   screen pixels and are clipped. */
void gui_background(int x, int y, int w, int h);

/* Nearest-neighbour blit of a 0x00RRGGBB image into the backbuffer. */
void gui_blit(const uint32_t *src, uint32_t sw, uint32_t sh,
              int dx, int dy, int dw, int dh);

/* --- scene cache ---------------------------------------------------------
   gui_scene_capture() copies the backbuffer as it stands into the cache; the
   caller composes the scene it wants frozen first. gui_scene_restore() puts a
   rectangle of that frozen scene back, which is how the pixels a moving
   window used to cover are repaired without redrawing what is under them. */
bool gui_scene_ready(void);
bool gui_scene_capture(void);
void gui_scene_restore(int x, int y, int w, int h);

/* Called once the framebuffer mode is known. Allocates what it can. */
void gui_init(void);

/* How much RAM the compositor took, for `mem` and the System Monitor to
   report honestly rather than leaving it as unexplained usage. */
uint64_t gui_bytes(void);

#endif /* RXOS_GUI_H */
