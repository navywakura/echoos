/* Virtual memory helpers. */
#ifndef RXOS_VMM_H
#define RXOS_VMM_H

#include "rogex.h"

/* Map one 4 KiB page virt->phys. flags: PTE_* bits from rogex.h. */
bool vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

#endif /* RXOS_VMM_H */
