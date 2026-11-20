# `elf_check_layout` (versione big): header e segmenti

Documentazione del progetto Scratch-OS (loader di programmi) · Gruppo 6 · Versione Big · Parte A, `kernel/elf.c` (Gianluigi)

## 1. Cosa fa questa parte

Quando l'utente scrive `run <nome>`, la SVC 13 passa per una catena di sei piani. Il **piano 2** risponde a una sola domanda: *questo file ELF si può caricare in uno slot, e come?*

- Riceve da Antonio (piano 1) solo due cose: `f`, il puntatore al primo byte del file ELF dentro l'immagine in FLASH, e `size`, la sua lunghezza. Non conosce né il nome del programma né lo slot: non gli servono, perché il controllo è uguale per qualunque slot.
- Restituisce `0` e riempie `struct load_plan` con i numeri che servono per caricare il programma, oppure `-4` se il file non si può caricare. Con `-4` la shell stampa `error: program cannot be loaded`.

Il piano 2 sta **sopra la linea di non ritorno**: legge soltanto, non scrive mai nello slot. Se un file è rovinato, non deve cambiare niente e il kernel non deve andare in fault.

Il piano 2 è diviso in due file:

| Passo della pipeline | Cosa controlla | File | Chi |
|---|---|---|---|
| 2.1 | header ELF e posizione delle due tabelle | `kernel/elf.c`, `check_header()` | Gianluigi |
| 2.2 | segmenti PT_LOAD | `kernel/elf.c`, `check_segments()` | Gianluigi |
| 2.3, 2.4, 2.5 | sezioni, `code_end`, entry point | `kernel/elf_sections.c`, `elf_check_sections()` | Athena (vedi `ELF_SECTION.md`) |
| 2.6 | riempimento del `load_plan` | `kernel/elf.c`, `elf_check_layout()` | Gianluigi |

Questo file descrive tutto quello che c'è in `kernel/elf.c`.

## 2. I file

| File | Cosa contiene | Chi lo modifica |
|---|---|---|
| `kernel/elf.c` | le funzioni comuni `elf_read16`, `elf_read32`, `elf_fits`; `check_header`, `check_segments`, `elf_check_layout` | Gianluigi |
| `kernel/elf_internal.h` | costanti ELF, `struct elf_view`, `struct elf_shdr`, prototipi interni | solo d'accordo fra Gianluigi, Athena e Simone |
| `kernel/loader.h` | `struct load_plan` e le firme pubbliche del loader (`elf_check_layout`, `elf_check_relocs`, `elf_apply_relocs`) | d'accordo con tutto il team |

`elf_check_layout` è l'unica funzione del piano 2 che gli altri piani vedono: è dichiarata in `loader.h`. Tutto il resto è interno al loader.

## 3. Le regole del file

1. **Mai leggere un byte fuori da `[0, size)`.** Ogni offset e ogni lunghezza vengono da un file che può essere rovinato apposta, quindi si controllano con `elf_fits()` *prima* di usarli (assignment 6.3, versione big).
2. **I soli risultati sono `0` e `-4`.** Un file sbagliato non deve mai far andare in fault il kernel.
3. **Solo lettura.** Il piano 2 non scrive nello slot.
4. **Niente variabili globali.** I dati passano per puntatore; le funzioni interne sono `static`.

**I nomi.** Lo standard ELF dà ai campi un prefisso che dice dove stanno: `e_` nell'header, `p_` nella riga di un segmento, `sh_` nella riga di una sezione. Nel nostro codice il prefisso sparisce: `e_phnum` viene letto in `v->phnum`, `p_vaddr` in `vaddr`. È lo stesso numero con due nomi: uno nel file, uno nella variabile.

## 4. Richiami sul file ELF

Un file ELF è una fila di byte. `f[k]` è il byte all'offset `k`, `f + k` è il puntatore a quel byte. Al piano 2 interessano tre zone:

```
offset 0       +--------------------------------+
               | ELF header        52 byte      |  identità, entry, posizione delle tabelle
offset e_phoff +--------------------------------+
               | program header table           |  una riga da 32 byte per segmento
               +--------------------------------+
               | ... byte dei segmenti, debug,  |
               |     simboli, rilocazioni ...   |
offset e_shoff +--------------------------------+
               | section header table           |  una riga da 40 byte per sezione
               +--------------------------------+  <- size (fine del file)
```

