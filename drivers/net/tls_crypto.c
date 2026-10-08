/* SHA-256, HMAC, TLS-PRF, AES-128-GCM, P-256 ECDH, RSA PKCS#1 v1.5. */
#include "tls_crypto.h"

#ifdef RXOS_HOSTTEST
#include <stddef.h>
#include <stdio.h>
#else
#include "rogex.h"
#endif

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void wr32be(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static void wr64be(uint8_t *p, uint64_t v) {
    wr32be(p, (uint32_t)(v >> 32)); wr32be(p + 4, (uint32_t)v);
}

/* ---------------- SHA-256 ------------------------------------------------ */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_block(uint32_t h[8], const uint8_t blk[64]) {
    uint32_t w[64], a,b,c,d,e,f,g,hh, t1, t2;
    int i;
    for (i = 0; i < 16; i++) w[i] = rd32be(blk + i * 4);
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15]>>3);
        uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=h[0]; b=h[1]; c=h[2]; d=h[3]; e=h[4]; f=h[5]; g=h[6]; hh=h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

void sha256(const uint8_t *in, uint32_t n, uint8_t out[32]) {
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    uint8_t blk[64];
    uint64_t bits = (uint64_t)n * 8;
    uint32_t i, rem = n;
    const uint8_t *p = in;
    while (rem >= 64) { sha256_block(h, p); p += 64; rem -= 64; }
    k_memset(blk, 0, 64);
    if (rem) k_memcpy(blk, p, rem);
    blk[rem] = 0x80;
    if (rem >= 56) { sha256_block(h, blk); k_memset(blk, 0, 64); }
    wr64be(blk + 56, bits);
    sha256_block(h, blk);
    for (i = 0; i < 8; i++) wr32be(out + i * 4, h[i]);
}

void hmac_sha256(const uint8_t *key, uint32_t klen,
                 const uint8_t *msg, uint32_t mlen, uint8_t out[32]) {
    uint8_t k[64], ipad[64], opad[64], inner[32], cat[64 + 2048];
    uint32_t i;
    k_memset(k, 0, 64);
    if (klen > 64) sha256(key, klen, k);
    else if (klen) k_memcpy(k, key, klen);
    for (i = 0; i < 64; i++) { ipad[i] = (uint8_t)(k[i] ^ 0x36); opad[i] = (uint8_t)(k[i] ^ 0x5c); }
    if (mlen > 2048) mlen = 2048;
    k_memcpy(cat, ipad, 64);
    if (mlen) k_memcpy(cat + 64, msg, mlen);
    sha256(cat, 64 + mlen, inner);
    k_memcpy(cat, opad, 64);
    k_memcpy(cat + 64, inner, 32);
    sha256(cat, 96, out);
}

void hkdf_extract(const uint8_t *salt, uint32_t slen,
                  const uint8_t *ikm, uint32_t ilen, uint8_t out[32]) {
    static const uint8_t z[32] = { 0 };
    if (!salt || !slen) hmac_sha256(z, 32, ikm ? ikm : z, ikm ? ilen : 0, out);
    else hmac_sha256(salt, slen, ikm ? ikm : z, ikm ? ilen : 0, out);
}

void tls13_expand_label(const uint8_t *secret,
                        const char *label,
                        const uint8_t *ctx, uint32_t ctxlen,
                        uint8_t *out, uint32_t outlen) {
    uint8_t info[96], t[32], prev[32];
    uint32_t n = 0, labellen = 6, i, produced = 0, round = 1, prevlen = 0;
    const char *p = "tls13 ";
    char full[40];
    int f = 0;
    while (*p) full[f++] = *p++;
    p = label; while (*p && f < 39) full[f++] = *p++;
    labellen = (uint32_t)f;
    info[n++] = (uint8_t)(outlen >> 8);
    info[n++] = (uint8_t)outlen;
    info[n++] = (uint8_t)labellen;
    k_memcpy(info + n, full, labellen); n += labellen;
    info[n++] = (uint8_t)ctxlen;
    if (ctxlen && ctx && n + ctxlen <= sizeof(info)) {
        k_memcpy(info + n, ctx, ctxlen); n += ctxlen;
    }
    while (produced < outlen) {
        uint8_t msg[128];
        uint32_t m = 0, k;
        if (prevlen) { k_memcpy(msg, prev, prevlen); m = prevlen; }
        k_memcpy(msg + m, info, n); m += n;
        msg[m++] = (uint8_t)round;
        hmac_sha256(secret, 32, msg, m, t);
        k_memcpy(prev, t, 32); prevlen = 32;
        for (k = 0; k < 32 && produced < outlen; k++) out[produced++] = t[k];
        round++;
        (void)i;
    }
}

