/*
 * kernel/reloc.c - Layer 3 of SVC 13: the relocations (Simone).
 * (Formerly kernel/loader.c.)
 *
 * Two passes over the same REL records, on purpose:
 *   elf_check_relocs  checks EVERY record, without writing anything;
 *   elf_apply_relocs  runs only after the check passed and the program
 *                     has been copied into the slot, and fixes the words.
 * The assignment (R9) wants every check done before any byte of the
 * program is written, so check and apply cannot be one single loop.
 *
 * What elf_check_layout (elf.c + elf_sections.c) has ALREADY checked, and
 * is not checked again here:
 *   - the section header table is inside the file (shoff, shnum);
 *   - every SHT_REL section has entsize 8, a whole number of records,
 *     and all its records inside the file;
 *   - the plan: [file_off, file_off + copy_size) is inside the file,
 *     copy_size = bss_start <= bss_end <= 3072, code_end <= 3072.
 *
 * Constants and helpers (elf_read16, elf_read32, elf_fits, elf_get_shdr)
 * come from kernel/elf_internal.h.
 */
#include <stdint.h>

#include "kernel/elf_internal.h"
#include "kernel/kprintf.h"

/* Decode, into *tgt, the section that a REL section corrects (its
 * sh_info row), and tell whether that section is copied into the slot
 * (flag A). Relocations for other sections, such as debug information,
 * are ignored by both passes. Used by both passes, so that they always
 * agree on which REL sections matter. */
static int rel_target_is_loaded( const uint8_t *f, uint32_t shoff, uint32_t info,
                                 struct elf_shdr *tgt )
{
    elf_get_shdr( f, shoff, info, tgt );
    return ( tgt->flags & SHF_ALLOC ) != 0;
}

int elf_check_relocs( const uint8_t *f, uint32_t size, const struct load_plan *p,
                      uint32_t slot_base )
{
    /* 3.1 Dove sta la tabella delle sezioni e quante righe ha.
     * Per thumb_ptr: shoff = 0x4888, shnum = 25. */
    uint32_t shoff = elf_read32( f + EH_SHOFF );
    uint32_t shnum = elf_read16( f + EH_SHNUM );

    /* 3.2 Tutte le righe della tabella: ci interessano solo le REL. */
    for ( uint32_t i = 0; i < shnum; i++ )
    {
        struct elf_shdr rel;
        elf_get_shdr( f, shoff, i, &rel );

        if ( rel.type != SHT_REL )
        {
            continue;
        }

        /* 3.3 info (sezione da correggere) e link (tabella dei simboli)
         * sono numeri di riga della stessa tabella: tra poco leggeremo
         * quelle righe, quindi devono esistere. entsize e record nel file
         * sono già stati controllati da elf_check_sections. */
        if ( rel.info >= shnum || rel.link >= shnum )
        {
            return -4;
        }

        /* 3.4 La sezione da correggere finisce nello slot? Se no (debug)
         * le sue relocation si ignorano. Per thumb_ptr succede per 6
         * sezioni REL su 7. */
        struct elf_shdr tgt;
        if ( !rel_target_is_loaded( f, shoff, rel.info, &tgt ) )
        {
            continue;
        }

        /* 3.5 La tabella dei simboli di questa REL: deve essere davvero una
         * SYMTAB, con voci da 16 byte, tutte dentro il file. */
        struct elf_shdr sym;
        elf_get_shdr( f, shoff, rel.link, &sym );

        if ( sym.type != SHT_SYMTAB || sym.entsize != ELF_SYM_SIZE
             || sym.size % ELF_SYM_SIZE != 0 || !elf_fits( sym.offset, sym.size, size ) )
        {
            return -4;
        }

        uint32_t nsyms = sym.size / ELF_SYM_SIZE;
        uint32_t nrel = rel.size / ELF_REL_SIZE;

        /* 3.6 Ogni record della REL. */
        for ( uint32_t r = 0; r < nrel; r++ )
        {
            const uint8_t *rec = f + rel.offset + r * ELF_REL_SIZE;
            uint32_t site = elf_read32( rec + REL_OFFSET );
            uint32_t r_info = elf_read32( rec + REL_INFO );

            /* r_info: tipo nel byte basso, indice del simbolo nei 24 bit
             * alti. Primo record di thumb_ptr: 0x350a -> tipo 10, simbolo 53. */
            uint32_t rtype = r_info & 0xFFu;
            uint32_t symi = r_info >> 8;

            /* 3.7 Tipo ammesso? Il messaggio va stampato PRIMA di ogni altro
             * controllo e con il tipo in decimale (assignment 6.3, test
             * unsupported_reloc). */
            if ( rtype != R_ARM_ABS32 && rtype != R_ARM_THM_CALL && rtype != R_ARM_THM_JUMP24 )
            {
                kprintf( "loader: unsupported relocation type %u\n", rtype );
                return -4;
            }

            /* 3.8 Il simbolo esiste ed è definito (st_shndx != SHN_UNDEF).
             * symi < nsyms garantisce che la voce stia nel file (3.5). */
            if ( symi >= nsyms
                 || elf_read16( f + sym.offset + symi * ELF_SYM_SIZE + SYM_SHNDX ) == SHN_UNDEF )
            {
                return -4;
            }

            /* 3.9 Il site, con i 4 byte che occupa (una word per ABS32,
             * un'istruzione BL/B.W per i salti Thumb), sta dentro la
             * sezione da correggere. Ferma un site fuori dal programma
             * (test bad_reloc). */
            if ( site < tgt.addr || !elf_fits( site - tgt.addr, 4, tgt.size ) )
            {
                return -4;
            }

            if ( rtype == R_ARM_ABS32 )
            {
                /* 3.10 ABS32: word allineata a 4, tra i byte copiati dal
                 * file (prima del BSS, che nel file non ha valore). */
                if ( site % 4 != 0 || !elf_fits( site, 4, p->bss_start ) )
                {
                    return -4;
                }

                /* Il valore originale, letto dallo STESSO byte del file
                 * che verrà copiato nello slot all'offset site
                 * (slot[x] = f[file_off + x]). Sta nel file: site + 4 <=
                 * bss_start = copy_size, e la copia è nel file.
                 * slot_base + word non deve superare 0xFFFFFFFF: il
                 * confronto evita di fare la somma, che girerebbe. */
                uint32_t word = elf_read32( f + p->file_off + site );
                if ( word > 0xFFFFFFFFu - slot_base )
                {
                    return -4;
                }
            }
            else
            {
                /* 3.11 Salti Thumb: relativi, codice e destinazione si
                 * spostano insieme, quindi non si correggono. Si controlla
                 * solo che il site sia un'istruzione vera: allineata a 2 e
                 * dentro il codice [0, code_end). */
                if ( site % 2 != 0 || !elf_fits( site, 4, p->code_end ) )
                {
                    return -4;
                }
            }
        }
    }

    /* Tutte le sezioni REL e tutti i record hanno superato i controlli. */
    return 0;
}