- Un **segmento** è un blocco di byte del file da copiare in memoria, a un indirizzo stabilito. Lo descrive una riga del program header.
- Una **sezione** è un pezzo in cui il linker organizza il file (`.text`, `.data`, `.rel.text`, `.symtab`, …). I segmenti raggruppano le sezioni da caricare.

### I campi dell'header che usiamo

Ogni campo sta sempre allo stesso offset. Quelli da 1 byte si leggono con `f[offset]`, quelli da 2 byte con `elf_read16( f + offset )`, quelli da 4 byte con `elf_read32( f + offset )`. Le macro `EH_*` di `elf_internal.h` sono le **posizioni** dei campi, non i loro valori.

| Byte | Campo | Macro | Dim. | Cosa dice | Valore richiesto | Perché |
|--:|---|---|--:|---|---|---|
| 0..3 | magic | — | 4 | «sono un file ELF» | `7f 45 4c 46` | standard ELF; assignment §2 |
| 4 | `EI_CLASS` | `EH_CLASS` | 1 | 32 o 64 bit | 1 = 32 bit | il Cortex-M3 è a 32 bit; assignment 6.3 |
| 5 | `EI_DATA` | `EH_DATA` | 1 | ordine dei byte | 1 = little-endian | `elf_read16/32` leggono in little-endian; 6.3 |
| 6 | `EI_VERSION` | `EH_VERSION` | 1 | versione ELF | 1 | lo standard definisce solo la versione 1; 6.3 |
| 16 | `e_type` | `EH_TYPE` | 2 | tipo di file | 2 = `ET_EXEC` | programma già linkato; un `.o` (1) ha simboli irrisolti, una libreria (3) richiede linking dinamico; 6.3 |
| 18 | `e_machine` | `EH_MACHINE` | 2 | processore | 40 = `EM_ARM` | codice per ARM; 6.3 |
| 20 | `e_version` | `EH_VERSION2` | 4 | versione ELF (di nuovo) | 1 | come il byte 6; 6.3 |
| 24 | `e_entry` | `EH_ENTRY` | 4 | offset della prima istruzione, bit 0 = Thumb | lo controlla Athena | — |
| 28 | `e_phoff` | `EH_PHOFF` | 4 | dove comincia la tabella dei segmenti | tabella dentro il file | 6.3, versione big |
| 32 | `e_shoff` | `EH_SHOFF` | 4 | dove comincia la tabella delle sezioni | tabella dentro il file | 6.3, versione big |
| 40 | `e_ehsize` | `EH_EHSIZE` | 2 | lunghezza dell'header | 52 | 6.3 |
| 42 | `e_phentsize` | `EH_PHENTSIZE` | 2 | lunghezza di **una** riga dei segmenti | 32 | 6.3 |
| 44 | `e_phnum` | `EH_PHNUM` | 2 | **quante** righe (segmenti) | almeno 1 | 6.3: almeno un PT_LOAD |
| 46 | `e_shentsize` | `EH_SHENTSIZE` | 2 | lunghezza di **una** riga delle sezioni | 40 | 6.3 |
| 48 | `e_shnum` | `EH_SHNUM` | 2 | **quante** righe (sezioni) | almeno 1 | scelta nostra: senza sezioni Athena e Simone non hanno niente da controllare |

I campi `e_ident[7..15]`, `e_flags` (36) ed `e_shstrndx` (50) non si usano.

**`…entsize` e `…num`.** Le due tabelle sono fatte di righe tutte uguali. La riga `i` sta a `inizio della tabella + i × lunghezza di una riga`. `e_phentsize` ed `e_shentsize` dicono quanto è lunga una riga; `e_phnum` ed `e_shnum` quante righe ci sono. Il codice usa le lunghezze fisse 32 e 40, quindi controlla che il file dica proprio quelle: altrimenti leggerebbe i campi nel punto sbagliato.

### I campi di una riga del program header

| Offset nella riga | Campo | Variabile nel codice | Cosa dice |
|--:|---|---|---|
| +0 | `p_type` | `type` | tipo: 1 = `PT_LOAD` (da caricare), 2 = `PT_DYNAMIC` (linking dinamico) |
| +4 | `p_offset` | `offset` | dove cominciano i byte del segmento **nel file** |
| +8 | `p_vaddr` | `vaddr` | dove va il segmento **nello slot** (offset, perché i programmi sono linkati a 0) |
| +12 | `p_paddr` | — | non usato |
| +16 | `p_filesz` | `filesz` | quanti byte si prendono dal file |
| +20 | `p_memsz` | `memsz` | quanti byte occupa in memoria; la parte in più è BSS da azzerare |
| +24 | `p_flags` | — | permessi R/W/X: non usato, il W^X viene da `code_end` |
| +28 | `p_align` | — | non usato |