void tls_prf(const uint8_t *sec, uint32_t slen,
             const char *label,
             const uint8_t *seed, uint32_t seedlen,
             uint8_t *out, uint32_t outlen) {
    uint8_t a[32], buf[32 + 64 + 256], seed2[64 + 256];
    uint32_t lablen = 0, sl, produced = 0, n;
    const uint8_t *lp = (const uint8_t *)label;
    while (lp[lablen]) lablen++;
    if (lablen + seedlen > sizeof(seed2)) return;
    k_memcpy(seed2, lp, lablen);
    if (seedlen) k_memcpy(seed2 + lablen, seed, seedlen);
    sl = lablen + seedlen;
    hmac_sha256(sec, slen, seed2, sl, a); /* A(1) */
    while (produced < outlen) {
        k_memcpy(buf, a, 32);
        k_memcpy(buf + 32, seed2, sl);
        hmac_sha256(sec, slen, buf, 32 + sl, buf);
        n = outlen - produced;
        if (n > 32) n = 32;
        k_memcpy(out + produced, buf, n);
        produced += n;
        hmac_sha256(sec, slen, a, 32, a); /* A(i+1) */
    }
}

/* ---------------- AES-128 ------------------------------------------------ */
static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};
static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static void aes_keyexp(const uint8_t key[16], uint8_t rk[176]) {
    static const uint8_t rcon[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
    int i;
    k_memcpy(rk, key, 16);
    for (i = 4; i < 44; i++) {
        uint8_t t[4];
        t[0]=rk[(i-1)*4]; t[1]=rk[(i-1)*4+1]; t[2]=rk[(i-1)*4+2]; t[3]=rk[(i-1)*4+3];
        if ((i % 4) == 0) {
            uint8_t tmp = t[0];
            t[0]=SBOX[t[1]] ^ rcon[i/4 - 1]; t[1]=SBOX[t[2]]; t[2]=SBOX[t[3]]; t[3]=SBOX[tmp];
        }
        rk[i*4]   = (uint8_t)(rk[(i-4)*4]   ^ t[0]);
        rk[i*4+1] = (uint8_t)(rk[(i-4)*4+1] ^ t[1]);
        rk[i*4+2] = (uint8_t)(rk[(i-4)*4+2] ^ t[2]);
        rk[i*4+3] = (uint8_t)(rk[(i-4)*4+3] ^ t[3]);
    }
}

static void aes_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    int r, i;
    for (i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
    for (r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (i = 0; i < 16; i++) t[i] = SBOX[s[i]];
        s[0]=t[0]; s[4]=t[4]; s[8]=t[8]; s[12]=t[12];
        { uint8_t a=t[1]; s[1]=t[5]; s[5]=t[9]; s[9]=t[13]; s[13]=a; }
        { uint8_t a=t[2], b=t[6]; s[2]=t[10]; s[6]=t[14]; s[10]=a; s[14]=b; }
        { uint8_t a=t[15]; s[15]=t[11]; s[11]=t[7]; s[7]=t[3]; s[3]=a; }
        if (r < 10) {
            for (i = 0; i < 4; i++) {
                uint8_t *c = s + i * 4;
                uint8_t a=c[0], b=c[1], cc=c[2], d=c[3];
                c[0] = (uint8_t)(xtime(a) ^ xtime(b) ^ b ^ cc ^ d);
                c[1] = (uint8_t)(a ^ xtime(b) ^ xtime(cc) ^ cc ^ d);
                c[2] = (uint8_t)(a ^ b ^ xtime(cc) ^ xtime(d) ^ d);
                c[3] = (uint8_t)(xtime(a) ^ a ^ b ^ cc ^ xtime(d));
            }
        }
        for (i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];
    }
    k_memcpy(out, s, 16);
}

/* GHASH over GF(2^128) with R = 0xe1 << 120 */
static void gcm_mult(uint8_t x[16], const uint8_t h[16]) {
    uint8_t z[16], v[16];
    int i, j;
    k_memset(z, 0, 16);
    k_memcpy(v, h, 16);
    for (i = 0; i < 16; i++) {
        for (j = 7; j >= 0; j--) {
            int k;
            if (x[i] & (1u << j)) {
                for (k = 0; k < 16; k++) z[k] ^= v[k];
            }
            {
                uint8_t lsb = (uint8_t)(v[15] & 1);
                for (k = 15; k > 0; k--) v[k] = (uint8_t)((v[k] >> 1) | (v[k-1] << 7));
                v[0] >>= 1;
                if (lsb) v[0] ^= 0xe1;
            }
        }
    }
    k_memcpy(x, z, 16);
}

static void gcm_ghash(const uint8_t h[16], const uint8_t *aad, uint32_t aadlen,
                      const uint8_t *ct, uint32_t ctlen, uint8_t y[16]) {
    uint8_t blk[16];
    uint32_t i, n;
    k_memset(y, 0, 16);
    for (i = 0; i < aadlen; i += 16) {
        k_memset(blk, 0, 16);
        n = aadlen - i; if (n > 16) n = 16;
        k_memcpy(blk, aad + i, n);
        for (n = 0; n < 16; n++) y[n] ^= blk[n];
        gcm_mult(y, h);
    }
    for (i = 0; i < ctlen; i += 16) {
        k_memset(blk, 0, 16);
        n = ctlen - i; if (n > 16) n = 16;
        k_memcpy(blk, ct + i, n);
        for (n = 0; n < 16; n++) y[n] ^= blk[n];
        gcm_mult(y, h);
    }
    k_memset(blk, 0, 16);
    wr64be(blk, (uint64_t)aadlen * 8);
    wr64be(blk + 8, (uint64_t)ctlen * 8);
    for (n = 0; n < 16; n++) y[n] ^= blk[n];
    gcm_mult(y, h);
}

static void gcm_ctr(const uint8_t rk[176], uint8_t ctr[16],
                    const uint8_t *in, uint32_t len, uint8_t *out) {
    uint8_t ks[16];
    uint32_t i, j;
    for (i = 0; i < len; i += 16) {
        uint32_t n = len - i; if (n > 16) n = 16;
        aes_encrypt_block(rk, ctr, ks);
        for (j = 0; j < n; j++) out[i + j] = (uint8_t)(in[i + j] ^ ks[j]);
        /* inc32 */
        for (j = 15; j >= 12; j--) { ctr[j]++; if (ctr[j]) break; }
    }
}

void aes128_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t *aad, uint32_t aadlen,
                        const uint8_t *pt, uint32_t plen,
                        uint8_t *out) {
    uint8_t rk[176], h[16], j0[16], ctr[16], s[16], tag[16];
    uint32_t i;
    aes_keyexp(key, rk);
    k_memset(h, 0, 16);
    aes_encrypt_block(rk, h, h);
    k_memcpy(j0, nonce, 12);
    j0[12]=0; j0[13]=0; j0[14]=0; j0[15]=1;
    k_memcpy(ctr, j0, 16);
    for (i = 15; i >= 12; i--) { ctr[i]++; if (ctr[i]) break; }
    if (plen) gcm_ctr(rk, ctr, pt, plen, out);
    gcm_ghash(h, aad, aadlen, out, plen, s);
    aes_encrypt_block(rk, j0, tag);
    for (i = 0; i < 16; i++) out[plen + i] = (uint8_t)(s[i] ^ tag[i]);
}

