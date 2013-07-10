#ifndef GDT_H
#define GDT_H

#define GDT_NUM_ENTRIES 3

#define GDT_GATE_KERNEL_CODE 0x9A
#define GDT_GATE_KERNEL_DATA 0x92

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

#endif
