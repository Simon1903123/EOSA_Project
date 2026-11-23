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
/* Fields used by the relocations (reloc.c)                           */
/* ------------------------------------------------------------------ */

/* Una riga della tabella delle sezioni (Elf32_Shdr): 40 byte.
 * Questi sono i byte, dentro la riga, dove inizia ogni campo. */
#define SH_NAME 0      /* indice del nome in .shstrtab: non lo usiamo */
#define SH_TYPE 4      /* tipo della sezione (REL, SYMTAB, ...) */
#define SH_FLAGS 8      /* flag: W = 1, A = 2, X = 4 */
#define SH_ADDR 12      /* dove va in memoria (= offset nello slot) */
#define SH_OFFSET 16      /* dove sta il contenuto nel file */
#define SH_SIZE 20      /* quanti byte di contenuto */
#define SH_LINK 24      /* per una REL: riga della tabella dei simboli */
#define SH_INFO 28      /* per una REL: riga della sezione da correggere */
#define SH_ENTSIZE 36      /* lunghezza di una voce, se la sezione è una tabella */

/* Un record REL (Elf32_Rel): 8 byte. */
#define R_OFFSET 0      /* il "site": dove sta la cosa da correggere */
#define R_INFO 4      /* tipo (byte basso) e indice del simbolo (24 bit alti) */

/* Un simbolo (Elf32_Sym): 16 byte. */
#define ST_SHNDX 14      /* sezione in cui è definito (2 byte); 0 = non definito */
#define SHN_UNDEF 0

/* Tipi di relocation ammessi (ELF for the ARM Architecture). */
#define R_ARM_ABS32 2u    /* word con un indirizzo completo: va corretta */
#define R_ARM_THM_CALL 10u    /* chiamata Thumb (BL): relativa, non si tocca */
#define R_ARM_THM_JUMP24 30u    /* salto Thumb (B.W): relativo, non si tocca */

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

/* ======================================================================
 * STEP 1 - The three common helpers.
 * Used by elf.c (Gianluigi), elf_sections.c (Athena) and reloc.c
 * (Simone): defined here once, so every file uses the same code.
 * ====================================================================== */

/*
 * elf_read16 - read a 16-bit number stored at p in the file.
 *
 * The file is little-endian: the LOW byte comes first, the HIGH byte second.
 *   p[0] = 0x34, p[1] = 0x00  ->  0x0034 = 52   (e_phoff, in every program)
 *   p[0] = 0x18, p[1] = 0x00  ->  0x0018 = 24   (e_shnum of global_state)
 * The caller must already know that p[0] and p[1] are inside the file.
 */
static inline uint32_t elf_read16( const uint8_t *p )
{
    /* Each byte is widened to uint32_t before the shift; p[1] is moved
     * 8 bits up (times 256) and the two halves are joined with | (they
     * share no bits, so | works like a sum). */
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 );
}

/*
 * elf_read32 - read a 32-bit number stored at p in the file.
 *
 * Same idea with 4 bytes: p[0] is the lowest byte, p[3] the highest.
 *   3c 36 01 00  ->  0x0001363c   (e_shoff of global_state)
 * Reading byte by byte, instead of *(uint32_t *) p, avoids an unaligned
 * access fault: p comes from the file and may be any address.
 */
static inline uint32_t elf_read32( const uint8_t *p )
{
    /* Without the (uint32_t) cast, p[3] << 24 would be computed on a
     * signed int and overflow when p[3] >= 0x80. */
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 )
           | ( (uint32_t) p[3] << 24 );
}

/*
 * elf_fits - is the range [off, off + len) completely inside [0, size)?
 *
 *   off   where the range starts, counted from the beginning of the
 *         container (a file offset, or a slot offset).
 *         Example: e_shoff = 0x1363c, the start of the section table.
 *   len   how many bytes the range is long.
 *         Example: e_shnum * 40 = 24 * 40 = 960, the whole section table.
 *   size  how big the container is: the file length (size of the ELF)
 *         or SLOT_PROG_MAX (3072) for the program part of a slot.
 *
 * Returns 1 if the range fits, 0 otherwise. All three values may come
 * from a damaged file, so off + len is never computed: with
 * off = 0xFFFFFFF0 and len = 0x20 the sum would wrap around to 0x10 and
 * look "inside".
 *
 * Examples: elf_fits( 0, 3072, 3072 )          = 1   (ends exactly at the end)
 *           elf_fits( 0, 3073, 3072 )          = 0   (too_large: one byte too many)
 *           elf_fits( 0xFFFFFFF0, 0x20, 3072 ) = 0   (overflow avoided)
 */
static inline int elf_fits( uint32_t off, uint32_t len, uint32_t size )
{
    /* First the start must be inside, then the length must fit in what is
     * left after the start. size - off cannot go below zero here, because
     * the first test already passed (&& stops at the first false). */
    return off <= size && len <= size - off;
}

/* elf_sections.c: decode row i of the section header table. Call it only
 * after elf.c has checked that the table is inside the file, and i < shnum. */
void elf_get_shdr( const uint8_t *f, uint32_t shoff, uint32_t i,
                   struct elf_shdr *sh );

/* elf_sections.c: check the sections, set p->code_end. 0 or -4. */
int elf_check_sections( const struct elf_view *v, struct load_plan *p );

#endif
