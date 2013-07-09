ENTRY(multiboot_entry) /* in boot.s */

SECTIONS
{
    /* Load the kernel at 1M of lower memory */
    . = 1M;
    _kernel_start = .;

    /* Multiboot header and code */
    .text ALIGN(4K) :
    {
        *(.mbhdr) /* must be within first 8K */
        *(.text)
    }

    /* Read-only data */
    .rodata ALIGN(4K) :
    {
        *(.rodata)
    }

    /* Read-write data (initialized) */
    .data ALIGN(4K) :
    {
        *(.data)
    }

    /* Read-write data (uninitialized) */
    .bss :
    {
        sbss = .;
        *(COMMON)
        *(.bss)
        ebss = .;
    }

    /* Declare the end of the kernel */
    _kernel_end = .;

    /* Throw away some sections */
    /DISCARD/ :
    {
        *(.note.gnu.build-id)
        *(.comment)
    }
}
