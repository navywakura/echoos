/*
 * Partition-aware install wizard. Destructive if you say yes — double-check
 * the target device. Bugs in disk tooling are high impact; test on images first.
 */
#include "install.h"
#include "blockdev.h"
#include "disk.h"
#include "partition.h"
#include "rxfs.h"
#include "vfs.h"
#include "hdboot.h"
#include "commands.h"    /* o_str / o_dec / o_nl: the shared output builder */

static bool eq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}
/* A partition view we attached, not a whole disk. Asked of the partition
   layer directly: guessing from a trailing digit in the name would misjudge
   any future device whose name happens to end in one. */
static bool is_view(const blockdev_t *d) { return part_is_view(d); }

static void size_of(uint64_t sectors) {
    uint64_t kib = sectors * BLOCKDEV_SECTOR / 1024;
    if (kib >= 1024 * 1024) { o_dec(kib / (1024 * 1024)); o_str(" GiB"); }
    else if (kib >= 1024)   { o_dec(kib / 1024);          o_str(" MiB"); }
    else                    { o_dec(kib);                 o_str(" KiB"); }
}

/* ------------------------- what is already on it -------------------------- */
static bool has_rxfs_at_zero(blockdev_t *d) {
    static uint8_t s[BLOCKDEV_SECTOR];
    if (!d->read(d, 0, s, 1)) return false;
    return s[0] == 'R' && s[1] == 'X' && s[2] == 'F' && s[3] == '1';
}

/* Print the disk's partition map. Returns the table so callers can decide. */
static bool show_map(blockdev_t *d, part_table_t *t, const char *indent) {
    if (!part_read(d, t)) {
        o_str(indent); o_str("could not read sector 0 (I/O error)\n");
        return false;
    }
    if (t->gpt) {
        o_str(indent); o_str("GPT partition table\n");
        return true;
    }
    if (!t->valid) {
        o_str(indent);
        o_str(has_rxfs_at_zero(d)
              ? "no partition table - RXFS written directly to sector 0\n"
              : "no partition table (unpartitioned)\n");
        return true;
    }
    o_str(indent); o_str("MBR partition table:\n");
    for (int i = 0; i < PART_MAX_PRIMARY; i++) {
        if (!t->e[i].used) continue;
        o_str(indent); o_str("  "); o_str(d->name); o_dec((uint64_t)(i + 1));
        o_str("  "); size_of(t->e[i].sectors);
        o_str("  type 0x");
        { uint8_t v = t->e[i].type;
          const char *hx = "0123456789ABCDEF";
          char b[3] = { hx[v >> 4], hx[v & 0xF], '\0' };
          o_str(b); }
        o_str("  "); o_str(part_type_name(t->e[i].type));
        if (t->e[i].bootable) o_str("  [boot]");
        o_nl();
    }
    if (t->count == 0) { o_str(indent); o_str("  (table present but empty)\n"); }
    return true;
}

/* --------------------------------- step 1 --------------------------------- */
static void survey(void) {
    o_str("EchOS disk install wizard\n \n");
    o_str("Writes an MBR, a BIOS bootloader, the kernel, one EchOS partition\n");
    o_str("(type 0x7F), formats RXFS, and saves your profile so the next boot\n");
    o_str("comes from this disk — the USB can come out.\n \n");
    o_str("Manual path:  partition <disk>   then   install <disk> ERASE-<disk>\n \n");

    int n = blockdev_count(), candidates = 0;
    o_str("Disks found:\n \n");
    for (int i = 0; i < n; i++) {
        blockdev_t *d = blockdev_at(i);
        if (!d || is_view(d)) continue;          /* views are shown in the map */
        o_str("  "); o_str(d->name); o_str("   "); size_of(d->sectors);
        if (is_ram(d)) { o_str("   the in-RAM disk (erased at power-off)\n"); continue; }
        o_nl();
        part_table_t t;
        show_map(d, &t, "      ");
        candidates++;
    }
    o_nl();

    if (candidates == 0) {
        o_str("No persistent disk was detected, so there is nothing to install to.\n");
        o_str("EchOS drives AHCI SATA, legacy IDE and virtio-blk. If the machine\n");
        o_str("has a disk and this says otherwise, check the firmware's SATA mode\n");
        o_str("(AHCI is what you want; RAID is not supported). `devices` shows\n");
        o_str("what was actually found.\n");
        return;
    }
    o_str("Step 2 of 3 - pick the disk:\n \n    install <disk>\n \n");
    o_str("for example:   install sda\n");
}

