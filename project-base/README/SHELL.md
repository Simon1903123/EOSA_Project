# Shell di scratch-OS — appunti

## 1. Cos'è la shell

La shell è un task: un programma che il kernel esegue a turno con gli altri task. Gira in modalità non privilegiata, quindi non può usare direttamente l'hardware né la memoria del kernel. Quando le serve qualcosa (leggere un tasto, stampare, avviare un programma) usa una system call, cioè l'istruzione `svc`, e il kernel esegue l'operazione al posto suo.

## 2. Come parte la shell

1. `Reset_Handler` (`boot/startup.s`) prepara la memoria e chiama `main()`.
2. `main()` (`src/main.c`) inizializza UART, timer e MPU. Poi esegue `svc #5`, che chiede al kernel di avviare i task. `main()` non riprende più.
3. `SVC_Handler` (`kernel/svc_entry.s`) riceve la `svc` e passa il controllo a `svc_dispatch`.
4. `svc_dispatch` (`kernel/syscall.c`) guarda il numero della `svc` (5) e chiama `task_start_first`.
5. `task_start_first` (`kernel/task.c`) chiama `task_project_configure()`. Nel kernel questa funzione è definita come *weak* e vuota: se un altro file ne definisce una versione, il linker usa quella. La nostra è in `src/shell.c`.
6. La nostra `task_project_configure()` chiama `task_configure_shell(...)`, che salva nella tabella dei task i dati della shell: nome, funzione di partenza, stack e memoria. La shell non viene ancora eseguita.
7. `task_start_first` riprende: copia in RAM i valori iniziali delle variabili della shell, prepara il suo stack e la segna come pronta. Poi la sceglie come primo task da eseguire.
8. Il kernel esce dalla `svc` e la CPU passa alla shell, in modalità non privilegiata. Parte `shell_main()`.

Da qui in poi lo scheduler alterna la shell e l'idle task.

`main.c` non va modificato.

## 3. Dove sta la shell in memoria

Il codice della shell sta in FLASH, insieme al resto del codice. Stack e variabili stanno nella sua partizione: 4 KiB di RAM riservati alla shell. La MPU le permette di usare solo questa zona della RAM.

| Zona | Indirizzi | Simboli del linker |
|---|---|---|
| stack (1 KiB) | `0x20000000..0x200003FF` | `_shell_part_start`, `_shell_stack_top` |
| variabili di `shell.o` | da `0x20000400` | `_shell_data_start`, `_shell_data_end` |
| spazio libero | fino a `0x20000FFF` | `_shell_part_end` |

I valori iniziali delle variabili sono salvati in FLASH, all'indirizzo `_shell_data_load`.

Il linker script mette nella partizione solo le variabili del file `shell.o`, quindi il sorgente deve chiamarsi `src/shell.c`. Una variabile della shell scritta in un altro file finirebbe fuori dalla partizione e il suo uso causerebbe un fault.

## 4. Tappa 1 — registrare la shell

In `shell.c` ci sono due funzioni:

- `task_project_configure()`: la chiama il kernel una sola volta, all'avvio. Registra la shell e termina.
- `shell_main()`: è il codice della shell. Gira come task e non termina mai.

**Simboli del linker.** `_shell_stack_top` e gli altri sono indirizzi stabiliti dal linker script, non variabili. Per usarli in C vanno dichiarati con `extern`, come array, nello stesso modo dei simboli `_idle_` in `task.c`. Il nome di un array, usato da solo, vale il suo indirizzo. `_shell_data_load` è `const` perché indica una zona di FLASH, che si può solo leggere.

| Parametro | Cosa si passa | Perché |
|---|---|---|
| `name` | `"shell"` | nome del task |
| `entry` | `shell_main` | indirizzo della funzione di partenza. Scrivendo `shell_main()` la funzione verrebbe eseguita subito, dentro il kernel |
| `stack_top` | `_shell_stack_top` | il kernel prepara lo stack iniziale a partire da qui |
| `part_start`, `part_end` | `(uint32_t) _shell_part_start`, `(uint32_t) _shell_part_end` | servono per configurare la MPU e per controllare i puntatori passati alle system call |
| `data_start`, `data_end`, `data_load` | i simboli, senza cast | il kernel copia i valori iniziali da `data_load` (FLASH) a `data_start..data_end` (RAM) |

## 5. Tappa 2 — prompt, lettura ed eco

