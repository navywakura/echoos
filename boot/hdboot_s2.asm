; RXos BIOS stage2. Loaded at 0000:8000 by the MBR.
; E820 + VESA LFB + RXBT kernel load + Multiboot2 handoff to 1 MiB.

BITS 16
ORG 0x8000

HDR_LBA        equ 17
KERN_BUF       equ 0x10000
KERN_DEST      equ 0x100000
MB2_BASE       equ 0x5000
E820_BASE      equ 0x2000
VBE_INFO       equ 0x3000
VBE_MODE       equ 0x3400
MODELIST       equ 0x3600
MODELIST_MAX   equ 128

stage2:
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    mov     [s2_drv], dl                ; BIOS boot drive, still in DL

    call    enable_a20
    call    collect_e820
    call    load_kernel            ; disk I/O before VBE (some BIOSes break INT 13 after)
    call    set_vesa
    call    build_mb2
    jmp     enter_pm

s2_drv: db 0

enable_a20:
    in      al, 0x92
    or      al, 2
    and     al, 0xFE
    out     0x92, al
    ret

collect_e820:
    xor     ebx, ebx
    mov     di, E820_BASE
    mov     word [e820_count], 0
.loop:
    mov     eax, 0xE820
    mov     edx, 0x534D4150
    mov     ecx, 24
    int     0x15
    jc      .done
    cmp     eax, 0x534D4150
    jne     .done
    add     di, 24
    inc     word [e820_count]
    cmp     word [e820_count], 32
    jae     .done
    test    ebx, ebx
    jnz     .loop
.done:
    ret

e820_count: dw 0

set_vesa:
    mov     word [fb_w], 0
    mov     ax, 0x4F00
    mov     di, VBE_INFO
    mov     dword [di], 0x32454256
    int     0x10
    cmp     ax, 0x004F
    jne     .fail

    push    ds
    push    es
    mov     ax, [VBE_INFO + 16]
    mov     ds, ax
    mov     si, [VBE_INFO + 14]
    xor     ax, ax
    mov     es, ax
    mov     di, MODELIST
    mov     cx, MODELIST_MAX
.cplp:
    lodsw
    stosw
    cmp     ax, 0xFFFF
    je      .cpdone
    loop    .cplp
.cpdone:
    pop     es
    pop     ds

    mov     si, pref_modes
.try:
    mov     cx, [si]
    mov     dx, [si + 2]
    add     si, 4
    test    cx, cx
    jz      .fail
    push    si
    call    find_mode
    pop     si
    jc      .try
    or      bx, 0x4000
    mov     ax, 0x4F02
    int     0x10
    cmp     ax, 0x004F
    jne     .try
    ret
.fail:
    mov     word [fb_w], 0
    ret

find_mode:
    mov     bp, MODELIST
.walk:
    mov     bx, [bp]
    add     bp, 2
    cmp     bx, 0xFFFF
    je      .nomode
    test    bx, bx
    jz      .nomode
    mov     [cand_mode], bx
    push    cx
    push    dx
    mov     cx, bx
    mov     ax, 0x4F01
    mov     di, VBE_MODE
    int     0x10
    pop     dx
    pop     cx
    cmp     ax, 0x004F
    jne     .walk
    test    word [VBE_MODE], 0x81
    jz      .walk
    cmp     byte [VBE_MODE + 25], 32
    jne     .walk
    cmp     [VBE_MODE + 18], cx
    jne     .walk
    cmp     [VBE_MODE + 20], dx
    jne     .walk
    mov     [fb_w], cx
    mov     [fb_h], dx
    mov     ax, [VBE_MODE + 16]
    mov     [fb_pitch], ax
    mov     eax, [VBE_MODE + 40]
    mov     [fb_addr], eax
    mov     dword [fb_addr + 4], 0
    mov     bx, [cand_mode]
    clc
    ret
.nomode:
    stc
    ret

pref_modes:
    dw 1366, 768
    dw 1360, 768
    dw 1280, 720
    dw 1280, 800
    dw 1024, 768
    dw 800,  600
    dw 640,  480
    dw 0, 0

fb_w:      dw 0
fb_h:      dw 0
fb_pitch:  dw 0
fb_addr:   dq 0
cand_mode: dw 0

load_kernel:
    mov     eax, HDR_LBA
    mov     bx, 0x4000
    xor     si, si                      ; 0000:4000
    mov     cx, 1
    call    read_lba
    jc      disk_fail
    cmp     dword [0x4000], 0x54425852
    jne     disk_fail
    mov     eax, [0x4004]
    cmp     eax, 1
    jne     disk_fail
    mov     eax, [0x4008]
    mov     [k_lba], eax
    mov     eax, [0x400C]
    mov     [k_sects], eax
    test    eax, eax
    jz      disk_fail
    mov     eax, [0x4014]              ; entry
    test    eax, eax
    jnz     .gotentry
    mov     eax, KERN_DEST
