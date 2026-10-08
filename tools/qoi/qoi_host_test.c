/* Host-side QOI checks. Run on the build machine, not in the guest. */

#include <stdio.h>
#include <stdlib.h>
typedef unsigned char  uint8_t;
typedef unsigned int   uint32_t;
typedef unsigned long long uint64_t;
typedef int bool;
#define true 1
#define false 0

typedef struct {

    uint32_t width, height;
    uint8_t  channels;      /* 3 = RGB, 4 = RGBA (alpha is dropped on decode) */
    uint8_t  colorspace;    /* 0 = sRGB with linear alpha, 1 = all linear     */
} qoi_desc_t;





/* Inlined QOI decoder (same logic as drivers/video/qoi.c). One pass, no zlib — wallpaper-friendly. */


#define QOI_OP_INDEX 0x00   /* 00xxxxxx */
#define QOI_OP_DIFF  0x40   /* 01xxxxxx */
#define QOI_OP_LUMA  0x80   /* 10xxxxxx */
#define QOI_OP_RUN   0xC0   /* 11xxxxxx */
#define QOI_OP_RGB   0xFE
#define QOI_OP_RGBA  0xFF
#define QOI_MASK_2   0xC0

#define QOI_HEADER_SIZE 14

typedef struct { uint8_t r, g, b, a; } qoi_rgba_t;

/* The format's own index hash. Fixed by the spec, not a design choice. */
static inline int qoi_hash(qoi_rgba_t p) {
    return (p.r * 3 + p.g * 5 + p.b * 7 + p.a * 11) % 64;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

bool qoi_info(const uint8_t *data, uint32_t len, qoi_desc_t *out) {
    if (!data || !out || len < QOI_HEADER_SIZE) return false;
    if (data[0] != 'q' || data[1] != 'o' || data[2] != 'i' || data[3] != 'f')
        return false;
    out->width      = be32(data + 4);
    out->height     = be32(data + 8);
    out->channels   = data[12];
    out->colorspace = data[13];
    if (out->width == 0 || out->height == 0) return false;
    if (out->channels != 3 && out->channels != 4) return false;
    /* A header claiming more pixels than could possibly be encoded in the
       remaining bytes is malformed; reject it before allocating anything. */
    if ((uint64_t)out->width * out->height > (uint64_t)1 << 28) return false;
    return true;
}

uint32_t qoi_decode(const uint8_t *data, uint32_t len,
                    uint32_t *pixels, uint32_t max_pixels,
                    qoi_desc_t *desc) {
    qoi_desc_t d;
    if (!qoi_info(data, len, &d)) return 0;
    if (desc) *desc = d;
    if (!pixels || max_pixels == 0) return 0;

    uint64_t want = (uint64_t)d.width * d.height;
    uint32_t total = want > max_pixels ? max_pixels : (uint32_t)want;

    qoi_rgba_t index[64];
    for (int i = 0; i < 64; i++) { index[i].r = index[i].g = index[i].b = 0; index[i].a = 0; }

    qoi_rgba_t px = { 0, 0, 0, 255 };
    uint32_t p = QOI_HEADER_SIZE;
    uint32_t run = 0;

    for (uint32_t n = 0; n < total; n++) {
        if (run > 0) {
            run--;
        } else if (p < len) {
            uint8_t b1 = data[p++];

            if (b1 == QOI_OP_RGB) {
                if (p + 3 > len) return n;          /* truncated: stop clean  */
                px.r = data[p]; px.g = data[p+1]; px.b = data[p+2];
                p += 3;
            } else if (b1 == QOI_OP_RGBA) {
                if (p + 4 > len) return n;
                px.r = data[p]; px.g = data[p+1]; px.b = data[p+2]; px.a = data[p+3];
                p += 4;
            } else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX) {
                px = index[b1 & 0x3F];
            } else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF) {
                /* Two bits per channel, biased by 2, wrapping in uint8. */
                px.r = (uint8_t)(px.r + ((b1 >> 4) & 0x03) - 2);
                px.g = (uint8_t)(px.g + ((b1 >> 2) & 0x03) - 2);
                px.b = (uint8_t)(px.b + ( b1       & 0x03) - 2);
            } else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA) {
                if (p >= len) return n;
                uint8_t b2 = data[p++];
                int vg = (int)(b1 & 0x3F) - 32;     /* green delta, bias 32   */
                px.r = (uint8_t)(px.r + vg - 8 + ((b2 >> 4) & 0x0F));
                px.g = (uint8_t)(px.g + vg);
                px.b = (uint8_t)(px.b + vg - 8 + ( b2       & 0x0F));
            } else {  /* QOI_OP_RUN */
                run = (uint32_t)(b1 & 0x3F);        /* stored with bias -1    */
            }
            index[qoi_hash(px)] = px;
        } else {
            return n;                               /* ran out of input       */
        }
        /* 0x00RRGGBB, matching the framebuffer's 32bpp layout. Alpha is
           dropped: the compositor has no blending and would ignore it. */
        pixels[n] = ((uint32_t)px.r << 16) | ((uint32_t)px.g << 8) | px.b;
    }
    return total;
}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) { puts("open failed"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n); fread(buf, 1, n, f); fclose(f);
    qoi_desc_t d;
    if (!qoi_info(buf, n, &d)) { puts("bad header"); return 1; }
    printf("header: %ux%u ch=%u cs=%u\n", d.width, d.height, d.channels, d.colorspace);
    uint32_t *px = malloc((size_t)d.width * d.height * 4);
    uint32_t got = qoi_decode(buf, n, px, d.width * d.height, &d);
    printf("decoded %u of %u pixels\n", got, d.width * d.height);
    if (got != d.width * d.height) { puts("INCOMPLETE"); return 1; }
    FILE *o = fopen(argv[2], "wb");
    fprintf(o, "P6\n%u %u\n255\n", d.width, d.height);
    for (uint32_t i = 0; i < got; i++) {
        uint8_t rgb[3] = { px[i] >> 16, (px[i] >> 8) & 0xff, px[i] & 0xff };
        fwrite(rgb, 1, 3, o);
    }
    fclose(o);
    puts("OK");
    return 0;
}