### Esempio: `global_state.elf`

Valori presi da `arm-none-eabi-readelf -l -S build/programs/global_state.elf` sulla nostra build:

| Cosa | Valore |
|---|---|
| entry | `0x1` |
| tabella dei segmenti | `e_phoff` = 52, `e_phnum` = 1 |
| unico segmento | `LOAD  offset 0x010000  vaddr 0x00000000  filesz 0x00314  memsz 0x00314  RWE` |
| tabella delle sezioni | `e_shoff` = `0x1363c`, `e_shnum` = 24, quindi finisce a `0x1363c + 24 × 40 = 0x139fc` |

Tutti i 9 programmi del test kit hanno un solo PT_LOAD con `p_offset = 0x10000` e `p_vaddr = 0`.

## 5. Le strutture dati

### `struct elf_view` (in `elf_internal.h`): il quaderno del piano 2

È una variabile locale di `elf_check_layout`, sullo stack: esiste solo durante la chiamata. La scrive **solo** `elf.c`; Athena la legge.

| Campo | Contenuto | Scritto da | Letto da |
|---|---|---|---|
| `f` | il file | `check_header` | `check_segments`, Athena |
| `size` | lunghezza del file | `check_header` | `check_segments`, Athena |
| `entry` | `e_entry`, non ancora controllato | `check_header` | Athena (2.5), `elf_check_layout` (2.6) |
| `phoff`, `phnum` | posizione e numero di righe della tabella dei segmenti, già dentro il file | `check_header` | `check_segments` |
| `shoff`, `shnum` | posizione e numero di righe della tabella delle sezioni, già dentro il file | `check_header` | Athena |
| `file_off` | offset nel file del primo byte da copiare | `check_segments` | `elf_check_layout`, Athena |
| `file_size` | quanti byte copiare | `check_segments` | `elf_check_layout`, Athena |
| `load_end` | fine del programma in memoria, ≤ 3072 | `check_segments` | `elf_check_layout`, Athena |

### `struct load_plan` (in `loader.h`): il risultato

Tutti i campi sono **offset nello slot**, contati da 0, tranne `file_off`, che è un offset **nel file**. L'indirizzo vero in RAM è `slot_base + campo`.

| Campo | Contenuto | Chi lo scrive | Chi lo usa | `global_state` |
|---|---|---|---|---|
| `entry` | prima istruzione, bit 0 = 1 (Thumb) | Gianluigi (da `v.entry`) | piano 6: pc iniziale = `slot_base + entry` | `0x1` |
| `file_off` | da dove copiare | Gianluigi | piano 4: sorgente = `f + file_off` | `0x10000` |
| `copy_size` | quanti byte copiare | Gianluigi | piano 4 | `0x314` |
| `bss_start` | inizio della zona da azzerare (= `copy_size`) | Gianluigi | piani 3 e 4 | `0x314` |
| `bss_end` | fine della zona da azzerare | Gianluigi | piano 4 | `0x314` |
| `code_end` | confine fra parte RX e parte RW | Athena | piano 3, MPU (Giulio) | vedi `ELF_SECTION.md` |
| `mem_end` | fine del programma, ≤ 3072 | Gianluigi | controllo del limite | `0x314` |

### Le costanti

| Costante | Valore | Dove | Significato |
|---|---|---|---|
| `SLOT_PROG_MAX` | 3072 | `elf.c` | spazio massimo per codice + dati + BSS nello slot. Lo slot è di 4096 byte e i 1024 alti sono lo stack (assignment 6.4) |
| `ELF_EHDR_SIZE`, `ELF_PHDR_SIZE`, `ELF_SHDR_SIZE` | 52, 32, 40 | `elf_internal.h` | lunghezze obbligatorie di header, riga dei segmenti, riga delle sezioni (6.3). EHDR = ELF Header, PHDR = Program Header, SHDR = Section Header |
| `EH_*` | 4 … 48 | `elf_internal.h` | posizioni dei campi dell'header (EH = ELF Header) |
| `ET_EXEC`, `EM_ARM` | 2, 40 | `elf_internal.h` | valori obbligatori di `e_type` ed `e_machine` |
| `PT_LOAD`, `PT_DYNAMIC` | 1, 2 | `elf_internal.h` | tipi di segmento (PT = Program header Type) |