/* What step 3 will do, printed before the confirmation phrase either way. */
static void plan(blockdev_t *d) {
    part_table_t t;
    bool gpt = part_read(d, &t) && t.gpt;
    int n = 1;
    o_str("The wizard will:\n");
    if (gpt) { o_str("  "); o_dec((uint64_t)n++); o_str(". erase both GPT copies\n"); }
    o_str("  "); o_dec((uint64_t)n++);
    o_str(". write an MBR with one EchOS partition (type 0x7F) from\n");
    o_str("     LBA 16384 (8 MiB boot area) to the end of the disk\n");
    o_str("  "); o_dec((uint64_t)n++); o_str(". format RXFS inside that partition\n");
    o_str("  "); o_dec((uint64_t)n++); o_str(". save the ");
    o_dec((uint64_t)vfs_count()); o_str(" file(s) in RAM, including your profile\n \n");
}

/* --------------------------------- step 2 --------------------------------- */
static void arm(const char *dev) {
    blockdev_t *d = blockdev_get(dev);
    if (!d) {
        o_str("no such disk: "); o_str(dev);
        o_str("\nRun `install` with no arguments to list what is available.\n");
        return;
    }
    if (is_ram(d)) {
        o_str("ram0 is the in-RAM disk - installing there would be erased at\n");
        o_str("power-off, which is the opposite of the point. Pick a real disk.\n");
        return;
    }
    if (is_view(d)) {
        o_str(dev); o_str(" is a partition, not a disk. Point the wizard at the\n");
        o_str("whole disk and it will manage the partition itself.\n");
        return;
    }

    o_str("Target: "); o_str(d->name); o_str("   "); size_of(d->sectors); o_str("\n \n");
    part_table_t t;
    if (!show_map(d, &t, "  ")) return;
    o_nl();

    if (t.gpt) {
        /* Action first, explanation second, and kept to about 20 lines: the
           shell's text view shows 26 and truncates the rest, so an earlier
           version buried the one line the user has to type below the fold. */
        o_str("+------------------------------------------------------------+\n");
        o_str("|  GPT DISK - needs converting to MBR first                  |\n");
        o_str("+------------------------------------------------------------+\n");
        o_str("  "); o_str(d->name); o_str("   "); size_of(d->sectors);
        o_str("      now: GPT      EchOS needs: MBR\n \n");

        o_str("  TO CONVERT AND INSTALL, TYPE:\n \n");
        o_str("      install "); o_str(d->name); o_str(" CONVERT-"); o_str(d->name); o_str("\n \n");
        o_str("  Or build an \"msdos\" table in GParted, then:\n");
        o_str("      install "); o_str(d->name); o_str(" ERASE-"); o_str(d->name); o_str("\n \n");

        o_str("  CONVERT: erase both GPT copies (front + backup at the end),\n");
        o_str("  write an MBR with one EchOS partition (0x7F), format RXFS in\n");
        o_str("  it, save your "); o_dec((uint64_t)vfs_count());
        o_str(" file(s) with your profile.\n \n");

        o_str("  !! Destroys the partition table. On an empty disk that is\n");
        o_str("     what you want; on a disk with data, everything on it\n");
        o_str("     becomes unreachable and EchOS cannot undo it.\n \n");
        o_str("  Nothing has been written yet.\n");
        return;
    }

    int occupied = 0;
    for (int i = 0; i < PART_MAX_PRIMARY; i++)
        if (t.e[i].used && t.e[i].type != PART_TYPE_RXOS) occupied++;

    if (occupied) {
        o_str("*** STOP AND READ ***\n \n");
        o_str("This disk already has "); o_dec((uint64_t)occupied);
        o_str(" partition(s) that are not EchOS. The\n");
        o_str("wizard writes a FRESH partition table with one EchOS partition,\n");
        o_str("which removes those entries. The data in them is not erased byte\n");
        o_str("by byte, but nothing will be able to find it, and EchOS has no\n");
        o_str("tool to put the table back.\n \n");
        o_str("If those partitions matter, stop here.\n \n");
    } else if (has_rxfs_at_zero(d)) {
        o_str("This disk has an older EchOS layout: RXFS written straight to\n");
        o_str("sector 0, with no partition table. Installing moves it into a\n");
        o_str("proper partition. Anything saved there and not loaded into this\n");
        o_str("session is lost - run `load` first to keep it.\n \n");
    } else if (t.count) {
        o_str("There is already an EchOS partition here. Installing replaces its\n");
        o_str("contents with what is currently in RAM; `load` first to keep what\n");
        o_str("is on the disk.\n \n");
    } else {
        o_str("The disk is empty. This is the straightforward case.\n \n");
    }

    plan(d);
    o_str("Step 3 of 3 - type this exactly to proceed:\n \n");
    o_str("    install "); o_str(d->name); o_str(" ERASE-"); o_str(d->name); o_str("\n");
    o_str("    install "); o_str(d->name); o_str(" REINSTALL-"); o_str(d->name); o_str("\n \n");
    o_str("Reinstalling over an older EchOS is allowed — that wipes the old\n");
    o_str("RXFS and writes a fresh one. The phrase names this disk on purpose.\n");
}