int aes128_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                       const uint8_t *aad, uint32_t aadlen,
                       const uint8_t *ct_and_tag, uint32_t ctlen,
                       uint8_t *out) {
    uint8_t rk[176], h[16], j0[16], ctr[16], s[16], tag[16];
    uint32_t plen, i, diff;
    if (ctlen < 16) return 0;
    plen = ctlen - 16;
    aes_keyexp(key, rk);
    k_memset(h, 0, 16);
    aes_encrypt_block(rk, h, h);
    k_memcpy(j0, nonce, 12);
    j0[12]=0; j0[13]=0; j0[14]=0; j0[15]=1;
    gcm_ghash(h, aad, aadlen, ct_and_tag, plen, s);
    aes_encrypt_block(rk, j0, tag);
    for (i = 0; i < 16; i++) tag[i] ^= s[i];
    diff = 0;
    for (i = 0; i < 16; i++) diff |= (uint32_t)(tag[i] ^ ct_and_tag[plen + i]);
    if (diff) return 0;
    k_memcpy(ctr, j0, 16);
    for (i = 15; i >= 12; i--) { ctr[i]++; if (ctr[i]) break; }
    if (plen) gcm_ctr(rk, ctr, ct_and_tag, plen, out);
    return 1;
}

/* ---------------- P-256 -------------------------------------------------- */
/* Field elements: 8 x uint32 little-endian. p = 2^256-2^224+2^192+2^96-1 */
static const uint32_t P256_P[8] = {
    0xffffffff, 0xffffffff, 0xffffffff, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0xffffffff
};
/* 2^256 - p = 2^256 mod p. Added when a field add carries out of 256 bits. */
static const uint32_t P256_R[8] = {
    0x00000001, 0x00000000, 0x00000000, 0xffffffff,
    0xffffffff, 0xffffffff, 0xfffffffe, 0x00000000
};
static const uint32_t P256_B[8] = {
    0x27d2604b, 0x3bce3c3e, 0xcc53b0f6, 0x651d06b0,
    0x769886bc, 0xb3ebbd55, 0xaa3a93e7, 0x5ac635d8
};
static const uint32_t P256_GX[8] = {
    0xd898c296, 0xf4a13945, 0x2deb33a0, 0x77037d81,
    0x63a440f2, 0xf8bce6e5, 0xe12c4247, 0x6b17d1f2
};
static const uint32_t P256_GY[8] = {
    0x37bf51f5, 0xcbb64068, 0x6b315ece, 0x2bce3357,
    0x7c0f9e16, 0x8ee7eb4a, 0xfe1a7f9b, 0x4fe342e2
};
static const uint32_t P256_N[8] = {
    0xfc632551, 0xf3b9cac2, 0xa7179e84, 0xbce6faad,
    0xffffffff, 0xffffffff, 0x00000000, 0xffffffff
};

