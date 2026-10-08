/* EL1 exception vectors.
 *
 * The table is 16 entries of 128 bytes, aligned to 2 KiB, and the CPU picks
 * an entry by (source level, exception type). Only the IRQ from the current
 * level with SP_ELx is wired to something; the rest land in a handler that
 * stops the machine rather than returning into an unknown state.
 */
.macro SAVE_ALL
    sub     sp, sp, #256
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]
.endm

.macro RESTORE_ALL
    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]
    add     sp, sp, #256
.endm

.macro VECTOR_ENTRY label
    .align  7
    b       \label
.endm

.section .text
.align 11
.global aarch64_vectors
aarch64_vectors:
    /* Current EL with SP_EL0 */
    VECTOR_ENTRY el1_fatal          /* synchronous                          */
    VECTOR_ENTRY el1_irq
    VECTOR_ENTRY el1_fatal          /* FIQ                                  */
    VECTOR_ENTRY el1_fatal          /* SError                               */
    /* Current EL with SP_ELx */
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_irq
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal
    /* Lower EL, AArch64 */
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_irq
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal
    /* Lower EL, AArch32 */
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal
    VECTOR_ENTRY el1_fatal

el1_irq:
    SAVE_ALL
    bl      gic_dispatch
    RESTORE_ALL
    eret

el1_fatal:
    SAVE_ALL
    mrs     x0, esr_el1
    mrs     x1, elr_el1
    mrs     x2, far_el1
    bl      aarch64_fatal
1:  wfe
    b       1b
