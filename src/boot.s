/* Multiboot header definitions
 * https://www.gnu.org/software/grub/manual/multiboot/multiboot.html#Header-magic-fields
 */
.set MAGIC, 0x1badb002
.set FLAGS, (1<<0 | 1<<1)
.set CHECKSUM, -(MAGIC + FLAGS)


/* Note the linker script collapses this into .text, but guarantees
 * it will be in the first 8K of the kernel binary.
 */
.section .mbhdr
    .long MAGIC
    .long FLAGS
    .long CHECKSUM

/* Create a 16K stack for the kernel to use (placed at the bottom of kernel image) */
.globl STACKSIZE
.set STACKSIZE, 16384
.section .kstack, "wa"
    kstack:
    .skip STACKSIZE


.section .text

/* Call the kernel proper */
.globl multiboot_entry
multiboot_entry:
    cli

    movl $kstack + STACKSIZE, %esp

    push %ebx # multiboot struct
    call kernel_main

    /* kernel returned - this shouldn't happen */
    push $kernel_returned
    call printk
    jmp cpu_halt

/* Halt the CPU */
.globl cpu_halt
cpu_halt:
    push $halting
    call printk
    jmp hltloop 
hltloop:
    cli
    hlt
    jmp hltloop


/* string messages */
.section .data
halting:
    .asciz "\n\nCPU halt\n"
kernel_returned:
    .asciz "\n\nBUG: kernel_main() returned?\n"
