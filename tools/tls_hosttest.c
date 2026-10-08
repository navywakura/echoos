/* Host unit tests for tls_crypto.c — SHA256, AES-GCM, P-256 ECDH. */
#define RXOS_HOSTTEST 1
#include "../drivers/net/tls_crypto.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;

static void expect(const char *name, int ok) {
    printf("%s %s\n", ok ? "OK  " : "FAIL", name);
    if (!ok) fails++;
}

static int hexeq(const uint8_t *a, const char *hex, int n) {
    int i;
    for (i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return 0;
        if (a[i] != (uint8_t)v) return 0;
    }
    return 1;
}

int main(void) {
    uint8_t out[64];

    /* FIPS 180-2 SHA-256("abc") */
    sha256((const uint8_t *)"abc", 3, out);
    expect("sha256(abc)",
           hexeq(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32));

    /* AES-GCM NIST empty-pt zero key/nonce
       tag = 58e2fccefa7e3061367f1d57a4e7455a */
    {
        uint8_t key[16] = {0}, nonce[12] = {0}, ct[16];
        uint8_t pt[16];
        aes128_gcm_encrypt(key, nonce, NULL, 0, NULL, 0, ct);
        expect("aes-gcm empty enc",
               hexeq(ct, "58e2fccefa7e3061367f1d57a4e7455a", 16));
        expect("aes-gcm empty dec",
               aes128_gcm_decrypt(key, nonce, NULL, 0, ct, 16, pt));
    }

    /* AES-GCM with plaintext */
    {
        uint8_t key[16], nonce[12], pt[16], ct[32], back[16];
        int i;
        for (i = 0; i < 16; i++) { key[i] = (uint8_t)i; nonce[i % 12] = (uint8_t)(i * 3); pt[i] = (uint8_t)(0xA0 + i); }
        memset(nonce + 8, 0, 4);
        aes128_gcm_encrypt(key, nonce, (const uint8_t *)"aad", 3, pt, 16, ct);
        expect("aes-gcm roundtrip",
               aes128_gcm_decrypt(key, nonce, (const uint8_t *)"aad", 3, ct, 32, back)
               && memcmp(pt, back, 16) == 0);
        ct[0] ^= 1;
        expect("aes-gcm tag reject",
               !aes128_gcm_decrypt(key, nonce, (const uint8_t *)"aad", 3, ct, 32, back));
    }

    /* P-256: two keypairs, shared secrets match. */
    {
        uint8_t sa[32], pa[64], sb[32], pb[64], zab[32], zba[32];
        int i;
        tls_crypto_reseed(0xC0FFEEu);
        for (i = 0; i < 32; i++) tls_crypto_reseed((uint32_t)(0x1111u * (i + 1)));
        expect("p256 keygen A", p256_keygen(sa, pa));
        expect("p256 keygen B", p256_keygen(sb, pb));
        {
            int ea = p256_ecdh(sa, pb, zab);
            int eb = p256_ecdh(sb, pa, zba);
            expect("p256 ecdh A*B", ea);
            expect("p256 ecdh B*A", eb);
            expect("p256 shared match", ea && eb && memcmp(zab, zba, 32) == 0);
            expect("p256 shared nonzero", ea && !(zab[0]==0 && zab[15]==0 && zab[31]==0));
        }
    }

    printf(fails ? "\n%d FAIL\n" : "\nall ok\n", fails);
    return fails ? 1 : 0;
}
