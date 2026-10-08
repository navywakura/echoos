/* REVM. */
#ifndef ROGEX_VM_H
#define ROGEX_VM_H

#include "rogex.h"

#define REVM_MAGIC        0x52475831u   /* "RGX1" bytecode container magic  */
#define REVM_VERSION      2             /* v2: ChaCha20-Poly1305 container   */
#define REVM_STACK_MAX    256
#define REVM_LOCALS_MAX   64
#define REVM_MEM_WORDS    4096          /* 32 KiB of VM linear memory        */
#define REVM_CODE_MAX     8192          /* max bytecode size per container    */
#define REVM_KEY_BYTES    32            /* AEAD key = ML-KEM shared secret    */
#define REVM_NONCE_BYTES  12
#define REVM_TAG_BYTES    16

/* ---- Opcode table (v1). Keep in lockstep with docs/OPCODES.md. ---------- */
typedef enum {
    OP_NOP     = 0x00,
    OP_HALT    = 0x01,

    /* stack */
    OP_PUSH    = 0x10,   /* + imm64 (LE)   push immediate                    */
    OP_POP     = 0x11,
    OP_DUP     = 0x12,
    OP_SWAP    = 0x13,

    /* arithmetic */
    OP_ADD     = 0x20,
    OP_SUB     = 0x21,
    OP_MUL     = 0x22,
    OP_DIV     = 0x23,
    OP_MOD     = 0x24,
    OP_NEG     = 0x25,

    /* bitwise */
    OP_AND     = 0x30,
    OP_OR      = 0x31,
    OP_XOR     = 0x32,
    OP_SHL     = 0x33,
    OP_SHR     = 0x34,
    OP_NOT     = 0x35,

    /* comparison (push 1/0) */
    OP_CMP_EQ  = 0x40,
    OP_CMP_LT  = 0x41,
    OP_CMP_GT  = 0x42,

    /* control flow (+ imm32 target/offset) */
    OP_JMP     = 0x50,
    OP_JZ      = 0x51,
    OP_JNZ     = 0x52,
    OP_CALL    = 0x53,
    OP_RET     = 0x54,

    /* VM memory + locals (+ imm16 index) */
    OP_LOAD    = 0x60,   /* push mem[idx]              */
    OP_STORE   = 0x61,   /* mem[idx] = pop            */
    OP_LOADL   = 0x62,   /* push locals[idx]          */
    OP_STOREL  = 0x63,   /* locals[idx] = pop         */

    /* host / syscall bridge (+ imm16 syscall number) */
    OP_SYSCALL = 0x70,

    /* crypto bridge into the Rust post-quantum core (+ imm16 op) */
    OP_KEM     = 0x80,   /* ML-KEM keypair/encap/decap via rogex-core */

    OP_DBG     = 0xf0,   /* dump top-of-stack to console (debug builds) */
} revm_opcode;

/* Host syscalls reachable through OP_SYSCALL. */
typedef enum {
    SYS_PUTC   = 0x01,   /* pop char, write to console            */
    SYS_PUTS   = 0x02,   /* pop ptr(VM mem word idx), write string */
    SYS_PUTN   = 0x03,   /* pop int, write decimal                */
    SYS_NET_TX = 0x10,   /* pop (len, buf idx) -> mesh transmit    */
    SYS_YIELD  = 0x20,   /* cooperative yield                      */
} revm_syscall;

/* ML-KEM sub-ops reachable through OP_KEM (mirror rogex_core.h). */
typedef enum {
    KEM_KEYPAIR = 0x01,
    KEM_ENCAP   = 0x02,
    KEM_DECAP   = 0x03,
} revm_kemop;

typedef enum {
    REVM_OK = 0,
    REVM_HALTED,
    REVM_ERR_BAD_MAGIC,
    REVM_ERR_BAD_VERSION,
    REVM_ERR_STACK_OVERFLOW,
    REVM_ERR_STACK_UNDERFLOW,
    REVM_ERR_OOB,
    REVM_ERR_BAD_OPCODE,
    REVM_ERR_DIV_ZERO,
    REVM_ERR_AUTH,          /* AEAD authentication failed (bad key/tamper) */
} revm_status;

/* Sealed bytecode container header (little-endian on disk), followed by the
   ChaCha20-Poly1305 ciphertext of length code_len. */
typedef struct ROGEX_PACKED {
    uint32_t magic;                     /* REVM_MAGIC                    */
    uint16_t version;                   /* REVM_VERSION                  */
    uint16_t flags;
    uint32_t code_len;                  /* ciphertext length             */
    uint32_t entry;                     /* entry instruction pointer     */
    uint8_t  nonce[REVM_NONCE_BYTES];   /* AEAD nonce                    */
    uint8_t  tag[REVM_TAG_BYTES];       /* Poly1305 tag over ciphertext  */
    /* sealed code[code_len] follows */
} revm_header;

typedef struct {
    uint8_t  code[REVM_CODE_MAX];   /* decrypted bytecode (after open)    */
    uint32_t code_len;
    uint32_t ip;                    /* instruction pointer into code[]    */

    int64_t  stack[REVM_STACK_MAX];
    uint32_t sp;

    int64_t  locals[REVM_LOCALS_MAX];
    int64_t  mem[REVM_MEM_WORDS];

    uint32_t call_stack[REVM_STACK_MAX];
    uint32_t csp;

    bool     running;
    int64_t  exit_code;
} revm_vm;

/* Open a sealed container: authenticate + decrypt under `key` (the ML-KEM
   shared secret). Returns REVM_ERR_AUTH on a wrong key or tampering. */
revm_status revm_init(revm_vm *vm, const uint8_t *container, uint32_t len,
                      const uint8_t key[REVM_KEY_BYTES]);

/* Run until HALT, fault, or `max_steps` executed (0 = unbounded). */
revm_status revm_run(revm_vm *vm, uint64_t max_steps);

const char *revm_status_str(revm_status s);

/* Exposed for the toolchain/self-test: seal plaintext opcodes into a `RGX1`
   container (random nonce, ChaCha20-Poly1305 under `key`). Returns total bytes
   written, or 0 on error. */
uint32_t revm_seal_container(uint8_t *out, uint32_t out_cap,
                             const uint8_t *plain_code, uint32_t code_len,
                             uint32_t entry, const uint8_t key[REVM_KEY_BYTES]);

#endif /* ROGEX_VM_H */
