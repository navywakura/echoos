/*
 * BrainChip Akida AKD1000 — PCI detection only.
 *
 * PCI: vendor 0x1E7C  device 0xBCA1 (lspci: "Co-processor: Device 1e7c:bca1").
 * This driver reports whether the card is on the bus and where firmware put
 * its BAR0. That is all it can honestly do: there is no board to test
 * against, no BrainChip runtime in this tree, and no published register
 * contract we have verified. An earlier version declared MAGIC / POWER_MW /
 * TEMP_C / LAT_NS offsets and read them; those numbers were invented, so a
 * "power" reading from them would have been a fabricated measurement. They
 * are gone rather than shipped behind a flag.
 *
 * Without a board: probe returns false and the software LIF stays the
 * backend. Offload is not implemented for either case.
 */
#ifndef ECHOS_AKIDA_H
#define ECHOS_AKIDA_H

#include "rogex.h"

#define AKIDA_PCI_VENDOR   0x1E7Cu
#define AKIDA_PCI_DEVICE   0xBCA1u   /* AKD1000 Neural Network Coprocessor */

typedef struct {
    bool     present;            /* the card answered on the PCI bus       */
    uint16_t vendor, device;
    uint8_t  bus, dev, fn;
    uint64_t bar0_phys;          /* as programmed by firmware, 0 if empty  */
    uint32_t bar0_size;          /* 0 until a datasheet says otherwise     */
} akida_info_t;

void                akida_init(void);
bool                akida_present(void);
const akida_info_t *akida_info(void);
const char         *akida_status(void);

#endif
