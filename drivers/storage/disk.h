/* Disk facade. */
#ifndef RXOS_DISK_H
#define RXOS_DISK_H

#include "blockdev.h"

bool        disk_init(void);        /* probe + register a persistent disk    */
bool        disk_present(void);
blockdev_t *disk_dev(void);         /* NULL when amnesic                     */
const char *disk_name(void);        /* "sda1" | "vda" | "hda" | "none"       */
const char *disk_backend(void);     /* human string for status/devices       */
bool        disk_partitioned(void); /* RXFS is in a partition, not sector 0  */

#endif /* RXOS_DISK_H */
