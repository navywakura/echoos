/* In-OS editor. */
#ifndef ROGEX_EDIT_H
#define ROGEX_EDIT_H

#include "rogex.h"

/* Edit rxfs file `name` (created if new). Returns a nav-tab index (0..) if the
   user clicked a left-nav tab, or -1 if they exited with the Exit button. */
int edit_run(const char *name);

#endif /* ROGEX_EDIT_H */
