/* GICv2 on QEMU virt: distributor at 0x08000000, CPU interface at 0x08010000.
 *
 * Two more hardcoded addresses, and the same caveat as the PL011: a device
 * tree walk is the right answer and is not written yet. They are named here
 * rather than spread through the code so that the day the DTB parser lands,
 * there is one place to change.
 */
#include "gic.h"

#define GICD_BASE 0x08000000UL
#define GICC_BASE 0x08010000UL

#define GICD_CTLR       0x000
#define GICD_ISENABLER  0x100
#define GICD_ICENABLER  0x180
#define GICD_IPRIORITYR 0x400
#define GICD_ITARGETSR  0x800
#define GICD_ICFGR      0xC00

#define GICC_CTLR 0x00
#define GICC_PMR  0x04
#define GICC_IAR  0x0C
#define GICC_EOIR 0x10

#define INTID_MAX 128

static void (*g_handler[INTID_MAX])(void);
static uint32_t g_spurious;
static uint64_t g_taken;

static volatile uint32_t *gicd(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICD_BASE + off);
}
static volatile uint32_t *gicc(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICC_BASE + off);
}

extern void aarch64_vectors(void);      /* vectors.S                        */

bool gic_init(void) {
    uint32_t i;
    /* Vectors first: an interrupt arriving before VBAR is set goes nowhere
       useful, and "nowhere useful" on this architecture is a reset loop. */
    __asm__ volatile ("msr vbar_el1, %0; isb" :: "r"(&aarch64_vectors));

    *gicd(GICD_CTLR) = 0;
    for (i = 0; i < INTID_MAX; i += 4) *gicd(GICD_IPRIORITYR + i) = 0xA0A0A0A0u;
    for (i = 32; i < INTID_MAX; i += 4) *gicd(GICD_ITARGETSR + i) = 0x01010101u;
    for (i = 0; i < INTID_MAX; i += 32) *gicd(GICD_ICENABLER + i / 8) = 0xFFFFFFFFu;
    *gicd(GICD_CTLR) = 1;               /* distributor on                   */

    *gicc(GICC_PMR) = 0xF0;             /* accept anything below this        */
    *gicc(GICC_CTLR) = 1;               /* CPU interface on                  */
    return true;
}

bool gic_register(int intid, void (*handler)(void)) {
    if (intid < 0 || intid >= INTID_MAX) return false;
    g_handler[intid] = handler;
    return true;
}

void gic_enable(int intid) {
    if (intid < 0 || intid >= INTID_MAX) return;
    *gicd(GICD_ISENABLER + (uint32_t)(intid / 32) * 4) = 1u << (intid % 32);
}

void gic_disable(int intid) {
    if (intid < 0 || intid >= INTID_MAX) return;
    *gicd(GICD_ICENABLER + (uint32_t)(intid / 32) * 4) = 1u << (intid % 32);
}

uint32_t gic_spurious(void) { return g_spurious; }
uint64_t gic_taken(void)    { return g_taken; }

/* Called from the IRQ vector. Acknowledge, dispatch, end-of-interrupt — in
   that order, because the GIC will not deliver the next one until EOI. */
void gic_dispatch(void) {
    for (;;) {
        uint32_t iar = *gicc(GICC_IAR);
        uint32_t id = iar & 0x3FF;
        if (id >= 1020) return;                 /* 1023 = spurious          */
        if (id < INTID_MAX && g_handler[id]) {
            g_taken++;
            g_handler[id]();
        } else {
            g_spurious++;
        }
        *gicc(GICC_EOIR) = iar;
    }
}
