/* Calculator app. */
#ifndef RXOS_CALC_H
#define RXOS_CALC_H

#include "rogex.h"

#define CALC_ROWS 5
#define CALC_COLS 4

void        calc_reset(void);
void        calc_key(int ch);          /* digits . + - * / = Enter Backspace C */
const char *calc_display(void);        /* the big number                       */
const char *calc_pending(void);        /* "12 +" while an operation is open    */

/* The keypad, so the window can draw it and hit-test it without keeping a
   second copy of the layout that could drift from the one calc_key() honours. */
int         calc_button_count(void);
const char *calc_button_label(int i);
char        calc_button_key(int i);

#endif /* RXOS_CALC_H */