**`read_line()`.** Legge una riga un carattere alla volta con `sys_read()`. `sys_read()` non aspetta: restituisce il carattere ricevuto, oppure `-1` se non è arrivato niente. In quel caso si chiama `task_delay(1)`: la shell si sospende per 10 ms, nel frattempo gira l'idle task, e poi riprova. Con `task_yield()` lo scheduler tornerebbe subito alla shell, e la CPU resterebbe sempre occupata.

Per ogni carattere ricevuto:

- se è `'\r'` o `'\n'`: va a capo, chiude la stringa con `'\0'` e termina;
- se la riga ha meno di 31 caratteri: salva il carattere e lo ristampa (eco);
- altrimenti: ignora il carattere, senza stampare niente.

Il buffer è di 32 byte: 31 caratteri più `'\0'`. Non si stampano messaggi extra, perché i test confrontano l'output carattere per carattere.

Per l'eco si usa `printf("%c", ch)`: se la UART non accetta subito tutti i byte, `printf` riprova. `sys_write` invece si ferma.

**`shell_main()`.** Ripete all'infinito: stampa `scratch> `, legge una riga con `read_line()` e la passa a `execute()`. Non deve mai terminare: se terminasse, il kernel chiuderebbe il task della shell per sempre.

## 6. Tappa 3 — interpretazione dei comandi

La specifica accetta solo tre comandi, scritti esattamente così: `help`, `ls` e `run` seguito da uno spazio e da un nome non vuoto. Gli spazi non vengono tolti: un secondo spazio dopo `run` fa parte del nome, e `help` o `ls` seguiti da qualunque carattere non sono comandi validi. Una riga vuota e qualunque altro input stampano `error: unknown command`.

**`skip_prefix(line, prefix)`.** Confronta l'inizio di `line` con `prefix`, un carattere alla volta. Se `line` inizia con `prefix`, restituisce un puntatore al resto della riga, subito dopo il prefisso; altrimenti restituisce 0. Una sola funzione copre due casi:

- confronto esatto: il prefisso corrisponde e il resto è la stringa vuota (`*rest == '\0'`);
- comando con argomento: per `"run "` il resto è il nome del programma.

**`execute(line)`.** Prova i comandi in ordine:

```c
if ((rest = skip_prefix(line, "help")) && *rest == '\0')        { /* stampa l'help */ }
else if ((rest = skip_prefix(line, "ls")) && *rest == '\0')     { cmd_ls(); }
else if ((rest = skip_prefix(line, "run ")) && *rest != '\0')   { cmd_run(rest); }
else                                                            { printf("error: unknown command\n"); }
```

Per `run`, `rest` punta dentro il buffer della riga: il nome non viene copiato e resta sullo stack della shell, nella sua partizione.

**`help`** stampa esattamente:

```
help - show commands
ls - list programs
run <n> - run a program
```

Le tre righe sono stampate con tre `printf` separate: `printf` usa un buffer da 64 byte, e il testo completo è lungo proprio 64 caratteri.

**Casi limite.**

| Riga digitata | Risultato |
|---|---|
| `help` | testo di help |
| `help` seguito da uno spazio | `error: unknown command` |
| uno spazio seguito da `help` | `error: unknown command` |
| `helpx`, `HELP` | `error: unknown command` |
| riga vuota | `error: unknown command` |
| `ls x` | `error: unknown command` |
| `run`, oppure `run` seguito solo da uno spazio | `error: unknown command` |
| `run x` | SVC 13 con il nome `x` |
| `run` seguito da due spazi e `x` | SVC 13 con il nome ` x` (con lo spazio iniziale) |

## 7. Percorso di una system call

Esempio con `ls`, che chiama `sys_image_count()`:

```
src/shell.c          sys_image_count()           chiamata normale di funzione
lib/syscalls.s       svc #11                     lo stub esegue la svc
kernel/svc_entry.s   SVC_Handler                 entra nel kernel, trova il frame
kernel/syscall.c     svc_dispatch → table[11]    legge il numero dall'istruzione
kernel/syscall.c     sys_image_count_impl()      esegue il servizio
                     frame[0] = risultato        torna alla shell in r0
```

