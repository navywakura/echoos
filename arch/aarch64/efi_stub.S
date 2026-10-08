/* UEFI entry for the AArch64 image.
 *
 * The firmware loads this image wherever it has room, but the kernel is
 * linked absolute at 0x40080000 — so before a single line of that kernel can
 * run, someone has to put it there. That someone cannot use an absolute
 * address itself, which is why this is assembly: every reference below is
 * PC-relative (adrp/adr) or comes out of a table the firmware handed us, so
 * the code runs correctly at whatever address it was loaded.
 *
 * The sequence is the one UEFI mandates and nothing more: read the device
 * tree from the configuration table, reserve the kernel's real address,
 * leave boot services, then move the image and jump. After
 * ExitBootServices() no firmware call is legal, so everything that needs the
 * firmware happens before it.
 */
.section .text, "ax"

/* EFI_SYSTEM_TABLE */
#define ST_CONOUT        64
#define ST_NR_TABLES    104
#define ST_CONFIG_TABLE 112
/* EFI_BOOT_SERVICES */
#define BS_ALLOCATE_PAGES  40
#define BS_GET_MEMORY_MAP  56
#define BS_ALLOCATE_POOL   64
#define BS_EXIT_BOOT_SVCS 232
#define ST_BOOTSERVICES    96
/* EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL */
#define OUT_OUTPUT_STRING   8

#define ALLOCATE_ADDRESS  2
#define EFI_LOADER_DATA   2

#define KERNEL_BASE 0x40080000
#define EFI_MAP_BYTES 16384
/* "ECHOSEFI": tells the kernel that x0 is a UEFI handoff and not a device
   tree. The arm64 boot protocol requires x1 to be zero from a Linux-style
   loader, so the two paths cannot be confused. */
#define ECHOS_EFI_MAGIC 0x4543484F53454649

