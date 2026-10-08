/* AArch64 entry.
 *
 * The image opens with the arm64 Linux boot header. It is not decoration:
 * a loader that does not recognise it (QEMU handed a bare ELF, for one)
 * jumps straight to the entry point with the registers zeroed and no device
 * tree, which is how this port spent a while inventing its own memory map.
 * With the header, QEMU and U-Boot both place the image at RAM+text_offset
 * and hand the device tree over in x0, the way a real board does.
 *
 * The loader drops us at EL1 (or EL2 on some machine types) with the
 * device tree in x0 and everything else undefined. The job here is the
 * minimum that C can rely on: a stack, a zeroed BSS, and the boot
 * information kept somewhere the handoff can find it.
 *
 * AAPCS64: x0-x7 arguments, x29 frame pointer, x30 link register, sp
 * 16-byte aligned at every public entry. Nothing below breaks that.
 */
/* The header page: arm64 boot header + PE/COFF, so one file is both a Linux
   arm64 image and a UEFI application.
 *
 * The trick that makes it possible is the first instruction. UEFI insists a
 * PE file begin with the two bytes "MZ"; AArch64 insists the image begin
 * with an instruction. `ccmp x18, #0, #0xd, pl` is a real instruction with no
 * effect whose encoding happens to start with those two bytes, so both
 * loaders are satisfied by the same four bytes. The PE header offset lives
 * at 0x3c, which is exactly the word the arm64 header reserves.
 */
.section .head.text, "ax"
.global _start
_start:
    ccmp    x18, #0, #0xd, pl           /* code0: also the "MZ" signature   */
    b       _start_real                 /* code1: the flat-image entry      */
    .quad   0x00080000                  /* text_offset from the RAM base    */
    .quad   _kernel_end - _start        /* image_size, BSS included         */
    .quad   0                           /* flags: LE, 4 KiB pages           */
    .quad   0                           /* res2                             */
    .quad   0                           /* res3                             */
    .quad   0                           /* res4                             */
    .ascii  "ARM\x64"                   /* magic                            */
    .long   pe_header - _start          /* res5: where UEFI looks           */

pe_header:
    .ascii  "PE\0\0"
    .short  0xAA64                      /* Machine: AArch64                 */
    .short  2                           /* NumberOfSections                 */
    .long   0                           /* TimeDateStamp: zero, so two
                                           builds of the same source give
                                           the same bytes                   */
    .long   0                           /* PointerToSymbolTable             */
    .long   0                           /* NumberOfSymbols                  */
    .short  section_table - optional_header
    .short  0x0206                      /* EXECUTABLE | LINE_NUMS_STRIPPED
                                           | DEBUG_STRIPPED                 */
optional_header:
    .short  0x020b                      /* PE32+                            */
    .byte   0, 0                        /* linker version                   */
    .long   _pe_text_size               /* SizeOfCode                       */
    .long   _pe_data_rawsize            /* SizeOfInitializedData            */
    .long   _pe_bss_size                /* SizeOfUninitializedData          */
    .long   _pe_entry_rva               /* AddressOfEntryPoint              */
    .long   _pe_base_of_code             /* BaseOfCode                       */

    .quad   0x40080000                  /* ImageBase: where we are linked   */
    .long   4096                        /* SectionAlignment                 */
    .long   512                         /* FileAlignment                    */
    .short  0, 0                        /* OS version                       */
    .short  0, 0                        /* image version                    */
    .short  0, 0                        /* subsystem version                */
    .long   0                           /* Win32VersionValue                */
    .long   _pe_image_size              /* SizeOfImage                      */
    .long   _pe_headers_size            /* SizeOfHeaders                    */
    .long   0                           /* CheckSum: firmware does not check */
    .short  10                          /* Subsystem: EFI application       */
    .short  0                           /* DllCharacteristics               */
    .quad   0x10000                     /* SizeOfStackReserve               */
    .quad   0x10000                     /* SizeOfStackCommit                */
    .quad   0                           /* SizeOfHeapReserve                */
    .quad   0                           /* SizeOfHeapCommit                 */
    .long   0                           /* LoaderFlags                      */
    .long   6                           /* NumberOfRvaAndSizes              */
    .quad   0                           /* 0 export                         */
    .quad   0                           /* 1 import                         */
    .quad   0                           /* 2 resource                       */
    .quad   0                           /* 3 exception                      */
    .quad   0                           /* 4 certificate                    */
    /* 5 base relocations. The table is a single do-nothing entry: the image
       carries no absolute relocations because the stub that runs before the
       copy is position-independent by construction. Declaring it anyway is
       what stops the firmware treating the image as fixed-address and
       refusing to load it where it has room. */
    .long   _pe_reloc_rva
    .long   _pe_reloc_size