static int fe_ge(const uint32_t a[8], const uint32_t b[8]) {
    int i;
    for (i = 7; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return 0;
    }
    return 1;
}
static void fe_subraw(uint32_t a[8], const uint32_t b[8]) {
    uint64_t br = 0;
    int i;
    for (i = 0; i < 8; i++) {
        uint64_t t = (uint64_t)a[i] - b[i] - br;
        a[i] = (uint32_t)t;
        br = (t >> 63) & 1;
    }
}
static void fe_norm(uint32_t a[8]) {
    if (fe_ge(a, P256_P)) fe_subraw(a, P256_P);
}
static void fe_set0(uint32_t a[8]) { k_memset(a, 0, 32); }
static void fe_cpy(uint32_t d[8], const uint32_t s[8]) { k_memcpy(d, s, 32); }
static int  fe_is0(const uint32_t a[8]) {
    int i; for (i = 0; i < 8; i++) if (a[i]) return 0; return 1;
}
static void fe_add(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]) {
    uint64_t c = 0;
    int i;
    for (i = 0; i < 8; i++) { c += (uint64_t)a[i] + b[i]; r[i] = (uint32_t)c; c >>= 32; }
    while (c) {
        uint64_t d = 0;
        for (i = 0; i < 8; i++) {
            d += (uint64_t)r[i] + P256_R[i];
            r[i] = (uint32_t)d;
            d >>= 32;
        }
        c = d;
    }
    if (fe_ge(r, P256_P)) fe_subraw(r, P256_P);
}
static void fe_sub(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]) {
    uint64_t br = 0;
    int i;
    for (i = 0; i < 8; i++) {
        uint64_t t = (uint64_t)a[i] - b[i] - br;
        r[i] = (uint32_t)t;
        br = (t >> 63) & 1;
    }
    if (br) {
        uint64_t c = 0;
        for (i = 0; i < 8; i++) { c += (uint64_t)r[i] + P256_P[i]; r[i] = (uint32_t)c; c >>= 32; }
    }
}
static void fe_dbl(uint32_t a[8]) {
    uint32_t t[8];
    fe_add(t, a, a);
    fe_cpy(a, t);
}
static void fe_add1(uint32_t a[8]) {
    uint32_t one[8], t[8];
    fe_set0(one); one[0] = 1;
    fe_add(t, a, one);
    fe_cpy(a, t);
}

/* r = x * y mod p  (schoolbook + bit-fold reduction — slow, correct). */
static void fe_mul(uint32_t r[8], const uint32_t a[8], const uint32_t b[8]) {
    unsigned __int128 acc[16];
    uint32_t x[16];
    unsigned __int128 c;
    int i, j;
    for (i = 0; i < 16; i++) acc[i] = 0;
    for (i = 0; i < 8; i++)
        for (j = 0; j < 8; j++)
            acc[i + j] += (unsigned __int128)a[i] * b[j];
    c = 0;
    for (i = 0; i < 16; i++) { c += acc[i]; x[i] = (uint32_t)c; c >>= 32; }
    fe_set0(r);
    for (i = 511; i >= 0; i--) {
        fe_dbl(r);
        if ((x[i / 32] >> (i % 32)) & 1u) fe_add1(r);
    }
}
static void fe_sqr(uint32_t r[8], const uint32_t a[8]) { fe_mul(r, a, a); }

