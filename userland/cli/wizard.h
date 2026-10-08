#ifndef ECHOS_WIZARD_H
#define ECHOS_WIZARD_H

#include "rogex.h"
#include "epk.h"

typedef enum {
    WIZARD_CANCEL = 0,
    WIZARD_LIVE,
    WIZARD_INSTALLED,
} wizard_result_t;

/* Full TUI installer. Language, region, edition, disk. */
wizard_result_t wizard_run(void);

/* Re-enter the wizard from a LIVE shell (`echos-install`). */
wizard_result_t echos_install(void);

const char *wizard_lang(void);
const char *wizard_locale(void);
const char *wizard_timezone(void);
const char *wizard_kbd(void);
epk_edition_t wizard_edition(void);

#endif
