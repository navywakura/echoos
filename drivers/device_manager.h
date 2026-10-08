/* Device manager. */
#ifndef RXOS_DEVICE_MANAGER_H
#define RXOS_DEVICE_MANAGER_H

#include "rogex.h"

#define DEVMGR_MAX 32

typedef struct {
    const char *name;              /* "framebuffer", "keyboard", ...        */
    const char *class_;            /* "video", "input", "timer", ...        */
    const char *status;            /* honest: "ok (polled)", "STUB", ...    */
} device_info_t;

void devmgr_register(const char *name, const char *class_, const char *status);
int  devmgr_count(void);
const device_info_t *devmgr_at(int i);

#endif /* RXOS_DEVICE_MANAGER_H */
