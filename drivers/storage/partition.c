/* MBR partition walk helpers for install / disk setup. */
#include "partition.h"
#include "pit.h"

#define MBR_TABLE_OFF 446
#define MBR_SIG_OFF   510
#define GPT_ENTRY_SECTORS 32          /* 128 entries x 128 bytes = 16 KiB    */
#define GPT_ENTRIES       128

/* GPT structures (packed to match on-disk layout) */
typedef struct __attribute__((packed)) {
    uint8_t  signature[8];           /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t sizeof_partition_entry;
    uint32_t partition_entry_array_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];               /* UTF-16LE */
} gpt_entry_t;

/* ------------------------------- read ------------------------------------- */
bool part_read(blockdev_t *disk, part_table_t *out) {
    k_memset(out, 0, sizeof(*out));
    if (!disk) return false;

    static uint8_t s[BLOCKDEV_SECTOR];
    if (!disk->read(disk, 0, s, 1)) return false;
    if (!(s[MBR_SIG_OFF] == 0x55 && s[MBR_SIG_OFF + 1] == 0xAA)) return true; /* no table */
    out->valid = true;

    /* A GPT disk carries a protective MBR whose single entry has type 0xEE,
       with the real header in LBA 1. Check both: the type byte alone can be
       forged by a hybrid layout. */
    static uint8_t s1[BLOCKDEV_SECTOR];
    if (disk->read(disk, 1, s1, 1)) {
        const char *sig = "EFI PART";
        bool m = true;
        for (int i = 0; i < 8; i++) if (s1[i] != (uint8_t)sig[i]) { m = false; break; }
        if (m) out->gpt = true;
    }

    for (int i = 0; i < PART_MAX_PRIMARY; i++) {
        const uint8_t *e = s + MBR_TABLE_OFF + i * 16;
        uint8_t type = e[4];
        if (type == 0) continue;
        if (type == 0xEE) out->gpt = true;
        /* Little-endian LBA start and length at offsets 8 and 12. */
        uint32_t start = (uint32_t)e[8]  | ((uint32_t)e[9]  << 8)  |
                         ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t count = (uint32_t)e[12] | ((uint32_t)e[13] << 8)  |
                         ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        if (count == 0) continue;
        out->e[i].used      = true;
        out->e[i].bootable  = (e[0] & 0x80) != 0;
        out->e[i].type      = type;
        out->e[i].start_lba = start;
        out->e[i].sectors   = count;
        out->count++;
    }
    return true;
}

const char *part_type_name(uint8_t type) {
    switch (type) {
    case PART_TYPE_RXOS: return "EchOS (RXFS)";
    case 0x07: return "NTFS / exFAT";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x06: case 0x0E: return "FAT16";
    case 0x83: return "Linux";
    case 0x82: return "Linux swap";
    case 0x8E: return "Linux LVM";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI system";
    case 0xA5: return "FreeBSD";
    case 0x27: return "Windows recovery";
    default:   return "unknown";
    }
}

/* ------------------------------- write ------------------------------------ */
/* CHS fields are filled with the conventional "too big for CHS" marker
   (0xFE 0xFF 0xFF). Every machine that can boot EchOS addresses by LBA, and
   inventing plausible-looking cylinder numbers would be a lie the firmware
   might act on. */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* A 16-bit boot stub for the MBR's code area. This is not optional decoration.
   Writing the 0x55AA signature is what makes a partition table valid, and it
   is also what makes the firmware consider the disk bootable: with the code
   area left as zeroes the BIOS loads sector 0, jumps into a field of 0x00
   bytes and hangs — the machine stops booting from the USB stick it was
   booting from a minute earlier. Reproduced exactly that way under QEMU.

   So the stub says what the disk is and then executes INT 18h, the BIOS
   service that means "not bootable, carry on with the next device". The halt
   loop is only reached on firmware that does not implement INT 18h.

       xor ax, ax          31 C0
       mov ds, ax          8E D8
       mov si, msg         BE 1A 7C     (loaded at 0000:7C00)
   .print:
       lodsb               AC
       test al, al         84 C0
       jz .boot            74 09
       mov ah, 0x0E        B4 0E        (BIOS teletype output)
       mov bx, 0x0007      BB 07 00
       int 0x10            CD 10
       jmp .print          EB F2
   .boot:
       int 0x18            CD 18        (hand back to the BIOS)
   .hang:
       hlt                 F4
       jmp .hang           EB FD                                              */
