/*
 * icon_loader.h — EchOS 1.0 GUI Icon Header
 */
#ifndef RXOS_ICON_LOADER_H
#define RXOS_ICON_LOADER_H

#include "rogex.h"

typedef struct {
    uint32_t  width;
    uint32_t  height;
    uint32_t *pixel_data;
} Icon;

extern Icon g_default_icon;

Icon* get_app_icon(Icon *app_custom_icon);
void  draw_app_tile(uint32_t x, uint32_t y, const char *title, Icon *custom_icon);

#endif /* RXOS_ICON_LOADER_H */
