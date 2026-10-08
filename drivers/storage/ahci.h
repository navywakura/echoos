/* AHCI block backend. */
#ifndef RXOS_AHCI_H
#define RXOS_AHCI_H

#include "rogex.h"
#include "blockdev.h"

/* Probe the PCI bus for an AHCI controller, bring up the first port with a
   SATA disk attached, and register it as "sda". False when there is no AHCI
   controller, no disk on it, or the controller refuses to start. */
bool        ahci_init(void);
bool        ahci_present(void);
blockdev_t *ahci_dev(void);
const char *ahci_model(void);     /* IDENTIFY model string, or "-"        */

#endif /* RXOS_AHCI_H */