## 6. Le funzioni comuni

Le usano anche Athena (`elf_sections.c`) e Simone (rilocazioni), quindi nomi e firme sono fissati da `elf_internal.h`.

### `elf_read16` ed `elf_read32`

```c
uint32_t elf_read16( const uint8_t *p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 );
}

uint32_t elf_read32( const uint8_t *p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 )
           | ( (uint32_t) p[3] << 24 );
}
```

Mettono insieme 2 o 4 byte little-endian: il primo byte è la parte bassa del numero. Esempi:

```
19 00        -> 0x0019     = 25        (e_shnum)
3c 36 01 00  -> 0x0001363c             (e_shoff di global_state)
```

- Ogni byte è convertito a `uint32_t` **prima** dello spostamento: senza il cast, `p[3] << 24` verrebbe calcolato su un `int` con segno e andrebbe in overflow quando `p[3] >= 0x80`.
- `|` mette insieme i pezzi: funziona come una somma, perché i pezzi non hanno bit in comune.
- Si legge byte per byte, invece di `*(uint32_t *) p`, perché `p` viene dal file e può essere un indirizzo non allineato: sul Cortex-M3 un accesso non allineato può causare un fault.
- Le due funzioni **non controllano niente**: chi le chiama deve sapere già che i byte sono dentro il file.

### `elf_fits`

```c
int elf_fits( uint32_t off, uint32_t len, uint32_t size )
{
    return off <= size && len <= size - off;
}
```

Risponde 1 se l'intervallo `[off, off + len)` sta tutto dentro `[0, size)`, altrimenti 0.

| Parametro | Significato | Esempio |
|---|---|---|
| `off` | dove comincia l'intervallo, contato dall'inizio del contenitore | `e_shoff` = `0x1363c` |
| `len` | quanti byte è lungo | `e_shnum × 40` = 960 |
| `size` | quanto è grande il contenitore: la lunghezza del file, oppure `SLOT_PROG_MAX` per lo slot | lunghezza del file |

**Perché non `off + len <= size`.** La somma può superare 32 bit e ripartire da 0: con `off = 0xFFFFFFF0` e `len = 0x20` darebbe `0x10`, e il controllo direbbe «ci sta». La versione usata non fa somme: prima controlla che l'inizio sia dentro, poi che la lunghezza stia nello spazio che resta (`size - off`). La sottrazione è sicura perché `&&` la calcola solo se la prima condizione è vera.

| Chiamata | Risultato | Perché |
|---|---|---|
| `elf_fits( 0, 3072, 3072 )` | 1 | arriva esatto alla fine |
| `elf_fits( 0, 3073, 3072 )` | 0 | un byte di troppo (`too_large`) |
| `elf_fits( 3073, 0, 3072 )` | 0 | l'inizio è già fuori |
| `elf_fits( 0xFFFFFFF0, 0x20, 3072 )` | 0 | overflow evitato |

## 7. `check_header()`: passo 2.1

```c
static int check_header( const uint8_t *f, uint32_t size, struct elf_view *v );
```

Legge l'header, lo controlla e riempie la prima parte di `v`. Dopo che ha restituito 0, ogni campo dell'header e tutte e due le tabelle sono garantiti dentro il file. Ogni `if` descrive il caso **sbagliato** e fa `return -4`; le condizioni sono unite con `||`, perché basta una cosa sbagliata per rifiutare il file.

### 2a · L'header è tutto nel file

```c
if ( !elf_fits( 0, ELF_EHDR_SIZE, size ) )
    return -4;
```

Tutti i campi letti dopo stanno nei byte 0..51. Questo controllo viene per primo: senza di lui, già leggere il magic potrebbe andare fuori dal file.

### 2b · Il magic

```c
if ( f[0] != 0x7f || f[1] != 'E' || f[2] != 'L' || f[3] != 'F' )
    return -4;
```

`'E'`, `'L'`, `'F'` sono i byte `0x45`, `0x4c`, `0x46`. Il test `bad_magic` scrive `"BAD!"` al loro posto.

### 2c · Classe, ordine dei byte, versione

```c
if ( f[EH_CLASS] != 1 || f[EH_DATA] != 1 || f[EH_VERSION] != 1 )
    return -4;
```

Campi da 1 byte, letti direttamente. Il test `bad_header` mette la classe a 2 (64 bit).

