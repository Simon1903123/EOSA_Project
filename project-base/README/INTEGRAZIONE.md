# Integrazione delle tappe 6-7 in `main`

Documentazione del progetto Scratch-OS (loader di programmi, versione big).

Questo documento racconta come i branch di lavoro delle tappe 6 e 7 sono stati
uniti in `main`, quali bug sono emersi solo mettendo insieme i pezzi, come sono
stati corretti e quali controlli automatici proteggono `main` da allora.

Il merge su GitHub è stato fatto con *squash*: in `main` c'è un solo commit,
`beb56c7` ("Integrate steps 6-7 …"), e la storia dei singoli commit non si vede
più da `git log`. Questo file la ricostruisce.

| Sezione | Contenuto |
|---|---|
| [1](#1-cosa-è-stato-unito) | I branch uniti e chi li ha scritti |
| [2](#2-come-è-stato-fatto-il-merge) | Il branch `integration` e i passi del merge |
| [3](#3-i-quattro-bug-emersi-dal-merge) | I quattro bug emersi dal merge |
| [4](#4-controlli-automatici-ci) | I controlli automatici su GitHub e i target `make` |
| [5](#5-formattazione-del-codice) | La formattazione con clang-format-18 e black |
| [6](#6-risultati) | Risultati dei test |
| [7](#7-fix-successivi) | Fix fatti dopo l'integrazione |
| [8](#8-limiti-noti-e-note-pratiche) | Limiti noti e note pratiche |

---

## 1. Cosa è stato unito

Tutti i branch partono da `main` = `bf4848c` (13/11, "Rimosso Warning - make
programs").

| Branch | Chi | Ultimo commit | Contenuto |
|---|---|---|---|
| `6_Elf_check` | Gianluigi | `b2ba714` (20/11) | `kernel/elf.c`: controllo dell'header ELF e dei segmenti `PT_LOAD`, riempimento di `elf_view` e `load_plan` (`elf_check_layout`). `kernel/elf_internal.h`. `ALIGN(512)` in `programs/prog.ld`. `README/ELF_CHECK.md` |
| `6_Elf_Check` (C maiuscola) | Athena, fix finale di Simone | `fea54e0` (23/11) | `kernel/elf_sections.c`: controllo delle sezioni, calcolo di `code_end`, controllo del confine codice/dati (`elf_check_sections`). `README/ELF_SECTION.md` |
| `6_Reloc` | Simone | `856794b` (19/11) | `kernel/loader.c`: controllo (`elf_check_relocs`) e applicazione (`elf_apply_relocs`) delle rilocazioni |
| `6_Loader` | Antonio, fix finali di Simone | `2711ef9` (23/11) | `kernel/syscall.c`: SVC 13 completa (`sys_program_run_impl`). `README/SVC13_LOADER.md` |
| `7_MPU` | Giulio | `84944e6` (20/11) | `kernel/mpu.c`: `mpu_set_program_regions()`. Campo `code_size` nel tcb e `task_program_set_code_size()` in `kernel/task.c`/`task.h`. W^X in `task_switch_context`. `README/MPU_WX.md` |

Il flusso di `run <nome>` dopo l'integrazione, in breve:

```
SVC 13 (syscall.c)
  copy_name -> image_valid -> find_entry -> first_free_slot
  elf_check_layout   (elf.c)          header, segmenti, load_plan
    elf_check_sections (elf_sections.c) sezioni, entry, code_end
  elf_check_relocs   (loader.c)       tutte le rilocazioni, prima di scrivere
  plan_inside_limits (syscall.c)      ricontrollo dei limiti dello slot
  copia + azzeramento BSS + elf_apply_relocs
  task_program_create                 (codice dato dal prof)
  task_program_set_code_size          (attiva W^X per quel task)
```

---

## 2. Come è stato fatto il merge

1. Branch `integration` creato da `origin/main`.
2. Merge dei cinque branch, uno alla volta e in quest'ordine: `6_Elf_check`,
   `6_Elf_Check`, `6_Reloc`, `6_Loader`, `7_MPU`. **Nessun conflitto git.**
3. Commit `a5bf66c`: controlli automatici, test kit aggiornato del prof e
   target `make test` / `make format` (sezione 4).
4. Commit `db2238e`: formattazione di tutto il codice (sezione 5).
5. Commit `53a695b` e `189587f`: `build/fs.img` e `build/scratch-os.axf`
   rigenerati dal codice attuale. Quelli arrivati da `6_Elf_Check` erano vecchi:
   `fs.img` era il segnaposto da 25 byte `SCRATCH-OS-PROJECT-IMAGE`, non
   un'immagine IMG1.
6. Pull request `integration` → `main`, con tutti i controlli verdi, unita con
   squash in `beb56c7` (23/11).

Il fatto che git non segnali conflitti **non** vuol dire che il codice unito
funzioni: i quattro bug della sezione 3 erano tutti invisibili branch per branch
e sono comparsi solo compilando e testando il merge.

---

## 3. I quattro bug emersi dal merge

| # | Bug | Effetto | File | Corretto in |
|---|---|---|---|---|
| 1 | Firma di `elf_check_relocs` diversa fra due branch | la build si ferma | `kernel/syscall.c` | `9411fa3` (`6_Loader`) |
| 2 | `code_size` mai impostato | W^X spento: slot tutto RWX | `kernel/syscall.c` | `9411fa3`, `2711ef9` (`6_Loader`) |
| 3 | `.data` nella stessa regione MPU del codice | dati eseguibili (viola B2) | `programs/prog.ld` | `b2ba714` (`6_Elf_check`) |
| 4 | Controllo del confine codice/dati che non scatta mai | controllo morto | `kernel/elf_sections.c` | `0fa0b7f` + `fea54e0` (`6_Elf_Check`) |

### Bug 1: firma di `elf_check_relocs`

- **Sintomo.**
  `kernel/syscall.c:482: error: too few arguments to function 'elf_check_relocs'`.
- **Causa.** Su `6_Reloc` il prototipo in `kernel/loader.h` aveva ricevuto un
  quarto argomento, `uint32_t slot_base`, che serve a controllare che
  `slot_base + parola` non vada oltre i 32 bit. Su `6_Loader` la chiamata usava
  ancora i 3 argomenti del prototipo di `main`. Ogni branch, preso da solo, era
  coerente, quindi git non poteva accorgersene.
- **Fix.** Passare `slot_base`, che `sys_program_run_impl` calcola già prima:
  ```c
  if ( elf_check_relocs( f, size, &plan, slot_base ) != 0 )
      return (uint32_t) -4;
  ```
- **Lezione.** Chi cambia un header condiviso (`loader.h`, `elf_internal.h`)
  lo dice al gruppo prima di pushare.

### Bug 2: W^X non si attivava

- **Sintomo.** Dopo il fix 1 la build passava, ma `wx_write_code` e
  `wx_exec_data` stampavano `program ... exited` invece di andare in
  MemManage.
- **Causa.** `task_switch_context` usa le regioni W^X solo se
  `executable && code_size > 0`. Nessuno chiamava
  `task_program_set_code_size()`: in `syscall.c` c'era ancora una riga
  commentata che scriveva un campo `code_end` inesistente nel tcb. Con
  `code_size = 0` restava la regione unica e, siccome il loader passa
  `LOADER_EXECUTABLE = 1`, tutto lo slot era leggibile, scrivibile ed
  eseguibile.
- **Fix.** Chiamare la funzione **dopo** `task_program_create`, perché
  `task_program_create` rimette `code_size` a 0:
  ```c
  if ( id < 0 )
      return (uint32_t) -4;

  task_program_set_code_size( (unsigned) slot, plan.code_end );
  return (uint32_t) id;
  ```
  Non c'è rischio che il programma parta prima di questa riga: siamo dentro
  l'handler della SVC e il cambio di task (PendSV) avviene solo dopo il
  ritorno.

### Bug 3: dati del programma eseguibili

- **Sintomo.** Con i fix 1 e 2, `wx_exec_data` saltava nei propri dati senza
  fault.
- **Causa.** `mpu_set_program_regions()` divide lo slot da 4 KiB in 8
  sottoregioni da 512 byte e rende "codice" (lettura + esecuzione, niente
  scrittura) le prime `ceil(code_size / 512)`. Senza allineamento, `.data`
  cominciava subito dopo `.text`:
  ```
  [ 1] .text   00000000  0002fc   AX
  [ 3] .data   000002fc  000004   WA
  ```
  `code_size` = 0x2fc (764 byte), quindi `ceil(764 / 512)` = 2 sottoregioni di
  codice: 0x000-0x3ff. `.data`, a 0x2fc, finiva dentro la parte eseguibile e
  di sola lettura.
- **Requisiti violati.** B2: "Data and stack can be read and written but not
  executed". §6.4: "data and stack must refuse instruction fetches".
- **Fix.** In `programs/prog.ld`, far cominciare `.data` su un confine di 512
  byte:
  ```
  .text : { *(.text._start) *(.text*) *(.rodata*) . = ALIGN(4); }
  . = ALIGN(512);
  .data : { *(.data*) . = ALIGN(4); }
  ```
- **Costo.** Al massimo 511 byte in più per programma. Tutti i programmi del
  test kit restano sotto il limite di 3072 byte (§6.4).
- **Nota.** Il testo non impone 512: è la nostra scelta, legata alla
  dimensione delle sottoregioni MPU.

### Bug 4: controllo del confine codice/dati morto

- **Sintomo.** Nessuno visibile nei test. È un controllo che non rifiutava mai
  niente.
- **Causa.** In `elf_check_sections()` la variabile `first_writable_addr`
  veniva inizializzata a `0xFFFFFFFF` e confrontata alla fine, ma mai
  aggiornata nel ciclo sulle sezioni. Il confronto finale era quindi sempre
  falso.
- **Primo tentativo di fix (`0fa0b7f`): sbagliato.** È stata aggiunta la riga
  giusta, ma senza graffe:
  ```c
  if ( sh.flags & SHF_WRITE )
      seen_write = 1;
      if(sh.addr<first_writable_addr) first_writable_addr = sh.addr;
  else if ( seen_write )
  ```
  In C il secondo `if` non fa parte del blocco `SHF_WRITE`, perché senza graffe
  l'`if` controlla una sola istruzione. Veniva quindi eseguito per **ogni**
  sezione: `first_writable_addr` prendeva l'indirizzo di `.text` (0) e il
  controllo rifiutava **tutti** i programmi. Risultato: small 4/10, big 4/14,
  tutti con `error: program cannot be loaded`. Il compilatore lo segnalava:
  `warning: this 'if' clause does not guard... [-Wmisleading-indentation]`.
- **Fix definitivo (`fea54e0`).**
  ```c
  if ( sh.flags & SHF_WRITE )
  {
      seen_write = 1;
      if ( sh.addr < first_writable_addr )
      {
          first_writable_addr = sh.addr;
      }
  }
  else if ( seen_write )
  {
      return -4;
  }
  ```
- **Legame con il bug 3.** Con questo controllo attivo ma **senza**
  `ALIGN(512)` in `prog.ld`, la prima sezione scrivibile cade nella stessa
  sottoregione della fine del codice e tutti i programmi validi vengono
  rifiutati (big 4/14). Per questo i fix 3 e 4 vanno sempre insieme.

---

## 4. Controlli automatici (CI)

Dalla PR di integrazione, ogni pull request e ogni push su `main` fanno partire
due workflow GitHub Actions. I file sono nella radice del repository, in
`.github/workflows/`. Ripresi dalla vecchia PR #1, che non si poteva più unire
perché partiva da prima di "NUOVO INIZIO".

| Workflow | Job | Cosa fa | Comando equivalente in locale |
|---|---|---|---|
| `tests.yml` | Host tool unit tests | 16 test di `tools/mkimage.py` | `python3 -m unittest discover -s tools` |
| `tests.yml` | Kernel build and big-suite configuration | controlla il test kit e compila il kernel | `make test-list && make` |
| `tests.yml` | Big suite in QEMU | compila tutto e lancia small + big | `make test` |
| `format.yml` | clang-format | il codice C rispetta `.clang-format` | `make format-check` |
| `format.yml` | black | il codice Python rispetta black | `black --check .` |

Tutti i comandi vanno lanciati da `project-base/`.

**Il test kit è dentro il repository**, in `project-base/test-kit/`, ed è la
versione **aggiornata** del prof (annuncio dell'11/11). Nella versione vecchia,
quella dello zip, `wx_write_code` voleva esattamente
`address=0x20002280`. Quella nuova accetta qualunque indirizzo pari in
`0x20002000..0x20002fff`. Il nostro build mette `main` a `0x200022b4`: con il
kit vecchio il test risulta FAIL, con quello nuovo PASS.

Nuovi target del `Makefile`:

| Target | Cosa fa |
|---|---|
| `make test` | `test-small` + `test-big` |
| `make test-small` / `make test-big` | compila kernel e programmi, fa generare al test kit la sua immagine (i nostri ELF più i file rovinati) in `build/test-kit/<suite>/fs.img`, poi lancia la suite in QEMU |
| `make test-list` | elenca i test delle due suite (serve solo Python) |
| `make format` | riformatta il codice C con clang-format-18 |
| `make format-check` | controlla soltanto, senza modificare |

`make test` **non** usa `build/fs.img`: il test kit costruisce un'immagine
sua.

---

## 5. Formattazione del codice

Commit `db2238e`: tutto il codice C del kernel e delle librerie (21 file) è
stato riformattato con **clang-format-18**, secondo `project-base/.clang-format`.
`tools/mkimage.py` è stato riformattato con **black**. Il comportamento non
cambia: cambiano solo spazi, graffe e a capo.

Sono esclusi, perché vanno lasciati come li ha distribuiti il prof:

- i programmi di test in `programs/*.c`;
- tutto `test-kit/`.

**Attenzione alla versione.** Il controllo su GitHub usa clang-format-**18**.
Versioni diverse, come quella di default di Fedora, possono formattare alcune
righe in modo diverso: in quel caso `make format` in locale "sistema" il codice
in un modo che il controllo su GitHub rifiuta. Se `make format-check` passa in
locale ma fallisce su GitHub, la causa è quasi sempre questa.

---

## 6. Risultati

| Stato | small | big |
|---|---:|---:|
| Merge dei 5 branch prima dei fix (20/11) | non compila | non compila |
| + fix 1 e 2 | | 13/14 (`wx_exec_data` fallisce) |
| + fix 4 senza fix 3 | | 4/14 |
| + fix 1-4, cioè `main` = `beb56c7` | **10/10** | **14/14** |

Verificato sia su Ubuntu 24.04 (GCC 13, come la CI) sia su Fedora (GCC 15).

---

### Come riprodurre il risultato

Da `project-base/`:

```bash
make clean
make
make test        # small 10/10, big 14/14
make format-check
```

Prerequisiti:

- ARM bare-metal GCC: `gcc-arm-none-eabi` su Ubuntu,
  `arm-none-eabi-gcc-cs` + `arm-none-eabi-newlib` su Fedora;
- `qemu-system-arm`;
- Python 3;
- per i controlli di formattazione, clang-format-18 e black 26.1.0.
