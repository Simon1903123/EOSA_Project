# Relocation

Questo file spiega il lavoro del branch `6\\\_Reloc`: le due funzioni del loader che si occupano delle **relocation**, `elf\\\_check\\\_relocs` ed `elf\\\_apply\\\_relocs`, scritte in `kernel/loader.c`. Le stesse cose sono riassunte in forma grafica nei due PDF alla radice del repository:

* `Loader\\\_SVC13\\\_pipeline.pdf`: la catena della SVC 13, piano per piano, con quello che ogni piano riceve, fa e consegna;
* `thumb\\\_ptr\\\_loader\\\_anatomy.pdf`: il programma `thumb\\\_ptr.elf` seguito byte per byte, dal file allo slot.

Qui gli stessi contenuti sono spiegati per esteso, passo per passo, con il perché di ogni scelta.

## 1\. Cos'è la relocation e perché serve

Ogni programma caricato è compilato **da solo**, separato dal kernel, e il linker lo prepara come se dovesse partire dall'indirizzo `0`. Ma un programma non gira mai all'indirizzo `0`: il loader lo copia nel primo slot libero, che può essere uno di questi quattro:

|Slot|Base|
|-:|-|
|0|`0x20002000`|
|1|`0x20003000`|
|2|`0x20004000`|
|3|`0x20005000`|

Quale slot toccherà a un programma si sa solo al momento del `run`, quindi il linker non può saperlo.

La maggior parte delle istruzioni non si accorge dello spostamento. Un salto come `bl add\\\_seven` non dice "vai all'indirizzo `0x2b4`", ma "vai 12 byte più indietro": se tutto il programma si sposta insieme, la distanza resta giusta.

Alcuni valori però contengono un **indirizzo completo**: un puntatore a funzione, l'indirizzo di una stringa, l'indirizzo di una variabile globale. Il linker li ha calcolati supponendo che il programma parta da `0`, quindi dopo la copia sono sbagliati di esattamente `slot\\\_base` byte. Il loader deve aggiungere `slot\\\_base` a ognuno di essi. Questa correzione è la **relocation**.

Per sapere *dove* sono questi valori, il loader non può indovinare: deve leggerlo nel file. Il linker lascia l'elenco nell'ELF, nelle sezioni di tipo `REL`, perché i programmi sono linkati con l'opzione `--emit-relocs`. Il Cortex-M3 non ha una MMU, cioè non può "fingere" che il programma sia all'indirizzo `0`: l'unico modo di farlo funzionare altrove è correggerlo.

Il mio compito è leggere quell'elenco, controllare che ogni voce sia sensata e, solo dopo, correggere le word nello slot.

## 2\. Cosa chiede la traccia

Sono le regole della versione big che riguardano le relocation (traccia, sezioni 5, 6.3 e 6.4):

* **Quali relocation contano.** Ogni sezione `REL` dice, nel campo `sh\\\_info`, a quale sezione si riferisce. Contano solo quelle che si riferiscono a sezioni **allocated**, cioè copiate nello slot. Quelle del debug si ignorano, purché i loro record stiano dentro il file.
* **Tre tipi ammessi.** Nelle sezioni allocated:

  * `R\\\_ARM\\\_ABS32` (tipo 2) va corretto aggiungendo la base dello slot;
  * `R\\\_ARM\\\_THM\\\_CALL` (tipo 10) e `R\\\_ARM\\\_THM\\\_JUMP24` (tipo 30) sono salti Thumb relativi: si controllano ma non si toccano;
  * ogni altro tipo va rifiutato.
* **Il messaggio per un tipo sbagliato.** Il kernel deve stampare prima `loader: unsupported relocation type <tipo>`, con il tipo in decimale, e poi rifiutare il programma.
* **Controlli prima dell'uso.** Ogni tabella, record, simbolo e site usato deve stare dentro il file e dentro lo slot, e nessun calcolo deve andare in overflow. In particolare `slot\\\_base + word originale` non deve superare i 32 bit.
* **Prima si controlla, poi si scrive (R9).** Un file sbagliato va rifiutato *prima* che un solo byte del programma venga copiato o eseguito, e nessuno slot deve cambiare.
* **Il bit Thumb.** Aggiungere la base a un puntatore a funzione deve lasciare invariato il bit 0.

## 3\. Dove si trova il mio lavoro: la catena della SVC 13

`run <nome>` nella shell diventa una chiamata a `sys\\\_program\\\_run(name)`, cioè `svc #13`, che il kernel gestisce in `sys\\\_program\\\_run\\\_impl`. Il caricamento è diviso in sei **piani**: ognuno riceve qualcosa dal piano sopra, fa il suo lavoro e consegna un risultato al piano sotto.

|Piano|Cosa fa|Chi|File|
|-:|-|-|-|
|1|copia il nome, valida l'immagine, cerca il programma, sceglie lo slot|Antonio|`kernel/syscall.c`|
|2|controlla l'ELF e prepara il piano di carico (`elf\\\_check\\\_layout`)|Gianluigi e Athena|`kernel/elf.c`, `kernel/elf\\\_sections.c`|
|**3**|**controlla le relocation (`elf\\\_check\\\_relocs`)**|**Simone**|**`kernel/loader.c`**|
|4|copia codice e dati nello slot, azzera il BSS|Antonio|`kernel/syscall.c`|
|**5**|**corregge gli indirizzi (`elf\\\_apply\\\_relocs`)**|**Simone**|**`kernel/loader.c`**|
|6|crea il task (`task\\\_program\\\_create`, codice dato)|—|`kernel/task.c`|

Mentre il programma gira, la MPU di Giulio lo tiene chiuso nel suo slot e applica W^X.

### 3.1 La linea di non ritorno

Tra il piano 3 e il piano 4 c'è un confine:

* **sopra (piani 1, 2, 3) si legge soltanto.** Al primo problema la SVC 13 restituisce l'errore e nessuno slot è cambiato. Vale anche quando i quattro slot sono già occupati;
* **sotto (piani 4, 5, 6) si scrive nello slot.** Si arriva qui solo se tutti i controlli sono passati, e da qui nessun passo può più fallire.

Per questo il mio lavoro è diviso in **due funzioni**. `elf\\\_check\\\_relocs` sta sopra la linea: controlla tutto e non scrive niente. `elf\\\_apply\\\_relocs` sta sotto: non controlla niente e non può fallire.

### 3.2 Il contratto: `kernel/loader.h`

I piani si parlano solo attraverso `kernel/loader.h`: la `struct load\\\_plan` e le firme delle tre funzioni. Ognuno si fida delle garanzie che riceve e non ha bisogno di sapere come gli altri hanno fatto il loro lavoro.

```c
struct load\\\_plan {
    uint32\\\_t entry;                  /\\\* offset nello slot, bit 0 = 1 (Thumb) \\\*/
    uint32\\\_t file\\\_off, copy\\\_size;    /\\\* byte da copiare dal file allo slot \\\*/
    uint32\\\_t bss\\\_start, bss\\\_end;     /\\\* offset da azzerare \\\*/
    uint32\\\_t code\\\_end;               /\\\* fine della parte RX = inizio della parte RW \\\*/
    uint32\\\_t mem\\\_end;                /\\\* fine del programma, <= 3072 \\\*/
};

int  elf\\\_check\\\_layout(const uint8\\\_t \\\*f, uint32\\\_t size, struct load\\\_plan \\\*p); /\\\* 0 / -4 \\\*/

int  elf\\\_check\\\_relocs(const uint8\\\_t \\\*f, uint32\\\_t size, const struct load\\\_plan \\\*p, uint32\\\_t slot\\\_base); /\\\* 0 / -4 \\\*/

void elf\\\_apply\\\_relocs(const uint8\\\_t \\\*f, uint32\\\_t slot\\\_base);
```

