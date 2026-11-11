# Programmi caricati e immagine IMG1 — appunti

## 1. Cosa sono i programmi caricati

La shell e il kernel sono compilati insieme in `build/scratch-os.axf`. I programmi che si lanciano con `run <nome>` no: ognuno è compilato a parte, in un proprio file ELF, e non condivide codice con il kernel. Ogni programma contiene tutto quello che gli serve: il codice di partenza, gli stub delle system call e `printf`.

Quando un programma viene compilato, nessuno sa ancora in quale zona della RAM verrà caricato: dipende da quale slot è libero in quel momento. Per questo ogni programma è linkato come se partisse dall'indirizzo `0`. Il loader lo copia poi in uno slot libero (per esempio `0x20002000`) e aggiunge l'indirizzo dello slot a tutti i valori che contengono un indirizzo completo. Questa correzione si chiama **rilocazione**, e l'elenco dei punti da correggere lo lascia il linker dentro l'ELF.

I programmi non stanno in file separati sul "disco": sono messi tutti insieme in un unico file, `build/fs.img`, che QEMU carica in FLASH all'indirizzo `0x00200000`. È l'immagine IMG1, che il kernel legge per `ls` e `run`.

## 2. Dal sorgente all'esecuzione

1. `make programs` compila ogni programma di `programs/` in `build/programs/NOME.elf`, linkato all'indirizzo `0` e con l'elenco delle rilocazioni.
2. `make image` lancia `tools/mkimage.py`, che mette tutti gli ELF in `build/fs.img`, con un indice dei nomi all'inizio.
3. `make run` avvia QEMU, che carica il kernel e copia `fs.img` in FLASH a `0x00200000`.
4. `ls` (SVC 11 e 12) legge l'indice dell'immagine e stampa i nomi.
5. `run <nome>` (SVC 13) cerca il nome nell'indice e passa l'ELF trovato al loader. Il loader lo copia in uno slot, applica le rilocazioni, imposta la MPU e crea il task.
6. Il task parte da `_start`, che chiama `main()`. Quando `main()` termina, `_start` chiama `sys_exit()` e il kernel libera lo slot.

I punti 1–3 sono descritti in questo file. I punti 4–6 sono del kernel (shell, system call e loader).

## 3. I file

| File | Cosa contiene |
|---|---|
| `programs/*.c` | i 9 programmi del test kit (`small/programs` e `big/programs`), copiati senza modifiche |
| `programs/user_start.s` | il codice di partenza `_start` (scritto da Giulio) |
| `programs/prog.ld` | il linker script dei programmi |
| `programs/lib/` | la libreria dei programmi: `printf`, `format`, stub delle system call (`syscalls.s`, `syscalls.h`) |
| `tools/mkimage.py` | il packer che crea `build/fs.img` |
| `Makefile` | le regole `programs` e `image` |

**Sorgenti del test kit.** I 9 file `.c` sono quelli del prof, byte per byte. Il test confronta l'output carattere per carattere, quindi non vanno toccati.

**Libreria.** `programs/lib/` è una copia di `lib/` della base. È una copia, e non lo stesso file del kernel, perché un programma non può usare codice del kernel: deve contenere la sua versione. Unica differenza: da `printf.c` sono state tolte `sleep_ms` e `spin_ticks`, che nessun programma usa (sezione 6).

## 4. Come è fatto un programma in memoria

**Il linker script (`prog.ld`).**

```
ENTRY(_start)
SECTIONS
{
  . = 0;
  .text : { *(.text._start) *(.text*) *(.rodata*) . = ALIGN(4); }
  .data : { *(.data*) . = ALIGN(4); }
  .bss  : { *(.bss*) *(COMMON) . = ALIGN(4); }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}
```

- `ENTRY(_start)`: il punto di partenza del programma è `_start`.
- `. = 0`: tutto parte dall'indirizzo `0`.
- `.text`: il codice e le costanti (`.rodata`), cioè la parte che si può solo leggere ed eseguire.
- `.data`: le variabili globali con un valore iniziale.
- `.bss`: le variabili globali senza valore iniziale. Non occupano spazio nel file: il loader le azzera.
- `ALIGN(4)`: ogni parte finisce a un multiplo di 4 byte, come chiede la specifica.
- `/DISCARD/`: elimina le tabelle per le eccezioni del C++, che non servono.

**Layout di un programma.** Esempio con `wx_write_code`:

