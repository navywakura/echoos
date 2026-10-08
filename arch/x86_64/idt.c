/*
 * IDT setup + exception dispatcher. Low-level stubs live in boot.asm;
 * here we build the 64-bit gates and load the table.
 */
#include "rogex.h"
#include "power.h"

#define IDT_ENTRIES 256

struct ROGEX_PACKED idt_entry {
    uint16_t offset_low;
    uint16_t selector;      /* code segment selector in our GDT */
    uint8_t  ist;           /* interrupt stack table index (0 = none) */
    uint8_t  type_attr;     /* gate type + DPL + present */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
};

struct ROGEX_PACKED idt_ptr {
    uint16_t limit;
    uint64_t base;
};

/* CPU register frame pushed by the common ASM stub, in reverse push order. */
struct ROGEX_PACKED isr_frame {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;   /* pushed by the CPU on interrupt */
};

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtr;

/* GDT code selector: null(0) + code(0x08) as laid out in boot.asm's gdt64. */
#define KERNEL_CS 0x08
/* 64-bit interrupt gate, present, DPL=0: 0x8E. */
#define GATE_INT64 0x8e

/* Exported from boot.asm — the 32 exception stubs and the lidt helper. */
extern void rogex_lidt(struct idt_ptr *p);
#define DECL_ISR(n) extern void isr##n(void);
#define ISR_ADDR(n) ((uint64_t)(uintptr_t)&isr##n)

DECL_ISR(0)  DECL_ISR(1)  DECL_ISR(2)  DECL_ISR(3)  DECL_ISR(4)  DECL_ISR(5)
DECL_ISR(6)  DECL_ISR(7)  DECL_ISR(8)  DECL_ISR(9)  DECL_ISR(10) DECL_ISR(11)
DECL_ISR(12) DECL_ISR(13) DECL_ISR(14) DECL_ISR(15) DECL_ISR(16) DECL_ISR(17)
DECL_ISR(18) DECL_ISR(19) DECL_ISR(20) DECL_ISR(21) DECL_ISR(22) DECL_ISR(23)
DECL_ISR(24) DECL_ISR(25) DECL_ISR(26) DECL_ISR(27) DECL_ISR(28) DECL_ISR(29)
DECL_ISR(30) DECL_ISR(31)

/* Public gate installer for vectors added after boot (hardware IRQs in
   arch/x86_64/irq.c). The IDT register already points at this table, so a new
   gate takes effect immediately — no re-lidt needed. */
void idt_register(int vec, uint64_t handler);

static void set_gate(int vec, uint64_t handler) {
    idt[vec].offset_low  = handler & 0xffff;
    idt[vec].selector    = KERNEL_CS;
    idt[vec].ist         = 0;
    idt[vec].type_attr   = GATE_INT64;
    idt[vec].offset_mid  = (handler >> 16) & 0xffff;
    idt[vec].offset_high = (handler >> 32) & 0xffffffff;
    idt[vec].reserved    = 0;
}

void idt_install(void) {
    const uint64_t stubs[32] = {
        ISR_ADDR(0),  ISR_ADDR(1),  ISR_ADDR(2),  ISR_ADDR(3),
        ISR_ADDR(4),  ISR_ADDR(5),  ISR_ADDR(6),  ISR_ADDR(7),
        ISR_ADDR(8),  ISR_ADDR(9),  ISR_ADDR(10), ISR_ADDR(11),
        ISR_ADDR(12), ISR_ADDR(13), ISR_ADDR(14), ISR_ADDR(15),
        ISR_ADDR(16), ISR_ADDR(17), ISR_ADDR(18), ISR_ADDR(19),
        ISR_ADDR(20), ISR_ADDR(21), ISR_ADDR(22), ISR_ADDR(23),
        ISR_ADDR(24), ISR_ADDR(25), ISR_ADDR(26), ISR_ADDR(27),
        ISR_ADDR(28), ISR_ADDR(29), ISR_ADDR(30), ISR_ADDR(31),
    };
    k_memset(idt, 0, sizeof(idt));
    for (int v = 0; v < 32; v++) set_gate(v, stubs[v]);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)(uintptr_t)&idt[0];
    rogex_lidt(&idtr);
}

static const char *EXC_NAME[32] = {
    "#DE divide error", "#DB debug", "NMI", "#BP breakpoint",
    "#OF overflow", "#BR bound range", "#UD invalid opcode",
    "#NM device n/a", "#DF double fault", "coproc overrun",
    "#TS invalid TSS", "#NP segment n/p", "#SS stack fault",
    "#GP protection", "#PF page fault", "reserved", "#MF x87",
    "#AC alignment", "#MC machine check", "#XM SIMD", "#VE virt",
    "#CP control-prot", "res", "res", "res", "res", "res", "res",
    "res", "res", "#SX security", "res",
};

/* Recoverable #GP: a guarded RDMSR probing an MSR the CPU does not implement.
   The trampoline in boot.asm restores the GPRs and does `iretq` after we
   return, so rewriting rip in the saved frame resumes execution wherever we
   point it. msr_read_safe() records both the address of its RDMSR and the
   address of the instruction after it, so this is an exact match rather than a
   guess at instruction length — and a #GP raised anywhere else in the kernel
   still falls through to the fatal path below. */
static bool recover_guarded_rdmsr(struct isr_frame *f) {
    if (f->int_no != 13 || !msr_guard_armed())  return false;
    if (f->rip != msr_guard_fault_rip())        return false;
    msr_guard_note_fault();
    f->rip = msr_guard_resume_rip();
    return true;
}

/* Called by the ASM trampoline with a pointer to the saved frame. */
void isr_dispatch(struct isr_frame *f) {
    if (recover_guarded_rdmsr(f)) return;

    console_write("\n[rogex] CPU EXCEPTION ");
    console_write_dec(f->int_no);
    if (f->int_no < 32) { console_write(" ("); console_write(EXC_NAME[f->int_no]); console_write(")"); }
    console_write("  err=");
    console_write_hex(f->err_code);
    console_write("  rip=");
    console_write_hex(f->rip);
    console_write("\n");
    panic("unhandled CPU exception");
}

void idt_register(int vec, uint64_t handler) {
    if (vec < 0 || vec >= IDT_ENTRIES) return;
    set_gate(vec, handler);
}
