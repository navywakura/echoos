/* Tiny EN/ES string table for the LIVE installer and the lock screen. */
#ifndef RXOS_I18N_H
#define RXOS_I18N_H

#include "rogex.h"

typedef enum { LANG_EN = 0, LANG_ES = 1 } i18n_lang;

enum {
    S_LOCKED = 0,
    S_USER,
    S_PASSWORD,
    S_UNLOCK,
    S_WRONG,
    S_WELCOME,
    S_INSTALL_TITLE,
    S_LANGUAGE,
    S_LOCALE,
    S_TIMEZONE,
    S_CLOCK,
    S_CLOCK_HINT,
    S_ACCOUNT,
    S_USERNAME,
    S_PASS_CONFIRM,
    S_DISK,
    S_ERASE,
    S_INSTALLING,
    S_DONE,
    S_REBOOT_HINT,
    S_CONTINUE,
    S_BACK,
    S_NEXT,
    S_SKIP,
    S_KEEP_TIME,
    S_EMPTY_PASS,
    S_MISMATCH,
    S_NO_DISK,
    S_TOO_SMALL,
    S_NO_PAYLOAD,
    S_COUNT
};

void        i18n_set(i18n_lang lang);
i18n_lang   i18n_get(void);
const char *i18n_code(void);          /* "en" / "es" */
bool        i18n_set_code(const char *code);
const char *T(int id);

#endif
