#ifndef RXOS_PNG_H
#define RXOS_PNG_H

#include "rogex.h"

typedef struct { uint32_t width, height; } png_info_t;

bool     png_info(const uint8_t *data, uint32_t len, png_info_t *out);
uint32_t png_decode(const uint8_t *data, uint32_t len,
                    uint32_t *pixels, uint32_t max_pixels, png_info_t *out);

#endif
