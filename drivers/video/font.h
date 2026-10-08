/*
 * Console typeface: JetBrains Mono, three weights, looked up by codepoint.
 *
 * The glyphs are baked by tools/mkfont.py into font_jbmono.h. This is the
 * only thing that knows how they are packed; everything else asks for a
 * codepoint and a style and gets a cell of coverage back.
 */
#ifndef ECHOS_FONT_H
#define ECHOS_FONT_H

#include "rogex.h"

typedef enum {
    FONT_REGULAR = 0,
    FONT_BOLD    = 1,
    FONT_ITALIC  = 2
} font_style_t;

uint32_t font_cell_w(void);
uint32_t font_cell_h(void);

/* Coverage cell for `cp`, or NULL when the face has no glyph for it. Ranges
   with no separate weights (box drawing, blocks, Braille) return the regular
   shape for every style, which is the honest answer: a bold box corner is
   not a different character. */
const uint8_t *font_glyph(uint32_t cp, font_style_t style);
bool           font_has(uint32_t cp);

/* Coverage of one pixel, 0..255, from a cell returned above. */
uint8_t font_coverage(const uint8_t *cell, uint32_t x, uint32_t y);

/* How many codepoints this build can draw, for `about` / diagnostics. */
uint32_t font_glyph_count(void);

/* Diagnostics the shell can print, so "it has the glyph" is checkable from
   outside: ink is the sum of coverage in the cell (0 = blank), and a style
   counts as present only when its raster actually differs from regular. */
uint32_t font_ink(uint32_t cp, font_style_t style);
bool     font_style_differs(uint32_t cp, font_style_t style);
const char *font_name(void);

#endif /* ECHOS_FONT_H */
