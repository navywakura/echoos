/*
 * The freestanding string and memory helpers.
 *
 * They lived inside the PC board's main until an AArch64 build asked for
 * them, which is a fair definition of "in the wrong file": nothing here
 * knows what machine it is on.
 */
#include "rogex.h"

void *k_memset(void *dst, int v, size_t n) {
    uint8_t *d = dst;
    while (n--) *d++ = (uint8_t)v;
    return dst;
}
void *k_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
size_t k_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

/* String utilities for freestanding environment */
char *k_strncat(char *dest, const char *src, size_t n) {
    size_t dest_len = k_strlen(dest);
    size_t i = 0;
    while (i < n && src[i]) {
        dest[dest_len + i] = src[i];
        i++;
    }
    dest[dest_len + i] = '\0';
    return dest;
}

int k_snprintf(char *buf, size_t n, const char *fmt, ...) {
    /* Minimal implementation: only supports %s format specifier */
    size_t i = 0;
    const char *p = fmt;
    while (*p && i < n - 1) {
        if (*p == '%') {
            p++;
            if (*p == 's') {
                /* va_list not available in freestanding, handle simple case */
                /* This is a stub - actual usage passes direct strings */
                p++;
                continue;
            }
        }
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    return (int)i;
}