### 3a · Tipo, macchina, versione

```c
if ( elf_read16( f + EH_TYPE ) != ET_EXEC
     || elf_read16( f + EH_MACHINE ) != EM_ARM
     || elf_read32( f + EH_VERSION2 ) != 1 )
    return -4;
```

`e_type` ed `e_machine` sono da 2 byte (`elf_read16`), `e_version` da 4 (`elf_read32`). Usare la funzione sbagliata vorrebbe dire leggere metà numero, oppure il numero più due byte del campo dopo.

### 3b · Le lunghezze dei record

```c
if ( elf_read16( f + EH_EHSIZE ) != ELF_EHDR_SIZE
     || elf_read16( f + EH_PHENTSIZE ) != ELF_PHDR_SIZE
     || elf_read16( f + EH_SHENTSIZE ) != ELF_SHDR_SIZE )
    return -4;
```

Se un file dicesse «le mie righe sono lunghe 56 byte», la riga 1 starebbe davvero a +56; noi la cercheremmo a +32 e leggeremmo pezzi di righe diverse mescolati. Invece di adattarsi, il codice rifiuta il file.

### 4a · Si riempie il quaderno

```c
v->f = f;
v->size = size;
v->entry = elf_read32( f + EH_ENTRY );
v->phoff = elf_read32( f + EH_PHOFF );
v->phnum = elf_read16( f + EH_PHNUM );
v->shoff = elf_read32( f + EH_SHOFF );
v->shnum = elf_read16( f + EH_SHNUM );
```

Dentro `check_header`, `v` è un **puntatore** (la struct vera sta in `elf_check_layout`), quindi si scrive `v->campo`. `entry` viene solo copiato: per sapere se punta dentro il codice serve `code_end`, che calcola Athena, quindi il controllo lo fa lei.

### 4b · Le due tabelle stanno nel file

```c
if ( v->phnum == 0 || !elf_fits( v->phoff, v->phnum * ELF_PHDR_SIZE, size ) )
    return -4;
if ( v->shnum == 0 || !elf_fits( v->shoff, v->shnum * ELF_SHDR_SIZE, size ) )
    return -4;
```

```
global_state:  segmenti da 52 a 52 + 1 × 32 = 84                    <= size
               sezioni  da 0x1363c a 0x1363c + 24 × 40 = 0x139fc    <= size
```

- Ogni tabella deve avere almeno una riga: senza segmenti non c'è niente da caricare, senza sezioni Athena e Simone non hanno niente da controllare.
- La moltiplicazione non va in overflow: `phnum` e `shnum` sono campi da 2 byte (al massimo 65535), e `65535 × 40` sta comodamente in 32 bit.
- La tabella delle sezioni è l'ultima cosa del file. Per questo il test `truncated_elf`, che taglia il file subito prima della fine del segmento, si ferma già qui.

Dopo questi due controlli, `check_segments`, Athena e Simone possono leggere qualunque riga delle due tabelle senza ricontrollare. Simone legge `e_shoff` ed `e_shnum` direttamente dall'header contando proprio su questa garanzia.

## 8. `check_segments()`: passo 2.2

```c
static int check_segments( struct elf_view *v );
```

Scorre una volta tutte le righe del program header, controlla ogni PT_LOAD e alla fine scrive `v->file_off`, `v->file_size` e `v->load_end`.

### Le variabili

| Variabile | Parte da | Significato | Alla fine |
|---|---|---|---|
| `i` | 0 | numero della riga, da 0 a `phnum − 1` | — |
| `delta` | 0 | `p_offset − p_vaddr`: «il byte che va all'offset X dello slot sta all'offset X + delta del file» | diventa `v->file_off` |
| `have_delta` | 0 | diventa 1 al primo PT_LOAD con byte nel file | deve valere 1 |
| `min_addr` | 3072 | il `vaddr` più basso fra i PT_LOAD | deve valere 0 |
| `file_end` | 0 | fine, in memoria, dei byte che vengono dal file | diventa `v->file_size` |
| `load_end` | 0 | fine, in memoria, di tutti i PT_LOAD (BSS compreso) | diventa `v->load_end` |

I valori di partenza sono scelti «al contrario» (`min_addr` dal massimo, `file_end` e `load_end` dal minimo), così il primo segmento li aggiorna sicuramente.

### Una sola copia: perché

`load_plan` ha **un solo** `file_off` e **un solo** `copy_size`: Antonio copia un unico blocco dal file allo slot. Per questo `check_segments` impone due regole più severe dell'assignment (scelta del team):