.gotentry:
    mov     [k_entry], eax

    call    enter_unreal
    cld

    mov     dword [k_dest], KERN_DEST
.next_chunk:
    mov     eax, [k_sects]
    test    eax, eax
    jz      .done
    cmp     eax, 32
    jbe     .use
    mov     eax, 32
.use:
    mov     [chunk], eax
    mov     eax, [k_lba]
    xor     bx, bx
    mov     si, 0x1000                  ; 1000:0000 = 64 KiB bounce
    mov     cx, [chunk]
    call    read_lba
    jc      disk_fail
    push    ds
    push    es
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     esi, KERN_BUF
    mov     edi, [k_dest]
    mov     ecx, [chunk]
    shl     ecx, 9
    a32 rep movsb
    pop     es
    pop     ds
    mov     eax, [chunk]
    add     [k_lba], eax
    sub     [k_sects], eax
    shl     eax, 9
    add     [k_dest], eax
    jmp     .next_chunk
.done:
    ret

k_lba:   dd 0
k_sects: dd 0
k_dest:  dd 0
k_entry: dd KERN_DEST
chunk:   dd 0

disk_fail:
    mov     si, s2msg
.p: lodsb
    test    al, al
    jz      .h
    mov     ah, 0x0E
    mov     bx, 0x0007
    int     0x10
    jmp     .p
.h: hlt
    jmp     .h

s2msg: db "RXos: cannot load kernel", 13, 10, 0

read_lba:
    mov     [rdap_lba], eax
    mov     [rdap_off], bx
    mov     [rdap_seg], si
    mov     [rdap_cnt], cx
    mov     si, rdap
    mov     dl, [s2_drv]
    mov     ah, 0x42
    int     0x13
    ret

    align 4
rdap:
    db 16, 0
rdap_cnt: dw 0
rdap_off: dw 0
rdap_seg: dw 0
rdap_lba: dq 0

enter_unreal:
    cli
    lgdt    [gdtr]
    mov     eax, cr0
    or      al, 1
    mov     cr0, eax
    mov     bx, 0x08
    mov     ds, bx
    mov     es, bx
    and     al, 0xFE
    mov     cr0, eax
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    sti
    ret

build_mb2:
    mov     di, MB2_BASE
    add     di, 8
    mov     eax, 6
    stosd
    movzx   eax, word [e820_count]
    mov     ecx, 24
    mul     ecx
    add     eax, 16
    stosd
    mov     eax, 24
    stosd
    xor     eax, eax
    stosd
    mov     si, E820_BASE
    mov     cx, [e820_count]
    test    cx, cx
    jz      .fake
.copy_mm:
    push    cx
    mov     cx, 24
    rep     movsb
    pop     cx
    loop    .copy_mm
    jmp     .align_mm
.fake:
    xor     eax, eax
    stosd
    mov     eax, 0x00100000
    stosd
    xor     eax, eax
    stosd
    mov     eax, 0x01F00000
    stosd
    mov     eax, 1
    stosd
    xor     eax, eax
    stosd
    mov     dword [MB2_BASE + 8 + 4], 40
.align_mm:
    add     di, 7
    and     di, 0xFFF8

    cmp     word [fb_w], 0
    je      .endtag
    mov     eax, 8
    stosd
    mov     eax, 32
    stosd
    mov     eax, [fb_addr]
    stosd
    mov     eax, [fb_addr + 4]
    stosd
    movzx   eax, word [fb_pitch]
    stosd
    movzx   eax, word [fb_w]
    stosd
    movzx   eax, word [fb_h]
    stosd
    mov     al, 32
    stosb
    mov     al, 1
    stosb
    xor     ax, ax
    stosw
    add     di, 7
    and     di, 0xFFF8

.endtag:
    xor     eax, eax
    stosd
    mov     eax, 8
    stosd
    movzx   eax, di
    sub     eax, MB2_BASE
    mov     [MB2_BASE], eax
    mov     dword [MB2_BASE + 4], 0
    ret

enter_pm:
    cli
    lgdt    [gdtr]
    mov     eax, cr0
    or      al, 1
    mov     cr0, eax
    jmp     0x10:pm32

BITS 32
pm32:
    mov     ax, 0x08
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     esp, 0x90000
    mov     eax, 0x36d76289
    mov     ebx, MB2_BASE
    mov     edx, [k_entry]
    jmp     edx

BITS 16
    align 8
gdt:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
gdt_end:
gdtr:
    dw gdt_end - gdt - 1
    dd gdt