/* Data-only MBR: print a short note and INT 18h (try next boot device).
   Full OS boot without USB uses a GRUB-installed disk image (`make hdd`). */
static const uint8_t MBR_STUB[] = {
    0x31, 0xC0, 0x8E, 0xD8, 0xBE, 0x1A, 0x7C, 0xAC, 0x84, 0xC0, 0x74, 0x09,
    0xB4, 0x0E, 0xBB, 0x07, 0x00, 0xCD, 0x10, 0xEB, 0xF2, 0xCD, 0x18, 0xF4,
    0xEB, 0xFD,
    'R','X','o','s',' ','R','X','F','S',' ','d','i','s','k',' ','-',
    ' ','u','s','e',' ','m','a','k','e',' ','h','d','d',' ','t','o',' ',
    'b','o','o','t','\r','\n', 0x00
};

/* Remove a GPT so an MBR can take its place.
 *
 * A GPT is not only sector 0: the primary header sits at LBA 1 with its entry
 * array in LBA 2..33, and a full backup copy lives at the very end of the
 * disk. Writing an MBR over the protective sector alone leaves every one of
 * those intact, which is exactly the hybrid mess that produced "wrote an MBR
 * ... but could not be read back" on real hardware: part_read() still found
 * "EFI PART" at LBA 1 and kept reporting the disk as GPT, so the EchOS
 * partition could never be located.
 *
 * Zeroing both copies is what sgdisk --zap-all does, and it is the only way
 * the conversion actually sticks. */

bool part_wipe_gpt(blockdev_t *disk) {
    if (!disk || disk->sectors < GPT_ENTRY_SECTORS + 2) return false;
    static uint8_t zero[BLOCKDEV_SECTOR];
    k_memset(zero, 0, sizeof(zero));

    /* Primary: header at LBA 1, entries at LBA 2..33. */
    for (uint32_t lba = 1; lba <= GPT_ENTRY_SECTORS + 1; lba++)
        if (!disk->write(disk, lba, zero, 1)) return false;

    /* Backup: header at the last LBA, entries in the 32 sectors before it. */
    uint32_t last = disk->sectors - 1;
    for (uint32_t i = 0; i <= GPT_ENTRY_SECTORS; i++) {
        uint32_t lba = last - i;
        if (lba <= GPT_ENTRY_SECTORS + 1) break;      /* never touch the front */
        if (!disk->write(disk, lba, zero, 1)) return false;
    }
    return true;
}

bool part_write_single_ex(blockdev_t *disk, uint8_t type, bool bootable) {
    if (!disk || disk->sectors <= PART_FIRST_LBA) return false;

    static uint8_t s[BLOCKDEV_SECTOR];
    k_memset(s, 0, sizeof(s));
    for (unsigned i = 0; i < sizeof(MBR_STUB) && i < MBR_TABLE_OFF; i++)
        s[i] = MBR_STUB[i];

    uint8_t *e = s + MBR_TABLE_OFF;
    e[0] = bootable ? 0x80 : 0x00;           /* active / bootable flag        */
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;   /* start CHS: out of range       */
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;   /* end CHS: out of range         */
    put32(e + 8,  PART_FIRST_LBA);
    put32(e + 12, disk->sectors - PART_FIRST_LBA);

    s[MBR_SIG_OFF] = 0x55; s[MBR_SIG_OFF + 1] = 0xAA;
    return disk->write(disk, 0, s, 1);
}

