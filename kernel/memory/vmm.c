/* 4-level page walker. Used for LFB / MMIO above the boot identity map. */
#include "rogex.h"
#include "arch.h"

#define PT_INDEX(v, lvl) (((v) >> (12 + 9 * (lvl))) & 0x1ff)

static inline uint64_t read_cr3(void) {
    return arch_mmu_root();
}

/* Table frames are inside the identity-mapped first 1 GiB, so a physical
   address doubles as a valid pointer here. */
static uint64_t *table_ptr(uint64_t phys) { return (uint64_t *)(uintptr_t)(phys & ~0xfffULL); }

static uint64_t *next_table(uint64_t *table, size_t idx, bool create) {
    if (!(table[idx] & PTE_PRESENT)) {
        if (!create) return NULL;
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return NULL;
        table[idx] = frame | PTE_PRESENT | PTE_WRITABLE;
    }
    return table_ptr(table[idx]);
}

bool vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *p4 = table_ptr(read_cr3());
    uint64_t *p3 = next_table(p4, PT_INDEX(virt, 3), true);  if (!p3) return false;
    uint64_t *p2 = next_table(p3, PT_INDEX(virt, 2), true);  if (!p2) return false;

    /* If P2 already holds a 2 MiB huge page for this range (as the boot stub
       set up for the first 1 GiB), the 4 KiB request is already satisfied. */
    if (p2[PT_INDEX(virt, 1)] & (1ULL << 7)) return true;    /* PS bit */

    uint64_t *p1 = next_table(p2, PT_INDEX(virt, 1), true);  if (!p1) return false;
    p1[PT_INDEX(virt, 0)] = (phys & ~0xfffULL) | (flags | PTE_PRESENT);
    arch_mmu_flush_page(virt);
    return true;
}
