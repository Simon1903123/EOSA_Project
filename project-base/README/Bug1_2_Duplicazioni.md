# Bug 1, Bug 2 e unificazione di `elf.c` e `reloc.c`

Questo documento spiega tre modifiche al kernel, fatte sul branch `test_new`:

| # | Cosa | File | Commit |
|---|---|---|---|
| Bug 1 | La regione MPU del codice del programma era in sola lettura anche per il kernel: `MPU_AP_RO` → `MPU_AP_PRIV_RW_USER_RO` | `kernel/mpu.c`, `include/cmsdk.h` | `32c421f` |
| Bug 2 | SVC 11 controllava solo l'header dell'immagine: ora usa `image_valid()` | `kernel/syscall.c` | `32c421f` (formattazione in `0523230`) |
| Duplicazioni | Gli helper e le costanti ELF scritti sia in `elf.c` sia in `loader.c` ora stanno solo in `elf_internal.h`; `loader.c` è stato rinominato `reloc.c` | `kernel/elf_internal.h`, `kernel/elf.c`, `kernel/reloc.c` | `9662863` |

---

## 1. Bug 1: il kernel andava in crash su un puntatore al codice

### 1.1 Cosa succedeva

Un programma caricato poteva bloccare tutto il sistema con una system call
del tutto lecita. Per esempio:

```c
static const char in_code[16] = "AAAAAAAAAAAAAAA";   /* const: finisce in .text */
...
sys_image_name( 0, (char *) in_code, 16 );
```

Il kernel stampava

```text
MemManage: unrecoverable CFSR=0x82 HFSR=0x0
```

e si fermava: niente prompt, bisognava riavviare QEMU. Lo stesso con
`task_delay_until( puntatore_al_codice, 1 )` (SVC 8), e con qualsiasi system
call che scrive in memoria del chiamante.

La traccia invece dice:

- **R9**: un argomento sbagliato di una system call va rifiutato con
  l'errore giusto, e **"the kernel must not crash"**;
- **§6.5**: la destinazione di una system call deve stare nella partizione
  del chiamante. Per un programma caricato la partizione è **tutto lo slot**
  da 4 KiB, codice compreso. Quindi quel puntatore è addirittura un argomento
  **valido**: la system call deve funzionare.

### 1.2 Quattro concetti per capire il bug

**a) Chi è privilegiato.** I programmi caricati (e la shell) girano in thread
mode, **non privilegiati**. Quando chiamano `svc #n`, la CPU entra in
**handler mode** e il kernel esegue la system call, **privilegiato**. Durante
la system call le regioni della MPU restano quelle del programma: la MPU si
riprogramma solo al cambio di task.

**b) Le regioni della MPU di un programma caricato.** `mpu_set_program_regions()`
imposta (esempio: slot 0, codice di 832 byte):

| Regione | Indirizzi | Permessi | Per cosa |
|---|---|---|---|
| 0 | FLASH `0x00000000..0x003fffff` | lettura + esecuzione | codice della shell, costanti, immagine |
| 1 | tutto lo slot `0x20002000..0x20002fff` | lettura + scrittura, **XN** (non eseguibile) | dati, BSS, stack |
| 2 | stesso slot, ma solo le prime *n* sottoregioni da 512 byte (qui `0x20002000..0x200023ff`) | lettura + esecuzione, niente scrittura | codice del programma |

Quando due regioni si sovrappongono **vince quella con il numero più alto**:
nelle sottoregioni del codice vale la 2 (eseguibile, non scrivibile), nel
resto dello slot la 1 (scrivibile, non eseguibile). Questo è **W^X**.

**c) Il campo AP.** Ogni regione ha un campo AP di 3 bit che dà permessi
diversi al codice privilegiato e a quello non privilegiato:

| AP | Kernel (privilegiato) | Task (non privilegiato) | Costante in `cmsdk.h` |
|---|---|---|---|
| `001` | lettura/scrittura | nessun accesso | `MPU_AP_PRIV_RW_USER_NONE` |
| **`010`** | **lettura/scrittura** | **sola lettura** | **`MPU_AP_PRIV_RW_USER_RO`** (aggiunta) |
| `011` | lettura/scrittura | lettura/scrittura | `MPU_AP_FULL_RW` |
| `110` | sola lettura | sola lettura | `MPU_AP_RO` |

