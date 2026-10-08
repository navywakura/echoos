/* PCI helpers. */
#ifndef RXOS_PCI_H
#define RXOS_PCI_H

#include "rogex.h"

typedef struct {
    uint8_t  bus, dev, fn;
    uint16_t vendor, device;
} pci_addr_t;

/* Config-space accessors (offset must be dword-aligned for the 32-bit ops). */
uint32_t pci_read32(pci_addr_t a, uint8_t off);
uint16_t pci_read16(pci_addr_t a, uint8_t off);
uint8_t  pci_read8(pci_addr_t a, uint8_t off);
void     pci_write32(pci_addr_t a, uint8_t off, uint32_t v);
void     pci_write16(pci_addr_t a, uint8_t off, uint16_t v);

/* Scan every programmed PCI bus (follows bridges) for vendor:device. */
bool pci_find(uint16_t vendor, uint16_t device, pci_addr_t *out);

/* Scan bus 0 by class/subclass/prog-if (pass progif < 0 to ignore it). Use
   this for standardised controllers — AHCI is 01:06:01 on every vendor's
   silicon, so matching the class works on hardware nobody tabulated. */
bool pci_find_class(uint8_t cls, uint8_t subcls, int progif, pci_addr_t *out);

/* Physical base of a memory BAR (handles 64-bit BARs by combining the
   following dword per PCI spec). Returns 0 for I/O BARs / unassigned. */
uint64_t pci_bar_mem_base(pci_addr_t a, int bar);

/* I/O-port BAR (bit0 set). Returns 0 if the BAR is memory or empty. */
uint16_t pci_bar_io_base(pci_addr_t a, int bar);

/* Command register: I/O space + bus master (RTL8139 and similar). */
void pci_enable_io_and_dma(pci_addr_t a);

/* Walk every function on every programmed bus (PCI-PCI bridges included).
   Callback returns false to stop. Returns how many devices were visited. */
typedef bool (*pci_visit_fn)(pci_addr_t a, uint8_t cls, uint8_t subcls, void *user);
int pci_foreach(pci_visit_fn fn, void *user);

/* Command register: enable memory-space decoding and bus mastering (DMA).
   Why: without bit1 the BARs don't decode; without bit2 the device cannot
   read/write our virtqueue and data buffers. */
void pci_enable_mem_and_dma(pci_addr_t a);

/* First capability offset with the given cap id (0 = none). `after` lets
   callers iterate: pass the previous result to get the next match. */
uint8_t pci_find_cap(pci_addr_t a, uint8_t cap_id, uint8_t after);

#endif /* RXOS_PCI_H */
