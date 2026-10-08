/*
 * Heap-0 Static Memory Layout — EchOS 1.0 / 2.0 Universal shared kernel
 * Pre-allocated static buffers in compilation time (BSS / fixed regions).
 * Guarantees zero fragmentation, deterministic O(1) allocation time,
 * and no runtime heap exhaustions during critical I/O operations.
 */
#ifndef RXOS_HEAP0_H
#define RXOS_HEAP0_H

#include "rogex.h"

#define HEAP0_WSP_POOL_SIZE    (64 * 1024)   /* 64 KiB RogexWSP message pool */
#define HEAP0_DEVMGR_MAX       64            /* 64 static device entries */
#define HEAP0_ICON_CACHE_MAX   16            /* 16 static icons */

typedef struct {
    const char *name;
    uint64_t    base_addr;
    uint32_t    size_bytes;
    const char *purpose;
} heap0_region_t;

void     heap0_init(void);
bool     heap0_ready(void);   /* WSP pool bound in BSS; other rows may be size-only */
size_t   heap0_total_bytes(void);
size_t   heap0_reserved_bytes(void);
uint32_t heap0_region_count(void);
const heap0_region_t *heap0_get_region(uint32_t idx);
void    *heap0_get_wsp_pool(size_t *out_size);

#endif /* RXOS_HEAP0_H */
