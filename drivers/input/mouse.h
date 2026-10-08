/* PS/2 mouse driver. */
#ifndef ROGEX_MOUSE_H
#define ROGEX_MOUSE_H

#include "rogex.h"

/* Initialize the aux device: enable, set defaults, sample rate 200 Hz, stream. */
void mouse_init(void);
bool mouse_present(void);
bool mouse_has_wheel(void);              /* IMPS/2 4-byte packets              */

/* Poll one packet. On a complete packet, fills relative dx/dy (screen space,
   y grows downward) and the button bitmask (bit0=left,1=right,2=middle), and
   returns true. Returns false if no (mouse) data was available. */
bool mouse_poll(int *dx, int *dy, int *buttons);

/* Switch to interrupt-driven input: the IRQ12 handler posts typed
   RX_EVENT_MOUSE descriptors into the event fabric's SPSC input ring. */
bool mouse_enable_irq(void);

#endif /* ROGEX_MOUSE_H */