/* inv via Fermat: a^{p-2} */
static void fe_inv(uint32_t r[8], const uint32_t a[8]) {
    uint32_t x[8], t[8];
    int i;
    fe_cpy(x, a);
    fe_cpy(r, a);
    /* p-2 = FFFFFFFF 00000001 00000000 00000000 00000000 FFFFFFFF FFFFFFFF FFFFFFFD */
    /* binary of p-2 from bit 255..0 — use square-and-multiply on p-2 */
    /* start from bit 254 (r already = a = bit 255 set of p-2? p-2 bit255 is 1) */
    static const uint32_t e[8] = {
        0xfffffffd, 0xffffffff, 0xffffffff, 0x00000000,
        0x00000000, 0x00000000, 0x00000001, 0xffffffff
    };
    fe_set0(r); r[0] = 1;
    for (i = 255; i >= 0; i--) {
        fe_sqr(t, r); fe_cpy(r, t);
        if ((e[i / 32] >> (i % 32)) & 1u) { fe_mul(t, r, x); fe_cpy(r, t); }
    }
}

typedef struct { uint32_t x[8], y[8], z[8]; int inf; } pt_t;

static void pt_set_inf(pt_t *p) { p->inf = 1; fe_set0(p->x); fe_set0(p->y); fe_set0(p->z); }
static void pt_set_xy(pt_t *p, const uint32_t x[8], const uint32_t y[8]) {
    fe_cpy(p->x, x); fe_cpy(p->y, y); fe_set0(p->z); p->z[0] = 1; p->inf = 0;
}
static void pt_cpy(pt_t *d, const pt_t *s) { *d = *s; }

/* Jacobian double (a = -3). */
static void pt_dbl(pt_t *r, const pt_t *p) {
    uint32_t A[8], B[8], C[8], D[8], t[8];
    if (p->inf || fe_is0(p->y)) { pt_set_inf(r); return; }
    fe_sqr(A, p->y);                 /* A = Y^2 */
    fe_mul(B, p->x, A); fe_add(t, B, B); fe_add(B, t, t); /* B = 4 X Y^2 */
    fe_sqr(C, A); fe_add(t, C, C); fe_add(C, t, t);
    fe_add(t, C, C); fe_cpy(C, t);   /* C = 8 Y^4 */
    fe_sqr(D, p->z);
    fe_sqr(t, p->x);                 /* X^2 */
    { uint32_t s[8], z4[8];
      fe_sqr(z4, D);                 /* Z^4 */
      fe_sub(s, t, z4);              /* X^2 - Z^4 */
      fe_add(D, t, t); fe_add(D, D, t); /* 3 X^2 */
      fe_add(t, z4, z4); fe_add(t, t, z4); /* 3 Z^4 */
      fe_sub(D, D, t);               /* M = 3 X^2 - 3 Z^4 */
    }
    fe_sqr(r->x, D); fe_add(t, B, B); fe_sub(r->x, r->x, t); /* X' = M^2 - 2B */
    fe_sub(t, B, r->x); fe_mul(r->y, D, t); fe_sub(r->y, r->y, C);
    fe_add(t, p->y, p->y); fe_mul(r->z, t, p->z);
    r->inf = 0;
}

static void pt_add(pt_t *r, const pt_t *p, const pt_t *q) {
    uint32_t A[8], B[8], C[8], D[8], E[8], F[8], t[8], u[8];
    if (p->inf) { pt_cpy(r, q); return; }
    if (q->inf) { pt_cpy(r, p); return; }
    /* H = X2 Z1^2 - X1 Z2^2 ; R = Y2 Z1^3 - Y1 Z2^3 */
    fe_sqr(A, p->z);                 /* Z1^2 */
    fe_sqr(B, q->z);                 /* Z2^2 */
    fe_mul(C, q->x, A);              /* U2 */
    fe_mul(D, p->x, B);              /* U1 */
    fe_mul(t, A, p->z); fe_mul(E, q->y, t); /* S2 */
    fe_mul(t, B, q->z); fe_mul(F, p->y, t); /* S1 */
    fe_sub(A, C, D);                 /* H */
    fe_sub(B, E, F);                 /* R */
    if (fe_is0(A)) {
        if (fe_is0(B)) { pt_dbl(r, p); return; }
        pt_set_inf(r); return;
    }
    fe_sqr(C, A);                    /* H^2 */
    fe_mul(E, C, A);                 /* H^3 */
    fe_mul(t, D, C);                 /* U1 H^2 */
    fe_sqr(r->x, B); fe_sub(r->x, r->x, E); fe_add(u, t, t); fe_sub(r->x, r->x, u);
    fe_sub(u, t, r->x); fe_mul(r->y, B, u); fe_mul(u, F, E); fe_sub(r->y, r->y, u);
    fe_mul(t, p->z, q->z); fe_mul(r->z, t, A);
    r->inf = 0;
}

