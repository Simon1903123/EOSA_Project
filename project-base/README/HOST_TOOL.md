# mkimage.py: il packer dell'immagine IMG1

Documentazione del progetto Scratch-OS (loader di programmi) · Gruppo 6 · Versione Big

## 1. Scopo e contesto

`tools/mkimage.py` è lo strumento che gira sul PC, in fase di build, e crea il file `build/fs.img`. Questo file contiene tutti i programmi ELF del progetto, più una directory con i loro nomi. QEMU lo carica in FLASH all'indirizzo `0x00200000` (sola lettura) e il kernel lo legge lì, senza copiarlo in RAM, per trovare i programmi quando l'utente scrive `run <nome>`.

Lo script copre il requisito R2 dell'assignment. Poiché il nostro team sviluppa la versione Big, il tool fa una cosa sola: impacchetta gli ELF **byte per byte, senza modificarli**. Capire l'ELF, controllarlo e rilocarlo è compito del kernel (extra B1). Per questo non esiste una conversione al formato SFT1, che serve solo alla versione Small.

Lo script usa soltanto la libreria standard di Python 3 (`argparse`, `os`, `re`, `struct`, `sys`) ed è stato provato con Python 3.12.

## 2. Utilizzo

Il comando segue lo schema usato dal Makefile della base:

```
python3 tools/mkimage.py --output build/fs.img <cartella>
```

- `--output` (obbligatorio): il file immagine da creare. Le cartelle mancanti nel percorso, come `build/`, vengono create.
- `<cartella>` (obbligatoria, una sola): la cartella che contiene i programmi già compilati.

Lo script prende da solo tutti i file `*.axf` al primo livello della cartella (non entra nelle sottocartelle). Sono i programmi compilati: l'estensione è `.axf`, ma il contenuto è in formato ELF. Ignora tutto il resto: sorgenti `.c`, oggetti `.o`, file `.map`, file con altre estensioni (compreso `.elf`), file nascosti (che iniziano con `.`) e cartelle. Il nome del programma è il nome del file senza `.axf`: `print_plain.axf` diventa `print_plain`.

I file sono presi in **ordine alfabetico**. In questo modo l'immagine è sempre la stessa a parità di ingressi e `ls` elenca i programmi sempre nello stesso ordine. Per cambiare l'ordine bisogna rinominare i file.

Se tutto va bene lo script stampa una riga di questo tipo ed esce con stato 0:

```
IMG1: build/fs.img (1404 bytes, 2 programs)
```

Nel Makefile il tool viene chiamato come `python3 $(IMAGE_TOOL) --output $(IMAGE) $(PROGRAM_DIR)`. Attenzione: `$(PROGRAM_DIR)` deve contenere gli ELF **già compilati**, perché lo script non compila nulla. Se la cartella contiene solo sorgenti, il tool si ferma con un errore. Per questo la regola che crea l'immagine deve dipendere dalla compilazione dei programmi.

## 3. Il formato dell'immagine (IMG1)

Tutti i numeri sono interi senza segno a 32 bit in **little-endian**: il byte meno significativo viene scritto per primo. Nell'immagine ci sono tre parti, una dopo l'altra:

```
byte 0    +----------------------------+
          | HEADER          20 byte    |
byte 20   +----------------------------+
          | DIRECTORY   count x 24     |
          +----------------------------+  <- inizio dei contenuti (multiplo di 4)
          | file ELF n. 1              |
          | (zeri di allineamento)     |
          | file ELF n. 2              |
          | ...                        |
          +----------------------------+  <- image_size
```

### Header (20 byte)

| Byte | Campo | Valore |
| --: | --- | --- |
| 0..3 | `magic` | `49 4d 47 31` (le lettere `IMG1`) |
| 4..7 | `version` | 1 |
| 8..11 | `image_size` | dimensione totale dell'immagine |
| 12..15 | `count` | numero di programmi, da 1 a 32 |
| 16..19 | `directory_offset` | sempre 20 |

Il magic number è un'etichetta fissa all'inizio del file: serve al kernel per riconoscere subito se il file è davvero un'immagine IMG1 e per scartarlo se non lo è.

### Voce di directory (24 byte, una per programma)

| Byte | Campo | Valore |
| --: | --- | --- |
| 0..15 | `name` | nome, poi NUL, poi zeri fino a 16 byte |
| 16..19 | `offset` | dove inizia il file ELF (multiplo di 4) |
| 20..23 | `size` | dimensione esatta del file ELF (almeno 40) |

Un nome ha da 1 a 15 caratteri scelti tra `A-Z`, `a-z`, `0-9`, `_` e `-`.

### Esempio numerico

Supponiamo due programmi, `print_plain` di 1234 byte e `thumb_ptr` di 100 byte, in questo ordine:

| Elemento | Calcolo | Valore |
| --- | --- | --: |
| Fine della directory | 20 + 2 x 24 | 68 |
| `offset` di `print_plain` | prima posizione utile | 68 |
| Fine di `print_plain` | 68 + 1234 | 1302 |
| `offset` di `thumb_ptr` | 1302 arrotondato al multiplo di 4 | 1304 |
| `image_size` | 1304 + 100 | 1404 |

