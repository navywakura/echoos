/*
 * Linear framebuffer: text console + GUI primitives + Aero graphics engine.
 * Default theme is Windows 7–style Aero; Settings can cycle themes.
 */
#include "framebuffer.h"
#include "handoff.h"
#include "title_icons.h"
#include "font.h"

_Static_assert(FB_GLYPH_W == 10 && FB_GLYPH_H == 22,
               "console cell must match the baked font");
#include "pit.h"
#include "version.h"

/* Multiboot2 framebuffer info tag (type 8). */

/* Runtime palette (default: Smoke Aero — black / gray / white glass). */
uint32_t RGX_BG = 0x000A0A0C, RGX_FG = 0x00E8E8EA, RGX_DIM = 0x00888890,
         RGX_PANEL = 0x0016161A, RGX_BAR = 0x00101014, RGX_BTN = 0x002A2A30,
         RGX_BTN_TXT = 0x00F2F2F4, RGX_WHITE = 0x00FFFFFF,
         RGX_NAV = 0x00222228, RGX_NAV_SEL = 0x003A3A44;
uint32_t RGX_SHADOW = 0x00000000, RGX_HIGHLIGHT = 0x00C8C8D0;
uint32_t RGX_GRAD_1 = 0x00101014, RGX_GRAD_2 = 0x001C1C22, RGX_GRAD_3 = 0x002A2A32,
         RGX_GRAD_4 = 0x003A3A44, RGX_GRAD_5 = 0x0050505A;
uint32_t RGX_TITLE_TOP = 0x00383840, RGX_TITLE_BOT = 0x0016161A;
uint32_t RGX_TITLE_UF_TOP = 0x00242428, RGX_TITLE_UF_BOT = 0x00121216;
uint32_t RGX_CLIENT = 0x00141418, RGX_BORDER = 0x00707078;
uint32_t RGX_CLOSE = 0x00C04040, RGX_START = 0x00C8C8D0;

static int g_theme = RGX_THEME_AERO;
static const char *THEME_NAMES[RGX_THEME_COUNT] = {
    "cyberpunk", "green", "amber", "ice", "mono"
};

/* Each theme: BG, FG, DIM, PANEL, BAR, BTN, BTN_TXT, WHITE, NAV, NAV_SEL. */
static const uint32_t THEMES[RGX_THEME_COUNT][10] = {
    /* cyberpunk — EchOS 1.0 (#090a0f, emerald green, cyan, dark glass) */
    { 0x00090A0F, 0x0000FF9D, 0x008892B0, 0x0012141C, 0x000E1017, 0x001A1D27, 0x0000E5FF, 0x00FFFFFF, 0x0012141C, 0x0000FF9D },
    /* green — classic Rogex terminal */
    { 0x00040a07, 0x0033ff77, 0x001a7f3b, 0x000d1a12, 0x0011cc55, 0x00124a2a, 0x00b6ffcf, 0x00d8ffe6, 0x00081410, 0x00124a2a },
    /* amber */
    { 0x000c0803, 0x00ffb038, 0x007f5a17, 0x001a1206, 0x00cc7711, 0x004a3312, 0x00ffdca0, 0x00ffe8c8, 0x00140f06, 0x004a3312 },
    /* ice */
    { 0x00040a0e, 0x0044ddff, 0x001a5a7f, 0x000d1620, 0x0011a0cc, 0x00123a4a, 0x00b6e8ff, 0x00d8f4ff, 0x00081014, 0x00123a4a },
    /* mono  — light classic gray chrome */
    { 0x00808080, 0x00000000, 0x00606060, 0x00F0F0F0, 0x00404040, 0x00E0E0E0, 0x00000000, 0x00FFFFFF, 0x00C0C0C0, 0x00808080 },
};

/* Gradient colors per theme: GRAD_1..GRAD_5 */
static const uint32_t GRADIENTS[RGX_THEME_COUNT][5] = {
    { 0x000E1017, 0x0012141C, 0x00181B26, 0x00202433, 0x002A3044 },  /* cyberpunk */
    { 0x001a8a4a, 0x0022aa5a, 0x002acc6a, 0x0033ee7a, 0x003aff8a },  /* green */
    { 0x008a5a1a, 0x00aa7a22, 0x00cc9a2a, 0x00eeba33, 0x00ffda3a },  /* amber */
    { 0x001a5a8a, 0x00227aaa, 0x002a9acc, 0x0033baee, 0x003adaff },  /* ice */
    { 0x00303030, 0x00505050, 0x00707070, 0x00909090, 0x00b0b0b0 },  /* mono */
};

/* Title / chrome extras: title_top, title_bot, uf_top, uf_bot, client, border, close, start */
static const uint32_t CHROME[RGX_THEME_COUNT][8] = {
    { 0x0012141C, 0x000E1017, 0x000E1017, 0x00090A0F, 0x00090A0F, 0x0000FF9D, 0x00FF4455, 0x0000E5FF },
    { 0x0033ee7a, 0x001a8a4a, 0x00124a2a, 0x000d1a12, 0x000d1a12, 0x0033ff77, 0x00aa3030, 0x0011cc55 },
    { 0x00ffda3a, 0x008a5a1a, 0x004a3312, 0x001a1206, 0x001a1206, 0x00ffb038, 0x00aa3030, 0x00cc7711 },
    { 0x003adaff, 0x001a5a8a, 0x00123a4a, 0x000d1620, 0x000d1620, 0x0044ddff, 0x00aa3030, 0x0011a0cc },
    { 0x00E8E8E8, 0x00A0A0A0, 0x00F0F0F0, 0x00C0C0C0, 0x00F0F0F0, 0x00404040, 0x00C04040, 0x00606060 },
};