static void pt_affine(pt_t *p) {
    uint32_t zi[8], z2[8], z3[8];
    if (p->inf) return;
    fe_inv(zi, p->z);
    fe_sqr(z2, zi);
    fe_mul(z3, z2, zi);
    fe_mul(p->x, p->x, z2);
    fe_mul(p->y, p->y, z3);
    fe_set0(p->z); p->z[0] = 1;
}

static void bytes_to_fe(uint32_t r[8], const uint8_t b[32]) {
    int i;
    for (i = 0; i < 8; i++)
        r[i] = rd32be(b + (7 - i) * 4);
    fe_norm(r);
}
static void fe_to_bytes(uint8_t b[32], const uint32_t a[8]) {
    int i;
    for (i = 0; i < 8; i++) wr32be(b + (7 - i) * 4, a[i]);
}

static int on_curve(const uint32_t x[8], const uint32_t y[8]) {
    uint32_t y2[8], x2[8], x3[8], t[8];
    fe_sqr(y2, y);
    fe_sqr(x2, x);
    fe_mul(x3, x2, x);
    fe_add(t, x, x); fe_add(t, t, x); /* 3x */
    fe_sub(x3, x3, t);
    fe_add(x3, x3, P256_B);
    return fe_ge(y2, x3) && fe_ge(x3, y2);
}

static void pt_scalarmul(pt_t *r, const uint32_t k[8], const pt_t *p) {
    pt_t acc, q;
    int i;
    pt_set_inf(&acc);
    pt_cpy(&q, p);
    for (i = 0; i < 256; i++) {
        if ((k[i / 32] >> (i % 32)) & 1u) {
            pt_t t;
            pt_add(&t, &acc, &q);
            pt_cpy(&acc, &t);
        }
        {
            pt_t t;
            pt_dbl(&t, &q);
            pt_cpy(&q, &t);
        }
    }
    pt_cpy(r, &acc);
}

static uint32_t g_rng;
static uint32_t rng32(void) {
    /* xorshift + mix. Seeded by the caller via p256_reseed. */
    uint32_t x = g_rng;
    if (!x) x = 0xA5A5A5A5u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_rng = x;
    return x;
}
void tls_crypto_reseed(uint32_t s) { if (s) g_rng ^= s; g_rng += 0x9E3779B9u; }

int p256_keygen(uint8_t scalar[32], uint8_t pub_xy[64]) {
    uint32_t k[8];
    pt_t G, R;
    int i, tries;
    for (tries = 0; tries < 8; tries++) {
        for (i = 0; i < 8; i++) k[i] = rng32();
        k[7] &= 0x7fffffff; /* keep < 2^255, hence < n */
        if (fe_is0(k)) continue;
        if (fe_ge(k, P256_N)) continue;
        pt_set_xy(&G, P256_GX, P256_GY);
        pt_scalarmul(&R, k, &G);
        if (R.inf) continue;
        pt_affine(&R);
        fe_to_bytes(scalar, k);
        fe_to_bytes(pub_xy, R.x);
        fe_to_bytes(pub_xy + 32, R.y);
        return 1;
    }
    return 0;
}

int p256_ecdh(const uint8_t scalar[32], const uint8_t peer_xy[64],
              uint8_t shared_x[32]) {
    uint32_t k[8], x[8], y[8];
    pt_t P, R;
    bytes_to_fe(k, scalar);
    bytes_to_fe(x, peer_xy);
    bytes_to_fe(y, peer_xy + 32);
    if (!on_curve(x, y)) return 0;
    pt_set_xy(&P, x, y);
    pt_scalarmul(&R, k, &P);
    if (R.inf) return 0;
    pt_affine(&R);
    fe_to_bytes(shared_x, R.x);
    return 1;
}

/* ---------------- RSA / X.509 -------------------------------------------- */
#define BN_MAX 128 /* 4096-bit */

static void bn_zero(uint32_t *a, int n) { int i; for (i = 0; i < n; i++) a[i] = 0; }
static int bn_cmp(const uint32_t *a, const uint32_t *b, int n) {
    int i; for (i = n - 1; i >= 0; i--) { if (a[i] > b[i]) return 1; if (a[i] < b[i]) return -1; }
    return 0;
}
static void bn_sub(uint32_t *a, const uint32_t *b, int n) {
    uint64_t br = 0; int i;
    for (i = 0; i < n; i++) {
        uint64_t t = (uint64_t)a[i] - b[i] - br;
        a[i] = (uint32_t)t; br = (t >> 63) & 1;
    }
}
static void bn_from_be(uint32_t *a, int n, const uint8_t *b, uint32_t blen) {
    uint32_t i;
    bn_zero(a, n);
    for (i = 0; i < blen; i++) {
        int bitbase = (int)((blen - 1 - i) * 8);
        int limb = bitbase / 32, off = bitbase % 32;
        if (limb < n) a[limb] |= (uint32_t)b[i] << off;
    }
}
static void bn_to_be(uint8_t *b, uint32_t blen, const uint32_t *a, int n) {
    uint32_t i;
    k_memset(b, 0, blen);
    for (i = 0; i < blen; i++) {
        int bitbase = (int)((blen - 1 - i) * 8);
        int limb = bitbase / 32, off = bitbase % 32;
        if (limb < n) b[i] = (uint8_t)(a[limb] >> off);
    }
}

/* r = (a * b) mod m. a,b,m,r are n limbs. Uses 2n scratch. */
static uint32_t g_bn_sc[BN_MAX * 2];

static void bn_mulmod(uint32_t *r, const uint32_t *a, const uint32_t *b,
                      const uint32_t *m, int n) {
    int i, j, bits;
    uint64_t c;
    uint32_t *prod = g_bn_sc;
    for (i = 0; i < 2 * n; i++) prod[i] = 0;
    for (i = 0; i < n; i++) {
        c = 0;
        for (j = 0; j < n; j++) {
            c += (uint64_t)prod[i + j] + (uint64_t)a[i] * b[j];
            prod[i + j] = (uint32_t)c;
            c >>= 32;
        }
        prod[i + n] += (uint32_t)c;
    }
    /* prod (2n limbs) mod m : bit-fold */
    {
        uint32_t acc[BN_MAX];
        bn_zero(acc, n);
        bits = n * 64; /* 2n * 32 */
        for (i = bits - 1; i >= 0; i--) {
            /* acc = 2*acc */
            c = 0;
            for (j = 0; j < n; j++) {
                c += (uint64_t)acc[j] << 1;
                acc[j] = (uint32_t)c;
                c >>= 32;
            }
            if (c || bn_cmp(acc, m, n) >= 0) bn_sub(acc, m, n);
            if ((prod[i / 32] >> (i % 32)) & 1u) {
                c = 1;
                for (j = 0; j < n; j++) {
                    c += acc[j];
                    acc[j] = (uint32_t)c;
                    c >>= 32;
                }
                if (c || bn_cmp(acc, m, n) >= 0) bn_sub(acc, m, n);
            }
        }
        for (i = 0; i < n; i++) r[i] = acc[i];
    }
}

static void bn_modexp(uint32_t *r, const uint32_t *a, const uint32_t *e,
                      const uint32_t *m, int n) {
    uint32_t acc[BN_MAX], t[BN_MAX];
    int i, top = 0, bits = n * 32;
    for (i = 0; i < bits; i++)
        if ((e[i / 32] >> (i % 32)) & 1u) top = i;
    for (i = 0; i < n; i++) acc[i] = 0;
    acc[0] = 1;
    for (i = top; i >= 0; i--) {
        bn_mulmod(t, acc, acc, m, n);
        for (int k = 0; k < n; k++) acc[k] = t[k];
        if ((e[i / 32] >> (i % 32)) & 1u) {
            bn_mulmod(t, acc, a, m, n);
            for (int k = 0; k < n; k++) acc[k] = t[k];
        }
    }
    for (i = 0; i < n; i++) r[i] = acc[i];
}

static int asn1_len(const uint8_t *p, int n, int *hdr, int *len) {
    int L;
    if (n < 2) return 0;
    L = p[1];
    if (L < 0x80) { *hdr = 2; *len = L; return *hdr + *len <= n; }
    if (L == 0x81 && n >= 3) { *hdr = 3; *len = p[2]; return *hdr + *len <= n; }
    if (L == 0x82 && n >= 4) { *hdr = 4; *len = (p[2] << 8) | p[3]; return *hdr + *len <= n; }
    return 0;
}

