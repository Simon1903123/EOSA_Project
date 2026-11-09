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

**Puntatore o intero.** Tutti i simboli sono indirizzi. Il kernel li vuole però di due tipi diversi:

- come puntatore (`uint32_t *`) quando deve leggere o scrivere in memoria a quell'indirizzo;
- come intero (`uint32_t`) quando deve solo fare calcoli. Per passarli serve un cast: il valore resta uguale, cambia solo il tipo.

Scrivere `*simbolo` non dà l'indirizzo: legge il contenuto della memoria a quell'indirizzo.

| Parametro | Cosa si passa | Perché |
|---|---|---|
| `name` | `"shell"` | nome del task |
| `entry` | `shell_main` | indirizzo della funzione di partenza. Scrivendo `shell_main()` la funzione verrebbe eseguita subito, dentro il kernel |
| `stack_top` | `_shell_stack_top` | il kernel prepara lo stack iniziale a partire da qui |
| `part_start`, `part_end` | `(uint32_t) _shell_part_start`, `(uint32_t) _shell_part_end` | servono per configurare la MPU e per controllare i puntatori passati alle system call |
| `data_start`, `data_end`, `data_load` | i simboli, senza cast | il kernel copia i valori iniziali da `data_load` (FLASH) a `data_start..data_end` (RAM) |

## 5. Tappa 2 — prompt, lettura ed eco

**`read_char()`.** `sys_read()` non aspetta: restituisce il carattere ricevuto, oppure `-1` se non è arrivato niente. Per aspettare un tasto si ripete la lettura in un ciclo. Quando arriva `-1` si chiama `task_delay(1)`: la shell si sospende per 10 ms e nel frattempo gira l'idle task. Con `task_yield()` lo scheduler tornerebbe subito alla shell, e la CPU resterebbe sempre occupata.

**`read_line()`.** Legge un carattere alla volta:

- se è `'\r'` o `'\n'`: va a capo, chiude la stringa con `'\0'` e termina;
- se la riga ha meno di 31 caratteri: salva il carattere e lo ristampa (eco);
- altrimenti: ignora il carattere, senza stampare niente.

Il buffer è di 32 byte: 31 caratteri più `'\0'`. Non si stampano messaggi extra, perché i test confrontano l'output carattere per carattere.

Per l'eco si usa `printf("%c", ch)`: se la UART non accetta subito tutti i byte, `printf` riprova. `sys_write` invece si ferma.

**`shell_main()`.** Ripete all'infinito: stampa `scratch> `, legge una riga, esegue il comando (tappa 3). Non deve mai terminare: se terminasse, il kernel chiuderebbe il task della shell per sempre.

## 6. Errori da non ripetere

- `#define` usato al posto di `#include`.
- Prototipo della funzione scritto al posto della chiamata.
- `*simbolo` usato per ottenere un indirizzo.
- Cast `(uint32_t *)` dove serviva `(uint32_t)`.
- Scrittura di `buf[32]` in un array da 32 elementi: gli indici validi vanno da 0 a 31.
- Buffer dichiarato dentro `read_char()`: una variabile locale si perde quando la funzione termina.

## 7. Prossimo passo — Tappa 3

Riconoscere i comandi `help`, `ls` e `run <nome>`. Per ogni altro input: `error: unknown command`.