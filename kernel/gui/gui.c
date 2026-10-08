/*
 * Compositor (lives under kernel/gui/). Damage tracking, wallpaper, layers.
 * Drag and redraw still have rough edges under load.
 */
#include "gui.h"
#include "framebuffer.h"
#include "imgdec.h"
#include "wallpaper_pack.h"
#include "pmm.h"

static uint32_t *g_img;               /* decoded wallpaper, native size       */
static uint32_t  g_iw, g_ih;
static uint32_t *g_scene;             /* frozen scene for the drag fast path  */
static uint32_t  g_sw, g_sh;
static uint64_t  g_bytes;
static const char *g_why = "not initialised";
static char      g_wp_name[32] = "default";

/* Nearest-neighbour sampling tables. g_colmap[x] is the source column for
   screen column x; g_rowmap[y] is the source row's base offset. Precomputing
   them turns per-pixel scaling into a load, and costs (w + h) entries instead
   of a second full-size image. */
static uint32_t *g_colmap, *g_rowmap;

static uint64_t alloc_px(uint32_t pixels, uint32_t **out) {
    uint64_t bytes = (uint64_t)pixels * 4;
    uint32_t pages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    uint64_t p = pmm_alloc_contig(pages);
    if (!p) { *out = NULL; return 0; }
    *out = (uint32_t *)(uintptr_t)p;
    return (uint64_t)pages * PAGE_SIZE;
}

/* ------------------------------- wallpaper -------------------------------- */
static bool rebuild_maps(uint32_t iw, uint32_t ih) {
    uint32_t sw, sh; fb_dims(&sw, &sh);
    if (!sw || !sh) return false;
    if (!g_colmap || !g_rowmap || g_iw != iw || g_ih != ih) {
        uint32_t *cm = NULL, *rm = NULL;
        uint64_t gc = alloc_px(sw, &cm);
        uint64_t gr = alloc_px(sh, &rm);
        if (!cm || !rm) return false;
        for (uint32_t x = 0; x < sw; x++) cm[x] = (uint32_t)((uint64_t)x * iw / sw);
        for (uint32_t y = 0; y < sh; y++) rm[y] = (uint32_t)((uint64_t)y * ih / sh) * iw;
        g_colmap = cm; g_rowmap = rm;
        g_bytes += gc + gr;
    }
    return true;
}

bool gui_wallpaper_load_mem(const uint8_t *data, uint32_t len, const char *name) {
    img_info_t inf;
    if (!data || !len || !img_info(data, len, &inf)) { g_why = "unknown image"; return false; }
    uint32_t *img = g_img;
    if (!img || g_iw != inf.width || g_ih != inf.height) {
        uint64_t got = alloc_px(inf.width * inf.height, &img);
        if (!img) { g_why = "out of memory"; return false; }
        g_bytes += got;
    }
    uint32_t n = img_decode(data, len, img, inf.width * inf.height, &inf);
    if (n < inf.width * inf.height) { g_why = "truncated image"; return false; }
    if (!rebuild_maps(inf.width, inf.height)) { g_why = "scale tables"; return false; }
    g_img = img; g_iw = inf.width; g_ih = inf.height;
    gui_wallpaper_set_name(name ? name : "image");
    g_why = (inf.kind == IMG_JPEG) ? "jpeg" : (inf.kind == IMG_PNG) ? "png" : "image";
    return true;
}

static int g_wp_index;
bool gui_wallpaper_load_index(int i) {
    uint32_t len = 0;
    const uint8_t *d = wp_data_at(i, &len);
    if (!d) return false;
    g_wp_index = i;
    return gui_wallpaper_load_mem(d, len, wp_name_at(i));
}
int gui_wallpaper_index(void) { return g_wp_index; }
int gui_wp_count(void) { return wp_count(); }
const char *gui_wp_name_at(int i) { return wp_name_at(i); }

bool gui_wallpaper_init(void) {
    if (g_img) return true;
    if (!fb_active()) { g_why = "no framebuffer"; return false; }
    int n = wp_count();
    for (int i = 0; i < n; i++)
        if (gui_wallpaper_load_index(i)) return true;
    g_why = "no wallpaper decoded";
    return false;
}

bool gui_wallpaper_ready(void) { return g_img != NULL; }
const char *gui_wallpaper_status(void) { return g_why; }
const char *gui_wallpaper_name(void) { return g_wp_name; }
void gui_wallpaper_set_name(const char *name) {
    if (!name || !name[0]) { g_wp_name[0] = '\0'; return; }
    uint32_t i = 0;
    while (name[i] && i + 1 < sizeof(g_wp_name)) { g_wp_name[i] = name[i]; i++; }
    g_wp_name[i] = '\0';
}

static uint32_t lerp_chan(uint32_t a, uint32_t b, uint32_t t /* 0..256 */) {
    return (a * (256 - t) + b * t) / 256;
}
static uint32_t lerp_rgb(uint32_t a, uint32_t b, uint32_t t) {
    uint32_t ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    uint32_t br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
    uint32_t r = lerp_chan(ar, br, t);
    uint32_t g = lerp_chan(ag, bg, t);
    uint32_t bl = lerp_chan(ab, bb, t);
    return 0xff000000u | (r << 16) | (g << 8) | bl;
}

