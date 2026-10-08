/* Disk facade: NVMe, virtio-blk, AHCI, ATA. Callers should not care which. */
#include "disk.h"
#include "nvme.h"
#include "virtio_blk.h"
#include "ahci.h"
#include "ata.h"
#include "partition.h"

static blockdev_t *g_disk;
static const char *g_backend = "none";
static bool        g_partitioned;

/* Prefer an EchOS partition over the raw disk. A machine that ran the install
   wizard has RXFS inside a partition, which leaves its partition table intact;
   a scratch image handed to QEMU still has RXFS at sector 0. Checking for the
   partition first means both keep working without the user choosing. */
static blockdev_t *pick(blockdev_t *whole) {
    blockdev_t *p = part_find_rxos(whole);
    g_partitioned = p != NULL;
    return p ? p : whole;
}

bool disk_init(void) {
    /* NVMe first: on a machine that has one it is the real disk, and the
       legacy paths would not see it at all. */
    if (nvme_init()) {
        g_disk = pick(nvme_dev());
        g_backend = g_partitioned ? "nvme0n1p1 (NVMe, EchOS partition)"
                                  : "nvme0n1 (NVMe, whole namespace)";
        return true;
    }
    if (virtio_blk_init()) {
        g_disk = pick(virtio_blk_dev());
        g_backend = g_partitioned ? "vda1 (virtio-blk, EchOS partition)"
                                  : "vda (virtio-blk, whole disk)";
        return true;
    }
    /* AHCI before ATA PIO: a controller in AHCI mode does not answer on the
       legacy IDE ports at all, and where both work AHCI does DMA instead of
       moving every sector through the CPU 16 bits at a time. */
    if (ahci_init()) {
        g_disk = pick(ahci_dev());
        g_backend = g_partitioned ? "sda1 (AHCI SATA, EchOS partition)"
                                  : "sda (AHCI SATA, whole disk)";
        return true;
    }
    if (ata_init()) {
        g_disk = pick(ata_dev());
        g_backend = g_partitioned ? "hda1 (ATA PIO, EchOS partition)"
                                  : "hda (ATA PIO, whole disk)";
        return true;
    }
    g_disk = NULL;
    g_backend = "none (amnesic; attach a disk: make run-disk)";
    return false;
}

bool        disk_present(void) { return g_disk != NULL; }
blockdev_t *disk_dev(void)     { return g_disk; }
const char *disk_name(void)    { return g_disk ? g_disk->name : "none"; }
const char *disk_backend(void) { return g_backend; }
bool        disk_partitioned(void) { return g_partitioned; }
