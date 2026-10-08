/* devfs. */
#ifndef RXOS_DEVFS_H
#define RXOS_DEVFS_H

#include "rogex.h"

bool        devfs_is_node(const char *path);
/* Generate this node's contents NOW. False when the path is not a node. */
bool        devfs_read(const char *path, char *out, size_t max, size_t *written);

int         devfs_count(void);
const char *devfs_name(int i);
const char *devfs_describe(int i);

#endif /* RXOS_DEVFS_H */
