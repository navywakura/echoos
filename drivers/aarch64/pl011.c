/* PL011 at the address QEMU virt uses. A device tree walk would find it
   properly; this is the one hardcoded address in the port and it is marked
   as such, because the console has to work before anything can parse a DTB
   to tell us where the console is. */
#include "pl011.h"

#define PL011_BASE 0x09000000UL
#define UART_DR    0x00
#define UART_FR    0x18
#define UART_IBRD  0x24
#define UART_FBRD  0x28
#define UART_LCRH  0x2C
#define UART_CR    0x30
#define UART_IMSC  0x38
#define FR_RXFE    (1u << 4)      /* receive FIFO empty                     */
#define FR_TXFF    (1u << 5)      /* transmit FIFO full                     */

static volatile uint32_t *reg(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(PL011_BASE + off);
}

void pl011_init(void) {
    *reg(UART_CR) = 0;                      /* off while we programme it    */
    *reg(UART_IBRD) = 26;                   /* 115200 from a 48 MHz clock   */
    *reg(UART_FBRD) = 3;
    *reg(UART_LCRH) = (3u << 5) | (1u << 4);/* 8N1, FIFOs on                */
    *reg(UART_IMSC) = 0;                    /* polled: no interrupts yet    */
    *reg(UART_CR) = (1u << 0) | (1u << 8) | (1u << 9);  /* enable, TX, RX   */
}

void pl011_putc(char c) {
    uint32_t guard = 0;
    while ((*reg(UART_FR) & FR_TXFF) && guard++ < 1000000) { }
    *reg(UART_DR) = (uint32_t)(unsigned char)c;
}

bool pl011_has_input(void) { return (*reg(UART_FR) & FR_RXFE) == 0; }

int pl011_getc_nb(void) {
    if (!pl011_has_input()) return -1;
    return (int)(*reg(UART_DR) & 0xFF);
}
