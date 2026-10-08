/* Framebuffer + theme API + Aero graphics engine primitives. */
#ifndef ROGEX_FB_H
#define ROGEX_FB_H

#include "rogex.h"

/* Parse the Multiboot2 framebuffer tag, map the LFB, clear it. Safe to call
   even if no framebuffer is present (fb_active() then returns false). */
struct boot_handoff;
void fb_init(const struct boot_handoff *bh);
bool fb_active(void);
void fb_dims(uint32_t *w, uint32_t *h);
uint32_t fb_get_pixel(uint32_t x, uint32_t y);   /* backbuffer, else LFB */
const char *fb_mode_string(void);   /* the mode we actually got, "WxHxD" */

/* Text console (used by console.c when the framebuffer is active). */
void fb_clear(void);
void fb_putc(char c);
/* Draw one Unicode codepoint. style is font_style_t (0 regular, 1 bold,
   2 italic). The _trans form leaves the background alone. */
void fb_glyph_cp(uint32_t x, uint32_t y, uint32_t cp, int style,
                 uint32_t fg, uint32_t bg);
void fb_glyph_cp_trans(uint32_t x, uint32_t y, uint32_t cp, int style, uint32_t fg);
void fb_console_set_color(uint32_t fg, uint32_t bg);
void fb_caret(int on);                           /* blinking block at text cursor */
void fb_set_cursor_px(uint32_t x, uint32_t y);   /* place text cursor        */
uint32_t fb_cursor_y(void);

/* --- double buffering ------------------------------------------------------
   Primitives draw into an offscreen backbuffer and record the rectangles they
   touched; fb_present() copies only those rectangles to VRAM. Callers that
   paint a whole screen should present once at the end instead of letting the
   user watch it being assembled — that intermediate state is what used to
   read as a black flash. All of this degrades to a no-op when the backbuffer
   could not be allocated, so no caller has to care whether it exists. */
bool fb_double_buffered(void);
uint64_t fb_backbuffer_bytes(void);     /* 0 if drawing straight to VRAM */
void fb_present(void);                  /* blit the damaged rectangles        */
/* The backbuffer itself, for the compositor's scene cache (kernel/gui). NULL
   when there is no backbuffer; *stride_px is pixels per row, which equals the
   width because the backbuffer, unlike VRAM, has no firmware row padding. */
uint32_t *fb_backbuffer(uint32_t *stride_px);
void fb_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void fb_damage_all(void);               /* next present rewrites the screen   */
void fb_damage_reset(void);             /* drop what was recorded             */

/* GUI primitives (32bpp 0x00RRGGBB). */
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_pixel(uint32_t x, uint32_t y, uint32_t color);
/* Blend `color` over the pixels already in the backbuffer. alpha 0 = invisible,
   255 = opaque. Used for the glass lock/installer cards. */
void fb_blend_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint32_t color, uint32_t alpha);
void fb_rect_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_rect_bevel(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t base_color, bool raised);
void fb_gradient_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t top_color, uint32_t bottom_color);
void fb_hgradient_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t left_color, uint32_t right_color);
void fb_text(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg);
void fb_text_n(uint32_t x, uint32_t y, const char *s, int n, uint32_t fg, uint32_t bg);
/* Text with transparent background (keeps whatever is already under the glyphs). */
void fb_text_trans(uint32_t x, uint32_t y, const char *s, uint32_t fg);

/* --- Aero graphics engine (Windows 7–style chrome) -----------------------
   Soft shadows, glass taskbar strips, classic caption buttons, and title-bar
   gradients. The window manager uses these so the desktop reads as classic
   Win7 Aero rather than a flat terminal palette. */
void fb_shadow_soft(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void fb_aero_titlebar(uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool focused);
void fb_aero_taskbar(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void fb_aero_button(uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool pressed, bool primary);
/* Smoke Aero: rounded glass. tint is 0x00RRGGBB; alpha 0..255 over wallpaper. */
void fb_round_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color);
void fb_round_blend(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r,
                    uint32_t color, uint32_t alpha);
void fb_round_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color);
void fb_glass_panel(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r,
                    uint32_t tint, uint32_t alpha, bool shine);
void fb_blit_rgba(const uint32_t *src, uint32_t sw, uint32_t sh, int dx, int dy);
/* Caption button strip: close (red), optional min/max. Returns close-box x. */
uint32_t fb_aero_caption(uint32_t win_x, uint32_t win_y, uint32_t win_w, uint32_t title_h,
                         bool focused, bool show_min_max);

/* Software mouse cursor (classic arrow). Drawn on top with save/restore. */
void fb_cursor_init(void);
void fb_cursor_hide(void);
void fb_cursor_show(void);
void fb_cursor_moveto(int x, int y);
void fb_cursor_get(uint32_t *x, uint32_t *y);

/* Rogex palette — runtime (Settings can switch the theme). */
extern uint32_t RGX_BG, RGX_FG, RGX_DIM, RGX_PANEL, RGX_BAR, RGX_BTN, RGX_BTN_TXT, RGX_WHITE;
extern uint32_t RGX_NAV, RGX_NAV_SEL;
extern uint32_t RGX_SHADOW, RGX_HIGHLIGHT;
extern uint32_t RGX_GRAD_1, RGX_GRAD_2, RGX_GRAD_3, RGX_GRAD_4, RGX_GRAD_5;
/* Aero chrome colours (always valid; match the active theme's accent). */
extern uint32_t RGX_TITLE_TOP, RGX_TITLE_BOT, RGX_TITLE_UF_TOP, RGX_TITLE_UF_BOT;
extern uint32_t RGX_CLIENT, RGX_BORDER, RGX_CLOSE, RGX_START;

void        fb_set_theme(int theme);   /* 0 smoke, 1 green, 2 amber, 3 ice, 4 mono */
int         fb_theme(void);
const char *fb_theme_name(void);
bool        fb_theme_is_aero(void);    /* glass chrome (smoke + mono) */
bool        fb_theme_is_dark(void);    /* smoke / green / amber / ice */
#define RGX_THEME_COUNT 5
#define RGX_THEME_AERO  0

/* Console cell. Must match the baked face (drivers/video/font_jbmono.h);
   framebuffer.c asserts it at compile time. */
#define FB_GLYPH_W 10
#define FB_GLYPH_H 22

#endif /* ROGEX_FB_H */
