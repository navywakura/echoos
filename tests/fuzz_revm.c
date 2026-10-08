/* Host-side libFuzzer harness for REVM. Run via make fuzz. */
#include "revm.h"
#include "rogex_core.h"

/* --- host stubs for the freestanding externs vm.c references --- */
void console_putc(char c) { (void)c; }
void console_write(const char *s) { (void)s; }
void console_write_dec(uint64_t v) { (void)v; }
void console_write_hex(uint64_t v) { (void)v; }

void *k_memset(void *d, int v, size_t n) {
    unsigned char *p = d; while (n--) *p++ = (unsigned char)v; return d;
}
void *k_memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d;
}
size_t k_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int32_t rogex_kem_keypair(uint8_t *a, uint8_t *b) { (void)a; (void)b; return 0; }
int32_t rogex_kem_encapsulate(const uint8_t *a, uint8_t *b, uint8_t *c) { (void)a; (void)b; (void)c; return 0; }
int32_t rogex_kem_decapsulate(const uint8_t *a, const uint8_t *b, uint8_t *c) { (void)a; (void)b; (void)c; return 0; }
int32_t rogex_mesh_tx(const uint8_t *b, uint32_t l) { (void)b; (void)l; return 0; }
int32_t rogex_aead_open(const uint8_t *k, const uint8_t *n, uint8_t *b, uint32_t l, const uint8_t *t) {
    (void)k; (void)n; (void)b; (void)l; (void)t; return 0;
}
int32_t rogex_aead_seal(const uint8_t *k, const uint8_t *n, uint8_t *b, uint32_t l, uint8_t *t) {
    (void)k; (void)n; (void)b; (void)l; (void)t; return 0;
}
void rogex_random_bytes(uint8_t *b, uint32_t l) { (void)b; (void)l; }

/* Big struct: keep it out of the fuzzer's stack frame. */
static revm_vm g_vm;

int LLVMFuzzerTestOneInput(const uint8_t *data, uint64_t size) {
    k_memset(&g_vm, 0, sizeof(g_vm));
    uint32_t n = size > REVM_CODE_MAX ? REVM_CODE_MAX : (uint32_t)size;
    for (uint32_t i = 0; i < n; i++) g_vm.code[i] = data[i];
    g_vm.code_len = n;
    g_vm.ip = 0;
    g_vm.running = true;
    /* Bounded steps: bytecode may legitimately loop; the interpreter must
       still make progress or stop, never hang the fuzzer. */
    revm_run(&g_vm, 100000);
    return 0;
}