.global efi_entry
efi_entry:
    /* x0 = image handle, x1 = system table. Keep both: ExitBootServices
       needs the handle, and everything else hangs off the table. */
    stp     x29, x30, [sp, #-96]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]

    mov     x19, x0                     /* image handle                     */
    mov     x20, x1                     /* system table                     */
    ldr     x21, [x20, #ST_BOOTSERVICES]
    mov     x22, xzr                    /* device tree, if we find one      */
    adrp    x0, systab_slot
    add     x0, x0, :lo12:systab_slot
    str     x20, [x0]
    mov     x0, x19

    adr     x0, msg_hello
    bl      efi_puts

    /* --- the device tree, from the configuration table -------------------
       UEFI does not hand a DTB in a register the way a Linux loader does;
       it publishes it in a table keyed by GUID. Without this the kernel
       would have to guess its own memory map. */
    ldr     x23, [x20, #ST_NR_TABLES]
    ldr     x24, [x20, #ST_CONFIG_TABLE]
    adr     x25, fdt_guid
    ldp     x26, x27, [x25]
1:  cbz     x23, 3f
    ldp     x0, x1, [x24]
    cmp     x0, x26
    b.ne    2f
    cmp     x1, x27
    b.ne    2f
    ldr     x22, [x24, #16]             /* VendorTable = the DTB            */
    b       3f
2:  add     x24, x24, #24
    sub     x23, x23, #1
    b       1b
3:
    cbnz    x22, 4f
    adr     x0, msg_no_fdt
    bl      efi_puts
4:

    /* --- reserve the address the kernel is linked at --------------------
       If the firmware already put us there, there is nothing to reserve and
       nothing to copy. Otherwise the range has to be ours before boot
       services end, because afterwards there is no allocator to ask. */
    adrp    x25, _start
    add     x25, x25, :lo12:_start      /* where we are actually running    */
    ldr     x26, =KERNEL_BASE
    cmp     x25, x26
    b.eq    6f

    adrp    x0, alloc_addr
    add     x0, x0, :lo12:alloc_addr
    str     x26, [x0]
    mov     x3, x0
    mov     x0, #ALLOCATE_ADDRESS
    mov     x1, #EFI_LOADER_DATA
    ldr     x2, =_pe_image_size
    add     x2, x2, #4095
    lsr     x2, x2, #12
    ldr     x4, [x21, #BS_ALLOCATE_PAGES]
    blr     x4
    cbz     x0, 6f
    adr     x0, msg_no_pages
    bl      efi_puts
    b       hang
6:

    /* --- the memory map -------------------------------------------------
       Needed twice over: ExitBootServices will not accept a key from a map
       we have not just read, and this firmware publishes no device tree, so
       under UEFI this *is* where the kernel's memory map comes from. The
       buffer is static and inside the image, so it travels with the copy
       and is still readable once the firmware is gone. */
    mov     x24, #4                     /* attempts                         */
8:  adrp    x0, map_size
    add     x0, x0, :lo12:map_size
    mov     x1, #EFI_MAP_BYTES
    str     x1, [x0]
    adrp    x1, efi_map_buf
    add     x1, x1, :lo12:efi_map_buf
    adrp    x2, map_key
    add     x2, x2, :lo12:map_key
    adrp    x3, desc_size
    add     x3, x3, :lo12:desc_size
    adrp    x4, desc_ver
    add     x4, x4, :lo12:desc_ver
    ldr     x5, [x21, #BS_GET_MEMORY_MAP]
    blr     x5
    cbnz    x0, 7f

    mov     x0, x19
    adrp    x1, map_key
    add     x1, x1, :lo12:map_key
    ldr     x1, [x1]
    ldr     x2, [x21, #BS_EXIT_BOOT_SVCS]
    blr     x2
    cbz     x0, 9f
    /* The map moved between our read and the call, which UEFI allows and
       expects us to handle by reading it again. */
    subs    x24, x24, #1
    b.ne    8b
7:  adr     x0, msg_no_exit
    bl      efi_puts
    b       hang
9:

    /* --- from here the firmware no longer exists -------------------------
       Describe the handoff while still running at the loaded address, so the
       copy carries a filled-in structure. Addresses recorded here are the
       ones the kernel will see, which is the load address plus the offset of
       each field inside the image. */
    adrp    x23, _start
    add     x23, x23, :lo12:_start      /* runtime image base               */
    ldr     x24, =KERNEL_BASE

    adrp    x5, efi_handoff
    add     x5, x5, :lo12:efi_handoff
    ldr     x6, =ECHOS_EFI_MAGIC
    str     x6, [x5]
    adrp    x6, efi_map_buf
    add     x6, x6, :lo12:efi_map_buf
    sub     x6, x6, x23
    add     x6, x6, x24                 /* the map, at its final address    */
    str     x6, [x5, #8]
    adrp    x6, map_size
    add     x6, x6, :lo12:map_size
    ldr     x6, [x6]
    str     x6, [x5, #16]
    adrp    x6, desc_size
    add     x6, x6, :lo12:desc_size
    ldr     x6, [x6]
    str     x6, [x5, #24]
    cbz     x22, 13f
    str     x22, [x5, #32]              /* a device tree, if one was found  */
13:

    adrp    x0, _start
    add     x0, x0, :lo12:_start        /* source                           */
    ldr     x1, =KERNEL_BASE            /* destination                      */
    cmp     x0, x1
    b.eq    11f
    ldr     x2, =_pe_file_size          /* only the bytes that exist        */
10: ldp     x3, x4, [x0], #16
    stp     x3, x4, [x1], #16
    subs    x2, x2, #16
    b.hi    10b

    /* The copy went through the data cache; the next stage will run with
       the MMU off, where accesses do not look in that cache. Clean it out,
       then throw away instructions fetched from the old image. */
    ldr     x0, =KERNEL_BASE
    ldr     x2, =_pe_file_size
    add     x2, x0, x2
    mrs     x3, ctr_el0
    ubfx    x3, x3, #16, #4             /* log2 words per line              */
    mov     x4, #4
    lsl     x4, x4, x3                  /* line size in bytes               */
    sub     x5, x4, #1
    bic     x0, x0, x5
12: dc      civac, x0
    add     x0, x0, x4
    cmp     x0, x2
    b.lo    12b
    dsb     sy
    ic      iallu
    dsb     sy
    isb
11:

    /* UEFI runs with its own page tables. The kernel installs its own and
       expects to start with the MMU off, so hand it the machine in the
       state it was written for. */
    mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 0)           /* M: address translation           */
    bic     x0, x0, #(1 << 2)           /* C: data cache                    */
    bic     x0, x0, #(1 << 12)          /* I: instruction cache             */
    msr     sctlr_el1, x0
    isb
    tlbi    vmalle1
    dsb     nsh
    isb

    /* The kernel takes a pointer and a magic. Under UEFI the pointer is
       this structure; from a Linux-style loader it is a device tree and the
       magic is zero, which the arm64 boot protocol guarantees. */
    adrp    x0, efi_handoff
    add     x0, x0, :lo12:efi_handoff
    sub     x0, x0, x23
    add     x0, x0, x24
    ldr     x1, =ECHOS_EFI_MAGIC
    ldr     x2, =KERNEL_BASE
    br      x2

hang:
    wfi
    b       hang

/* Print a NUL-terminated ASCII string as UTF-16, which is what UEFI speaks.
   Only used before ExitBootServices, and only to say what went wrong. The
   system table comes from memory rather than a register so that this can be
   called from anywhere in the stub without reserving one. */
efi_puts:
    stp     x29, x30, [sp, #-32]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    mov     x19, x0
    adrp    x20, wbuf
    add     x20, x20, :lo12:wbuf
    mov     x1, x20
1:  ldrb    w2, [x19], #1
    strh    w2, [x1], #2
    cbnz    w2, 1b
    adrp    x0, systab_slot
    add     x0, x0, :lo12:systab_slot
    ldr     x0, [x0]
    cbz     x0, 2f
    ldr     x0, [x0, #ST_CONOUT]
    cbz     x0, 2f
    mov     x1, x20
    /* ConOut already points at the protocol structure; OutputString is a
       field inside it, not an entry in a table it points to. The extra
       dereference loads the first instruction of some function and branches
       to it as if it were an address. */
    ldr     x2, [x0, #OUT_OUTPUT_STRING]
    blr     x2
2:  ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret

    .align 3
fdt_guid:
    .quad   0x41A5F19CB1B621D5
    .quad   0xE0AA692C15D90B83

msg_hello:    .asciz "echOS: UEFI entry\r\n"
msg_no_fdt:   .asciz "echOS: no device tree in the UEFI configuration table\r\n"
msg_no_pages: .asciz "echOS: could not reserve 0x40080000\r\n"
msg_no_exit:  .asciz "echOS: ExitBootServices refused\r\n"

/* Scratch, in .data rather than .bss: the stub runs before anything has
   zeroed a BSS, and three hundred bytes in the file is a cheaper answer than
   a second zeroing path. */
.section .data.efistub, "aw"
    .align 3
systab_slot: .space 8
    .align 3
    .global efi_handoff
efi_handoff:  .space 40      /* magic, map, bytes, desc_size, dtb           */
efi_map_buf:  .space EFI_MAP_BYTES
alloc_addr: .space 8
pool_ptr:   .space 8
map_size:   .space 8
map_key:    .space 8
desc_size:  .space 8
desc_ver:   .space 8
wbuf:       .space 256