void fb_set_theme(int t) {
    if (t < 0 || t >= RGX_THEME_COUNT) t = 0;
    g_theme = t;
    const uint32_t *c = THEMES[t];
    RGX_BG = c[0]; RGX_FG = c[1]; RGX_DIM = c[2]; RGX_PANEL = c[3]; RGX_BAR = c[4];
    RGX_BTN = c[5]; RGX_BTN_TXT = c[6]; RGX_WHITE = c[7]; RGX_NAV = c[8]; RGX_NAV_SEL = c[9];
    RGX_GRAD_1 = GRADIENTS[t][0]; RGX_GRAD_2 = GRADIENTS[t][1]; RGX_GRAD_3 = GRADIENTS[t][2];
    RGX_GRAD_4 = GRADIENTS[t][3]; RGX_GRAD_5 = GRADIENTS[t][4];
    const uint32_t *ch = CHROME[t];
    RGX_TITLE_TOP = ch[0]; RGX_TITLE_BOT = ch[1];
    RGX_TITLE_UF_TOP = ch[2]; RGX_TITLE_UF_BOT = ch[3];
    RGX_CLIENT = ch[4]; RGX_BORDER = ch[5];
    RGX_CLOSE = ch[6]; RGX_START = ch[7];
    RGX_SHADOW = 0x00000000;
    RGX_HIGHLIGHT = (t == 4) ? 0x00FFFFFF : 0x00C8C8D0;
}
int fb_theme(void) { return g_theme; }
const char *fb_theme_name(void) { return THEME_NAMES[g_theme]; }
bool fb_theme_is_aero(void) { return g_theme == 0 || g_theme == 4; }
bool fb_theme_is_dark(void) { return g_theme != 4; }

static volatile uint8_t *g_lfb;    /* the real linear framebuffer (VRAM)      */
static uint32_t *g_back;           /* offscreen backbuffer, or NULL           */
static uint32_t *g_draw;           /* where primitives write: back, else LFB  */
static uint32_t g_pitch, g_width, g_height;
static uint32_t g_stride;          /* g_draw stride in pixels                 */
static uint8_t  g_bpp;
static bool     g_active;
static uint32_t cur_x, cur_y;   /* text cursor, in pixels */

/* CLI console palette — independent of the GUI theme (which is green/cyan). */
static uint32_t g_con_fg = 0x00E8E8E8;
static uint32_t g_con_bg = 0x00000000;
static bool     g_caret_on;
static uint32_t g_caret_x, g_caret_y;

void fb_console_set_color(uint32_t fg, uint32_t bg) {
    g_con_fg = fg;
    g_con_bg = bg;
}

static inline uint32_t align_up8(uint32_t x) { return (x + 7) & ~7u; }

/* ============================ damage tracking =============================
   Every primitive records the rectangle it touched; fb_present() copies only
   those rectangles from the backbuffer to VRAM. This is what removes the
   black flashes: the screen never shows a half-drawn frame, because the
   half-drawn frame only ever exists in RAM.

   The list is small on purpose. When it fills up, the new rectangle is merged
   into the first one — the union always covers both, so the invariant "every
   dirtied pixel lies inside some rectangle" is preserved; we only lose
   precision, never correctness. */
#define FB_MAX_DAMAGE 16
typedef struct { uint32_t x0, y0, x1, y1; } fb_rect;   /* [x0,x1) x [y0,y1)   */
static fb_rect g_dmg[FB_MAX_DAMAGE];
static int     g_dmg_n;

static inline uint32_t min_u(uint32_t a, uint32_t b) { return a < b ? a : b; }
static inline uint32_t max_u(uint32_t a, uint32_t b) { return a > b ? a : b; }

void fb_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_active || !g_back || w == 0 || h == 0) return;
    if (x >= g_width || y >= g_height) return;
    uint32_t x1 = min_u(x + w, g_width), y1 = min_u(y + h, g_height);
    if (x1 <= x || y1 <= y) return;

    if (g_dmg_n < FB_MAX_DAMAGE) {
        g_dmg[g_dmg_n++] = (fb_rect){ x, y, x1, y1 };
        return;
    }
    g_dmg[0].x0 = min_u(g_dmg[0].x0, x);  g_dmg[0].y0 = min_u(g_dmg[0].y0, y);
    g_dmg[0].x1 = max_u(g_dmg[0].x1, x1); g_dmg[0].y1 = max_u(g_dmg[0].y1, y1);
}

void fb_damage_all(void)   { g_dmg_n = 0; fb_damage(0, 0, g_width, g_height); }
void fb_damage_reset(void) { g_dmg_n = 0; }

void fb_present(void) {
    if (!g_active || !g_back || g_dmg_n == 0) return;
    for (int r = 0; r < g_dmg_n; r++) {
        fb_rect d = g_dmg[r];
        uint32_t rw = d.x1 - d.x0;
        for (uint32_t y = d.y0; y < d.y1; y++) {
            const uint32_t *src = g_back + (uint64_t)y * g_width + d.x0;
            volatile uint32_t *dst = (volatile uint32_t *)
                (g_lfb + (uint64_t)y * g_pitch + (uint64_t)d.x0 * 4);
            for (uint32_t i = 0; i < rw; i++) dst[i] = src[i];
        }
    }
    g_dmg_n = 0;
}