/* --------------------------------- step 3 --------------------------------- */
static void commit(blockdev_t *d, bool converting) {
    o_str("Installing to "); o_str(d->name); o_str("...\n \n");

    /* The GPT check in step 2 was advisory: only this one is load-bearing.
       Without it the confirmation phrase walked straight past the warning and
       wrote an MBR over a live GPT, leaving the disk in a hybrid state that
       could not be read back. A guard that only runs on the path the user
       reads is not a guard. */
    part_table_t t;
    if (!part_read(d, &t)) {
        o_str("  [FAIL] could not read the partition table. Nothing was written.\n");
        return;
    }
    if (t.gpt && !converting) {
        /* Hybrid leftovers (old ISO flashed to a disk, or a previous EchOS)
           still look like GPT. Reinstall is allowed: wipe and continue. */
        o_str("  [note] GPT leftover (or an older EchOS/ISO layout).\n");
        o_str("         Wiping it so the reinstall can proceed.\n");
        converting = true;
    }
    if (converting && !t.gpt) {
        o_str("  [note] this disk is not GPT after all - nothing to convert.\n");
        o_str("Continuing with the normal install.\n \n");
    }

    if (t.gpt) {
        if (!part_wipe_gpt(d)) {
            o_str("  [FAIL] could not erase the GPT copies (I/O error).\n");
            o_str("Nothing else was attempted.\n");
            return;
        }
        o_str("  [ok]   erased both GPT copies (primary + backup)\n");
    }

    if (!part_write_single(d, PART_TYPE_RXOS)) {
        o_str("  [FAIL] could not write the partition table (I/O error).\n");
        o_str("Nothing else was attempted.\n");
        return;
    }
    o_str("  [ok]   wrote an MBR with one EchOS partition\n");

    if (hdboot_write(d)) {
        o_str("  [ok]   wrote BIOS bootloader + kernel (disk will boot without USB)\n");
    } else {
        uint32_t plen = 0;
        hdboot_payload(&plen);
        o_str(plen ? "  [warn] bootloader write failed; USB still needed to boot\n"
                   : "  [note] no kernel payload on this LIVE image; USB still needed to boot\n");
    }

    blockdev_t *p = part_find_rxos(d);
    if (!p) {
        o_str("  [FAIL] the partition was written but could not be read back.\n");
        o_str("The disk is partitioned; nothing was formatted. This should not\n");
        o_str("happen - if it does, `install` again after a reboot.\n");
        return;
    }
    o_str("  [ok]   attached "); o_str(p->name); o_str("  ("); size_of(p->sectors); o_str(")\n");

    if (!rxfs_disk_format(p)) {
        o_str("  [FAIL] could not format RXFS inside the partition (I/O error).\n");
        return;
    }
    o_str("  [ok]   formatted RXFS\n");

    if (!rxfs_save(p)) {
        o_str("  [FAIL] could not save the filesystem (partition too small or I/O error).\n");
        o_str("The partition is formatted but empty. Try `save` again.\n");
        return;
    }
    o_str("  [ok]   saved "); o_dec((uint64_t)vfs_count());
    o_str(" file(s), including your profile\n \n");

    o_str("Done. Next reboot that sees this disk will say \"Welcome back\" and\n");
    o_str("restore these files - it finds the partition by type 0x7F.\n \n");
    o_str("From now on: `save` after changes you want to keep, `load` to pull\n");
    o_str("the disk copy back into RAM.\n \n");
    o_str("If the bootloader wrote successfully, remove the USB and reboot.\n");
    o_str("Otherwise the host-side path is still: make hdd && dd to the disk.\n");
}

