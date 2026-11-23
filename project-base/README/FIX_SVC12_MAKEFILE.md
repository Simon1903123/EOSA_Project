# Branch `fix-svc12-makefile`

Questo branch corregge due bug trovati nella review di `main` (`beb56c7`) e
mette in ordine il target di default del Makefile.

| Commit | Cosa cambia | File |
|---|---|---|
| `eee6773` | SVC 12: un nome che riempie esattamente il buffer non veniva copiato | `kernel/syscall.c` |
| `162057f` | `make run`, `make debug` e `make run-trace` partivano con un'immagine finta | `Makefile` |
| `272775f` | `all` era definito due volte; ora è definito una volta sola, in cima | `Makefile` |

Non sono inclusi il bug di SVC 11 (controllo della directory) e quello della
MPU (fault del kernel su un puntatore al codice): li seguono altri membri del
team (vedi in fondo).

---

## 1. SVC 12: off-by-one nella copia del nome

### Cosa dice la traccia (§6.5)
`int sys_image_name(unsigned index, char *name, unsigned capacity)` copia il
nome completo, **con il suo NUL finale**, e restituisce `0`. Restituisce `-1`
se il nome non ci sta. `capacity` può valere da 1 a 16, e un nome ha al
massimo 15 caratteri.

Quindi un nome di 15 caratteri con `capacity = 16` ci sta esattamente: 15
caratteri + 1 NUL = 16 byte.

### Il problema
Il ciclo di copia si fermava un byte prima:

```c
for ( uint32_t i = 0; i < a2 - 1; i++ )   /* a2 = capacity */
```

Con `capacity = 16` il ciclo va da `i = 0` a `i = 14`, cioè copia solo i
primi 15 byte. Per un nome di 15 caratteri quei 15 byte sono tutti lettere: il
NUL sta nel byte 15, che il ciclo non raggiunge mai. La funzione allora pensa
che il nome non sia finito, quindi che non ci stia, e restituisce `-1`.

Esempio con il nome `abcdefghijklmno` (15 caratteri):

| Byte | 0 … 14 | 15 |
|---|---|---|
| Contenuto | `a` … `o` | `\0` |
| Vecchio ciclo (`i < 15`) | copiati | **mai letto** → `-1` |
| Nuovo ciclo (`i < 16`) | copiati | copiato → `0` |

### L'effetto visibile
La shell usa SVC 12 per `ls`. Se SVC 12 restituisce `-1`, `ls` salta il
programma senza dire niente. Un programma con un nome di 15 caratteri quindi:
- **non compariva** in `ls`;
- però partiva lo stesso con `run abcdefghijklmno`, perché SVC 13 cerca il
  nome per conto suo.

I programmi del test kit hanno nomi più corti, per questo i test non se ne
accorgevano.

### Il fix
Il ciclo arriva fino all'ultimo byte del buffer:

```c
for ( uint32_t i = 0; i < a2; i++ )
```

Il resto della funzione non cambia: se il NUL non arriva entro `capacity`
byte, il nome non ci sta e il caso viene gestito come prima.

---

## 2. Makefile: `make run` usava un'immagine finta

### Il problema
Dopo `make clean && make run`, al prompt della shell `ls` rispondeva
`error: no valid image`.

Nel Makefile c'erano due regole che scrivevano lo stesso file
`build/fs.img`:

- **`$(IMAGE)`**, rimasta dalla base del prof: scriveva nel file solo il testo
  `SCRATCH-OS-PROJECT-IMAGE`. Era un segnaposto, che serviva prima che
  esistesse `mkimage.py`.
- **`image`**: la nostra regola, che compila i programmi e crea l'immagine
  vera con `tools/mkimage.py`.

`run` dipendeva dalla prima:

```make
run: $(BIN) $(IMAGE)
```

