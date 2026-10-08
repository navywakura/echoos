/* Block device registry — ram0, hda, vda, whatever shows up. */
#include "blockdev.h"

static blockdev_t *g_devs[BLOCKDEV_MAX];
static int g_ndevs;

static bool eqs(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

bool blockdev_register(blockdev_t *dev) {
    if (g_ndevs >= BLOCKDEV_MAX || !dev || !dev->read || !dev->write) return false;
    g_devs[g_ndevs++] = dev;
    return true;
}

blockdev_t *blockdev_get(const char *name) {
    for (int i = 0; i < g_ndevs; i++)
        if (eqs(g_devs[i]->name, name)) return g_devs[i];
    return NULL;
}

int blockdev_count(void) { return g_ndevs; }
blockdev_t *blockdev_at(int i) { return (i >= 0 && i < g_ndevs) ? g_devs[i] : NULL; }
