/* virtio-blk (vda). */
#ifndef RXOS_VIRTIO_BLK_H
#define RXOS_VIRTIO_BLK_H

#include "blockdev.h"

/* Probe PCI for a virtio-blk device, bring it up per the VirtIO spec
   handshake, and register it with the block layer as "vda". Returns true
   only if the device negotiated and a capacity was read. */
bool virtio_blk_init(void);
bool virtio_blk_present(void);
blockdev_t *virtio_blk_dev(void);     /* NULL when absent */

#endif /* RXOS_VIRTIO_BLK_H */