Dopo `make clean` il file `build/fs.img` non esiste più. Quindi `make` usava
la regola `$(IMAGE)`, scriveva il segnaposto e avviava QEMU. Il kernel leggeva
`SCRATCH-OS-PROJECT-IMAGE` invece della firma `IMG1` e dichiarava l'immagine
non valida.

Prima non ce ne eravamo accorti perché, se avevi appena lanciato `make test`
o `make image`, il file c'era già (quello vero) e `make` non lo riscriveva.

Lo stesso problema c'era anche in `debug` e in `run-trace`.

### Il fix
- Cancellata la regola segnaposto `$(IMAGE)` e il commento che ne parlava.
- `run`, `debug` e `run-trace` ora dipendono da `image`:

```make
run: $(BIN) image
	$(QEMU)

debug: $(BIN) image
	$(QEMU) -S -gdb tcp::1234

run-trace: $(BIN) image
	$(QEMU) -d unimp
```

La variabile `IMAGE := $(BUILD_DIR)/fs.img` resta: la usano la ricetta di
`image` e la riga di QEMU, che carica quel file in FLASH all'indirizzo
`0x00200000`.

Siccome `image` è un target `.PHONY`, `make run` ricrea l'immagine ogni volta.
Ci vuole un secondo in più, ma non si parte mai con un'immagine vecchia o
finta.

---

## 3. Makefile: il target di default `all`

### Il problema
`all` era scritto due volte:

```make
all: $(BIN)      # in alto
...
all: image       # in fondo
```

Funzionava lo stesso: quando una regola non ha ricetta, `make` **somma** le
dipendenze delle righe con lo stesso nome, quindi `all` dipendeva sia da
`$(BIN)` sia da `image`. Però chi legge il Makefile vede in alto
`all: $(BIN)` e pensa che `make` compili solo il kernel.

### Il fix
`all` ora è definito una volta sola, subito dopo `.PHONY`:

```make
# Default goal: build the kernel, every program and the real fs.img.
all: image
```

La posizione conta. Lanciato senza argomenti, `make` costruisce **il primo
target del file** (le regole con `%` non contano). Se `all` stesse in fondo,
il primo target diventerebbe `$(BIN)` e `make` compilerebbe solo il kernel.
È proprio quello che è successo in una prova intermedia.

---

## Come sono collegati i target del Makefile

| Comando | Kernel | 9 programmi | `build/fs.img` | Avvia QEMU |
|---|:---:|:---:|:---:|:---:|
| `make` / `make all` | ✅ | ✅ | ✅ | |
| `make image` | ✅ | ✅ | ✅ | |
| `make programs` | | ✅ | | |
| `make run` / `debug` / `run-trace` | ✅ | ✅ | ✅ | ✅ |
| `make test` | ✅ | ✅ | usa l'immagine del test kit | ✅ (per ogni test) |

- **`programs`** compila solo i 9 `.elf` in `build/programs/`, uno per voce di
  `PROG_MAP`. Lo usano `image` e i test, quindi va tenuto.
- **`make test`** non usa `build/fs.img`: il test kit costruisce un'immagine
  sua in `build/test-kit/<suite>/fs.img`, con i nostri `.elf` più i file
  rovinati che aggiunge lui. Per questo i test passavano anche quando
  `make run` partiva con l'immagine finta.

---

## Verifica

```bash
make clean && make && ls build/fs.img build/programs/
```
Devono esserci `fs.img` e i 9 file `.elf`.

```bash
make clean && make run
```
Al prompt `ls` deve elencare i 9 programmi (non `error: no valid image`).
Uscita da QEMU: Ctrl+A, poi X.

```bash
make test
```
Risultato atteso: `small passed=10 failed=0`, `big passed=14 failed=0`.

Nota: `make` e `make test` rigenerano `build/fs.img` e `build/scratch-os.axf`,
che sono tracciati nel repository. Prima di ogni `git add` si rimettono come
sul branch con:

```bash
git restore build/fs.img build/scratch-os.axf
```

Si rigenerano e si committano una volta sola, alla fine, per la consegna.

---
