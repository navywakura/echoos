/* ATA PIO block device (hda). */
#ifndef RXOS_ATA_H
#define RXOS_ATA_H

#include "blockdev.h"

/* Probe primary-master, and if a disk answers IDENTIFY, register it with the
   block layer as "hda". Returns true if the disk is present + registered. */
bool ata_init(void);
bool ata_present(void);
blockdev_t *ata_dev(void);            /* NULL when absent */

#endif /* RXOS_ATA_H */