Tra i due file ci sono 2 byte di zeri di allineamento. Dopo l'ultimo file non si aggiunge nessun riempimento, quindi `image_size` coincide con la fine dell'ultimo file. Il primo blocco dell'immagine, letto con `xxd`, è:

```
494d4731 01000000 7c050000 02000000 14000000
 IMG1    versione  1404     2 prog.  dir a 20
```

## 4. Come funziona lo script

Il flusso è una catena di cinque passi. Ogni passo è una funzione con un compito solo:

```
scan_directory -> load_entries -> build_image -> verify_image -> write_output
```

Tutti i controlli avvengono **prima** della scrittura: se anche uno solo fallisce, non viene creato nessun file.

### 4.1 Costanti e funzione di supporto

Tutti i numeri del formato sono definiti all'inizio del file e usati ovunque: `HEADER_SIZE` (20), `ENTRY_SIZE` (24), `MAX_PROGRAMS` (32), `MAX_IMAGE_SIZE` (2 MiB), `MIN_FILE_SIZE` (40) e così via. Ogni regola dell'assignment ha quindi un solo punto da cui dipende. `NAME_RE` è l'espressione regolare del nome valido.

`align_up(value)` arrotonda un numero per eccesso al multiplo di 4, con la formula `(value + 3) // 4 * 4`. Serve a decidere dove inizia ogni file.

### 4.2 `scan_directory`: cerca i programmi

1. Verifica che il percorso sia una cartella.
2. Ne legge il contenuto con `sorted(os.listdir(...))`. L'ordinamento è per codice dei caratteri, quindi non dipende dalla lingua o dal sistema.
3. Scarta i file nascosti, i file senza estensione `.axf` e le cartelle chiamate `qualcosa.axf`.
4. Per gli altri ricava il nome con `os.path.splitext`.

Restituisce la lista `(nome, percorso)`. Se non trova nessun `.axf`, si ferma con un errore che ricorda che la cartella deve contenere i programmi compilati e non i sorgenti `.c`.

### 4.3 `load_entries`: controlla e legge i file

Per ogni coppia `(nome, percorso)`:

1. il nome deve rispettare `NAME_RE`. Si usa `fullmatch`, così un carattere di a capo finale non passa inosservato;
2. il file deve essere leggibile;
3. il file deve avere almeno 40 byte, perché lo richiede la regola della directory IMG1.

Non serve controllare i nomi duplicati, perché i nomi derivano da file diversi della stessa cartella. Restituisce la lista `(nome, byte_del_file)`.

### 4.4 `build_image`: costruisce l'immagine

La costruzione avviene in tre mosse.

**Primo: calcolo del layout, senza scrivere nulla.** La directory è sempre a 20. Il primo file parte alla fine della directory, arrotondata a 4. Ogni file successivo parte alla fine del precedente, arrotondata a 4. `image_size` è la fine dell'ultimo file. Se supera 2 MiB (`0x200000`) la funzione solleva `ValueError`.

**Secondo: un `bytearray` di `image_size` byte tutto a zero.** È una scelta voluta: l'allineamento tra i file e i byte dopo il NUL dei nomi risultano già a zero, senza codice dedicato.

**Terzo: riempimento delle zone**, con `struct.pack_into`:

- l'header con il formato `<4sIIII` (4 byte di magic e quattro interi);
- ogni voce di directory con il formato `<16sII`. Il codice `16s` aggiunge da solo i NUL ai nomi più corti e, poiché i nomi hanno al massimo 15 caratteri, c'è sempre almeno un NUL finale;
- i file ELF, copiati identici con `image[offset:offset+len] = data`.

### 4.5 `verify_image`: autocontrollo

Rilegge l'immagine appena costruita come farebbe il kernel e verifica tutte le regole della sezione 6.1 dell'assignment:

- magic, versione, `image_size` uguale alla lunghezza reale e compreso tra 20 byte e 2 MiB;
- `count` tra 1 e 32, `directory_offset` multiplo di 4 e almeno 20, directory contenuta nell'immagine;
- per ogni voce: nome con NUL, in ASCII, valido e unico; offset multiplo di 4; dimensione almeno 40; file che non invade la directory e non esce dall'immagine;
- nessuna sovrapposizione tra file (ordina gli intervalli e confronta ogni coppia vicina).

Non dovrebbe mai scattare: se succede, c'è un bug nello script. Ha anche un secondo valore, perché è una traccia già pronta dei controlli da riscrivere in C nel kernel.

### 4.6 `write_output`: scrive il file

Crea la cartella di destinazione se manca. Scrive prima su `<output>.tmp` e poi lo rinomina con `os.replace`. Così, se la scrittura si interrompe, non resta mai un `fs.img` a metà.

### 4.7 `main`

Definisce con `argparse` l'opzione `--output` (obbligatoria) e l'argomento `DIRECTORY`. Poi esegue i cinque passi nell'ordine, controlla che i programmi trovati siano al massimo 32, e infine stampa il messaggio di riuscita.

## 5. Gestione degli errori

