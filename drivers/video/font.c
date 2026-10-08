#include "font.h"
#include "font_jbmono.h"

uint32_t font_cell_w(void) { return JB_CELL_W; }
uint32_t font_cell_h(void) { return JB_CELL_H; }
uint32_t font_glyph_count(void) { return JB_GLYPHS; }
const char *font_name(void) { return "JetBrains Mono"; }

/* The ranges are sorted and disjoint, so a binary search finds the block a
   codepoint lives in without a 64 K table. */
static const jb_range_t *range_of(uint32_t cp) {
    uint32_t lo = 0, hi = JB_RANGE_COUNT;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const jb_range_t *r = &JB_RANGES[mid];
        if (cp < r->first)      hi = mid;
        else if (cp > r->last)  lo = mid + 1;
        else                    return r;
    }
    return NULL;
}

bool font_has(uint32_t cp) { return range_of(cp) != NULL; }

const uint8_t *font_glyph(uint32_t cp, font_style_t style) {
    const jb_range_t *r = range_of(cp);
    uint32_t off;
    if (!r) return NULL;
    off = cp - r->first;
    if (style != FONT_REGULAR && r->styled_base != JB_NO_STYLE) {
        uint32_t i = r->styled_base + off;
        if (i < JB_STYLED_GLYPHS)
            return style == FONT_BOLD ? JB_BOLD[i] : JB_ITALIC[i];
    }
    return JB_REGULAR[r->base + off];
}

/* Two pixels per byte, high nibble first; a nibble scales to 8 bits by *17
   (0x0 -> 0, 0xF -> 255) with no rounding error at either end. */
uint8_t font_coverage(const uint8_t *cell, uint32_t x, uint32_t y) {
    uint32_t i;
    uint8_t b;
    if (!cell || x >= JB_CELL_W || y >= JB_CELL_H) return 0;
    i = y * JB_CELL_W + x;
    b = cell[i >> 1];
    return (uint8_t)(((i & 1) ? (b & 0x0F) : (b >> 4)) * 17u);
}

uint32_t font_ink(uint32_t cp, font_style_t style) {
    const uint8_t *cell = font_glyph(cp, style);
    uint32_t x, y, sum = 0;
    if (!cell) return 0;
    for (y = 0; y < JB_CELL_H; y++)
        for (x = 0; x < JB_CELL_W; x++) sum += font_coverage(cell, x, y);
    return sum;
}

bool font_style_differs(uint32_t cp, font_style_t style) {
    const uint8_t *a = font_glyph(cp, FONT_REGULAR);
    const uint8_t *b = font_glyph(cp, style);
    uint32_t i;
    if (!a || !b) return false;
    for (i = 0; i < JB_CELL_BYTES; i++)
        if (a[i] != b[i]) return true;
    return false;
}
