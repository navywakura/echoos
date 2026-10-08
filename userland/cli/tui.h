/*
 * echOS TUI — buffered cell grid, presented once per frame.
 * JetBrains Mono 10x22, antialiased. Black background, white text, colour
 * accents.
 */
#ifndef ECHOS_TUI_H
#define ECHOS_TUI_H

#include "rogex.h"

#define TUI_COLS 80
#define TUI_ROWS 25

#define TUI_BG     0x00000000u
#define TUI_NAVY   TUI_BG
#define TUI_FG     0x00F2F2F2u
#define TUI_ACCENT 0x005EC8F0u
#define TUI_GOLD   0x00F5C542u
#define TUI_DIM    0x008A8A8Au
#define TUI_HI_BG  0x002A5A8Cu
#define TUI_HI_FG  0x00FFFFFFFFu
#define TUI_RED    0x00E85D4Cu
#define TUI_BORDER 0x004A90B8u

typedef enum {
    TUI_KEY_NONE = 0,
    TUI_KEY_UP,
    TUI_KEY_DOWN,
    TUI_KEY_LEFT,
    TUI_KEY_RIGHT,
    TUI_KEY_ENTER,
    TUI_KEY_ESC,
    TUI_KEY_BACK,
    TUI_KEY_TAB,
    TUI_KEY_CHAR,
    TUI_KEY_Q,
} tui_key;

void     tui_begin(void);
void     tui_end(void);
void     tui_cell(int col, int row, uint32_t cp, uint32_t fg, uint32_t bg);
void     tui_text(int col, int row, const char *s, uint32_t fg, uint32_t bg);
void     tui_fill(int col, int row, int w, int h, uint32_t cp, uint32_t fg, uint32_t bg);
void     tui_box(int col, int row, int w, int h, const char *title);
void     tui_hbar(int col, int row, int w, uint32_t fg);
void     tui_item(int col, int row, int w, const char *label, bool selected);
void     tui_hint(int row, const char *s);
tui_key  tui_read_key(void);
char     tui_last_char(void);

#endif
