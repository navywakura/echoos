#include "bootmode.h"
#include "handoff.h"
#include "version.h"

static boot_mode_t g_mode = BOOT_NORMAL;

void bootmode_init(void) {
    char val[32];
    g_mode = BOOT_NORMAL;
    if (boot_cmdline_value("echos.mode", val, sizeof(val))) {
        if (val[0] == 'l' && val[1] == 'i') g_mode = BOOT_LIVE;
        else if (val[0] == 'i' && val[1] == 'n') g_mode = BOOT_INSTALL;
        else if (val[0] == 'm' && val[1] == 'e') g_mode = BOOT_MEMTEST;
        else if (val[0] == 'r' && val[1] == 'e') g_mode = BOOT_RECOVERY;
    }
#if ECHOS_SHARED_KERNEL
    /* Universal ISO with no mode still opens LIVE so the GRUB default
       "Installer / LIVE" entry can omit the argument and keep working. */
    if (g_mode == BOOT_NORMAL) g_mode = BOOT_LIVE;
#endif
}

boot_mode_t bootmode_get(void) { return g_mode; }

const char *bootmode_name(void) {
    switch (g_mode) {
    case BOOT_LIVE:     return "live";
    case BOOT_INSTALL:  return "install";
    case BOOT_MEMTEST:  return "memtest";
    case BOOT_RECOVERY: return "recovery";
    default:            return "normal";
    }
}

bool bootmode_cli_only(void) {
    if (ECHOS_CLI_ONLY) return true;
    return g_mode == BOOT_LIVE || g_mode == BOOT_INSTALL ||
           g_mode == BOOT_RECOVERY || g_mode == BOOT_MEMTEST;
}

bool wizard_autostart(void) {
    /* Automated QEMU tests drive COM1 with a script already queued.
       Never steal that input: the wizard would hang the smoke test. */
    if (console_serial_has_input()) return false;
    if (g_mode == BOOT_INSTALL || g_mode == BOOT_LIVE) return true;
    return false;
}
