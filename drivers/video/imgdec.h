/* Image sniff + decode: JPEG, PNG, QOI. Pixels are 0x00RRGGBB. */
#ifndef RXOS_IMGDEC_H
#define RXOS_IMGDEC_H

#include "rogex.h"

typedef enum { IMG_UNKNOWN = 0, IMG_JPEG, IMG_PNG, IMG_QOI } img_kind;

typedef struct {
    img_kind kind;
    uint32_t width;
    uint32_t height;
} img_info_t;

img_kind img_probe(const uint8_t *data, uint32_t len);
bool     img_info(const uint8_t *data, uint32_t len, img_info_t *out);
uint32_t img_decode(const uint8_t *data, uint32_t len,
                    uint32_t *pixels, uint32_t max_pixels, img_info_t *out);

#endif
