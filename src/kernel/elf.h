#pragma once

#include <stdint.h>

/* Minimal ELF32 definitions for loading static executables. */

#define ELF_MAGIC 0x464C457F  /* "\x7FELF" as little-endian uint32 */

/* ELF header */
struct elf32_header {
    uint32_t e_ident_magic;     /* must be ELF_MAGIC */
    uint8_t  e_ident_class;     /* 1 = 32-bit */
    uint8_t  e_ident_data;      /* 1 = little-endian */
    uint8_t  e_ident_version;
    uint8_t  e_ident_osabi;
    uint8_t  e_ident_pad[8];
    uint16_t e_type;            /* 2 = ET_EXEC */
    uint16_t e_machine;         /* 3 = EM_386 */
    uint32_t e_version;
    uint32_t e_entry;           /* virtual address of entry point */
    uint32_t e_phoff;           /* program header table offset */
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;           /* number of program headers */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

/* Program header */
struct elf32_phdr {
    uint32_t p_type;            /* 1 = PT_LOAD */
    uint32_t p_offset;          /* offset in file */
    uint32_t p_vaddr;           /* virtual address */
    uint32_t p_paddr;
    uint32_t p_filesz;          /* size in file */
    uint32_t p_memsz;           /* size in memory (may be > filesz for .bss) */
    uint32_t p_flags;           /* PF_X=1, PF_W=2, PF_R=4 */
    uint32_t p_align;
};

#define ET_EXEC     2
#define EM_386      3
#define PT_LOAD     1
#define PF_W        2

/* Load an ELF binary into a task's address space.
 * elf_data: pointer to the ELF file in memory
 * elf_size: size of the ELF file
 * page_dir: physical address of the task's page directory
 * Returns the entry point virtual address, or 0 on failure. */
uint32_t elf_load(const void *elf_data, uint32_t elf_size, uint32_t page_dir);
