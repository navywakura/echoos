/* In-OS install / persistence wizard. */
#ifndef RXOS_INSTALL_H
#define RXOS_INSTALL_H

#include "rogex.h"

/* `install`                       -> list disks and what they hold
   `install <dev>`                 -> inspect the target, print the phrase
   `install <dev> ERASE-<dev>`     -> format + save (destructive)            */
void install_wizard(const char *dev, const char *confirm);

/* Used by the LIVE GUI wizard. Writes MBR + bootloader + kernel + RXFS.
   On failure writes a short reason into err. */
struct blockdev;
bool install_to_disk(struct blockdev *d, char *err, uint32_t errmax);

#endif /* RXOS_INSTALL_H */