L'**esecuzione** non dipende da AP ma da un altro bit, **XN**.

**d) PRIVDEFENA non mette il kernel "sopra" la MPU.** `mpu_init()` accende la
MPU con `PRIVDEFENA`, che vuol dire: per il kernel, **dove nessuna regione è
attiva**, vale la mappa di memoria di default. Ma dove una regione **è**
attiva, i suoi permessi valgono **anche per il kernel**.

### 1.3 La causa

La regione 2 era configurata così:

```c
MPU->RBAR = slot_base | MPU_RBAR_VALID | 2;
MPU->RASR = MPU_RASR_AP( MPU_AP_RO ) | MPU_RASR_S
            | MPU_RASR_C | srd_mask << 8
            | MPU_RASR_SIZE( log2_size ) | MPU_RASR_ENABLE;
```

`MPU_AP_RO` vale `110`: **sola lettura per tutti, kernel compreso**.

Il bug passo per passo, con `in_code` all'indirizzo `0x20002330`:

1. il programma chiama `sys_image_name( 0, in_code, 16 )`;
2. `svc #12`: il kernel, privilegiato, esegue `sys_image_name_impl`. Le
   regioni MPU sono ancora quelle del programma;
3. il kernel controlla il puntatore: `0x20002330` sta nello slot del
   chiamante, quindi è accettato (giustamente);
4. il kernel scrive il nome a `0x20002330`. Quell'indirizzo è nella regione 2,
   attiva, con AP=`110`: **il kernel non può scrivere**;
5. la MPU genera un **MemManage**, `CFSR=0x82`: bit 1 `DACCVIOL` (accesso ai
   dati vietato) e bit 7 `MMARVALID` (indirizzo valido in `MMFAR`);
6. il fault è avvenuto **dentro la SVC**, in handler mode. `fault_dispatch()`
   in `faults.c` (codice dato dal corso) recupera solo i fault avvenuti mentre
   gira un programma in thread mode; un fault in handler mode è un **fault del
   kernel**, e per la traccia deve restare non recuperabile. Quindi stampa
   `unrecoverable` e ferma il sistema.

Il test kit non se ne accorgeva perché nessun test passa a una system call un
puntatore al codice. `wx_write_code` scrive nel codice **dal programma**: lì
il fault è giusto e recuperabile.

### 1.4 La correzione

**`include/cmsdk.h`**: la costante per AP=`010` non esisteva ed è stata
aggiunta accanto alle altre:

```c
#define MPU_AP_PRIV_RW_USER_RO 2UL /* AP=010: privileged read/write, unprivileged read only */
```

**`kernel/mpu.c`**, `mpu_set_program_regions()`, regione 2:

```c
MPU->RBAR = slot_base | MPU_RBAR_VALID | 2;
MPU->RASR = MPU_RASR_AP( MPU_AP_PRIV_RW_USER_RO ) | MPU_RASR_S
            | MPU_RASR_C | srd_mask << 8
            | MPU_RASR_SIZE( log2_size ) | MPU_RASR_ENABLE;
```

È cambiata solo la regione 2. La regione 0 (FLASH) resta `MPU_AP_RO`: la
FLASH non si scrive comunque.

Adesso, nell'esempio di prima, `sys_image_name` scrive il nome, restituisce
`0` e il programma prosegue normalmente.

### 1.5 Perché W^X resta valido

Permessi **del programma** (non privilegiato) prima e dopo:

| Area dello slot | Prima | Dopo |
|---|---|---|
| Codice (regione 2) | lettura + esecuzione, non scrivibile | lettura + esecuzione, non scrivibile |
| Dati, BSS, stack (regione 1) | lettura + scrittura, non eseguibile | lettura + scrittura, non eseguibile |

