; RXos BIOS MBR (stage1). Concatenated with hdboot_s2.bin by the Makefile.
; Firmware loads this at 0000:7C00. We load LBA 1..16 to 0000:8000 and jump.

BITS 16
ORG 0x7C00

STAGE2_LBA     equ 1
STAGE2_SECTS   equ 16
STAGE2_OFF     equ 0x8000

stage1:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti
    mov     [boot_drv], dl

    mov     si, dap
    mov     ah, 0x42
    int     0x13
    jc      .fail
    jmp     0x0000:STAGE2_OFF

.fail:
    mov     si, s1msg
.p: lodsb
    test    al, al
    jz      .h
    mov     ah, 0x0E
    mov     bx, 0x0007
    int     0x10
    jmp     .p
.h: hlt
    jmp     .h

boot_drv: db 0

    align 4
dap:
    db 16, 0
    dw STAGE2_SECTS
    dw STAGE2_OFF, 0
    dq STAGE2_LBA

s1msg: db "RXos: disk read error", 13, 10, 0

    times 446 - ($ - $$) db 0
    times 64 db 0
    dw 0xAA55
