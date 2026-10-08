; IRQ stubs vectors 32..47. Same frame layout as the exception stubs in boot.asm.
bits 64

extern irq_dispatch

%macro IRQ_STUB 2                  ; %1 = irq number, %2 = vector
global irq%1
irq%1:
    push qword 0                   ; dummy error code -> uniform frame
    push qword %2                  ; vector number
    jmp irq_common
%endmacro

IRQ_STUB 0,  32                    ; PIT timer
IRQ_STUB 1,  33                    ; PS/2 keyboard
IRQ_STUB 2,  34
IRQ_STUB 3,  35
IRQ_STUB 4,  36
IRQ_STUB 5,  37
IRQ_STUB 6,  38
IRQ_STUB 7,  39
IRQ_STUB 8,  40
IRQ_STUB 9,  41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44                    ; PS/2 mouse
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

irq_common:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    mov rdi, rsp                   ; arg0 = &isr_frame
    cld
    call irq_dispatch

    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    add rsp, 16                    ; discard vector + dummy error code
    iretq
