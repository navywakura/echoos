/* REVM: sealed bytecode under ML-KEM. Fuzzed once; not a security proof. */
#include "revm.h"
#include "rogex_core.h"

/* ------------------------------ fetch helpers ---------------------------- */
/* Code is plaintext after revm_init() opened the sealed container. */
static bool fetch_u8(revm_vm *vm, uint8_t *out) {
    if (vm->ip >= vm->code_len) return false;
    *out = vm->code[vm->ip++];
    return true;
}
static bool fetch_u16(revm_vm *vm, uint16_t *out) {
    uint8_t a, b;
    if (!fetch_u8(vm, &a) || !fetch_u8(vm, &b)) return false;
    *out = (uint16_t)a | ((uint16_t)b << 8);
    return true;
}
static bool fetch_u32(revm_vm *vm, uint32_t *out) {
    uint16_t a, b;
    if (!fetch_u16(vm, &a) || !fetch_u16(vm, &b)) return false;
    *out = (uint32_t)a | ((uint32_t)b << 16);
    return true;
}
static bool fetch_u64(revm_vm *vm, uint64_t *out) {
    uint32_t a, b;
    if (!fetch_u32(vm, &a) || !fetch_u32(vm, &b)) return false;
    *out = (uint64_t)a | ((uint64_t)b << 32);
    return true;
}

/* ------------------------------ stack helpers ---------------------------- */
#define PUSH(v) do { if (vm->sp >= REVM_STACK_MAX) return REVM_ERR_STACK_OVERFLOW; \
                     vm->stack[vm->sp++] = (int64_t)(v); } while (0)
#define POP(dst) do { if (vm->sp == 0) return REVM_ERR_STACK_UNDERFLOW; \
                      (dst) = vm->stack[--vm->sp]; } while (0)

/* ------------------------------- host bridge ----------------------------- */
static revm_status do_syscall(revm_vm *vm, uint16_t num) {
    int64_t a, b;
    switch (num) {
    case SYS_PUTC:
        POP(a); console_putc((char)a); return REVM_OK;
    case SYS_PUTN:
        POP(a); console_write_dec((uint64_t)a); return REVM_OK;
    case SYS_PUTS: {
        POP(a);                       /* a = word index into VM mem[]        */
        if (a < 0 || (uint64_t)a >= REVM_MEM_WORDS) return REVM_ERR_OOB;
        /* Each mem word packs up to 8 chars, NUL-terminated. */
        for (uint64_t w = (uint64_t)a; w < REVM_MEM_WORDS; w++) {
            uint64_t packed = (uint64_t)vm->mem[w];
            for (int i = 0; i < 8; i++) {
                char c = (char)((packed >> (i * 8)) & 0xff);
                if (c == '\0') return REVM_OK;
                console_putc(c);
            }
        }
        return REVM_OK;
    }
    case SYS_NET_TX: {
        POP(a); POP(b);               /* a = len, b = buf word index         */
        if (b < 0 || (uint64_t)b >= REVM_MEM_WORDS) return REVM_ERR_OOB;
        /* Hand off to the Rust mesh core (garlic-routed, ML-KEM per hop). */
        rogex_mesh_tx((const uint8_t *)&vm->mem[b], (uint32_t)a);
        return REVM_OK;
    }
    case SYS_YIELD:
        return REVM_OK;               /* cooperative scheduler hook          */
    default:
        return REVM_ERR_BAD_OPCODE;
    }
}

static revm_status do_kem(revm_vm *vm, uint16_t op) {
    /* Buffers live in VM linear memory; args are word indices on the stack. */
    int64_t out_idx, in_idx, key_idx;
    switch (op) {
    case KEM_DECAP:
        POP(out_idx); POP(in_idx); POP(key_idx);  /* ss <- decap(sk, ct)     */
        if (out_idx < 0 || in_idx < 0 || key_idx < 0) return REVM_ERR_OOB;
        rogex_kem_decapsulate((const uint8_t *)&vm->mem[key_idx],
                              (const uint8_t *)&vm->mem[in_idx],
                              (uint8_t *)&vm->mem[out_idx]);
        PUSH(0);
        return REVM_OK;
    case KEM_KEYPAIR:
        POP(out_idx); POP(key_idx);               /* (pk,sk)                 */
        if (out_idx < 0 || key_idx < 0) return REVM_ERR_OOB;
        rogex_kem_keypair((uint8_t *)&vm->mem[out_idx],
                          (uint8_t *)&vm->mem[key_idx]);
        PUSH(0);
        return REVM_OK;
    case KEM_ENCAP:
        POP(out_idx); POP(in_idx); POP(key_idx);  /* (ct,ss) <- encap(pk)    */
        if (out_idx < 0 || in_idx < 0 || key_idx < 0) return REVM_ERR_OOB;
        rogex_kem_encapsulate((const uint8_t *)&vm->mem[key_idx],
                              (uint8_t *)&vm->mem[out_idx],
                              (uint8_t *)&vm->mem[in_idx]);
        PUSH(0);
        return REVM_OK;
    default:
        return REVM_ERR_BAD_OPCODE;
    }
}