**La modifica al contratto: `slot\\\_base` in `elf\\\_check\\\_relocs`.** Nella prima versione di `loader.h`, `elf\\\_check\\\_relocs` riceveva solo `f`, `size` e `p`. Ma uno dei controlli richiesti dalla traccia è che `slot\\\_base + word originale` non vada in overflow, e senza la base non si può fare. Per questo ho aggiunto il parametro `uint32\\\_t slot\\\_base`. È l'unica modifica a `loader.h` di questo branch.

Questo ha una conseguenza sull'ordine dei controlli: lo slot va scelto (piano 1) **prima** di guardare il file (piani 2 e 3). Quindi, se nessuno slot è libero, la SVC 13 restituisce `-3` anche quando il file sarebbe anche rovinato (`-4`). La traccia non dice quale errore deve prevalere quando ce n'è più di uno, quindi questo ordine è lecito.

### 3.3 Cosa ricevo e cosa so già

|Ricevo|Da chi|A cosa serve|
|-|-|-|
|`f`, `const uint8\\\_t \\\*`|piano 1|l'ELF, letto **direttamente in FLASH**, nell'immagine: nessuna copia|
|`size`, `uint32\\\_t`|piano 1|la lunghezza dell'ELF: ogni lettura deve stare in `\\\[0, size)`|
|`p`, `const struct load\\\_plan \\\*`|piano 2|solo lettura: mi servono `bss\\\_start` e `code\\\_end`|
|`slot\\\_base`, `uint32\\\_t`|piano 1|solo per il controllo dell'overflow|

E so già, perché me lo **garantisce il piano 2** (che ha restituito 0, altrimenti non verrei chiamato):

* l'header ELF è valido;
* la tabella delle sezioni (`shoff`, `shnum` righe da 40 byte) sta tutta dentro il file;
* nel file non ci sono sezioni `RELA`.

Non mi serve sapere il nome del programma, come è stato scelto lo slot né come è stato controllato l'header.

**Cosa consegno.** `elf\\\_check\\\_relocs` restituisce `0` oppure `-4`, e nient'altro: nessuna lista di site, nessuna scrittura né in `p` né nello slot. `elf\\\_apply\\\_relocs` non restituisce niente (`void`): consegna lo slot pronto per girare a `slot\\\_base`.

## 4\. Quello che serve sapere dell'ELF

Non serve conoscere tutto il formato ELF: bastano quattro pezzi.

**La tabella delle sezioni.** È l'indice del file: una riga di 40 byte (`Elf32\\\_Shdr`) per ogni sezione. Dove inizia e quante righe ha lo dice l'header ELF: `e\\\_shoff` al byte 32 (4 byte) ed `e\\\_shnum` al byte 48 (2 byte). Di ogni riga servono questi campi:

|Byte nella riga|Campo|Significato|
|-:|-|-|
|4|`sh\\\_type`|tipo: `REL` = 9, `SYMTAB` = 2, ...|
|8|`sh\\\_flags`|flag: W = 1, **A = 2** (allocated, finisce nello slot), X = 4|
|12|`sh\\\_addr`|dove va in memoria; il programma è linkato a 0, quindi è l'offset nello slot|
|16|`sh\\\_offset`|dove sta il contenuto nel file|
|20|`sh\\\_size`|quanti byte di contenuto|
|24|`sh\\\_link`|per una REL: la riga della tabella dei simboli|
|28|`sh\\\_info`|per una REL: la riga della sezione da correggere|
|36|`sh\\\_entsize`|lunghezza di una voce, se la sezione è una tabella|

Sono le colonne `Type`, `Flg`, `Addr`, `Off`, `Size`, `Lk`, `Inf` ed `ES` di `arm-none-eabi-readelf -S`.

**Le sezioni REL.** Sono un elenco di record da 8 byte (`Elf32\\\_Rel`):

|Byte nel record|Campo|Significato|
|-:|-|-|
|0|`r\\\_offset`|il **site**: dove sta la cosa da correggere, come offset nello slot|
|4|`r\\\_info`|due informazioni in un numero solo: il **tipo** nel byte basso (`r\\\_info \\\& 0xFF`) e l'**indice del simbolo** nei 24 bit alti (`r\\\_info >> 8`)|

Sono le colonne `Offset` e `Info` di `arm-none-eabi-readelf -r`.

**La tabella dei simboli.** Ogni record nomina un simbolo, cioè una voce da 16 byte (`Elf32\\\_Sym`) della sezione `SYMTAB`. Mi serve un solo campo: `st\\\_shndx` al byte 14 (2 byte), la sezione in cui il simbolo è definito. Se vale `0` (`SHN\\\_UNDEF`, `UND` in `readelf -s`), il simbolo non è definito.

**Perché il valore del simbolo non serve.** In una relocation `REL` (diversamente da `RELA`) il numero da sommare non è scritto nel record: è già dentro la word del programma. Il linker ha calcolato l'indirizzo finale supponendo la base `0` e l'ha scritto lì. Quando il programma si sposta di `slot\\\_base`, **ogni** indirizzo del programma si sposta di `slot\\\_base`, quindi basta sommare `slot\\\_base` alla word: `nuova word = vecchia word + slot\\\_base`. Del simbolo basta sapere che esiste ed è definito.

## 5\. I due tipi di relocation, con un esempio vero

**`R\\\_ARM\\\_ABS32`: una word con un indirizzo completo.** In `thumb\\\_ptr`, alla fine di `main`, il compilatore ha messo l'indirizzo di `add\\\_seven` in una word di dati in mezzo al codice, un *literal pool*:

```text
000002b8 <main>:
 2b8:  b508       push   {r3, lr}
 2ba:  2023       movs   r0, #35
 2bc:  f7ff fffa  bl     2b4 <add\\\_seven>
 2c0:  4904       ldr    r1, \\\[pc, #16]   @ legge la word a 0x2d4
 ...
 2d4:  000002b5   .word  0x000002b5      @ indirizzo di add\\\_seven, bit 0 = 1
 2d8:  000002e4   .word  0x000002e4      @ indirizzo della stringa del printf
```

`0x2b5` è "l'indirizzo di `add\\\_seven` se il programma fosse a 0", con il bit 0 acceso perché è codice Thumb. Nello slot 0 `add\\\_seven` sta in realtà a `0x200022b4`, quindi la word deve diventare `0x200022b5`. Questa è una relocation `R\\\_ARM\\\_ABS32`, con site `0x2d4`.

