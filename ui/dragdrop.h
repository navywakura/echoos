/* EchOS Eclipse Shell — global drag & drop. */
#ifndef ECHOS_DRAGDROP_H
#define ECHOS_DRAGDROP_H

#include "rogex.h"

enum { DRAG_ICON_FILE = 0, DRAG_ICON_APP, DRAG_ICON_TEXT };

enum {
    DRAG_ZONE_NONE = 0,
    DRAG_ZONE_DESKTOP,
    DRAG_ZONE_DOCK,
    DRAG_ZONE_WINDOW,
    DRAG_ZONE_TRASH,
};

typedef struct {
    bool     accepted;
    int      zone_kind;
    void    *zone_ctx;     /* window*, dock tile index (cast), ... */
    void    *payload;      /* what was passed to dragdrop_press    */
    uint32_t x, y;
} dragdrop_result;

void dragdrop_reset(void);
bool dragdrop_press(uint32_t x, uint32_t y, const char *label, int kind, void *payload);
bool dragdrop_update(uint32_t x, uint32_t y);          /* true once dragging  */
int  dragdrop_add_zone(int kind, uint32_t x, uint32_t y,
                       uint32_t w, uint32_t h, void *ctx);
void dragdrop_clear_zones(void);
bool dragdrop_active(void);
bool dragdrop_drop(uint32_t x, uint32_t y, dragdrop_result *out);
void dragdrop_cancel(void);
void dragdrop_render(void);

#endif /* ECHOS_DRAGDROP_H */
