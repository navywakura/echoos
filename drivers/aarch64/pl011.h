/* PL011 UART — the console on QEMU virt and on most ARM boards. */
#ifndef ECHOS_PL011_H
#define ECHOS_PL011_H
#include "rogex.h"
void pl011_init(void);
void pl011_putc(char c);
bool pl011_has_input(void);
int  pl011_getc_nb(void);      /* -1 when nothing is waiting */
#endif