**`R\\\_ARM\\\_THM\\\_CALL` e `R\\\_ARM\\\_THM\\\_JUMP24`: salti relativi.** L'istruzione `bl` a `0x2bc` (`f7ff fffa`) non contiene l'indirizzo di `add\\\_seven`: contiene una distanza, −12 byte rispetto al `pc` (che nel Thumb vale l'indirizzo dell'istruzione + 4). A indirizzo 0 il salto va a `0x2bc + 4 − 12 = 0x2b4`. Nello slot 0 va a `0x200022bc + 4 − 12 = 0x200022b4`, dove adesso c'è `add\\\_seven`. Funziona senza correzioni, perché l'istruzione e la sua destinazione si sono spostate insieme. Lo stesso vale per `b.w` (`R\\\_ARM\\\_THM\\\_JUMP24`), che è un salto senza ritorno. Il linker ha già risolto questi salti: il loader controlla solo che il site sia un'istruzione vera.

**Il bit Thumb.** Il Cortex-M3 esegue solo codice Thumb, e l'indirizzo di una funzione Thumb ha il bit 0 a 1. Le basi degli slot sono multipli di 4096, cioè hanno i 12 bit bassi a zero: sommarle non tocca il bit 0. `0x2b5 + 0x20002000 = 0x200022b5`, ancora dispari.

## 6\. Le scelte di base del codice

### 6.1 Le costanti: posizioni fisse invece di struct

In cima a `loader.c` ci sono le posizioni dei campi, prese dalla specifica ELF32 (le struct di `/usr/include/elf.h`):

```c
#define EH\\\_SHOFF    32      /\\\* e\\\_shoff: dove inizia la tabella delle sezioni (4 byte) \\\*/
#define EH\\\_SHNUM    48      /\\\* e\\\_shnum: quante righe ha la tabella (2 byte) \\\*/
#define SHDR\\\_SIZE   40
#define SH\\\_TYPE      4      /\\\* tipo della sezione (REL, SYMTAB, ...) \\\*/
...
#define R\\\_ARM\\\_ABS32       2u    /\\\* word con un indirizzo completo: va corretta \\\*/
#define R\\\_ARM\\\_THM\\\_CALL   10u    /\\\* chiamata Thumb (BL): relativa, non si tocca \\\*/
#define R\\\_ARM\\\_THM\\\_JUMP24 30u    /\\\* salto Thumb (B.W): relativo, non si tocca \\\*/
```

Ogni campo si legge come "byte `N` dentro la riga", invece di sovrapporre una `struct Elf32\\\_Shdr` ai byte del file. I motivi sono tre:

* **nel kernel `<elf.h>` non c'è**, perché si compila senza libreria standard;
* **il file viene dall'esterno.** Sovrapporre una struct a byte qualsiasi suppone che siano allineati e senza padding, mentre leggere byte per byte funziona sempre;
* **è leggibile.** `row + SH\\\_INFO` dice esattamente quale campo si sta leggendo, ed è la stessa numerazione delle colonne di `readelf`.

### 6.2 `read32` e `read16`: leggere un numero dal file

```c
static uint32\\\_t read32(const uint8\\\_t \\\*p)
{
    return  (uint32\\\_t) p\\\[0]
         | ((uint32\\\_t) p\\\[1] << 8)
         | ((uint32\\\_t) p\\\[2] << 16)
         | ((uint32\\\_t) p\\\[3] << 24);
}
```

Legge un numero little-endian (il byte meno significativo per primo) dai 4 byte che iniziano a `p`. `read16` fa lo stesso con 2 byte: serve per `e\\\_shnum` e `st\\\_shndx`, che sono campi da 2 byte. Si legge byte per byte, e non con `\\\*(uint32\\\_t \\\*) p`, per due motivi:

* **`p` può non essere un multiplo di 4.** `st\\\_shndx`, per esempio, è al byte 14 di un simbolo. In C leggere una word da un indirizzo non allineato è un comportamento non definito, e il compilatore potrebbe tradurlo con istruzioni che non accettano indirizzi disallineati;
* **ogni byte viene convertito in `uint32\\\_t` prima dello spostamento.** Senza il cast, `p\\\[3] << 24` sarebbe calcolato su un `int` con segno, e con `p\\\[3] ≥ 0x80` andrebbe in overflow.

Non c'è nessuna differenza di significato: `p` è il **posto** (un puntatore dentro il file), il valore restituito è il **numero** scritto in quel posto. Nel codice si vedono tutti e due: `f + EH\\\_SHOFF` è il byte 32 del file, `read32(f + EH\\\_SHOFF)` è il numero `e\\\_shoff`.

### 6.3 `fits`: il controllo dei confini senza overflow

```c
/\\\* Is \\\[off, off + len) inside a file of size bytes?
 \\\* Written as len <= size - off so that off + len can never overflow. \\\*/
static int fits(uint32\\\_t off, uint32\\\_t len, uint32\\\_t size)
{
    return len <= size \\\&\\\& off <= size - len;
}
```

Risponde a "l'intervallo `\\\[off, off + len)` sta tutto dentro `\\\[0, size)`?". La forma ovvia sarebbe `off + len <= size`, ma con numeri a 32 bit la somma può "girare". Con un `off` rovinato, per esempio `off = 0xFFFFFFF0` e `len = 0x20`, la somma fa `0x10` e il controllo direbbe "sì, ci sta". `fits` non fa mai la somma:

1. `len <= size`: l'intervallo non è più lungo del contenitore. Questo garantisce anche che `size - len` non vada sotto zero;
2. `off <= size - len`: c'è abbastanza spazio dopo `off`.

Con i numeri di prima: `0x20 <= size` è vero, ma `0xFFFFFFF0 <= size − 0x20` è falso, quindi il risultato è "no", come deve essere.

La stessa funzione si usa per tre contenitori diversi: il file (`size`), una sezione (`tgt\\\_size`) e la parte del programma copiata dal file (`bss\\\_start`) o il codice (`code\\\_end`).

## 7\. `elf\\\_check\\\_relocs` passo per passo (piano 3)

La numerazione dei passi è la stessa dei commenti nel codice e del PDF della pipeline. Gli esempi vengono da `thumb\\\_ptr`.

### 3.1 Dove sta la tabella delle sezioni

```c
uint32\\\_t shoff = read32(f + EH\\\_SHOFF);
uint32\\\_t shnum = read16(f + EH\\\_SHNUM);
```

Per `thumb\\\_ptr`: `shoff = 0x4888`, `shnum = 25`. Qui **non** controllo che la tabella stia nel file: l'ha già fatto il piano 2, ed è la sua garanzia. Ricontrollarlo sarebbe solo codice duplicato.

### 3.2 Tutte le righe, ma solo le REL

```c
for (uint32\\\_t i = 0; i < shnum; i++)
{
    const uint8\\\_t \\\*row = f + shoff + i \\\* SHDR\\\_SIZE;
    if (read32(row + SH\\\_TYPE) != SHT\\\_REL)
        continue;
    ...
```

Non si sa in anticipo quante sezioni REL ci sono né in che posizione: dipende dal programma. Quindi si scorrono tutte le righe e si guarda il tipo di ognuna. Il tipo è un **numero** (`REL` = 9), non un nome: il loader decide sempre per tipo e flag, mai leggendo nomi come `.rel.text`, che sono solo testo per `readelf`.

Ogni sezione REL viene controllata per intero nel giro in cui la si trova, quindi non serve memorizzare niente per dopo.

Della riga REL si leggono cinque campi:

```c
uint32\\\_t rel\\\_off  = read32(row + SH\\\_OFFSET);    /\\\* dove iniziano i record nel file \\\*/
uint32\\\_t rel\\\_size = read32(row + SH\\\_SIZE);      /\\\* quanti byte di record \\\*/
uint32\\\_t entsize  = read32(row + SH\\\_ENTSIZE);   /\\\* lunghezza di un record \\\*/
uint32\\\_t link     = read32(row + SH\\\_LINK);      /\\\* riga della tabella dei simboli \\\*/
uint32\\\_t info     = read32(row + SH\\\_INFO);      /\\\* riga della sezione da correggere \\\*/
```

Per `.rel.text` di `thumb\\\_ptr` valgono `0x3cb8`, `0x98`, `8`, `22` e `1`.

### 3.3 La sezione REL stessa è fatta bene?

Questi valori vengono dal file, e un file rovinato può contenere qualunque cosa. Prima di usarli per leggere altrove, li controllo. Al primo problema la funzione restituisce `-4`.

```c
if (entsize != REL\\\_SIZE || rel\\\_size % REL\\\_SIZE != 0)
    return -4;
```

Ogni record deve essere lungo 8 byte, e la sezione deve contenerne un numero intero: altrimenti l'ultimo record sarebbe tagliato a metà. Basta che **una** delle due condizioni sia sbagliata per rifiutare, quindi si usa `||`.

```c
if (!fits(rel\\\_off, rel\\\_size, size))
    return -4;
```

Tutti i record devono essere byte del file. Senza questo controllo, un `sh\\\_offset` o un `sh\\\_size` rovinati farebbero leggere a `read32` oltre la fine del file, cioè il programma successivo nell'immagine, o peggio.

```c
if (info >= shnum || link >= shnum)
    return -4;
```

`info` e `link` sono **numeri di riga** della stessa tabella delle sezioni. Tra poco andrò a leggere proprio quelle righe, quindi devono esistere.

**Perché questi controlli vengono prima del 3.4.** La traccia dice che le relocation delle sezioni non allocated si ignorano "as long as their records fit in the file": anche quelle del debug devono avere i record dentro il file. Per questo i controlli del 3.3 valgono per **tutte** le sezioni REL, prima di decidere se ignorarle.

### 3.4 La sezione da correggere finisce nello slot?

```c
const uint8\\\_t \\\*target = f + shoff + info \\\* SHDR\\\_SIZE;
if ((read32(target + SH\\\_FLAGS) \\\& SHF\\\_ALLOC) == 0)
    continue;
```

`info` dice **quale** riga descrive la sezione bersaglio: la raggiungo con la stessa formula della riga `i`. Poi guardo il bit A (valore 2) dei suoi flag. Se non c'è, la sezione non viene copiata nello slot (è debug) e le sue relocation non vanno né controllate né applicate: passo alla riga successiva. Per `thumb\\\_ptr` succede per 6 sezioni REL su 7, in totale 331 record ignorati.

**Il bit A si guarda sulla riga del bersaglio, non su quella della REL.** Le sezioni REL hanno il flag `I` (`0x40`), che vuol dire solo "`sh\\\_info` è un indice di sezione". Per decidere se una REL conta bisogna seguire `info` fino al bersaglio: `.rel.text` → riga 1 → `.text`, flag `AX` = 6 = A + X.

Se la REL conta, leggo tre campi del bersaglio, una volta per sezione e non per ogni record, perché non cambiano:

```c
uint32\\\_t tgt\\\_addr = read32(target + SH\\\_ADDR);    /\\\* dove inizia nello slot \\\*/
uint32\\\_t tgt\\\_off  = read32(target + SH\\\_OFFSET);  /\\\* dove inizia nel file \\\*/
uint32\\\_t tgt\\\_size = read32(target + SH\\\_SIZE);    /\\\* quanto è lunga \\\*/
```

Per `.text` di `thumb\\\_ptr`: `0`, `0x1000` e `0x310`.

### 3.5 La tabella dei simboli

```c
const uint8\\\_t \\\*symrow = f + shoff + link \\\* SHDR\\\_SIZE;
if (read32(symrow + SH\\\_TYPE) != SHT\\\_SYMTAB || read32(symrow + SH\\\_ENTSIZE) != SYM\\\_SIZE)
    return -4;

uint32\\\_t sym\\\_off  = read32(symrow + SH\\\_OFFSET);
uint32\\\_t sym\\\_size = read32(symrow + SH\\\_SIZE);
if (sym\\\_size % SYM\\\_SIZE != 0 || !fits(sym\\\_off, sym\\\_size, size))
    return -4;

uint32\\\_t nsyms = sym\\\_size / SYM\\\_SIZE;
```

Anche la tabella dei simboli non si cerca per nome: la si raggiunge da `link` (per `thumb\\\_ptr` la riga 22, `.symtab`). Deve essere davvero di tipo `SYMTAB`, con voci da 16 byte, un numero intero di voci, tutte dentro il file, perché tra poco le leggerò. Per `thumb\\\_ptr`: `sym\\\_off = 0x377c`, `sym\\\_size = 0x3d0`, quindi `nsyms = 61`. Un record può usare solo i simboli da 0 a 60.

### 3.6 Ogni record

```c
uint32\\\_t nrel = rel\\\_size / REL\\\_SIZE;
for (uint32\\\_t r = 0; r < nrel; r++)
{
    const uint8\\\_t \\\*rec = f + rel\\\_off + r \\\* REL\\\_SIZE;
    uint32\\\_t site   = read32(rec + R\\\_OFFSET);
    uint32\\\_t r\\\_info = read32(rec + R\\\_INFO);
    uint32\\\_t rtype  = r\\\_info \\\& 0xFF;
    uint32\\\_t sym    = r\\\_info >> 8;
    ...
```

Per `.rel.text` di `thumb\\\_ptr` i record sono `0x98 / 8 = 19`, tutti dentro il file grazie al 3.3. Per il primo, `site = 0x0` e `r\\\_info = 0x350a`, quindi tipo `0x0a = 10` (`R\\\_ARM\\\_THM\\\_CALL`) e simbolo `0x35 = 53` (`main`): è la chiamata a `main` in `\\\_start`.

Poi tre controlli, in questo ordine.

**Il tipo è ammesso?**

```c
if (rtype != R\\\_ARM\\\_ABS32 \\\&\\\& rtype != R\\\_ARM\\\_THM\\\_CALL \\\&\\\& rtype != R\\\_ARM\\\_THM\\\_JUMP24)
{
    kprintf("loader: unsupported relocation type %u\\\\n", rtype);
    return -4;
}
```

Se il tipo non è uno dei tre, stampo la riga richiesta dalla traccia, con il tipo in decimale (`%u`), e rifiuto. Questo controllo l'ho messo **subito dopo** aver letto il tipo, prima di guardare il simbolo, con il commit "sistemazioni fatte". La traccia vuole che il messaggio esca *prima* del rifiuto. Se il controllo venisse dopo, un record di tipo 3 con anche un simbolo non definito verrebbe rifiutato al controllo del simbolo, senza stampare la riga. Così invece la riga esce sempre.

Si usa `kprintf` e non `printf` perché siamo dentro una SVC: `printf` farebbe a sua volta una `svc`, e una `svc` dentro un'eccezione manda il processore in HardFault.

**Il simbolo esiste?**

```c
if (sym >= nsyms)
    return -4;
const uint8\\\_t \\\*s = f + sym\\\_off + sym \\\* SYM\\\_SIZE;
```