Per il programma **non cambia niente**: AP=`010` gli dà la sola lettura, come
AP=`110`. Cambia solo la colonna del kernel, che adesso può scrivere nel
codice del programma corrente. Infatti `wx_write_code` dà ancora
`MemManage in loaded program: CFSR=0x82 ...` e `wx_exec_data` ancora
`MemManage in loaded program: CFSR=0x1`.

Che il kernel possa scrivere lì non è un rischio: il kernel poteva già
scrivere in tutta la RAM fuori dalle regioni attive (compresi gli altri slot).
La MPU serve a proteggere il sistema **dai programmi**, e quella protezione è
rimasta identica.

### 1.6 Perché non rifiutare i puntatori al codice

L'alternativa era lasciare AP=`110` e far restituire `-1` alle system call
quando il puntatore cade nell'area del codice. È stata scartata perché:

1. **viola la specifica**: per §6.5 è valido tutto ciò che sta nella
   partizione del chiamante, e il codice ci sta;
2. **è fragile**: il controllo andrebbe ripetuto in SVC 8, SVC 12 e in ogni
   system call futura che scrive. Il fix nella MPU invece vale per tutte, una
   volta sola.

---

## 2. Bug 2: SVC 11 controllava solo l'header dell'immagine

### 2.1 Cosa chiede la traccia

§6.1 elenca le regole dell'immagine IMG1. Alcune riguardano l'**header**
(i primi 20 byte), altre la **directory** (le voci da 24 byte con nome,
offset e dimensione di ogni programma):

| Header | Directory |
|---|---|
| magic `IMG1`, versione 1 | ogni nome: 1–15 caratteri tra `A-Z a-z 0-9 _ -`, poi NUL |
| `image_size` da 20 a 2 MiB | nessun nome duplicato |
| `count` da 1 a 32 | `offset` multiplo di 4, `size` almeno 40 |
| `directory_offset` multiplo di 4, almeno 20 | ogni file dopo la directory e dentro `image_size` |
| la directory sta dentro `image_size` | nessuna coppia di file sovrapposta |

E poi: **"If any of these rules is broken, the whole image is invalid. Then
`ls` prints `error: no valid image`, `run <name>` prints
`error: program cannot be loaded`"**. Per SVC 11 la tabella di §6.5 dice:
`-1` se l'immagine non è valida.

### 2.2 Cosa faceva il codice

Le regole erano scritte tutte, ma divise in due funzioni usate da system
call diverse:

| Funzione | Cosa controllava | Chi la usava |
|---|---|---|
| `sys_image_count_impl` (SVC 11) | solo l'**header** | SVC 11 e SVC 12 (che la chiama per sapere se l'immagine è valida) |
| `image_valid` | l'header (chiamando SVC 11) **e la directory** | solo SVC 13 |

`ls` usa SVC 11 e SVC 12, `run` usa SVC 13. Risultato: con una directory
rovinata (due file sovrapposti, un nome con un carattere non ammesso, due
nomi uguali) `ls` elencava i programmi, mentre `run` diceva
`error: program cannot be loaded`. Due risposte diverse sulla stessa
immagine, e `ls` sbagliata rispetto alla traccia.

Il test kit non se ne accorgeva perché i suoi file rovinati sono **programmi**
rovinati, mai una **directory** rovinata.

### 2.3 La correzione: tre funzioni, ognuna con il suo compito

```text
image_header_count()     controlla SOLO l'header
        │                → count (1..32) oppure -1
        ▼
image_valid()            header (chiamando image_header_count)
        │                + ogni voce della directory
        │                → 1 = valida, 0 = non valida
        ▼
sys_image_count_impl()   SVC 11: -1 se !image_valid(),
                         altrimenti image_header_count()
```

Le modifiche in `kernel/syscall.c` sono quattro.

**1. Il vecchio corpo di SVC 11 diventa `image_header_count()`.** Stesso
codice, stessi controlli, cambia solo il nome (e spariscono i tre
`(void) aN;`, perché la funzione non ha più parametri):

```c
static uint32_t image_header_count( void )
{
    struct img_header *img = (struct img_header *) &__fs_image_start;

    /* 1. Controllo Magic Number (deve essere "IMG1") */
    if ( img->magic != 0x31474d49 )
    ...
    return img->count;
}
```

