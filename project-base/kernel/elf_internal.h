/* kernel/elf_internal.h - shared by the ELF check (elf.c, elf_sections.c)
 * and the relocations (reloc.c). Not used outside the loader. */
#ifndef ELF_INTERNAL_H
#define ELF_INTERNAL_H

#include <stdint.h>

#include "kernel/loader.h"

/* ------------------------------------------------------------------ */
/* ELF constants (only the ones we use)                               */
/* ------------------------------------------------------------------ */

/* Record sizes required by the assignment. */
#define ELF_EHDR_SIZE 52u
#define ELF_PHDR_SIZE 32u
#define ELF_SHDR_SIZE 40u
#define ELF_REL_SIZE 8u
#define ELF_SYM_SIZE 16u

/* Offsets of the ELF header fields. */
#define EH_CLASS 4u  /* 1 = 32 bit */
#define EH_DATA 5u  /* 1 = little-endian */
#define EH_VERSION 6u  /* 1 */
#define EH_TYPE 16u /* 2 = ET_EXEC */
#define EH_MACHINE 18u /* 40 = ARM */
#define EH_VERSION2 20u /* 1 */
#define EH_ENTRY 24u
#define EH_PHOFF 28u
#define EH_SHOFF 32u
#define EH_EHSIZE 40u
#define EH_PHENTSIZE 42u
#define EH_PHNUM 44u
#define EH_SHENTSIZE 46u
#define EH_SHNUM 48u

#define ET_EXEC 2u
#define EM_ARM 40u

/* Segment types. */
#define PT_LOAD 1u
#define PT_DYNAMIC 2u

/* Section types and flags. */
#define SHT_SYMTAB 2u
#define SHT_RELA 4u
#define SHT_DYNAMIC 6u
#define SHT_NOBITS 8u
#define SHT_REL 9u

#define SHF_WRITE 0x1u
#define SHF_ALLOC 0x2u
#define SHF_EXECINSTR 0x4u

/* ------------------------------------------------------------------ */
/* What elf.c has already checked, passed to elf_sections.c            */
/* Written by elf.c only; the other files only read it.               */
/* ------------------------------------------------------------------ */
struct elf_view {
    const uint8_t *f;   /* the whole file, in FLASH */
    uint32_t size;      /* its size in bytes */
    uint32_t entry;     /* e_entry, not checked yet */
    uint32_t phoff;     /* program header table: inside the file */
    uint32_t phnum;
    uint32_t shoff;     /* section header table: inside the file */
    uint32_t shnum;
    uint32_t file_off;  /* first loaded byte, as an offset in the file */
    uint32_t file_size; /* loaded bytes taken from the file */
    uint32_t load_end;  /* end of the PT_LOAD segments in memory, <= 3072 */
};

/* ------------------------------------------------------------------ */
/* One row of the section header table, already decoded               */
/* Used by elf_sections.c (checks) and reloc.c (relocations).         */
/* ------------------------------------------------------------------ */
struct elf_shdr {
    uint32_t type;    /* sh_type */
    uint32_t flags;   /* sh_flags */
    uint32_t addr;    /* sh_addr: offset in the slot */
    uint32_t offset;  /* sh_offset: offset in the file */
    uint32_t size;    /* sh_size */
    uint32_t link;    /* sh_link: for REL, the symbol table */
    uint32_t info;    /* sh_info: for REL, the section to relocate */
    uint32_t entsize; /* sh_entsize */
};

/* ------------------------------------------------------------------ */
/* Functions                                                          */
/* ------------------------------------------------------------------ */

/* elf.c: little-endian reads and overflow-safe range check. */
uint32_t elf_read16( const uint8_t *p );
uint32_t elf_read32( const uint8_t *p );
int elf_fits( uint32_t off, uint32_t len, uint32_t size );

/* elf_sections.c: decode row i of the section header table. Call it only
 * after elf.c has checked that the table is inside the file, and i < shnum. */
void elf_get_shdr( const uint8_t *f, uint32_t shoff, uint32_t i,
                   struct elf_shdr *sh );

/* elf_sections.c: check the sections, set p->code_end. 0 or -4. */
int elf_check_sections( const struct elf_view *v, struct load_plan *p );

#endif
