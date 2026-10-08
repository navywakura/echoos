/* Ramdisk. */
#ifndef RXOS_RAMDISK_H
#define RXOS_RAMDISK_H

#include "blockdev.h"

#define RAMDISK_BYTES (512 * 1024)

bool ramdisk_init(void);           /* registers "ram0" with the block layer */
blockdev_t *ramdisk_dev(void);

#endif /* RXOS_RAMDISK_H */