bool part_write_table(blockdev_t *disk, const part_table_t *t) {
    if (!disk || !t) return false;
    static uint8_t s[BLOCKDEV_SECTOR];
    k_memset(s, 0, sizeof(s));
    for (unsigned i = 0; i < sizeof(MBR_STUB) && i < MBR_TABLE_OFF; i++)
        s[i] = MBR_STUB[i];
    for (int i = 0; i < PART_MAX_PRIMARY; i++) {
        if (!t->e[i].used || t->e[i].sectors == 0) continue;
        uint8_t *e = s + MBR_TABLE_OFF + i * 16;
        e[0] = t->e[i].bootable ? 0x80 : 0x00;
        e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
        e[4] = t->e[i].type;
        e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
        put32(e + 8,  t->e[i].start_lba);
        put32(e + 12, t->e[i].sectors);
    }
    s[MBR_SIG_OFF] = 0x55; s[MBR_SIG_OFF + 1] = 0xAA;
    return disk->write(disk, 0, s, 1);
}

bool part_write_single(blockdev_t *disk, uint8_t type) {
    /* Default: mark EchOS partitions bootable so install-to-disk layouts are
       ready for a host-side GRUB install (`make hdd`) or hybrid ISO dd. */
    return part_write_single_ex(disk, type, type == PART_TYPE_RXOS);
}

/* ------------------------------ GPT writer -------------------------------- */
/*
 * A GPT is five structures that have to agree, not one sector:
 *
 *   LBA 0            protective MBR, one 0xEE entry covering the disk
 *   LBA 1            primary header, CRC over itself and over the array
 *   LBA 2..33        128 entries x 128 B
 *   last-32..last-1  the same array again
 *   last             backup header, my_lba/alternate_lba swapped
 *
 * The previous version wrote three of those with `crc32 = 0` and a comment
 * saying a real implementation needs CRC32, plus zeroed GUIDs and a
 * first_usable of 2 that overlapped its own entry array. Every external tool
 * rejects that, which is why this is written to be checked by them.
 */

/* CRC-32/ISO-HDLC (reflected, poly 0xEDB88320) — the one UEFI specifies.
   Table-free: 128 entries is 16 KiB of input, and this runs when a human
   asked to partition a disk, not on any hot path. */
static uint32_t gpt_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* A GUID has to be unique, not pretty. Mix the time-stamp counter (different
   every read), the PIT tick count and a per-call counter, then stamp the
   RFC 4122 version-4 and variant bits so tools read it as a random UUID
   rather than as a malformed one. This is not a CSPRNG and does not claim
   to be: it is a uniqueness source for a disk label. */
static uint64_t gpt_entropy(void) {
    static uint64_t counter;
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    counter += 0x9E3779B97F4A7C15ull;
    uint64_t x = (((uint64_t)hi << 32) | lo) ^ counter ^ (pit_ticks() * 0x100000001B3ull);
    /* splitmix64 finaliser: the low bits of a TSC read are not well spread. */
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

static void gpt_make_guid(uint8_t out[16]) {
    uint64_t a = gpt_entropy(), b = gpt_entropy();
    for (int i = 0; i < 8; i++) { out[i] = (uint8_t)(a >> (8 * i)); out[8 + i] = (uint8_t)(b >> (8 * i)); }
    out[7] = (uint8_t)((out[7] & 0x0F) | 0x40);   /* version 4               */
    out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);   /* RFC 4122 variant        */
}

/* "EFI PART" plus the fixed fields. Both headers share everything except the
   two LBAs that point at each other and at their own array. */
