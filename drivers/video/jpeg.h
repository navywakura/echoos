/* Baseline JPEG (SOF0) decoder. Output is 0x00RRGGBB, same as the FB. */
#ifndef RXOS_JPEG_H
#define RXOS_JPEG_H

#ifndef JPEG_HOST
#include "rogex.h"
#endif

typedef struct {
    uint32_t width;
    uint32_t height;
} jpeg_info_t;

bool     jpeg_info(const uint8_t *data, uint32_t len, jpeg_info_t *out);
uint32_t jpeg_decode(const uint8_t *data, uint32_t len,
                     uint32_t *pixels, uint32_t max_pixels, jpeg_info_t *out);

#endif