L'assignment chiede che gli errori passino da `argparse`, con questo formato su stderr e uscita con stato 2:

```
mkimage.py: error: <motivo>
```

Lo script usa `parser.error(...)` per tutti gli errori sugli ingressi, quindi il formato è uniforme. L'unica eccezione è l'errore interno di `verify_image`, che esce con stato 1 e il prefisso `mkimage.py: internal error:`.

| Situazione | Esito |
| --- | --- |
| Manca `--output` o la cartella | errore di `argparse`, stato 2 |
| Cartella inesistente o che è un file | errore |
| Cartella vuota o senza file `.axf` | errore |
| Nome del file non valido (spazi, caratteri vietati, più di 15 caratteri) | errore, con il nome del file |
| File non leggibile | errore |
| File sotto i 40 byte | errore |
| Più di 32 programmi | errore |
| Immagine oltre 2 MiB | errore |
| Impossibile scrivere l'output | errore |

In tutti questi casi non viene creato nessun file di output.

## 6. Scelte di progetto

1. **Nessuna validazione dell'ELF.** Il tool controlla solo la dimensione minima. Il kernel deve validare tutto da solo (B1) e il test kit usa programmi volutamente rotti, come `bad_magic`, che devono arrivare nell'immagine per essere rifiutati dal kernel. Un packer troppo severo li bloccherebbe prima.
2. **Copia byte per byte.** Come richiesto per la versione Big, ogni ELF entra nell'immagine invariato.
3. **Ordine deterministico.** I programmi sono sempre in ordine alfabetico, quindi la build è riproducibile e l'ordine di `ls` è prevedibile.
4. **Una sola fonte per le regole.** Le costanti del formato stanno in un unico posto e sono usate sia dal costruttore sia dall'autocontrollo.
5. **Autocontrollo prima della scrittura.** L'immagine viene verificata prima di finire su disco.
6. **Scrittura sicura.** File temporaneo e rinomina, per non lasciare file a metà.
7. **Nessuna dipendenza esterna.** Si può usare su qualunque PC con Python 3.
8. **Un solo modo d'uso.** Lo script accetta soltanto la forma `--output OUTPUT DIRECTORY` usata dal Makefile, per tenere il codice semplice.

## 7. Limiti noti

- La ricerca dei programmi **non è ricorsiva**: cerca i `.axf` solo nel primo livello della cartella. Ogni `.axf` trovato viene impacchettato, quindi la cartella non deve contenere altri eseguibili, come il kernel (`scratch-os.axf`).
- Il tool **non compila**: la cartella deve già contenere gli ELF. La compilazione spetta alle regole del Makefile.
- Il tool **non controlla il contenuto dell'ELF** (vedi la scelta di progetto 1).
- Si può indicare **una sola cartella**.
- L'ordine dei programmi è alfabetico e non si può cambiare dalla riga di comando: bisogna rinominare i file.
- Se la scrittura viene interrotta in modo brusco può restare un file `<output>.tmp`, che si può cancellare.
- L'assignment (sezione 6.3) indica anche il comando `python3 tools/mkimage.py image build/fs.img NAME=PROGRAM.elf ...`. Questa versione non lo supporta, perché il Makefile usa la forma con `--output`.

## 8. Verifiche eseguite

Lo script è stato provato con file ELF finti con estensione `.axf` (contenuto arbitrario, dimensioni diverse):

- l'immagine prodotta è stata riletta con un lettore scritto a parte: header, directory e contenuto dei file risultano corretti, con i file identici agli originali;
- le posizioni coincidono con i calcoli manuali dell'esempio della sezione 3, e il riempimento è fatto di zeri;
- limiti esatti: 32 programmi sì e 33 no; nome di 15 caratteri sì e di 16 no; file di 40 byte sì e di 39 no; immagine di 2 097 152 byte sì e di 2 097 153 no;
- file nascosti, sorgenti `.c`, file `.map`, file `.elf` e cartelle chiamate `x.axf` vengono ignorati;
- cartella inesistente, vuota o con soli sorgenti: errore con stato 2, senza creare file;
- un file con magic sbagliato viene accettato, come previsto dalla scelta di progetto 1;
- nessun file `.tmp` resta dopo una scrittura riuscita.

Comandi utili per un controllo manuale:

```
python3 tools/mkimage.py --output build/fs.img build/programs/
xxd -l 64 build/fs.img
ls -l build/fs.img
```

## 9. Da completare prima della consegna

- [ ] Provare lo script con gli ELF reali dei programmi e controllare l'immagine con `xxd`.
- [ ] Controllare nel test kit se `run_tests.py` o i generatori di file rotti chiamano `mkimage.py` (ed eventualmente con quale comando).
- [ ] Verificare che il kernel accetti l'immagine prodotta: `ls` deve elencare i programmi in ordine alfabetico.
- [ ] Verificare che la regola del Makefile che crea `build/fs.img` dipenda dai programmi `.axf` compilati e che la cartella indicata non contenga altri `.axf` (come il kernel).
- [ ] Aggiornare questa documentazione se cambia qualcosa nello script.