bool fb_double_buffered(void) { return g_back != NULL; }
uint64_t fb_backbuffer_bytes(void) {
    if (!g_back || !g_width || !g_height) return 0;
    return (uint64_t)g_width * g_height * 4;
}

uint32_t *fb_backbuffer(uint32_t *stride_px) {
    if (stride_px) *stride_px = g_stride;
    return g_back;
}

uint32_t fb_get_pixel(uint32_t x, uint32_t y) {
    if (!g_active || x >= g_width || y >= g_height) return 0;
    if (g_draw)
        return g_draw[(uint64_t)y * g_stride + x];
    if (g_lfb)
        return *(volatile uint32_t *)(g_lfb + (uint64_t)y * g_pitch + (uint64_t)x * 4u);
    return 0;
}

/* ---------------------------- low-level pixels ---------------------------
   These write to g_draw, which is the backbuffer when we have one. Note the
   distinction between g_stride (pixels per row in g_draw) and g_pitch (bytes
   per row in VRAM): the firmware pads VRAM rows, our backbuffer does not. */
static inline void putpx(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= g_width || y >= g_height) return;
    g_draw[(uint64_t)y * g_stride + x] = color;
}
static inline uint32_t peekpx(uint32_t x, uint32_t y) {
    if (x >= g_width || y >= g_height) return 0;
    return g_draw[(uint64_t)y * g_stride + x];
}

/* Clip a rectangle to the screen. False if nothing is left. */
static bool clip(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) {
    if (!g_active || *w == 0 || *h == 0) return false;
    if (*x >= g_width || *y >= g_height) return false;
    if (*x + *w > g_width)  *w = g_width - *x;
    if (*y + *h > g_height) *h = g_height - *y;
    return *w != 0 && *h != 0;
}

/* Clip once, then fill whole rows. The old version called putpx per pixel,
   which meant two bounds comparisons and a multiply for every single pixel of
   every rectangle — and wrote straight to VRAM, which is far slower than RAM.
   Filling a full screen this way was the single biggest cost in a repaint. */
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!clip(&x, &y, &w, &h)) return;
    for (uint32_t j = 0; j < h; j++) {
        uint32_t *row = g_draw + (uint64_t)(y + j) * g_stride + x;
        for (uint32_t i = 0; i < w; i++) row[i] = color;
    }
    fb_damage(x, y, w, h);
}

void fb_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!g_active || x >= g_width || y >= g_height) return;
    putpx(x, y, color);
    fb_damage(x, y, 1, 1);
}

void fb_blend_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   uint32_t color, uint32_t alpha) {
    if (!clip(&x, &y, &w, &h)) return;
    if (alpha >= 255) { fb_fill_rect(x, y, w, h, color); return; }
    if (alpha == 0) return;
    uint32_t ia = 255 - alpha;
    uint32_t cr = (color >> 16) & 0xff, cg = (color >> 8) & 0xff, cb = color & 0xff;
    for (uint32_t j = 0; j < h; j++) {
        uint32_t *row = g_draw + (uint64_t)(y + j) * g_stride + x;
        for (uint32_t i = 0; i < w; i++) {
            uint32_t p = row[i];
            uint32_t r = (((p >> 16) & 0xff) * ia + cr * alpha) / 255;
            uint32_t g = (((p >>  8) & 0xff) * ia + cg * alpha) / 255;
            uint32_t b = (( p        & 0xff) * ia + cb * alpha) / 255;
            row[i] = (r << 16) | (g << 8) | b;
        }
    }
    fb_damage(x, y, w, h);
}

/* Draw a rectangle with beveled edges and drop shadow for depth effect */
void fb_rect_bevel(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t base_color, bool raised) {
    if (!g_active || w < 4 || h < 4) return;
    
    /* Drop shadow (offset by 3 pixels down-right) */
    uint32_t shadow_offset = 3;
    for (uint32_t i = 0; i < w; i++) {
        if (y + h - 1 + shadow_offset < g_height) putpx(x + i, y + h - 1 + shadow_offset, RGX_SHADOW);
        if (y + h + shadow_offset < g_height) putpx(x + i, y + h + shadow_offset, RGX_SHADOW);
    }
    for (uint32_t j = 0; j < shadow_offset; j++) {
        if (x + w - 1 + shadow_offset < g_width) putpx(x + w - 1 + shadow_offset, y + j, RGX_SHADOW);
        if (x + w + shadow_offset < g_width) putpx(x + w + shadow_offset, y + j, RGX_SHADOW);
    }
    for (uint32_t i = 0; i < shadow_offset; i++) {
        if (y + h + i < g_height && x + w + i < g_width) 
            putpx(x + w + i, y + h + i, RGX_SHADOW);
    }
    
    /* Base panel */
    fb_fill_rect(x, y, w, h, base_color);
    
    /* Bevel edges */
    uint32_t light = raised ? RGX_HIGHLIGHT : RGX_SHADOW;
    uint32_t dark = raised ? RGX_SHADOW : RGX_HIGHLIGHT;
    
    /* Top and left edges (light) */
    putpx(x, y, light);
    putpx(x + 1, y, light);
    putpx(x, y + 1, light);
    for (uint32_t i = 2; i < w - 2; i++) putpx(x + i, y, light);
    for (uint32_t j = 2; j < h - 2; j++) putpx(x, y + j, light);
    
    /* Bottom and right edges (dark) */
    putpx(x + w - 1, y, dark);
    putpx(x + w - 1, y + 1, dark);
    putpx(x, y + h - 1, dark);
    putpx(x + 1, y + h - 1, dark);
    for (uint32_t i = 2; i < w - 2; i++) putpx(x + i, y + h - 1, dark);
    for (uint32_t j = 2; j < h - 2; j++) putpx(x + w - 1, y + j, dark);
    
    fb_damage(x, y, w + shadow_offset, h + shadow_offset);
}

