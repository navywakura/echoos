/* Block device abstraction. */
#ifndef RXOS_BLOCKDEV_H
#define RXOS_BLOCKDEV_H

#include "rogex.h"

#define BLOCKDEV_SECTOR 512
#define BLOCKDEV_MAX    4

typedef struct blockdev {
    const char *name;              /* "ram0", ...                          */
    uint32_t    sectors;           /* device size in 512-byte sectors      */
    bool (*read)(struct blockdev *dev, uint32_t lba, void *buf, uint32_t count);
    bool (*write)(struct blockdev *dev, uint32_t lba, const void *buf, uint32_t count);
    void       *ctx;
} blockdev_t;

bool        blockdev_register(blockdev_t *dev);
blockdev_t *blockdev_get(const char *name);
int         blockdev_count(void);
blockdev_t *blockdev_at(int i);

#endif /* RXOS_BLOCKDEV_H */
