#include "hdboot.h"
#include "partition.h"
#include "handoff.h"

extern const uint8_t hdboot_stage1[];
extern const uint8_t hdboot_stage2[];
extern const uint32_t hdboot_stage1_len;
extern const uint32_t hdboot_stage2_len;

const uint8_t *hdboot_payload(uint32_t *len) {
    return boot_payload(len);
}

static bool write_secs(blockdev_t *d, uint32_t lba, const uint8_t *p, uint32_t n) {
    static uint8_t sec[BLOCKDEV_SECTOR];
    while (n) {
        uint32_t chunk = n > BLOCKDEV_SECTOR ? BLOCKDEV_SECTOR : n;
        k_memset(sec, 0, BLOCKDEV_SECTOR);
        k_memcpy(sec, p, chunk);
        if (!d->write(d, lba, sec, 1)) return false;
        p += chunk;
        n -= chunk;
        lba++;
    }
    return true;
}

bool hdboot_write(blockdev_t *disk) {
    if (!disk) return false;
    uint32_t plen = 0;
    const uint8_t *pay = boot_payload(&plen);
    if (!pay || plen < 4096) return false;

    uint32_t ksects = (plen + BLOCKDEV_SECTOR - 1) / BLOCKDEV_SECTOR;
    if (RXBOOT_KERNEL_LBA + ksects >= PART_FIRST_LBA) return false;

    /* Stage2 into LBA 1..16 (pad / clip to 16 sectors). */
    uint32_t s2 = hdboot_stage2_len;
    if (s2 > RXBOOT_STAGE2_SECTS * BLOCKDEV_SECTOR)
        s2 = RXBOOT_STAGE2_SECTS * BLOCKDEV_SECTOR;
    if (!write_secs(disk, RXBOOT_STAGE2_LBA, hdboot_stage2, s2)) return false;
    /* Zero any unused stage2 tail so a leftover ISO header cannot confuse us. */
    static uint8_t zero[BLOCKDEV_SECTOR];
    k_memset(zero, 0, sizeof(zero));
    uint32_t used = (s2 + BLOCKDEV_SECTOR - 1) / BLOCKDEV_SECTOR;
    for (uint32_t i = used; i < RXBOOT_STAGE2_SECTS; i++)
        if (!disk->write(disk, RXBOOT_STAGE2_LBA + i, zero, 1)) return false;

    rxboot_hdr hdr;
    k_memset(&hdr, 0, sizeof(hdr));
    hdr.magic = RXBOOT_MAGIC;
    hdr.version = RXBOOT_VERSION;
    hdr.kernel_lba = RXBOOT_KERNEL_LBA;
    hdr.kernel_sectors = ksects;
    hdr.flags = 0;
    {
        extern void _start(void);
        hdr.entry = (uint32_t)(uintptr_t)_start;
    }
    if (!write_secs(disk, RXBOOT_HDR_LBA, (const uint8_t *)&hdr, sizeof(hdr)))
        return false;

    if (!write_secs(disk, RXBOOT_KERNEL_LBA, pay, plen)) return false;

    /* Splice stage1 into the existing MBR, keep the partition table. */
    static uint8_t mbr[BLOCKDEV_SECTOR];
    if (!disk->read(disk, 0, mbr, 1)) return false;
    uint32_t n = hdboot_stage1_len;
    if (n > 446) n = 446;
    k_memcpy(mbr, hdboot_stage1, n);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    return disk->write(disk, 0, mbr, 1);
}
