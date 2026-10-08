/*
 * 8259 PIC: remap IRQs 0..15 to vectors 32..47, dispatch, EOI.
 * Lines start masked; unmasked when a driver registers.
 */
#include "irq.h"
#include "ports.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xa0
#define PIC2_DATA 0xa1
#define PIC_EOI   0x20

#define IRQ_BASE  32

/* The 16 stubs from isr.asm. */
#define DECL_IRQ(n) extern void irq##n(void);
DECL_IRQ(0)  DECL_IRQ(1)  DECL_IRQ(2)  DECL_IRQ(3)
DECL_IRQ(4)  DECL_IRQ(5)  DECL_IRQ(6)  DECL_IRQ(7)
DECL_IRQ(8)  DECL_IRQ(9)  DECL_IRQ(10) DECL_IRQ(11)
DECL_IRQ(12) DECL_IRQ(13) DECL_IRQ(14) DECL_IRQ(15)

/* From arch/x86_64/idt.c: install a gate in the already-loaded IDT. */
void idt_register(int vec, uint64_t handler);

static irq_handler_t g_handlers[16];

static void pic_remap(void) {
    uint8_t m1 = inb(PIC1_DATA), m2 = inb(PIC2_DATA);
    outb(PIC1_CMD, 0x11); io_wait();          /* ICW1: init + ICW4          */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, IRQ_BASE); io_wait();     /* ICW2: vector offsets        */
    outb(PIC2_DATA, IRQ_BASE + 8); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();         /* ICW3: slave on line 2       */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();         /* ICW4: 8086 mode             */
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, m1);                       /* restore masks               */
    outb(PIC2_DATA, m2);
}

void irq_mask(int line) {
    uint16_t port = line < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = 1 << (line & 7);
    outb(port, inb(port) | bit);
}

void irq_unmask(int line) {
    uint16_t port = line < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = 1 << (line & 7);
    outb(port, inb(port) & (uint8_t)~bit);
}

void irq_register(int line, irq_handler_t handler) {
    if (line < 0 || line > 15) return;
    g_handlers[line] = handler;
    irq_unmask(line);
    if (line >= 8) irq_unmask(2);              /* cascade line for the slave */
}

void irq_install(void) {
    const uint64_t stubs[16] = {
        (uint64_t)(uintptr_t)&irq0,  (uint64_t)(uintptr_t)&irq1,
        (uint64_t)(uintptr_t)&irq2,  (uint64_t)(uintptr_t)&irq3,
        (uint64_t)(uintptr_t)&irq4,  (uint64_t)(uintptr_t)&irq5,
        (uint64_t)(uintptr_t)&irq6,  (uint64_t)(uintptr_t)&irq7,
        (uint64_t)(uintptr_t)&irq8,  (uint64_t)(uintptr_t)&irq9,
        (uint64_t)(uintptr_t)&irq10, (uint64_t)(uintptr_t)&irq11,
        (uint64_t)(uintptr_t)&irq12, (uint64_t)(uintptr_t)&irq13,
        (uint64_t)(uintptr_t)&irq14, (uint64_t)(uintptr_t)&irq15,
    };
    pic_remap();
    for (int i = 0; i < 16; i++) {
        idt_register(IRQ_BASE + i, stubs[i]);
        irq_mask(i);                           /* everything masked at first */
    }
    __asm__ volatile ("sti");
}

/* Same frame layout as idt.c's isr_frame; we only need the vector. */
struct ROGEX_PACKED irq_frame {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t vector, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

void irq_dispatch(struct irq_frame *f);
void irq_dispatch(struct irq_frame *f) {
    int line = (int)f->vector - IRQ_BASE;
    if (line >= 0 && line < 16 && g_handlers[line]) g_handlers[line]();
    if (line >= 8) outb(PIC2_CMD, PIC_EOI);    /* EOI slave first            */
    outb(PIC1_CMD, PIC_EOI);
}
