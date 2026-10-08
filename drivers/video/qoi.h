/* QOI image decode. */
#ifndef RXOS_QOI_H
#define RXOS_QOI_H

#include "rogex.h"

typedef struct {
    uint32_t width, height;
    uint8_t  channels;      /* 3 = RGB, 4 = RGBA (alpha is dropped on decode) */
    uint8_t  colorspace;    /* 0 = sRGB with linear alpha, 1 = all linear     */
} qoi_desc_t;

/* Parse and validate the 14-byte header without decoding. */
bool qoi_info(const uint8_t *data, uint32_t len, qoi_desc_t *out);

/* Decode into a caller-provided 0x00RRGGBB buffer. Returns the number of
   pixels written, which is less than width*height for a truncated or
   malformed file — it never writes past `max_pixels`. */
uint32_t qoi_decode(const uint8_t *data, uint32_t len,
                    uint32_t *pixels, uint32_t max_pixels,
                    qoi_desc_t *desc);

#endif /* RXOS_QOI_H */
