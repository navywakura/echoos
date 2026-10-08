#ifndef RXOS_ICON_PACK_H
#define RXOS_ICON_PACK_H

#include "rogex.h"

typedef struct {
    const uint8_t *data;
    uint64_t       len;
    const char    *name;
} rxos_icon_ent;

extern const uint64_t      rxos_icon_count;
extern const rxos_icon_ent rxos_icon_table[];
extern const uint32_t      rxos_icon_dim[2];

int         icon_count(void);
int         icon_dim(void);
const char *icon_name_at(int i);
/* 48x48 0xAARRGGBB little-endian. NULL if missing. */
const uint32_t *icon_rgba_at(int i);
int         icon_index_by_name(const char *name);

#endif
