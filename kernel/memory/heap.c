/* First-fit heap over a fixed arena. Self-tested at boot. Growing from PMM is TODO. */
#include "heap.h"

#define HEAP_MAGIC 0x52584842u   /* "RXHB" */
#define ALIGN16(x) (((x) + 15) & ~(size_t)15)

typedef struct block {
    uint32_t      magic;
    uint32_t      free;
    size_t        size;          /* payload bytes (excluding header) */
    struct block *next;          /* address-ordered list             */
} block_t;

static uint8_t  g_arena[HEAP_ARENA_BYTES] __attribute__((aligned(16)));
static block_t *g_head;
static size_t   g_used;

void heap_init(void) {
    g_head = (block_t *)g_arena;
    g_head->magic = HEAP_MAGIC;
    g_head->free  = 1;
    g_head->size  = HEAP_ARENA_BYTES - sizeof(block_t);
    g_head->next  = NULL;
    g_used = 0;
}

/* Every request is counted, granted or not. "Nothing was leaked" and
   "nothing was asked for" are different claims, and the robotic runtime is
   required to make the second one. */
static uint64_t g_alloc_calls;
uint64_t heap_alloc_calls(void) { return g_alloc_calls; }

void *kmalloc(size_t n) {
    g_alloc_calls++;
    if (n == 0 || !g_head) return NULL;
    n = ALIGN16(n);
    for (block_t *b = g_head; b; b = b->next) {
        if (b->magic != HEAP_MAGIC) { klog_error("heap: corrupted block header"); return NULL; }
        if (!b->free || b->size < n) continue;
        /* Split if the remainder can hold a header + a minimal payload. */
        if (b->size >= n + sizeof(block_t) + 16) {
            block_t *rest = (block_t *)((uint8_t *)(b + 1) + n);
            rest->magic = HEAP_MAGIC;
            rest->free  = 1;
            rest->size  = b->size - n - sizeof(block_t);
            rest->next  = b->next;
            b->next = rest;
            b->size = n;
        }
        b->free = 0;
        g_used += b->size;
        return (void *)(b + 1);
    }
    return NULL;                 /* exhausted */
}

void kfree(void *p) {
    if (!p) return;
    block_t *b = ((block_t *)p) - 1;
    if (b->magic != HEAP_MAGIC || b->free) { klog_error("heap: bad kfree"); return; }
    b->free = 1;
    g_used -= b->size;
    /* Coalesce forward runs of free blocks (list is address-ordered). */
    for (block_t *c = g_head; c; c = c->next)
        while (c->free && c->next && c->next->free &&
               (uint8_t *)(c + 1) + c->size == (uint8_t *)c->next) {
            c->size += sizeof(block_t) + c->next->size;
            c->next  = c->next->next;
        }
}

size_t heap_total(void) { return HEAP_ARENA_BYTES; }
size_t heap_used(void)  { return g_used; }
size_t heap_free(void)  { return HEAP_ARENA_BYTES - g_used; }
int heap_block_count(void) {
    int n = 0;
    for (block_t *b = g_head; b; b = b->next) n++;
    return n;
}

bool heap_selftest(void) {
    void *a = kmalloc(100), *b = kmalloc(2000), *c = kmalloc(40);
    if (!a || !b || !c) return false;
    k_memset(a, 0xAA, 100); k_memset(b, 0xBB, 2000); k_memset(c, 0xCC, 40);
    if (((uint8_t *)a)[99] != 0xAA || ((uint8_t *)b)[1999] != 0xBB) return false;
    kfree(b);
    void *d = kmalloc(1500);               /* must reuse b's hole   */
    if (d == NULL) return false;
    kfree(a); kfree(c); kfree(d);
    if (heap_used() != 0) return false;    /* everything returned   */
    void *big = kmalloc(HEAP_ARENA_BYTES); /* must fail, not crash  */
    if (big != NULL) return false;
    return heap_block_count() == 1;        /* fully coalesced again */
}
