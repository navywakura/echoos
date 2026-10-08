/* AArch64 implementation of arch/arch.h, for QEMU `virt` and anything else
   that puts a PL011 and a GIC where the device tree says.

   Nothing here is a translation of the x86 file: `hlt` becomes `wfi` because
   both park the core, not because they are the same instruction, and the
   memory barriers are the ones this architecture actually needs — AArch64 is
   weakly ordered, so a missing `dsb` is a bug that x86 would have hidden. */
#include "arch.h"
#include "pl011.h"
#include "gic.h"
#include "atimer.h"

const char *arch_name(void) { return "aarch64"; }

/* --- interrupts ---------------------------------------------------------- */
/* DAIF.I is the interrupt mask: 1 means masked. The token is the whole DAIF
   so restoring cannot enable interrupts that were not enabled before. */

uint64_t arch_irq_save(void) {
    uint64_t daif;
    __asm__ volatile ("mrs %0, daif" : "=r"(daif));
    __asm__ volatile ("msr daifset, #2" ::: "memory");
    return daif;
}

void arch_irq_restore(uint64_t token) {
    __asm__ volatile ("msr daif, %0" :: "r"(token) : "memory");
}

void arch_irq_enable(void)  { __asm__ volatile ("msr daifclr, #2" ::: "memory"); }
void arch_irq_disable(void) { __asm__ volatile ("msr daifset, #2" ::: "memory"); }

bool arch_irq_enabled(void) {
    uint64_t daif;
    __asm__ volatile ("mrs %0, daif" : "=r"(daif));
    return (daif & (1u << 7)) == 0;      /* bit 7 = I */
}

bool arch_irq_init(void) { return gic_init(); }

bool arch_irq_register(int line, void (*handler)(void)) {
    return gic_register(line, handler);
}

/* --- timer --------------------------------------------------------------- */

bool     arch_timer_init(void)      { return atimer_init(); }
uint64_t arch_timer_ticks(void)     { return atimer_ticks(); }
uint64_t arch_timer_uptime_ms(void) { return atimer_uptime_ms(); }
uint32_t arch_timer_hz(void)        { return ATIMER_HZ; }

/* --- idling -------------------------------------------------------------- */

void arch_idle_wait(void) { __asm__ volatile ("wfi"); }
void arch_relax(void)     { __asm__ volatile ("yield"); }

ROGEX_NORETURN void arch_halt_forever(void) {
    arch_irq_disable();
    for (;;) __asm__ volatile ("wfe");
}

/* --- barriers ------------------------------------------------------------ */
/* `sy` is the full system domain: the virtio rings are shared with a device,
   and an inner-shareable barrier would not order against it. */

void arch_barrier_full(void)  { __asm__ volatile ("dsb sy" ::: "memory"); }
void arch_barrier_read(void)  { __asm__ volatile ("dsb ld" ::: "memory"); }
void arch_barrier_write(void) { __asm__ volatile ("dsb st" ::: "memory"); }

/* --- MMU ----------------------------------------------------------------- */

uint64_t arch_mmu_root(void) {
    uint64_t v;
    __asm__ volatile ("mrs %0, ttbr0_el1" : "=r"(v));
    return v;
}

void arch_mmu_flush_page(uint64_t va) {
    __asm__ volatile ("dsb ishst\n\t"
                      "tlbi vaae1is, %0\n\t"
                      "dsb ish\n\t"
                      "isb" :: "r"(va >> 12) : "memory");
}

void arch_mmu_flush_all(void) {
    __asm__ volatile ("dsb ishst\n\t"
                      "tlbi vmalle1is\n\t"
                      "dsb ish\n\t"
                      "isb" ::: "memory");
}

/* --- early console ------------------------------------------------------- */

void arch_console_early_init(void) { pl011_init(); }
void arch_console_putc(char c)     { pl011_putc(c); }

/* --- cycles -------------------------------------------------------------- */
/* The virtual counter. Unlike the x86 TSC this one is architecturally a
   fixed-frequency counter, which is why the monotonic clock on this target
   needs no watchdog against the tick. */