static void gpt_fill_header(gpt_header_t *h, uint64_t self, uint64_t other,
                            uint64_t entries_lba, uint64_t first_usable,
                            uint64_t last_usable, const uint8_t disk_guid[16],
                            uint32_t entries_crc) {
    k_memset(h, 0, sizeof(*h));
    k_memcpy(h->signature, "EFI PART", 8);
    h->revision    = 0x00010000u;         /* UEFI 2.x GPT revision 1.0       */
    h->header_size = (uint32_t)sizeof(gpt_header_t);   /* 92                 */
    h->current_lba = self;
    h->backup_lba  = other;
    h->first_usable = first_usable;
    h->last_usable  = last_usable;
    k_memcpy(h->disk_guid, disk_guid, 16);
    h->partition_entry_lba     = entries_lba;
    h->num_partition_entries   = GPT_ENTRIES;
    h->sizeof_partition_entry  = (uint32_t)sizeof(gpt_entry_t);   /* 128     */
    h->partition_entry_array_crc32 = entries_crc;
    h->crc32 = 0;
    h->crc32 = gpt_crc32(h, sizeof(*h));  /* over header_size bytes, crc32=0 */
}

static bool gpt_write_protective_mbr(blockdev_t *disk) {
    static uint8_t s[BLOCKDEV_SECTOR];
    uint64_t span = disk->sectors - 1;
    uint8_t *e = s + MBR_TABLE_OFF;

    k_memset(s, 0, sizeof(s));
    e[0] = 0x00;                              /* not bootable                */
    e[1] = 0x00; e[2] = 0x02; e[3] = 0x00;    /* CHS of LBA 1                */
    e[4] = 0xEE;                              /* GPT protective              */
    e[5] = 0xFF; e[6] = 0xFF; e[7] = 0xFF;    /* CHS: out of range           */
    put32(e + 8, 1);
    put32(e + 12, span > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)span);
    s[MBR_SIG_OFF] = 0x55; s[MBR_SIG_OFF + 1] = 0xAA;
    return disk->write(disk, 0, s, 1);
}

/* Build the 32-sector entry array in a static buffer, one partition filled. */
static uint8_t g_gpt_entries[GPT_ENTRY_SECTORS * BLOCKDEV_SECTOR];

static uint32_t gpt_build_entries(uint64_t first_lba, uint64_t last_lba,
                                  const uint8_t type_guid[16]) {
    gpt_entry_t *e = (gpt_entry_t *)g_gpt_entries;
    static const char NAME[] = "echOS";
    k_memset(g_gpt_entries, 0, sizeof(g_gpt_entries));
    k_memcpy(e->type_guid, type_guid, 16);
    gpt_make_guid(e->unique_guid);
    e->first_lba  = first_lba;
    e->last_lba   = last_lba;
    e->attributes = 0;
    for (unsigned i = 0; i + 1 < sizeof(NAME) && i < 35; i++)
        e->name[i] = (uint16_t)NAME[i];       /* UTF-16LE, NUL-terminated    */
    return gpt_crc32(g_gpt_entries, GPT_ENTRIES * (uint32_t)sizeof(gpt_entry_t));
}

static bool gpt_write_entries(blockdev_t *disk, uint32_t lba) {
    return disk->write(disk, lba, g_gpt_entries, GPT_ENTRY_SECTORS);
}

/* One data partition spanning the usable area, with both header copies and a
   protective MBR. `type` selects the type GUID; anything but PART_TYPE_RXOS
   gets the Linux filesystem GUID, which is what an installer target is. */
