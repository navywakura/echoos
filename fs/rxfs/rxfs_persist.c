/* Save/load the RXFS image to a block device. Guard rails avoid clobbering MBR-looking disks. */
#include "rxfs.h"
#include "blockdev.h"

/* One file record occupies this many sectors on the device. */
#define REC_SECTORS  ((sizeof(rxfs_img_file) + BLOCKDEV_SECTOR - 1) / BLOCKDEV_SECTOR)
#define HDR_SECTORS  1

static uint8_t g_sec[REC_SECTORS * BLOCKDEV_SECTOR];   /* sector bounce buffer */

bool rxfs_save(struct blockdev *dev) {
    blockdev_t *bd = (blockdev_t *)dev;
    if (!bd) return false;

    char names[RXFS_MAX_FILES][RXFS_NAME];
    int n = rxfs_list_paths(names, RXFS_MAX_FILES);

    uint32_t need = HDR_SECTORS + (uint32_t)n * REC_SECTORS;
    if (need > bd->sectors) return false;

    k_memset(g_sec, 0, BLOCKDEV_SECTOR);
    rxfs_img_header *h = (rxfs_img_header *)g_sec;
    h->magic = RXFS_IMG_MAGIC;
    h->version = 1;
    h->count = (uint32_t)n;
    if (!bd->write(bd, 0, g_sec, 1)) return false;

    uint32_t lba = HDR_SECTORS;
    for (int i = 0; i < n; i++) {
        k_memset(g_sec, 0, sizeof(g_sec));
        rxfs_img_file *rec = (rxfs_img_file *)g_sec;
        uint32_t len = 0;
        const char *data = rxfs_read(names[i], &len);
        if (!data) return false;
        for (int c = 0; names[i][c] && c < RXFS_NAME - 1; c++) rec->name[c] = names[i][c];
        rec->len = len;
        k_memcpy(rec->data, data, len);
        /* One sector at a time: virtio-blk (and some AHCI paths) get flaky
           after the installer has already written a few thousand kernel
           sectors, and a 9-sector request then times out. */
        for (uint32_t s = 0; s < REC_SECTORS; s++)
            if (!bd->write(bd, lba + s, g_sec + s * BLOCKDEV_SECTOR, 1))
                return false;
        lba += REC_SECTORS;
    }
    return true;
}

/* Does the device carry a valid RXFS image? (Safety gate: `save` refuses to
   touch a disk that was never explicitly formatted for RXos, so pointing the
   OS at a disk with a real MBR/partition table cannot clobber it.) */
bool rxfs_disk_has_image(struct blockdev *dev) {
    blockdev_t *bd = (blockdev_t *)dev;
    if (!bd || !bd->read(bd, 0, g_sec, 1)) return false;
    rxfs_img_header *h = (rxfs_img_header *)g_sec;
    return h->magic == RXFS_IMG_MAGIC && h->version == 1 &&
           h->count <= RXFS_MAX_FILES;
}

/* Write an empty, valid RXFS image header (explicit `format hda yes`). */
bool rxfs_disk_format(struct blockdev *dev) {
    blockdev_t *bd = (blockdev_t *)dev;
    if (!bd) return false;
    k_memset(g_sec, 0, BLOCKDEV_SECTOR);
    rxfs_img_header *h = (rxfs_img_header *)g_sec;
    h->magic = RXFS_IMG_MAGIC;
    h->version = 1;
    h->count = 0;
    return bd->write(bd, 0, g_sec, 1);
}

bool rxfs_load(struct blockdev *dev) {
    blockdev_t *bd = (blockdev_t *)dev;
    if (!bd) return false;

    if (!bd->read(bd, 0, g_sec, 1)) return false;
    rxfs_img_header *h = (rxfs_img_header *)g_sec;
    if (h->magic != RXFS_IMG_MAGIC || h->version != 1) return false;
    uint32_t count = h->count;
    if (count > RXFS_MAX_FILES) return false;

    rxfs_format();
    uint32_t lba = HDR_SECTORS;
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t s = 0; s < REC_SECTORS; s++)
            if (!bd->read(bd, lba + s, g_sec + s * BLOCKDEV_SECTOR, 1))
                return false;
        rxfs_img_file *rec = (rxfs_img_file *)g_sec;
        rec->name[RXFS_NAME - 1] = '\0';
        uint32_t len = rec->len > RXFS_SIZE ? RXFS_SIZE : rec->len;
        if (!rxfs_write(rec->name, (const char *)rec->data, len)) return false;
        lba += REC_SECTORS;
    }
    return true;
}
