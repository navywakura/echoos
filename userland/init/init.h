/* Init. */
#ifndef RXOS_INIT_H
#define RXOS_INIT_H

#include "hwid.h"

/* Mount the FS, seed apps, log status lines. Returns false on FS failure. */
bool init_userland(void);

/* Start the interactive environment (shell/desktop). Returns on `halt`. */
void init_start_shell(const hwid_t *hw);

/* Built-in app registry (for the `apps` command). */
int         init_app_count(void);
const char *init_app_name(int i);

#endif /* RXOS_INIT_H */
