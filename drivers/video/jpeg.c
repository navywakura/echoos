/*
 * Baseline JPEG (SOF0, 8-bit, YCbCr or greyscale). No progressive, no CMYK.
 * Integer IDCT; output 0x00RRGGBB.
 */
#include "jpeg.h"
#ifndef JPEG_HOST
#include "pmm.h"
#endif

#define JMAXC 3
#define JMAXH 4

typedef struct {
    const uint8_t *p, *end;
    uint32_t bits;
    int      nbits;
} jbs;

typedef struct {
    uint8_t bits[17];
    uint8_t hval[256];
    uint16_t minc[17], maxc[17];
    int16_t  valptr[17];
} jhuff;

typedef struct {
    uint8_t id, h, v, tq;
    int     dcpred;
} jcomp;

static int clamp8(int x) { return x < 0 ? 0 : (x > 255 ? 255 : x); }

static void bs_init(jbs *b, const uint8_t *p, const uint8_t *end) {
    b->p = p; b->end = end; b->bits = 0; b->nbits = 0;
}
static int bs_load(jbs *b) {
    while (b->nbits <= 24 && b->p < b->end) {
        uint8_t c = *b->p++;
        if (c == 0xFF) {
            if (b->p >= b->end) return 0;
            uint8_t n = *b->p++;
            if (n != 0x00) { b->p -= 2; return 0; }
        }
        b->bits = (b->bits << 8) | c;
        b->nbits += 8;
    }
    return 1;
}
static int bs_get(jbs *b, int n) {
    if (b->nbits < n) bs_load(b);
    if (b->nbits < n) return 0;
    b->nbits -= n;
    return (int)((b->bits >> b->nbits) & ((1u << n) - 1));
}

static int huff_build(jhuff *h, const uint8_t *bits, const uint8_t *vals, int nval) {
    k_memset(h, 0, sizeof(*h));
    for (int i = 1; i <= 16; i++) h->bits[i] = bits[i];
    if (nval > 256) nval = 256;
    for (int i = 0; i < nval; i++) h->hval[i] = vals[i];
    int code = 0, k = 0;
    for (int i = 1; i <= 16; i++) {
        if (h->bits[i]) {
            h->valptr[i] = (int16_t)k;
            h->minc[i] = (uint16_t)code;
            code += h->bits[i];
            h->maxc[i] = (uint16_t)(code - 1);
            k += h->bits[i];
        } else {
            h->valptr[i] = -1;
            h->minc[i] = 0;
            h->maxc[i] = 0;
        }
        code <<= 1;
    }
    return 1;
}
static int huff_dec(jbs *b, const jhuff *h) {
    int code = 0;
    for (int i = 1; i <= 16; i++) {
        code = (code << 1) | bs_get(b, 1);
        if (h->valptr[i] >= 0 && code >= h->minc[i] && code <= h->maxc[i])
            return h->hval[h->valptr[i] + (code - h->minc[i])];
    }
    return 0;
}

