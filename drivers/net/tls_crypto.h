/* Compact crypto for TLS 1.2 ECDHE_RSA_WITH_AES_128_GCM_SHA256. */
#ifndef RXOS_TLS_CRYPTO_H
#define RXOS_TLS_CRYPTO_H

#ifdef RXOS_HOSTTEST
#include <stdint.h>
#include <string.h>
#ifndef __cplusplus
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#ifndef bool
typedef int bool;
#define true 1
#define false 0
#endif
#endif
#endif
#define k_memcpy memcpy
#define k_memset memset
#else
#include "rogex.h"
#endif

void sha256(const uint8_t *in, uint32_t n, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, uint32_t klen,
                 const uint8_t *msg, uint32_t mlen, uint8_t out[32]);
/* TLS 1.2 PRF (P_SHA256). */
void tls_prf(const uint8_t *sec, uint32_t slen,
             const char *label,
             const uint8_t *seed, uint32_t seedlen,
             uint8_t *out, uint32_t outlen);

/* TLS 1.3 HKDF-SHA256. salt=NULL/0 → Hash.length zeroes. */
void hkdf_extract(const uint8_t *salt, uint32_t slen,
                  const uint8_t *ikm, uint32_t ilen, uint8_t out[32]);
void tls13_expand_label(const uint8_t *secret,
                        const char *label,
                        const uint8_t *ctx, uint32_t ctxlen,
                        uint8_t *out, uint32_t outlen);

/* AES-128-GCM. nonce is 12 bytes. tag is 16 bytes at the end of `out`.
   encrypt: out = ciphertext || tag  (plen+16 bytes)
   decrypt: in  = ciphertext || tag; returns 0 on tag fail. */
void aes128_gcm_encrypt(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t *aad, uint32_t aadlen,
                        const uint8_t *pt, uint32_t plen,
                        uint8_t *out);
int  aes128_gcm_decrypt(const uint8_t key[16], const uint8_t nonce[12],
                        const uint8_t *aad, uint32_t aadlen,
                        const uint8_t *ct_and_tag, uint32_t ctlen,
                        uint8_t *out);

/* Mix extra entropy into the P-256 nonce (PIT ticks / TSC). */
void tls_crypto_reseed(uint32_t s);

/* P-256 ECDH. scalar/points are 32-byte big-endian (point is uncompressed X||Y). */
int p256_keygen(uint8_t scalar[32], uint8_t pub_xy[64]);
int p256_ecdh(const uint8_t scalar[32], const uint8_t peer_xy[64],
              uint8_t shared_x[32]);

/* Extract RSA n,e from an X.509 DER cert. n_len/e_len in bytes. */
int x509_rsa_pub(const uint8_t *cert, uint32_t clen,
                 uint8_t *n, uint32_t *nlen,
                 uint8_t *e, uint32_t *elen);

/* PKCS#1 v1.5 SHA-256 verify. Returns 1 if ok. */
int rsa_pkcs1_sha256_verify(const uint8_t *n, uint32_t nlen,
                            const uint8_t *e, uint32_t elen,
                            const uint8_t *sig, uint32_t slen,
                            const uint8_t *msg, uint32_t mlen);

#endif