**2. La nuova SVC 11**, subito dopo:

```c
static uint32_t sys_image_count_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    (void) a0;
    (void) a1;
    (void) a2;

    if ( !image_valid() )
    {
        return (uint32_t) -1;
    }

    return image_header_count();
}
```

**3. Una dichiarazione anticipata di `image_valid`**, più in alto nel file:

```c
static int image_valid( void );
```

Serve perché `image_valid` è definita più in basso di `sys_image_count_impl`:
senza dichiarazione il compilatore, arrivato alla chiamata, non saprebbe che
la funzione esiste.

**4. In `image_valid()` cambia la prima riga:**

```c
uint32_t count = image_header_count();   /* era: sys_image_count_impl( 0, 0, 0 ) */
```

### 2.4 Perché `image_valid()` non può più chiamare SVC 11

Prima `image_valid()` cominciava chiamando `sys_image_count_impl( 0, 0, 0 )`
per controllare l'header. Ma adesso è `sys_image_count_impl` a chiamare
`image_valid()`. Se non si cambiasse quella riga:

```text
sys_image_count_impl → image_valid → sys_image_count_impl → image_valid → ...
```

le due funzioni si chiamerebbero a vicenda all'infinito: ogni chiamata usa un
po' di stack del kernel, finché lo stack finisce e il kernel va in fault. Per
questo serve una funzione che controlli **solo** l'header e non chiami
nessun'altra: `image_header_count()`.

C'è anche un motivo di **ordine**: `image_valid()` legge la directory usando
`directory_offset` e `count`. Quei campi devono essere già stati controllati
dall'header check, altrimenti si leggerebbe memoria a caso. Con
`image_header_count()` chiamata per prima, dentro `image_valid()`, l'ordine è
sempre giusto.

Nota: il commento sopra quella riga dice ancora
`l'intestazione dell'immagine viene già controllata da svc 11`; adesso la
controlla `image_header_count()`.

### 2.5 Chi ne beneficia

| System call | Cosa usa | Effetto |
|---|---|---|
| SVC 11 (`sys_image_count`) | `image_valid()` | `-1` anche se è rovinata solo la directory |
| SVC 12 (`sys_image_name`) | chiama `sys_image_count_impl( 0, 0, 0 )` | eredita il controllo completo, **senza modifiche** |
| SVC 13 (`sys_program_run`) | usava già `image_valid()` | invariata |

Con una directory rovinata adesso `ls` stampa `error: no valid image` e
`run` stampa `error: program cannot be loaded`, come chiede la traccia, e la
shell continua a funzionare.

Il commit `0523230` ha sistemato la formattazione della nuova
`sys_image_count_impl` (un tab e degli spazi in fondo alla riga), che faceva
fallire `make format-check`.

---

## 3. Unificazione di helper e costanti di `elf.c` e `reloc.c`

### 3.1 Il problema

Il loader ELF è diviso in tre file:

| File | Cosa fa |
|---|---|
| `kernel/elf.c` | controlla l'header ELF e i segmenti `PT_LOAD` |
| `kernel/elf_sections.c` | controlla le sezioni, calcola `code_end`, controlla l'entry |
| `kernel/loader.c` (ora `reloc.c`) | controlla e applica le relocation |

e `kernel/elf_internal.h` è l'header che condividono. Ma `loader.c` non usava
quasi niente di `elf_internal.h`: si era riscritto per conto suo sia gli
helper sia le costanti.

**Helper scritti due volte:**

| In `elf.c` | Copia in `loader.c` | Cosa fa |
|---|---|---|
| `elf_read16()` | `read16()` | legge un numero little-endian da 2 byte |
| `elf_read32()` | `read32()` | legge un numero little-endian da 4 byte |
| `elf_fits()` | `fits()` | controlla che `[off, off + len)` stia in `[0, size)` senza overflow |

Stesso codice, due nomi, due posti da correggere se si trova un errore.

