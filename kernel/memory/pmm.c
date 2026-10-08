/* Physical frame allocator over the firmware-independent boot handoff. */
#include "rogex.h"
#include "handoff.h"

/* Every frame this allocator hands out is used through its identity mapping,
   so it may only allocate from memory boot.asm actually mapped: the first
   4 GiB. That bound is not cosmetic. QEMU with 512 MiB has exactly one usable
   region and it fits; a real 8 GiB laptop reports its *largest* usable region
   above 4 GiB, and the old "pick the biggest region" rule handed out frames at
   0x1_0000_0000 that nothing had mapped — the first kmalloc() faulted and the
   machine never reached the shell. Pick the largest region that lies inside
   the mapped window instead, and clip it to that window. */
#define PMM_MAPPED_LIMIT (4ULL << 30)

/* End of the loaded kernel image, from the link script. The old code assumed
   the image fit under 2 MiB; it does not once the Rust post-quantum core and
   the embedded apps are linked in, so the floor is computed, not guessed. */
extern uint8_t _kernel_end[];

/* The firmware memory map, kept verbatim. Summaries hide the one thing that
   actually explains an odd total: what the firmware reported, region by
   region. Storing it costs 16 entries of BSS and turns "the number looks
   wrong" into a table anyone can read off the screen. */
/* 32, not 16: the HP 15-ac195nl reports more than 16 and the table silently
   stopped there — it listed 2496 MiB of available RAM while the totals said
   8112 MiB, with the difference in entries that were never stored. A capped
   table that does not say it is capped is worse than no table. */
#define PMM_MAX_REGIONS 32
static pmm_region_t g_regions[PMM_MAX_REGIONS];
static int          g_nregions;
static int          g_nregions_seen;   /* what the firmware actually reported */

/* Frame allocator state. */
static uint64_t alloc_next  = 0;   /* next free physical frame (page-aligned) */
static uint64_t alloc_limit = 0;   /* one past the end of the usable region   */
static uint64_t total_bytes = 0;   /* all usable RAM the firmware reports     */
static uint64_t usable_bytes = 0;  /* the part of it we can actually address  */
static uint64_t region_base  = 0;  /* start of the region we allocate from     */

static inline uint64_t align_up(uint64_t x, uint64_t a) {
    return (x + a - 1) & ~(a - 1);
}

void pmm_init(const struct boot_handoff *bh) {
    uint64_t floor;
    uint64_t best_base = 0, best_len = 0;
    uint32_t i;

    if (!bh || !bh->valid) return;

    /* Nothing may be allocated on top of the kernel image or of the loader's
       information block — the handoff still points into it. */
    floor = (uint64_t)(uintptr_t)_kernel_end;
    if (bh->info_end > floor) floor = bh->info_end;
    /* Relocate the LIVE payload module so the bump allocator cannot hand
       those frames out from under the installer. */
    boot_take_payload(&floor);
    if (floor < 2 * 1024 * 1024) floor = 2 * 1024 * 1024;
    floor = align_up(floor, PAGE_SIZE);

    g_nregions_seen = (int)bh->mem_reported;
    for (i = 0; i < bh->mem_count; i++) {
        const boot_mem_range_t *r = &bh->mem[i];
        uint64_t lo, hi;

        /* Record every region, usable or not: a reserved block in the middle
           of RAM is exactly what makes a total look wrong. */
        if (g_nregions < PMM_MAX_REGIONS) {
            g_regions[g_nregions].base = r->base;
            g_regions[g_nregions].len  = r->length;
            g_regions[g_nregions].type = r->raw_type;
            g_nregions++;
        }

        if (r->type != BOOT_MEM_USABLE) continue;
        total_bytes += r->length;

        /* Clip the region to [floor, 4 GiB) before judging its size: the
           region containing the kernel is large but mostly spoken for, and a
           region straddling 4 GiB is only usable up to it. */
        lo = r->base;
        hi = r->base + r->length;
        if (lo < floor) lo = floor;
        if (hi > PMM_MAPPED_LIMIT) hi = PMM_MAPPED_LIMIT;
        if (hi <= lo) continue;

        usable_bytes += hi - lo;
        if (hi - lo > best_len) { best_len = hi - lo; best_base = lo; }
    }

    alloc_next  = align_up(best_base, PAGE_SIZE);
    alloc_limit = best_base + best_len;
    region_base = alloc_next;
    if (best_len == 0 || alloc_next >= alloc_limit) { alloc_next = alloc_limit = 0; }
}