L'indice va controllato **prima** di usarlo: un indice enorme (fino a 2²⁴ − 1) farebbe leggere fuori dal file. Con `sym < nsyms`, e i simboli tutti nel file (3.5), anche il simbolo `sym` è nel file.

**Il simbolo è definito?**

```c
if (read16(s + ST\\\_SHNDX) == SHN\\\_UNDEF)
    return -4;
```

Un simbolo non definito vuol dire che il programma usa qualcosa che non contiene, e nessuno saprebbe dove punta. La traccia chiede di rifiutare questi programmi. È un campo da 2 byte, quindi `read16`.

### 3.7 – 3.8 Controlli diversi secondo il tipo

Uno `switch` sul tipo. Il caso `R\\\_ARM\\\_ABS32` è racchiuso tra graffe perché dichiara una variabile (`word`), e in C una dichiarazione dentro un `case` ha bisogno di un blocco suo.

#### 3.7 `R\\\_ARM\\\_ABS32`

È una word da 4 byte che `elf\\\_apply\\\_relocs` correggerà. Qui controllo che si possa fare senza danni. Esempio: site `0x2d4`, il puntatore ad `add\\\_seven`.

**(a) Allineata a 4.**

```c
if (site % 4 != 0)
    return -4;
```

La word verrà scritta come `uint32\\\_t`, quindi deve essere una word intera e allineata. `0x2d4 % 4 = 0`.

**(b) Dentro la sezione bersaglio.**

```c
if (site < tgt\\\_addr || !fits(site - tgt\\\_addr, 4, tgt\\\_size))
    return -4;
```

La sezione occupa, nello slot, `\\\[tgt\\\_addr, tgt\\\_addr + tgt\\\_size)`, per `.text` `\\\[0, 0x310)`. Il site non deve stare prima dell'inizio, e i 4 byte della word, contati dall'inizio della sezione (`site - tgt\\\_addr`), devono starci dentro. L'ordine conta: grazie a `||`, `site - tgt\\\_addr` si calcola solo quando `site >= tgt\\\_addr`, quindi la sottrazione non va mai sotto zero. Senza questo controllo, applicare la relocation scriverebbe fuori dal programma.

**(c) Tra i byte copiati dal file.**

```c
if (!fits(site, 4, p->bss\\\_start))
    return -4;
```

Il BSS non ha byte nel file, esiste solo la sua dimensione: una word lì non avrebbe un valore originale da leggere, e verrebbe comunque azzerata. Quindi la word deve stare prima di `bss\\\_start`, che arriva dal piano di carico del piano 2 (per `thumb\\\_ptr` vale `0x310`).

**(d) Il valore originale, letto dal file.**

```c
if (!fits(tgt\\\_off, tgt\\\_size, size))
    return -4;
uint32\\\_t word = read32(f + tgt\\\_off + (site - tgt\\\_addr));
```

La word si legge dal **file**, non dallo slot: siamo sopra la linea di non ritorno e lo slot non è ancora stato riempito. La sezione inizia nel file a `tgt\\\_off`, e la word sta `site - tgt\\\_addr` byte più avanti. Prima di leggere controllo che la sezione stia nel file: così anche quel posto ci sta. Per site `0x2d4`: si legge a `f + 0x1000 + 0x2d4` e si trova `0x2b5`.

**(e) Niente overflow.**

```c
if (word > 0xFFFFFFFFu - slot\\\_base)
    return -4;
```

`slot\\\_base + word` non deve superare `0xFFFFFFFF`, altrimenti la somma "girerebbe" e il puntatore diventerebbe un indirizzo basso qualunque. Anche qui la somma non si fa: si confronta `word` con lo spazio che resta sopra `slot\\\_base`. Con lo slot 0 il limite è `0xFFFFFFFF − 0x20002000 = 0xDFFFDFFF`, e `0x2b5` ci sta. Una word come `0xE0000000` verrebbe rifiutata. È il controllo che rende necessario `slot\\\_base` in `elf\\\_check\\\_relocs` (sezione 3.2).

**Dove punta la word non si controlla.** La traccia dice che il valore originale può puntare fuori dal programma, e che non è un errore: il loader deve solo poter fare la somma senza danni.

#### 3.8 `R\\\_ARM\\\_THM\\\_CALL` e `R\\\_ARM\\\_THM\\\_JUMP24`

Sono salti relativi (sezione 5): non si correggono. Controllo solo che il site sia un'istruzione vera.

**(a) Allineato a 2.**

```c
if (site % 2 != 0)
    return -4;
```

Le istruzioni Thumb sono allineate a 2 byte. Un site come `0x066` va bene anche se non è multiplo di 4; uno dispari no.

**(b) Dentro il codice.**

```c
if (!fits(site, 4, p->code\\\_end))
    return -4;
```

`bl` e `b.w` sono istruzioni da 4 byte, e devono stare nel codice, `\\\[0, code\\\_end)`. `code\\\_end` arriva dal piano di carico (per `thumb\\\_ptr` vale `0x310`).

#### 3.9 Qualunque altro tipo

```c
default:
    kprintf("loader: unsupported relocation type %u\\\\n", rtype);
    return -4;
```

È il ramo che stampa il messaggio per un tipo non ammesso. Da quando il controllo del tipo è stato anticipato al 3.6, qui non si arriva più, perché allo `switch` arrivano solo i tre tipi ammessi. È rimasto come difesa: se un giorno il controllo anticipato venisse tolto, il messaggio uscirebbe comunque.

### Alla fine

```c
return 0;
```

Si arriva qui solo se **tutte** le sezioni REL che contano e **tutti** i loro record hanno superato i controlli.

## 8\. `elf\\\_apply\\\_relocs` passo per passo (piano 5)

Questa funzione viene chiamata solo dopo che:

* `elf\\\_check\\\_relocs` ha restituito `0` sullo stesso `f` e con lo stesso `slot\\\_base`, quindi ogni sezione, record e site è già valido;
* il piano 4 ha già copiato il programma nello slot e azzerato il BSS.

Per questo non contiene nessun controllo e non può fallire: è `void`.

### 5.1 Lo stesso giro del controllo

```c
uint32\\\_t shoff = read32(f + EH\\\_SHOFF);
uint32\\\_t shnum = read16(f + EH\\\_SHNUM);
for (uint32\\\_t i = 0; i < shnum; i++)
{
    const uint8\\\_t \\\*row = f + shoff + i \\\* SHDR\\\_SIZE;
    if (read32(row + SH\\\_TYPE) != SHT\\\_REL)
        continue;

    uint32\\\_t rel\\\_off  = read32(row + SH\\\_OFFSET);
    uint32\\\_t rel\\\_size = read32(row + SH\\\_SIZE);
    uint32\\\_t info     = read32(row + SH\\\_INFO);

    const uint8\\\_t \\\*target = f + shoff + info \\\* SHDR\\\_SIZE;
    if ((read32(target + SH\\\_FLAGS) \\\& SHF\\\_ALLOC) == 0)
        continue;
    ...
```

Rifà lo stesso percorso di `elf\\\_check\\\_relocs`: stesse sezioni REL, e soprattutto **la stessa regola del 3.4**, che qui è obbligatoria. Le relocation del debug hanno site che si riferiscono alle sezioni di debug, non allo slot. Applicarle scriverebbe nello slot in posti sbagliati e rovinerebbe il codice. Le due funzioni devono quindi scorrere il file esattamente nello stesso modo.