/* Public-domain integer IDCT (nanojpeg / Martin J. Fiedler). */
#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565
static void idct_row(int *blk) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (!((x1 = blk[4] << 11) | (x2 = blk[6]) | (x3 = blk[2]) |
          (x4 = blk[1]) | (x5 = blk[7]) | (x6 = blk[5]) | (x7 = blk[3]))) {
        blk[0] = blk[1] = blk[2] = blk[3] = blk[4] = blk[5] = blk[6] = blk[7] = blk[0] << 3;
        return;
    }
    x0 = (blk[0] << 11) + 128;
    x8 = W7 * (x4 + x5);
    x4 = x8 + (W1 - W7) * x4;
    x5 = x8 - (W1 + W7) * x5;
    x8 = W3 * (x6 + x7);
    x6 = x8 - (W3 - W5) * x6;
    x7 = x8 - (W3 + W5) * x7;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = W6 * (x3 + x2);
    x2 = x1 - (W2 + W6) * x2;
    x3 = x1 + (W2 - W6) * x3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    blk[0] = (x7 + x1) >> 8;
    blk[1] = (x3 + x2) >> 8;
    blk[2] = (x0 + x4) >> 8;
    blk[3] = (x8 + x6) >> 8;
    blk[4] = (x8 - x6) >> 8;
    blk[5] = (x0 - x4) >> 8;
    blk[6] = (x3 - x2) >> 8;
    blk[7] = (x7 - x1) >> 8;
}
static void idct_col(int *blk) {
    int x0, x1, x2, x3, x4, x5, x6, x7, x8;
    if (!((x1 = blk[8 * 4] << 8) | (x2 = blk[8 * 6]) | (x3 = blk[8 * 2]) |
          (x4 = blk[8 * 1]) | (x5 = blk[8 * 7]) | (x6 = blk[8 * 5]) | (x7 = blk[8 * 3]))) {
        int t = (blk[0] + 32) >> 6;
        blk[0] = blk[8] = blk[16] = blk[24] = blk[32] = blk[40] = blk[48] = blk[56] = t;
        return;
    }
    x0 = (blk[0] << 8) + 8192;
    x8 = W7 * (x4 + x5) + 4;
    x4 = (x8 + (W1 - W7) * x4) >> 3;
    x5 = (x8 - (W1 + W7) * x5) >> 3;
    x8 = W3 * (x6 + x7) + 4;
    x6 = (x8 - (W3 - W5) * x6) >> 3;
    x7 = (x8 - (W3 + W5) * x7) >> 3;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = W6 * (x3 + x2) + 4;
    x2 = (x1 - (W2 + W6) * x2) >> 3;
    x3 = (x1 + (W2 - W6) * x3) >> 3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    blk[0]  = (x7 + x1) >> 14;
    blk[8]  = (x3 + x2) >> 14;
    blk[16] = (x0 + x4) >> 14;
    blk[24] = (x8 + x6) >> 14;
    blk[32] = (x8 - x6) >> 14;
    blk[40] = (x0 - x4) >> 14;
    blk[48] = (x3 - x2) >> 14;
    blk[56] = (x7 - x1) >> 14;
}
static void idct8(int *blk) {
    for (int i = 0; i < 8; i++) idct_row(blk + i * 8);
    for (int i = 0; i < 8; i++) idct_col(blk + i);
}

static const uint8_t *find_sof(const uint8_t *p, const uint8_t *end, uint32_t *w, uint32_t *h) {
    while (p + 4 < end) {
        if (p[0] != 0xFF) { p++; continue; }
        uint8_t m = p[1];
        if (m == 0xD8) { p += 2; continue; }
        if (m == 0xD9) break;
        if (m == 0x00 || m == 0xFF) { p++; continue; }
        if (p + 4 > end) break;
        uint16_t seglen = ((uint16_t)p[2] << 8) | p[3];
        if (seglen < 2 || p + 2 + seglen > end) break;
        if (m == 0xC0) {
            if (seglen < 8) return NULL;
            *h = ((uint32_t)p[5] << 8) | p[6];
            *w = ((uint32_t)p[7] << 8) | p[8];
            return p;
        }
        if (m >= 0xC1 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)
            return NULL; /* progressive / lossless — not this decoder */
        p += 2 + seglen;
    }
    return NULL;
}

bool jpeg_info(const uint8_t *data, uint32_t len, jpeg_info_t *out) {
    if (!data || !out || len < 4) return false;
    if (data[0] != 0xFF || data[1] != 0xD8) return false;
    uint32_t w = 0, h = 0;
    if (!find_sof(data, data + len, &w, &h) || !w || !h) return false;
    if ((uint64_t)w * h > (1u << 26)) return false;
    out->width = w; out->height = h;
    return true;
}

