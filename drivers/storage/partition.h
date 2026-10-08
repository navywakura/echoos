/* Partition table helpers. */
#ifndef RXOS_PARTITION_H
#define RXOS_PARTITION_H

#include "rogex.h"
#include "blockdev.h"

#define PART_MAX_PRIMARY 4

/* 0x7F is the "Alt-OS-Development Partition Standard" type byte: reserved for
   exactly this, an operating system that is not one of the mainstream ones. */
#define PART_TYPE_RXOS   0x7F
#define PART_TYPE_LINUX  0x83

/* 8 MiB boot gap (MBR + stage2 + RXBT + kernel). Older images that put the
   EchOS partition at LBA 2048 still mount — we read the table, we do not
   assume this constant on load. */
#define PART_FIRST_LBA   16384u

typedef struct {
    bool     used;
    bool     bootable;
    uint8_t  type;              /* MBR type byte: 0x83 Linux, 0x07 NTFS, ... */
    uint32_t start_lba;
    uint32_t sectors;
} part_entry_t;

typedef struct {
    bool        valid;          /* a 0x55AA signature was present            */
    bool        gpt;            /* GPT header found in LBA 1 — do not touch  */
    int         count;          /* populated entries                         */
    part_entry_t e[PART_MAX_PRIMARY];
} part_table_t;

/* Read and parse sector 0 (and sector 1, to spot GPT). Always fills *out. */
bool part_read(blockdev_t *disk, part_table_t *out);

/* Human-readable name for an MBR type byte ("Linux", "NTFS/exFAT", "EchOS"). */
const char *part_type_name(uint8_t type);

/* Replace the partition table with a single partition of `type` spanning from
   PART_FIRST_LBA to the end of the disk. DESTRUCTIVE: it overwrites sector 0.
   Writes a fresh MBR. When `bootable` is true the partition is marked active
   (0x80) so a Multiboot-aware bootloader on the disk can hand off to it. */
bool part_write_single(blockdev_t *disk, uint8_t type);
bool part_write_single_ex(blockdev_t *disk, uint8_t type, bool bootable);

/* Write the four primary MBR slots from `t` (used entries only). */
bool part_write_table(blockdev_t *disk, const part_table_t *t);

/* Erase both GPT copies (primary header + entries at LBA 1..33, backup at the
   end of the disk) so an MBR written afterwards is what the disk actually has.
   DESTRUCTIVE and irreversible. Writing an MBR without this leaves the GPT
   headers in place and the disk keeps reading as GPT. */
bool part_wipe_gpt(blockdev_t *disk);

/* Write a GPT partition table with a single Linux partition. Returns true on success. */
bool part_write_gpt_single(blockdev_t *disk, uint8_t type);

/* Re-read a GPT and check it the way firmware does: signatures, both header
   CRCs, both array CRCs, and the two headers agreeing. *why names the first
   failure, or "valid". */
bool part_verify_gpt(blockdev_t *disk, const char **why);

/* Expose [start, start+sectors) of `parent` as its own block device and
   register it. `suffix` is appended to the parent's name ("sda" + "1").
   Returns NULL if there is no room in the registry or the range is bad. */
blockdev_t *part_attach(blockdev_t *parent, uint32_t start, uint32_t sectors,
                        char suffix);

/* Find a partition of PART_TYPE_RXOS and attach it. NULL when the disk has no
   partition table, no EchOS partition, or is GPT. */
blockdev_t *part_find_rxos(blockdev_t *disk);

/* True when this block device is a partition view we attached, rather than a
   whole disk. Exact identity, not a guess from the name. */
bool part_is_view(const blockdev_t *d);

/* True when this block device is the in-RAM disk (ramdisk). */
bool is_ram(const blockdev_t *d);

#endif /* RXOS_PARTITION_H */