1. `src/shell.c` chiama `sys_image_count()` come una normale funzione C. Il prototipo è in `lib/syscalls.h`, che contiene solo dichiarazioni e i numeri dei servizi (`SYS_IMAGE_COUNT` = 11, ...) ed è condiviso da shell e kernel.
2. Il corpo della funzione è uno stub in `lib/syscalls.s`, che esegue `svc #11`. Il numero è scritto dentro l'istruzione, quindi il file assembly usa numeri scritti a mano, che devono coincidere con quelli di `syscalls.h`. Gli argomenti viaggiano nei registri `r0`–`r2`, secondo la normale convenzione di chiamata del C.
3. `svc` genera un'eccezione. L'hardware salva sullo stack della shell otto registri (`r0`–`r3`, `r12`, `lr`, `pc`, `xPSR`): è il *frame*. La CPU passa in modalità privilegiata ed esegue `SVC_Handler` (`kernel/svc_entry.s`), che trova il frame e chiama `svc_dispatch`.
4. `svc_dispatch` (`kernel/syscall.c`) ricava il numero del servizio dall'istruzione `svc` stessa: è il byte basso dell'istruzione, che si trova 2 byte prima del `pc` salvato nel frame.
5. Se il numero è fuori dalla tabella o la sua voce è vuota, scrive `-1` nell'`r0` salvato. Altrimenti chiama `table[n]` con `r0`, `r1` e `r2` del chiamante e scrive il risultato nell'`r0` salvato.
6. All'uscita dall'eccezione l'hardware ripristina i registri dal frame. La shell riprende dopo la `svc` e trova il risultato in `r0`, cioè come valore di ritorno di `sys_image_count()`.

**La tabella dei servizi.** In `syscall.c`, `table[]` associa a ogni numero la funzione che lo implementa:

```c
[SYS_PROGRAM_WAIT] = sys_program_wait_impl,
[SYS_IMAGE_COUNT]  = sys_image_count_impl,
[SYS_IMAGE_NAME]   = sys_image_name_impl,
[SYS_PROGRAM_RUN]  = sys_program_run_impl,
```

La forma `[SYS_IMAGE_COUNT] = ...` mette ogni funzione nella posizione del suo numero, indipendentemente dall'ordine delle righe. `TABLE_SIZE` è calcolata come `sizeof(table) / sizeof(table[0])`: con la voce 14 presente, la tabella copre i numeri da 0 a 14. I numeri senza voce restituiscono `-1`: lo 0, il 5 chiamato da un task e tutti i numeri da 15 in su.

**Firma dei servizi.** Ogni funzione `_impl` ha la stessa firma: tre argomenti `uint32_t` (i registri `r0`–`r2` del chiamante) e un risultato `uint32_t` (il nuovo `r0`). Un puntatore passato dalla shell arriva come numero, e il servizio lo riconverte con un cast. `-1` viaggia come `0xFFFFFFFF` e torna `-1` nell'`int` del chiamante.

## 8. `ls` e `run` lato shell

**`cmd_ls()`.**

1. Chiama la SVC 11 (`sys_image_count()`), che restituisce il numero di programmi nell'immagine, oppure `-1` se l'immagine non è valida.
2. Se il risultato è `-1`, stampa `error: no valid image` e termina.
3. Stampa il numero di programmi. È una riga temporanea di debug (vedi sezione 10).
4. Per ogni indice da 0 a n−1 chiama la SVC 12 (`sys_image_name()`), che copia il nome in `name`. Se la chiamata restituisce 0, stampa il nome su una riga.

`name` è di 16 byte perché un nome ha al massimo 15 caratteri più `'\0'`. Sta sullo stack della shell, cioè nella sua partizione, ed è l'unica destinazione che la SVC 12 accetta: la FLASH non va bene, e `capacity` deve essere tra 1 e 16. Secondo la specifica, `ls` stampa solo i nomi, nell'ordine della directory.

**`cmd_run(name)`.** Chiama la SVC 13 (`sys_program_run()`) e traduce il risultato:

| Risultato della SVC 13 | Significato | La shell stampa |
|---|---|---|
| id ≥ 0 | programma caricato e avviato | niente; poi SVC 14 |
| `-2` | programma non trovato | `error: program not found` |
| `-3` | nessuno slot libero | `error: no free slot` |
| altro negativo (`-1`, `-4`) | nome o immagine non validi, file programma non valido | `error: program cannot be loaded` |

Due conseguenze delle regole:

- dopo `run ` il nome può arrivare a 27 caratteri, ma la SVC 13 ne accetta al massimo 15. Con un nome più lungo restituisce `-1`, e la shell stampa `error: program cannot be loaded`;
- con due spazi dopo `run` il nome inizia con uno spazio e non può esistere: la SVC 13 restituisce `-2`, e la shell stampa `error: program not found`.