| Offset | Contenuto | Da dove viene |
|---|---|---|
| `0x000` | `_start` | `user_start.s` |
| `0x00a` | stub delle system call (`sys_write`, `sys_exit`, …) | `lib/syscalls.s` |
| `0x048` | `printf` | `lib/printf.c` |
| `0x0e4` | `vformat`, `utoa_…` | `lib/format.c` |
| `0x280` | `main` | `wx_write_code.c` |

L'ordine è quello in cui i file sono passati al linker: prima il runtime, poi il programma.

**Lo startup (`_start`).**

```asm
_start:
    bl main        @ esegue il programma
    bl sys_exit    @ se main fa return, chiude il task (SVC 4)
hang:
    b hang         @ non dovrebbe mai arrivarci
```

La specifica chiede che tornare da `main` valga come chiamare `sys_exit()`. `.thumb_func` fa sì che l'indirizzo di `_start` abbia il bit 0 a 1: il Cortex-M3 esegue solo codice Thumb, e un salto a un indirizzo con il bit 0 a 0 causa un fault. Per questo l'entry point dell'ELF è `0x1` e non `0x0`.

## 5. `make programs`

**Le variabili.**

| Variabile | Valore | A cosa serve |
|---|---|---|
| `PROG_DIR` | `programs` | dove stanno i sorgenti |
| `PROG_OUT` | `build/programs` | dove finiscono gli ELF; `make clean` li cancella |
| `PROG_MAP` | coppie `NOME:SORGENTE` | associa a ogni sorgente il nome usato da `ls` e `run` |
| `PROG_RT` | `user_start.s`, `lib/syscalls.s`, `lib/printf.c`, `lib/format.c` | il runtime, linkato in ogni programma |
| `PROG_CFLAGS` | vedi sotto | opzioni del compilatore |
| `PROG_LDFLAGS` | `-nostdlib -static -T programs/prog.ld -Wl,--emit-relocs` | opzioni del linker |
| `PROG_ELF` | `build/programs/NOME.elf`, uno per voce di `PROG_MAP` | la lista dei file da costruire |

**`PROG_MAP`.** Il nome che usa la shell non è sempre quello del file, e il test lancia i programmi con i nomi:

| Nome per `ls` e `run` | Sorgente |
|---|---|
| `print_plain` | `print_plain.c` |
| `global_state` | `globals_bss_const.c` |
| `thumb_ptr` | `thumb_function_pointer.c` |
| `nonzero_slot` | `nonzero_slot_relocation.c` |
| `normal_return` | `normal_return.c` |
| `explicit_exit` | `explicit_exit.c` |
| `kernel_store` | `kernel_store.c` |
| `wx_write_code` | `wx_write_code.c` |
| `wx_exec_data` | `wx_exec_data.c` |

Per aggiungere un programma basta aggiungere una coppia `NOME:SORGENTE` a `PROG_MAP`.

**Opzioni del compilatore.**

| Opzione | Perché |
|---|---|
| `-mcpu=cortex-m3 -mthumb` | codice Thumb per il Cortex-M3 |
| `-ffreestanding -nostdlib` | niente libreria C: `printf` e gli altri sono i nostri |
| `-fno-builtin` | GCC non sostituisce `printf` con la sua versione |
| `-fno-pic` | niente codice indipendente dalla posizione: il loader gestisce le rilocazioni, non le tabelle che userebbe il codice PIC |
| `-O2` | ottimizzazione: codice più piccolo; cambia anche l'indirizzo di `main` (sezione 6) |
| `-g` | informazioni di debug, per GDB; non vengono caricate in RAM |
| `-Iprograms` | `#include "lib/printf.h"` cerca dentro `programs/`, non nel kernel |

**`--emit-relocs`.** Di solito il linker, finito il suo lavoro, butta via l'elenco delle rilocazioni. Con questa opzione lo lascia nell'ELF, nella sezione `.rel.text`: è l'elenco che il loader usa per correggere gli indirizzi.

**Una regola per ogni programma.** Invece di scrivere nove regole quasi uguali, il `Makefile` ne scrive una sola come modello e la ripete per ogni voce di `PROG_MAP`:

```make
define PROG_RULE
$(PROG_OUT)/$(1).elf: $(PROG_DIR)/$(2) $(PROG_RT) $(PROG_DIR)/prog.ld
	@mkdir -p $(PROG_OUT)
	arm-none-eabi-gcc $(PROG_CFLAGS) -o $$@ $(PROG_RT) $(PROG_DIR)/$(2) $(PROG_LDFLAGS) -lgcc
endef

$(foreach m,$(PROG_MAP),$(eval $(call PROG_RULE,$(word 1,$(subst :, ,$(m))),$(word 2,$(subst :, ,$(m))))))
```