/* --------------------------------- init ---------------------------------- */
revm_status revm_init(revm_vm *vm, const uint8_t *container, uint32_t len,
                      const uint8_t key[REVM_KEY_BYTES]) {
    if (len < sizeof(revm_header)) return REVM_ERR_OOB;
    const revm_header *h = (const revm_header *)container;
    if (h->magic != REVM_MAGIC)     return REVM_ERR_BAD_MAGIC;
    if (h->version != REVM_VERSION) return REVM_ERR_BAD_VERSION;
    if (h->code_len > REVM_CODE_MAX) return REVM_ERR_OOB;
    if ((uint64_t)sizeof(revm_header) + h->code_len > len) return REVM_ERR_OOB;

    k_memset(vm, 0, sizeof(*vm));
    /* Copy the sealed ciphertext into the VM, then authenticate + decrypt it
       in place under the ML-KEM shared secret. Nothing runs until it verifies. */
    k_memcpy(vm->code, container + sizeof(revm_header), h->code_len);
    if (rogex_aead_open(key, h->nonce, vm->code, h->code_len, h->tag) != 0)
        return REVM_ERR_AUTH;

    vm->code_len = h->code_len;
    vm->ip       = h->entry;
    vm->running  = true;
    return REVM_OK;
}

/* --------------------------------- run ----------------------------------- */
revm_status revm_run(revm_vm *vm, uint64_t max_steps) {
    uint64_t steps = 0;
    while (vm->running) {
        if (max_steps && ++steps > max_steps) return REVM_OK;
        if (vm->ip >= vm->code_len) return REVM_HALTED;

        uint8_t op;
        if (!fetch_u8(vm, &op)) return REVM_ERR_OOB;

        int64_t a, b;
        switch (op) {
        case OP_NOP:  break;
        case OP_HALT:
            vm->running = false;
            if (vm->sp) vm->exit_code = vm->stack[vm->sp - 1];
            return REVM_HALTED;

        case OP_PUSH: { uint64_t imm; if (!fetch_u64(vm, &imm)) return REVM_ERR_OOB;
                        PUSH((int64_t)imm); break; }
        case OP_POP:  POP(a); break;
        case OP_DUP:  if (!vm->sp) return REVM_ERR_STACK_UNDERFLOW;
                      PUSH(vm->stack[vm->sp - 1]); break;
        case OP_SWAP: if (vm->sp < 2) return REVM_ERR_STACK_UNDERFLOW;
                      a = vm->stack[vm->sp - 1];
                      vm->stack[vm->sp - 1] = vm->stack[vm->sp - 2];
                      vm->stack[vm->sp - 2] = a; break;

        case OP_ADD: POP(b); POP(a); PUSH(a + b); break;
        case OP_SUB: POP(b); POP(a); PUSH(a - b); break;
        case OP_MUL: POP(b); POP(a); PUSH(a * b); break;
        case OP_DIV: POP(b); POP(a); if (!b) return REVM_ERR_DIV_ZERO; PUSH(a / b); break;
        case OP_MOD: POP(b); POP(a); if (!b) return REVM_ERR_DIV_ZERO; PUSH(a % b); break;
        case OP_NEG: POP(a); PUSH(-a); break;

        case OP_AND: POP(b); POP(a); PUSH(a & b); break;
        case OP_OR:  POP(b); POP(a); PUSH(a | b); break;
        case OP_XOR: POP(b); POP(a); PUSH(a ^ b); break;
        case OP_SHL: POP(b); POP(a); PUSH((int64_t)((uint64_t)a << (b & 63))); break;
        case OP_SHR: POP(b); POP(a); PUSH((int64_t)((uint64_t)a >> (b & 63))); break;
        case OP_NOT: POP(a); PUSH(~a); break;

        case OP_CMP_EQ: POP(b); POP(a); PUSH(a == b ? 1 : 0); break;
        case OP_CMP_LT: POP(b); POP(a); PUSH(a <  b ? 1 : 0); break;
        case OP_CMP_GT: POP(b); POP(a); PUSH(a >  b ? 1 : 0); break;

        case OP_JMP: { uint32_t t; if (!fetch_u32(vm, &t)) return REVM_ERR_OOB;
                       if (t >= vm->code_len) return REVM_ERR_OOB; vm->ip = t; break; }
        case OP_JZ:  { uint32_t t; if (!fetch_u32(vm, &t)) return REVM_ERR_OOB;
                       POP(a); if (a == 0) { if (t >= vm->code_len) return REVM_ERR_OOB; vm->ip = t; } break; }
        case OP_JNZ: { uint32_t t; if (!fetch_u32(vm, &t)) return REVM_ERR_OOB;
                       POP(a); if (a != 0) { if (t >= vm->code_len) return REVM_ERR_OOB; vm->ip = t; } break; }
        case OP_CALL:{ uint32_t t; if (!fetch_u32(vm, &t)) return REVM_ERR_OOB;
                       if (vm->csp >= REVM_STACK_MAX) return REVM_ERR_STACK_OVERFLOW;
                       if (t >= vm->code_len) return REVM_ERR_OOB;
                       vm->call_stack[vm->csp++] = vm->ip; vm->ip = t; break; }
        case OP_RET: if (vm->csp == 0) { vm->running = false; return REVM_HALTED; }
                     vm->ip = vm->call_stack[--vm->csp]; break;

        case OP_LOAD:   { uint16_t i; if (!fetch_u16(vm, &i)) return REVM_ERR_OOB;
                          if (i >= REVM_MEM_WORDS) return REVM_ERR_OOB; PUSH(vm->mem[i]); break; }
        case OP_STORE:  { uint16_t i; if (!fetch_u16(vm, &i)) return REVM_ERR_OOB;
                          if (i >= REVM_MEM_WORDS) return REVM_ERR_OOB; POP(a); vm->mem[i] = a; break; }
        case OP_LOADL:  { uint16_t i; if (!fetch_u16(vm, &i)) return REVM_ERR_OOB;
                          if (i >= REVM_LOCALS_MAX) return REVM_ERR_OOB; PUSH(vm->locals[i]); break; }
        case OP_STOREL: { uint16_t i; if (!fetch_u16(vm, &i)) return REVM_ERR_OOB;
                          if (i >= REVM_LOCALS_MAX) return REVM_ERR_OOB; POP(a); vm->locals[i] = a; break; }

        case OP_SYSCALL:{ uint16_t n; if (!fetch_u16(vm, &n)) return REVM_ERR_OOB;
                          revm_status s = do_syscall(vm, n); if (s != REVM_OK) return s; break; }
        case OP_KEM:    { uint16_t n; if (!fetch_u16(vm, &n)) return REVM_ERR_OOB;
                          revm_status s = do_kem(vm, n); if (s != REVM_OK) return s; break; }

        case OP_DBG: if (vm->sp) { console_write("[dbg] tos="); console_write_dec((uint64_t)vm->stack[vm->sp - 1]); console_putc('\n'); } break;

        default:
            return REVM_ERR_BAD_OPCODE;
        }
    }
    return REVM_HALTED;
}

