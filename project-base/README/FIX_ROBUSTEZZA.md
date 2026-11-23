# Fix di robustezza del loader e della MPU

Documentazione del progetto Scratch-OS (loader di programmi, versione big).

Commit `f6259c7` sul branch `fix-svc12-makefile`. Questi quattro problemi sono
emersi nella revisione di `main` dopo l'integrazione (`beb56c7`). Nessuno dei
quattro cambia il comportamento con i programmi validi o con il test kit: small
10/10 e big 14/14 prima e dopo. Cambia cosa succede con file ELF rovinati, o se
in futuro qualcuno toglie per sbaglio una riga importante.

| # | Problema | File | Prima | Dopo |
|---|---|---|---|---|
| 1a | posizione nel file e indirizzo di una sezione non confrontati | `kernel/elf_sections.c` | file caricato | rifiutato |
| 1b | stesso sito `R_ARM_ABS32` presente due volte | `kernel/loader.c` | caricato, poi MemManage | rifiutato |
| 1c | siti THM controllati con `code_end` invece che con la loro sezione | `kernel/loader.c` | caricato, poi **fault irrecuperabile** | rifiutato |
| 2 | fallback MPU che dà uno slot RWX se `code_size` è 0 | `kernel/task.c` | slot RWX in silenzio | fault immediato |

---

## Contesto: due "indirizzi" per ogni sezione

Ogni sezione di un ELF (`.text`, `.data`, …) ha due numeri:

- **`sh_offset`**: dove stanno i suoi byte **nel file**;
- **`sh_addr`**: dove devono finire **in memoria**, cioè quanti byte dopo
  l'inizio dello slot.

Il loader copia **un blocco unico**: i byte del file da `file_off` in poi vanno
nello slot da 0 in poi. Il byte che nel file sta a `file_off + X` finisce nello
slot a `X`.

In un file prodotto dal linker vale sempre `sh_offset - file_off == sh_addr`.
Per esempio, in `print_plain` `file_off` = 0x1000 e `.text` ha
`sh_offset` = 0x1000 e `sh_addr` = 0.

Le **rilocazioni** sono la lista dei punti (*siti*) da correggere. Ogni sezione
di rilocazioni dice, nel campo `sh_info`, a quale sezione si riferisce:
`.rel.text` corregge `.text`. Un sito è un indirizzo **nello slot**, come
`sh_addr`.

---

## 1a. La posizione nel file deve corrispondere all'indirizzo

**Cosa dice la traccia.** §6.3: il kernel controlla "the loadable segments and
the loaded sections it needs to find relocation targets", e che "every
calculation, table, record, segment, section, relocation site and copied range
fits inside the file and the slot".

**Il problema.** Prima di rilocare, `elf_check_relocs` controlla che
`slot_base + parola` non superi i 32 bit. Per leggere la parola usa la
**sezione**: `sh_offset + (sito - sh_addr)`. La copia nello slot invece usa il
**blocco unico**: `file_off + sito`. Se `sh_offset - file_off` è diverso da
`sh_addr`, i due calcoli leggono byte diversi.

**Esempio.** Un file rovinato dice che `.text` ha `sh_offset` = 0x1004, cioè 4
byte più avanti del vero. Per il sito 0x100:

- il **controllo** legge la parola a 0x1004 + 0x100 = **0x1104**;
- nello slot finisce la parola che sta a 0x1000 + 0x100 = **0x1100**.

La parola rilocata non è quella controllata. Il controllo "la sezione sta nella
parte caricata del file" passava lo stesso, perché spostata di 4 byte la
sezione stava ancora dentro.

**Il fix** (`elf_check_sections`, dentro il blocco delle sezioni che hanno byte
nel file):

```c
            /* the bytes of the section are copied to sh_addr: the file
             * position must match, or the relocation checks would read
             * different bytes from the ones that end up in the slot */
            if ( sh.offset - v->file_off != sh.addr )
            {
                return -4;
            }
```

- `sh.offset - v->file_off` non va mai sotto zero: il controllo subito prima
  garantisce `sh.offset >= v->file_off`.
- `.bss` (`SHT_NOBITS`) non è toccata, perché non ha byte nel file.
- Nei file veri del linker l'uguaglianza vale sempre, quindi i programmi validi
  passano come prima.

---

## 1b. Lo stesso sito ABS32 non può comparire due volte

**Cosa dice la traccia.** Per SFT1 il §6.2 dice che ogni rilocazione "appears
only once". Per gli ELF della versione big il testo **non dice niente** sui
duplicati: è una difesa in più, coerente con la regola di SFT1.

**Il problema.** `R_ARM_ABS32` vuol dire "aggiungi alla parola l'indirizzo dello
slot". Con due record per lo stesso sito, `elf_apply_relocs` lo aggiunge
**due volte**.

**Esempio.** Un puntatore a una variabile a 0x400, programma nello slot 0, che
parte a 0x20002000:

- con un record: 0x400 + 0x20002000 = **0x20002400**, la variabile;
- con due record: 0x20002400 + 0x20002000 = **0x40004400**, un indirizzo che
  non esiste. Il programma va in MemManage.

**Il fix.** I siti ABS32 sono multipli di 4 e stanno nei primi 3072 byte, quindi
sono al massimo 3072 / 4 = **768**. Basta **un bit per posizione**: 768 bit,
cioè 24 parole da 32 bit, 96 byte sullo stack del kernel.