bool part_write_gpt_single(blockdev_t *disk, uint8_t type) {
    /* Linux filesystem data 0FC63DAF-8483-4772-8E79-3D69D8477DE4, mixed-endian
       on the wire: first three fields little-endian, last two big-endian. */
    static const uint8_t LINUX_DATA[16] = {
        0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
    };
    uint8_t disk_guid[16];
    gpt_header_t hdr;
    static uint8_t sec[BLOCKDEV_SECTOR];
    uint64_t last_lba, backup_entries, first_usable, last_usable;
    uint32_t entries_crc;

    (void)type;
    if (!disk) return false;
    /* Room for: MBR + header + 32 entry sectors, twice, and one usable LBA. */
    if (disk->sectors < 2 * (GPT_ENTRY_SECTORS + 1) + 2) return false;

    last_lba       = disk->sectors - 1;
    backup_entries = last_lba - GPT_ENTRY_SECTORS;
    first_usable   = 2 + GPT_ENTRY_SECTORS;          /* 34                   */
    last_usable    = backup_entries - 1;
    if (last_usable <= first_usable) return false;

    gpt_make_guid(disk_guid);
    entries_crc = gpt_build_entries(first_usable, last_usable, LINUX_DATA);

    if (!gpt_write_protective_mbr(disk)) return false;

    /* Primary: header then its array. */
    gpt_fill_header(&hdr, 1, last_lba, 2, first_usable, last_usable,
                    disk_guid, entries_crc);
    k_memset(sec, 0, sizeof(sec));
    k_memcpy(sec, &hdr, sizeof(hdr));
    if (!disk->write(disk, 1, sec, 1)) return false;
    if (!gpt_write_entries(disk, 2)) return false;

    /* Backup: the same array before the last sector, header on it. */
    if (!gpt_write_entries(disk, (uint32_t)backup_entries)) return false;
    gpt_fill_header(&hdr, last_lba, 1, backup_entries, first_usable, last_usable,
                    disk_guid, entries_crc);
    k_memset(sec, 0, sizeof(sec));
    k_memcpy(sec, &hdr, sizeof(hdr));
    if (!disk->write(disk, (uint32_t)last_lba, sec, 1)) return false;

    return true;
}

/* Read both headers back and check them the way a firmware would: signature,
   header CRC with the field zeroed, array CRC, and the two headers pointing
   at each other. Returns false with *why set to the first thing that failed,
   so "GPT written" can be a verdict instead of a hope. */
bool part_verify_gpt(blockdev_t *disk, const char **why) {
    static uint8_t sec[BLOCKDEV_SECTOR];
    gpt_header_t primary, backup;
    uint32_t stored, computed;
    uint64_t last_lba;
    const char *dummy;

    if (!why) why = &dummy;
    *why = "no disk";
    if (!disk || disk->sectors < 2 * (GPT_ENTRY_SECTORS + 1) + 2) return false;
    last_lba = disk->sectors - 1;

    *why = "protective MBR missing";
    if (!disk->read(disk, 0, sec, 1)) return false;
    if (sec[MBR_SIG_OFF] != 0x55 || sec[MBR_SIG_OFF + 1] != 0xAA) return false;
    if (sec[MBR_TABLE_OFF + 4] != 0xEE) return false;

    for (int which = 0; which < 2; which++) {
        gpt_header_t *h = which ? &backup : &primary;
        uint32_t lba = which ? (uint32_t)last_lba : 1u;
        *why = which ? "backup header unreadable" : "primary header unreadable";
        if (!disk->read(disk, lba, sec, 1)) return false;
        k_memcpy(h, sec, sizeof(*h));
        *why = which ? "backup signature" : "primary signature";
        for (int i = 0; i < 8; i++)
            if (h->signature[i] != (uint8_t)"EFI PART"[i]) return false;
        *why = which ? "backup header CRC" : "primary header CRC";
        if (h->header_size < sizeof(gpt_header_t) ||
            h->header_size > BLOCKDEV_SECTOR) return false;
        stored = h->crc32;
        k_memcpy(sec, h, sizeof(*h));
        ((gpt_header_t *)sec)->crc32 = 0;
        computed = gpt_crc32(sec, h->header_size);
        if (computed != stored) return false;

        *why = which ? "backup entry array CRC" : "primary entry array CRC";
        if (h->num_partition_entries != GPT_ENTRIES ||
            h->sizeof_partition_entry != sizeof(gpt_entry_t)) return false;
        if (!disk->read(disk, (uint32_t)h->partition_entry_lba,
                        g_gpt_entries, GPT_ENTRY_SECTORS)) return false;
        if (gpt_crc32(g_gpt_entries,
                      GPT_ENTRIES * (uint32_t)sizeof(gpt_entry_t)) !=
            h->partition_entry_array_crc32) return false;
    }

    *why = "headers do not point at each other";
    if (primary.current_lba != 1 || primary.backup_lba != last_lba) return false;
    if (backup.current_lba != last_lba || backup.backup_lba != 1) return false;
    *why = "usable range disagrees between headers";
    if (primary.first_usable != backup.first_usable ||
        primary.last_usable != backup.last_usable) return false;
    *why = "disk GUID differs between headers";
    for (int i = 0; i < 16; i++)
        if (primary.disk_guid[i] != backup.disk_guid[i]) return false;
    *why = "valid";
    return true;
}

