/* Hardware IRQ registration. */
#ifndef RXOS_IRQ_H
#define RXOS_IRQ_H

#include "rogex.h"

typedef void (*irq_handler_t)(void);

/* Remap the PICs to vectors 32..47, install the IDT gates for all 16 lines,
   mask every line, and enable CPU interrupts (sti). */
void irq_install(void);

/* Register a handler for IRQ line 0..15 and unmask that line. */
void irq_register(int line, irq_handler_t handler);

void irq_mask(int line);
void irq_unmask(int line);

#endif /* RXOS_IRQ_H */
