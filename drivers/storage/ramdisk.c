/* In-RAM block device for FS self-tests and amnesic boots. */
#include "ramdisk.h"

static uint8_t g_disk[RAMDISK_BYTES];
static blockdev_t g_ram0;

static bool ram_rw(blockdev_t *dev, uint32_t lba, void *buf, uint32_t count, bool write) {
    (void)dev;
    uint64_t off = (uint64_t)lba * BLOCKDEV_SECTOR;
    uint64_t len = (uint64_t)count * BLOCKDEV_SECTOR;
    if (off + len > RAMDISK_BYTES) return false;
    if (write) k_memcpy(&g_disk[off], buf, len);
    else       k_memcpy(buf, &g_disk[off], len);
    return true;
}

static bool ram_read(blockdev_t *d, uint32_t lba, void *buf, uint32_t n) {
    return ram_rw(d, lba, buf, n, false);
}
static bool ram_write(blockdev_t *d, uint32_t lba, const void *buf, uint32_t n) {
    return ram_rw(d, lba, (void *)buf, n, true);
}

bool ramdisk_init(void) {
    g_ram0.name    = "ram0";
    g_ram0.sectors = RAMDISK_BYTES / BLOCKDEV_SECTOR;
    g_ram0.read    = ram_read;
    g_ram0.write   = ram_write;
    g_ram0.ctx     = g_disk;
    return blockdev_register(&g_ram0);
}

blockdev_t *ramdisk_dev(void) { return &g_ram0; }

/* True when this block device is the in-RAM disk (ramdisk). */
bool is_ram(const blockdev_t *d) {
    return d && d->name[0] == 'r' && d->name[1] == 'a' && d->name[2] == 'm';
}
