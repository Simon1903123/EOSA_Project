# `elf_check_sections` (versione big): spiegazione

## 1. Cosa deve fare la funzione

`elf_check_sections` controlla le sezioni di un ELF già validato da `elf.c` (identità, dimensioni dei record, tabelle dentro il file, segmenti PT_LOAD). Ritorna `0` se tutto va bene, `-4` per qualunque rifiuto (e il messaggio `error: program cannot be loaded`). In caso di successo scrive `p->code_end`.

Requisiti presi dal compito (paragrafo 6.3, "Big version: what the kernel checks in an ELF") e dal TODO nello stub:

1. l'ELF non deve contenere record RELA né dynamic linking;
2. tutte le sezioni REL devono avere i record dentro il file;
3. ogni sezione caricata (allocata) deve stare dentro i segmenti caricati, in memoria e nel file, quindi dentro lo slot;
4. le sezioni read-only vengono prima di quelle scrivibili (dal TODO, serve a W^X);
5. calcolo di `code_end`;
6. l'entry è un indirizzo Thumb dentro il codice.

Il compito dice anche che il kernel non deve controllare nomi di sezione, debug, stringhe e campi dei simboli che non usa.

## 2. Assunzioni da verificare nel resto del progetto

- `elf_fits(off, len, size)` ritorna diverso da zero se `[off, off+len)` sta dentro `size`, ed è overflow-safe.
- `load_end` è la fine dei PT_LOAD in memoria (`<= 3072`, quindi dentro lo slot).
- I byte caricati dal file sono quelli in `[file_off, file_off + file_size)`.
- La definizione esatta di `code_end` non è nel compito: va verificata nel commento di `struct load_plan` (`loader.h`) e in come la usa il codice MPU.

## 3. Codice

```c
int elf_check_sections( const struct elf_view *v, struct load_plan *p )
{
    uint32_t code_end = 0;
    int seen_write = 0;
    int entry_ok = 0;
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

    if ( !entry_ok )
        return -4;

    p->code_end = code_end;
    return 0;
}
```

## 4. Spiegazione punto per punto

### 4.1 Variabili

- `code_end`: massimo `addr + size` tra le sezioni di codice.
- `seen_write`: diventa 1 alla prima sezione allocata scrivibile; serve a imporre l'ordine "read-only prima, scrivibili dopo".
- `entry_ok`: diventa 1 se l'entry cade dentro una sezione eseguibile.
- `entry`: l'indirizzo reale dell'entry, cioè `v->entry & ~1u`.
- `i`: indice del ciclo.

Sono variabili **locali**: ripartono da 0 a ogni chiamata, cioè a ogni ELF controllato. Dentro il ciclo si aggiornano soltanto, non si reimpostano mai, altrimenti `seen_write` non ricorderebbe le sezioni precedenti. Nel ciclo si dichiara solo ciò che vale per una singola sezione (`struct elf_shdr sh`).

### 4.2 Bit Thumb dell'entry

```c
if ( !( v->entry & 1u ) )
    return -4;
```

Due operazioni diverse sul bit 0:

- `entry & 1u` **legge** il bit Thumb (1 = Thumb valido);
- `entry & ~1u` **azzera** il bit 0 e dà l'indirizzo reale. `~1u` è la maschera `0xFFFFFFFE`.

```
entry         = 0x00000125   (bit 0 = 1, Thumb)
entry & 1u    = 1
entry & ~1u   = 0x00000124   (indirizzo reale dell'istruzione)
```

Il bit 0 segnala solo la modalità e non fa parte dell'indirizzo, quindi per confrontare l'entry con le sezioni si usa `entry & ~1u`.

### 4.3 Ciclo e decodifica

Il ciclo scorre le righe della tabella delle sezioni di **un solo ELF**. `elf_get_shdr` si può chiamare senza controlli aggiuntivi perché `elf.c` ha già verificato che la tabella stia nel file e che `i < shnum`.

### 4.4 Niente RELA né DYNAMIC

L'ELF non deve contenere record RELA né dynamic linking, quindi si rifiuta qualunque sezione di tipo `SHT_RELA` o `SHT_DYNAMIC`, anche non allocata.

- **RELA**: record da 12 byte con addend esplicito. Il loader gestisce solo REL da 8 byte (`ELF_REL_SIZE`).
- **DYNAMIC**: indica linking dinamico, che qui non esiste.

Si rifiutano solo questi due tipi, non qualunque tipo sconosciuto (nomi, debug e stringhe non vanno controllati). La riga `loader: unsupported relocation type` riguarda solo i tipi di relocation e non si stampa qui.

