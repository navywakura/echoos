; Multiboot2 entry: long mode, page tables (4 GiB identity), flat GDT,
; exception stubs 0..31, then kmain. After GRUB hands off we own the machine.
; ------------------------------ Multiboot2 header ---------------------------
MB2_MAGIC        equ 0xe85250d6          ; multiboot2 magic
MB2_ARCH         equ 0                   ; 0 = i386 (protected mode handoff)

section .multiboot_header
align 8
header_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd header_end - header_start                              ; header length
    dd 0x100000000 - (MB2_MAGIC + MB2_ARCH + (header_end - header_start))

    ; --- framebuffer request tag (type 5): ask GRUB for a linear FB ---
    ; Width and height are 0 = "no preference" (Multiboot2 spec). GRUB turns
    ; whatever it finds here into a gfxpayload setting, so naming a size forces
    ; that one size on every machine: 1280x720 is not a standard VESA mode, and
    ; when a laptop's VBE BIOS does not have it GRUB drops to text and we boot
    ; with no framebuffer at all. Asking for nothing lets grub.cfg supply an
    ; ordered fallback list instead (see boot/grub/grub-universal.cfg). Depth stays 32
    ; because drivers/video/framebuffer.c only draws 32bpp and declines
    ; anything else.
align 8
    dw 5        ; type = framebuffer
    dw 0        ; flags = 0 (required)
    dd 20       ; size
    dd 0        ; width  — no preference
    dd 0        ; height — no preference
    dd 32       ; bpp    — required: the kernel only renders 32bpp

    ; --- end tag ---
align 8
    dw 0        ; type
    dw 0        ; flags
    dd 8        ; size
header_end:

; ------------------------------- 32-bit entry -------------------------------
section .bootstrap_stack nobits
align 16
stack_bottom:
    ; 256 KiB boot stack. ML-KEM-768 keygen/encap/decap place large polynomial
    ; matrices on the stack (tens of KiB); a small stack overflows and faults.
    resb 262144
stack_top:

section .boot_data nobits
align 8
mb_info_ptr:  resq 1    ; Multiboot2 info structure pointer (from GRUB ebx)
mb_magic:     resq 1    ; Multiboot2 magic value          (from GRUB eax)

section .text
bits 32
global _start
extern kmain
extern bss_start
extern _kernel_end
_start:
    ; Flat-binary disk boots do not get GRUB's BSS wipe. Zero it with a
    ; temporary stack below 1 MiB so we do not erase the stack we are on.
    ; Save the Multiboot2 registers first — they live in .boot_data (BSS).
    mov esp, 0x90000
    push eax
    push ebx
    mov edi, bss_start
    mov ecx, _kernel_end
    sub ecx, edi
    xor eax, eax
    shr ecx, 2
    rep stosd
    pop ebx
    pop eax
    mov esp, stack_top
    ; GRUB entry contract: eax = multiboot2 magic, ebx = info struct pointer.
    mov [mb_info_ptr], ebx
    mov [mb_magic], eax

    call check_multiboot
    call check_cpuid
    call check_long_mode

    call set_up_page_tables
    call enable_paging

    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start      ; far jump reloads CS -> 64-bit code

    ; unreachable
    cli
.hang32:
    hlt
    jmp .hang32

; --- Was the Multiboot2 magic present in eax on entry? ----------------------
check_multiboot:
    cmp dword [mb_magic], 0x36d76289    ; value the loader leaves in eax
    jne .no_multiboot
    ret
.no_multiboot:
    mov al, "0"
    jmp boot_error

; --- Is CPUID available (toggle bit 21 of EFLAGS)? --------------------------
check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov al, "1"
    jmp boot_error

; --- Does the CPU support IA-32e long mode? --------------------------------
check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29                   ; LM bit
    jz .no_long_mode
    ret
.no_long_mode:
    mov al, "2"
    jmp boot_error

; --- Identity-map the first 4 GiB using 2 MiB pages ------------------------
; One GiB was enough for QEMU with 512 MiB of RAM and a framebuffer the kernel
; mapped by hand. It is not enough for a real laptop: an 8 GiB machine puts its
; largest usable RAM region above 4 GiB, and every PCI BAR, the local APIC and
; the integrated GPU's aperture live in the MMIO hole between 3 and 4 GiB. Four
; PDPT entries, each a full page directory of 512 huge pages, cover the whole
; 32-bit physical space for 16 KiB of page tables.
PAGE_DIR_COUNT equ 4                    ; 4 * 1 GiB
set_up_page_tables:
    ; P4[0] -> P3
    mov eax, p3_table
    or  eax, 0b11                       ; present + writable
    mov [p4_table], eax

    ; P3[i] -> P2 table i
    mov eax, p2_table
    or  eax, 0b11
    xor ecx, ecx
.map_p3:
    mov [p3_table + ecx * 8], eax
    add eax, 4096                       ; next page directory
    inc ecx
    cmp ecx, PAGE_DIR_COUNT
    jne .map_p3

    ; P2[i] -> 2 MiB huge page i  (PAGE_DIR_COUNT * 512 entries)
    xor ecx, ecx
