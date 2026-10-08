/* Shell command table. */
#ifndef RXOS_COMMANDS_H
#define RXOS_COMMANDS_H

#include "rogex.h"

typedef enum {
    CMD_NOT_HANDLED = 0,  /* not a system command — shell handles it        */
    CMD_TEXT,             /* `out` filled: show the terminal output view    */
    CMD_RUN_FILE,         /* run `runfile` with the Roxenite runtime        */
    CMD_CLEAR,            /* clear the terminal view                        */
    CMD_HALT,             /* halt the OS                                    */
} cmd_result_t;

/* `line` is modified in place (tokenized). */
/* Output builder shared with the install wizard: appends into the buffer the
   current commands_dispatch() call was handed. */
void o_str(const char *s);
void o_dec(uint64_t v);
void o_nl(void);

/* Verbose trace: a `# …` line on the console so the user sees what the
   kernel is doing. Default on. `verbose off` silences it. */
int  commands_verbose(void);
void commands_v(const char *s);

/* Every command name, in one array. Tab completion reads this; keeping a
   second list would guarantee the two drift apart. */
int         commands_name_count(void);
const char *commands_name_at(int i);

/* Tab complete the last token of `buf[0..n)`. Writes extra characters into
   `insert`. If several matches, also fills `extra` (up to extra_max names).
   Returns the match count. */
int commands_tab_complete(const char *buf, size_t n,
                          char *insert, size_t imax,
                          char extra[][48], int extra_max);

void        commands_hist_add(const char *line);
int         commands_hist_count(void);
const char *commands_hist_at(int i);          /* 0 = oldest */
const char *commands_hist_from_end(int n);    /* 1 = last command */

cmd_result_t commands_dispatch(char *line, char *runfile, size_t runmax,
                               char *out, size_t outmax);

/* Pulse the 8042 reset line. Shared with the desktop's Start menu so there is
   one way to restart this machine, not two. Does not return. */
ROGEX_NORETURN void system_reboot(void);

#endif /* RXOS_COMMANDS_H */
