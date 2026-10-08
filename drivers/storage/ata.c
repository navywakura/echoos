/*
 * ATA PIO, primary master. Polled (no IRQ14/DMA). LBA28, bounded timeouts,
 * FLUSH CACHE after writes. Works; not pretty. Known: only master, no ATAPI.
 */
#include "ata.h"
#include "ports.h"
#include "scheduler.h"

/* Primary channel I/O ports. */
#define ATA_DATA    0x1f0
#define ATA_ERROR   0x1f1
#define ATA_SECCNT  0x1f2
#define ATA_LBA_LO  0x1f3
#define ATA_LBA_MID 0x1f4
#define ATA_LBA_HI  0x1f5
#define ATA_DRIVE   0x1f6
#define ATA_STATUS  0x1f7          /* read: status, write: command */
#define ATA_CMD     0x1f7
#define ATA_ALTSTAT 0x3f6          /* alternate status (no interrupt ack)  */

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_BSY  0x80

#define CMD_READ28   0x20
#define CMD_WRITE28  0x30
#define CMD_FLUSH    0xe7
#define CMD_IDENTIFY 0xec

#define POLL_BUDGET 2000000        /* bounded spins so a dead disk can't hang boot */

static bool       g_present;
static uint32_t   g_sectors;
static blockdev_t g_hda;

/* ~400 ns settle: four alternate-status reads. */
static void ata_delay(void) {
    inb(ATA_ALTSTAT); inb(ATA_ALTSTAT); inb(ATA_ALTSTAT); inb(ATA_ALTSTAT);
}

static bool wait_not_busy(void) {
    for (int i = 0; i < POLL_BUDGET; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (!(s & ST_BSY)) return true;
        sched_poll_relax(i);          /* cooperative: yield during transfers */
    }
    return false;
}

/* Wait for DRQ (data ready). False on timeout, ERR or device fault. */
static bool wait_drq(void) {
    for (int i = 0; i < POLL_BUDGET; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & (ST_ERR | ST_DF)) return false;
        if (!(s & ST_BSY) && (s & ST_DRQ)) return true;
        sched_poll_relax(i);          /* cooperative: yield during transfers */
    }
    return false;
}

static void select_lba(uint32_t lba, uint8_t count) {
    outb(ATA_DRIVE, (uint8_t)(0xe0 | ((lba >> 24) & 0x0f)));  /* master, LBA mode */
    ata_delay();
    outb(ATA_SECCNT, count);
    outb(ATA_LBA_LO,  (uint8_t)(lba & 0xff));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xff));
    outb(ATA_LBA_HI,  (uint8_t)((lba >> 16) & 0xff));
}

static bool ata_read(blockdev_t *dev, uint32_t lba, void *buf, uint32_t count) {
    (void)dev;
    if (!g_present || lba + count > g_sectors || count == 0 || count > 255) return false;
    select_lba(lba, (uint8_t)count);
    outb(ATA_CMD, CMD_READ28);
    uint16_t *p = (uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (!wait_drq()) return false;
        for (int w = 0; w < 256; w++) *p++ = inw(ATA_DATA);
        ata_delay();
    }
    return true;
}

static bool ata_write(blockdev_t *dev, uint32_t lba, const void *buf, uint32_t count) {
    (void)dev;
    if (!g_present || lba + count > g_sectors || count == 0 || count > 255) return false;
    select_lba(lba, (uint8_t)count);
    outb(ATA_CMD, CMD_WRITE28);
    const uint16_t *p = (const uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (!wait_drq()) return false;
        for (int w = 0; w < 256; w++) outw(ATA_DATA, *p++);
        ata_delay();
    }
    /* Durability: flush the drive cache and wait for it to settle. */
    outb(ATA_CMD, CMD_FLUSH);
    return wait_not_busy() && !(inb(ATA_STATUS) & (ST_ERR | ST_DF));
}

bool ata_init(void) {
    g_present = false;

    /* Floating bus (no controller at all) reads 0xFF. */
    if (inb(ATA_STATUS) == 0xff) return false;

    /* IDENTIFY the primary master. */
    outb(ATA_DRIVE, 0xa0);
    ata_delay();
    outb(ATA_SECCNT, 0); outb(ATA_LBA_LO, 0); outb(ATA_LBA_MID, 0); outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, CMD_IDENTIFY);
    if (inb(ATA_STATUS) == 0) return false;            /* no drive          */
    if (!wait_not_busy()) return false;
    /* ATAPI/SATA signature? Not a plain ATA disk — leave it alone. */
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) return false;
    if (!wait_drq()) return false;

    uint16_t id[256];
    for (int w = 0; w < 256; w++) id[w] = inw(ATA_DATA);

    /* Words 60-61: total LBA28 sectors. */
    g_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (g_sectors == 0) return false;

    g_hda.name    = "hda";
    g_hda.sectors = g_sectors;
    g_hda.read    = ata_read;
    g_hda.write   = ata_write;
    g_hda.ctx     = NULL;
    g_present     = blockdev_register(&g_hda);
    return g_present;
}

bool ata_present(void)   { return g_present; }
blockdev_t *ata_dev(void) { return g_present ? &g_hda : NULL; }
