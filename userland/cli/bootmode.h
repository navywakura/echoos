#ifndef ECHOS_BOOTMODE_H
#define ECHOS_BOOTMODE_H

#include "rogex.h"

typedef enum {
    BOOT_NORMAL = 0,    /* 1.x desktop path, or installed 2.0 system   */
    BOOT_LIVE,          /* ISO: wizard then Minimal-in-RAM             */
    BOOT_INSTALL,       /* ISO: force the TUI installer                */
    BOOT_MEMTEST,       /* GRUB "EchOS Memory Test"                    */
    BOOT_RECOVERY,      /* GRUB "EchOS Recovery Mode"                  */
} boot_mode_t;

void        bootmode_init(void);
boot_mode_t bootmode_get(void);
const char *bootmode_name(void);
bool        bootmode_cli_only(void);   /* skip the graphical desktop     */
bool        wizard_autostart(void);    /* ISO first boot, not a test run */

#endif