const char *revm_status_str(revm_status s) {
    switch (s) {
    case REVM_OK:                  return "ok";
    case REVM_HALTED:              return "halted";
    case REVM_ERR_BAD_MAGIC:       return "bad magic";
    case REVM_ERR_BAD_VERSION:     return "bad version";
    case REVM_ERR_STACK_OVERFLOW:  return "stack overflow";
    case REVM_ERR_STACK_UNDERFLOW: return "stack underflow";
    case REVM_ERR_OOB:             return "out of bounds";
    case REVM_ERR_BAD_OPCODE:      return "bad opcode";
    case REVM_ERR_DIV_ZERO:        return "divide by zero";
    case REVM_ERR_AUTH:            return "auth failed (bad key/tamper)";
    default:                       return "?";
    }
}

/* --------- container builder: seal plaintext bytecode under `key` -------- */
uint32_t revm_seal_container(uint8_t *out, uint32_t out_cap,
                             const uint8_t *plain_code, uint32_t code_len,
                             uint32_t entry, const uint8_t key[REVM_KEY_BYTES]) {
    if (code_len > REVM_CODE_MAX) return 0;
    uint32_t total = (uint32_t)sizeof(revm_header) + code_len;
    if (out_cap < total) return 0;

    revm_header *h = (revm_header *)out;
    h->magic    = REVM_MAGIC;
    h->version  = REVM_VERSION;
    h->flags    = 0;
    h->code_len = code_len;
    h->entry    = entry;
    rogex_random_bytes(h->nonce, REVM_NONCE_BYTES);   /* unique nonce per seal */

    uint8_t *ct = out + sizeof(revm_header);
    k_memcpy(ct, plain_code, code_len);               /* plaintext -> ct region */
    rogex_aead_seal(key, h->nonce, ct, code_len, h->tag); /* encrypt in place + tag */
    return total;
}