Della riga REL servono solo tre campi: dove sono i record, quanti byte occupano e quale sezione correggono. `entsize` e `link` servivano solo ai controlli.

### 5.2 Ogni `R\\\_ARM\\\_ABS32`: word + slot\_base

```c
uint32\\\_t nrel = rel\\\_size / REL\\\_SIZE;
for (uint32\\\_t r = 0; r < nrel; r++)
{
    const uint8\\\_t \\\*rec = f + rel\\\_off + r \\\* REL\\\_SIZE;
    uint32\\\_t site  = read32(rec + R\\\_OFFSET);
    uint32\\\_t rtype = read32(rec + R\\\_INFO) \\\& 0xFF;

    if (rtype == R\\\_ARM\\\_ABS32)
    {
        uint32\\\_t \\\*word = (uint32\\\_t \\\*) (uintptr\\\_t) (slot\\\_base + site);
        \\\*word += slot\\\_base;
    }
}
```

Riga per riga:

* **dove sta la word in RAM.** Il programma è linkato a 0, quindi il site è già l'offset dentro lo slot. Per `thumb\\\_ptr` nello slot 0, site `0x2d4` → `0x200022d4`;
* **il cast.** `slot\\\_base + site` è un numero; per scriverci bisogna trasformarlo in un puntatore a una word. Passare da `uintptr\\\_t` è il modo corretto in C di convertire un intero in un puntatore;
* **l'allineamento.** Si può scrivere direttamente come `uint32\\\_t` perché il site è multiplo di 4 (3.7 a) e `slot\\\_base` è multiplo di 4096, quindi l'indirizzo è allineato;
* **si scrive nello slot, in RAM**, non nel file `f`, che è in FLASH e in sola lettura. La word nello slot è quella che il piano 4 ha appena copiato dal file;
* **`\\\*word += slot\\\_base`**: legge la word, le somma la base e la riscrive nello stesso posto. `0x000002b5` → `0x200022b5`. Il bit 0 resta 1, perché `slot\\\_base` ha i 12 bit bassi a zero. Non c'è overflow: l'ha escluso il 3.7 (e).

**Le word si possono scrivere anche se sono nel codice.** Le word ABS32 di `thumb\\\_ptr` sono nei literal pool, dentro `.text`, che per W^X il programma non può scrivere. Ma qui scrive il **kernel**, privilegiato, durante la SVC 13, quindi prima che il programma parta. In quel momento la MPU è impostata per chi ha chiamato la SVC 13 (la shell, oppure un programma che ne lancia un altro, come in `nonzero\\\_slot\\\_relocation`), e lo slot nuovo non è coperto da nessuna delle sue regioni: il kernel ci può scrivere liberamente.

### 5.3 I salti Thumb: nessuna modifica

Per `R\\\_ARM\\\_THM\\\_CALL` e `R\\\_ARM\\\_THM\\\_JUMP24` non si fa niente: sono relativi, codice e destinazione si sono spostati insieme. Altri tipi qui non possono arrivare, perché li ha già rifiutati `elf\\\_check\\\_relocs`.

## 9\. Perché due funzioni e due giri

Si potrebbe pensare di controllare e correggere nello stesso ciclo. Non si può, per la regola R9: **nessun byte del programma deve essere scritto prima che tutti i controlli siano passati**. Con un solo giro, un errore al record 15 arriverebbe quando i record da 1 a 14 hanno già modificato lo slot.

Un'alternativa sarebbe stata far restituire a `elf\\\_check\\\_relocs` la lista dei site da correggere. L'ho scartata:

* **servirebbe memoria.** Un programma da 3072 byte può avere fino a 768 word ABS32, e nel kernel non c'è allocazione dinamica: servirebbe un buffer fisso grande abbastanza per il caso peggiore;
* **rileggere il file costa poco.** È in FLASH, non cambia tra un giro e l'altro e non va copiato.

Per questo il PDF della pipeline dice che il piano 3 non consegna "nessuna lista di site": consegna solo `0` o `-4`. Il piano 5 rilegge le sezioni REL da `f`.

||`elf\\\_check\\\_relocs` (piano 3)|`elf\\\_apply\\\_relocs` (piano 5)|
|-|-|-|
|Quando|prima della copia|dopo la copia|
|Legge|il file in FLASH|il file in FLASH|
|Scrive|niente|le word ABS32 nello slot|
|Controlla|tutto|niente|
|Risultato|`0` o `-4`|nessuno (`void`)|
|Sezioni REL|tutte; ignora quelle con bersaglio non allocated|le stesse, con la stessa regola|

## 10\. `thumb\\\_ptr.elf` sotto la lente

È il programma usato dai test `thumb\\\_ptr` e `nonzero\\\_slot\\\_relocation`, ed è il caso guida del PDF dell'anatomia. Il sorgente:

```c
static uint32\\\_t add\\\_seven( uint32\\\_t value )
{
    return value + 7;
}

static uint32\\\_t ( \\\*operation )( uint32\\\_t ) = add\\\_seven;

int main( void )
{
    uintptr\\\_t address = (uintptr\\\_t) operation;
    printf( "thumb\\\_function\\\_pointer: thumb=%u result=%u\\\\n",
            (uint32\\\_t) ( address \\\& 1u ), operation( 35 ) );
    return 0;
}
```

### 10.1 Il file in numeri

|||
|-|-|
|Byte nel file|19568|
|Byte che vanno nello slot|`0x310` (784): solo `.text`|
|Segmenti `PT\\\_LOAD`|1|
|Sezioni|25, di cui 7 REL|
|Relocation|350 in tutto; 19 contano (quelle di `.rel.text`)|
|Word da correggere (ABS32)|4|

### 10.2 Mappa del file

|Offset|Contenuto|Chi lo usa|
|-|-|-|
|`0x0000–0x0033`|ELF header, 52 byte|piano 2; il piano 3 legge solo `e\\\_shoff` ed `e\\\_shnum`|
|`0x0034–0x0053`|program header, una riga|piano 2 (diventa il `load\\\_plan`)|
|`0x0054–0x0fff`|zeri, per allineare `.text` a `0x1000`|nessuno|
|`0x1000–0x130f`|`.text`: codice e literal pool, `0x310` byte|l'unica parte che va nello slot: il piano 4 la copia, il piano 5 ne corregge 4 word|
|`0x1310–0x377b`|attributi, commenti, debug|nessuno: non allocated|
|`0x377c–0x3b4b`|`.symtab`, 61 simboli|piano 3|
|`0x3b4c–0x3cb5`|`.strtab`, nomi dei simboli|nessuno|
|`0x3cb8–0x3d4f`|`.rel.text`, 19 record|piano 3 (controlla), piano 5 (applica)|
|`0x3d50–0x47a7`|6 sezioni `.rel.debug\\\_\\\*`, 331 record|piano 3: controlla solo che stiano nel file, poi le ignora|
|`0x47a8–0x4885`|`.shstrtab`, nomi delle sezioni|nessuno|
|`0x4888–0x4c6f`|tabella delle sezioni, 25 righe|piani 2 e 3; finisce a `0x4c70` = 19568, l'ultimo byte del file|

