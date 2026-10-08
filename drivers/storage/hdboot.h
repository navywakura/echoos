/* Write a BIOS-bootable RXos disk: MBR + stage2 + kernel payload + RXFS. */
#ifndef RXOS_HDBOOT_H
#define RXOS_HDBOOT_H

#include "rogex.h"
#include "blockdev.h"

#define RXBOOT_MAGIC        0x54425852u   /* "RXBT" */
#define RXBOOT_VERSION      1u
#define RXBOOT_STAGE2_LBA   1u
#define RXBOOT_STAGE2_SECTS 16u
#define RXBOOT_HDR_LBA      17u
#define RXBOOT_KERNEL_LBA   18u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t kernel_lba;
    uint32_t kernel_sectors;
    uint32_t flags;
    uint32_t entry;           /* physical address of _start */
} rxboot_hdr;

/* Kernel image the LIVE ISO carried as a Multiboot2 module (flat binary). */
const uint8_t *hdboot_payload(uint32_t *len);

/* Write stage1/stage2 + RXBT header + kernel into the pre-partition gap.
   Partition table in LBA 0 is preserved. False if there is no payload or I/O
   failed. */
bool hdboot_write(blockdev_t *disk);

#endif