### 4.5 Sezioni REL

Le sezioni REL vanno controllate tutte, anche quelle che riguardano sezioni non caricate: il compito dice che per quelle basta che i record stiano dentro il file. Si verifica quindi che:

- `entsize == 8`;
- `size` sia multiplo di 8;
- `[offset, offset + size)` stia dentro il file (`elf_fits(..., v->size)`).

Il controllo sta prima del `continue` sulle sezioni non allocate, perché le REL non hanno `SHF_ALLOC`.

### 4.6 Sezioni non allocate

Senza `SHF_ALLOC` la sezione non finisce in memoria (debug, stringhe, simboli, REL). Dopo i controlli sopra si passa alla successiva.

### 4.7 La sezione sta nello slot

```c
if ( !elf_fits( sh.addr, sh.size, v->load_end ) )
    return -4;
```

Verifica che `[addr, addr + size)` stia dentro `load_end`. `elf_fits` è overflow-safe: non si fa mai `addr + size` a mano prima del controllo.

### 4.8 I byte nel file (solo se non NOBITS)

Le sezioni NOBITS (`.bss`) non hanno contenuto nel file, quindi basta il controllo in memoria. Le altre devono stare nei byte effettivamente caricati. Si rifiuta se:

- `sh.offset < v->file_off`: la sezione inizia prima dei byte caricati. Il controllo viene **prima** della sottrazione per evitare underflow senza segno;
- `!elf_fits(sh.offset - file_off, sh.size, file_size)`: la sezione esce dalla parte caricata.

### 4.9 Read-only prima delle scrivibili

```c
if ( sh.flags & SHF_WRITE )
    seen_write = 1;
else if ( seen_write )
    return -4;
```

Il criterio è il flag `SHF_WRITE`, non la distinzione codice/dati.

| Gruppo | Esempi |
|---|---|
| Non scrivibili | `.text`, `.rodata` |
| Scrivibili | `.data`, `.bss` |

Tutto il primo gruppo deve precedere il secondo nell'ordine della tabella. Nel compito questa regola compare esplicitamente solo per il convertitore della versione small; per la big viene dal TODO ed è coerente con W^X (B2): serve un confine netto tra la parte read-only e quella scrivibile dello slot.

### 4.10 Codice, `code_end` ed entry

Per ogni sezione con `SHF_EXECINSTR`:

- `code_end` prende il massimo `addr + size`. La somma non può overfloware perché il punto 4.7 ha già garantito `addr + size <= load_end`;
- se `entry` cade in `[addr, addr + size)`, allora `entry_ok = 1`. Si scrive `entry - sh.addr < sh.size` dopo aver controllato `entry >= sh.addr`, per evitare overflow.

### 4.11 Risultato

Se nessuna sezione eseguibile contiene l'entry (compreso il caso senza codice), `entry_ok` resta 0 e si rifiuta. Altrimenti si scrive `p->code_end` e si ritorna `0`. Il `load_plan` si modifica solo se tutti i controlli sono passati.

## 5. Cosa NON fa questa funzione

Da fare in `reloc.c` o in `elf.c`, senza duplicarlo qui:

- `sh_link` e `sh_info` delle REL: indici validi (`< shnum`), con `info` che punta alla sezione target e `link` alla tabella dei simboli;
- i record di relocation: sito dentro la sezione target, tipo ammesso, simbolo definito;
- la stampa `loader: unsupported relocation type <type>` seguita dal rifiuto normale;
- la verifica dei PT_LOAD e il calcolo di `file_off`, `file_size`, `load_end` (fatti da `elf.c`).

## 6. Dubbi aperti

1. **`code_end`**: il compito non lo definisce. Se nel progetto significa "fine della parte read-only", va calcolato come massimo su tutte le sezioni allocate non scrivibili. Controllare `loader.h` e il codice MPU.
2. **Ordine delle sezioni**: la regola viene dal TODO e da W^X. Va tenuta solo se coerente con la divisione code/data dell'MPU.
3. **Consistenza tra file e memoria**: la funzione non verifica che `addr == offset - file_off`. Se `elf.c` copia il file in un blocco unico a partire da `file_off`, questa uguaglianza è implicita nel loader e conviene controllarla; se i PT_LOAD sono più di uno, non vale in generale.
4. **BSS**: se il loader copia `file_size` byte e poi azzera il resto fino a `load_end`, una NOBITS che cade nell'area copiata non risulterebbe a zero. In quel caso aggiungere `sh.addr >= v->file_size` per le NOBITS.