void fb_rect_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!g_active || w == 0 || h == 0) return;
    for (uint32_t i = 0; i < w; i++) { putpx(x + i, y, color); putpx(x + i, y + h - 1, color); }
    for (uint32_t j = 0; j < h; j++) { putpx(x, y + j, color); putpx(x + w - 1, y + j, color); }
    fb_damage(x, y, w, h);
}

/* Lerp two 24-bit colours. ratio is 0..255. */
static uint32_t lerp_color(uint32_t a, uint32_t b, uint32_t ratio) {
    uint8_t r1 = (a >> 16) & 0xFF, g1 = (a >> 8) & 0xFF, b1 = a & 0xFF;
    uint8_t r2 = (b >> 16) & 0xFF, g2 = (b >> 8) & 0xFF, b2 = b & 0xFF;
    uint8_t r = (uint8_t)(r1 + ((int)r2 - (int)r1) * (int)ratio / 255);
    uint8_t g = (uint8_t)(g1 + ((int)g2 - (int)g1) * (int)ratio / 255);
    uint8_t bl = (uint8_t)(b1 + ((int)b2 - (int)b1) * (int)ratio / 255);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | bl;
}

/* Draw a rectangle with vertical gradient (top_color at top, bottom_color at bottom) */
void fb_gradient_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t top_color, uint32_t bottom_color) {
    if (!clip(&x, &y, &w, &h)) return;
    if (h == 0) return;
    if (h == 1) {
        fb_fill_rect(x, y, w, 1, top_color);
        return;
    }
    for (uint32_t j = 0; j < h; j++) {
        uint32_t ratio = (j * 255) / (h - 1);
        uint32_t color = lerp_color(top_color, bottom_color, ratio);
        uint32_t *row = g_draw + (uint64_t)(y + j) * g_stride + x;
        for (uint32_t i = 0; i < w; i++) row[i] = color;
    }
    fb_damage(x, y, w, h);
}

void fb_hgradient_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t left_color, uint32_t right_color) {
    if (!clip(&x, &y, &w, &h)) return;
    if (w == 0) return;
    if (w == 1) {
        fb_fill_rect(x, y, 1, h, left_color);
        return;
    }
    for (uint32_t i = 0; i < w; i++) {
        uint32_t ratio = (i * 255) / (w - 1);
        uint32_t color = lerp_color(left_color, right_color, ratio);
        for (uint32_t j = 0; j < h; j++) putpx(x + i, y + j, color);
    }
    fb_damage(x, y, w, h);
}

/* Soft multi-layer drop shadow (Smoke Aero: dark penumbra). */
void fb_shadow_soft(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_active || w < 4 || h < 4) return;
    static const uint32_t layers[5] = {
        0x00101010, 0x00181818, 0x00202020, 0x00282828, 0x00303030
    };
    for (int layer = 0; layer < 5; layer++) {
        int o = layer + 2;
        uint32_t c = layers[layer];
        fb_blend_rect(x + (uint32_t)o, y + h + (uint32_t)layer,
                      w > (uint32_t)o ? w - (uint32_t)o : 1, 1, c, (uint32_t)(40 + layer * 20));
        fb_blend_rect(x + w + (uint32_t)layer, y + (uint32_t)o,
                      1, h > (uint32_t)o ? h - (uint32_t)o : 1, c, (uint32_t)(40 + layer * 20));
    }
}

static int in_round(int px, int py, int x, int y, int w, int h, int r) {
    int dx, dy;
    if (r < 1) return 1;
    if (px >= x + r && px < x + w - r) return 1;
    if (py >= y + r && py < y + h - r) return 1;
    if (px < x + r && py < y + r) { dx = x + r - px; dy = y + r - py; return dx * dx + dy * dy <= r * r; }
    if (px >= x + w - r && py < y + r) { dx = px - (x + w - 1 - r); dy = y + r - py; return dx * dx + dy * dy <= r * r; }
    if (px < x + r && py >= y + h - r) { dx = x + r - px; dy = py - (y + h - 1 - r); return dx * dx + dy * dy <= r * r; }
    dx = px - (x + w - 1 - r); dy = py - (y + h - 1 - r);
    return dx * dx + dy * dy <= r * r;
}

void fb_round_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color) {
    uint32_t i, j;
    if (!clip(&x, &y, &w, &h)) return;
    for (j = 0; j < h; j++) {
        uint32_t *row = g_draw + (uint64_t)(y + j) * g_stride + x;
        for (i = 0; i < w; i++)
            if (in_round((int)(x + i), (int)(y + j), (int)x, (int)y, (int)w, (int)h, (int)r))
                row[i] = color;
    }
    fb_damage(x, y, w, h);
}

