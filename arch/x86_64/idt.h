/* IDT public API. */
#ifndef RXOS_IDT_H
#define RXOS_IDT_H

#include "rogex.h"

void idt_install(void);                       /* called from boot.asm      */
void idt_register(int vec, uint64_t handler); /* add a gate after boot     */

#endif /* RXOS_IDT_H */