> \\\*\\\*Nota sui numeri.\\\*\\\* Gli offset delle parti che \\\*\\\*non\\\*\\\* vanno nello slot (debug, simboli, tabella delle sezioni) cambiano leggermente da una build all'altra, perché le informazioni di debug contengono i percorsi delle cartelle in cui si compila. Su un'altra macchina, per esempio, il file può essere lungo 19340 byte, con `e\\\_shoff = 0x47a4` e `.rel.text` a `0x3bd4`. Tutto quello che va nello slot invece è identico: `.text` lunga `0x310`, gli stessi 19 record, gli stessi site e le stesse word.

### 10.3 Le sezioni che contano

|Nr|Nome|Tipo|Addr|Off|Size|ES|Flg|Lk|Inf|Per il piano 3|
|-:|-|-|-|-|-|-:|-|-:|-:|-|
|1|`.text`|PROGBITS|0|`0x1000`|`0x310`|0|AX|0|0|bersaglio di `.rel.text`|
|2|`.rel.text`|REL|0|`0x3cb8`|`0x98`|8|I|22|1|bersaglio allocated → 19 record da controllare|
|3|`.data`|PROGBITS|`0x310`|`0x1310`|0|0|WA|0|0|vuota|
|4|`.bss`|NOBITS|`0x310`|`0x1310`|0|0|WA|0|0|vuota|
|8, 11, 14, 17, 19, 21|`.rel.debug\\\_\\\*`|REL|0|||8|I|22|7, 10, 13, 16, 18, 20|bersaglio senza A → si ignorano (dopo il 3.3)|
|22|`.symtab`|SYMTAB|0|`0x377c`|`0x3d0`|16||23|38|61 simboli|

`.data` e `.bss` sono vuote perché, compilando con `-O2`, il compilatore vede che `operation` non cambia mai: invece di tenerla in `.data`, mette direttamente l'indirizzo di `add\\\_seven` nel literal pool di `main`.

### 10.4 Le 19 relocation di `.rel.text`

Tutti i simboli usati hanno indice < 61 e sono definiti in `.text`.

|Site|Info|Tipo|Simbolo|Controllo|Dopo la copia|
|-|-|-|-|-|-|
|`0x000`|`0000350a`|THM\_CALL|`main` (53)|pari, `+ 4 ≤ 0x310`|nessuna modifica|
|`0x004`|`00003b0a`|THM\_CALL|`sys\\\_exit` (59)|pari, nel codice|nessuna modifica|
|`0x018`|`00003b1e`|THM\_JUMP24|`sys\\\_exit` (59)|pari, nel codice|nessuna modifica|
|`0x040`|`00003b0a`|THM\_CALL|`sys\\\_exit` (59)|pari, nel codice|nessuna modifica|
|`0x044`|`0000391e`|THM\_JUMP24|`task\\\_exit` (57)|pari, nel codice|nessuna modifica|
|`0x066`|`0000310a`|THM\_CALL|`sys\\\_write` (49)|pari (non multiplo di 4: va bene)|nessuna modifica|
|`0x088`|`0000310a`|THM\_CALL|`sys\\\_write` (49)|pari, nel codice|nessuna modifica|
|`0x0b4`|`00002f0a`|THM\_CALL|`vformat` (47)|pari, nel codice|nessuna modifica|
|`0x0c2`|`0000310a`|THM\_CALL|`sys\\\_write` (49)|pari, nel codice|nessuna modifica|
|**`0x0e0`**|`00001602`|**ABS32**|`printf\\\_out` (22)|multiplo di 4, dentro `.text`, niente overflow|**word + slot\_base**|
|`0x0e8`|`0000370a`|THM\_CALL|`sys\\\_uptime` (55)|pari, nel codice|nessuna modifica|
|`0x0f2`|`0000370a`|THM\_CALL|`sys\\\_uptime` (55)|pari, nel codice|nessuna modifica|
|`0x104`|`0000330a`|THM\_CALL|`task\\\_tick\\\_count` (51)|pari, nel codice|nessuna modifica|
|`0x10a`|`0000330a`|THM\_CALL|`task\\\_tick\\\_count` (51)|pari, nel codice|nessuna modifica|
|**`0x2b0`**|`00000102`|**ABS32**|`.text` (1)|multiplo di 4, dentro `.text`, niente overflow|**word + slot\_base**|
|`0x2bc`|`0000220a`|THM\_CALL|`add\\\_seven` (34)|pari, nel codice|nessuna modifica|
|`0x2ca`|`0000280a`|THM\_CALL|`printf` (40)|pari, `+ 4 = 0x2ce ≤ 0x310`|nessuna modifica|
|**`0x2d4`**|`00002202`|**ABS32**|`add\\\_seven` (34)|multiplo di 4, dentro `.text`, niente overflow|**word + slot\_base**|
|**`0x2d8`**|`00000102`|**ABS32**|`.text` (1)|multiplo di 4, `+ 4 = 0x2dc ≤ 0x310`|**word + slot\_base**|

Due record usano il simbolo `.text` (indice 1): è il simbolo della sezione stessa, che vale 0. Con una relocation `REL` lo scostamento è già scritto nella word (sezione 4), quindi la word contiene direttamente l'offset dell'oggetto dentro `.text`.

### 10.5 Le 4 word ABS32: cosa sono e cosa diventano

|Site|Word nel file|Cos'è|Slot 0 (`0x20002000`)|Slot 1 (`0x20003000`)|
|-|-|-|-|-|
|`0x0e0`|`0x00000049`|puntatore alla funzione `printf\\\_out` (`0x48`, bit Thumb acceso), che `printf` passa a `vformat`|`0x20002049`|`0x20003049`|
|`0x2b0`|`0x000002dc`|indirizzo della stringa `"(null)"`, usata da `vformat`|`0x200022dc`|`0x200032dc`|
|`0x2d4`|`0x000002b5`|puntatore alla funzione `add\\\_seven` (`0x2b4`, bit Thumb acceso)|`0x200022b5`|`0x200032b5`|
|`0x2d8`|`0x000002e4`|indirizzo della stringa di formato del `printf` di `main`|`0x200022e4`|`0x200032e4`|

Si vede bene perché sono tutte dentro `.text`: nella tabella dei simboli ci sono dei marcatori `$d` ("qui iniziano dati") proprio a `0x0e0`, `0x2b0` e `0x2d4`. Sono i literal pool, word di dati in mezzo al codice, dove il compilatore mette gli indirizzi completi.

**Cosa succederebbe senza correzione.** Le word punterebbero a indirizzi bassi, in FLASH, dove c'è il kernel e non il programma. Per esempio `printf` chiamerebbe `0x49`, cioè codice del kernel, invece di `printf\\\_out` nello slot, e la stringa di formato verrebbe letta da `0x2e4` invece che dallo slot.

> Nel PDF della pipeline, al punto 5.2, l'esempio della correzione usa il valore `0x2D`. Il valore reale di `thumb\\\_ptr` è `0x2b5`, come riportato nel PDF dell'anatomia e qui.

### 10.6 Cosa diventa nello slot 0

