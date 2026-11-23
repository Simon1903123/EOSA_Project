/*
 * kernel/elf_internal.h - everything the three parts of the ELF loader
 * share: elf.c (header and segments), elf_sections.c (sections) and
 * reloc.c (relocations). Not used outside the loader: the rest of the
 * kernel sees only kernel/loader.h.
 *
 * Every ELF constant, record size and field position is defined HERE
 * ONLY, and so are the three helpers that read the file safely. The .c
 * files must not define their own copies.
 */
#ifndef ELF_INTERNAL_H
#define ELF_INTERNAL_H

#include <stdint.h>

#include "kernel/loader.h"

/* ------------------------------------------------------------------ */
/* Record sizes required by the assignment (6.3)                      */
/* ------------------------------------------------------------------ */
#define ELF_EHDR_SIZE 52u /* file header (Elf32_Ehdr) */
#define ELF_PHDR_SIZE 32u /* one program-header row (Elf32_Phdr) */
#define ELF_SHDR_SIZE 40u /* one section-header row (Elf32_Shdr) */
#define ELF_REL_SIZE 8u   /* one REL record (Elf32_Rel) */
#define ELF_SYM_SIZE 16u  /* one symbol (Elf32_Sym) */

/* ------------------------------------------------------------------ */
/* Positions of the fields, inside each record                        */
/* ------------------------------------------------------------------ */

/* ELF header. */
#define EH_CLASS 4u     /* 1 = 32 bit (1 byte) */
#define EH_DATA 5u      /* 1 = little-endian (1 byte) */
#define EH_VERSION 6u   /* 1 (1 byte) */
#define EH_TYPE 16u     /* 2 = ET_EXEC (2 bytes) */
#define EH_MACHINE 18u  /* 40 = ARM (2 bytes) */
#define EH_VERSION2 20u /* 1 (4 bytes) */
#define EH_ENTRY 24u
#define EH_PHOFF 28u
#define EH_SHOFF 32u
#define EH_EHSIZE 40u
#define EH_PHENTSIZE 42u
#define EH_PHNUM 44u
#define EH_SHENTSIZE 46u
#define EH_SHNUM 48u

/* One program-header row (segment). */
#define PH_TYPE 0u
#define PH_OFFSET 4u
#define PH_VADDR 8u
#define PH_FILESZ 16u
#define PH_MEMSZ 20u

/* One section-header row: decoded by elf_get_shdr() into struct elf_shdr. */
#define SH_TYPE 4u
#define SH_FLAGS 8u
#define SH_ADDR 12u
#define SH_OFFSET 16u
#define SH_SIZE 20u
#define SH_LINK 24u
#define SH_INFO 28u
#define SH_ENTSIZE 36u

/* One REL record. */
#define REL_OFFSET 0u /* r_offset: the site to relocate */
#define REL_INFO 4u   /* r_info: type (low byte) and symbol index (high 24 bits) */

/* One symbol. */
#define SYM_SHNDX 14u /* st_shndx: section where it is defined (2 bytes) */

/* ------------------------------------------------------------------ */
/* Values                                                             */
/* ------------------------------------------------------------------ */
#define ET_EXEC 2u
#define EM_ARM 40u

#define PT_LOAD 1u
#define PT_DYNAMIC 2u

#define SHT_SYMTAB 2u
#define SHT_RELA 4u
#define SHT_DYNAMIC 6u
#define SHT_NOBITS 8u
#define SHT_REL 9u

#define SHF_WRITE 0x1u
#define SHF_ALLOC 0x2u /* the section is copied into the slot */
#define SHF_EXECINSTR 0x4u

#define SHN_UNDEF 0u /* st_shndx of an undefined symbol */

/* Relocation types accepted for loaded content (ELF for the ARM Architecture). */
#define R_ARM_ABS32 2u       /* a full 32-bit address: add slot_base */
#define R_ARM_THM_CALL 10u   /* Thumb BL: PC-relative, unchanged */
#define R_ARM_THM_JUMP24 30u /* Thumb B.W: PC-relative, unchanged */

/* W^X works on 512-byte MPU subregions (4096 / 8): data must start in a
 * different subregion than the one where the code ends. */
#define ELF_WX_GRANULE 512u

/* ------------------------------------------------------------------ */
/* What elf.c has already checked, passed to elf_sections.c            */
/* ------------------------------------------------------------------ */
struct elf_view {
    const uint8_t *f;   /* the whole file, in FLASH */
    uint32_t size;      /* its size in bytes */
    uint32_t entry;     /* e_entry, checked by elf_sections.c */
    uint32_t phoff;     /* program header table: inside the file */
    uint32_t phnum;
    uint32_t shoff;     /* section header table: inside the file */
    uint32_t shnum;
    uint32_t file_off;  /* first loaded byte, as an offset in the file */
    uint32_t file_size; /* loaded bytes taken from the file */
    uint32_t load_end;  /* end of the PT_LOAD segments in memory, <= 3072 */
};

/* One row of the section header table, already decoded.
 * Used by elf_sections.c (checks) and reloc.c (relocations). */
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
/* The shared helpers (one definition, used by all three files)       */
/* ------------------------------------------------------------------ */

/* Read a little-endian 16-bit number stored at p in the file.
 * The caller must already know that p[0] and p[1] are inside the file. */
static inline uint32_t elf_read16( const uint8_t *p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 );
}

/* Read a little-endian 32-bit number stored at p in the file.
 * Byte by byte, instead of *(uint32_t *) p: p comes from the file and may
 * be unaligned. Each byte is widened before the shift, so p[3] << 24 is
 * never computed on a signed int. */
static inline uint32_t elf_read32( const uint8_t *p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 )
           | ( (uint32_t) p[3] << 24 );
}

/* Is [off, off + len) completely inside [0, size)? 1 = yes, 0 = no.
 * off + len is never computed, so a damaged value cannot wrap around:
 * elf_fits( 0xFFFFFFF0, 0x20, 3072 ) = 0. */
static inline int elf_fits( uint32_t off, uint32_t len, uint32_t size )
{
    return off <= size && len <= size - off;
}

/* elf_sections.c: decode row i of the section header table. Call it only
 * after elf.c has checked that the table is inside the file, and i < shnum. */
void elf_get_shdr( const uint8_t *f, uint32_t shoff, uint32_t i, struct elf_shdr *sh );

/* elf_sections.c: check the sections, set p->code_end. 0 or -4. */
int elf_check_sections( const struct elf_view *v, struct load_plan *p );

#endif /* ELF_INTERNAL_H */