- tutti i PT_LOAD con byte nel file hanno lo stesso `p_offset − p_vaddr`;
- il BSS di un segmento comincia solo dopo l'ultimo byte copiato.

I nostri programmi hanno un solo PT_LOAD e le rispettano sempre. Se `prog.ld` cambia e produce più segmenti, va ricontrollato con `readelf -l`.

### 5a · Lettura della riga

```c
const uint8_t *ph = v->f + v->phoff + i * ELF_PHDR_SIZE;
uint32_t type = elf_read32( ph + 0 );
uint32_t offset = elf_read32( ph + 4 );
uint32_t vaddr = elf_read32( ph + 8 );
uint32_t filesz = elf_read32( ph + 16 );
uint32_t memsz = elf_read32( ph + 20 );
```

`ph` punta all'inizio della riga `i`. Leggere è sicuro perché `check_header` ha già verificato che tutta la tabella sta nel file.

### 5b · Il tipo

```c
if ( type == PT_DYNAMIC )
    return -4;
if ( type != PT_LOAD || memsz == 0 )
    continue;
```

- `PT_DYNAMIC` vuol dire linking dinamico, vietato (assignment 6.3).
- Ogni altro tipo, e un PT_LOAD vuoto, non ha niente da caricare: `continue` salta alla riga successiva. Un `return` qui chiuderebbe tutta la funzione e le righe dopo non verrebbero controllate.

### 5c · Il segmento sta nello slot e nel file

```c
if ( filesz > memsz
     || !elf_fits( vaddr, memsz, SLOT_PROG_MAX )
     || !elf_fits( offset, filesz, v->size ) )
    return -4;
```

Ci sono due contenitori diversi:

- `[vaddr, vaddr + memsz)` si controlla contro lo **slot**: deve stare nei 3072 byte bassi (assignment 6.4). È qui che si ferma `too_large`, che porta la fine del segmento a 3073.
- `[offset, offset + filesz)` si controlla contro il **file**: i byte del segmento devono esistere.
- `filesz > memsz` vorrebbe dire prendere dal file più byte di quanti il segmento ne occupa in memoria.

### 6a · Una copia sola (solo per i segmenti con byte nel file)

```c
if ( filesz != 0 ) {
    if ( offset < vaddr )
        return -4;
    if ( !have_delta ) {
        delta = offset - vaddr;
        have_delta = 1;
    } else if ( offset - vaddr != delta ) {
        return -4;
    }
    if ( vaddr + filesz > file_end )
        file_end = vaddr + filesz;
}
```

- `offset < vaddr` renderebbe `delta` «negativo»: con i numeri senza segno diventerebbe un valore enorme.
- Il primo segmento fissa `delta`; gli altri devono avere lo stesso, altrimenti servirebbe una seconda copia.
- `file_end` tiene la fine più alta dei byte presi dal file: è quanti byte copiare.

### 6b · Inizio e fine del programma

```c
if ( vaddr < min_addr )
    min_addr = vaddr;
if ( vaddr + memsz > load_end )
    load_end = vaddr + memsz;
```

Vale per **tutti** i PT_LOAD, anche quelli di solo BSS (`filesz = 0`): non hanno byte da copiare, ma occupano memoria. Per questo sta fuori dall'`if ( filesz != 0 )`. `vaddr + memsz` qui non va in overflow, perché il passo 5c ha già dimostrato che è ≤ 3072.

Esempio con due segmenti (codice e dati separati):

| Dopo la riga | `min_addr` | `load_end` |
|---|---|---|
| partenza | 3072 | 0 |
| A: `vaddr 0x000`, `memsz 0x400` | 0 | `0x400` |
| B: `vaddr 0x400`, `memsz 0x100` | 0 | `0x500` |

### 6c · Controlli dopo il ciclo

```c
if ( have_delta != 1 || min_addr != 0 || !elf_fits( delta, file_end, v->size ) )
    return -4;
```

- `have_delta == 1`: c'è almeno un PT_LOAD con byte nel file (assignment 6.3: «at least one PT_LOAD»).
- `min_addr == 0`: il programma è linkato a 0 e Antonio copia all'inizio dello slot, quindi il programma deve cominciare da lì.
- `[delta, delta + file_end)` dentro il file: la copia unica che farà Antonio (da `f + file_off`, `file_end` byte) non esce dal file.

### 6d · Secondo giro: il BSS dopo i byte copiati

