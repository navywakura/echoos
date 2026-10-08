/* Device registry for the `devices` command. */
#include "device_manager.h"

static device_info_t g_devices[DEVMGR_MAX];
static int g_ndev;

void devmgr_register(const char *name, const char *class_, const char *status) {
    if (g_ndev >= DEVMGR_MAX) return;
    g_devices[g_ndev].name   = name;
    g_devices[g_ndev].class_ = class_;
    g_devices[g_ndev].status = status;
    g_ndev++;
}

int devmgr_count(void) { return g_ndev; }

const device_info_t *devmgr_at(int i) {
    return (i >= 0 && i < g_ndev) ? &g_devices[i] : NULL;
}