uint64_t pmm_alloc_frame(void) {
    if (alloc_next == 0 || alloc_next + PAGE_SIZE > alloc_limit) return 0;
    uint64_t f = alloc_next;
    alloc_next += PAGE_SIZE;
    /* Always safe now: pmm_init refuses to select memory outside the map. */
    k_memset((void *)(uintptr_t)f, 0, PAGE_SIZE);
    return f;
}

/* Contiguous run of frames. The allocator is a bump pointer, so consecutive
   frames are already adjacent — this just reserves the whole run in one step
   so nothing can interleave. Needed by the framebuffer backbuffer, which is
   several megabytes and must be one flat array. Returns 0 if it does not fit. */
uint64_t pmm_alloc_contig(uint32_t pages) {
    if (alloc_next == 0 || pages == 0) return 0;
    uint64_t bytes = (uint64_t)pages * PAGE_SIZE;
    if (alloc_next + bytes > alloc_limit) return 0;
    uint64_t base = alloc_next;
    alloc_next += bytes;
    k_memset((void *)(uintptr_t)base, 0, bytes);
    return base;
}

uint64_t pmm_total_bytes(void)  { return total_bytes; }
uint64_t pmm_usable_bytes(void) { return usable_bytes; }
uint64_t pmm_free_bytes(void)   { return alloc_limit > alloc_next ? alloc_limit - alloc_next : 0; }

/* Frames actually handed out. Reporting "free / total" invited the reading
   that the difference is in use, which on this machine it is not: the gap is
   RAM above the 4 GiB identity map, the PCI hole, and the regions the bump
   allocator simply did not pick. Actual consumption is this number, and on a
   fresh boot it is a handful of megabytes. */
int pmm_region_count(void)      { return g_nregions; }
int pmm_region_total_seen(void) { return g_nregions_seen; }

const pmm_region_t *pmm_region(int i) {
    return (i >= 0 && i < g_nregions) ? &g_regions[i] : NULL;
}

uint64_t pmm_used_bytes(void) {
    return (alloc_next && alloc_next >= region_base) ? alloc_next - region_base : 0;
}

/* Pattern-fill unused frames and restore them to zero. The bump pointer is
   not advanced: the installer/shell still own those frames afterwards. */
bool pmm_memtest(uint64_t max_bytes, uint64_t *out_tested, uint64_t *out_errors) {
    uint64_t lo, hi, tested = 0, errors = 0, left;
    if (out_tested) *out_tested = 0;
    if (out_errors) *out_errors = 0;
    if (!alloc_next || alloc_next >= alloc_limit) return false;
    lo = alloc_next;
    hi = alloc_limit;
    left = hi - lo;
    if (max_bytes && max_bytes < left) left = max_bytes;
    hi = lo + (left & ~(PAGE_SIZE - 1));
    for (uint64_t addr = lo; addr < hi; addr += PAGE_SIZE) {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)addr;
        uint32_t i;
        /* 0xAA */
        for (i = 0; i < PAGE_SIZE; i++) p[i] = 0xAA;
        for (i = 0; i < PAGE_SIZE; i++) if (p[i] != 0xAA) errors++;
        /* 0x55 */
        for (i = 0; i < PAGE_SIZE; i++) p[i] = 0x55;
        for (i = 0; i < PAGE_SIZE; i++) if (p[i] != 0x55) errors++;
        /* address-in-page walking byte */
        for (i = 0; i < PAGE_SIZE; i++) p[i] = (uint8_t)(i ^ (uint8_t)(addr >> 12));
        for (i = 0; i < PAGE_SIZE; i++)
            if (p[i] != (uint8_t)(i ^ (uint8_t)(addr >> 12))) errors++;
        /* restore the same zero fill alloc_frame() would write */
        for (i = 0; i < PAGE_SIZE; i++) p[i] = 0;
        tested += PAGE_SIZE;
    }
    if (out_tested) *out_tested = tested;
    if (out_errors) *out_errors = errors;
    return true;
}