.map_p2:
    mov eax, 0x200000                   ; 2 MiB
    mul ecx                             ; edx:eax = i * 2 MiB (stays < 4 GiB)
    or  eax, 0b10000011                 ; present + writable + huge
    mov [p2_table + ecx * 8], eax
    inc ecx
    cmp ecx, PAGE_DIR_COUNT * 512
    jne .map_p2
    ret

; --- Turn on PAE, long mode enable, and paging -----------------------------
enable_paging:
    mov eax, p4_table
    mov cr3, eax                        ; load top-level table

    mov eax, cr4
    or  eax, 1 << 5                     ; CR4.PAE
    mov cr4, eax

    mov ecx, 0xC0000080                 ; EFER MSR
    rdmsr
    or  eax, 1 << 8                     ; EFER.LME (long mode enable)
    wrmsr

    mov eax, cr0
    or  eax, 1 << 31                    ; CR0.PG (paging)
    or  eax, 1 << 16                    ; CR0.WP (honor write-protect in ring0)
    mov cr0, eax
    ret

; --- Fatal boot error: print "ERR: X" to VGA and halt ----------------------
boot_error:
    ; al already holds the ASCII error code
    mov dword [0xb8000], 0x4f524f45     ; "ER" (white on red)
    mov dword [0xb8004], 0x4f3a4f52     ; "R:"
    mov dword [0xb8008], 0x4f204f20     ; "  "
    mov byte  [0xb800a], al
    cli
.hang:
    hlt
    jmp .hang

; ------------------------------- 64-bit entry -------------------------------
section .text
bits 64
long_mode_start:
    ; Null out the data segment selectors — long mode ignores base/limit but
    ; the registers must hold valid (or null) selectors.
    xor ax, ax
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call load_idt                       ; install initial interrupt vector

    ; System V AMD64 ABI: rdi = arg0, rsi = arg1
    mov rdi, [mb_info_ptr]              ; arg0: Multiboot2 info pointer
    mov rsi, [mb_magic]                 ; arg1: Multiboot2 magic
    call kmain                          ; hand off to freestanding C kernel

.hang:
    cli
    hlt
    jmp .hang

;  Initial interrupt vector (ISR stubs 0..31)
;  Each stub normalizes the stack frame (push dummy error code where the CPU
;  does not) and jumps to a common trampoline that calls the C dispatcher
;  `isr_dispatch(struct isr_frame *)`.
extern isr_dispatch

; CPU exceptions 8,10,11,12,13,14,17 push a real error code; the rest do not.
%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0            ; dummy error code -> uniform frame
    push qword %1           ; interrupt number
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push qword %1           ; interrupt number (error code already on stack)
    jmp isr_common
%endmacro

ISR_NOERR 0    ; #DE divide error
ISR_NOERR 1    ; #DB debug
ISR_NOERR 2    ; NMI
ISR_NOERR 3    ; #BP breakpoint
ISR_NOERR 4    ; #OF overflow
ISR_NOERR 5    ; #BR bound range
ISR_NOERR 6    ; #UD invalid opcode
ISR_NOERR 7    ; #NM device not available
ISR_ERR   8    ; #DF double fault
ISR_NOERR 9    ; coprocessor segment overrun (legacy)
ISR_ERR   10   ; #TS invalid TSS
ISR_ERR   11   ; #NP segment not present
ISR_ERR   12   ; #SS stack fault
ISR_ERR   13   ; #GP general protection
ISR_ERR   14   ; #PF page fault
ISR_NOERR 15   ; reserved
ISR_NOERR 16   ; #MF x87 FP
ISR_ERR   17   ; #AC alignment check
ISR_NOERR 18   ; #MC machine check
ISR_NOERR 19   ; #XM SIMD FP
ISR_NOERR 20   ; #VE virtualization
ISR_NOERR 21   ; #CP control protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30   ; #SX security exception
ISR_NOERR 31

; Common trampoline: save GPRs, pass a pointer to the frame, restore, iretq.
isr_common:
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

    mov rdi, rsp            ; arg0 = &isr_frame (top of saved state)
    cld
    call isr_dispatch

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

    add rsp, 16             ; discard pushed int_no + error_code
    iretq

; --- Fill the IDT (in .bss, populated by C) then lidt -----------------------
; The 256-entry IDT storage lives in C (idt.c) so gate encoding stays readable;
; here we only export the stub addresses (isr0..isr31, above) and provide the
; lidt helper the C side calls back into.
global rogex_lidt
rogex_lidt:
    lidt [rdi]             ; rdi -> struct idt_ptr { u16 limit; u64 base; }
    ret

; load_idt: let the C kernel build the descriptors, then load them.
extern idt_install
load_idt:
    call idt_install       ; C builds gates from isrN table + calls rogex_lidt
    ret

; ------------------------------ Page tables ---------------------------------
section .bss
align 4096
p4_table:
    resb 4096
p3_table:
    resb 4096
p2_table:
    resb 4096 * PAGE_DIR_COUNT

; --------------------------------- GDT64 ------------------------------------
section .rodata
gdt64:
    dq 0                                        ; null descriptor
.code: equ $ - gdt64
    ; executable | descriptor type | present | 64-bit (L) flag
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)
.data: equ $ - gdt64
    dq (1 << 44) | (1 << 47) | (1 << 41)        ; present | writable data
.pointer:
    dw $ - gdt64 - 1
    dq gdt64