bool install_to_disk(struct blockdev *dev, char *err, uint32_t errmax) {
    blockdev_t *d = (blockdev_t *)dev;
    if (err && errmax) err[0] = '\0';
    if (!d) {
        if (err && errmax) { const char *s = "no disk"; uint32_t i = 0;
            while (s[i] && i + 1 < errmax) { err[i] = s[i]; i++; } err[i] = '\0'; }
        return false;
    }
    /* Reuse the same path as `install <dev> ERASE-<dev>`. The wizard already
       confirmed; we do not print the long survey. */
    part_table_t t;
    if (!part_read(d, &t)) {
        if (err && errmax) { const char *s = "cannot read partition table";
            uint32_t i = 0; while (s[i] && i + 1 < errmax) { err[i] = s[i]; i++; } err[i] = '\0'; }
        return false;
    }
    commit(d, true);
    /* commit() prints; treat a formatted RXFS as success. */
    blockdev_t *p = part_find_rxos(d);
    if (!p || !rxfs_disk_has_image(p)) {
        if (err && errmax) { const char *s = "install did not produce RXFS";
            uint32_t i = 0; while (s[i] && i + 1 < errmax) { err[i] = s[i]; i++; } err[i] = '\0'; }
        return false;
    }
    return true;
}

/* -------------------------------- dispatch -------------------------------- */
void install_wizard(const char *dev, const char *confirm) {
    if (!dev || !dev[0]) { survey(); return; }
    if (!confirm || !confirm[0]) { arm(dev); return; }

    blockdev_t *d = blockdev_get(dev);
    if (!d) { o_str("no such disk: "); o_str(dev); o_nl(); return; }
    if (is_ram(d) || is_view(d)) { arm(dev); return; }

    /* Rebuild the expected phrases rather than parse the given one, so only an
       exact match for THIS disk can trigger a write. Two of them: converting a
       GPT disk destroys more than an install does, so it says so in the word
       the user has to type. */
    char erase[40], convert[40];
    { int n = 0;
      for (const char *p = "ERASE-"; *p && n < (int)sizeof(erase) - 1; p++) erase[n++] = *p;
      for (const char *p = d->name; *p && n < (int)sizeof(erase) - 1; p++) erase[n++] = *p;
      erase[n] = '\0'; }
    { int n = 0;
      for (const char *p = "CONVERT-"; *p && n < (int)sizeof(convert) - 1; p++) convert[n++] = *p;
      for (const char *p = d->name; *p && n < (int)sizeof(convert) - 1; p++) convert[n++] = *p;
      convert[n] = '\0'; }

    char reinstall[48];
    { int n = 0;
      for (const char *p = "REINSTALL-"; *p && n < (int)sizeof(reinstall) - 1; p++) reinstall[n++] = *p;
      for (const char *p = d->name; *p && n < (int)sizeof(reinstall) - 1; p++) reinstall[n++] = *p;
      reinstall[n] = '\0'; }

    if (eq(confirm, convert) || eq(confirm, reinstall) || eq(confirm, "yes")) {
        commit(d, true);  return;
    }
    if (eq(confirm, erase))   { commit(d, true); return; }

    o_str("That is not the confirmation phrase for "); o_str(d->name); o_str(".\n");
    o_str("Nothing was written. Run `install "); o_str(d->name);
    o_str("` to see which phrase\nthis disk needs.\n");
}