**Costanti scritte due volte** (definite in `elf_internal.h` e di nuovo in
`loader.c`, a volte con un altro nome):

| In `elf_internal.h` | In `loader.c` |
|---|---|
| `EH_SHOFF`, `EH_SHNUM` | `EH_SHOFF`, `EH_SHNUM` (di nuovo) |
| `SHT_SYMTAB`, `SHT_REL`, `SHF_ALLOC` | `SHT_SYMTAB`, `SHT_REL`, `SHF_ALLOC` (di nuovo) |
| `ELF_SHDR_SIZE` | `SHDR_SIZE` |
| `ELF_REL_SIZE` | `REL_SIZE` |
| `ELF_SYM_SIZE` | `SYM_SIZE` |

**Costanti che stavano solo in `loader.c`**, ma che descrivono il formato ELF
e quindi appartengono all'header comune: `SH_NAME`, `SH_TYPE`, `SH_FLAGS`,
`SH_ADDR`, `SH_OFFSET`, `SH_SIZE`, `SH_LINK`, `SH_INFO`, `SH_ENTSIZE` (posizioni
dei campi di una riga della tabella delle sezioni), `R_OFFSET`, `R_INFO` (di un
record REL), `ST_SHNDX`, `SHN_UNDEF` (di un simbolo), `R_ARM_ABS32`,
`R_ARM_THM_CALL`, `R_ARM_THM_JUMP24` (tipi di relocation).

### 3.2 Cosa è stato fatto

#### `kernel/elf_internal.h`: tutto quello che è condiviso, una volta sola

**Gli helper.** Le tre funzioni sono state **spostate** da `elf.c`, con il loro
codice e i loro commenti esplicativi (esempi compresi), e sono diventate
`static inline` (qui sotto senza i commenti, per brevità):

```c
static inline uint32_t elf_read16( const uint8_t *p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 );
}

static inline uint32_t elf_read32( const uint8_t *p )
{
    return (uint32_t) p[0] | ( (uint32_t) p[1] << 8 ) | ( (uint32_t) p[2] << 16 )
           | ( (uint32_t) p[3] << 24 );
}

static inline int elf_fits( uint32_t off, uint32_t len, uint32_t size )
{
    return off <= size && len <= size - off;
}
```

Prendono il posto dei tre prototipi che c'erano prima. Nel commento in testa
agli helper la frase "their names and signatures are fixed by elf_internal.h"
è diventata "defined here once, so every file uses the same code".

**Le costanti.** Quelle che stavano solo in `loader.c` sono state spostate in
un nuovo blocco, "Fields used by the relocations (reloc.c)", con i loro
commenti originali:

```c
#define SH_NAME 0      /* indice del nome in .shstrtab: non lo usiamo */
#define SH_TYPE 4      /* tipo della sezione (REL, SYMTAB, ...) */
...
#define R_OFFSET 0      /* il "site": dove sta la cosa da correggere */
#define R_INFO 4      /* tipo (byte basso) e indice del simbolo (24 bit alti) */
#define ST_SHNDX 14      /* sezione in cui è definito (2 byte); 0 = non definito */
#define SHN_UNDEF 0
#define R_ARM_ABS32 2u    /* word con un indirizzo completo: va corretta */
#define R_ARM_THM_CALL 10u    /* chiamata Thumb (BL): relativa, non si tocca */
#define R_ARM_THM_JUMP24 30u    /* salto Thumb (B.W): relativo, non si tocca */
```

Quelle che `elf_internal.h` aveva già (`EH_SHOFF`, `SHT_REL`, `ELF_SHDR_SIZE`,
...) non sono state ricopiate: restano definite una volta.

#### `kernel/elf.c`: tolti gli helper

Il blocco "STEP 1" con le tre funzioni è stato tolto. Al suo posto c'è un
commento che dice dove sono adesso:

```c
/* ======================================================================
 * STEP 1 - The three common helpers (elf_read16, elf_read32, elf_fits) are
 * defined in kernel/elf_internal.h, shared with elf_sections.c and reloc.c.
 * ====================================================================== */
```