```c
for ( i = 0; i < v->phnum; i++ ) {
    const uint8_t *ph = v->f + v->phoff + i * ELF_PHDR_SIZE;
    uint32_t vaddr = elf_read32( ph + 8 );
    uint32_t filesz = elf_read32( ph + 16 );
    uint32_t memsz = elf_read32( ph + 20 );

    if ( elf_read32( ph + 0 ) == PT_LOAD && memsz > filesz && vaddr + filesz < file_end )
        return -4;
}
```

Antonio copia `file_end` byte in un blocco e poi azzera il BSS. Se un segmento avesse la sua parte BSS (`memsz > filesz`) prima di `file_end`, la copia metterebbe byte del file dove servono zeri. Serve un secondo giro perché `file_end` è noto solo alla fine del primo.

### 6e · Il risultato

```c
v->file_off = delta;
v->file_size = file_end;
v->load_end = load_end;
return 0;
```

`global_state`: `file_off = 0x10000`, `file_size = 0x314`, `load_end = 0x314`.

## 9. `elf_check_layout()`: la funzione pubblica

```c
int elf_check_layout( const uint8_t *f, uint32_t size, struct load_plan *p )
{
    struct elf_view v;

    if ( check_header( f, size, &v ) != 0 || check_segments( &v ) != 0 )
        return -4;

    if ( elf_check_sections( &v, p ) != 0 )
        return -4;

    p->entry = v.entry;
    p->file_off = v.file_off;
    p->copy_size = v.file_size;
    p->bss_start = v.file_size;
    p->bss_end = v.load_end;
    p->mem_end = v.load_end;
    return 0;
}
```

- `v` è una variabile locale (niente globali). Alle funzioni interne si passa il suo indirizzo `&v`, così possono scriverci dentro.
- Con `||`, se `check_header` fallisce `check_segments` non parte: leggerebbe una tabella che nessuno ha controllato.
- Poi i passi 2.3–2.5 di Athena, che scrive `p->code_end`.
- Infine il passo 2.6: `v.campo` con il punto perché qui `v` è una struct, `p->campo` con la freccia perché `p` è un puntatore.
- Con `-4` il contenuto di `*p` non vale niente (Athena potrebbe aver già scritto `code_end`): chi chiama non deve usarlo.

**Perché `struct elf_view v;` e non `= { 0 }`.** Il kernel è compilato con `-nostdlib` e `-ffreestanding`. Con `-O0`, gcc trasforma l'azzeramento di una struct in una chiamata a `memset`, che senza libreria C non esiste: il link si ferma con `undefined reference to 'memset'`. L'azzeramento non serve, perché `check_header` e `check_segments` scrivono tutti i campi di `v` prima che qualcuno li legga. Sul PC questo errore non si vede, perché lì `memset` esiste: va sempre fatto anche il `make` del kernel.

## 10. Esempio completo: `global_state`

| Passo | Cosa succede |
|---|---|
| 2a | il file è più lungo di 52 byte |
| 2b, 2c | `7f 45 4c 46`, poi `01 01 01` |
| 3a, 3b | tipo 2, macchina 40, versione 1; record 52, 32, 40 |
| 4a | `entry 0x1`, `phoff 52`, `phnum 1`, `shoff 0x1363c`, `shnum 24` |
| 4b | segmenti fino a 84, sezioni fino a `0x139fc`: tutto nel file |
| 5a | riga 0: `type 1`, `offset 0x10000`, `vaddr 0`, `filesz 0x314`, `memsz 0x314` |
| 5b | è un PT_LOAD non vuoto |
| 5c | `0x314 ≤ 0x314`; `0 + 0x314 ≤ 3072`; `0x10000 + 0x314` nel file |
| 6a | `delta = 0x10000`, `have_delta = 1`, `file_end = 0x314` |
| 6b | `min_addr = 0`, `load_end = 0x314` |
| 6c | `have_delta` 1, `min_addr` 0, copia dentro il file |
| 6d | niente BSS |
| 6e | `file_off 0x10000`, `file_size 0x314`, `load_end 0x314` |
| 2.3–2.5 | Athena: sezioni ok, `code_end` |
| 2.6 | `load_plan`: `entry 0x1`, `file_off 0x10000`, `copy_size 0x314`, `bss 0x314..0x314`, `mem_end 0x314` |

Antonio copierà `0x314` byte da `f + 0x10000` a `slot_base`; il task partirà da `slot_base + 0x1`.