void fb_round_blend(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r,
                    uint32_t color, uint32_t alpha) {
    uint32_t i, j, ia, cr, cg, cb;
    if (!clip(&x, &y, &w, &h)) return;
    if (alpha >= 255) { fb_round_fill(x, y, w, h, r, color); return; }
    if (alpha == 0) return;
    ia = 255 - alpha;
    cr = (color >> 16) & 0xff; cg = (color >> 8) & 0xff; cb = color & 0xff;
    for (j = 0; j < h; j++) {
        uint32_t *row = g_draw + (uint64_t)(y + j) * g_stride + x;
        for (i = 0; i < w; i++) {
            uint32_t p, rr, gg, bb;
            if (!in_round((int)(x + i), (int)(y + j), (int)x, (int)y, (int)w, (int)h, (int)r))
                continue;
            p = row[i];
            rr = (((p >> 16) & 0xff) * ia + cr * alpha) / 255;
            gg = (((p >>  8) & 0xff) * ia + cg * alpha) / 255;
            bb = (( p        & 0xff) * ia + cb * alpha) / 255;
            row[i] = (rr << 16) | (gg << 8) | bb;
        }
    }
    fb_damage(x, y, w, h);
}

void fb_round_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color) {
    uint32_t i, j;
    if (!g_active || w < 2 || h < 2) return;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            int inside = in_round((int)(x + i), (int)(y + j), (int)x, (int)y, (int)w, (int)h, (int)r);
            int inset  = in_round((int)(x + i), (int)(y + j), (int)(x + 1), (int)(y + 1),
                                  (int)(w > 2 ? w - 2 : 1), (int)(h > 2 ? h - 2 : 1),
                                  r > 1 ? (int)r - 1 : 0);
            if (inside && !inset) putpx(x + i, y + j, color);
        }
    }
    fb_damage(x, y, w, h);
}

void fb_glass_panel(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r,
                    uint32_t tint, uint32_t alpha, bool shine) {
    uint32_t band, t;
    if (w < 4 || h < 4) return;
    fb_round_blend(x, y, w, h, r, tint, alpha);
    /* Specular reflection: lighter band on the upper third, not a flat fade. */
    if (h > 6)
        fb_round_blend(x + 1, y + 1, w > 2 ? w - 2 : w, h / 3,
                       r > 1 ? r - 1 : 0, 0x00FFFFFF, shine ? 55 : 28);
    fb_round_outline(x, y, w, h, r, lerp_color(tint, 0x00FFFFFF, 90));
    if (h > 2)
        fb_fill_rect(x + r, y, w > 2 * r ? w - 2 * r : 1, 1, 0x00E0E0E8);
    if (!shine || w < 16) return;
    /* Slow moving highlight — Aero glass catching a light. */
    t = (uint32_t)(pit_uptime_ms() / 28);
    band = t % (w + 48);
    if (band < w) {
        uint32_t bw = 18;
        if (band + bw > w) bw = w - band;
        fb_blend_rect(x + band, y + 1, bw, h > 4 ? h / 2 : h, 0x00FFFFFF, 38);
    }
}

void fb_blit_rgba(const uint32_t *src, uint32_t sw, uint32_t sh, int dx, int dy) {
    uint32_t i, j;
    if (!g_active || !src || !sw || !sh) return;
    for (j = 0; j < sh; j++) {
        int yy = dy + (int)j;
        if (yy < 0 || (uint32_t)yy >= g_height) continue;
        for (i = 0; i < sw; i++) {
            int xx = dx + (int)i;
            uint32_t px, a, ia, cr, cg, cb, p, rr, gg, bb;
            if (xx < 0 || (uint32_t)xx >= g_width) continue;
            px = src[j * sw + i];
            a = (px >> 24) & 0xff;
            if (a < 8) continue;
            cr = (px >> 16) & 0xff; cg = (px >> 8) & 0xff; cb = px & 0xff;
            if (a >= 250) {
                putpx((uint32_t)xx, (uint32_t)yy, (cr << 16) | (cg << 8) | cb);
                continue;
            }
            p = g_draw[(uint64_t)yy * g_stride + (uint32_t)xx];
            ia = 255 - a;
            rr = (((p >> 16) & 0xff) * ia + cr * a) / 255;
            gg = (((p >>  8) & 0xff) * ia + cg * a) / 255;
            bb = (( p        & 0xff) * ia + cb * a) / 255;
            putpx((uint32_t)xx, (uint32_t)yy, (rr << 16) | (gg << 8) | bb);
        }
    }
    if (dx < (int)g_width && dy < (int)g_height) {
        uint32_t bx = dx < 0 ? 0 : (uint32_t)dx;
        uint32_t by = dy < 0 ? 0 : (uint32_t)dy;
        fb_damage(bx, by, sw, sh);
    }
}

/* Focused / unfocused Smoke Aero title bar (glass + specular). */
void fb_aero_titlebar(uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool focused) {
    if (h == 0 || w == 0) return;
    fb_glass_panel(x, y, w, h, 0, focused ? RGX_TITLE_TOP : RGX_TITLE_UF_TOP,
                   focused ? 200 : 170, focused);
}

/* Glass taskbar strip: dark smoke + top highlight. */
void fb_aero_taskbar(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (h == 0) return;
    fb_blend_rect(x, y, w, h, RGX_BAR, 200);
    fb_blend_rect(x, y, w, h > 6 ? h / 3 : 1, 0x00FFFFFF, 36);
    fb_fill_rect(x, y, w, 1, 0x00C8C8D0);
    fb_fill_rect(x, y + h - 1, w, 1, 0x00000000);
}

