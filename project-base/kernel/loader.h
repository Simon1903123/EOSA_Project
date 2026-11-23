/*
 * kernel/loader.h - public interface of the program loader (SVC 13).
 *
 * Used by kernel/syscall.c. Implemented by:
 *   kernel/elf.c          ELF header and PT_LOAD segments  -> elf_check_layout
 *   kernel/elf_sections.c sections, code_end and entry     (called by elf.c)
 *   kernel/reloc.c        relocations: check, then apply   -> elf_check_relocs,
 *                                                             elf_apply_relocs
 *
 * Results: 0 = file accepted, -4 = file refused (the SVC 13 code for
 * "program file invalid"). No function here writes into a slot except
 * elf_apply_relocs, which runs only after every check has passed.
 */
#ifndef LOADER_H
#define LOADER_H

#include <stdint.h>

/* Slot layout (assignment 6.4), defined once for all the loader code.
 * A slot is 4096 bytes: code + data + BSS use at most the lower 3072,
 * the upper 1024 are the stack, which starts at the end of the slot. */
#define LOADER_SLOT_SIZE 4096u
#define LOADER_PROGRAM_MAX 3072u

/* What elf_check_layout found out about a file. Every field is an offset
 * in the SLOT, except file_off, which is an offset in the FILE. */
struct load_plan {
    uint32_t entry;               /* offset nello slot, bit 0 = 1 (Thumb) */
    uint32_t file_off, copy_size; /* byte da copiare dal file allo slot */
    uint32_t bss_start, bss_end;  /* offset da azzerare */
    uint32_t code_end;            /* fine della parte RX = inizio della parte RW */
    uint32_t mem_end;             /* fine del programma, <= LOADER_PROGRAM_MAX */
};

/* Check the ELF header, the segments and the sections; fill *p. 0 / -4. */
int elf_check_layout( const uint8_t *f, uint32_t size, struct load_plan *p );

/* Check every relocation that matters for a load at slot_base. 0 / -4.
 * Call it only after elf_check_layout returned 0 on the same file. */
int elf_check_relocs( const uint8_t *f, uint32_t size, const struct load_plan *p,
                      uint32_t slot_base );

/* Add slot_base to every R_ARM_ABS32 word, in the slot. Cannot fail:
 * call it only after elf_check_relocs returned 0 and the slot is filled. */
void elf_apply_relocs( const uint8_t *f, uint32_t slot_base );

#endif /* LOADER_H */
