/* OS update channel: check / download / apply file patches, then reboot.
   Kernel/ISO replacements cannot rewrite a live ISO9660 USB — those are
   advertised as kind=iso. See docs/updates.md. */
#ifndef RXOS_UPDATE_H
#define RXOS_UPDATE_H

#include "rogex.h"

#define UPDATE_CHANNEL_HOST "www.rogexlaboratories.com"
#define UPDATE_CHANNEL_PATH "/rx-os/updates"
#define UPDATE_CHANNEL_URL  "https://www.rogexlaboratories.com/rx-os/updates"

void update_init(void);          /* seed /etc/os-release + update dirs      */
void update_boot_apply(void);    /* apply staged patch + wallpaper on boot  */

/* Runtime stamp: /etc/os-release VERSION= if present, else compiled ISO. */
void update_runtime_version(char *out, size_t max);

/* `update` / `wallpaper` / `rx update …` — writes human text into out. */
void update_cmd(const char *verb, const char *sub, const char *arg,
                char *out, size_t outmax);

#endif /* RXOS_UPDATE_H */