/* Glass push-button (taskbar / start). */
void fb_aero_button(uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool pressed, bool primary) {
    if (w < 4 || h < 4) return;
    uint32_t tint = primary ? RGX_START : (pressed ? 0x00202026 : 0x00383840);
    uint32_t a = pressed ? 210 : 180;
    fb_round_blend(x, y, w, h, 4, tint, a);
    fb_round_blend(x + 1, y + 1, w > 2 ? w - 2 : w, h / 3, 3, 0x00FFFFFF, primary ? 70 : 40);
    fb_round_outline(x, y, w, h, 4, primary ? 0x00E8E8F0 : 0x00606068);
}

/* Caption buttons in the title bar. Returns the x of the close box (for hit tests). */
uint32_t fb_aero_caption(uint32_t win_x, uint32_t win_y, uint32_t win_w, uint32_t title_h,
                         bool focused, bool show_min_max) {
    uint32_t bw = 16, bh = title_h > 6 ? title_h - 6 : title_h;
    uint32_t by = win_y + (title_h > bh ? (title_h - bh) / 2 : 0);
    uint32_t close_x = win_x + win_w - bw - 4;
    uint32_t ct = focused ? lerp_color(RGX_CLOSE, 0x00FFFFFF, 40) : 0x00404044;
    uint32_t cb = focused ? RGX_CLOSE : 0x0028282C;
    fb_round_fill(close_x, by, bw, bh, 3, cb);
    fb_round_blend(close_x, by, bw, bh / 2, 3, ct, 120);
    fb_blit_rgba(title_icon(2), 14, 14, close_x + (bw > 14 ? (bw - 14) / 2 : 0),
                 by + (bh > 14 ? (bh - 14) / 2 : 0));
    if (show_min_max && win_w > 80) {
        uint32_t max_x = close_x - bw - 2;
        uint32_t min_x = max_x - bw - 2;
        uint32_t gb = focused ? 0x00383840 : 0x00202024;
        fb_round_fill(max_x, by, bw, bh, 3, gb);
        fb_round_blend(max_x, by, bw, bh / 2, 3, 0x00FFFFFF, 40);
        fb_blit_rgba(title_icon(1), 14, 14, max_x + (bw > 14 ? (bw - 14) / 2 : 0),
                     by + (bh > 14 ? (bh - 14) / 2 : 0));
        fb_round_fill(min_x, by, bw, bh, 3, gb);
        fb_round_blend(min_x, by, bw, bh / 2, 3, 0x00FFFFFF, 40);
        fb_blit_rgba(title_icon(0), 14, 14, min_x + (bw > 14 ? (bw - 14) / 2 : 0),
                     by + (bh > 14 ? (bh - 14) / 2 : 0));
    }
    return close_x;
}

/* Blend two colours by an 8-bit coverage, per channel, integer only. The
   +127 rounds instead of truncating: without it every antialiased edge sits
   one step darker than it should and the text looks muddy. */
static inline uint32_t blend(uint32_t bg, uint32_t fg, uint8_t a) {
    uint32_t r, g, b;
    if (a == 0)   return bg;
    if (a == 255) return fg;
    r = (((fg >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * (255u - a) + 127u) / 255u;
    g = (((fg >>  8) & 0xFF) * a + ((bg >>  8) & 0xFF) * (255u - a) + 127u) / 255u;
    b = (( fg        & 0xFF) * a + ( bg        & 0xFF) * (255u - a) + 127u) / 255u;
    return (r << 16) | (g << 8) | b;
}

/* One cell of the console face. `cp` is a Unicode codepoint, not a byte: the
   console decodes UTF-8 before it gets here, so ñ, ─ and ⣿ are ordinary
   glyphs rather than special cases. */
void fb_glyph_cp(uint32_t x, uint32_t y, uint32_t cp, int style,
                 uint32_t fg, uint32_t bg) {
    const uint8_t *cell = font_glyph(cp, (font_style_t)style);
    if (!cell) cell = font_glyph(0xFFFD, (font_style_t)style);   /* replacement */
    for (uint32_t row = 0; row < FB_GLYPH_H; row++)
        for (uint32_t col = 0; col < FB_GLYPH_W; col++)
            putpx(x + col, y + row,
                  blend(bg, fg, cell ? font_coverage(cell, col, row) : 0));
    fb_damage(x, y, FB_GLYPH_W, FB_GLYPH_H);
}

void fb_glyph_cp_trans(uint32_t x, uint32_t y, uint32_t cp, int style, uint32_t fg) {
    const uint8_t *cell = font_glyph(cp, (font_style_t)style);
    if (!cell) return;
    for (uint32_t row = 0; row < FB_GLYPH_H; row++)
        for (uint32_t col = 0; col < FB_GLYPH_W; col++) {
            uint8_t a = font_coverage(cell, col, row);
            if (a) putpx(x + col, y + row, blend(peekpx(x + col, y + row), fg, a));
        }
    fb_damage(x, y, FB_GLYPH_W, FB_GLYPH_H);
}

static void draw_glyph(uint32_t x, uint32_t y, unsigned char ch, uint32_t fg, uint32_t bg) {
    fb_glyph_cp(x, y, ch, FONT_REGULAR, fg, bg);
}

static void draw_glyph_trans(uint32_t x, uint32_t y, unsigned char ch, uint32_t fg) {
    fb_glyph_cp_trans(x, y, ch, FONT_REGULAR, fg);
}

void fb_text_n(uint32_t x, uint32_t y, const char *s, int n, uint32_t fg, uint32_t bg) {
    if (!g_active || !s || n <= 0) return;
    uint32_t x0 = x;
    int i;
    for (i = 0; i < n; i++, x += FB_GLYPH_W) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch == 0x7f) ch = ' ';
        draw_glyph(x, y, ch, fg, bg);
    }
    if (x > x0) fb_damage(x0, y, x - x0, FB_GLYPH_H);
}