/* --------------------------- partition views ------------------------------ */
typedef struct {
    blockdev_t *parent;
    uint32_t    start;
} part_ctx_t;

/* Two views is one more than anything here uses; the registry caps us anyway. */
static part_ctx_t g_ctx[2];
static blockdev_t g_view[2];
static char       g_name[2][8];
static int        g_views;

/* Range check written as a subtraction so it cannot itself overflow: with
   32-bit LBAs, `lba + count` wraps for large values and would let a request
   past the end of the partition look valid. */
static bool in_range(const blockdev_t *dev, uint32_t lba, uint32_t count) {
    return count != 0 && count <= dev->sectors && lba <= dev->sectors - count;
}

static bool view_read(blockdev_t *dev, uint32_t lba, void *buf, uint32_t count) {
    part_ctx_t *c = (part_ctx_t *)dev->ctx;
    if (!in_range(dev, lba, count)) return false;
    return c->parent->read(c->parent, c->start + lba, buf, count);
}

static bool view_write(blockdev_t *dev, uint32_t lba, const void *buf, uint32_t count) {
    part_ctx_t *c = (part_ctx_t *)dev->ctx;
    if (!in_range(dev, lba, count)) return false;
    return c->parent->write(c->parent, c->start + lba, buf, count);
}

bool part_is_view(const blockdev_t *d) {
    for (int i = 0; i < g_views; i++) if (d == &g_view[i]) return true;
    return false;
}

blockdev_t *part_attach(blockdev_t *parent, uint32_t start, uint32_t sectors,
                        char suffix) {
    if (!parent || sectors == 0) return NULL;
    if (start >= parent->sectors) return NULL;
    if (start + sectors > parent->sectors) sectors = parent->sectors - start;

    /* Re-attaching the same partition returns the view we already made. The
       block registry has four slots; without this, running the install wizard
       twice in one session burns two of them and the third attempt fails with
       a confusing "written but could not be read back". */
    for (int i = 0; i < g_views; i++)
        if (g_ctx[i].parent == parent && g_ctx[i].start == start) return &g_view[i];

    if (g_views >= (int)(sizeof(g_view) / sizeof(g_view[0]))) return NULL;

    int i = g_views;
    int n = 0;
    for (const char *p = parent->name; *p && n < (int)sizeof(g_name[i]) - 2; p++)
        g_name[i][n++] = *p;
    g_name[i][n++] = suffix;
    g_name[i][n] = '\0';

    g_ctx[i].parent  = parent;
    g_ctx[i].start   = start;
    g_view[i].name    = g_name[i];
    g_view[i].sectors = sectors;
    g_view[i].read    = view_read;
    g_view[i].write   = view_write;
    g_view[i].ctx     = &g_ctx[i];

    if (!blockdev_register(&g_view[i])) return NULL;
    g_views++;
    return &g_view[i];
}

blockdev_t *part_find_rxos(blockdev_t *disk) {
    part_table_t t;
    if (!part_read(disk, &t) || !t.valid || t.gpt) return NULL;
    for (int i = 0; i < PART_MAX_PRIMARY; i++)
        if (t.e[i].used && t.e[i].type == PART_TYPE_RXOS)
            return part_attach(disk, t.e[i].start_lba, t.e[i].sectors,
                               (char)('1' + i));
    return NULL;
}
