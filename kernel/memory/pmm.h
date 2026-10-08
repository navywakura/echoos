/* Physical memory manager. */
#ifndef RXOS_PMM_H
#define RXOS_PMM_H

#include "rogex.h"

struct boot_handoff;
void     pmm_init(const struct boot_handoff *bh);
uint64_t pmm_alloc_frame(void);              /* -> physical addr, or 0 */
uint64_t pmm_total_bytes(void);
uint64_t pmm_free_bytes(void);

/* Walk unused frames in the bump region (does not consume them).
   max_bytes == 0 means "all remaining free frames". Returns false if the
   region is empty; *errors is the number of mismatching bytes. */
bool pmm_memtest(uint64_t max_bytes, uint64_t *out_tested, uint64_t *out_errors);

#endif /* RXOS_PMM_H */
