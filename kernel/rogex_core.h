/* C bindings for the Rust no_std crypto core (rogex-core). */
#ifndef ROGEX_CORE_H
#define ROGEX_CORE_H

#include "rogex.h"

/* ML-KEM-768 sizes (bytes). Public key + ciphertext are the wire-standard
   FIPS-203 sizes. The secret key here is the RustCrypto ml-kem seed encoding
   (d||z, 64 B) that the core round-trips locally, not the 2400-byte expanded
   form (the secret key never leaves the device). */
#define MLKEM_PUBLICKEY_BYTES  1184
#define MLKEM_SECRETKEY_BYTES  64
#define MLKEM_CIPHERTEXT_BYTES 1088
#define MLKEM_SHAREDSECRET_BYTES 32

/* Initialize the Rust core (RNG seeding hook, mesh state). Returns 0 on ok. */
int32_t rogex_core_init(uint64_t seed);

/* Post-quantum KEM (ML-KEM). All buffers are caller-provided.
   Return 0 on success, negative on error. */
int32_t rogex_kem_keypair(uint8_t *pk, uint8_t *sk);
int32_t rogex_kem_encapsulate(const uint8_t *pk, uint8_t *ct, uint8_t *ss);
int32_t rogex_kem_decapsulate(const uint8_t *sk, const uint8_t *ct, uint8_t *ss);

/* Mesh transport (garlic-routed, per-hop ML-KEM). Non-blocking enqueue. */
int32_t rogex_mesh_tx(const uint8_t *buf, uint32_t len);

/* --- ChaCha20-Poly1305 AEAD (RFC 8439) ---------------------------------- */
#define AEAD_KEY_BYTES   32
#define AEAD_NONCE_BYTES 12
#define AEAD_TAG_BYTES   16

/* Fill buf with len random bytes from the core PRNG (e.g. an AEAD nonce). */
void    rogex_random_bytes(uint8_t *buf, uint32_t len);

/* Seal buf (len bytes) in place under key(32)+nonce(12); tag(16) out. AAD empty. */
int32_t rogex_aead_seal(const uint8_t *key, const uint8_t *nonce,
                        uint8_t *buf, uint32_t len, uint8_t *tag);

/* Verify tag over buf(len); if valid, decrypt in place. 0 ok, -1 auth fail. */
int32_t rogex_aead_open(const uint8_t *key, const uint8_t *nonce,
                        uint8_t *buf, uint32_t len, const uint8_t *tag);

/* RFC 8439 known-answer self-test. Returns 0 if the AEAD is correct. */
int32_t rogex_aead_selftest(void);

/* --- Phase 2: hardware identity + system of laws ------------------------ */
#define ROGEX_UID_BYTES 32

/* Derive the 32-byte Unique Hardware ID from TPM/CPU/MAC material. */
int32_t rogex_uid_derive(const uint8_t *tpm, uint32_t tpm_len,
                         const uint8_t *cpu, uint32_t cpu_len,
                         const uint8_t *mac, uint32_t mac_len, uint8_t *out);

/* Commit an immutable pseudonym to a UID; writes the 32-byte binding. */
int32_t rogex_pseudonym_bind(const uint8_t *uid, const uint8_t *name,
                             uint32_t name_len, uint8_t *out);

/* Proof-of-Report vote: 2 now-banned, 1 accepted, 0 duplicate, -1 already. */
int32_t rogex_report(const uint8_t *reporter, const uint8_t *subject);
int32_t rogex_is_banned(const uint8_t *uid);       /* 1 banned, 0 not        */
int32_t rogex_mesh_admit(const uint8_t *uid);      /* 1 admit, 0 deny        */
int32_t rogex_blacklist_count(void);

/* SHA3-256 over data -> out(32). Used to verify .rgxsite content integrity. */
int32_t rogex_sha3_256(const uint8_t *data, uint32_t len, uint8_t *out);

#endif /* ROGEX_CORE_H */
