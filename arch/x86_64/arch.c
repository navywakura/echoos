/* x86_64 implementation of arch/arch.h. Everything the rest of the kernel is
   not allowed to write directly lives here. */
#include "arch.h"
#include "ports.h"
#include "pit.h"
#include "irq.h"

const char *arch_name(void) { return "x86_64"; }

/* --- interrupts ---------------------------------------------------------- */

uint64_t arch_irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

void arch_irq_restore(uint64_t token) {
    /* Bit 9 is IF. Restoring means "put it back as it was", not "enable". */
    if (token & 0x200ull) __asm__ volatile ("sti" ::: "memory");
}

void arch_irq_enable(void)  { __asm__ volatile ("sti" ::: "memory"); }
void arch_irq_disable(void) { __asm__ volatile ("cli" ::: "memory"); }

bool arch_irq_enabled(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags));
    return (flags & 0x200ull) != 0;
}

bool arch_irq_init(void) { irq_install(); return true; }

bool arch_irq_register(int line, void (*handler)(void)) {
    if (line < 0 || !handler) return false;
    irq_register(line, handler);
    return true;
}

/* --- timer --------------------------------------------------------------- */

bool     arch_timer_init(void)      { return pit_init(); }
uint64_t arch_timer_ticks(void)     { return pit_ticks(); }
uint64_t arch_timer_uptime_ms(void) { return pit_uptime_ms(); }
uint32_t arch_timer_hz(void)        { return PIT_HZ; }

/* --- idling -------------------------------------------------------------- */

void arch_idle_wait(void) { __asm__ volatile ("hlt"); }
void arch_relax(void)     { __asm__ volatile ("pause"); }

ROGEX_NORETURN void arch_halt_forever(void) {
    for (;;) __asm__ volatile ("cli; hlt");
}

/* --- barriers ------------------------------------------------------------ */

void arch_barrier_full(void)  { __asm__ volatile ("mfence" ::: "memory"); }
void arch_barrier_read(void)  { __asm__ volatile ("lfence" ::: "memory"); }
void arch_barrier_write(void) { __asm__ volatile ("sfence" ::: "memory"); }

/* --- MMU ----------------------------------------------------------------- */

uint64_t arch_mmu_root(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

void arch_mmu_flush_page(uint64_t va) {
    __asm__ volatile ("invlpg (%0)" :: "r"(va) : "memory");
}

void arch_mmu_flush_all(void) {
    uint64_t v = arch_mmu_root();
    __asm__ volatile ("mov %0, %%cr3" :: "r"(v) : "memory");
}

/* --- early console ------------------------------------------------------- */
/* COM1, programmed directly: this has to work before the serial driver, the
   framebuffer or the heap exist, and it is what a panic writes to. */
#define COM1 0x3F8

void arch_console_early_init(void) {
    outb(COM1 + 1, 0x00);      /* no interrupts                             */
    outb(COM1 + 3, 0x80);      /* DLAB                                      */
    outb(COM1 + 0, 0x01);      /* 115200                                    */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);      /* 8N1                                       */
    outb(COM1 + 2, 0xC7);      /* FIFO on, clear, 14-byte threshold         */
}

void arch_console_putc(char c) {
    uint32_t guard = 0;
    while (!(inb(COM1 + 5) & 0x20) && guard++ < 100000) { }
    outb(COM1, (uint8_t)c);
}

/* --- cycles -------------------------------------------------------------- */

uint64_t arch_cycles(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* --- CPU state ----------------------------------------------------------- */

uint32_t arch_cpu_state(arch_reg_t *out, uint32_t max) {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rsp, rbp, rflags, cr0, cr3, cr4;
    uint32_t n = 0;
    if (!out || !max) return 0;
    __asm__ volatile ("mov %%rax, %0" : "=r"(rax));
    __asm__ volatile ("mov %%rbx, %0" : "=r"(rbx));
    __asm__ volatile ("mov %%rcx, %0" : "=r"(rcx));
    __asm__ volatile ("mov %%rdx, %0" : "=r"(rdx));
    __asm__ volatile ("mov %%rsi, %0" : "=r"(rsi));
    __asm__ volatile ("mov %%rdi, %0" : "=r"(rdi));
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile ("mov %%rbp, %0" : "=r"(rbp));
    __asm__ volatile ("pushfq; pop %0" : "=r"(rflags));
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
#define PUT(N, V) do { if (n < max) { out[n].name = (N); out[n].value = (V); n++; } } while (0)
    PUT("rax", rax); PUT("rbx", rbx); PUT("rcx", rcx); PUT("rdx", rdx);
    PUT("rsi", rsi); PUT("rdi", rdi); PUT("rsp", rsp); PUT("rbp", rbp);
    PUT("rflags", rflags); PUT("cr0", cr0); PUT("cr3", cr3); PUT("cr4", cr4);
#undef PUT
    return n;
}

/* --- reset --------------------------------------------------------------- */
/* The i8042 keyboard controller's reset line: crude, universal on this
   architecture, and the fallback when ACPI has nothing to offer. */
bool arch_reboot_input_busy(void) { return (inb(0x64) & 0x02) != 0; }

void arch_reboot(void) { outb(0x64, 0xFE); }

/* --- CPU identity -------------------------------------------------------- */

static void cpuid_raw(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                              : "a"(leaf), "c"(0));
}

void arch_cpu_vendor(char out[13]) {
    uint32_t a, b, c, d;
    cpuid_raw(0, &a, &b, &c, &d);
    for (int i = 0; i < 4; i++) {
        out[i]     = (char)((b >> (8 * i)) & 0xFF);
        out[4 + i] = (char)((d >> (8 * i)) & 0xFF);
        out[8 + i] = (char)((c >> (8 * i)) & 0xFF);
    }
    out[12] = '\0';
}

uint32_t arch_cpu_signature(void) {
    uint32_t a, b, c, d;
    cpuid_raw(1, &a, &b, &c, &d);
    return a;
}

uint32_t arch_cpu_features(void) {
    uint32_t a, b, c, d;
    cpuid_raw(1, &a, &b, &c, &d);
    return d;
}

/* --- task frames --------------------------------------------------------- */
/* What ctx_switch (switch.asm) restores, in the order it pops them: six
   callee-saved registers, then the address it `ret`s into. The slot below
   `entry` is the return address entry() itself sees, so a task that returns
   lands in on_return rather than executing whatever follows. */
uint64_t arch_task_frame_init(uint64_t stack_top, void (*entry)(void),
                              void (*on_return)(void)) {
    uint64_t *sp = (uint64_t *)(stack_top & ~0xFULL);
    *--sp = (uint64_t)on_return;   /* entry()'s own return address           */
    *--sp = (uint64_t)entry;       /* what the switch will `ret` into        */
    *--sp = 0;                     /* rbp                                    */
    *--sp = 0;                     /* rbx                                    */
    *--sp = 0;                     /* r12                                    */
    *--sp = 0;                     /* r13                                    */
    *--sp = 0;                     /* r14                                    */
    *--sp = 0;                     /* r15                                    */
    return (uint64_t)sp;
}
