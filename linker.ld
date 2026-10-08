/* ==========================================================================
 *  Rogex OS — kernel link script
 *  Freestanding x86_64 ELF, loaded by a Multiboot2 loader at 1 MiB.
 * ========================================================================== */
ENTRY(_start)

SECTIONS
{
    /* Conventional load address for a Multiboot2 kernel. */
    . = 1M;

    /* Multiboot2 header MUST live in the first 32 KiB of the file and be
       8-byte aligned, so it leads the image. */
    .boot :
    {
        KEEP(*(.multiboot_header))
    }

    .text :
    {
        *(.text .text.*)
    }

    .rodata :
    {
        *(.rodata .rodata.*)
    }

    .data :
    {
        *(.data .data.*)
    }

    .bss :
    {
        bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        *(.bootstrap_stack)
        *(.boot_data)
    }

    /* End of the loaded image. The physical memory manager starts allocating
       above this so it can never hand out a frame the kernel is sitting in —
       on a real machine the image is bigger than the 2 MiB it used to assume. */
    . = ALIGN(4K);
    _kernel_end = .;

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.comment)
        *(.note .note.*)
    }
}