**Attesa con la SVC 14.** Se il caricamento riesce, il kernel stampa la riga `loader: ...` (da `task_program_create`) e la shell chiama `sys_program_wait(id)`. Il kernel mette la shell in attesa (`TASK_WAITING`) ed esegue il programma. Quando il programma termina, il kernel stampa `program <nome> exited`, oppure la riga del fault, libera lo slot, scrive lo stato di fine nell'`r0` salvato della shell e la rimette pronta. La shell riprende, ignora lo stato e ristampa il prompt. Per questo il prompt non compare mentre il programma gira. Se il programma è già finito quando la shell chiama la SVC 14, il kernel restituisce subito lo stato salvato.

## 9. Lato kernel: le SVC 11–13

`lib/syscalls.h` e `lib/syscalls.s` non sono stati modificati: prototipi e stub delle SVC 11–14 sono già presenti nella base, e la SVC 14 è già implementata (`sys_program_wait_impl`). In `kernel/syscall.c` sono state aggiunte:

- tre funzioni, `sys_image_count_impl`, `sys_image_name_impl` e `sys_program_run_impl`, con la firma comune dei servizi;
- le tre voci corrispondenti in `table[]` (sezione 7).

**Comportamento attuale.** Le tre funzioni sono stub di prova: ignorano gli argomenti, stampano con `kprintf` una riga che conferma che sono state chiamate e restituiscono 0. Nel kernel si usa `kprintf` e mai `printf`: `printf` esegue una `svc`, e una `svc` eseguita dentro un'altra eccezione manda il processore in HardFault (`HFSR=0x40000000`, bit FORCED).

**Contratto da implementare** (dalla specifica):

| SVC | Funzione | Risultato |
|---|---|---|
| 11 | `sys_image_count()` | numero di voci, da 1 a 32; `-1` se l'immagine non è valida |
| 12 | `sys_image_name(index, name, capacity)` | `0` e nome completo con `'\0'`; `-1` se l'immagine o l'indice non sono validi, se `capacity` non è tra 1 e 16, se la destinazione è fuori dalla partizione del chiamante o se il nome non ci sta. In quest'ultimo caso copia comunque la prima parte del nome, terminata da `'\0'` |
| 13 | `sys_program_run(name)` | task id ≥ 0; `-1` per un puntatore o un nome non validi o per un'immagine non valida; `-2` se il programma non c'è; `-3` se nessuno slot è libero; `-4` se il file programma non è valido |
| 14 | `sys_program_wait(id)` | già data: `0` dopo return o exit, `-2` dopo un fault recuperato; `-1` per id 0, per l'id del chiamante o per un id malformato, sconosciuto o vecchio |

**Controllo dei puntatori.** I puntatori che arrivano dalla shell non sono affidabili, e il kernel deve controllarli prima di usarli:

- SVC 12: la destinazione deve stare tutta nella partizione del chiamante;
- SVC 13: il nome deve essere non vuoto e terminare con `'\0'` entro 16 byte (quindi al massimo 15 caratteri), e ogni byte letto deve stare nella partizione del chiamante o in FLASH (`0x00000000..0x003FFFFF`).

Per questi controlli si usa `in_range()`, già presente in `syscall.c` e usata da `sys_write_impl`. È scritta come `len <= end - p`, così il calcolo `p + len` non può andare in overflow.

## 10. Stato attuale e prossimi passi

**Cosa succede oggi con gli stub.**

- `ls`: il kernel stampa `sys_image_count_impl called`. La SVC 11 restituisce 0, quindi la shell stampa `0` e nessun nome: il ciclo non parte e la SVC 12 non viene mai chiamata.
- `run x`: il kernel stampa `sys_program_run_impl called`. La SVC 13 restituisce 0, che la shell interpreta come un id valido, quindi chiama `sys_program_wait(0)`. La SVC 14 rifiuta l'id 0 e restituisce subito `-1`, e il prompt ricompare senza messaggi d'errore.

Con l'immagine segnaposto della base, la specifica richiede invece `error: no valid image` per `ls` e `error: program cannot be loaded` per `run`.

**Prossimi passi.**

1. Negli stub delle SVC 11, 12 e 13: togliere le `kprintf` e restituire `(uint32_t) -1`. Così il comportamento con l'immagine segnaposto è già quello richiesto.
2. Prima della consegna: togliere da `cmd_ls()` la riga che stampa il numero di programmi, perché il test kit confronta l'output byte per byte.
3. Implementare le SVC 11–13 con il loader: lettura e validazione dell'immagine in FLASH (`0x00200000`), ricerca del programma, scelta dello slot, parsing ELF, relocation, MPU e `task_program_create`. Da documentare a parte in `LOADER.md`.