void elf_apply_relocs( const uint8_t *f, uint32_t slot_base )
{
    /* Chiamata solo DOPO elf_check_relocs == 0 sullo stesso f e slot_base,
     * e dopo la copia nello slot: ogni sezione, record e site è già valido,
     * quindi qui non c'è nessun controllo e la funzione non può fallire.
     * Rifà lo stesso giro (stesse REL, stessa regola 3.4) e per ogni ABS32
     * corregge la word nello SLOT, in RAM, non nel file in FLASH. */
    uint32_t shoff = elf_read32( f + EH_SHOFF );
    uint32_t shnum = elf_read16( f + EH_SHNUM );

    for ( uint32_t i = 0; i < shnum; i++ )
    {
        struct elf_shdr rel;
        struct elf_shdr tgt;
        elf_get_shdr( f, shoff, i, &rel );

        if ( rel.type != SHT_REL || !rel_target_is_loaded( f, shoff, rel.info, &tgt ) )
        {
            continue;
        }

        uint32_t nrel = rel.size / ELF_REL_SIZE;

        for ( uint32_t r = 0; r < nrel; r++ )
        {
            const uint8_t *rec = f + rel.offset + r * ELF_REL_SIZE;

            if ( ( elf_read32( rec + REL_INFO ) & 0xFFu ) == R_ARM_ABS32 )
            {
                /* Il programma è linkato a 0: il site è già l'offset nello
                 * slot. slot_base è multiplo di 4096 e site di 4, quindi
                 * l'accesso è allineato. Il bit 0 (Thumb) resta com'è,
                 * perché slot_base ha i 12 bit bassi a zero.
                 * thumb_ptr nello slot 0, site 0x2d4: 0x2b5 -> 0x200022b5. */
                uint32_t site = elf_read32( rec + REL_OFFSET );
                uint32_t *word = (uint32_t *) (uintptr_t) ( slot_base + site );
                *word += slot_base;
            }
        }
    }
}