section_table:
    .ascii  ".text\0\0\0"
    .long   _pe_text_size               /* VirtualSize                      */
    .long   _pe_text_rva                /* VirtualAddress                   */
    .long   _pe_text_size               /* SizeOfRawData                    */
    .long   _pe_text_rva                /* PointerToRawData                 */
    .long   0, 0                        /* relocations, line numbers        */
    .short  0, 0
    .long   0x60000020                  /* CODE | EXECUTE | READ            */

    .ascii  ".data\0\0\0"
    .long   _pe_data_vsize              /* VirtualSize (BSS included)       */
    .long   _pe_data_rva                /* VirtualAddress                   */
    .long   _pe_data_rawsize            /* SizeOfRawData                    */
    .long   _pe_data_rva                /* PointerToRawData                 */
    .long   0, 0                        /* relocations, line numbers        */
    .short  0, 0
    .long   0xC0000040                  /* INITIALIZED_DATA | READ | WRITE  */

.section .reloc, "a"
reloc_blob:
    .long   0                           /* Page RVA                         */
    .long   reloc_blob_end - reloc_blob /* Block size                       */
    .short  0                           /* one ABSOLUTE entry: a no-op      */
    .short  0                           /* padding to a multiple of four    */
reloc_blob_end:

.section .text.boot, "ax"
_start_real:
    /* First instruction, before anything can need a scratch register: x0 and
       x1 describe the boot and the stack setup below uses x1. Saving them
       later — which this did — loses the magic between the two. */
    mov     x20, x0
    mov     x21, x1

    /* If we came in at EL2, drop to EL1: the kernel runs where the MMU and
       the generic timer are the ones we programme. */
    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    cmp     x1, #2
    b.ne    1f

    mov     x2, #(1 << 31)              /* EL1 is AArch64                   */
    msr     hcr_el2, x2
    mov     x2, #0x3c5                  /* EL1h, DAIF masked                */
    msr     spsr_el2, x2
    adr     x2, 1f
    msr     elr_el2, x2
    eret

1:  /* Stack first: nothing else may run without one. */
    ldr     x1, =_stack_top
    mov     sp, x1

    /* Zero the BSS. C assumes it. */
    ldr     x1, =_bss_start
    ldr     x2, =_bss_end
2:  cmp     x1, x2
    b.hs    3f
    str     xzr, [x1], #8
    b       2b

3:  /* The MMU before any C: with it off, every access is Device memory and
       an unaligned one aborts, which is what killed this port's first boot.
       x0 and x1 describe the boot and must survive the call: x0 is a device
       tree from a Linux-style loader, or a handoff structure from the UEFI
       stub, and x1 says which. The arm64 boot protocol requires x1 to be
       zero in the first case, so the two cannot be mistaken for each other.
       Both were saved at _start_real; x19-x28 survive a C call. */
    bl      aarch64_mmu_init
    mov     x0, x20
    mov     x1, x21
    bl      kmain

4:  wfe                                 /* kmain does not return            */
    b       4b

.section .bss
.align 12
.global _stack_bottom
_stack_bottom:
    .space 0x10000                      /* 64 KiB boot stack                */
.global _stack_top
_stack_top:
