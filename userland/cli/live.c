#include "live.h"
#include "tui.h"
#include "epk.h"
#include "hwprobe.h"
#include "vfs.h"
#include "blockdev.h"
#include "partition.h"
#include "version.h"
#include "echos_arch.h"

void live_enter(void) {
    tui_begin();
    tui_box(1, 0, 78, 24, ECHOS_VERSION_NAME "  LIVE");
    tui_text(4, 2, ECHOS_PRETTY_NAME, TUI_GOLD, TUI_BG);
    tui_text(4, 3, "Every .epk is available in LIVE.  echos-install writes a disk.", TUI_DIM, TUI_BG);
    tui_text(4, 5, "Tree:", TUI_ACCENT, TUI_BG);
    tui_text(6, 6, "/boot /sys /bin /etc /epk /var /dev /users /tmp", TUI_FG, TUI_BG);
    tui_text(4, 8, "Try:", TUI_ACCENT, TUI_BG);
    tui_text(6, 9,  "echofetch   epk list   hwprobe   bench-snn   help", TUI_FG, TUI_BG);
    tui_text(6, 10, "tcc  python3.14  qjs  doas  rcctl  pfctl  sudoers", TUI_FG, TUI_BG);
    tui_text(4, 12, "Users: /users/<you>/{Desktop,Documents,Downloads,Images,Videos}", TUI_DIM, TUI_BG);
    tui_text(4, 14, "NPU:   hwprobe   (Akida AKD1000  PCI 1e7c:bca1)", TUI_DIM, TUI_BG);
    tui_hint(21, "type commands at the prompt   |   echos-install writes a disk");
    tui_end();
}

void live_status(char *out, size_t outmax) {
    char *d = out;
    size_t n = 0;
    const char *s =
        "LIVE Mode  edition=";
    if (!out || !outmax) return;
    out[0] = '\0';
    while (*s && n + 1 < outmax) out[n++] = *s++;
    s = epk_runtime_edition();
    while (*s && n + 1 < outmax) out[n++] = *s++;
    s = "  arch=";
    while (*s && n + 1 < outmax) out[n++] = *s++;
    s = ECHOS_ARCH;
    while (*s && n + 1 < outmax) out[n++] = *s++;
    s = "\nRAM session. `echos-install` writes a disk. `hwprobe` lists PCI/SSD/HDD.\n"
        "Packages: `epk list`   Tree: `catalog /`   Bench: `bench-snn`\n";
    while (*s && n + 1 < outmax) out[n++] = *s++;
    out[n] = '\0';
    (void)d;
    (void)vfs_getcwd;
    (void)blockdev_count;
}
