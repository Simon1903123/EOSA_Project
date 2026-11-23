# SVC 13, il loader di scratch-OS: documentazione ad alto livello

**Parte C (Antonio)** · `kernel/syscall.c`, funzione `sys_program_run_impl` · versione big (ELF + W^X) · Cortex-M3 su QEMU mps2-an385 · 19 novembre 2026

> **A cosa serve questo documento.** Spiega a livello alto cosa fa la SVC 13, come si inserisce nella pipeline del loader e come si collega al lavoro degli altri, per prepararti alla presentazione e alle domande individuali del professore. Il dettaglio riga per riga è nella guida passo passo (`SVC13_guida_Antonio.pdf`): qui c'è il filo del discorso. Le parti degli altri sono descritte quanto basta per collegarle alla tua, e quelle che mancano sono elencate nella sezione 7. Le domande possibili sono nella sezione 10.

<!-- indice -->
## Indice

1. [In breve](#1-in-breve)
2. [Il contesto: cosa deve fare il sistema](#2-il-contesto-cosa-deve-fare-il-sistema)
3. [La pipeline: chi fa cosa](#3-la-pipeline-chi-fa-cosa)
4. [La SVC 13 passo per passo](#4-la-svc-13-passo-per-passo)
5. [Le scelte di progetto](#5-le-scelte-di-progetto)
6. [Il collegamento con gli altri](#6-il-collegamento-con-gli-altri)
7. [Le parti che mancano](#7-le-parti-che-mancano)
8. [Verifica e prove](#8-verifica-e-prove)
9. [Presentazione e demo](#9-presentazione-e-demo)
10. [Domande possibili del professore](#10-domande-possibili-del-professore)
11. [Glossario](#11-glossario)
<!-- /indice -->

## 1. In breve

**Che cos'è.** La SVC 13 (`sys_program_run`) è la chiamata di sistema con cui la shell dice «avvia questo programma». Il codice della parte C, cioè `sys_program_run_impl` e le sue funzioni di supporto in `kernel/syscall.c`, è il **coordinatore** del loader: non sa leggere un ELF né correggere gli indirizzi, ma stabilisce l'ordine delle operazioni, passa i dati giusti a chi sa farle e, solo se tutto è valido, scrive nello slot e crea il task.

**In sette passi.**

1. copia il nome nel kernel (il puntatore arriva da un task, non è affidabile);
2. controlla l'intera immagine IMG1;
3. cerca il programma nella directory;
4. sceglie il primo slot libero;
5. fa controllare il file ad A (struttura dell'ELF) e a B (relocation);
6. **linea di non ritorno**: copia il programma, azzera il BSS, fa correggere gli indirizzi a B;
7. crea il task (codice dato), scrive `code_end` per la MPU e restituisce l'id.

**Risultato.** Un id ≥ 4 (il primo programma nello slot 0 ha id 4), oppure uno di quattro errori: **-1** nome o immagine non validi, **-2** programma non trovato, **-3** nessuno slot libero, **-4** file del programma non valido.

**Due regole che reggono tutto.** Prima si controlla tutto, poi si scrive: un file rovinato non lascia mai uno slot a metà. Il kernel non si fida di nulla di ciò che arriva da un task.

**Come ci si parla.** Solo attraverso `kernel/loader.h`: la `struct load_plan` (il «piano di carico») e tre funzioni. La SVC 13 le usa ma non le modifica.

**Dove siamo.** Il codice della guida è stato provato in QEMU, su una copia del repository, con versioni finte delle funzioni di A e B. Le parti vere degli altri (controllo ELF, relocation, W^X) non c'erano ancora nello snapshot del repository usato per questo documento: sono elencate nella sezione 7.

## 2. Il contesto: cosa deve fare il sistema

Nello scratch-OS di partenza ogni task è compilato e linkato insieme al kernel: per aggiungerne uno bisogna ricompilare tutto. Il progetto aggiunge la possibilità di **avviare programmi separati**, come fa un sistema operativo vero. Servono quattro pezzi:

- **la shell** (sulla UART): `ls` elenca i programmi, `run <nome>` ne avvia uno;
- **l'immagine IMG1**: un file in FLASH (da `0x00200000`, sola lettura) con una directory di nomi e, nella versione big, gli ELF dei programmi così come li ha prodotti il linker. Il kernel la legge sul posto, senza copiarla;
- **il loader nel kernel**: trova il programma, lo copia in uno dei **quattro slot** di RAM da 4 KiB, lo corregge perché funzioni a quell'indirizzo, lo protegge con la MPU e lo avvia come task. Quando il programma finisce, o va in fault, lo slot torna libero;
- **la build e l'host tool**: sul PC ogni programma si compila a parte, linkato all'indirizzo 0 con `--emit-relocs`, e `tools/mkimage.py` li impacchetta nell'immagine.

**Perché serve la relocation.** Il programma è linkato come se partisse dall'indirizzo 0, ma finirà in uno slot (`0x20002000`, `0x20003000`, …). I valori che contengono un indirizzo assoluto, come un puntatore a una variabile globale o una tabella di puntatori a funzione, sono giusti solo per l'indirizzo 0: il loader deve sommare la base dello slot a ciascuno. Il Cortex-M3 non ha MMU, quindi non esiste un altro modo.

**Perché serve la MPU.** I programmi girano unprivileged: la MPU lascia loro solo il proprio slot, niente RAM del kernel e niente altri slot. Nella versione big si aggiunge il **W^X**: nello slot il codice si legge e si esegue ma non si scrive, mentre dati e stack si leggono e si scrivono ma non si eseguono.

**Numeri e fatti da ricordare**

| Cosa | Valore |
|---|---|
| Immagine IMG1 | in FLASH da `0x00200000`, al massimo 2 MiB, sola lettura; intestazione da 20 byte e da 1 a 32 voci da 24 byte |
| Nome di un programma | da 1 a 15 caratteri `A-Za-z0-9_-`, più il NUL finale (16 byte) |
| Slot | 4, da 4096 byte e allineati: `0x20002000`, `0x20003000`, `0x20004000`, `0x20005000` |
| Dentro uno slot | il programma (codice, dati, BSS) occupa al massimo 3072 byte; sopra c'è 1 KiB di stack, con i frame iniziali da 64 byte in cima |
| Scelta dello slot | sempre il primo libero, nell'ordine 0, 1, 2, 3 |
| Id del task | `generation * 4 + slot`: 4, 5, 6, 7 la prima volta; il programma successivo nello slot 0 ha id 8 |
| Errori della SVC 13 | -1 nome o immagine, -2 non trovato, -3 nessuno slot, -4 file non valido |
| Messaggi della shell | -2: `error: program not found`; -3: `error: no free slot`; -1 e -4: `error: program cannot be loaded` |

## 3. La pipeline: chi fa cosa

Il comando `run` attraversa sei piani in fila. I primi tre solo leggono; gli ultimi tre scrivono nello slot e non possono più fallire. Ogni piano è una scatola chiusa: lavora con ciò che riceve, consegna un risultato e si fida delle garanzie di chi sta sopra.

```text
 shell:  run print_plain
    |
    v
+--------------------------------------------------------------------------+
| C  SVC 13: sys_program_run_impl                              (Antonio)   |
|    1 nome copiato nel kernel   -1       3 ricerca nella directory   -2   |
|    2 immagine valida           -1       4 primo slot libero         -3   |
+--------------------------------------------------------------------------+
    |  f, size (l'ELF in FLASH)          fino alla linea sotto: solo letture
    v
+-------------------------------+      +-----------------------------------+
| A  elf_check_layout           |      | B  elf_check_relocs               |
|    Gianluigi, Athena          | ---> |    Simone                         |
|    controlla l'ELF e riempie  |      |    controlla le relocation        |
|    il piano di carico         |      |    (non scrive niente)            |
+-------------------------------+      +-----------------------------------+
    |  ciascuno risponde 0 oppure -4: al primo -4 la SVC 13 restituisce -4
    |  e nessuno slot è cambiato
====|====  LINEA DI NON RITORNO: da qui in poi si scrive nello slot  =======
    v
+-------------------------------+      +-----------------------------------+
| C  copia il programma         |      | B  elf_apply_relocs               |
|    e azzera il BSS            | ---> |    somma la base dello slot a     |
|    (secondo il piano)         |      |    ogni indirizzo assoluto        |
+-------------------------------+      +-----------------------------------+
    |
    v
+--------------------------------------------------------------------------+
| codice dato: task_program_create -> task READY, riga "loader: ..."       |
| C scrive code_end nel record del task (campo aggiunto da Giulio)         |
+--------------------------------------------------------------------------+
    |
    v
+--------------------------------------------------------------------------+
| D  MPU (Giulio), a ogni cambio di task: il programma vede solo il suo    |
|    slot; codice r-x, dati e stack rw- (W^X)                              |
+--------------------------------------------------------------------------+
    |
    v
 fine o fault (codice dato): slot libero, la shell ristampa il prompt
```

| Piano | Chi | Dove | Cosa fa | Cosa consegna | Errori |
|---|---|---|---|---|---|
| 1 | C, Antonio | `kernel/syscall.c` | nome, immagine, ricerca, slot | `f`, `size`, `slot`, `slot_base`, nome | -1, -2, -3 |
| 2 | A, Gianluigi e Athena | `kernel/elf.c`, `kernel/elf_sections.c` | `elf_check_layout`: controlla la struttura dell'ELF e riempie il piano di carico | 0 o -4, più il piano | -4 |
| 3 | B, Simone | `kernel/loader.c` | `elf_check_relocs`: controlla le relocation | 0 o -4 | -4 |
| 4 | C, Antonio | `kernel/syscall.c` | copia il programma nello slot e azzera il BSS | lo slot con il programma com'era nel file | nessuno |
| 5 | B, Simone | `kernel/loader.c` | `elf_apply_relocs`: somma la base dello slot a ogni indirizzo assoluto | lo slot pronto per girare lì | nessuno |
| 6 | codice dato | `kernel/task.c` | `task_program_create`: crea il task, lo mette READY, stampa `loader: …` | l'id del task | -1 (non deve mai succedere) |
| MPU | D, Giulio | `kernel/mpu.c`, `kernel/task.h` | a ogni cambio di task confina il programma nel suo slot e applica il W^X | un fault recuperabile se il programma sgarra | |

**Il piano di carico (`struct load_plan`).** È il messaggio che A consegna agli altri: dice cosa copiare, cosa azzerare, dove si parte e dove finisce il codice. È l'unico dato che passa da un piano all'altro, insieme a `f` e `size`.

| Campo | Significato | Lo riempie | Lo usano |
|---|---|---|---|
| `entry` | dove parte il programma: offset nello slot, con il bit 0 a 1 (Thumb) | A | C, per l'indirizzo di partenza |
| `file_off`, `copy_size` | da dove e quanti byte copiare dal file allo slot | A | C, per la copia |
| `bss_start`, `bss_end` | la zona da azzerare, come offset nello slot (nel file il BSS non ha byte) | A | B (`bss_start`), C |
| `code_end` | fine del codice e inizio dei dati, multiplo di 512 | A | B, C (lo scrive nel record del task), D (la MPU) |
| `mem_end` | fine del programma, al massimo 3072 | A | A, per il controllo del limite |

I programmi sono linkati a 0, quindi gli offset nello slot coincidono con gli indirizzi scritti nell'ELF; solo `file_off` è un offset dentro il file.

## 4. La SVC 13 passo per passo

**Il percorso reale.** `cmd_run` in `src/shell.c` chiama `sys_program_run(name)`; lo stub in `lib/syscalls.s` esegue `svc #13`; `SVC_Handler` passa il frame dei registri a `svc_dispatch`, che chiama `table[13]`, cioè `sys_program_run_impl(a0, a1, a2)`. `a0` è l'indirizzo del nome; `a1` e `a2` non servono. Il risultato torna nel registro `r0` della shell.

| Passo | Cosa fa | Perché | Errore |
|---|---|---|---|
| **1. Nome** | Copia il nome dall'indirizzo `a0` in un buffer del kernel da 16 byte, un byte alla volta. Ogni byte letto deve stare nella partizione del chiamante o in FLASH; il nome non deve essere vuoto e deve finire con NUL entro 16 byte (al massimo 15 caratteri). | `a0` è scelto da un task non privilegiato, mentre il kernel è privilegiato: non deve leggere per suo conto memoria che il task non potrebbe leggere. Dopo si lavora su una copia sicura. | -1 |
| **2. Immagine** | Controlla l'intera immagine IMG1: intestazione (magic `IMG1`, versione 1, dimensioni, directory) e ogni voce (nome da 1 a 15 caratteri ammessi, offset multiplo di 4 e dopo la directory, size almeno 40, file dentro l'immagine, nomi tutti diversi, nessuna sovrapposizione). | Un'immagine rotta in un punto qualsiasi va rifiutata prima di fidarsi dei suoi offset (R9). `ls` e `run` applicano la stessa regola. | -1 |
| **3. Ricerca** | Cerca il nome nella directory, confrontando fino al NUL. | Trova la voce con l'offset e la lunghezza dell'ELF. | -2 |
| **4. Slot** | Sceglie il primo slot libero (0, 1, 2, 3) con `task_program_slot_free`. Calcola `slot_base`, il puntatore `f` all'ELF direttamente in FLASH e la sua lunghezza `size`. | Il primo libero lo vuole la specifica. `f` punta dentro l'immagine: nessuna copia (R3). | -3 |
| **5. Controlli** | Chiama `elf_check_layout(f, size, &plan)` (A) e poi `elf_check_relocs(f, size, &plan)` (B). Poi ricontrolla da sé tre fatti del piano (`plan_inside_limits`). Solo letture. | B si fida delle garanzie di A: l'ordine è fisso e lo garantisce la SVC 13. Qualunque risposta diversa da 0 diventa -4. | -4 |
| | **Linea di non ritorno** | **Da qui in poi si scrive nello slot e nessun passo può più fallire.** | |
| **6. Scrittura** | Copia `copy_size` byte da `f + file_off` all'inizio dello slot. Azzera il BSS, da `bss_start` a `bss_end`. Chiama `elf_apply_relocs(f, slot_base)` (B), che somma la base dello slot a ogni indirizzo assoluto. | Il programma è linkato a 0 e va corretto per girare in quello slot. La FLASH non si scrive: si corregge la copia. | nessuno |
| **7. Task** | Chiama `task_program_create(slot, nome, slot_base + entry, slot_base + 4096, executable)` (codice dato), scrive `code_end` nel record del nuovo task e restituisce l'id. | Il task si crea solo a programma completo. `code_end` serve alla MPU di D. | id ≥ 4 |

**Lo scheletro del codice** (versione abbreviata: nel file ci sono i cast e più commenti).

```c
static uint32_t sys_program_run_impl(uint32_t a0, uint32_t a1, uint32_t a2)
{
    char name[16];
    if (copy_name(a0, name) != 0)   return -1;     /* 1  nome sicuro            */
    if (!image_valid())             return -1;     /* 2  immagine intera        */

    const struct img_dirent *found = find_entry(name);
    if (found == 0)                 return -2;     /* 3  ricerca                */

    int slot = first_free_slot();
    if (slot < 0)                   return -3;     /* 4  slot                   */
    uint32_t slot_base = (uint32_t)__program_slots_start + slot * SLOT_SIZE;
    const uint8_t *f   = (const uint8_t *)&__fs_image_start + found->offset;
    uint32_t size      = found->size;

    struct load_plan plan = { 0 };                 /* 5  controlli: solo letture */
    if (elf_check_layout(f, size, &plan) != 0)  return -4;    /* A */
    if (elf_check_relocs(f, size, &plan) != 0)  return -4;    /* B */
    if (!plan_inside_limits(&plan, size))       return -4;    /* rete di sicurezza */

    /* ======== linea di non ritorno: da qui in poi si scrive ======== */

    uint8_t *dst = (uint8_t *)slot_base;                      /* 6  scrittura */
    for (uint32_t i = 0; i < plan.copy_size; i++)  dst[i] = f[plan.file_off + i];
    for (uint32_t i = plan.bss_start; i < plan.bss_end; i++)  dst[i] = 0;
    elf_apply_relocs(f, slot_base);                           /* B */

    int id = task_program_create(slot, name,                  /* 7  task */
                                 slot_base + plan.entry, slot_base + SLOT_SIZE,
                                 LOADER_EXECUTABLE);
    if (id < 0)  return -4;                        /* incoerenza: non deve mai succedere */
    /* qui si scrive code_end nel record del task (campo aggiunto da Giulio) */
    return id;
}
```

**Le funzioni di supporto** (tutte `static`, sopra `sys_program_run_impl`, nello stesso file):

| Funzione | A cosa serve | Passo |
|---|---|---|
| `caller_can_read`, `copy_name` | il chiamante può leggere questo intervallo? copia sicura del nome | 1 |
| `img_entry`, `name_char_ok`, `same_name`, `image_valid` | accesso alle voci della directory, regole sui nomi, controllo dell'intera immagine | 2, 3 |
| `find_entry` | la voce con quel nome | 3 |
| `first_free_slot` | il primo slot libero | 4 |
| `plan_inside_limits` | i tre confronti di sicurezza sul piano di A | 5 |

Il resto di `syscall.c`, in particolare la tabella delle chiamate di sistema, non cambia. In cima si aggiunge `#include "kernel/loader.h"`.

## 5. Le scelte di progetto

Sono le decisioni che la specifica lascia aperte o che vanno giustificate. Conviene saperle spiegare con le parole «scelta, perché, limite».

1. **Prima si controlla, poi si scrive.** Tutti i piani prima della linea di non ritorno leggono soltanto. Il kernel è privilegiato e la MPU non lo ferma: una scrittura sbagliata danneggerebbe il kernel o un altro slot senza nessun fault. In più un rifiuto non lascia tracce: nessuno slot cambia e nemmeno l'id, tanto che dopo un file rifiutato il programma buono successivo ha ancora id 4.
2. **Il kernel non si fida del chiamante.** Il nome si copia byte per byte e ogni lettura è controllata contro la partizione del chiamante e la FLASH, con confronti scritti per non andare mai in overflow (`len <= fine - p`, non `p + len <= fine`). Non è un dettaglio teorico: la SVC 13 può essere chiamata anche da un programma caricato (test `nonzero_slot_relocation`).
3. **L'immagine si legge sul posto e si valida tutta, a ogni `run`.** `f` punta direttamente in FLASH, senza nessuna copia (R3, efficienza). Il controllo completo costa poco (al massimo 32 voci) e fa sì che `ls` e `run` concordino.
4. **L'ordine degli errori: -1, -2, -3, -4.** Il documento di progetto elenca i codici ma non dice quale prevale quando ce n'è più di uno: presenta l'ordine dei passi 10–13 come una proposta («da chiarire con il professore») e osserva che il controllo `slot_base + word` richiede di conoscere già lo slot. Si segue quell'ordine, quindi con quattro slot occupati la risposta è -3 anche se il file è rotto. La scelta va scritta nella documentazione.
5. **Un solo contratto, `loader.h`, che non si tocca.** Il prototipo di `elf_check_relocs` ha tre parametri `(f, size, p)` e il gruppo lo tiene com'è. Conseguenza: B non riceve la base dello slot, quindi il controllo dell'overflow di `slot_base + word` è un accordo con lui (sezione 6).
6. **Una rete di sicurezza sul piano di A (`plan_inside_limits`).** Dopo A e B, la SVC 13 ricontrolla tre fatti: i byte da leggere stanno nel file, i byte da scrivere stanno nei 3072 byte del programma, il BSS comincia prima di finire. Impedisce che un errore di A diventi una scrittura del kernel fuori dallo slot. È una difesa in profondità e **opzionale**: se A garantisce quei tre fatti, e lo si scrive nella documentazione, si può togliere. Chiamare A e B, invece, è obbligatorio.
7. **Il task si crea per ultimo, e `code_end` subito dopo.** `task_program_create` si chiama solo a programma completo, perché fa avanzare la `generation` e mette il task in READY. `code_end` si scrive nel record del nuovo task, cioè in `tasks[PROGRAM_TASK_FIRST + slot]` e non in `tcb_current`, che durante la SVC è il chiamante. È sicuro farlo qui perché lo scheduler non può interrompere la SVC (priorità 0x80 contro 0xE0 di PendSV).
8. **Nessuna stampa di debug.** Il test kit confronta l'uscita della UART byte per byte: la riga `loader:` la stampa il codice dato, la SVC 13 non stampa niente.
9. **Un limite noto: i resti nello slot.** Si azzera il BSS, come chiede la specifica, ma non l'intero slot: tra la fine del programma e lo stack restano i byte del programma precedente, e il nuovo programma può leggerli. Azzerare tutti i 4 KiB prima della copia eliminerebbe il limite, al costo di 4 KiB di scritture a ogni caricamento.
10. **Due correzioni vicine (SVC 11 e 12).** Scrivendo la SVC 13 sono emersi due difetti nel codice già presente: `run` con un nome più lungo di 15 caratteri rispondeva «program not found» invece di «program cannot be loaded» (ora -1, come vuole la specifica), e la SVC 12 non elencava un nome di esattamente 15 caratteri, perché il ciclo di copia si fermava un byte prima del NUL. La guida (Appendice A) propone una sola funzione di controllo dell'immagine, usata da `ls` e da `run`; la modifica va concordata con chi ha scritto SVC 11 e 12.

## 6. Il collegamento con gli altri

La SVC 13 vede gli altri solo attraverso `loader.h`, il flag `executable` e un campo del record del task. Per ogni persona: cosa la SVC 13 le dà, cosa si aspetta in cambio, cosa va concordato.

| Con chi | La SVC 13 fornisce | La SVC 13 si aspetta | Da concordare |
|---|---|---|---|
| **A**, Gianluigi e Athena (`elf_check_layout`) | `f`, `size` (l'ELF in FLASH, ancora tutto da controllare) e un piano vuoto | 0 oppure -4; piano riempito con `entry`, `file_off`, `copy_size`, `bss_start`, `bss_end`, `code_end` (multiplo di 512), `mem_end` ≤ 3072; nessuna scrittura | `bss_start == copy_size`; `copy_size` dentro il file e ≤ 3072; la regola dei 512 byte con Giulio |
| **B**, Simone (`elf_check_relocs`, `elf_apply_relocs`) | per il controllo: `f`, `size`, `plan` in sola lettura, solo se A ha risposto 0. Per la correzione: `f` e `slot_base`, dopo la copia | controllo: 0 oppure -4, e la riga `loader: unsupported relocation type N` per un tipo non ammesso. Correzione: somma la base a ogni `R_ARM_ABS32` dentro lo slot, senza errori | overflow di `slot_base + word`; dove stanno le sue funzioni; consegnare per prima `elf_apply_relocs` |
| **D**, Giulio (MPU e record del task) | `code_end` scritto nel record del task subito dopo la creazione; il flag `executable` | il campo `code_end` in `struct tcb`; una MPU che isola lo slot e applica il W^X con `code_end` | nome e unità del campo (offset o indirizzo); valore di `executable`; `.data` allineato a 512 byte in `prog.ld` |
| **Codice dato** (`task_program_create`) | `slot`, nome, `entry`, `stack_top`, `executable` | l'id, oppure -1 se un suo controllo interno fallisce (non deve succedere); la riga `loader:` stampata da lei | niente: non si riscrive né si copia |
| **E**, tutti | l'ordine degli errori e le scelte di progetto, da scrivere nella documentazione | test, file rotti, documentazione e demo | chi fa cosa nella demo |

**Cosa la SVC 13 garantisce agli altri.** A riceve l'ELF intero e una struttura vuota, senza nome né slot. B riceve il file solo dopo che A ha detto sì, quindi può contare sulle garanzie di A: header valido, tabella delle sezioni dentro il file, nessuna sezione RELA. `elf_apply_relocs` viene chiamata solo dopo che il controllo di B ha dato 0 e dopo la copia. La MPU di D trova `code_end` già scritto quando il task parte per la prima volta.

**Punti da chiarire con il gruppo prima della presentazione**

1. `loader.h` resta com'è: `elf_check_relocs` ha tre parametri e nessuno lo cambia da solo.
2. Overflow di `slot_base + word` (Simone): senza la base dello slot, un controllo prudente confronta la parola con la base dell'ultimo slot (rifiuta se `word > 0xFFFFFFFF - 0x20005000`); per uno esatto servirebbe un quarto parametro, da decidere tutti insieme.
3. Cosa garantisce A: `bss_start == copy_size`, `copy_size` dentro il file e ≤ 3072. Se lo garantisce per scritto, `plan_inside_limits` si può togliere.
4. `code_end`: regola dei 512 byte, unità (offset nello slot o indirizzo), nome del campo nel record del task e valore di `executable`, con A e Giulio.
5. Chi stampa `loader: unsupported relocation type N`: la proposta è Simone, dentro `elf_check_relocs`.
6. Ordine degli errori (-3 prima di -4) e scelta su `plan_inside_limits`: da scrivere nella documentazione.
7. Git: ognuno tocca solo i propri file (la SVC 13 solo `sys_program_run_impl` e le funzioni sopra di lei, in `syscall.c`); fine riga LF, come in `.gitattributes`.

## 7. Le parti che mancano

Nello snapshot del repository usato per questo documento (progetto base, 13 novembre) c'è solo il contratto `kernel/loader.h`: le implementazioni degli altri non ci sono. Prima della presentazione va aggiornata la colonna «Stato» con la situazione reale del gruppo.

| Parte | Chi | File previsti | Cosa deve fare | Stato nello snapshot |
|---|---|---|---|---|
| Controllo dell'ELF: `elf_check_layout` | A, Gianluigi e Athena | `kernel/elf.c`, `kernel/elf_sections.c` | header e tabelle dentro il file, segmenti `PT_LOAD`, limite di 3072 byte, sezioni, entry Thumb, calcolo di `code_end`; riempie il piano; tutto senza overflow | manca (c'è solo il prototipo) |
| Relocation: `elf_check_relocs`, `elf_apply_relocs` | B, Simone | `kernel/loader.c` | sezioni REL con bersaglio allocato, record e simboli dentro il file, siti nel programma, tipi ammessi (ABS32, THM_CALL, THM_JUMP24), riga `unsupported relocation type N`; somma della base agli ABS32 | manca |
| MPU: isolamento dello slot e W^X | D, Giulio | `kernel/mpu.c`, `kernel/mpu.h`, `kernel/task.h`, due righe di `kernel/task.c`, `programs/prog.ld` | regione r-x sul codice, rw- sul resto dello slot, `.data` allineato a 512 byte, campo `code_end` nel record del task | manca: la MPU di base ha una sola regione per slot |
| Collaudo, documentazione e demo | E, tutti | script di test, `README/` | suite completa a `failed=0` su 14, file rotti in modi nuovi, documentazione, demo | da fare a integrazione |
| SVC 13 (questa parte) | C, Antonio | `kernel/syscall.c` | i sette passi | codice pronto nella guida e provato con le versioni finte; da riportare e verificare nel repository; restano `code_end` (quando D aggiunge il campo) e `executable` da concordare |

Perché i test della suite dipendono da persone diverse:

| Test | Cosa verifica | Chi serve |
|---|---|---|
| `global_state` | copia, BSS a zero, dati e costanti | C + A + B |
| `thumb_ptr` | puntatore a funzione Thumb corretto dalla relocation, con il bit 0 a 1 | C + A + B |
| `nonzero_slot_relocation` | un programma chiama la SVC 13; il figlio gira nello slot 1 (base `0x20003000`) con la relocation giusta | C + A + B |
| `normal_return`, `explicit_exit` | fine normale e `sys_exit` (codice dato) | C + A + B, perché il programma deve prima partire |
| `kernel_store` | scrittura nella RAM del kernel: fault recuperabile, lo slot si riusa con id 8 | C + A + B (la MPU di base già basta) |
| `bad_magic`, `bad_header` | ELF con magic o header sbagliati: rifiuto, la shell resta viva | A (la SVC 13 traduce il suo -4) |
| `too_large` | programma di 3076 byte rifiutato | A |
| `bad_reloc` | relocation fuori dal programma | B |
| `unsupported_reloc` | tipo 3: stampa `loader: unsupported relocation type 3` e rifiuta | B |
| `truncated_elf` | ELF tagliato: rifiuto senza nessun fault | A e B |
| `wx_write_code`, `wx_exec_data` | W^X: scrittura nel codice (`CFSR=0x82`), salto nei dati (`CFSR=0x1`) | D (con la SVC 13 che scrive `code_end`) |

Sul `main` del gruppo, prima del loader, passavano 4 test su 14 (`bad_magic`, `bad_header`, `too_large`, `bad_reloc`), ma solo perché la SVC 13 rifiutava tutto: vanno ricontrollati quando A e B ci sono, perché dovranno rifiutare per il motivo giusto. Il peso nel voto è alto: «caricamento e relocation» vale 2,50 punti su 14, e se manca il caricamento ELF (B1) c'è una penalità di 2 punti, se manca il W^X (B2) di 1 punto. Il voto di squadra è lo stesso per tutti.

## 8. Verifica e prove

Le prove sono state eseguite sul codice della guida, in QEMU, su una copia del repository, con alcuni file in più che non fanno parte della consegna: le versioni finte di A e B (`kernel/elf_stub.c`), tre programmi di prova (`sleeper`, `launcher`, `ptr_test`) e due script che costruiscono immagini e piani rotti (`mkbroken.py`, `mkevil.py`). Le stesse prove vanno **ripetute sul repository vero** prima della presentazione (guida, sezione 13).

| Prova | Cosa dimostra | Esito |
|---|---|---|
| I nove programmi del test kit, uno dopo l'altro | copia, BSS, relocation (con lo stub), creazione del task; id 4, 8, 12, … nello slot 0 | stampe come previsto. `wx_write_code` e `wx_exec_data` finiscono senza fault perché il W^X non c'è ancora |
| `nonzero_slot` | relocation in uno slot diverso da 0: il figlio gira nello slot 1, base `0x20003000`, id 5, `thumb=1 result=42` | ok |
| `launcher` con tre `sleeper` | quattro slot pieni: il quinto avvio dà -3, nessuno slot cambia; gli id sono 5, 6, 7 | ok |
| `ptr_test` | quattro puntatori ostili al nome (RAM della shell, oltre la FLASH, `0xFFFFFFFF`, ultimo byte dello slot senza NUL) danno -1; un nome nella propria RAM parte | ok |
| Nomi di prova | un nome di 20 caratteri dà -1; un nome di 15 caratteri parte; un nome inesistente dà -2 | ok |
| 23 immagini IMG1 rotte, una violazione per volta (10 nell'intestazione, 11 nelle voci, 2 ancora valide) | ogni violazione dà -1; le due valide sono accettate | ok |
| Tre piani di carico «cattivi» (`copy_size` di 5000; `file_off + copy_size` in overflow; `bss_start > bss_end`) | `plan_inside_limits` li ferma con -4; subito dopo il programma buono ha ancora id 4, quindi nessuno slot è cambiato | ok |
| Compilazione con `-Wall -Wextra` | nessun avviso in `kernel/syscall.c` | ok |

Il test kit ufficiale non è stato lanciato con A, B e D veri, perché non esistono ancora. Quando saranno uniti: `python3 run_tests.py --suite big --kernel build/scratch-os.axf --image build/fs.img`, con obiettivo `failed=0` su 14 test.

## 9. Presentazione e demo

**Come funziona l'esame.** Presentazione orale di 20 minuti con tutti i membri, poi domande individuali e demo dal vivo. Ogni membro deve capire tutto il progetto: il professore può chiedere a chiunque qualunque parte, anche scritta dagli altri, e ogni risposta conta per il voto individuale (3 punti). La demo deve mostrare: caricamento normale, relocation in uno slot diverso da 0, rifiuto di un input sbagliato, recupero da un fault e il comportamento specifico della versione (ELF e W^X).

**Scaletta di 3 minuti per la tua parte**

1. **Cos'è (20 s).** «Mi sono occupato della SVC 13: è il punto d'ingresso del loader. La shell dice `run nome` e la SVC 13 coordina tutto, nell'ordine giusto.»
2. **La pipeline (40 s).** Lo schema della sezione 3: chi fa cosa, e il fatto che ci si parla solo tramite `loader.h` e il piano di carico.
3. **I sette passi (60 s).** Nome, immagine, ricerca, slot; poi i controlli di A e B; la linea di non ritorno; copia, BSS, relocation; il task. Dire i quattro errori.
4. **Le due scelte da difendere (40 s).** Prima si controlla, poi si scrive; il kernel non si fida di quello che arriva da un task (e nemmeno del piano, con `plan_inside_limits`).
5. **Il collegamento (20 s).** Cosa do e cosa mi aspetto da A, B e D; cosa manca, se manca ancora.

**La demo, comando per comando**

| Comando | Cosa dire |
|---|---|
| `run nosuch` | nome valido ma non in directory: passo 3, -2, la shell stampa «program not found» |
| `run abcdefghijklmnopqrst` | 20 caratteri: nessun NUL nei primi 16 byte, passo 1, -1 |
| `run print_plain` | passi 1–7; la riga `loader:` la stampa il codice dato quando creo il task; id 4 = generation 1, slot 0 |
| `run nonzero_slot` | il programma chiama a sua volta la SVC 13: il figlio va nello slot 1, base `0x20003000`. È la relocation con una base diversa da 0 |
| `run kernel_store` | il programma scrive nella RAM del kernel: fault recuperabile, lo slot torna libero subito e il programma dopo riusa lo slot 0 con l'id successivo (qui 16, la quarta volta che lo slot 0 viene usato) |

**Trascrizione reale** in QEMU, con le versioni finte di A e B (gli id dipendono da quante volte è già stato usato ogni slot)

```text
scratch> run nosuch
error: program not found
scratch> run abcdefghijklmnopqrst
error: program cannot be loaded
scratch> run print_plain
loader: print_plain slot=0 base=0x20002000 id=4
print_plain: hello from a loaded program
program print_plain exited
scratch> run nonzero_slot
loader: nonzero_slot slot=0 base=0x20002000 id=8
nonzero_slot: starting thumb_ptr
loader: thumb_ptr slot=1 base=0x20003000 id=5
thumb_function_pointer: thumb=1 result=42
program thumb_ptr exited
nonzero_slot: child status=0
program nonzero_slot exited
scratch> run kernel_store
loader: kernel_store slot=0 base=0x20002000 id=12
kernel_store: writing 0x20000000
MemManage in loaded program: CFSR=0x82 address=0x20000000
program kernel_store faulted
scratch> run print_plain
loader: print_plain slot=0 base=0x20002000 id=16
print_plain: hello from a loaded program
program print_plain exited
```

Per mostrare il rifiuto di un file rotto (-4) servono gli ELF rovinati dei test (A e B) e per il W^X i programmi `wx_write_code` e `wx_exec_data` (D). Per mostrare il -3 servono quattro programmi vivi insieme: `launcher` e `sleeper`, descritti nella guida (sezione 13.7), sono programmi di prova privati, da aggiungere a `PROG_MAP` solo se si vuole questa scena.

## 10. Domande possibili del professore

Le risposte sono brevi: servono per ricordare il filo, non da recitare.

### Sulla SVC 13

- **«Che ruolo ha la SVC 13 nel loader?»** È il punto d'ingresso e il coordinatore: riceve il nome dalla shell, controlla nome e immagine, trova il programma, sceglie lo slot, chiama nell'ordine A e B per i controlli e solo dopo copia, azzera il BSS, fa applicare le relocation e crea il task. Non sa come si controlla un ELF o come si correggono gli indirizzi: lo chiede agli altri e decide in base alle risposte.
- **«Cosa succede da `run print_plain` fino al tuo codice?»** `cmd_run` chiama `sys_program_run(name)`, che esegue `svc #13`. L'hardware salva r0–r3, r12, lr, pc e xPSR sullo stack del chiamante; `SVC_Handler` passa quel frame a `svc_dispatch`, che legge il numero dall'istruzione `svc` e chiama `table[13]`, cioè `sys_program_run_impl`. Il risultato va in `frame[0]` e diventa l'`r0` della shell.
- **«Che differenza c'è tra -1, -2, -3 e -4? Perché la shell stampa lo stesso messaggio per -1 e -4?»** -1: nome o immagine non validi. -2: nome valido ma assente. -3: nessuno slot libero. -4: file del programma non valido, deciso da A e B. La shell traduce -2 e -3 con messaggi propri e ogni altro negativo con «program cannot be loaded»; la distinzione serve agli sviluppatori e ai test, per capire chi ha detto no.
- **«Cosa succede con un nome di più di 15 caratteri? E con uno di 15?»** Con 16 o più manca il NUL nei 16 byte, quindi -1 e non -2, come vuole la specifica. Con 15 il nome è valido: se esiste il programma parte. Un nome che non può esistere, per esempio con caratteri non ammessi, non si controlla: semplicemente non si trova, -2.
- **«Perché scegli lo slot prima di guardare il file?»** È l'ordine proposto nel documento di progetto (passi 10–13), che osserva che il controllo `slot_base + word` presuppone di conoscere lo slot. Con il prototipo attuale B non riceve la base, ma l'ordine lascia aperta la strada. Conseguenza: con quattro slot occupati e un file rotto si risponde -3. Un altro ordine sarebbe lecito, purché scritto nella documentazione.
- **«Cosa succede al quinto `run` con quattro programmi vivi?»** Non c'è uno slot libero: -3, la shell stampa `error: no free slot` e nessuno slot cambia.

### Sicurezza e robustezza

- **«Perché copi il nome invece di usare direttamente il puntatore?»** Il puntatore viene da un task non privilegiato, mentre il kernel è privilegiato: senza controllo leggerebbe per conto del task memoria che il task non può leggere. Il nome si copia byte per byte in un buffer da 16 byte, ogni byte deve stare nella partizione del chiamante o in FLASH, e il controllo non va in overflow.
- **«Cosa vuol dire "prima si controlla, poi si scrive" e perché conta?»** I piani 1–3 leggono soltanto; solo se tutti dicono sì si scrive nello slot. Il kernel è privilegiato e la MPU non lo ferma: una scrittura sbagliata danneggerebbe il kernel o un altro slot senza nessun fault. Un file rotto non deve lasciare uno slot a metà né far avanzare l'id.
- **«Come evitate l'overflow nei controlli sugli intervalli?»** Si scrive `offset <= size && n <= size - offset` invece di `offset + n <= size`: in C a 32 bit la somma può ripartire da zero e un controllo ingenuo direbbe «dentro».
- **«Perché controlli tutta l'immagine a ogni `run` e non solo l'intestazione?»** La specifica vuole che un'immagine invalida in un punto qualsiasi sia rifiutata (R9), e fidarsi di un offset non controllato è pericoloso. Il costo è trascurabile (al massimo 32 voci) e `ls` e `run` così concordano. Anche se il programma cercato fosse presente, con un'immagine invalida si risponde -1.
- **«Cos'è `plan_inside_limits`? La potresti togliere?»** Sono tre confronti sul piano di A, dopo i controlli: i byte letti stanno nel file, i byte scritti stanno nei 3072 byte, il BSS comincia prima di finire. Trasformano un errore di A in un rifiuto -4 invece che in una scrittura fuori dallo slot. Si può togliere se A garantisce quei tre fatti e lo si scrive nella documentazione; chiamare A e B resta obbligatorio.
- **«Se A o B hanno un bug, cosa succede?»** Se il piano è incoerente lo ferma `plan_inside_limits`; per il resto (entry, `code_end`, relocation) il kernel si fida: è il contratto tra i piani. L'integrazione è la parte delicata, e quando un programma non parte il debug comincia dalla SVC 13, per capire se il problema è di A, di B o suo.
- **«Un programma caricato può chiamare la SVC 13?»** Sì, è il test `nonzero_slot_relocation`: il padre avvia il figlio. Il chiamante può quindi essere un task qualsiasi, e per questo il controllo del nome guarda la partizione del chiamante e non solo la FLASH.
- **«Il kernel azzera tutto lo slot prima della copia?»** No, solo il BSS, come chiede la specifica. Tra la fine del programma e lo stack restano i byte del programma precedente: è un limite noto, eliminabile azzerando i 4 KiB a ogni caricamento.

### Caricamento e relocation

- **«Cos'è la relocation e perché serve? Chi la fa?»** Il programma è linkato all'indirizzo 0 ma gira in uno slot (`0x20002000`, …). Ogni parola che contiene un indirizzo assoluto va corretta sommandole la base dello slot. Il linker, con `--emit-relocs`, lascia nell'ELF l'elenco dei punti da correggere; lo legge e lo applica B, la SVC 13 la chiama dopo la copia.
- **«Perché `R_ARM_ABS32` si corregge e `R_ARM_THM_CALL` no?»** ABS32 contiene un indirizzo assoluto e va sommato alla base. THM_CALL e THM_JUMP24 sono salti relativi alla posizione dell'istruzione: codice e destinazione si spostano insieme, quindi la distanza non cambia. Vanno solo verificati.
- **«Perché il bit Thumb resta a 1?»** La base dello slot è un multiplo di 4096, quindi ha i 12 bit bassi a zero: sommarla a un puntatore Thumb non cambia il bit 0. Se diventasse 0, il primo salto con `BX` o `BLX` darebbe un UsageFault, perché il Cortex-M3 non ha il modo ARM.
- **«Perché la relocation si applica nello slot, dopo la copia, e non nel file?»** L'immagine sta in FLASH e non si scrive mai, e lo stesso file può finire in slot diversi con basi diverse: si copia il programma com'era e si correggono le parole nella copia. L'ordine conta: se la relocation precedesse la copia, la copia sovrascriverebbe le correzioni.
- **«Cos'è il BSS e perché si azzera?»** Sono le variabili globali che partono da zero: il C promette zero, ma nel file non hanno byte, c'è solo la dimensione. Lo slot contiene i resti del programma precedente, quindi va azzerato.
- **«Cos'è il piano di carico e perché serve?»** È la `struct load_plan` che A riempie e gli altri leggono: da dove copiare, quanti byte, cosa azzerare, dove si parte, dove finisce il codice, dove finisce il programma. Così i piani non devono sapere come l'ELF è fatto: si parlano solo attraverso quella struttura.

### Collegamento con gli altri e con la MPU

- **«Come hai provato la SVC 13 senza A e B?»** Con versioni finte: un `elf_check_layout` che riempie il piano con i valori di un ELF valido, un `elf_check_relocs` che risponde sempre 0 e un `elf_apply_relocs` semplice. Così si provano copia, BSS e creazione del task sui programmi buoni. Quando arrivano le versioni vere si cancella il file delle finte.
- **«Perché `elf_check_relocs` ha tre parametri? Come si controlla `slot_base + word` senza la base?»** Il prototipo di `loader.h` è quello e il gruppo lo tiene. B può confrontare la parola con la base dell'ultimo slot e rifiutare se `word > 0xFFFFFFFF - 0x20005000`: rifiuta qualcosa in più del necessario, ma solo parole tra `0xDFFFB000` e `0xDFFFDFFF`, che nessun programma reale contiene. Per un controllo esatto servirebbe un quarto parametro, da decidere insieme.
- **«Come si collega la tua parte alla MPU di Giulio?»** Con due cose: il flag `executable` che passo a `task_program_create` e il campo `code_end` che scrivo nel record del task subito dopo la creazione. A ogni cambio di task la MPU legge la partizione e `code_end` e programma le regioni.
- **«Perché `code_end` deve essere un multiplo di 512?»** Una regione MPU da 4 KiB si divide in 8 sottoregioni da 512 byte. Il confine tra codice e dati deve cadere su un bordo di sottoregione: se cadesse a metà, quella sottoregione sarebbe scrivibile ed eseguibile insieme e il W^X sarebbe violato. A lo controlla, Giulio allinea `.data` in `prog.ld`.
- **«Cos'è `executable` e perché da solo non dà W^X?»** È il flag che il codice dato passa a `mpu_set_task_region`: a 1 la regione RAM del task è eseguibile. Con una sola regione per lo slot decide tutto o niente; per il W^X serve una seconda regione, di Giulio, con numero più alto sul solo codice. Con `executable` a 0 senza W^X, la prima istruzione darebbe `CFSR=0x1`.
- **«Perché `code_end` si scrive in `tasks[PROGRAM_TASK_FIRST + slot]` e non in `tcb_current`?»** Durante la SVC `tcb_current` è il chiamante, la shell o un programma padre; il record del nuovo task è quello dello slot scelto.

### Vita del programma (codice dato)

- **«Cosa fa `task_program_create`?»** Controlla che lo slot sia libero, che `entry` non sia 0 e che `stack_top` sia esattamente `base + 4096`; copia il nome (al massimo 15 caratteri); aumenta la `generation` e calcola l'id; riempie il record; costruisce il frame iniziale da 64 byte con `pc = entry`, `lr = task_exit` e xPSR Thumb; mette il task in READY e stampa la riga `loader:`. È codice dato: non si riscrive.
- **«Com'è fatto l'id? Perché il secondo programma nello slot 0 ha id 8?»** `id = generation * 4 + slot`, e la `generation` dello slot aumenta a ogni programma. Nello slot 0 gli id sono 4, 8, 12, …; un id vecchio non si confonde con quello del programma attuale nello stesso slot, e la SVC 14 lo riconosce.
- **«Cosa succede quando il programma finisce o va in fault?»** Il codice dato stampa `program <nome> exited` (o la riga del fault e `faulted`), salva lo stato, mette il record in UNUSED, sveglia chi aspetta nella SVC 14 e chiede un cambio di task: lo slot è subito riusabile e la shell ristampa il prompt. Un fault irrecuperabile, per esempio durante il salvataggio dello stack frame, ferma invece il sistema.
- **«Perché durante la SVC 13 nessun altro task può interferire?»** `SVC_Handler` ha priorità 0x80, PendSV e SysTick 0xE0, che è meno urgente: lo scheduler non può interrompere la SVC. Scelta dello slot, copia e scrittura di `code_end` sono quindi indivisibili rispetto al cambio di task.
- **«Il programma può partire prima che la shell chiami la SVC 14?»** Sì: dopo la creazione il task è READY e può partire al primo cambio di task. Se finisce prima, il suo stato resta salvato e la SVC 14 lo restituisce subito.

### Sulle parti degli altri

- **«Cosa controlla A in un ELF?»** Header (52 byte, magic `7f 45 4c 46`, 32 bit, little-endian, `ET_EXEC`, ARM), tabelle dei program e dei section header dentro il file, almeno un `PT_LOAD` con byte nel file e `filesz <= memsz`, programma entro i 3072 byte, sezioni allocate senza sovrapposizioni, BSS in fondo, niente RELA né dynamic, entry Thumb dentro il codice. Calcola `code_end` e riempie il piano. Ogni calcolo è protetto dall'overflow.
- **«Cosa controlla B nelle relocation?»** Le sezioni REL con bersaglio allocato (`sh_info`): record e tabella dei simboli dentro il file, simbolo esistente e definito, sito dentro il programma. Ammette `R_ARM_ABS32` (sito multiplo di 4 e prima del BSS), `R_ARM_THM_CALL` e `R_ARM_THM_JUMP24` (solo verificati). Per un altro tipo stampa `loader: unsupported relocation type N` e rifiuta. Le relocation di sezioni non allocate, come il debug, si ignorano.
- **«Come funziona il W^X con la MPU?»** Nel disegno proposto nella chat del gruppo: una regione sullo slot intero, rw- e non eseguibile, più una regione con numero più alto sul solo codice, r-x. Dove si sovrappongono vince quella col numero più alto, quindi nessuna zona resta scrivibile ed eseguibile. La riprogrammazione avviene a ogni cambio di task; il disegno vero è quello che sceglie Giulio.
- **«Cosa significano i due fault dei test W^X?»** `CFSR=0x82` è una violazione di accesso ai dati con indirizzo valido, la scrittura nel codice. `CFSR=0x1` è una violazione di accesso alle istruzioni, il salto nei dati. Sono fault recuperabili: il codice dato libera lo slot e la shell torna al prompt.

### Domande trabocchetto

- **«E se chiamassi `task_program_create` prima dei controlli?»** Farebbe avanzare la `generation` anche per un programma rifiutato, e metterebbe in READY un task con lo slot ancora da scrivere. Il test lo rivelerebbe: dopo un rifiuto il programma buono avrebbe id 8 invece di 4.
- **«E se applicassi le relocation prima della copia?»** La copia sovrascriverebbe le correzioni e lo slot resterebbe con gli indirizzi validi per lo 0: i puntatori punterebbero nella FLASH invece che nel programma, con risultati sbagliati o un fault al primo accesso non permesso.
- **«E se passassi `stack_top = slot_base + 3072`?»** `task_program_create` rifiuta qualunque valore diverso da `base + 4096` e restituisce -1.
- **«E se un nome fosse `0xFFFFFFFF` o puntasse alla RAM del kernel?»** Il controllo di intervallo lo rifiuta: -1, senza leggere niente (test `ptr_test`).
- **«E se togliessi il controllo dell'immagine nella SVC 13?»** Un offset non controllato in una voce rotta potrebbe far leggere al kernel fuori dall'immagine e far cadere il kernel, o caricare byte a caso come se fossero un programma: sarebbe un cedimento di R9.

## 11. Glossario

| Termine | Significato |
|---|---|
| SVC | istruzione che chiede un servizio al kernel; la 13 è `sys_program_run` |
| Slot | area fissa di RAM da 4 KiB dove vive un programma caricato; ce ne sono quattro |
| IMG1 | il formato dell'immagine in FLASH: intestazione, directory dei nomi, file dei programmi |
| ELF | il formato dei programmi prodotto dal linker; nella versione big l'immagine lo contiene così com'è |
| `PT_LOAD`, segmento | la parte dell'ELF che va caricata in memoria |
| Sezione allocata | sezione che occupa memoria quando il programma gira (il debug, per esempio, no) |
| BSS | variabili globali a zero iniziale, senza byte nel file |
| Relocation | indirizzo assoluto nel programma da correggere sommandogli la base dello slot |
| `R_ARM_ABS32`, `R_ARM_THM_CALL` | tipi di relocation: indirizzo assoluto da correggere; salto relativo da verificare soltanto |
| Bit Thumb | il bit 0 di un indirizzo di funzione, sempre 1 sul Cortex-M3 |
| Piano di carico | la `struct load_plan` riempita da A: cosa copiare, azzerare, dove si parte |
| `code_end` | confine tra codice e dati nello slot, multiplo di 512 |
| MPU, W^X | unità che limita gli accessi alla memoria; W^X: un'area è scrivibile o eseguibile, mai entrambe |
| Fault recuperabile | errore di un programma caricato dopo cui il sistema libera lo slot e prosegue |
| `generation`, id | contatore dello slot che cresce a ogni programma; l'id del task è `generation * 4 + slot` |
| Linea di non ritorno | il punto della SVC 13 dopo il quale si scrive nello slot e non si può più fallire |