All'inizio di `elf_check_relocs`:

```c
    /* One bit per 4-byte word of the program (3072 / 4 = 768 words):
     * an ABS32 site seen twice would get the slot base added twice. */
    uint32_t abs_seen[3072u / 4u / 32u];
    for ( uint32_t k = 0; k < 3072u / 4u / 32u; k++ )
    {
        abs_seen[k] = 0;
    }
```

Nel ramo `R_ARM_ABS32`, dopo gli altri controlli:

```c
                /* site < bss_start <= 3072 here, so site / 4 < 768 */
                uint32_t w = site / 4u;
                if ( abs_seen[w / 32u] & ( 1u << ( w % 32u ) ) )
                {
                    return -4;
                }
                abs_seen[w / 32u] |= 1u << ( w % 32u );
```

Esempio di calcolo: per il sito 0x400 = 1024, `w` = 256. La parola è
`abs_seen[256 / 32]`, cioè `abs_seen[8]`, e il bit è 256 % 32 = 0. Il secondo
record con lo stesso sito trova il bit già acceso e il file viene rifiutato.

- **Non esce dall'array.** Arrivati lì, `fits( site, 4, p->bss_start )` ha già
  garantito `site` < 3072, quindi `w` < 768.
- **Ciclo invece di `= { 0 }`.** Senza libreria C, `= { 0 }` può diventare una
  chiamata a `memset`, che nel kernel non esiste. Verificato con `objdump`:
  `memset` non compare.
- **Efficienza.** Un solo passaggio sui record. Un doppio ciclo "ogni record
  contro tutti gli altri" sarebbe quadratico, e lentissimo con un file rovinato
  pieno di record.
- Le sezioni non caricate (debug) sono saltate prima, quindi i loro record non
  occupano bit.

---

## 1c. I siti THM devono stare nella sezione a cui si riferiscono

**Cosa dice la traccia.** §6.3: il kernel controlla "the loaded sections it
needs to find relocation targets", e ogni "relocation site" deve stare nel file
e nello slot.

**Il problema.** Per `R_ARM_ABS32` il codice controllava che il sito stesse
dentro la sezione indicata da `sh_info`. Per `R_ARM_THM_CALL` e
`R_ARM_THM_JUMP24` (le istruzioni BL e B.W) controllava solo che stesse prima
di `code_end`, senza guardare la sezione indicata.

**Esempio.** Un file rovinato ha `.rel.text` con `sh_info` che punta a `.data`,
e tutti i record di tipo `R_ARM_THM_CALL` con siti dentro `.text`. Il controllo
vecchio diceva "il sito è prima di `code_end`, va bene". Le rilocazioni del
codice non venivano applicate, il programma partiva con indirizzi sbagliati e il
sistema si fermava con:

```
MemManage: unrecoverable CFSR=0x92 HFSR=0x0
```

È il caso peggiore: per il §6.6 un fault durante il salvataggio dello stack
ferma il sistema, senza più il prompt.

**Il fix.** Lo stesso controllo di ABS32, aggiunto a quello che c'era:

```c
                if ( site < tgt_addr || !fits( site - tgt_addr, 4, tgt_size ) || !fits( site, 4, p->code_end ) )
                {
                    return -4;
                }
```

Le tre condizioni dicono:

1. il sito non è prima dell'inizio della sezione bersaglio;
2. i 4 byte dell'istruzione stanno dentro la sezione bersaglio;
3. stanno anche nel codice, come prima.

`site - tgt_addr` viene calcolato solo quando `site >= tgt_addr`, perché `||`
si ferma alla prima condizione vera.

---

## 2. Mai una regione eseguibile su tutto lo slot

**Cosa dice la traccia.** B2 e §6.4: codice lettura + esecuzione; dati e stack
lettura + scrittura, mai esecuzione. §6.7: i team big "may … extend the MPU setup
done at each task switch, so that code and data get different permissions".

**Il problema.** In `task_switch_context`:

```c
    if ( next->executable && next->code_size > 0 )
    {
        mpu_set_program_regions( ... );        /* W^X */
    }
    else
    {
        mpu_set_task_region( ..., next->executable );
    }
```

Per i programmi `executable` vale 1 (`LOADER_EXECUTABLE`). Se `code_size`
restasse 0, il ramo `else` creerebbe una regione unica leggibile, scrivibile ed
**eseguibile** su tutto lo slot: W^X sparirebbe senza nessun messaggio. È
esattamente il bug 2 del merge (vedi `INTEGRAZIONE.md`). Oggi non succede,
perché `task_program_set_code_size` viene chiamata nella stessa SVC 13, ma
basterebbe spostare o cancellare quella riga.

**Il fix.** Nel ramo `else` si passa sempre `0`, cioè non eseguibile:

```c
        /* Big version: a whole-slot region is never executable. A
         * program without code_size faults instead of running RWX. */
        mpu_set_task_region( next->part_start, next->part_end - next->part_start, 0 );
```

- **Shell e idle:** avevano già `executable = 0`, quindi non cambia niente. Il
  loro codice è in FLASH, che ha la sua regione eseguibile.
- **Un programma senza `code_size`:** alla prima istruzione va in
  `MemManage in loaded program: CFSR=0x1`, viene chiuso e la shell torna al
  prompt. Il fallimento è sicuro e visibile, invece che silenzioso.

---