|Indirizzi|Contenuto|
|-|-|
|`0x20002000–0x2000230f`|codice e literal pool: `0x310` byte copiati da `f + 0x1000`; le word a `0x200020e0`, `0x200022b0`, `0x200022d4` e `0x200022d8` corrette dal piano 5|
|da `0x20002310`|`.data` e `.bss`: 0 byte, niente da copiare né da azzerare|
|`0x20002310–0x20002bff`|libero: il programma potrebbe arrivare fino a 3072 byte|
|`0x20002c00–0x20002fff`|stack, 1 KiB; frame iniziale di 64 byte a `0x20002fc0`|

Il piano 6 riceve `entry = 0x20002000 + 0x1 = 0x20002001`, `stack\\\_top = 0x20003000` e assegna l'id 4 (generation 1 × 4 + slot 0).

```text
scratch> run thumb\\\_ptr
loader: thumb\\\_ptr slot=0 base=0x20002000 id=4
thumb\\\_function\\\_pointer: thumb=1 result=42
program thumb\\\_ptr exited
scratch>
```

In questa riga si vedono **tutti e due i tipi di relocation**:

* `result=42` viene da `operation(35)`, che il compilatore ha tradotto in un `bl add\\\_seven` diretto (site `0x2bc`): un salto relativo, che funziona senza correzioni;
* `thumb=1` viene dal bit 0 della word a `0x2d4`, letta da `ldr r1, \\\[pc, #16]`: una ABS32, corretta da `0x2b5` a `0x200022b5` con il bit Thumb ancora a 1;
* la stringa stessa si stampa perché anche le word a `0x2d8` (il formato) e a `0x0e0` (`printf\\\_out`) sono state corrette.

Nel test `nonzero\\\_slot\\\_relocation` lo stesso `thumb\\\_ptr` viene lanciato da un altro programma che occupa lo slot 0, quindi finisce nello slot 1, con base `0x20003000`. Le word diventano quelle dell'ultima colonna della tabella 10.5, e l'uscita è la stessa: è la prova che la relocation funziona anche in uno slot diverso da 0.

## 11\. Cosa viene rifiutato

Tutti i rifiuti del piano 3 restituiscono `-4`. `sys\\\_program\\\_run\\\_impl` lo passa alla shell, che stampa `error: program cannot be loaded`.

|Problema nel file|Dove si ferma|Stampa in più|
|-|-|-|
|sezione REL con `entsize ≠ 8` o dimensione non multipla di 8|3.3|—|
|record REL fuori dal file|3.3|—|
|`sh\\\_info` o `sh\\\_link` fuori dalla tabella delle sezioni|3.3|—|
|tabella dei simboli di tipo sbagliato, voci non da 16 byte, fuori dal file|3.5|—|
|tipo di relocation non ammesso, in una sezione allocated|3.6|`loader: unsupported relocation type N`|
|indice del simbolo ≥ numero dei simboli|3.6|—|
|simbolo non definito|3.6|—|
|ABS32 non allineata, fuori dalla sua sezione, nel BSS|3.7 (a), (b), (c)|—|
|sezione bersaglio fuori dal file|3.7 (d)|—|
|`slot\\\_base + word` oltre `0xFFFFFFFF`|3.7 (e)|—|
|salto Thumb a indirizzo dispari o fuori dal codice|3.8 (a), (b)|—|

**I test del kit che passano dal piano 3.** Le fixture rovinate sono ricavate da `print\\\_plain`:

* **`unsupported\\\_reloc`**: il tipo del primo record di `.rel.text` è cambiato in 3. Il controllo anticipato del 3.6 stampa `loader: unsupported relocation type 3` e rifiuta. Subito dopo un programma buono parte nello slot 0 con id 4, perché il rifiuto non ha toccato nessuno slot;
* **`bad\\\_reloc`**: il site del primo record di `.rel.text` viene spostato subito dopo la fine di `.text`, cioè fuori dal programma. Quel record è la chiamata a `main` (`R\\\_ARM\\\_THM\\\_CALL`), quindi lo ferma il controllo (b) del 3.8: l'istruzione non sta nel codice. Se fosse una ABS32, la fermerebbe il controllo (b) del 3.7;
* **`truncated\\\_elf`**: il file è tagliato un byte prima della fine del segmento da caricare, quindi anche la tabella delle sezioni, che viene dopo, finisce oltre la fine del file. Lo rifiuta già il piano 2, prima di arrivare al piano 3. Il 3.3 protegge invece da un caso diverso: tabella delle sezioni sana, ma `sh\\\_offset` o `sh\\\_size` di una REL che puntano oltre la fine del file;
* **`thumb\\\_ptr`** e **`nonzero\\\_slot\\\_relocation`**: i due casi positivi, nello slot 0 e nello slot 1 (sezione 10).

## 12\. Le scelte di progetto, in breve

|Scelta|Perché|
|-|-|
|Due funzioni, una che controlla e una che scrive|R9: tutti i controlli prima di scrivere un byte; la linea di non ritorno della pipeline|
|Il secondo giro rilegge il file invece di usare una lista di site|niente allocazione dinamica nel kernel; il file è in FLASH, non cambia e non va copiato|
|`slot\\\_base` aggiunto a `elf\\\_check\\\_relocs`|serve per il controllo dell'overflow di `slot\\\_base + word`|
|Ci si fida delle garanzie del piano 2 (header, tabella delle sezioni, niente RELA)|ricontrollarle sarebbe duplicazione; ogni piano lavora su quello che riceve|
|Lettura byte per byte (`read32`, `read16`)|il file viene dall'esterno e i campi possono essere disallineati; nessuna dipendenza da `<elf.h>`|
|`fits` senza somme|un valore rovinato non può far "girare" il calcolo|
|Indici controllati prima di essere usati (`info`, `link`, `sym`)|non si legge mai una riga o un simbolo che non esiste|
|Sezioni scelte per tipo e flag, mai per nome|il nome è solo testo; tipo e flag sono quello che conta davvero|
|Controlli del 3.3 su tutte le REL, anche del debug|la traccia vuole che anche i record ignorati stiano nel file|
|Il controllo del tipo subito dopo averlo letto|il messaggio `unsupported relocation type` esce sempre, prima di qualunque altro rifiuto|
|La word originale letta dal file, non dallo slot|al momento del controllo lo slot non è ancora stato riempito|
|`kprintf` e mai `printf`|siamo dentro una SVC: `printf` farebbe un'altra `svc` e un HardFault|
|Nessun output di debug|il test kit confronta l'uscita della UART byte per byte: le uniche righe ammesse sono quelle della specifica|

## 13\. Stato del branch

In questo branch `loader.c` compila senza warning insieme al resto del kernel. Le due funzioni sono pronte ma non ancora chiamate: il collegamento con `sys\\\_program\\\_run\\\_impl` (piano 1) e con `elf\\\_check\\\_layout` (piano 2) avviene nell'integrazione dei branch, documentata a parte.

Per vedere da soli tutti i valori di questo file:

```bash
arm-none-eabi-readelf -h build/programs/thumb\\\_ptr.elf    # e\\\_shoff, e\\\_shnum
arm-none-eabi-readelf -S build/programs/thumb\\\_ptr.elf    # la tabella delle sezioni
arm-none-eabi-readelf -r build/programs/thumb\\\_ptr.elf    # le relocation
arm-none-eabi-readelf -s build/programs/thumb\\\_ptr.elf    # i simboli, compresi i $d
arm-none-eabi-objdump -d build/programs/thumb\\\_ptr.elf    # il codice e i literal pool
```

