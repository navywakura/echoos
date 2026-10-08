; Cooperative context switch. Saves/restores callee-saved regs + rsp.
; Called only from the scheduler yield path — never from an ISR.
;
; The scheduler calls this through the arch HAL, so the exported name is the
; HAL's: what a context switch saves is machine knowledge, and the generic
; scheduler must not have to know that x86 spells it ctx_switch.
bits 64
section .text
global arch_context_switch
global ctx_switch

arch_context_switch:
ctx_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp          ; *save_rsp = current stack pointer
    mov rsp, rsi            ; switch to the incoming task's stack

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret                     ; resume: pops that task's return address
