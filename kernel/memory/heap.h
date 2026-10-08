/* kmalloc / kfree. */
#ifndef RXOS_HEAP_H
#define RXOS_HEAP_H

#include "rogex.h"

#define HEAP_ARENA_BYTES (512 * 1024)

void   heap_init(void);
void  *kmalloc(size_t n);            /* NULL on exhaustion; 16-byte aligned */
void   kfree(void *p);               /* NULL is a no-op                     */

/* Introspection for the `mem` shell command. */
size_t heap_total(void);
size_t heap_used(void);
/* Calls to kmalloc since boot, granted or refused. */
uint64_t heap_alloc_calls(void);
size_t heap_free(void);
int    heap_block_count(void);

/* Boot self-test: alloc/free/coalesce patterns. True = PASS. */
bool   heap_selftest(void);

#endif /* RXOS_HEAP_H */
