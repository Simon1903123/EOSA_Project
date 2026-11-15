/* kernel/elf_sections.c - section checks and code_end (Athena). */
#include "kernel/elf_internal.h"

void elf_get_shdr( const uint8_t *f, uint32_t shoff, uint32_t i,
                   struct elf_shdr *sh )
{
    const uint8_t *r = f + shoff + i * ELF_SHDR_SIZE;

    sh->type = elf_read32( r + 4 );
    sh->flags = elf_read32( r + 8 );
    sh->addr = elf_read32( r + 12 );
    sh->offset = elf_read32( r + 16 );
    sh->size = elf_read32( r + 20 );
    sh->link = elf_read32( r + 24 );
    sh->info = elf_read32( r + 28 );
    sh->entsize = elf_read32( r + 36 );
}

int elf_check_sections( const struct elf_view *v, struct load_plan *p )
{
    (void) v;
    /* TODO: allocated sections inside the segments, no RELA/DYNAMIC,
     * read-only before writable, code_end, entry inside the code. */
    p->code_end = 0;
    return 0;
}
