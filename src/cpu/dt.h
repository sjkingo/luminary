#pragma once

#include <stdint.h>

/* Descriptor table structures (GDT, IDT, TSS) and associated definitions. */

#define GDT_NUM_ENTRIES 6

#define GDT_GATE_KERNEL_CODE 0x9A
#define GDT_GATE_KERNEL_DATA 0x92
#define GDT_GATE_USER_CODE   0xFA
#define GDT_GATE_USER_DATA   0xF2

/* Segment selectors (GDT index * 8, plus RPL bits) */
#define SEG_KERNEL_CODE  0x08   /* GDT entry 1 */
#define SEG_KERNEL_DATA  0x10   /* GDT entry 2 */
#define SEG_USER_CODE    0x1B   /* GDT entry 3, RPL=3 */
#define SEG_USER_DATA    0x23   /* GDT entry 4, RPL=3 */
#define SEG_TSS          0x2B   /* GDT entry 5, RPL=3 */

/* An entry in the GDT. Structure is described at http://wiki.osdev.org/File:GDT_Entry.png */
struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char base_middle;
    unsigned char access;
    unsigned char granularity;
    unsigned char base_high;
} __attribute__((packed));

/* The GDT structure. Described at http://wiki.osdev.org/File:Gdtr.png */
struct gdt_ptr {
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));


#define IDT_KERNEL_SEG 0x8

/* An entry in the IDT. */
struct idt_entry {
    unsigned short base_low;
    unsigned short selector;
    unsigned char always0;
    unsigned char flags;
    unsigned short base_high;
} __attribute__((packed));

/* The IDT structure. */
struct idt_ptr {
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));

/* Task State Segment. Only esp0/ss0 are used for ring transitions. */
struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;      /* kernel stack pointer for ring 0 */
    uint32_t ss0;       /* kernel stack segment for ring 0 */
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

/* Set the kernel stack pointer in the TSS (called on context switch) */
void tss_set_kernel_stack(uint32_t esp0);
