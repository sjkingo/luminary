.section .text

/* Load the GDT defined in cpu.c and flush the segment descriptors */
.extern gptr
.globl gdt_flush
gdt_flush:
    lgdt gptr
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    ljmp $0x08, $_gdt_flush_ret
_gdt_flush_ret:
    ret