Tutto il resto di `elf.c` è invariato. `SLOT_PROG_MAX` resta in `elf.c`,
perché la usa solo `elf.c` e non è duplicata.

#### `kernel/loader.c` → `kernel/reloc.c`

Il file è stato rinominato con `git mv` (git ne conserva la storia) e:

1. include `kernel/elf_internal.h` al posto di `kernel/loader.h`
   (`elf_internal.h` include già `loader.h`, quindi `struct load_plan` e i
   prototipi sono ancora visibili);
2. non contiene più il blocco di `#define` né le tre funzioni `read32`,
   `read16`, `fits`;
3. i nomi duplicati sono sostituiti da quelli comuni:

| Prima (in `loader.c`) | Dopo (in `reloc.c`) |
|---|---|
| `read32( ... )` | `elf_read32( ... )` |
| `read16( ... )` | `elf_read16( ... )` |
| `fits( ... )` | `elf_fits( ... )` |
| `SHDR_SIZE` | `ELF_SHDR_SIZE` |
| `REL_SIZE` | `ELF_REL_SIZE` |
| `SYM_SIZE` | `ELF_SYM_SIZE` |

Le sostituzioni valgono anche nei commenti che nominavano queste funzioni.

**A parte i blocchi tolti e questi nomi, ogni riga di `reloc.c` è identica a
`loader.c`**, commenti compresi: stessi controlli, nello stesso ordine, con
gli stessi risultati. Le funzioni pubbliche (`elf_check_relocs`,
`elf_apply_relocs`) hanno lo stesso nome e la stessa firma, quindi chi le
chiama (`syscall.c`) non è cambiato.

Il `Makefile` non è stato toccato: trova da solo tutti i `.c` di `kernel/`,
quindi compila `reloc.c` al posto di `loader.c`.

### 3.3 Perché `static inline` in un header

Una funzione normale definita in un header che è incluso da tre `.c`
(`elf.c`, `elf_sections.c`, `reloc.c`) darebbe un errore al link: "multiple
definition", perché ogni `.c` ne avrebbe una copia pubblica con lo stesso
nome.

- `static` rende la funzione **privata** di ogni file che include l'header,
  quindi niente conflitti al link;
- `inline` dice al compilatore che può inserire il corpo della funzione
  direttamente dove viene chiamata.

Il **sorgente** è uno solo: se un giorno si corregge `elf_fits`, la correzione
vale per tutti e tre i file.

Una precisazione: il kernel è compilato con `-O0` (per poterlo seguire passo
passo), e a `-O0` GCC non espande le funzioni `inline`. Ogni `.c` che le usa
ne ottiene quindi una piccola copia privata nel codice macchina (pochi
byte per funzione). È il prezzo di avere tutto nell'header. Se si volesse
una sola copia anche nel binario, basterebbe lasciare in `elf_internal.h`
solo i prototipi e mettere le definizioni normali in `elf.c`: il
comportamento sarebbe identico.

### 3.4 Cosa non è cambiato

- Il **comportamento** del kernel: stessi file accettati, stessi file
  rifiutati, stessi messaggi.
- `elf_sections.c`, `syscall.c`, `loader.h`, `mpu.c`: non toccati da questa
  modifica.
- La logica di `reloc.c`: è quella di `loader.c`, riga per riga.

### 3.5 In numeri

| File | Prima | Dopo |
|---|---:|---:|
| `elf.c` | 499 righe | 438 righe |
| `loader.c` → `reloc.c` | 447 righe | 382 righe |
| `elf_internal.h` | 104 righe | 194 righe |
| **Totale** | **1050 righe** | **1014 righe** |

`elf_internal.h` cresce perché riceve gli helper (con i loro commenti) e le
costanti; `elf.c` e `reloc.c` si accorciano perché non hanno più le loro
copie.

### 3.6 Nota sulla storia dei commit

Nella storia di `test_new` c'è anche un commit `cd34e8a` ("Remove
duplications...") seguito dal suo revert `0a63606`. Quel commit faceva più
modifiche di quelle richieste ed è stato annullato per intero. L'unificazione
descritta in questa sezione è il commit `9662863`.
