/* Shell entry. */
#ifndef ROGEX_SHELL_H
#define ROGEX_SHELL_H

#include "hwid.h"

/* Run the Wired 4.0 environment: first-run account setup, then the browser
   REPL. Returns only when the user issues `halt`. */
void wired_run(const hwid_t *hw);

/* Local rgx:// page lookup for Nova (returns page title, fills `out`). */
const char *rgx_local_page(const char *url, char *out, size_t max);

#endif /* ROGEX_SHELL_H */
