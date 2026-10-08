/*
 * PNG 8-bit RGB/RGBA, filter 0-4, zlib inflate (fixed + dynamic).
 * Output 0x00RRGGBB.
 */
#include "png.h"
#ifndef PNG_HOST
#include "pmm.h"
#endif

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = p - a, pb = p - b, pc = p - c;
    if (pa < 0) pa = -pa; if (pb < 0) pb = -pb; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* ---- inflate (RFC 1951) ------------------------------------------------- */
typedef struct { const uint8_t *p, *end; uint32_t bits; int nbits; } ibs;
static void ib_init(ibs *b, const uint8_t *p, uint32_t n) {
    b->p = p; b->end = p + n; b->bits = 0; b->nbits = 0;
}
static int ib_get(ibs *b, int n) {
    while (b->nbits < n) {
        if (b->p >= b->end) return 0;
        b->bits |= (uint32_t)(*b->p++) << b->nbits;
        b->nbits += 8;
    }
    int v = (int)(b->bits & ((1u << n) - 1));
    b->bits >>= n; b->nbits -= n;
    return v;
}

typedef struct { uint16_t count[16], symbol[288]; } ihuff;
static void ih_build(ihuff *h, const uint8_t *len, int n) {
    k_memset(h, 0, sizeof(*h));
    uint16_t offs[16];
    for (int i = 0; i < n; i++) if (len[i] < 16) h->count[len[i]]++;
    h->count[0] = 0;
    uint16_t code = 0;
    offs[0] = 0;
    for (int i = 1; i < 16; i++) {
        code = (uint16_t)((code + h->count[i - 1]) << 1);
        offs[i] = code;
    }
    for (int i = 0; i < n; i++)
        if (len[i]) h->symbol[offs[len[i]]++] = (uint16_t)i;
}
static int ih_dec(ibs *b, const ihuff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= ib_get(b, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t LBASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t  LBITS[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t DBASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t  DBITS[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static int inflate_block(ibs *b, uint8_t *out, uint32_t *pos, uint32_t omax,
                         const ihuff *lit, const ihuff *dist) {
    for (;;) {
        int s = ih_dec(b, lit);
        if (s < 0) return 0;
        if (s < 256) {
            if (*pos >= omax) return 0;
            out[(*pos)++] = (uint8_t)s;
        } else if (s == 256) return 1;
        else {
            s -= 257;
            if (s > 28) return 0;
            int len = LBASE[s] + ib_get(b, LBITS[s]);
            int d = ih_dec(b, dist);
            if (d < 0 || d > 29) return 0;
            int distv = DBASE[d] + ib_get(b, DBITS[d]);
            if (distv <= 0 || (uint32_t)distv > *pos) return 0;
            for (int i = 0; i < len; i++) {
                if (*pos >= omax) return 0;
                out[*pos] = out[*pos - (uint32_t)distv];
                (*pos)++;
            }
        }
    }
}

static int inflate(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dlen, uint32_t *wrote) {
    if (slen < 2) return 0;
    /* zlib header */
    int cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8) return 0;
    if (((cmf << 8) + flg) % 31) return 0;
    ibs b; ib_init(&b, src + 2, slen - 2);
    uint32_t pos = 0;
    int last;
    do {
        last = ib_get(&b, 1);
        int type = ib_get(&b, 2);
        if (type == 0) {
            b.bits = 0; b.nbits = 0;
            if (b.p + 4 > b.end) return 0;
            uint16_t len = (uint16_t)(b.p[0] | (b.p[1] << 8));
            b.p += 4;
            if (pos + len > dlen || b.p + len > b.end) return 0;
            for (uint16_t i = 0; i < len; i++) dst[pos++] = *b.p++;
        } else if (type == 1 || type == 2) {
            ihuff lit, dist;
            uint8_t ll[288], dd[32];
            if (type == 1) {
                int i;
                for (i = 0; i <= 143; i++) ll[i] = 8;
                for (; i <= 255; i++) ll[i] = 9;
                for (; i <= 279; i++) ll[i] = 7;
                for (; i <= 287; i++) ll[i] = 8;
                for (i = 0; i < 32; i++) dd[i] = 5;
            } else {
                int hlit = ib_get(&b, 5) + 257;
                int hdist = ib_get(&b, 5) + 1;
                int hclen = ib_get(&b, 4) + 4;
                static const uint8_t order[19] = {
                    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
                };
                uint8_t cl[19];
                k_memset(cl, 0, sizeof(cl));
                for (int i = 0; i < hclen; i++) cl[order[i]] = (uint8_t)ib_get(&b, 3);
                ihuff ch; ih_build(&ch, cl, 19);
                int n = 0;
                uint8_t lens[320];
                k_memset(lens, 0, sizeof(lens));
                while (n < hlit + hdist) {
                    int s = ih_dec(&b, &ch);
                    if (s < 0) return 0;
                    if (s < 16) lens[n++] = (uint8_t)s;
                    else {
                        int rep = 0, v = 0;
                        if (s == 16) { v = n ? lens[n - 1] : 0; rep = 3 + ib_get(&b, 2); }
                        else if (s == 17) { rep = 3 + ib_get(&b, 3); }
                        else { rep = 11 + ib_get(&b, 7); }
                        while (rep--) lens[n++] = (uint8_t)v;
                    }
                }
                k_memcpy(ll, lens, (uint32_t)hlit);
                k_memcpy(dd, lens + hlit, (uint32_t)hdist);
                for (int i = hlit; i < 288; i++) ll[i] = 0;
                for (int i = hdist; i < 32; i++) dd[i] = 0;
            }
            ih_build(&lit, ll, 288);
            ih_build(&dist, dd, 32);
            if (!inflate_block(&b, dst, &pos, dlen, &lit, &dist)) return 0;
        } else return 0;
    } while (!last);
    *wrote = pos;
    return 1;
}

bool png_info(const uint8_t *data, uint32_t len, png_info_t *out) {
    static const uint8_t sig[8] = { 137,80,78,71,13,10,26,10 };
    if (!data || !out || len < 24) return false;
    for (int i = 0; i < 8; i++) if (data[i] != sig[i]) return false;
    if (be32(data + 8) < 13) return false;
    if (data[12]!='I'||data[13]!='H'||data[14]!='D'||data[15]!='R') return false;
    out->width  = be32(data + 16);
    out->height = be32(data + 20);
    if (!out->width || !out->height) return false;
    if ((uint64_t)out->width * out->height > (1u << 26)) return false;
    return true;
}

uint32_t png_decode(const uint8_t *data, uint32_t len,
                    uint32_t *pixels, uint32_t max_pixels, png_info_t *out) {
    png_info_t inf;
    if (!png_info(data, len, &inf) || !pixels) return 0;
    if (out) *out = inf;
    uint32_t want = inf.width * inf.height;
    if (want > max_pixels) return 0;
    uint8_t bitd = data[24], ct = data[25], inter = data[28];
    if (bitd != 8 || inter != 0) return 0;
    if (ct != 2 && ct != 6) return 0; /* RGB or RGBA */
    int bpp = (ct == 6) ? 4 : 3;

    /* concatenate IDAT */
    uint32_t zcap = len;
    uint32_t zpages = (zcap + 4095) / 4096;
#ifndef PNG_HOST
    uint64_t zphys = pmm_alloc_contig(zpages);
    if (!zphys) return 0;
    uint8_t *zbuf = (uint8_t *)(uintptr_t)zphys;
#else
    static uint8_t zstatic[4 * 1024 * 1024];
    if (zcap > sizeof(zstatic)) return 0;
    uint8_t *zbuf = zstatic;
#endif
    uint32_t zlen = 0;
    uint32_t off = 8;
    while (off + 12 <= len) {
        uint32_t cl = be32(data + off);
        if (off + 12 + cl > len) break;
        const uint8_t *ty = data + off + 4;
        if (ty[0]=='I'&&ty[1]=='D'&&ty[2]=='A'&&ty[3]=='T') {
            if (zlen + cl > zcap) break;
            k_memcpy(zbuf + zlen, data + off + 8, cl);
            zlen += cl;
        }
        if (ty[0]=='I'&&ty[1]=='E'&&ty[2]=='N'&&ty[3]=='D') break;
        off += 12 + cl;
    }
    uint32_t raw_need = (inf.width * (uint32_t)bpp + 1) * inf.height + 64;
    uint32_t rpages = (raw_need + 4095) / 4096;
#ifndef PNG_HOST
    uint64_t rphys = pmm_alloc_contig(rpages);
    if (!rphys) return 0;
    uint8_t *raw = (uint8_t *)(uintptr_t)rphys;
#else
    static uint8_t rstatic[8 * 1024 * 1024];
    if (raw_need > sizeof(rstatic)) return 0;
    uint8_t *raw = rstatic;
#endif
    uint32_t wrote = 0;
    if (!inflate(zbuf, zlen, raw, raw_need, &wrote)) return 0;

    uint32_t stride = inf.width * (uint32_t)bpp;
    uint8_t *prev = 0;
    uint32_t rp = 0;
    for (uint32_t y = 0; y < inf.height; y++) {
        if (rp >= wrote) return 0;
        int filt = raw[rp++];
        uint8_t *row = raw + rp;
        if (rp + stride > wrote) return 0;
        for (uint32_t x = 0; x < stride; x++) {
            int a = (x >= (uint32_t)bpp) ? row[x - (uint32_t)bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= (uint32_t)bpp) ? prev[x - (uint32_t)bpp] : 0;
            int v = row[x];
            if (filt == 1) v += a;
            else if (filt == 2) v += b;
            else if (filt == 3) v += (a + b) / 2;
            else if (filt == 4) v += paeth(a, b, c);
            else if (filt != 0) return 0;
            row[x] = (uint8_t)v;
        }
        for (uint32_t x = 0; x < inf.width; x++) {
            uint8_t r = row[x * (uint32_t)bpp + 0];
            uint8_t g = row[x * (uint32_t)bpp + 1];
            uint8_t bl = row[x * (uint32_t)bpp + 2];
            pixels[y * inf.width + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | bl;
        }
        prev = row;
        rp += stride;
    }
    return want;
}