## 11. Come è stato provato

### Il test sul PC: `elf_steps.c`

`elf.c` non usa niente del kernel, quindi si può compilare con il `gcc` del PC insieme a un piccolo programma di prova, `elf_steps.c` (tenuto fuori dal repository, in `~/elf_steps.c`):

```bash
cd ~/team/project-base
make programs
gcc -std=gnu17 -Wall -Wextra -I. kernel/elf.c kernel/elf_sections.c ~/elf_steps.c -o /tmp/elf_steps
/tmp/elf_steps -s 7 build/programs/print_plain.elf build/programs/*.elf
```

Il programma prova i tre helper, costruisce 22 copie rovinate del primo file (ognuna con un solo errore) e controlla che i programmi validi diano 0 con il `load_plan` giusto. Il `load_plan` atteso lo calcola da solo, leggendo i PT_LOAD.

| Passo | Copia rovinata | Errore | Dove viene fermata |
|--:|---|---|---|
| 2 | `short_51` | file di 51 byte | 2a |
| 2 | `bad_magic` | byte 0 = `'B'` | 2b |
| 2 | `class_64` | byte 4 = 2 | 2c |
| 2 | `big_endian` | byte 5 = 2 | 2c |
| 2 | `ident_version_0` | byte 6 = 0 | 2c |
| 3 | `type_rel` | `e_type` = 1 | 3a |
| 3 | `machine_x86_64` | `e_machine` = 62 | 3a |
| 3 | `version_0` | `e_version` = 0 | 3a |
| 3 | `ehsize_64` | `e_ehsize` = 64 | 3b |
| 3 | `phentsize_56` | `e_phentsize` = 56 | 3b |
| 3 | `shentsize_64` | `e_shentsize` = 64 | 3b |
| 4 | `phnum_0` | `e_phnum` = 0 | 4b |
| 4 | `phoff_outside` | `e_phoff` = size | 4b |
| 4 | `shnum_0` | `e_shnum` = 0 | 4b |
| 4 | `shoff_outside` | `e_shoff` = size − 10 | 4b |
| 4 | `truncated` | file tagliato a fine segmento − 1 | 4b |
| 5 | `pt_dynamic` | `p_type` = 2 | 5b |
| 5 | `filesz_gt_memsz` | `p_filesz` = `p_memsz` + 4 | 5c |
| 5 | `too_large` | `p_vaddr + p_memsz` = 3073 | 5c |
| 5 | `segment_outside` | `p_offset` = size | 5c |
| 6 | `vaddr_4` | `p_vaddr` = 4 | 6c (`min_addr`) |
| 6 | `no_pt_load` | `p_type` = 6: niente da caricare | 6c (`have_delta`) |

**Risultato** sulla nostra build: tutti i 30 controlli passano (8 sugli helper e 22 sulle copie rovinate), e tutti i 9 programmi danno 0 con il `load_plan` giusto (`OK: 0 test(s) of steps <= 7 failing`).

### I file rovinati del test kit

| Test | Cosa fa il generatore | Dove si ferma | Risultato |
|---|---|---|---|
| `bad_magic` | byte 0–3 = `"BAD!"` | 2b | -4 |
| `bad_header` | byte 4 = 2 | 2c | -4 |
| `truncated_elf` | file tagliato a fine segmento − 1 | 4b: la tabella delle sezioni resta fuori | -4 |
| `too_large` | `p_memsz` del PT_LOAD scrivibile portato a 3073 | 5c | -4 |
| `bad_reloc`, `unsupported_reloc` | rilocazioni rovinate | passano il piano 2: li ferma Simone (piano 3) | 0 qui |

Nel kernel questi test si possono verificare solo quando Antonio chiama `elf_check_layout()` dalla SVC 13:

```bash
python3 ~/test-kit/run_tests.py --suite big --kernel build/scratch-os.axf --image build/test-kit/big/fs.img
```

## 12. Cosa NON fa questa parte

- Le sezioni, `code_end` e il controllo dell'entry: Athena, `elf_sections.c` (vedi `ELF_SECTION.md`).
- Le rilocazioni (REL, simboli, ABS32, THM_CALL, THM_JUMP24) e il controllo `slot_base + parola`: Simone, piani 3 e 5.
- La ricerca del nome, la scelta dello slot, la copia, l'azzeramento del BSS e la creazione del task: Antonio, piani 1, 4 e 6.
- La MPU e il W^X: Giulio.