uint64_t arch_cycles(void) {
    uint64_t v;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/* --- reset --------------------------------------------------------------- */
/* PSCI SYSTEM_RESET through HVC, which is what QEMU virt provides. If the
   firmware ignores it the caller stops, as on every other target. */

bool arch_reboot_input_busy(void) { return false; }

void arch_reboot(void) {
    register uint64_t x0 __asm__("x0") = 0x84000009ull;   /* SYSTEM_RESET   */
    __asm__ volatile ("hvc #0" :: "r"(x0) : "memory");
}

/* --- CPU identity -------------------------------------------------------- */

void arch_cpu_vendor(char out[13]) {
    uint64_t midr;
    const char *name;
    int i = 0;
    __asm__ volatile ("mrs %0, midr_el1" : "=r"(midr));
    switch ((midr >> 24) & 0xFF) {
    case 0x41: name = "ARM";      break;
    case 0x51: name = "Qualcomm"; break;
    case 0x61: name = "Apple";    break;
    case 0x00: name = "QEMU";     break;
    default:   name = "unknown";  break;
    }
    while (name[i] && i < 12) { out[i] = name[i]; i++; }
    out[i] = '\0';
}

uint32_t arch_cpu_signature(void) {
    uint64_t midr;
    __asm__ volatile ("mrs %0, midr_el1" : "=r"(midr));
    return (uint32_t)midr;
}

uint32_t arch_cpu_features(void) {
    uint64_t isar0;
    __asm__ volatile ("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    return (uint32_t)isar0;
}

/* --- CPU state ----------------------------------------------------------- */

uint32_t arch_cpu_state(arch_reg_t *out, uint32_t max) {
    uint64_t sp, elr, spsr, esr, ttbr0, sctlr, currentel, daif, midr, cntfrq;
    uint32_t n = 0;
    if (!out || !max) return 0;
    __asm__ volatile ("mov %0, sp"            : "=r"(sp));
    __asm__ volatile ("mrs %0, elr_el1"       : "=r"(elr));
    __asm__ volatile ("mrs %0, spsr_el1"      : "=r"(spsr));
    __asm__ volatile ("mrs %0, esr_el1"       : "=r"(esr));
    __asm__ volatile ("mrs %0, ttbr0_el1"     : "=r"(ttbr0));
    __asm__ volatile ("mrs %0, sctlr_el1"     : "=r"(sctlr));
    __asm__ volatile ("mrs %0, CurrentEL"     : "=r"(currentel));
    __asm__ volatile ("mrs %0, daif"          : "=r"(daif));
    __asm__ volatile ("mrs %0, midr_el1"      : "=r"(midr));
    __asm__ volatile ("mrs %0, cntfrq_el0"    : "=r"(cntfrq));
#define PUT(N, V) do { if (n < max) { out[n].name = (N); out[n].value = (V); n++; } } while (0)
    PUT("sp", sp);        PUT("elr_el1", elr);   PUT("spsr_el1", spsr);
    PUT("esr_el1", esr);  PUT("ttbr0", ttbr0);   PUT("sctlr", sctlr);
    PUT("EL", currentel >> 2); PUT("daif", daif);
    PUT("midr", midr);    PUT("cntfrq", cntfrq);
#undef PUT
    return n;
}

/* --- task frames --------------------------------------------------------- */
/* Mirrors what switch.S pops: x19-x28 then x29/x30, 96 bytes, and the frame
   must stay 16-byte aligned or the first `stp` in the task faults. x30 is
   the link register, so putting `entry` there is what makes the switch's
   final `ret` land in the new task; x19 carries on_return for a task that
   falls off the end of its entry function. */
extern void arch_task_trampoline(void);

uint64_t arch_task_frame_init(uint64_t stack_top, void (*entry)(void),
                              void (*on_return)(void)) {
    uint64_t *sp = (uint64_t *)(stack_top & ~0xFULL);
    sp -= 12;                       /* 96 bytes, 16-byte aligned            */
    sp[0]  = (uint64_t)on_return;   /* x29 (frame pointer slot, unused)     */
    sp[1]  = (uint64_t)arch_task_trampoline;  /* x30: the switch's `ret`    */
    sp[2]  = 0; sp[3]  = 0;         /* x27, x28                             */
    sp[4]  = 0; sp[5]  = 0;         /* x25, x26                             */
    sp[6]  = 0; sp[7]  = 0;         /* x23, x24                             */
    sp[8]  = 0; sp[9]  = 0;         /* x21, x22                             */
    sp[10] = (uint64_t)entry;       /* x19: the trampoline's `blr` target   */
    sp[11] = (uint64_t)on_return;   /* x20: where a returning task lands    */
    return (uint64_t)sp;
}
