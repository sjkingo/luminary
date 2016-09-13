#include "multiboot.h"

/* Note the linker script collapses this into .text, but guarantees
 * it will be in the first 8K of the kernel binary.
 */
.section .mbhdr
    .long MULTIBOOT_MAGIC
    .long MULTIBOOT_FLAGS
    .long MULTIBOOT_CHECKSUM
    /* tell multiboot we want VBE initialised and hardcode the resolution */
    .long 0, 0, 0, 0, 0
    .long 0
    .long 1024, 768, 32


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