void fb_text(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg) {
    if (!g_active || !s) return;
    int n = 0;
    while (s[n]) n++;
    fb_text_n(x, y, s, n, fg, bg);
}

void fb_text_trans(uint32_t x, uint32_t y, const char *s, uint32_t fg) {
    if (!g_active) return;
    uint32_t x0 = x;
    for (; *s; s++, x += FB_GLYPH_W) draw_glyph_trans(x, y, (unsigned char)*s, fg);
    if (x > x0) fb_damage(x0, y, x - x0, FB_GLYPH_H);
}

/* ------------------------------ text console ----------------------------- */
void fb_caret(int on);

void fb_clear(void) {
    if (!g_active) return;
    g_caret_on = false;
#if ECHOS_CLI_ONLY
    fb_fill_rect(0, 0, g_width, g_height, g_con_bg);
#else
    fb_fill_rect(0, 0, g_width, g_height, RGX_BG);
#endif
    cur_x = 4;
    cur_y = 2;
}

static void fb_scroll(void) {
    /* Shift the visible area up by one text row (16 px). The old version
       copied one *byte* at a time across the whole framebuffer, straight in
       VRAM — several million uncached accesses per scrolled line. Move whole
       pixels within the backbuffer instead. */
    uint32_t shift = FB_GLYPH_H * g_stride;
    uint64_t total = (uint64_t)g_stride * g_height;
    for (uint64_t i = 0; i + shift < total; i++) g_draw[i] = g_draw[i + shift];
    fb_fill_rect(0, g_height - FB_GLYPH_H, g_width, FB_GLYPH_H, g_con_bg);
    cur_y -= FB_GLYPH_H;
    fb_damage_all();
}

#define FB_CONS_COLS 80
#define FB_CONS_X0   4

static void fb_advance(void) {
    cur_x += FB_GLYPH_W;
    if (cur_x >= FB_CONS_X0 + FB_CONS_COLS * FB_GLYPH_W) {
        cur_x = FB_CONS_X0;
        cur_y += FB_GLYPH_H;
    }
    if (cur_y + FB_GLYPH_H > g_height) fb_scroll();
}

void fb_putc(char c) {
    if (!g_active) return;
    if (g_caret_on) fb_caret(0);
    if (c == '\r') { cur_x = FB_CONS_X0; fb_present(); return; }
    if (c == '\n') { cur_x = FB_CONS_X0; cur_y += FB_GLYPH_H; }
    else if (c == '\b') {
        if (cur_x >= FB_CONS_X0 + FB_GLYPH_W) cur_x -= FB_GLYPH_W;
        else if (cur_y >= 2 + FB_GLYPH_H) {
            cur_y -= FB_GLYPH_H;
            cur_x = FB_CONS_X0 + (FB_CONS_COLS - 1) * FB_GLYPH_W;
        }
    } else {
        if (cur_x >= FB_CONS_X0 + FB_CONS_COLS * FB_GLYPH_W) {
            cur_x = FB_CONS_X0;
            cur_y += FB_GLYPH_H;
        }
        if (cur_y + FB_GLYPH_H > g_height) fb_scroll();
        draw_glyph(cur_x, cur_y, (unsigned char)c, g_con_fg, g_con_bg);
        fb_damage(cur_x, cur_y, FB_GLYPH_W, FB_GLYPH_H);
        fb_advance();
        fb_present();
        return;
    }
    if (cur_y + FB_GLYPH_H > g_height) fb_scroll();
    fb_present();
}


/* XOR invert so blinking never wipes the glyph under the caret. */
static void caret_xor(uint32_t x, uint32_t y) {
    uint32_t r, c;
    for (r = 0; r < FB_GLYPH_H; r++) {
        for (c = 0; c < FB_GLYPH_W; c++) {
            uint32_t p = peekpx(x + c, y + r);
            putpx(x + c, y + r, p ^ 0x00FFFFFFu);
        }
    }
    fb_damage(x, y, FB_GLYPH_W, FB_GLYPH_H);
}

void fb_caret(int on) {
    if (!g_active) return;
    if (g_caret_on) {
        caret_xor(g_caret_x, g_caret_y);
        g_caret_on = false;
    }
    if (!on) { fb_present(); return; }
    g_caret_x = cur_x;
    g_caret_y = cur_y;
    caret_xor(cur_x, cur_y);
    g_caret_on = true;
    fb_present();
}

void fb_set_cursor_px(uint32_t x, uint32_t y) { cur_x = x; cur_y = y; }
uint32_t fb_cursor_y(void) { return cur_y; }

bool fb_active(void) { return g_active; }
void fb_dims(uint32_t *w, uint32_t *h) { if (w) *w = g_width; if (h) *h = g_height; }

/* The mode is whatever the firmware gave us — 1280x720 under QEMU, 1366x768 on
   a laptop panel. Report the real one instead of the one we asked for. */
const char *fb_mode_string(void) {
    static char s[24];
    if (!g_active) return "text (no linear framebuffer)";
    char *p = s;
    uint32_t vals[3] = { g_width, g_height, g_bpp };
    for (int i = 0; i < 3; i++) {
        uint32_t v = vals[i];
        char d[12]; int n = 0;
        do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) *p++ = d[--n];
        if (i < 2) *p++ = 'x';
    }
    *p = '\0';
    return s;
}

