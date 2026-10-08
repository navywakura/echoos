/* QEMU virt puts RAM at 0x40000000; the kernel sits where Linux would, at
   +0x80000, so the same image can later be handed to a UEFI loader that
   expects that convention. */
ENTRY(_start)
SECTIONS
{
    . = 0x40080000;
    _kernel_start = .;
    /* The image opens with a page of headers: the arm64 boot header a Linux
       loader reads, and the PE/COFF headers UEFI reads. One page, so that a
       PE section can begin at a 4 KiB boundary as the format requires, and
       so that RVA == file offset for everything after it — which is what
       lets the section table be written with plain symbol arithmetic. */
    .head : { KEEP(*(.head.text)) . = ALIGN(4096); }
    _head_end = .;
    .text : ALIGN(4096) { KEEP(*(.text.boot)) *(.text*) }
    .rodata : ALIGN(4096) { *(.rodata*) }
    /* Inside the section, after ALIGN: a PE section's VirtualAddress must be
       a multiple of SectionAlignment, and a symbol taken before the align
       records the unaligned dot. */
    .data : ALIGN(4096) { _data_start = .; *(.data*) }
    /* Before .bss on purpose: .bss occupies no file bytes, so anything
       placed after it would make objcopy pad the flat image by the whole
       BSS. */
    /* The bounds go inside the section: put outside, `_reloc_start` lands
       before ALIGN's padding and the directory then claims three kilobytes
       of zeros are relocations. The trailing pad is real file content, so
       that SizeOfRawData is a multiple of FileAlignment and never promises
       the firmware more bytes than the file holds. */
    .reloc : ALIGN(4096) {
        _reloc_start = .;
        KEEP(*(.reloc))
        _reloc_end = .;
        . = ALIGN(512);
    }
    _file_end = .;
    .bss : ALIGN(4096) {
        _bss_start = .;
        *(COMMON) *(.bss*)
        . = ALIGN(16);
        _bss_end = .;
    }
    . = ALIGN(4096);
    _kernel_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }

    /* The PE header has to state its own geometry, and an assembler cannot
       subtract two symbols the linker defines. Computing them here is the
       only place where both ends of every span are known. */
    _pe_size_of_code = _file_end - _head_end;
    _pe_bss_size     = _kernel_end - _file_end;
    _pe_entry_rva    = efi_entry - _kernel_start;
    _pe_base_of_code = _head_end - _kernel_start;
    _pe_image_size   = _kernel_end - _kernel_start;
    _pe_headers_size = _head_end - _kernel_start;
    _pe_text_vsize   = _kernel_end - _head_end;
    _pe_reloc_rva    = _reloc_start - _kernel_start;
    _pe_reloc_size   = _reloc_end - _reloc_start;
    /* The stub needs the same two spans at run time: how much to reserve,
       and how many bytes actually exist in the file to copy. */
    _pe_file_size    = _file_end - _kernel_start;
    /* Two sections, not one. UEFI applies memory attributes from the section
       characteristics: a single section marked as code is mapped read-only,
       and the stub's first write to its own scratch takes a permission
       fault before it has said a word. Code and data are therefore
       described separately, as PE intends. */
    _pe_text_rva     = _head_end - _kernel_start;
    _pe_text_size    = _data_start - _head_end;
    _pe_data_rva     = _data_start - _kernel_start;
    _pe_data_vsize   = _kernel_end - _data_start;
    _pe_data_rawsize = _file_end - _data_start;
}
