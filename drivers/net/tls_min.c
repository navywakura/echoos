/* TLS 1.2 client: ECDHE_RSA_WITH_AES_128_GCM_SHA256 + SNI. */
#include "tls_min.h"
#include "tls_crypto.h"

#ifdef RXOS_HOSTTEST
#include <stdint.h>
#include <string.h>
#include <stdio.h>
extern uint64_t pit_ticks(void);
extern int      host_tcp_send(const void *p, uint16_t n);
extern uint16_t host_tcp_recv(void *p, uint16_t n, uint32_t ms);
extern int      host_tcp_connected(void);
#ifndef k_memcpy
#define k_memcpy memcpy
#define k_memset memset
#endif
#else
#include "tcp.h"
#include "pit.h"
#include "rogex.h"
#endif

#ifdef RXOS_HOSTTEST
/* already declared above */
#define T_SEND(p,n) host_tcp_send((p),(n))
#define T_RECV(p,n,ms) host_tcp_recv((p),(n),(ms))
#define T_UP() host_tcp_connected()
#else
#define T_SEND(p,n) (tcp_send((p),(n)) ? (int)(n) : -1)
#define T_RECV(p,n,ms) tcp_recv((p),(n),(ms))
#define T_UP() tcp_connected()
#endif

#define HS_MAX   16384
#define APP_MAX  16384
#define REC_MAX  (HS_MAX + 2048)

static int      g_on, g_tls13;
static uint8_t  g_crand[32], g_srand[32];
static uint8_t  g_master[48];
static uint8_t  g_ckey[16], g_skey[16], g_civ[4], g_siv[4];
static uint8_t  g_civ12[12], g_siv12[12];
static uint8_t  g_scalar[32], g_pub_xy[64];
static uint8_t  g_chs[32], g_shs[32];   /* TLS 1.3 handshake traffic secrets */
static uint8_t  g_hash_sfin[32];        /* transcript through server Finished */
static uint64_t g_wseq, g_rseq;
static const char *g_vername = "TLS 1.2";
static uint8_t  g_hs[HS_MAX];
static uint32_t g_hs_n;
static uint8_t  g_app[APP_MAX];
static uint32_t g_app_n;
static uint8_t  g_rxrec[REC_MAX];
static uint32_t g_rxrec_n;
static uint8_t  g_pt[HS_MAX + 64];   /* decrypt scratch — not on the stack */

static void put16(uint8_t *p, unsigned v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void put24(uint8_t *p, unsigned v) {
    p[0]=(uint8_t)(v>>16); p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)v;
}
static unsigned get16(const uint8_t *p) { return ((unsigned)p[0]<<8)|p[1]; }
static unsigned get24(const uint8_t *p) {
    return ((unsigned)p[0]<<16)|((unsigned)p[1]<<8)|p[2];
}
static void cpywhy(char *d, int max, const char *s) {
    int i = 0;
    if (!d || max < 2) return;
    if (!s) { d[0]=0; return; }
    while (s[i] && i + 1 < max) { d[i]=s[i]; i++; }
    d[i]=0;
}

static void why_alert(char *d, int max, const uint8_t *b, uint32_t n) {
    char m[40];
    int i = 0;
    const char *p = "tls: alert";
    static const char *H = "0123456789abcdef";
    while (*p) m[i++] = *p++;
    if (n >= 2 && i + 8 < (int)sizeof(m)) {
        m[i++] = ' ';
        m[i++] = H[b[0] >> 4]; m[i++] = H[b[0] & 15];
        m[i++] = '/';
        m[i++] = H[b[1] >> 4]; m[i++] = H[b[1] & 15];
    }
    m[i] = 0;
    cpywhy(d, max, m);
}

static void hs_feed(const uint8_t *p, uint32_t n) {
    if (g_hs_n + n > HS_MAX) n = HS_MAX - g_hs_n;
    if (n) { k_memcpy(g_hs + g_hs_n, p, n); g_hs_n += n; }
}

static int rec_send(uint8_t type, const uint8_t *body, uint32_t blen, int enc) {
    uint8_t rec[2048];
    uint32_t n;
    if (blen + 64 > sizeof rec) return 0;
    if (!enc) {
        rec[0] = type; rec[1] = 3; rec[2] = 3;
        put16(rec + 3, (unsigned)blen);
        if (blen) k_memcpy(rec + 5, body, blen);
        n = 5 + blen;
        return T_SEND(rec, (uint16_t)n) == (int)n;
    }
    if (g_tls13) {
        uint8_t nonce[12], aad[5];
        uint32_t i, ilen = blen + 1;
        if (ilen > HS_MAX) return 0;
        if (blen) k_memcpy(g_pt, body, blen);
        g_pt[blen] = type;
        k_memcpy(nonce, g_civ12, 12);
        for (i = 0; i < 8; i++) nonce[4 + i] ^= (uint8_t)(g_wseq >> (56 - 8 * i));
        aad[0] = 23; aad[1] = 3; aad[2] = 3;
        put16(aad + 3, (unsigned)(ilen + 16));
        rec[0] = 23; rec[1] = 3; rec[2] = 3;
        put16(rec + 3, (unsigned)(ilen + 16));
        aes128_gcm_encrypt(g_ckey, nonce, aad, 5, g_pt, ilen, rec + 5);
        n = 5 + ilen + 16;
        g_wseq++;
        return T_SEND(rec, (uint16_t)n) == (int)n;
    }
    {
        uint8_t nonce[12], aad[13], ct[HS_MAX + 16];
        uint32_t i;
        k_memcpy(nonce, g_civ, 4);
        for (i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(g_wseq >> (56 - 8 * i));
        aad[0]=(uint8_t)(g_wseq>>56); aad[1]=(uint8_t)(g_wseq>>48);
        aad[2]=(uint8_t)(g_wseq>>40); aad[3]=(uint8_t)(g_wseq>>32);
        aad[4]=(uint8_t)(g_wseq>>24); aad[5]=(uint8_t)(g_wseq>>16);
        aad[6]=(uint8_t)(g_wseq>>8);  aad[7]=(uint8_t)g_wseq;
        aad[8] = type; aad[9] = 3; aad[10] = 3;
        put16(aad + 11, (unsigned)blen);
        aes128_gcm_encrypt(g_ckey, nonce, aad, 13, body, blen, ct);
        rec[0] = type; rec[1] = 3; rec[2] = 3;
        put16(rec + 3, (unsigned)(8 + blen + 16));
        k_memcpy(rec + 5, nonce + 4, 8);           /* explicit nonce = seq */
        k_memcpy(rec + 13, ct, blen + 16);
        n = 13 + blen + 16;
        g_wseq++;
        return T_SEND(rec, (uint16_t)n) == (int)n;
    }
}

static int rec_pull(uint8_t *type, uint8_t *body, uint32_t *blen, int enc,
                    uint32_t timeout_ms) {
    uint64_t start = pit_ticks();
    uint32_t ticks = timeout_ms / 10;
    if (!ticks) ticks = 1;
    for (;;) {
        if (g_rxrec_n >= 5) {
            unsigned want = get16(g_rxrec + 3);
            if (g_rxrec_n >= 5 + want) {
                uint8_t t = g_rxrec[0];
                const uint8_t *pl = g_rxrec + 5;
                if (enc && g_tls13 && t == 20) {
                    /* dummy CCS (middlebox compatibility) — not encrypted */
                    *type = 20;
                    *blen = want;
                    if (*blen > HS_MAX) *blen = 0;
                    else if (want) k_memcpy(body, pl, want);
                    g_rxrec_n -= 5 + want;
                    if (g_rxrec_n)
                        k_memcpy(g_rxrec, g_rxrec + 5 + want, g_rxrec_n);
                    return 1;
                }
                if (enc && g_tls13) {
                    uint8_t nonce[12], aad[5];
                    uint32_t i, ptlen;
                    if (want < 17) return 0;
                    if (want > HS_MAX + 16) {
                        /* oversized record: drop so we do not stall */
                        g_rxrec_n -= 5 + want;
                        if (g_rxrec_n)
                            k_memcpy(g_rxrec, g_rxrec + 5 + want, g_rxrec_n);
                        continue;
                    }
                    k_memcpy(nonce, g_siv12, 12);
                    for (i = 0; i < 8; i++)
                        nonce[4 + i] ^= (uint8_t)(g_rseq >> (56 - 8 * i));
                    aad[0] = t; aad[1] = 3; aad[2] = 3;
                    put16(aad + 3, (unsigned)want);
                    if (!aes128_gcm_decrypt(g_skey, nonce, aad, 5, pl, want, g_pt)) {
#ifdef RXOS_HOSTTEST
                        fprintf(stderr, "tls gcm-fail outer=%u want=%u rseq=%llu leftover=%u on=%d\n",
                                t, want, (unsigned long long)g_rseq, g_rxrec_n, g_on);
#endif
                        /* Leftover record encrypted under the previous
                           traffic secret. Drop it so tls_read does not spin
                           on the same 5+want bytes. */
                        g_rxrec_n -= 5 + want;
                        if (g_rxrec_n)
                            k_memcpy(g_rxrec, g_rxrec + 5 + want, g_rxrec_n);
                        continue;
                    }
                    ptlen = want - 16;
                    while (ptlen > 0 && g_pt[ptlen - 1] == 0) ptlen--;
                    if (ptlen < 1) {
                        g_rseq++;
                        g_rxrec_n -= 5 + want;
                        if (g_rxrec_n)
                            k_memcpy(g_rxrec, g_rxrec + 5 + want, g_rxrec_n);
                        continue;
                    }
                    *type = g_pt[ptlen - 1];
                    *blen = ptlen - 1;
                    if (*blen > HS_MAX) *blen = HS_MAX;
                    if (*blen) k_memcpy(body, g_pt, *blen);
                    g_rseq++;
                    g_rxrec_n -= 5 + want;
                    if (g_rxrec_n)
                        k_memcpy(g_rxrec, g_rxrec + 5 + want, g_rxrec_n);
                    return 1;
                } else if (enc) {
                    uint8_t nonce[12], aad[13], pt[HS_MAX];
                    uint32_t ctlen;
                    if (want < 8 + 16) return 0;
                    k_memcpy(nonce, g_siv, 4);
                    k_memcpy(nonce + 4, pl, 8);
                    ctlen = want - 8;
                    aad[0]=(uint8_t)(g_rseq>>56); aad[1]=(uint8_t)(g_rseq>>48);
                    aad[2]=(uint8_t)(g_rseq>>40); aad[3]=(uint8_t)(g_rseq>>32);
                    aad[4]=(uint8_t)(g_rseq>>24); aad[5]=(uint8_t)(g_rseq>>16);
                    aad[6]=(uint8_t)(g_rseq>>8);  aad[7]=(uint8_t)g_rseq;
                    aad[8] = t; aad[9] = 3; aad[10] = 3;
                    put16(aad + 11, (unsigned)(ctlen - 16));
                    if (!aes128_gcm_decrypt(g_skey, nonce, aad, 13,
                                            pl + 8, ctlen, pt))
                        return 0;
                    *blen = ctlen - 16;
                    if (*blen > HS_MAX) return 0;
                    k_memcpy(body, pt, *blen);
                    g_rseq++;
                } else {
                    *blen = want;
                    if (*blen > HS_MAX) return 0;
                    k_memcpy(body, pl, *blen);
                }
                *type = t;
                g_rxrec_n -= 5 + want;
                if (g_rxrec_n)
                    k_memcpy(g_rxrec, g_rxrec + 5 + want, g_rxrec_n);
                return 1;
            }
        }
        if (pit_ticks() - start >= ticks) return 0;
        {
            uint16_t room = (uint16_t)(sizeof(g_rxrec) - g_rxrec_n);
            uint16_t got;
            if (!room) return 0;
            got = (uint16_t)T_RECV(g_rxrec + g_rxrec_n, room, 200);
            if (got) {
                g_rxrec_n += got;
                continue;
            }
            if (!T_UP() && g_rxrec_n < 5) return 0;
        }
    }
}

static int send_client_hello(const char *sni) {
    uint8_t body[512], rec[520];
    unsigned n = 0, hs, ext, snilen, hostlen = 0;
    const char *h = sni ? sni : "";
    uint32_t i, tsc_lo, tsc_hi;
    while (h[hostlen]) hostlen++;
    if (hostlen > 80) hostlen = 80;

    __asm__ volatile ("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
    tls_crypto_reseed((uint32_t)pit_ticks() ^ tsc_lo ^ (tsc_hi << 1));
    if (!p256_keygen(g_scalar, g_pub_xy)) return 0;

    body[n++] = 1;                 /* ClientHello */
    hs = n; n += 3;
    body[n++] = 3; body[n++] = 3;
    for (i = 0; i < 32; i++) {
        uint32_t r = (uint32_t)pit_ticks() * 2654435761u + tsc_lo + i * 97;
        g_crand[i] = (uint8_t)(r ^ (tsc_hi >> (i & 7)) ^ (i * 17));
        body[n++] = g_crand[i];
    }
    body[n++] = 32;
    for (i = 0; i < 32; i++) body[n++] = g_crand[i] ^ (uint8_t)(i * 13);
    put16(body + n, 4); n += 2;
    body[n++] = 0x13; body[n++] = 0x01; /* TLS_AES_128_GCM_SHA256 */
    body[n++] = 0xC0; body[n++] = 0x2F; /* TLS 1.2 ECDHE_RSA_AES_128_GCM */
    body[n++] = 1; body[n++] = 0;  /* null compression */

    ext = n; n += 2;
    /* SNI */
    snilen = 5 + hostlen;
    put16(body + n, 0); n += 2;
    put16(body + n, snilen); n += 2;
    put16(body + n, 3 + hostlen); n += 2;
    body[n++] = 0;                 /* host_name */
    put16(body + n, hostlen); n += 2;
    for (i = 0; i < hostlen; i++) body[n++] = (uint8_t)h[i];
    /* elliptic_curves: secp256r1 */
    put16(body + n, 10); n += 2;
    put16(body + n, 4); n += 2;
    put16(body + n, 2); n += 2;
    put16(body + n, 0x0017); n += 2;
    /* ec_point_formats: uncompressed */
    put16(body + n, 11); n += 2;
    put16(body + n, 2); n += 2;
    body[n++] = 1; body[n++] = 0;
    /* signature_algorithms: rsa_pss / rsa_pkcs1 / ecdsa */
    put16(body + n, 13); n += 2;
    put16(body + n, 14); n += 2;
    put16(body + n, 12); n += 2;
    put16(body + n, 0x0804); n += 2;
    put16(body + n, 0x0805); n += 2;
    put16(body + n, 0x0401); n += 2;
    put16(body + n, 0x0501); n += 2;
    put16(body + n, 0x0403); n += 2;
    put16(body + n, 0x0503); n += 2;
    /* supported_versions: TLS 1.3 + 1.2 */
    put16(body + n, 0x002b); n += 2;
    put16(body + n, 5); n += 2;
    body[n++] = 4;
    put16(body + n, 0x0304); n += 2;
    put16(body + n, 0x0303); n += 2;
    /* psk_key_exchange_modes: psk_dhe_ke (Cloudflare / 1.3 stacks expect this) */
    put16(body + n, 0x002d); n += 2;
    put16(body + n, 2); n += 2;
    body[n++] = 1;
    body[n++] = 1;
    /* key_share secp256r1
       ext_len = 2 (shares vec) + 2 (group) + 2 (ke_len) + 65 (0x04||X||Y) = 71
       shares  = 2 (group) + 2 (ke_len) + 65 = 69 */
    put16(body + n, 0x0033); n += 2;
    put16(body + n, 2 + 2 + 2 + 65); n += 2;
    put16(body + n, 2 + 2 + 65); n += 2;
    put16(body + n, 0x0017); n += 2;
    put16(body + n, 65); n += 2;
    body[n++] = 0x04;
    k_memcpy(body + n, g_pub_xy, 64); n += 64;

    put16(body + ext, (unsigned)(n - ext - 2));
    put24(body + hs, n - hs - 3);

    rec[0] = 22; rec[1] = 3; rec[2] = 3;
    put16(rec + 3, n);
    k_memcpy(rec + 5, body, n);
    hs_feed(body, n);
    return T_SEND(rec, (uint16_t)(5 + n)) == (int)(5 + n);
}

static uint8_t g_peer_xy[64];

static int parse_server_hello(const uint8_t *m, uint32_t n) {
    unsigned pos = 0, sid, extlen, eoff, cipher;
    if (n < 38 || m[0] != 3 || m[1] != 3) return 0;
    k_memcpy(g_srand, m + 2, 32);
    pos = 34;
    sid = m[pos++];
    if (pos + sid + 3 > n) return 0;
    pos += sid;
    cipher = get16(m + pos); pos += 2;
    if (m[pos++] != 0) return 0;
    g_tls13 = 0;
    g_vername = "TLS 1.2";
    if (cipher == 0xC02F) return 1;
    if (cipher != 0x1301) return 0;
    g_tls13 = 1;
    g_vername = "TLS 1.3";
    if (pos + 2 > n) return 0;
    extlen = get16(m + pos); pos += 2;
    if (pos + extlen > n) return 0;
    eoff = pos;
    while (eoff + 4 <= pos + extlen) {
        unsigned et = get16(m + eoff), el = get16(m + eoff + 2);
        eoff += 4;
        if (eoff + el > pos + extlen) break;
        if (et == 0x0033 && el >= 69) {
            unsigned koff = eoff;
            if (get16(m + koff) == 0x0017 && m[koff + 4] == 0x04) {
                k_memcpy(g_peer_xy, m + koff + 5, 64);
                return 2;
            }
        }
        eoff += el;
    }
    return 0;
}

static int parse_cert_rsa(const uint8_t *m, uint32_t n,
                          uint8_t *mod, uint32_t *mlen,
                          uint8_t *exp, uint32_t *elen) {
    unsigned clen, certlen;
    if (n < 6) return 0;
    clen = get24(m);
    if (clen + 3 > n || clen < 6) return 0;
    certlen = get24(m + 3);
    if (certlen + 6 > n) return 0;
    return x509_rsa_pub(m + 6, certlen, mod, mlen, exp, elen);
}

static int parse_ske(const uint8_t *m, uint32_t n,
                     const uint8_t *mod, uint32_t mlen,
                     const uint8_t *exp, uint32_t elen,
                     uint8_t peer_xy[64]) {
    unsigned pos = 0, slen;
    uint8_t signed_msg[32 + 32 + 4 + 66];
    uint32_t sm = 0;
    if (n < 4 + 65 + 4) return 0;
    if (m[0] != 3) return 0;                 /* named_curve */
    if (get16(m + 1) != 0x0017) return 0;    /* secp256r1 */
    if (m[3] != 65 || m[4] != 0x04) return 0;
    k_memcpy(peer_xy, m + 5, 64);
    pos = 4 + 65;
    if (pos + 4 > n) return 0;
    /* hash + sig scheme */
    if (m[pos] != 4 || m[pos + 1] != 1) {
        /* some stacks still send sha1+rsa (0201) */
        if (!(m[pos] == 2 && m[pos + 1] == 1)) return 0;
    }
    slen = get16(m + pos + 2);
    pos += 4;
    if (pos + slen > n) return 0;
    k_memcpy(signed_msg + sm, g_crand, 32); sm += 32;
    k_memcpy(signed_msg + sm, g_srand, 32); sm += 32;
    signed_msg[sm++] = 3;
    signed_msg[sm++] = 0x00; signed_msg[sm++] = 0x17;
    signed_msg[sm++] = 65;
    signed_msg[sm++] = 0x04;
    k_memcpy(signed_msg + sm, peer_xy, 64); sm += 64;
    return rsa_pkcs1_sha256_verify(mod, mlen, exp, elen,
                                   m + pos, slen, signed_msg, sm);
}

static void derive_keys(const uint8_t premaster[32]) {
    uint8_t seed[64], kb[40];
    k_memcpy(seed, g_crand, 32);
    k_memcpy(seed + 32, g_srand, 32);
    tls_prf(premaster, 32, "master secret", seed, 64, g_master, 48);
    k_memcpy(seed, g_srand, 32);
    k_memcpy(seed + 32, g_crand, 32);
    tls_prf(g_master, 48, "key expansion", seed, 64, kb, 40);
    k_memcpy(g_ckey, kb, 16);
    k_memcpy(g_skey, kb + 16, 16);
    k_memcpy(g_civ, kb + 32, 4);
    k_memcpy(g_siv, kb + 36, 4);
    g_wseq = 0;
    g_rseq = 0;
}

static int send_cke_finished(const uint8_t pub_xy[64], const uint8_t premaster[32]) {
    uint8_t cke[70], fin[16], verify[12], h[32];
    unsigned n = 0;
    cke[n++] = 16;                 /* ClientKeyExchange */
    put24(cke + n, 66); n += 3;
    cke[n++] = 65;
    cke[n++] = 0x04;
    k_memcpy(cke + n, pub_xy, 64); n += 64;
    hs_feed(cke, n);
    if (!rec_send(22, cke, n, 0)) return 0;

    derive_keys(premaster);

    { uint8_t ccs = 1; if (!rec_send(20, &ccs, 1, 0)) return 0; }

    sha256(g_hs, g_hs_n, h);
    tls_prf(g_master, 48, "client finished", h, 32, verify, 12);
    fin[0] = 20;
    put24(fin + 1, 12);
    k_memcpy(fin + 4, verify, 12);
    hs_feed(fin, 16);
    return rec_send(22, fin, 16, 1);
}

static int expect_server_finished(void) {
    for (;;) {
        uint8_t typ, body[HS_MAX];
        uint32_t blen = 0;
        if (!rec_pull(&typ, body, &blen, 0, 5000)) return 0;
        if (typ == 21) return 0; /* alert */
        if (typ == 22) {
            /* NewSessionTicket (or leftover handshake) — hash it, stay clear */
            uint32_t off = 0;
            while (off + 4 <= blen) {
                unsigned mlen = get24(body + off + 1);
                if (off + 4 + mlen > blen) break;
                hs_feed(body + off, 4 + mlen);
                off += 4 + mlen;
            }
            continue;
        }
        if (typ == 20) { /* CCS — next records are encrypted */
            uint8_t et, eb[HS_MAX];
            uint32_t el = 0;
            uint8_t h[32], verify[12];
            if (!rec_pull(&et, eb, &el, 1, 5000)) return 0;
            if (et != 22 || el < 16 || eb[0] != 20) return 0;
            sha256(g_hs, g_hs_n, h);
            tls_prf(g_master, 48, "server finished", h, 32, verify, 12);
            {
                int i; uint32_t d = 0;
                for (i = 0; i < 12; i++) d |= (uint32_t)(verify[i] ^ eb[4 + i]);
                if (d) return 0;
            }
            return 1;
        }
    }
}

static int tls13_hs_keys(const uint8_t shared[32]) {
    uint8_t z[32], early[32], empty[32], derived[32], h[32];
    k_memset(z, 0, 32);
    sha256(z, 0, empty);
    hkdf_extract(z, 32, z, 32, early);
    tls13_expand_label(early, "derived", empty, 32, derived, 32);
    hkdf_extract(derived, 32, shared, 32, g_master);
    sha256(g_hs, g_hs_n, h);
    tls13_expand_label(g_master, "c hs traffic", h, 32, g_chs, 32);
    tls13_expand_label(g_master, "s hs traffic", h, 32, g_shs, 32);
    tls13_expand_label(g_chs, "key", 0, 0, g_ckey, 16);
    tls13_expand_label(g_chs, "iv",  0, 0, g_civ12, 12);
    tls13_expand_label(g_shs, "key", 0, 0, g_skey, 16);
    tls13_expand_label(g_shs, "iv",  0, 0, g_siv12, 12);
    g_wseq = 0; g_rseq = 0;
    return 1;
}

static int tls13_app_keys(void) {
    uint8_t z[32], empty[32], derived[32], master[32];
    k_memset(z, 0, 32);
    sha256(z, 0, empty);
    tls13_expand_label(g_master, "derived", empty, 32, derived, 32);
    hkdf_extract(derived, 32, z, 32, master);
    /* RFC 9846 7.1: both application traffic secrets use
       Transcript-Hash(ClientHello...server Finished). */
    tls13_expand_label(master, "c ap traffic", g_hash_sfin, 32, g_chs, 32);
    tls13_expand_label(master, "s ap traffic", g_hash_sfin, 32, g_shs, 32);
    tls13_expand_label(g_chs, "key", 0, 0, g_ckey, 16);
    tls13_expand_label(g_chs, "iv",  0, 0, g_civ12, 12);
    tls13_expand_label(g_shs, "key", 0, 0, g_skey, 16);
    tls13_expand_label(g_shs, "iv",  0, 0, g_siv12, 12);
    g_wseq = 0; g_rseq = 0;
    return 1;
}

static int tls13_client_finished(void) {
    uint8_t fin_key[32], verify[32], msg[36], h[32];
    tls13_expand_label(g_chs, "finished", 0, 0, fin_key, 32);
    sha256(g_hs, g_hs_n, h);
    hmac_sha256(fin_key, 32, h, 32, verify);
    msg[0] = 20;
    put24(msg + 1, 32);
    k_memcpy(msg + 4, verify, 32);
    hs_feed(msg, 36);
    return rec_send(22, msg, 36, 1);
}

static int tls13_rest(char *why, int whymax) {
    uint8_t shared[32];
    int got_fin = 0;
    if (!p256_ecdh(g_scalar, g_peer_xy, shared)) {
        cpywhy(why, whymax, "tls13: ECDH fail"); return 0;
    }
    tls13_hs_keys(shared);
    while (!got_fin) {
        uint8_t typ, body[HS_MAX];
        uint32_t blen = 0, off = 0;
        if (!rec_pull(&typ, body, &blen, 1, 6000)) {
            cpywhy(why, whymax, "tls13: timeout after ServerHello"); return 0;
        }
        if (typ == 21) { why_alert(why, whymax, body, blen); return 0; }
        if (typ != 22) continue;
        while (off + 4 <= blen) {
            uint8_t mt = body[off];
            unsigned ml = get24(body + off + 1);
            if (off + 4 + ml > blen) break;
            hs_feed(body + off, 4 + ml);
            if (mt == 20) { got_fin = 1; break; } /* stop hashing past Finished */
            off += 4 + ml;
        }
    }
    sha256(g_hs, g_hs_n, g_hash_sfin);
    if (!tls13_client_finished()) {
        cpywhy(why, whymax, "tls13: Client Finished"); return 0;
    }
    tls13_app_keys();
    return 1;
}

int tls_handshake(const char *sni, char *why, int whymax) {
    uint8_t mod[512], exp[8], peer_xy[64], pub_xy[64], scalar[32], shared[32];
    uint32_t mlen = 0, elen = 0;
    int got_sh = 0, got_cert = 0, got_ske = 0, got_done = 0;

    g_on = 0; g_hs_n = 0; g_app_n = 0; g_rxrec_n = 0;
    g_wseq = 0; g_rseq = 0; g_tls13 = 0; g_vername = "TLS 1.2";
    if (!T_UP()) { cpywhy(why, whymax, "tls: no tcp"); return 0; }
    if (!send_client_hello(sni)) { cpywhy(why, whymax, "tls: ClientHello send"); return 0; }

    while (!got_done) {
        uint8_t typ, body[HS_MAX];
        uint32_t blen = 0, off = 0;
        if (!rec_pull(&typ, body, &blen, 0, 6000)) {
            cpywhy(why, whymax, "tls: timeout waiting ServerHello");
            return 0;
        }
        if (typ == 21) { why_alert(why, whymax, body, blen); return 0; }
        if (typ != 22) continue;
        while (off + 4 <= blen) {
            uint8_t mt = body[off];
            unsigned mlen_hs = get24(body + off + 1);
            const uint8_t *mp = body + off + 4;
            if (off + 4 + mlen_hs > blen) break;
            hs_feed(body + off, 4 + mlen_hs);
            if (mt == 2) {
                int shv = parse_server_hello(mp, mlen_hs);
                if (!shv) {
                    cpywhy(why, whymax, "tls: bad ServerHello / cipher");
                    return 0;
                }
                if (shv == 2) {
                    if (!tls13_rest(why, whymax)) return 0;
                    g_on = 1;
                    cpywhy(why, whymax, NULL);
                    return 1;
                }
                got_sh = 1;
            } else if (mt == 11) {
                if (!parse_cert_rsa(mp, mlen_hs, mod, &mlen, exp, &elen)) {
                    cpywhy(why, whymax, "tls: no RSA cert (ECDSA-only host?)");
                    return 0;
                }
                got_cert = 1;
            } else if (mt == 12) {
                if (!got_cert || !parse_ske(mp, mlen_hs, mod, mlen, exp, elen, peer_xy)) {
                    cpywhy(why, whymax, "tls: ServerKeyExchange verify fail");
                    return 0;
                }
                got_ske = 1;
            } else if (mt == 14) {
                got_done = 1;
            }
            off += 4 + mlen_hs;
        }
    }
    if (!got_sh || !got_cert || !got_ske) {
        cpywhy(why, whymax, "tls: handshake incomplete");
        return 0;
    }
    if (!p256_keygen(scalar, pub_xy)) {
        cpywhy(why, whymax, "tls: P-256 keygen fail");
        return 0;
    }
    if (!p256_ecdh(scalar, peer_xy, shared)) {
        cpywhy(why, whymax, "tls: ECDH fail");
        return 0;
    }
    if (!send_cke_finished(pub_xy, shared)) {
        cpywhy(why, whymax, "tls: ClientKeyExchange/Finished send");
        return 0;
    }
    if (!expect_server_finished()) {
        cpywhy(why, whymax, "tls: server Finished mismatch");
        return 0;
    }
    g_on = 1;
    cpywhy(why, whymax, NULL);
    return 1;
}

int tls_ready(void) { return g_on; }
const char *tls_version_name(void) { return g_vername ? g_vername : "TLS 1.2"; }

int tls_write(const void *data, uint16_t len) {
    if (!g_on || !data) return 0;
    return rec_send(23, (const uint8_t *)data, len, 1);
}

int tls_read(void *data, uint16_t max, uint32_t timeout_ms) {
    uint64_t start;
    uint32_t ticks;
    if (!g_on) return 0;
    if (g_app_n) {
        uint16_t n = (uint16_t)g_app_n;
        if (n > max) n = max;
        k_memcpy(data, g_app, n);
        if (n < g_app_n) k_memcpy(g_app, g_app + n, g_app_n - n);
        g_app_n -= n;
        return (int)n;
    }
    start = pit_ticks();
    ticks = timeout_ms / 10; if (!ticks) ticks = 1;
    while (pit_ticks() - start < ticks) {
        uint8_t typ;
        uint32_t blen = 0;
        if (!rec_pull(&typ, g_pt, &blen, 1, 500)) {
            if (!T_UP()) return 0;
            continue;
        }
        if (typ == 21) {
#ifdef RXOS_HOSTTEST
            fprintf(stderr, "tls alert %02x/%02x\n",
                    blen > 0 ? g_pt[0] : 0, blen > 1 ? g_pt[1] : 0);
#endif
            g_on = 0; return 0;
        }
        if (typ == 22 || typ == 20) continue; /* NST / dummy CCS */
        if (typ == 23 && blen) {
            if (blen > max) {
                k_memcpy(data, g_pt, max);
                g_app_n = blen - max;
                if (g_app_n > APP_MAX) g_app_n = APP_MAX;
                k_memcpy(g_app, g_pt + max, g_app_n);
                return (int)max;
            }
            k_memcpy(data, g_pt, blen);
            return (int)blen;
        }
    }
    return 0;
}

void tls_close(void) {
    g_on = 0;
    g_hs_n = 0;
    g_app_n = 0;
    g_rxrec_n = 0;
    k_memset(g_master, 0, sizeof g_master);
    k_memset(g_ckey, 0, sizeof g_ckey);
    k_memset(g_skey, 0, sizeof g_skey);
}

int tls_min_probe(char *why, int whymax) {
    if (tls_handshake(NULL, why, whymax)) {
        cpywhy(why, whymax, "tls: handshake ok");
        return 1;
    }
    return 0;
}
