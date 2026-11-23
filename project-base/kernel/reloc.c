#include <stdint.h>
#include "kernel/elf_internal.h"
#include "kernel/kprintf.h"

int elf_check_relocs( const uint8_t *f, uint32_t size, const struct load_plan *p, uint32_t slot_base )
{
    /* ============================================================== */
    /* 3.1 Dove sta la tabella delle sezioni e quante righe ha.       */
    /* ============================================================== */
    /* f + EH_SHOFF è il POSTO (il byte 32 del file); elf_read32 va lì e
     * restituisce il NUMERO scritto in quei 4 byte. Per thumb_ptr:
     * shoff = 0x4888, shnum = 25.
     * Non controlliamo che la tabella stia nel file: l'ha già fatto
     * elf_check_layout (Gianluigi e Athena) prima di chiamarci. */
    uint32_t shoff = elf_read32( f + EH_SHOFF );
    uint32_t shnum = elf_read16( f + EH_SHNUM );

    /* ============================================================== */
    /* 3.2 Scorriamo TUTTE le righe della tabella.                    */
    /* ============================================================== */
    /* Non sappiamo in anticipo quali righe sono REL né quante sono:
     * dipende dal programma. Le scopriamo leggendo il tipo di ognuna.
     * Ogni REL viene controllata per intero nel giro in cui la troviamo:
     * non serve memorizzare niente per dopo. */
    for ( uint32_t i = 0; i < shnum; i++ )
    {
        /* Il posto della riga i: inizio del file + inizio della tabella
         * + i righe da 40 byte. È un puntatore, non un numero. */
        const uint8_t *row = f + shoff + i * ELF_SHDR_SIZE;

        /* Il tipo è un numero (REL = 9), non un nome: confrontiamo numeri.
         * Se non è una REL, il resto del giro non ci riguarda. */
        if ( elf_read32( row + SH_TYPE ) != SHT_REL )
        {
            continue;
        }

        /* Siamo su una riga REL. Leggiamo i cinque campi che ci servono.
         * Sono le colonne Off, Size, ES, Lk, Inf di readelf -S.
         * Per .rel.text di thumb_ptr: 0x3cb8, 0x98, 8, 22, 1. */
        uint32_t rel_off = elf_read32( row + SH_OFFSET );    /* dove iniziano i record nel file */
        uint32_t rel_size = elf_read32( row + SH_SIZE );      /* quanti byte di record */
        uint32_t entsize = elf_read32( row + SH_ENTSIZE );   /* lunghezza di un record */
        uint32_t link = elf_read32( row + SH_LINK );      /* riga della tabella dei simboli */
        uint32_t info = elf_read32( row + SH_INFO );      /* riga della sezione da correggere */

        /* ========================================================== */
        /* 3.3 La sezione REL stessa è fatta bene?                    */
        /* ========================================================== */
        /* Questi valori vengono dal file: un file rovinato può metterci
         * qualunque cosa. Prima di usarli per leggere altrove, li
         * controlliamo. Al primo problema ci fermiamo con -4. */

        /* Ogni record è lungo 8 byte, e la sezione deve contenerne un
         * numero intero: altrimenti l'ultimo record sarebbe tagliato.
         * Basta che UNA delle due sia sbagliata per rifiutare: ||. */
        if ( entsize != ELF_REL_SIZE || rel_size % ELF_REL_SIZE != 0 )
        {
            return -4;
        }

        /* Tutti i record, da rel_off a rel_off + rel_size, devono essere
         * byte del file. Altrimenti elf_read32 leggerebbe oltre la fine del
         * file (il programma successivo nell'immagine, o peggio).
         * È il controllo che ferma un ELF tagliato (test truncated_elf).
         * Si usa elf_fits e non la somma, che potrebbe andare in overflow. */
        if ( !elf_fits( rel_off, rel_size, size ) )
        {
            return -4;
        }

        /* info e link sono NUMERI DI RIGA della stessa tabella: tra poco
         * andremo a leggere quelle righe, quindi devono esistere.
         * Basta che UNO dei due sia fuori dalla tabella: ||. */
        if ( info >= shnum || link >= shnum )
        {
            return -4;
        }

        /* ========================================================== */
        /* 3.4 La sezione da correggere finisce nello slot?           */
        /* ========================================================== */
        /* info ci dice QUALE riga descrive la sezione bersaglio. Il suo
         * posto si calcola con la stessa formula della riga i.
         * Per .rel.text la riga è la 1, .text, con flag 6 = A + X. */
        const uint8_t *target = f + shoff + info * ELF_SHDR_SIZE;

        /* Il bit A (valore 2) dice se la sezione è allocated, cioè se
         * viene copiata nello slot. Le sezioni di debug non lo hanno:
         * le loro relocation non vanno né controllate né applicate.
         * Basta che i record stiano nel file, e l'abbiamo già visto al
         * 3.3. Quindi passiamo alla riga successiva della tabella.
         * Per thumb_ptr succede per 6 sezioni REL su 7. */
        if ( ( elf_read32( target + SH_FLAGS ) & SHF_ALLOC ) == 0 )
        {
            continue;
        }

        /* Tre campi del bersaglio che serviranno al 3.7. Si leggono una
         * volta per sezione REL, non per ogni record: non cambiano.
         * Per .text di thumb_ptr: addr = 0, off = 0x1000, size = 0x310.
         * - tgt_addr: dove inizia la sezione nello slot (colonna Addr);
         * - tgt_off:  dove inizia la sezione nel file  (colonna Off);
         * - tgt_size: quanto è lunga                   (colonna Size). */
        uint32_t tgt_addr = elf_read32( target + SH_ADDR );
        uint32_t tgt_off = elf_read32( target + SH_OFFSET );
        uint32_t tgt_size = elf_read32( target + SH_SIZE );

        /* ========================================================== */
        /* 3.5 La tabella dei simboli usata da questa REL.            */
        /* ========================================================== */
        /* link ci dice QUALE riga descrive la tabella dei simboli.
         * Non la cerchiamo per nome: la raggiungiamo da qui.
         * Per thumb_ptr è la riga 22, .symtab. */
        const uint8_t *symrow = f + shoff + link * ELF_SHDR_SIZE;

        /* Deve essere davvero una tabella dei simboli, con voci da 16 byte. */
        if ( elf_read32( symrow + SH_TYPE ) != SHT_SYMTAB || elf_read32( symrow + SH_ENTSIZE ) != ELF_SYM_SIZE )
        {
            return -4;
        }

        /* Dove stanno i simboli nel file e quanti byte occupano.
         * Per thumb_ptr: 0x377c e 0x3d0. */
        uint32_t sym_off = elf_read32( symrow + SH_OFFSET );
        uint32_t sym_size = elf_read32( symrow + SH_SIZE );

        /* Stesse regole dei record: un numero intero di simboli, tutti
         * dentro il file, perché tra poco li leggeremo. */
        if ( sym_size % ELF_SYM_SIZE != 0 || !elf_fits( sym_off, sym_size, size ) )
        {
            return -4;
        }

        /* Quanti simboli ci sono: per thumb_ptr 0x3d0 / 16 = 61.
         * Un record può usare solo i simboli da 0 a nsyms - 1. */
        uint32_t nsyms = sym_size / ELF_SYM_SIZE;

        /* ========================================================== */
        /* 3.6 Ogni record della REL.                                 */
        /* ========================================================== */
        /* Il numero di record: per .rel.text di thumb_ptr 0x98 / 8 = 19.
         * Sappiamo già (3.3) che sono tutti dentro il file. */
        uint32_t nrel = rel_size / ELF_REL_SIZE;

        for ( uint32_t r = 0; r < nrel; r++ )
        {
            /* Il posto del record r: inizio dei record + r record da 8 byte. */
            const uint8_t *rec = f + rel_off + r * ELF_REL_SIZE;

            /* I due campi del record. Sono le colonne Offset e Info di
             * readelf -r. Per il primo record di thumb_ptr:
             * site = 0x0, r_info = 0x350a. */
            uint32_t site = elf_read32( rec + R_OFFSET );
            uint32_t r_info = elf_read32( rec + R_INFO );

            /* r_info contiene due informazioni in un numero solo:
             * - il tipo nel byte basso: 0x350a & 0xff = 0x0a = 10 (THM_CALL);
             * - l'indice del simbolo nei 24 bit alti: 0x350a >> 8 = 0x35 = 53 (main). */
            uint32_t rtype = r_info & 0xFF;
            uint32_t sym = r_info >> 8;

            if ( rtype != R_ARM_ABS32 && rtype != R_ARM_THM_CALL && rtype != R_ARM_THM_JUMP24 )
            {
                kprintf( "loader: unsupported relocation type %u\n", rtype );
                return -4;
            }

            /* Il simbolo esiste? Deve essere uno dei nsyms della tabella.
             * Va controllato PRIMA di leggerlo: un indice enorme ci farebbe
             * leggere fuori dal file. */
            if ( sym >= nsyms )
            {
                return -4;
            }

            /* Il posto del simbolo sym: inizio dei simboli + sym voci da 16
             * byte. Sta nel file, perché sym < nsyms e i simboli stanno
             * tutti nel file (3.5). */
            const uint8_t *s = f + sym_off + sym * ELF_SYM_SIZE;

            /* Il simbolo è definito? st_shndx è la sezione in cui è
             * definito; 0 vuol dire "non definito" (UND in readelf -s).
             * Un programma che usa un simbolo non definito non può
             * essere caricato: nessuno saprebbe dove punta.
             * È un campo da 2 byte: elf_read16. */
            if ( elf_read16( s + ST_SHNDX ) == SHN_UNDEF )
            {
                return -4;
            }

            /* ====================================================== */
            /* 3.7 – 3.9 Controlli diversi secondo il tipo.           */
            /* ====================================================== */
            switch ( rtype )
            {
            case R_ARM_ABS32: {
                /* -------------------------------------------------- */
                /* 3.7 ABS32: una word da 4 byte che contiene un      */
                /* indirizzo completo. elf_apply_relocs le sommerà    */
                /* slot_base, quindi qui controlliamo che si possa    */
                /* fare senza danni. Esempio thumb_ptr: site 0x2d4,   */
                /* il puntatore ad add_seven.                         */
                /* -------------------------------------------------- */

                /* (a) Allineata a 4: una word intera, che poi si
                 * scriverà come uint32_t. 0x2d4 % 4 = 0: ok. */
                if ( site % 4 != 0 )
                {
                    return -4;
                }

                /* (b) Dentro la sezione bersaglio. La sezione occupa,
                 * nello slot, [tgt_addr, tgt_addr + tgt_size): per .text
                 * [0, 0x310). Prima il site non deve stare prima
                 * dell'inizio; poi, contando da dove inizia la sezione
                 * (site - tgt_addr), i 4 byte della word devono starci.
                 * È il controllo che ferma un site fuori dal programma
                 * (test bad_reloc): applicandolo scriveremmo fuori. */
                if ( site < tgt_addr || !elf_fits( site - tgt_addr, 4, tgt_size ) )
                {
                    return -4;
                }

                /* (c) Tra i byte copiati, cioè prima del BSS. Il BSS nel
                 * file non ha byte: una word lì non avrebbe un valore
                 * originale da leggere. bss_start viene dal piano di
                 * Gianluigi e Athena; per thumb_ptr vale 0x310. */
                if ( !elf_fits( site, 4, p->bss_start ) )
                {
                    return -4;
                }

                /* (d) Il valore originale della word, letto dal FILE: lo
                 * slot non è ancora stato riempito. La sezione inizia nel
                 * file a tgt_off, e la word sta (site - tgt_addr) byte più
                 * avanti. Per sicurezza controlliamo che la sezione stia
                 * nel file, così anche quel posto ci sta.
                 * Per site 0x2d4: posto f + 0x1000 + 0x2d4, valore 0x2b5. */
                if ( !elf_fits( tgt_off, tgt_size, size ) )
                {
                    return -4;
                }
                uint32_t word = elf_read32( f + tgt_off + ( site - tgt_addr ) );

                /* (e) slot_base + word non deve superare 0xFFFFFFFF,
                 * altrimenti la somma "gira" e il puntatore diventa un
                 * indirizzo basso a caso. Invece di fare la somma (che
                 * girerebbe), confrontiamo word con lo spazio che resta.
                 * 0x2b5 <= 0xFFFFFFFF - 0x20002000: ok.
                 * Dove punta la word non ci interessa: anche fuori dal
                 * programma non è un errore. */
                if ( word > 0xFFFFFFFFu - slot_base )
                {
                    return -4;
                }
                break;
            }

            case R_ARM_THM_CALL:
            case R_ARM_THM_JUMP24:
                /* -------------------------------------------------- */
                /* 3.8 Salti Thumb (BL e B.W). Dicono "salta N byte   */
                /* più avanti o indietro": codice e destinazione si   */
                /* spostano insieme nello slot, quindi non si         */
                /* correggono. Controlliamo solo che il site sia      */
                /* un'istruzione vera.                                */
                /* -------------------------------------------------- */

                /* (a) Le istruzioni Thumb sono allineate a 2: un site
                 * come 0x66 va bene, uno dispari no. */
                if ( site % 2 != 0 )
                {
                    return -4;
                }

                /* (b) BL e B.W sono istruzioni da 4 byte, e devono stare
                 * nel codice: [0, code_end). code_end viene dal piano;
                 * per thumb_ptr vale 0x310. */
                if ( !elf_fits( site, 4, p->code_end ) )
                {
                    return -4;
                }
                break;

            default:
                /* -------------------------------------------------- */
                /* 3.9 Qualunque altro tipo, in una sezione che       */
                /* finisce nello slot: non sappiamo correggerlo, il   */
                /* programma non si può caricare. La specifica vuole  */
                /* esattamente questa riga, con il tipo in decimale   */
                /* (test unsupported_reloc, tipo 3).                  */
                /* -------------------------------------------------- */
                kprintf( "loader: unsupported relocation type %u\n", rtype );
                return -4;
            }
        }
    }

    /* Tutte le sezioni REL e tutti i record hanno superato i controlli. */
    return 0;
}

