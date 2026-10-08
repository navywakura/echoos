/* LIVE first-boot wizard + lock-screen chrome. */
#ifndef RXOS_SETUP_H
#define RXOS_SETUP_H

#include "rogex.h"

/* True when this boot should open the installer (writable disk, no profile,
   and nobody is already typing on the serial console). */
bool setup_should_run(void);

/* Full-screen (or serial) wizard. Returns true if an account was created.
   On success the caller finds lang/locale/tz already applied. */
bool setup_run(void);

/* Wallpaper + glass login card. Returns when `ok(password)` is true. */
void setup_lock_screen(const char *user, bool (*ok)(const char *pass));

/* Account + locale the wizard collected (valid after setup_run / load). */
void setup_apply_locale(const char *lang, const char *locale,
                        const char *tz, int tzoff);

const char *setup_lang(void);
const char *setup_locale(void);
const char *setup_timezone(void);
int         setup_tzoff(void);

/* Implemented in shell.c — the wizard writes the account through these. */
void account_create(const char *user, const char *pass,
                    const char *lang, const char *locale,
                    const char *tz, int tzoff);

#endif
