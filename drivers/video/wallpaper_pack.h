#ifndef RXOS_WALLPAPER_PACK_H
#define RXOS_WALLPAPER_PACK_H

#include "rogex.h"

typedef struct {
    const uint8_t *data;
    uint64_t       len;
    const char    *name;
} rxos_wp_ent;

extern const uint64_t    rxos_wp_count;
extern const rxos_wp_ent rxos_wp_table[];

int         wp_count(void);
const char *wp_name_at(int i);
const uint8_t *wp_data_at(int i, uint32_t *len);

#endif
