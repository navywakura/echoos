#include "imgdec.h"
#include "jpeg.h"
#include "png.h"
#include "qoi.h"

img_kind img_probe(const uint8_t *data, uint32_t len) {
    if (!data || len < 8) return IMG_UNKNOWN;
    if (data[0] == 0xFF && data[1] == 0xD8) return IMG_JPEG;
    if (data[0]==0x89 && data[1]=='P' && data[2]=='N' && data[3]=='G') return IMG_PNG;
    if (data[0]=='q' && data[1]=='o' && data[2]=='i' && data[3]=='f') return IMG_QOI;
    return IMG_UNKNOWN;
}

bool img_info(const uint8_t *data, uint32_t len, img_info_t *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    out->kind = img_probe(data, len);
    if (out->kind == IMG_JPEG) {
        jpeg_info_t j; if (!jpeg_info(data, len, &j)) return false;
        out->width = j.width; out->height = j.height; return true;
    }
    if (out->kind == IMG_PNG) {
        png_info_t p; if (!png_info(data, len, &p)) return false;
        out->width = p.width; out->height = p.height; return true;
    }
    if (out->kind == IMG_QOI) {
        qoi_desc_t q; if (!qoi_info(data, len, &q)) return false;
        out->width = q.width; out->height = q.height; return true;
    }
    return false;
}

uint32_t img_decode(const uint8_t *data, uint32_t len,
                    uint32_t *pixels, uint32_t max_pixels, img_info_t *out) {
    img_info_t inf;
    if (!img_info(data, len, &inf)) return 0;
    if (out) *out = inf;
    if (inf.kind == IMG_JPEG) {
        jpeg_info_t j;
        return jpeg_decode(data, len, pixels, max_pixels, &j);
    }
    if (inf.kind == IMG_PNG) {
        png_info_t p;
        return png_decode(data, len, pixels, max_pixels, &p);
    }
    if (inf.kind == IMG_QOI) {
        qoi_desc_t q;
        return qoi_decode(data, len, pixels, max_pixels, &q);
    }
    return 0;
}