- `PROG_RULE` è il modello: `$(1)` è il nome, `$(2)` il sorgente.
- `$$@` è il file da costruire. Il doppio `$` rimanda l'espansione al momento in cui la ricetta viene eseguita.
- `foreach` scorre `PROG_MAP`; `subst` e `word` separano nome e sorgente; `call` riempie il modello; `eval` lo trasforma in una regola vera.

La riga `programs: $(PROG_ELF)` sta dopo la definizione di `PROG_ELF`: `make` espande le dipendenze quando legge la riga, e se la variabile non esistesse ancora la regola resterebbe vuota.

## 6. Dove finisce `main` e il test `wx_write_code`

Il programma `wx_write_code` prova a scrivere sopra `main`, cioè nel proprio codice. La MPU lo blocca, e il kernel stampa l'indirizzo che ha provato a scrivere:

```
MemManage in loaded program: CFSR=0x82 address=<CODE_ADDRESS>
```

Quell'indirizzo dipende da dove il linker mette `main`, cioè dall'ordine dei file e dalla dimensione del runtime. Per questo il test kit aggiornato non chiede un valore preciso: accetta qualunque indirizzo pari di 8 cifre esadecimali nello slot 0, tra `0x20002000` e `0x20002fff`. La prima versione del test kit chiedeva esattamente `0x20002280`.

Nel nostro build `main` è all'offset `0x280`, quindi il kernel stamperà `0x20002280`, che va bene con entrambe le versioni. Ci si arriva con due scelte, fatte quando il test chiedeva ancora il valore esatto:

1. **Il runtime prima del programma.** Con il sorgente del programma come primo file passato al linker, `main` finirebbe a `0x0`.
2. **Niente `sleep_ms` e `spin_ticks`.** La `printf.c` della base le contiene. Il linker include tutto un file oggetto, anche le funzioni non usate: le due funzioni occupavano 52 byte (`0x34`) prima di `main`, che finiva a `0x2b4`. Nessun programma del test le usa, quindi sono state tolte dalla copia in `programs/lib/`.

Con il test kit aggiornato nessuna delle due è più obbligatoria, ma non danno problemi e rendono il programma un po' più piccolo.

## 7. Cosa trova il loader nell'ELF

| Cosa | Valore |
|---|---|
| tipo | `ET_EXEC`, ARM, 32 bit little-endian |
| entry point | `0x1`: `_start` all'offset 0, bit 0 a 1 (Thumb) |
| segmenti `PT_LOAD` | uno solo, che parte da `0` e contiene `.text` e poi `.data` |
| sezioni caricate | `.text` (codice e costanti), `.data`, `.bss` |
| rilocazioni | in `.rel.text`, formato `REL` (niente `RELA`) |
| tipi di rilocazione | `R_ARM_ABS32`, `R_ARM_THM_CALL`, `R_ARM_THM_JUMP24` |
| dimensione in RAM | da 700 a 820 byte per programma (il limite è 3072) |

**I tre tipi di rilocazione.**

- `R_ARM_ABS32`: una parola che contiene un indirizzo completo, per esempio l'indirizzo di una stringa usata da `printf`. Il loader le aggiunge l'indirizzo dello slot.
- `R_ARM_THM_CALL` e `R_ARM_THM_JUMP24`: chiamate e salti Thumb (`bl`, `b.w`). Sono relativi alla posizione dell'istruzione, quindi restano giusti in qualunque slot: il loader li controlla ma non li cambia.

Ci sono anche rilocazioni nelle sezioni di debug (`.rel.debug_*`). Riguardano sezioni che non vengono caricate in RAM, quindi il loader le ignora.

**Un solo segmento.** Il segmento `PT_LOAD` contiene sia il codice sia i dati. Per la protezione W^X (codice leggibile ed eseguibile, dati leggibili e scrivibili) il loader deve quindi separarli usando le sezioni: `.text` da una parte, `.data` e `.bss` dall'altra. Se al loader servono due segmenti separati, si può cambiare `prog.ld`.

Con `-O2` il compilatore elimina le variabili globali che non vengono mai modificate. Per questo quasi tutti i programmi hanno `.data` e `.bss` vuote: solo `wx_exec_data` ha 4 byte di dati.

## 8. `make image` e `tools/mkimage.py`

**Il comando** è esattamente quello della specifica (6.3):

```
python3 tools/mkimage.py image build/fs.img NOME=PROGRAMMA.elf [NOME=PROGRAMMA.elf ...]
```