uint32_t jpeg_decode(const uint8_t *data, uint32_t len,
                     uint32_t *pixels, uint32_t max_pixels, jpeg_info_t *out) {
    jpeg_info_t info;
    if (!jpeg_info(data, len, &info) || !pixels) return 0;
    if (out) *out = info;
    uint32_t want = info.width * info.height;
    if (want > max_pixels) return 0;

    int qt[4][64];
    jhuff hdc[4], hac[4];
    jcomp comp[JMAXC];
    int ncomp = 0, precision = 8;
    int qt_ok[4] = {0, 0, 0, 0};
    int ht_ok[8] = {0};
    const uint8_t *sos = NULL, *scan_end = data + len;
    k_memset(qt, 0, sizeof(qt));
    k_memset(hdc, 0, sizeof(hdc));
    k_memset(hac, 0, sizeof(hac));
    k_memset(comp, 0, sizeof(comp));

    const uint8_t *p = data;
    const uint8_t *end = data + len;
    while (p + 2 < end) {
        if (p[0] != 0xFF) { p++; continue; }
        uint8_t m = p[1];
        if (m == 0xD8) { p += 2; continue; }
        if (m == 0xD9) break;
        if (m == 0x00 || m == 0xFF) { p++; continue; }
        if (p + 4 > end) break;
        uint16_t seglen = ((uint16_t)p[2] << 8) | p[3];
        if (seglen < 2 || p + 2 + seglen > end) break;
        const uint8_t *s = p + 4;
        const uint8_t *se = p + 2 + seglen;
        if (m == 0xDB) { /* DQT */
            while (s < se) {
                int pq = s[0] >> 4, tq = s[0] & 0x0F;
                s++;
                if (tq > 3) return 0;
                for (int i = 0; i < 64; i++) {
                    int v = pq ? (((int)s[0] << 8) | s[1]) : s[0];
                    s += pq ? 2 : 1;
                    if (s > se) return 0;
                    qt[tq][i] = v;
                }
                qt_ok[tq] = 1;
            }
        } else if (m == 0xC4) { /* DHT */
            while (s < se) {
                int tc = s[0] >> 4, th = s[0] & 0x0F;
                s++;
                if (tc > 1 || th > 3) return 0;
                uint8_t bits[17]; int nval = 0;
                bits[0] = 0;
                for (int i = 1; i <= 16; i++) {
                    if (s >= se) return 0;
                    bits[i] = *s++; nval += bits[i];
                }
                if (nval > 256 || s + nval > se) return 0;
                jhuff *hh = tc ? &hac[th] : &hdc[th];
                huff_build(hh, bits, s, nval);
                s += nval;
                ht_ok[tc * 4 + th] = 1;
            }
        } else if (m == 0xC0) { /* SOF0 */
            precision = s[0];
            ncomp = s[5];
            if (precision != 8 || ncomp < 1 || ncomp > JMAXC) return 0;
            s += 6;
            for (int i = 0; i < ncomp; i++) {
                if (s + 3 > se) return 0;
                comp[i].id = s[0];
                comp[i].h  = s[1] >> 4;
                comp[i].v  = s[1] & 0x0F;
                comp[i].tq = s[2];
                comp[i].dcpred = 0;
                if (!comp[i].h || !comp[i].v || comp[i].h > JMAXH || comp[i].v > JMAXH)
                    return 0;
                s += 3;
            }
        } else if (m == 0xDA) { /* SOS */
            sos = se;
            break;
        }
        p += 2 + seglen;
    }
    if (!sos || ncomp == 0) return 0;

    int Hmax = 1, Vmax = 1;
    for (int i = 0; i < ncomp; i++) {
        if (comp[i].h > Hmax) Hmax = comp[i].h;
        if (comp[i].v > Vmax) Vmax = comp[i].v;
        if (!qt_ok[comp[i].tq]) return 0;
    }
    int mcu_w = 8 * Hmax, mcu_h = 8 * Vmax;
    int nx = (int)((info.width  + (uint32_t)mcu_w - 1) / (uint32_t)mcu_w);
    int ny = (int)((info.height + (uint32_t)mcu_h - 1) / (uint32_t)mcu_h);

    jbs bs;
    bs_init(&bs, sos, scan_end);

    /* Per-component plane, full MCU grid. */
    int plane_w[JMAXC], plane_h[JMAXC];
    int16_t *plane[JMAXC];
    for (int c = 0; c < ncomp; c++) {
        plane_w[c] = nx * comp[c].h * 8;
        plane_h[c] = ny * comp[c].v * 8;
        uint32_t bytes = (uint32_t)plane_w[c] * (uint32_t)plane_h[c] * 2;
        uint32_t pages = (bytes + 4095) / 4096;
        uint64_t phys = pmm_alloc_contig(pages);
        if (!phys) {
            for (int j = 0; j < c; j++) { /* leak on fail is boot-only; abort */ }
            return 0;
        }
        plane[c] = (int16_t *)(uintptr_t)phys;
    }

    static const uint8_t zz[64] = {
        0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,12,19,26,33,40,48,41,34,
        27,20,13, 6, 7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
        58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
    };

    int td[JMAXC], ta[JMAXC];
    /* SOS header already consumed as `sos` pointing after length. We need
       the table selectors from the SOS segment just before `sos`. */
    {
        const uint8_t *q = data;
        const uint8_t *last = NULL;
        while (q + 4 < end) {
            if (q[0] != 0xFF) { q++; continue; }
            uint8_t mm = q[1];
            if (mm == 0xD8) { q += 2; continue; }
            if (mm == 0x00 || mm == 0xFF) { q++; continue; }
            if (q + 4 > end) break;
            uint16_t sl = ((uint16_t)q[2] << 8) | q[3];
            if (sl < 2 || q + 2 + sl > end) break;
            if (mm == 0xDA) { last = q + 4; sos = q + 2 + sl; }
            if (mm == 0xDA) break;
            q += 2 + sl;
        }
        if (!last) return 0;
        int ns = last[0];
        last++;
        if (ns != ncomp) return 0;
        for (int i = 0; i < ns; i++) {
            td[i] = last[1] >> 4;
            ta[i] = last[1] & 0x0F;
            last += 2;
        }
        bs_init(&bs, sos, scan_end);
    }

    for (int my = 0; my < ny; my++) {
        for (int mx = 0; mx < nx; mx++) {
            for (int c = 0; c < ncomp; c++) {
                if (!ht_ok[td[c]] || !ht_ok[4 + ta[c]]) return 0;
                for (int v = 0; v < comp[c].v; v++) {
                    for (int hh = 0; hh < comp[c].h; hh++) {
                        int blk[64];
                        for (int i = 0; i < 64; i++) blk[i] = 0;
                        int rs = huff_dec(&bs, &hdc[td[c]]);
                        int s = rs & 0x0F;
                        int diff = 0;
                        if (s) {
                            int vbits = bs_get(&bs, s);
                            diff = (vbits < (1 << (s - 1))) ? vbits - ((1 << s) - 1) : vbits;
                        }
                        comp[c].dcpred += diff;
                        blk[0] = comp[c].dcpred * qt[comp[c].tq][0];
                        int k = 1;
                        while (k < 64) {
                            int rsac = huff_dec(&bs, &hac[ta[c]]);
                            int rr = rsac >> 4, ss = rsac & 0x0F;
                            if (ss == 0) {
                                if (rr == 15) k += 16;
                                else break;
                            } else {
                                k += rr;
                                if (k >= 64) break;
                                int vbits = bs_get(&bs, ss);
                                int ac = (vbits < (1 << (ss - 1))) ? vbits - ((1 << ss) - 1) : vbits;
                                blk[zz[k]] = ac * qt[comp[c].tq][k];
                                k++;
                            }
                        }
                        idct8(blk);
                        int dx = (mx * comp[c].h + hh) * 8;
                        int dy = (my * comp[c].v + v) * 8;
                        for (int yy = 0; yy < 8; yy++)
                            for (int xx = 0; xx < 8; xx++)
                                plane[c][(dy + yy) * plane_w[c] + dx + xx] =
                                    (int16_t)blk[yy * 8 + xx];
                    }
                }
            }
        }
    }

    /* Upsample + YCbCr → RGB */
    for (uint32_t y = 0; y < info.height; y++) {
        for (uint32_t x = 0; x < info.width; x++) {
            int Y, Cb = 0, Cr = 0;
            if (ncomp == 1) {
                int sx = (int)x * plane_w[0] / (int)info.width;
                int sy = (int)y * plane_h[0] / (int)info.height;
                Y = plane[0][sy * plane_w[0] + sx];
                int g = clamp8(Y + 128);
                pixels[y * info.width + x] = ((uint32_t)g << 16) | ((uint32_t)g << 8) | (uint32_t)g;
                continue;
            }
            {
                int sx = (int)x * plane_w[0] / (int)info.width;
                int sy = (int)y * plane_h[0] / (int)info.height;
                Y = plane[0][sy * plane_w[0] + sx];
            }
            if (ncomp > 1) {
                int sx = (int)x * plane_w[1] / (int)info.width;
                int sy = (int)y * plane_h[1] / (int)info.height;
                Cb = plane[1][sy * plane_w[1] + sx];
            }
            if (ncomp > 2) {
                int sx = (int)x * plane_w[2] / (int)info.width;
                int sy = (int)y * plane_h[2] / (int)info.height;
                Cr = plane[2][sy * plane_w[2] + sx];
            }
            int yy = Y + 128;
            int r = clamp8(yy + ((359 * Cr) >> 8));
            int g = clamp8(yy - ((88 * Cb + 183 * Cr) >> 8));
            int b = clamp8(yy + ((454 * Cb) >> 8));
            pixels[y * info.width + x] =
                ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
    return want;
}
