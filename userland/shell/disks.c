/*
 * cfdisk-style partition explorer + helpers for the Disks GUI.
 */
#include "disks.h"
#include "blockdev.h"
#include "partition.h"
#include "disk.h"
#include "rxfs.h"
#include "install.h"
#include "commands.h"

static char  *o_buf;
static size_t o_max, o_len;
static void o_reset(char *b, size_t m) { o_buf = b; o_max = m; o_len = 0; if (m) b[0] = 0; }
static void d_str(const char *s) {
    if (!s) return;
    while (*s && o_len + 1 < o_max) o_buf[o_len++] = *s++;
    if (o_max) o_buf[o_len] = 0;
}
static void d_nl(void) { d_str("\n"); }
static void d_dec(uint64_t v) {
    char t[21]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0 && o_len + 1 < o_max) o_buf[o_len++] = t[--i];
    if (o_max) o_buf[o_len] = 0;
}
static bool eqs(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
static void size_of(uint64_t sectors) {
    uint64_t kib = sectors * BLOCKDEV_SECTOR / 1024;
    if (kib >= 1024 * 1024) { d_dec(kib / (1024 * 1024)); d_str(" GiB"); }
    else if (kib >= 1024)   { d_dec(kib / 1024);          d_str(" MiB"); }
    else                    { d_dec(kib);                 d_str(" KiB"); }
}

static void show(blockdev_t *d) {
    part_table_t t;
    d_str("cfdisk  "); d_str(d->name); d_str("  "); size_of(d->sectors); d_nl();
    if (!part_read(d, &t)) { d_str("  I/O error reading sector 0\n"); return; }
    if (t.gpt) d_str("  [GPT leftover / protective MBR]\n");
    if (!t.valid) { d_str("  (no partition table)\n"); return; }
    d_str("  #  Start      Size       Type\n");
    for (int i = 0; i < PART_MAX_PRIMARY; i++) {
        d_str("  "); d_dec((uint64_t)(i + 1)); d_str("  ");
        if (!t.e[i].used) { d_str("(empty)\n"); continue; }
        d_dec(t.e[i].start_lba); d_str("      ");
        size_of(t.e[i].sectors); d_str("     ");
        d_str(part_type_name(t.e[i].type));
        if (t.e[i].bootable) d_str("  [boot]");
        d_nl();
    }
    d_str("\ncommands:\n");
    d_str("  cfdisk "); d_str(d->name); d_str(" del <n>\n");
    d_str("  cfdisk "); d_str(d->name); d_str(" mkpart [type_hex]\n");
    d_str("  cfdisk "); d_str(d->name); d_str(" wipe yes     empty MBR\n");
    d_str("  cfdisk "); d_str(d->name); d_str(" zap yes      wipe GPT+MBR\n");
    d_str("  install "); d_str(d->name); d_str(" REINSTALL-"); d_str(d->name); d_nl();
}

static int parse_u(const char *s) {
    int v = 0;
    if (!s || !*s) return -1;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

void disks_cmd(const char *dev, const char *op, const char *arg, char *out, size_t outmax) {
    o_reset(out, outmax);
    if (!dev || !dev[0]) {
        d_str("cfdisk — partition explorer (MBR)\n\n");
        d_str("usage:  cfdisk [disk] [del n | mkpart | wipe yes | zap yes]\n\n");
        int n = blockdev_count();
        for (int i = 0; i < n; i++) {
            blockdev_t *d = blockdev_at(i);
            if (!d || part_is_view(d)) continue;
            d_str("  "); d_str(d->name); d_str("  "); size_of(d->sectors);
            if (is_ram(d)) d_str("  (ram)");
            d_nl();
        }
        return;
    }
    blockdev_t *d = blockdev_get(dev);
    if (!d) { d_str("no such disk: "); d_str(dev); d_nl(); return; }
    if (part_is_view(d)) { d_str("point cfdisk at the whole disk\n"); return; }
    if (is_ram(d)) { d_str("cannot partition ram0\n"); return; }

    if (!op || !op[0]) { show(d); return; }

    part_table_t t;
    if (!part_read(d, &t)) { d_str("read failed\n"); return; }

    if (eqs(op, "del") || eqs(op, "delete") || eqs(op, "d")) {
        int n = parse_u(arg);
        if (n < 1 || n > 4) { d_str("usage: cfdisk "); d_str(dev); d_str(" del <1-4>\n"); return; }
        if (!t.e[n - 1].used) { d_str("slot empty\n"); return; }
        t.e[n - 1].used = false; t.e[n - 1].sectors = 0;
        if (!part_write_table(d, &t)) { d_str("write failed\n"); return; }
        d_str("deleted partition "); d_dec((uint64_t)n); d_nl();
        show(d);
        return;
    }
    if (eqs(op, "mkpart") || eqs(op, "n") || eqs(op, "new")) {
        int slot = -1;
        for (int i = 0; i < 4; i++) if (!t.e[i].used) { slot = i; break; }
        if (slot < 0) { d_str("no free primary slot\n"); return; }
        uint8_t type = PART_TYPE_RXOS;
        if (arg && arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
            /* crude hex */
            type = 0;
            for (const char *p = arg + 2; *p; p++) {
                type <<= 4;
                if (*p >= '0' && *p <= '9') type |= (uint8_t)(*p - '0');
                else if (*p >= 'a' && *p <= 'f') type |= (uint8_t)(*p - 'a' + 10);
                else if (*p >= 'A' && *p <= 'F') type |= (uint8_t)(*p - 'A' + 10);
            }
        }
        uint32_t start = PART_FIRST_LBA;
        for (int i = 0; i < 4; i++)
            if (t.e[i].used) {
                uint32_t end = t.e[i].start_lba + t.e[i].sectors;
                if (end > start) start = end;
            }
        if (start + 2048 >= d->sectors) { d_str("no free space\n"); return; }
        t.e[slot].used = true;
        t.e[slot].bootable = (type == PART_TYPE_RXOS);
        t.e[slot].type = type;
        t.e[slot].start_lba = start;
        t.e[slot].sectors = d->sectors - start;
        t.valid = true;
        if (!part_write_table(d, &t)) { d_str("write failed\n"); return; }
        d_str("created "); d_str(d->name); d_dec((uint64_t)(slot + 1));
        d_str(" type 0x"); d_dec(type); d_nl();
        show(d);
        return;
    }
    if (eqs(op, "wipe")) {
        if (!eqs(arg, "yes")) { d_str("confirm: cfdisk "); d_str(dev); d_str(" wipe yes\n"); return; }
        k_memset(&t, 0, sizeof(t));
        if (!part_write_table(d, &t)) { d_str("write failed\n"); return; }
        d_str("wrote empty MBR on "); d_str(dev); d_nl();
        return;
    }
    if (eqs(op, "zap")) {
        if (!eqs(arg, "yes")) { d_str("confirm: cfdisk "); d_str(dev); d_str(" zap yes\n"); return; }
        part_wipe_gpt(d);
        k_memset(&t, 0, sizeof(t));
        part_write_table(d, &t);
        d_str("zapped GPT+MBR on "); d_str(dev); d_nl();
        return;
    }
    show(d);
}

static char g_line[96];
int disks_gui_count(void) {
    int n = 0;
    for (int i = 0; i < blockdev_count(); i++) {
        blockdev_t *d = blockdev_at(i);
        if (d && !part_is_view(d)) n++;
    }
    return n;
}
const char *disks_gui_line(int idx) {
    int n = 0;
    g_line[0] = 0;
    for (int i = 0; i < blockdev_count(); i++) {
        blockdev_t *d = blockdev_at(i);
        if (!d || part_is_view(d)) continue;
        if (n++ != idx) continue;
        char *p = g_line, *e = g_line + sizeof(g_line);
        const char *s = d->name;
        while (*s && p < e - 1) *p++ = *s++;
        *p++ = ' ';
        uint64_t mib = d->sectors * BLOCKDEV_SECTOR / (1024 * 1024);
        /* tiny itoa */
        char tmp[12]; int k = 0; uint64_t v = mib ? mib : 1;
        if (!mib) { tmp[k++] = '0'; }
        else { while (v) { tmp[k++] = (char)('0' + (v % 10)); v /= 10; } }
        while (k && p < e - 1) *p++ = tmp[--k];
        s = " MiB";
        while (*s && p < e - 1) *p++ = *s++;
        if (is_ram(d)) { s = " ram"; while (*s && p < e - 1) *p++ = *s++; }
        *p = 0;
        return g_line;
    }
    return "";
}
