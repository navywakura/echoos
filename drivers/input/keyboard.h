/* PS/2 keyboard driver. */
#ifndef RXOS_KEYBOARD_H
#define RXOS_KEYBOARD_H

#include "rogex.h"   /* kbd_has_input / kbd_poll_char / kbd_set_layout / kbd_layout */

bool kbd_controller_present(void);

#endif /* RXOS_KEYBOARD_H */