/* ------------------------------ mouse cursor ----------------------------- */
/* Classic arrow: '.' transparent, 'X' black outline, 'O' white fill. */
#define CUR_W 12
#define CUR_H 19
static const char *CURSOR[CUR_H] = {
    "X...........",
    "XX..........",
    "XOX.........",
    "XOOX........",
    "XOOOX.......",
    "XOOOOX......",
    "XOOOOOX.....",
    "XOOOOOOX....",
    "XOOOOOOOX...",
    "XOOOOOOOOX..",
    "XOOOOOOOOOX.",
    "XOOOOOXXXXXX",
    "XOOXOOX.....",
    "XOX.XOOX....",
    "XX..XOOX....",
    "X....XOOX...",
    ".....XOOX...",
    "......XOOX..",
    ".......XX...",
};
#define CUR_BLACK 0x00000000u
#define CUR_FILL  0x00FFFFFFu

static uint32_t g_cur_bg[CUR_W * CUR_H];
static int      g_cx, g_cy;
static bool     g_cur_vis;

static uint32_t getpx(uint32_t x, uint32_t y) {
    if (x >= g_width || y >= g_height) return 0;
    return g_draw[(uint64_t)y * g_stride + x];
}

void fb_cursor_init(void) {
    g_cx = (int)g_width / 2;
    g_cy = (int)g_height / 2;
    g_cur_vis = false;
}

static void cursor_save(void) {
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            g_cur_bg[j * CUR_W + i] = getpx(g_cx + i, g_cy + j);
}
static void cursor_restore(void) {
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++)
            putpx(g_cx + i, g_cy + j, g_cur_bg[j * CUR_W + i]);
}
static void cursor_blit(void) {
    for (int j = 0; j < CUR_H; j++)
        for (int i = 0; i < CUR_W; i++) {
            char c = CURSOR[j][i];
            if (c == 'X') putpx(g_cx + i, g_cy + j, CUR_BLACK);
            else if (c == 'O') putpx(g_cx + i, g_cy + j, CUR_FILL);
        }
}

/* The cursor saves and restores through g_draw like everything else, so it
   composites correctly against the backbuffer. Each operation damages exactly
   the 12x19 box it touched, which is why moving the mouse costs a ~900-byte
   blit rather than a screen repaint. */
void fb_cursor_hide(void) {
    if (!g_active || !g_cur_vis) return;
    cursor_restore();
    fb_damage((uint32_t)g_cx, (uint32_t)g_cy, CUR_W, CUR_H);
    g_cur_vis = false;
}
void fb_cursor_show(void) {
    if (!g_active || g_cur_vis) return;
    cursor_save();
    cursor_blit();
    fb_damage((uint32_t)g_cx, (uint32_t)g_cy, CUR_W, CUR_H);
    g_cur_vis = true;
}
void fb_cursor_moveto(int x, int y) {
    if (!g_active) return;
    bool was = g_cur_vis;
    fb_cursor_hide();
    if (x < 0) x = 0; if (x > (int)g_width - 2) x = (int)g_width - 2;
    if (y < 0) y = 0; if (y > (int)g_height - 2) y = (int)g_height - 2;
    g_cx = x; g_cy = y;
    if (was) fb_cursor_show();
    fb_present();          /* old box + new box, nothing else */
}
void fb_cursor_get(uint32_t *x, uint32_t *y) { if (x) *x = g_cx; if (y) *y = g_cy; }

/* ------------------------------- init ------------------------------------ */
void fb_init(const struct boot_handoff *bh) {
    g_active = false;
    if (!bh || !bh->fb_present || bh->fb_bpp != 32 || !bh->fb_addr) return;

    {
        uint64_t addr = bh->fb_addr;
        uint32_t pitch = bh->fb_pitch, width = bh->fb_width, height = bh->fb_height;
        /* Map the LFB (identity). The boot code covers the first 4 GiB with
           huge pages, but a discrete card — or a UEFI GOP framebuffer — can
           sit above that, so map it explicitly rather than assume. */
        uint64_t bytes = (uint64_t)pitch * height;
        uint64_t back_bytes = (uint64_t)width * height * 4;
        uint32_t pages = (uint32_t)((back_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
        uint64_t bb;
        for (uint64_t off = 0; off < bytes; off += PAGE_SIZE)
            vmm_map_page(addr + off, addr + off, PTE_WRITABLE);
        g_lfb = (volatile uint8_t *)(uintptr_t)addr;
        g_pitch = pitch; g_width = width; g_height = height; g_bpp = bh->fb_bpp;

        /* Offscreen backbuffer: tightly packed, one uint32 per pixel, no
           firmware row padding. Drawing into cached RAM and blitting the
           changed rectangles is both faster than drawing straight to VRAM and
           free of the half-drawn frames that showed up as black flashes. If
           the allocation fails we fall back to drawing directly on the LFB —
           same output, old flicker. */
        bb = pmm_alloc_contig(pages);
        if (bb) {
            g_back   = (uint32_t *)(uintptr_t)bb;
            g_draw   = g_back;
            g_stride = width;
        } else {
            g_back   = NULL;
            g_draw   = (uint32_t *)(uintptr_t)addr;
            g_stride = pitch / 4;
        }
        g_active = true;
        g_dmg_n  = 0;
        fb_set_theme(RGX_THEME_AERO);   /* Windows 7 Aero default */
        fb_clear();
        fb_present();
    }
}