int x509_rsa_pub(const uint8_t *cert, uint32_t clen,
                 uint8_t *n, uint32_t *nlen,
                 uint8_t *e, uint32_t *elen) {
    /* Find rsaEncryption OID 1.2.840.113549.1.1.1 then the following BIT STRING. */
    static const uint8_t oid[] = {0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
    uint32_t i, j;
    int hdr, ln;
    const uint8_t *bs, *seq, *nb, *eb;
    int nblen, eblen;
    if (!cert || clen < 20) return 0;
    for (i = 0; i + sizeof(oid) < clen; i++) {
        int ok = 1;
        for (j = 0; j < sizeof(oid); j++) if (cert[i + j] != oid[j]) { ok = 0; break; }
        if (!ok) continue;
        /* After OID: optional NULL (05 00), then BIT STRING. */
        bs = cert + i + sizeof(oid);
        if (bs + 2 < cert + clen && bs[0] == 0x05) {
            int h2, l2;
            if (!asn1_len(bs, (int)(cert + clen - bs), &h2, &l2)) continue;
            bs += h2 + l2;
        }
        if (bs >= cert + clen || bs[0] != 0x03) continue;
        if (!asn1_len(bs, (int)(cert + clen - bs), &hdr, &ln)) continue;
        /* BIT STRING: unused-bits byte then RSAPublicKey SEQUENCE */
        if (ln < 10 || bs[hdr] != 0x00) continue;
        seq = bs + hdr + 1;
        if (seq[0] != 0x30) continue;
        if (!asn1_len(seq, ln - 1, &hdr, &ln)) continue;
        nb = seq + hdr;
        if (nb[0] != 0x02) continue;
        if (!asn1_len(nb, (int)(cert + clen - nb), &hdr, &nblen)) continue;
        nb += hdr;
        /* skip leading 0x00 on INTEGER */
        if (nblen && nb[0] == 0x00) { nb++; nblen--; }
        eb = nb + nblen;
        if (eb[0] != 0x02) continue;
        if (!asn1_len(eb, (int)(cert + clen - eb), &hdr, &eblen)) continue;
        eb += hdr;
        if (eblen && eb[0] == 0x00) { eb++; eblen--; }
        if (nblen < 64 || nblen > 512 || eblen < 1 || eblen > 8) continue;
        k_memcpy(n, nb, (uint32_t)nblen);
        k_memcpy(e, eb, (uint32_t)eblen);
        *nlen = (uint32_t)nblen;
        *elen = (uint32_t)eblen;
        return 1;
    }
    return 0;
}

int rsa_pkcs1_sha256_verify(const uint8_t *n, uint32_t nlen,
                            const uint8_t *e, uint32_t elen,
                            const uint8_t *sig, uint32_t slen,
                            const uint8_t *msg, uint32_t mlen) {
    static const uint8_t der[] = {
        0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,
        0x05,0x00,0x04,0x20
    };
    uint8_t hash[32], em[512], want[512];
    uint32_t N[BN_MAX], E[BN_MAX], S[BN_MAX], R[BN_MAX];
    int limbs, i, pad, want_len;
    if (nlen < 64 || nlen > 512 || slen != nlen) return 0;
    if (elen < 1 || elen > 8) return 0;
    sha256(msg, mlen, hash);
    limbs = (int)((nlen + 3) / 4);
    if (limbs > BN_MAX) return 0;
    bn_from_be(N, limbs, n, nlen);
    bn_from_be(E, limbs, e, elen);
    bn_from_be(S, limbs, sig, slen);
    bn_modexp(R, S, E, N, limbs);
    bn_to_be(em, nlen, R, limbs);

    /* EMSA-PKCS1-v1_5: 00 01 FF..FF 00 DigestInfo */
    want_len = (int)nlen;
    k_memset(want, 0, nlen);
    want[0] = 0x00; want[1] = 0x01;
    pad = want_len - 3 - (int)sizeof(der) - 32;
    if (pad < 8) return 0;
    for (i = 0; i < pad; i++) want[2 + i] = 0xff;
    want[2 + pad] = 0x00;
    k_memcpy(want + 3 + pad, der, sizeof(der));
    k_memcpy(want + 3 + pad + sizeof(der), hash, 32);
    {
        uint32_t diff = 0;
        for (i = 0; i < want_len; i++) diff |= (uint32_t)(want[i] ^ em[i]);
        return diff == 0;
    }
}

