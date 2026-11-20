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
    uint32_t code_end = 0;
    int seen_write = 0;
    int entry_ok = 0;
    uint32_t first_writable_addr = 0xFFFFFFFFu;
    uint32_t entry = v->entry & ~1u;
    uint32_t i;

    /* l'entry deve essere Thumb (bit 0 = 1) */
    if ( !( v->entry & 1u ) )
        return -4;

    for ( i = 0; i < v->shnum; i++ ) {
        struct elf_shdr sh;
        elf_get_shdr( v->f, v->shoff, i, &sh );

        /* niente RELA né dynamic linking, in qualunque sezione */
        if ( sh.type == SHT_RELA || sh.type == SHT_DYNAMIC )
            return -4;

        /* tutte le sezioni REL: i record devono stare nel file */
        if ( sh.type == SHT_REL ) {
            if ( sh.entsize != ELF_REL_SIZE ||
                 ( sh.size % ELF_REL_SIZE ) != 0 ||
                 !elf_fits( sh.offset, sh.size, v->size ) )
                return -4;
        }

        /* le sezioni non caricate non servono altro */
        if ( !( sh.flags & SHF_ALLOC ) )
            continue;

        /* in memoria: dentro i segmenti caricati (e quindi nello slot) */
        if ( !elf_fits( sh.addr, sh.size, v->load_end ) )
            return -4;

        /* nel file: solo se ha byte (non NOBITS), dentro la parte caricata */
        if ( sh.type != SHT_NOBITS ) {
            if ( sh.offset < v->file_off ||
                 !elf_fits( sh.offset - v->file_off, sh.size, v->file_size ) )
                return -4;
        }

        /* read-only prima delle scrivibili (serve a W^X) */
        if ( sh.flags & SHF_WRITE )
            seen_write = 1;
        else if ( seen_write )
            return -4;

        /* codice: aggiorna code_end e controlla l'entry */
        if ( sh.flags & SHF_EXECINSTR ) {
            if ( sh.addr + sh.size > code_end )   /* sicuro: <= load_end */
                code_end = sh.addr + sh.size;
            if ( entry >= sh.addr && entry - sh.addr < sh.size )
                entry_ok = 1;
        }
    }

    if ( !entry_ok ) return -4;

    /*la prima area scrivibile deve iniziare in una regione da 512 B
       diversa da quella dove finisce il codice, così W^X vale a livello
       di regione */
    if ( first_writable_addr != 0xFFFFFFFFu &&
         first_writable_addr < ( ( code_end + 511u ) & ~511u ) )
        return -4;

    p->code_end = code_end;
    return 0;
}