`make image` lo costruisce da `PROG_MAP`, nello stesso ordine:

```
print_plain=build/programs/print_plain.elf global_state=build/programs/global_state.elf ...
```

L'ordine conta: è l'ordine della directory, e quindi l'ordine in cui `ls` stampa i nomi.

**Il formato IMG1.**

| Parte | Dimensione | Contenuto |
|---|---|---|
| header | 20 byte | `IMG1`, versione 1, dimensione totale, numero di programmi, dove inizia la directory (20) |
| directory | 24 byte per programma | nome (16 byte, con `'\0'`), offset del file, dimensione del file |
| file | variabile | gli ELF, uno dopo l'altro, ognuno a un offset multiplo di 4 |

Tutti i numeri sono a 32 bit, little-endian. Gli ELF sono copiati byte per byte, senza modifiche: il packer non controlla che siano validi, perché è compito del kernel. Il test kit mette apposta nell'immagine anche file rovinati, per verificare che il kernel li rifiuti.

**Cosa fa lo script.**

1. Legge le coppie `NOME=PERCORSO` e controlla i nomi: da 1 a 15 caratteri tra `A-Z a-z 0-9 _ -`, senza doppioni.
2. Legge i file: ognuno deve avere almeno 40 byte, e i programmi devono essere al massimo 32.
3. Calcola dove va ogni file e controlla che l'immagine non superi 2 MiB.
4. Costruisce l'immagine e la rilegge con le stesse regole che userà il kernel.
5. Solo alla fine la scrive, prima in un file temporaneo e poi rinominandolo.

Se qualcosa non va non scrive niente, stampa `mkimage.py: error: <motivo>` ed esce con stato 2. Se va tutto bene stampa, per esempio:

```
IMG1: build/fs.img (714436 bytes, 9 programs)
```

**I primi 20 byte di `fs.img`.**

```
49 4d 47 31   "IMG1"
01 00 00 00   versione 1
c4 e6 0a 00   dimensione totale (qui 714436; dipende dal compilatore)
09 00 00 00   9 programmi
14 00 00 00   la directory inizia a 20
```

## 9. Come provarlo

```bash
cd project-base
make programs                                                     # 9 file in build/programs/
arm-none-eabi-nm -u build/programs/*.elf                          # vuoto: nessun simbolo mancante
arm-none-eabi-size build/programs/*.elf                           # text + data + bss sotto 3072
arm-none-eabi-nm build/programs/wx_write_code.elf | grep " main$"  # 00000280 T main (dentro lo slot)
make image                                                        # IMG1: build/fs.img (... 9 programs)
xxd -l 20 build/fs.img                                            # inizia con 49 4d 47 31
```

**I test pubblici non usano `build/fs.img`.** Il test kit costruisce una sua immagine a partire dai nostri ELF, aggiungendo i file rovinati:

```bash
python3 TEST_KIT/big/generate_fixtures.py \
  --output-dir build/test-kit/big/files --image build/test-kit/big/fs.img \
  print_plain=build/programs/print_plain.elf global_state=build/programs/global_state.elf \
  thumb_ptr=build/programs/thumb_ptr.elf nonzero_slot=build/programs/nonzero_slot.elf \
  normal_return=build/programs/normal_return.elf explicit_exit=build/programs/explicit_exit.elf \
  kernel_store=build/programs/kernel_store.elf wx_write_code=build/programs/wx_write_code.elf \
  wx_exec_data=build/programs/wx_exec_data.elf

python3 TEST_KIT/run_tests.py --suite big \
  --kernel build/scratch-os.axf --image build/test-kit/big/fs.img
```

`TEST_KIT` è la cartella `test-kit` del materiale del corso, nella versione aggiornata. Il generatore accetta i nostri ELF senza errori. `build/fs.img` serve invece per `make run` e per la demo.

## 10. Stato attuale e prossimi passi

**Fatto.** `make programs` e `make image` funzionano. Gli ELF rispettano la specifica: tipo `ET_EXEC`, indirizzo `0`, entry Thumb, solo rilocazioni `REL` dei tre tipi ammessi, nessun simbolo mancante. `main` di `wx_write_code` è a `0x280`, dentro l'intervallo accettato dal test.

**Prossimi passi.**

1. Aggiungere un a capo alla fine di `user_start.s`: senza, l'assembler stampa un warning (non cambia il codice).
2. Aggiungere un target `make test` che lanci il generatore e `run_tests.py` con un solo comando.
3. Quando shell, SVC 11–13 e loader saranno pronti, lanciare i test con il test kit aggiornato.
