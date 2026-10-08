/* ROSH public API. */
#ifndef RXOS_ROSH_H
#define RXOS_ROSH_H

#include "rogex.h"

#define ROSH_LINE_MAX 128
#define ROSH_SESSIONS 2           /* one editor per Terminal split pane     */

typedef enum {
    ROSH_IGNORED,        /* key had no effect: redraw nothing               */
    ROSH_LINE_CHANGED,   /* the input line differs: redraw that strip       */
    ROSH_SUBMIT,         /* a command is ready: call rosh_take()            */
} rosh_result;

void        rosh_init(void);
void        rosh_init_session(int id);   /* wipe one pane without the other */
void        rosh_select(int id);         /* 0 .. ROSH_SESSIONS-1            */
int         rosh_active(void);
void        rosh_copy(int from, int to); /* duplicate a session (unsplit)   */
rosh_result rosh_key(int ch, const char **completions);
bool        rosh_take(char *out, int max);   /* the submitted line, once     */

const char *rosh_line(void);      /* the line as it stands right now         */
int         rosh_cursor(void);    /* caret position within it                */

int         rosh_history_count(void);
const char *rosh_history_at(int i);          /* 0 = oldest kept              */
int         rosh_completions(const char *pre, const char **out, int max);

#endif /* RXOS_ROSH_H */
