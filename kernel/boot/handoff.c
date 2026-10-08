#include "handoff.h"

/* The struct and its accessors are generic; filling it is the boot
   transport's job, and a transport belongs to a machine. Multiboot2 from
   GRUB lives in arch/x86_64/handoff_mb2.c; an AArch64 UEFI stub would add
   its own and change nothing here. */

/* The one instance. The transport fills it; every accessor below reads it. */
boot_handoff_t g_boot;

const boot_handoff_t *boot_handoff(void) { return &g_boot; }

const char *boot_firmware_name(void) {
    switch (g_boot.firmware) {
    case BOOT_FW_BIOS: return "bios";
    case BOOT_FW_UEFI: return "uefi";
    default:           return "unknown";
    }
}

const char *boot_mode_string(void) {
    switch (g_boot.firmware) {
    case BOOT_FW_BIOS: return "bios-multiboot2";
    case BOOT_FW_UEFI: return "uefi-multiboot2";
    default:           return "unknown";
    }
}

/* ---- LIVE payload ------------------------------------------------------- */

static const uint8_t *g_pay;
static uint32_t       g_pay_len;

static void move_bytes(void *dst, const void *src, uint32_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d == s || n == 0) return;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
}

void boot_take_payload(uint64_t *floor) {
    uint32_t i;
    g_pay = NULL;
    g_pay_len = 0;
    if (!floor || !g_boot.valid) return;

    for (i = 0; i < g_boot.module_count; i++) {
        boot_module_t *m = &g_boot.module[i];
        uint64_t len = m->end - m->start;
        uint64_t dst;
        if (len < 4096) continue;            /* not a kernel image          */
        dst = (*floor + 15ull) & ~15ull;
        if (m->start != dst)
            move_bytes((void *)(uintptr_t)dst, (const void *)(uintptr_t)m->start,
                       (uint32_t)len);
        m->start = dst;
        m->end   = dst + len;
        g_pay     = (const uint8_t *)(uintptr_t)dst;
        g_pay_len = (uint32_t)len;
        *floor = dst + len;
        return;
    }
}

const uint8_t *boot_payload(uint32_t *len) {
    if (len) *len = g_pay_len;
    return g_pay;
}

/* ---- command line ------------------------------------------------------- */

const char *boot_cmdline(void) { return g_boot.cmdline; }

static int token_eq_key(const char *tok, const char *key) {
    while (*key) {
        if (*tok != *key) return 0;
        tok++; key++;
    }
    return (*tok == '\0' || *tok == '=');
}

bool boot_cmdline_has(const char *key) {
    const char *p = g_boot.cmdline;
    if (!key || !key[0]) return false;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (token_eq_key(p, key)) return true;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return false;
}

bool boot_cmdline_value(const char *key, char *dst, uint32_t max) {
    const char *p = g_boot.cmdline;
    if (!dst || !max) return false;
    dst[0] = '\0';
    if (!key || !key[0]) return false;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (token_eq_key(p, key)) {
            const char *k = key;
            uint32_t n = 0;
            while (*k) { p++; k++; }
            if (*p == '=') p++;
            while (*p && *p != ' ' && *p != '\t' && n + 1 < max) dst[n++] = *p++;
            dst[n] = '\0';
            return true;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return false;
}