void elf_apply_relocs( const uint8_t *f, uint32_t slot_base )
{
    /* Questa funzione viene chiamata da Antonio solo DOPO che:
     * - elf_check_relocs ha restituito 0 sullo stesso f e con lo stesso
     *   slot_base: ogni sezione, ogni record e ogni site sono già validi;
     * - il programma è già stato copiato nello slot e il BSS azzerato.
     * Per questo qui non c'è nessun controllo, e la funzione non può
     * fallire: è void. Rifà lo stesso giro di elf_check_relocs, nello
     * stesso modo, e per ogni ABS32 corregge la word nello slot. */

    /* 3.1 Come nel controllo: dove sta la tabella delle sezioni e quante
     * righe ha. Per thumb_ptr: 0x4888 e 25. */
    uint32_t shoff = elf_read32( f + EH_SHOFF );
    uint32_t shnum = elf_read16( f + EH_SHNUM );

    /* 3.2 Come nel controllo: tutte le righe, ma solo le REL. */
    for ( uint32_t i = 0; i < shnum; i++ )
    {
        const uint8_t *row = f + shoff + i * ELF_SHDR_SIZE;

        if ( elf_read32( row + SH_TYPE ) != SHT_REL )
        {
            continue;
        }

        /* Della riga REL servono solo tre campi: dove sono i record,
         * quanti byte occupano, e quale sezione correggono. entsize e
         * link servivano solo ai controlli. */
        uint32_t rel_off = elf_read32( row + SH_OFFSET );
        uint32_t rel_size = elf_read32( row + SH_SIZE );
        uint32_t info = elf_read32( row + SH_INFO );

        /* 3.4 Stessa regola del controllo, ed è obbligatoria anche qui:
         * le REL del debug hanno site che si riferiscono alle sezioni di
         * debug, non allo slot. Applicarle scriverebbe nello slot in posti
         * sbagliati e rovinerebbe il codice. Quindi si saltano. */
        const uint8_t *target = f + shoff + info * ELF_SHDR_SIZE;
        if ( ( elf_read32( target + SH_FLAGS ) & SHF_ALLOC ) == 0 )
        {
            continue;
        }

        /* Ogni record della REL: per .rel.text di thumb_ptr 19 record. */
        uint32_t nrel = rel_size / ELF_REL_SIZE;

        for ( uint32_t r = 0; r < nrel; r++ )
        {
            const uint8_t *rec = f + rel_off + r * ELF_REL_SIZE;
            uint32_t site = elf_read32( rec + R_OFFSET );
            uint32_t rtype = elf_read32( rec + R_INFO ) & 0xFF;

            /* Solo le ABS32 contengono un indirizzo completo, calcolato
             * come se il programma fosse all'indirizzo 0: va spostato di
             * slot_base. I salti Thumb sono relativi e restano come sono.
             * Altri tipi qui non possono arrivare: li ha già rifiutati
             * elf_check_relocs (3.9). */
            if ( rtype == R_ARM_ABS32 )
            {
                /* Dove sta la word in RAM: il programma è linkato a 0, quindi
                 * il site è già l'offset dentro lo slot.
                 * Per thumb_ptr nello slot 0, site 0x2d4: 0x200022d4.
                 * slot_base + site è un numero; il cast lo trasforma in un
                 * puntatore a una word. Il passaggio da uintptr_t è il modo
                 * corretto in C di convertire un intero in un puntatore.
                 * Qui si può scrivere direttamente come uint32_t: il site è
                 * multiplo di 4 (3.7) e slot_base è multiplo di 4096, quindi
                 * l'indirizzo è allineato. Si scrive nello SLOT, in RAM, non
                 * nel file f, che è in FLASH e in sola lettura. */
                uint32_t *word = (uint32_t *) (uintptr_t) ( slot_base + site );

                /* Legge la word copiata da Antonio, le somma slot_base e la
                 * riscrive nello stesso posto: 0x000002b5 -> 0x200022b5.
                 * Il bit 0 (Thumb) resta 1, perché slot_base finisce con
                 * 12 bit a zero. Niente overflow: l'ha controllato il 3.7. */
                *word += slot_base;
            }
        }
    }
}
