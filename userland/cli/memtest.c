#include "memtest.h"
#include "arch.h"
#include "tui.h"
#include "version.h"
#include "heap.h"
#include "heap0.h"

static void dec_to(char *d, uint64_t v, int max) {
    char t[24];
    int i = 0, n = 0;
    if (!max) return;
    if (!v) t[i++] = '0';
    while (v && i < 20) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0 && n + 1 < max) d[n++] = t[--i];
    d[n] = '\0';
}

void memtest_run(void) {
    char line[80];
    char nbuf[24];
    uint64_t tested = 0, errors = 0;
    int row;
    bool heap_ok, ran;

    tui_begin();
    tui_box(1, 0, 78, 24, "EchOS Memory Test");
    tui_text(4, 2, ECHOS_VERSION_NAME, TUI_GOLD, TUI_NAVY);
    tui_text(4, 3, "Diagnostico de RAM / RAM diagnostic  (Heap-0 unused frames)", TUI_DIM, TUI_NAVY);

    tui_text(4, 5, "Firmware map:", TUI_ACCENT, TUI_NAVY);
    row = 6;
    {
        int i, n = pmm_region_count();
        for (i = 0; i < n && row < 14; i++) {
            const pmm_region_t *r = pmm_region(i);
            const char *ty = "reserved";
            if (!r) continue;
            if (r->type == 1) ty = "available";
            else if (r->type == 3) ty = "ACPI reclaim";
            else if (r->type == 4) ty = "ACPI NVS";
            else if (r->type == 5) ty = "BAD";
            tui_text(6, row, ty, r->type == 5 ? TUI_RED : TUI_FG, TUI_NAVY);
            dec_to(nbuf, r->len / (1024 * 1024), sizeof(nbuf));
            tui_text(22, row, nbuf, TUI_FG, TUI_NAVY);
            tui_text(30, row, "MiB", TUI_DIM, TUI_NAVY);
            row++;
        }
    }

    dec_to(nbuf, pmm_total_bytes() / (1024 * 1024), sizeof(nbuf));
    tui_text(4, 15, "Total firmware RAM:", TUI_FG, TUI_NAVY);
    tui_text(28, 15, nbuf, TUI_ACCENT, TUI_NAVY);
    tui_text(36, 15, "MiB", TUI_DIM, TUI_NAVY);

    dec_to(nbuf, pmm_free_bytes() / (1024 * 1024), sizeof(nbuf));
    tui_text(4, 16, "Free (bump region):", TUI_FG, TUI_NAVY);
    tui_text(28, 16, nbuf, TUI_ACCENT, TUI_NAVY);
    tui_text(36, 16, "MiB  — testing...", TUI_DIM, TUI_NAVY);
    tui_end();

    heap_ok = heap_selftest();
    ran = pmm_memtest(0, &tested, &errors);

    tui_begin();
    tui_box(1, 0, 78, 24, "EchOS Memory Test");
    tui_text(4, 2, ran ? "Pattern pass: 0xAA  0x55  addr-xor  zero-restore" : "No free frames to test.",
             TUI_FG, TUI_NAVY);

    dec_to(nbuf, tested / (1024 * 1024), sizeof(nbuf));
    tui_text(4, 4, "Bytes tested:", TUI_FG, TUI_NAVY);
    tui_text(20, 4, nbuf, TUI_ACCENT, TUI_NAVY);
    tui_text(28, 4, "MiB", TUI_DIM, TUI_NAVY);

    dec_to(nbuf, errors, sizeof(nbuf));
    tui_text(4, 5, "Byte errors:", TUI_FG, TUI_NAVY);
    tui_text(20, 5, nbuf, errors ? TUI_RED : TUI_ACCENT, TUI_NAVY);

    tui_text(4, 7, "Heap self-test:", TUI_FG, TUI_NAVY);
    tui_text(20, 7, heap_ok ? "PASS" : "FAIL", heap_ok ? TUI_ACCENT : TUI_RED, TUI_NAVY);

    dec_to(nbuf, heap0_total_bytes() / 1024, sizeof(nbuf));
    tui_text(4, 8, "Heap-0 static:", TUI_FG, TUI_NAVY);
    tui_text(20, 8, nbuf, TUI_ACCENT, TUI_NAVY);
    tui_text(28, 8, "KiB reserved (O(1), 0% fragmentation)", TUI_DIM, TUI_NAVY);

    if (errors || !heap_ok || !ran) {
        tui_text(4, 11, "RESULT: FAIL  — RAM did not verify. Do not install.", TUI_RED, TUI_NAVY);
    } else {
        tui_text(4, 11, "RESULT: PASS  — unused frames verified.", TUI_ACCENT, TUI_NAVY);
    }
    tui_hint(21, "Press Enter to reboot  |  Esc to halt");
    tui_end();
    (void)line;
    for (;;) {
        tui_key k = tui_read_key();
        if (k == TUI_KEY_ENTER || k == TUI_KEY_Q) return;
        if (k == TUI_KEY_ESC) {
            console_write("\n[echos] memtest halt.\n");
            arch_halt_forever();
        }
    }
}
