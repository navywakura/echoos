/*
 * MMU bring-up: an identity map, 4 KiB granule.
 *
 * This is not optional housekeeping. With the MMU off, AArch64 treats every
 * data access as Device-nGnRnE, and Device memory does not allow unaligned
 * access — so the first packed struct or byte-wise copy in ordinary C takes
 * an alignment fault, before any vector table exists to report it. That is
 * exactly how this port failed its first boot: the assembly ran, the C did
 * not, and there was no output to say why.
 *
 * Level 1 blocks of 1 GiB are enough for the identity map: 0-1 GiB is where
 * QEMU virt puts its devices (PL011, GIC, virtio-mmio), 1-4 GiB is RAM.
 * Page-granular mapping for later MMIO uses the deeper levels.
 */
#include "rogex.h"

#define MAIR_DEVICE_nGnRnE 0x00ull      /* attr index 0                     */
#define MAIR_NORMAL_WB     0xFFull      /* attr index 1: write-back, RA/WA  */

#define PTE_VALID     (1ull << 0)
#define PTE_BLOCK     (0ull << 1)       /* block at L1/L2                   */
#define PTE_TABLE     (1ull << 1)
#define PTE_ATTR(i)   ((uint64_t)(i) << 2)
#define PTE_NS        (1ull << 5)
#define PTE_AP_RW     (0ull << 6)
#define PTE_SH_INNER  (3ull << 8)
#define PTE_AF        (1ull << 10)      /* access flag: without it, faults  */
#define PTE_PXN       (1ull << 53)
#define PTE_UXN       (1ull << 54)

/* 512 entries x 1 GiB = 512 GiB of virtual address space, which is what a
   39-bit VA gives. Only the first four are filled. */
static uint64_t g_l1[512] __attribute__((aligned(4096)));

void aarch64_mmu_init(void) {
    uint64_t mair, tcr, sctlr;
    uint32_t i;

    for (i = 0; i < 512; i++) g_l1[i] = 0;

    /* 0-1 GiB: devices. Execute-never, because nothing runs from MMIO. */
    g_l1[0] = (0ull) | PTE_VALID | PTE_BLOCK | PTE_ATTR(0) | PTE_AF |
              PTE_AP_RW | PTE_PXN | PTE_UXN;
    /* 1-4 GiB: RAM, normal memory, inner shareable. Unaligned access is
       allowed here, which is the whole point. */
    for (i = 1; i < 4; i++)
        g_l1[i] = ((uint64_t)i << 30) | PTE_VALID | PTE_BLOCK | PTE_ATTR(1) |
                  PTE_AF | PTE_AP_RW | PTE_SH_INNER;

    mair = MAIR_DEVICE_nGnRnE | (MAIR_NORMAL_WB << 8);
    __asm__ volatile ("msr mair_el1, %0" :: "r"(mair));

    /* T0SZ=25 -> 39-bit VA. TG0=0 -> 4 KiB granule. Inner/outer write-back
       walks, inner shareable. TTBR1 disabled: one address space for now. */
    tcr = (25ull << 0)      /* T0SZ                                        */
        | (0ull  << 14)     /* TG0 = 4 KiB                                 */
        | (3ull  << 12)     /* SH0 = inner shareable                       */
        | (1ull  << 10)     /* ORGN0 = write-back                          */
        | (1ull  << 8)      /* IRGN0 = write-back                          */
        | (1ull  << 23)     /* EPD1: no TTBR1 walks                        */
        | (2ull  << 32);    /* IPS = 40-bit physical                       */
    __asm__ volatile ("msr tcr_el1, %0" :: "r"(tcr));
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r"((uint64_t)(uintptr_t)g_l1));

    __asm__ volatile ("dsb ish; isb");
    __asm__ volatile ("tlbi vmalle1; dsb ish; isb");

    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |=  (1ull << 0);      /* M: MMU on                                */
    sctlr |=  (1ull << 2);      /* C: data cache                            */
    sctlr |=  (1ull << 12);     /* I: instruction cache                     */
    sctlr &= ~(1ull << 1);      /* A: no strict alignment checking          */
    __asm__ volatile ("msr sctlr_el1, %0; isb" :: "r"(sctlr));
}
