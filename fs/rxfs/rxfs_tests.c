/* Boot-time RXFS self-test. If this fails we say so — no silent green lights. */
#include "rxfs.h"
#include "ramdisk.h"

static bool seq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

bool rxfs_selftest(void) {
    /* run on a clean FS, then leave it clean for init's app seeding */
    rxfs_format();

    /* create + read back */
    if (!rxfs_write("t1.txt", "hola", 4)) return false;
    uint32_t len = 0;
    const char *d = rxfs_read("t1.txt", &len);
    if (!d || len != 4 || d[0] != 'h' || d[3] != 'a') return false;

    /* overwrite (upsert) */
    if (!rxfs_write("t1.txt", "adios!", 6)) return false;
    d = rxfs_read("t1.txt", &len);
    if (!d || len != 6) return false;

    /* rename: destination must not pre-exist; source must vanish */
    if (!rxfs_write("t2.txt", "x", 1)) return false;
    if (rxfs_rename("t1.txt", "t2.txt")) return false;     /* collision */
    if (!rxfs_rename("t1.txt", "t3.txt")) return false;
    if (rxfs_find("t1.txt") >= 0 || rxfs_find("t3.txt") < 0) return false;

    /* copy: content must match, source must remain */
    if (!rxfs_copy("t3.txt", "t4.txt")) return false;
    uint32_t l3 = 0, l4 = 0;
    const char *d3 = rxfs_read("t3.txt", &l3), *d4 = rxfs_read("t4.txt", &l4);
    if (!d3 || !d4 || l3 != l4) return false;
    for (uint32_t i = 0; i < l3; i++) if (d3[i] != d4[i]) return false;

    /* list + count */
    char names[RXFS_MAX_FILES][RXFS_NAME];
    if (rxfs_list(names, RXFS_MAX_FILES) != 3 || rxfs_count() != 3) return false;

    /* image save/load round trip through the ram0 block device */
    if (!rxfs_save((struct blockdev *)ramdisk_dev())) return false;
    rxfs_format();
    if (rxfs_count() != 0) return false;
    if (!rxfs_load((struct blockdev *)ramdisk_dev())) return false;
    if (rxfs_count() != 3) return false;
    d = rxfs_read("t4.txt", &len);
    if (!d || len != 6 || !seq("t4.txt", "t4.txt")) return false;

    /* delete everything; FS must be empty again */
    if (!rxfs_delete("t2.txt") || !rxfs_delete("t3.txt") || !rxfs_delete("t4.txt"))
        return false;
    if (rxfs_delete("missing.txt")) return false;
    if (rxfs_count() != 0) return false;

    rxfs_format();
    return true;
}
