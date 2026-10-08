/*
 * The line between "this kernel" and "this machine".
 *
 * Everything below is what a generic module is allowed to ask of the
 * hardware. Each target implements it in arch/<target>/; nothing outside
 * that directory may write an instruction, touch a port, or name a chip.
 *
 * The rule is not aesthetic. A generic module that says `hlt` compiles only
 * on x86, and finding those by hand on the day the AArch64 port starts is
 * how ports die. tools/portability_gate.py fails the build's certificate if
 * one comes back.
 *
 * What is deliberately NOT here: device drivers. A PS/2 keyboard, a PIC and
 * a PCI configuration port are x86 devices, and their drivers are allowed to
 * know it. What must not know it is the scheduler, the filesystem, the shell
 * or the robotic runtime.
 */
#ifndef ECHOS_ARCH_HAL_H
#define ECHOS_ARCH_HAL_H

#include "rogex.h"

/* --- identity ------------------------------------------------------------ */
const char *arch_name(void);          /* "x86_64", "aarch64", ...           */

/* --- interrupts ---------------------------------------------------------- */
/* Save-and-disable returns an opaque token: restoring it re-enables only if
   interrupts were enabled before. A bare enable in a nested critical section
   is the classic way to corrupt a queue. */
uint64_t arch_irq_save(void);
void     arch_irq_restore(uint64_t token);
void     arch_irq_enable(void);
void     arch_irq_disable(void);
bool     arch_irq_enabled(void);

/* Interrupt controller and its lines. */
bool arch_irq_init(void);
bool arch_irq_register(int line, void (*handler)(void));

/* --- periodic timer ------------------------------------------------------ */
/* The tick that drives scheduling and every elapsed-time measurement that
   does not need microseconds (those use arch/mono.h). */
bool     arch_timer_init(void);
uint64_t arch_timer_ticks(void);
uint64_t arch_timer_uptime_ms(void);
uint32_t arch_timer_hz(void);

/* --- idling -------------------------------------------------------------- */
void arch_idle_wait(void);            /* park until the next interrupt      */
void arch_relax(void);                /* hint inside a spin loop            */
ROGEX_NORETURN void arch_halt_forever(void);   /* stop, for good            */

/* --- barriers ------------------------------------------------------------ */
/* Ordering, not optimisation fences: the SPSC rings and the device drivers
   need the writes to land in the order they were written. */
void arch_barrier_full(void);
void arch_barrier_read(void);
void arch_barrier_write(void);

/* --- MMU ----------------------------------------------------------------- */
uint64_t arch_mmu_root(void);              /* CR3 / TTBR0                   */
void     arch_mmu_flush_page(uint64_t va);
void     arch_mmu_flush_all(void);

/* --- console of last resort ---------------------------------------------- */
/* Before any driver exists, and after everything else has failed. */
void arch_console_early_init(void);
void arch_console_putc(char c);

/* --- cycle counter ------------------------------------------------------- */
/* For benchmarks only. It counts something the CPU counts; it is not wall
   time and must not be used as such — see arch/mono.h for that distinction,
   which cost this project a link that flapped eighteen times. */
uint64_t arch_cycles(void);

/* --- reset --------------------------------------------------------------- */
/* Ask the machine to restart. Returns only if it refused; the caller is
   expected to stop afterwards rather than carry on in an unknown state. */
void arch_reboot(void);
bool arch_reboot_input_busy(void);     /* the controller is not ready yet   */

/* --- context switch ------------------------------------------------------ */
void arch_context_switch(uint64_t *save_sp, uint64_t load_sp);

/* Build the initial stack frame for a task so the first switch into it lands
   at `entry`, and a task that returns lands in `on_return` instead of
   executing whatever was on the stack. Returns the stack pointer to store.
 *
 * The scheduler used to build this frame itself, in x86 layout, with a
 * comment explaining which slot was `rbx`. It compiled on AArch64 and would
 * have jumped into a register save area — the kind of thing a port finds and
 * a header cannot. */
uint64_t arch_task_frame_init(uint64_t stack_top, void (*entry)(void),
                              void (*on_return)(void));

/* --- CPU identity -------------------------------------------------------- */
/* Enough for a hardware UID and a name on a status card, without the caller
   knowing whether that came from CPUID, MIDR_EL1 or a device tree. */
void        arch_cpu_vendor(char out[13]);
uint32_t    arch_cpu_signature(void);
uint32_t    arch_cpu_features(void);

/* --- CPU state, for diagnostics ------------------------------------------ */
/* Named registers, so a `cpu` command prints whatever this target has
   instead of assuming x86 has rax and everyone else does too. */
typedef struct { const char *name; uint64_t value; } arch_reg_t;
uint32_t arch_cpu_state(arch_reg_t *out, uint32_t max);

#endif /* ECHOS_ARCH_HAL_H */