bool gui_wallpaper_fill_gradient(uint32_t from, uint32_t to,
                                 uint32_t accent, uint32_t grain) {
    if (!g_img || !g_iw || !g_ih) { g_why = "no wallpaper buffer"; return false; }
    if (grain > 32) grain = 32;
    for (uint32_t y = 0; y < g_ih; y++) {
        uint32_t ty = g_ih > 1 ? (y * 256) / (g_ih - 1) : 0;
        uint32_t col = lerp_rgb(from, to, ty);
        for (uint32_t x = 0; x < g_iw; x++) {
            uint32_t half = g_iw / 2;
            uint32_t ad = half ? (x > half ? x - half : half - x) : 0;
            uint32_t tm = half ? (ad * 256) / half : 256;
            if (tm > 256) tm = 256;
            /* Wash the accent toward the horizontal centre. */
            uint32_t pix = lerp_rgb(accent, col, tm);
            if (grain) {
                uint32_t n = x * 374761393u + y * 668265263u + 0x9e3779b9u;
                n ^= n >> 13; n *= 1274126177u;
                int32_t d = (int32_t)((n >> 24) % (grain * 2 + 1)) - (int32_t)grain;
                uint32_t r = (pix >> 16) & 0xff;
                uint32_t g = (pix >> 8) & 0xff;
                uint32_t b = pix & 0xff;
                int32_t nr = (int32_t)r + d, ng = (int32_t)g + d, nb = (int32_t)b + d;
                if (nr < 0) nr = 0; if (nr > 255) nr = 255;
                if (ng < 0) ng = 0; if (ng > 255) ng = 255;
                if (nb < 0) nb = 0; if (nb > 255) nb = 255;
                pix = 0xff000000u | ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
            }
            g_img[(uint64_t)y * g_iw + x] = pix;
        }
    }
    g_why = "recipe gradient";
    return true;
}

bool gui_wallpaper_restore_default(void) {
    return gui_wallpaper_load_index(0);
}

void gui_blit(const uint32_t *src, uint32_t sw, uint32_t sh,
              int dx, int dy, int dw, int dh) {
    if (!src || !sw || !sh || dw <= 0 || dh <= 0) return;
    uint32_t stride = 0;
    uint32_t *back = fb_backbuffer(&stride);
    if (!back) return;
    uint32_t fw, fh; fb_dims(&fw, &fh);
    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    if (dx + dw > (int)fw) dw = (int)fw - dx;
    if (dy + dh > (int)fh) dh = (int)fh - dy;
    if (dw <= 0 || dh <= 0) return;
    for (int row = 0; row < dh; row++) {
        uint32_t sy = (uint32_t)row * sh / (uint32_t)dh;
        const uint32_t *s = src + (uint64_t)sy * sw;
        uint32_t *d = back + (uint64_t)(dy + row) * stride + (uint32_t)dx;
        for (int col = 0; col < dw; col++) {
            uint32_t sx = (uint32_t)col * sw / (uint32_t)dw;
            d[col] = s[sx];
        }
    }
    fb_damage((uint32_t)dx, (uint32_t)dy, (uint32_t)dw, (uint32_t)dh);
}

void gui_background(int x, int y, int w, int h) {
    uint32_t sw, sh; fb_dims(&sw, &sh);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x + w > (int)sw) w = (int)sw - x;
    if (y + h > (int)sh) h = (int)sh - y;
    if (w <= 0 || h <= 0) return;

    if (!g_img) { fb_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, RGX_BG); return; }

    uint32_t stride = 0;
    uint32_t *back = fb_backbuffer(&stride);
    if (!back) {
        /* No backbuffer: there is nothing to compose into, and per-pixel
           writes straight to VRAM for a full-screen image would be visibly
           slow. A flat desktop is the honest degradation. */
        fb_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, RGX_BG);
        return;
    }
    for (int row = 0; row < h; row++) {
        const uint32_t *src = g_img + g_rowmap[y + row];
        uint32_t *dst = back + (uint64_t)(y + row) * stride + x;
        for (int col = 0; col < w; col++) dst[col] = src[g_colmap[x + col]];
    }
    fb_damage((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}

/* ------------------------------ scene cache ------------------------------- */
bool gui_scene_ready(void) { return g_scene != NULL; }

static bool scene_alloc(void) {
    if (g_scene) return true;
    uint32_t sw, sh; fb_dims(&sw, &sh);
    if (!sw || !sh) return false;
    uint64_t got = alloc_px(sw * sh, &g_scene);
    if (!g_scene) return false;
    g_sw = sw; g_sh = sh; g_bytes += got;
    return true;
}

bool gui_scene_capture(void) {
    uint32_t stride = 0;
    uint32_t *back = fb_backbuffer(&stride);
    if (!back || !scene_alloc()) return false;
    for (uint32_t y = 0; y < g_sh; y++) {
        const uint32_t *s = back + (uint64_t)y * stride;
        uint32_t *d = g_scene + (uint64_t)y * g_sw;
        for (uint32_t x = 0; x < g_sw; x++) d[x] = s[x];
    }
    return true;
}

void gui_scene_restore(int x, int y, int w, int h) {
    uint32_t stride = 0;
    uint32_t *back = fb_backbuffer(&stride);
    if (!back || !g_scene) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x + w > (int)g_sw) w = (int)g_sw - x;
    if (y + h > (int)g_sh) h = (int)g_sh - y;
    if (w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        const uint32_t *s = g_scene + (uint64_t)(y + row) * g_sw + x;
        uint32_t *d = back + (uint64_t)(y + row) * stride + x;
        for (int col = 0; col < w; col++) d[col] = s[col];
    }
    fb_damage((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
}

void gui_init(void) {
    gui_wallpaper_init();
    scene_alloc();
}

uint64_t gui_bytes(void) { return g_bytes; }
