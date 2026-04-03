ENTRY(_start)

SECTIONS
{
    /* Load at the start of user space */
    . = 0x01000000;

    .text ALIGN(4K) :
    {
        *(.text)
    }

    .rodata ALIGN(4K) :
    {
        *(.rodata)
    }

    .data ALIGN(4K) :
    {
        *(.data)
    }

    .bss ALIGN(4K) :
    {
        *(.bss)
        *(COMMON)
    }

    /DISCARD/ :
    {
        *(.note.gnu.build-id)
        *(.comment)
        *(.eh_frame)
    }
}
