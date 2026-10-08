#include "cli.h"
#include "bootmode.h"
#include "memtest.h"
#include "wizard.h"
#include "echofetch.h"
#include "epk.h"
#include "bsdtools.h"
#include "version.h"
#include "live.h"

static bool g_recovery;

void echos2_boot(void) {
    bootmode_init();
    epk_init();
    bsdtools_init();
    g_recovery = (bootmode_get() == BOOT_RECOVERY);

    if (bootmode_get() == BOOT_MEMTEST) {
        memtest_run();
        /* memtest_run returns on Enter — reboot from the shell. */
        console_write("\n[echos] memtest done. Type `reboot`.\n");
        return;
    }

    if (g_recovery) {
        console_write("\n");
        console_write("  EchOS Recovery Mode / Modo Recuperacion\n");
        console_write("  Shared kernel ");
        console_write(ECHOS_VERSION);
        console_write("  Heap-0  font=");
        console_write(ECHOS_FONT);
        console_write("\n  Commands: fsck is `load`/`save`, install, epk, disklabel, pfctl\n");
        console_write("  Type `echos-install` to re-run the TUI wizard.\n\n");
        return;
    }

    if (wizard_autostart()) {
        wizard_result_t r;
        console_write(
            "\n[echOS] installer  —  1=English  2=Espanol  q=LIVE\n"
            "[echOS] arrows/Enter in the menu, or type 1 / 2 / q\n\n");
        r = wizard_run();
        (void)r;
        live_enter();
        console_clear();
        return;
    }

    if (bootmode_get() == BOOT_LIVE) {
        live_enter();
        console_clear();
    }
}

bool echos2_cli_only(void) { return bootmode_cli_only(); }
bool echos2_recovery(void) { return g_recovery; }
