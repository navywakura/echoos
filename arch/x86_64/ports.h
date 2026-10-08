/* inb/outb and friends. */
#ifndef RXOS_PORTS_H
#define RXOS_PORTS_H

#include "rogex.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t r;
    __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
/* Tiny delay for slow legacy devices (write to an unused diagnostic port). */
static inline void io_wait(void) { outb(0x80, 0); }

#endif /* RXOS_PORTS_H */
