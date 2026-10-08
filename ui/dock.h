/* EchOS Eclipse Shell — macOS-style dock. */
#ifndef ECHOS_DOCK_H
#define ECHOS_DOCK_H

#include "rogex.h"

void        dock_init(void);
bool        dock_pin(const char *app);            /* context menu "Pin to Dock" */
bool        dock_unpin(const char *app);
void        dock_set_running(const char *app, bool running);
void        dock_set_visible(bool visible);
void        dock_layout(uint32_t screen_w, uint32_t screen_h, uint32_t cursor_x,
                        uint32_t cursor_y);
void        dock_render(void);
int         dock_hit_test(uint32_t x, uint32_t y); /* tile index or -1          */
const char *dock_app_at(int index);
bool        dock_name_is(const char *app, const char *name);
void        dock_bounce(const char *app);    /* launch pulse animation */
bool        dock_animating(void);            /* true while the pulse runs */
bool        dock_near(uint32_t x, uint32_t y); /* cursor in magnification band */
void        dock_band(int *x, int *y, int *w, int *h); /* repaint region */

#endif /* ECHOS_DOCK_H */
