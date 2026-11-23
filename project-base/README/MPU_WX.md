# MPU e protezione W^X — appunti di implementazione

Documentazione del progetto Scratch-OS (protezione della memoria) · Gruppo 6 · Versione Big

## 1. Scopo e contesto: dal modello base al W^X

Nella base di Scratch-OS fornita dal corso, la **MPU (Memory Protection Unit)** utilizza solo due regioni hardware:

* **Regione 0:** tutta la memoria FLASH (`0x00000000`, 4 MiB), con permessi Read-Only ed eseguibile (`RO + Exec`), usata per il codice e le costanti di `shell` e `idle`.

* **Regione 1:** la partizione RAM da 4 KiB del task attualmente in esecuzione, impostata con `mpu_set_task_region()`.

Il limite della base riguarda i **programmi caricati dinamicamente in RAM** (negli slot `0x20002000..0x20005FFF`): chiamando `mpu_set_task_region()` con `executable = 1`, l'intero slot da 4 KiB riceve contemporaneamente i permessi di lettura, scrittura **ed** esecuzione (**RWX**). Questa configurazione viola il principio di sicurezza **W^X (Write XOR Execute)**:

* un programma può sovrascrivere le proprie istruzioni in RAM (il test `wx_write_code` non verrebbe bloccato);

* un programma può saltare ed eseguire byte presenti nella propria area dati o sullo stack (il test `wx_exec_data` non verrebbe bloccato).

L'obiettivo del lavoro documentato qui è implementare una protezione **W^X reale in hardware** dentro ogni slot da 4 KiB, mantenendo intatto l'isolamento del kernel e degli altri slot:

* **Codice e costanti (`.text`, `.rodata`):** solo lettura ed esecuzione (`RO + Exec`, `XN = 0`).

* **Dati e stack (`.data`, `.bss`, stack):** lettura e scrittura, **non eseguibili** (`RW + XN`, `XN = 1`).

* **Tutto il resto della RAM** (kernel, `shell`, `idle`, altri slot): nessuna regione abilitata per il codice utente, quindi qualsiasi accesso scatena immediatamente un `MemManage` fault (bloccando `kernel_store` e gli stack overflow).

## 2. Come funziona la MPU del Cortex-M3 (concetti chiave)

Per capire le scelte implementative in `kernel/mpu.c` e `kernel/task.c` è importante tenere chiari cinque aspetti dell'hardware ARM Cortex-M3:

1. **Le regioni MPU non sono memoria, ma regole di controllo.**
   Una regione non alloca byte e non sposta né sovrascrive dati in RAM. La MPU è un blocco logico combinatorio posto sul bus tra la CPU e la memoria: dispone di esattamente **8 regioni hardware** (numerate da `0` a `7`, valore cablato nel silicio e leggibile in `MPU->TYPE`).

2. **Controllo in parallelo a ogni accesso.**
   A ogni operazione di memoria (`ldr`, `str` o instruction fetch), l'indirizzo a 32 bit viene confrontato **contemporaneamente** con tutte le regioni abilitate in meno di un ciclo di clock, senza alcun ciclo software nel kernel. Se l'accesso è vietato (o in modalità non privilegiata non cade in nessuna regione attiva), la MPU blocca il trasferimento prima che raggiunga la RAM e solleva un'eccezione `MemManage`.

3. **Vincoli geometrici rigidi.**
   Per decidere in un solo ciclo tramite maschere di bit, ogni regione deve avere una dimensione potenza di 2, con un minimo di $2^5 = 32$ byte fino a 4 GiB, codificata nel campo `SIZE` come $\text{dimensione} = 2^{\text{SIZE}+1}$. L'indirizzo base deve essere un multiplo esatto della dimensione (i bit `4:0` di `MPU->RBAR` sono occupati da `VALID` e `REGION`, quindi i 5 bit più bassi della base sono sempre zero).

4. **Priorità in caso di sovrapposizione.**
   Se due regioni abilitate coprono lo stesso indirizzo, l'hardware applica sempre i permessi della regione con **indice numerico più alto**. È questo meccanismo che ci permette di sovrapporre la **Regione 2** (codice, `RO + Exec`) sopra la **Regione 1** (intero slot, `RW + XN`).

5. **Sub-regioni (`SRD`, bit `15:8` di `MPU->RASR`).**
   Ogni regione di dimensione $\ge 256$ byte viene divisa dall'hardware in **8 fette (sub-regioni) uguali**. Su uno slot da 4 KiB ($4096$ byte), ogni fetta misura esattamente:
   

   $$
   \text{sub\_size} = \frac{4096}{8} = 512 \text{ byte} \quad (\text{0x200})
   $$

   
   Gli 8 bit del campo `SRD` (*Sub-Region Disable*) controllano ciascuna fetta con logica negata: **`0` = fetta attiva**, **`1` = fetta disabilitata**.

6. **Il kernel resta libero (`PRIVDEFENA = 1`).**
   In `MPU->CTRL` è attivo il bit `MPU_CTRL_PRIVDEFENA`: quando la CPU gira in modalità privilegiata (Handler mode, cioè kernel, syscall, interrupt e fault), qualsiasi accesso che non rientra in una regione attiva utilizza la mappa di memoria di default. Solo il codice utente non privilegiato (`CONTROL.nPRIV = 1`) è confinato alle regioni abilitate.

## 3. Mappa della memoria e gestione delle regioni a runtime

Il Cortex-M3 è single-core ed esegue un solo task alla volta. Anche se in RAM possono coesistere fino a 4 programmi caricati nei 4 slot fisici (`0x20002000..0x20005FFF`), **non si tengono mai aperte 4 regioni contemporaneamente**, altrimenti un programma potrebbe leggere o corrompere gli slot degli altri.

Il kernel usa una **finestra mobile** aggiornata ad ogni context switch in `task_switch_context()`:

| Area di memoria (da `mps2_m3.ld`) | Indirizzi | Dimensione | Regioni MPU quando il task è attivo | Permessi utente | 
| ----- | ----- | ----- | ----- | ----- | 
| **FLASH** (kernel, `shell`, `idle`, `fs.img`) | `0x00000000..0x003FFFFF` | 4 MiB | **Regione 0** (fissa, sempre accesa) | `RO + Exec` (`AP=110`, `XN=0`) | 
| **Partizione `shell`** | `0x20000000..0x20000FFF` | 4 KiB | **Regione 1** = `shell`; **Regione 2** = spenta | `RW + XN` (`AP=011`, `XN=1`) | 
| **Partizione `idle`** | `0x20001000..0x20001FFF` | 4 KiB | **Regione 1** = `idle`; **Regione 2** = spenta | `RW + XN` (`AP=011`, `XN=1`) | 
| **Slot programmi `0..3`** | `0x20002000..0x20005FFF` | $4 \times 4$ KiB | **Regione 1** (4 KiB) + **Regione 2** (`SRD`) solo sullo slot attivo | Fette $0 \dots n-1$: `RO + Exec`    Fette $n \dots 7$: `RW + XN` | 
| **RAM kernel** (`.data`, `.bss`, stack kernel) | `0x20006000..0x203FFFFF` | Resto RAM | **Nessuna regione** | Vietata agli utenti (`MemManage`); accessibile al kernel via `PRIVDEFENA` | 

Le regioni dalla `3` alla `7` restano spente (`ENABLE = 0`).

### Layout interno di uno slot programma da 4 KiB (es. Slot 0)

In un programma caricato, il codice parte dal fondo dello slot (`slot_base + 0x0`), i dati seguono su un confine di 512 byte, e lo stack parte dalla cima dello slot (`slot_base + 0x1000`) crescendo verso il basso (come imposto da `task_program_create()`):

```
0x20002000                       0x20002400                       0x20003000
+---------------+---------------+---------------+---------------+----------+
| Fetta 0 (512B)| Fetta 1 (512B)| Fetta 2 (512B)| ...           | Fetta 7  |
| .text+.rodata | .text  | pad  | .data | .bss  | spazio libero |  <- SP   |
+---------------+---------------+---------------+---------------+----------+
|<------ REGIONE 2: RO + Exec ->| (fette 2..7 disabilitate via SRD = 1)
|<====================== REGIONE 1: 4 KiB RW + XN ========================>|

```

### Perché il codice sta in Regione 2 anche se fisicamente è "in basso"

Il numero di regione non indica la posizione fisica in RAM, ma la **priorità hardware**:

* La **Regione 1** funge da strato di sfondo `RW + XN` sull'intero slot da 4 KiB (tutte le 8 fette attive, `SRD = 0x00`).

* La **Regione 2** viene sovrapposta esattamente sugli stessi 4 KiB con permessi `RO + Exec`, ma con `SRD` impostato per lasciare attive (`0`) solo le prime $n$ fette di codice e spegnere (`1`) le restanti $8 - n$ fette.

* Poiché **Regione 2 > Regione 1**, sulle fette $0 \dots n-1$ prevale la Regione 2 (`RO + Exec`); sulle fette $n \dots 7$ la Regione 2 è spenta dalla maschera `SRD`, quindi l'accesso ricade automaticamente sulla Regione 1 (`RW + XN`).

Se i numeri fossero invertiti, la regione `RW + XN` di indice superiore sovrascriverebbe la regione codice rendendo l'intero slot non eseguibile.

## 4. Modifiche a `kernel/mpu.h` e `kernel/mpu.c`

Prima di modificare i file è stato verificato `include/cmsdk.h`: i registri `MPU->TYPE`, `CTRL`, `RNR`, `RBAR`, `RASR` e le macro di permesso sono già definiti. In particolare `MPU_RASR_SIZE(n)` è definita come `(((n) - 1UL) << 1)`, quindi accetta direttamente l'esponente $n = \log_2(\text{size})$ (ad esempio `12` per 4 KiB) e non $n - 1$. `include/cmsdk.h` non è stato modificato.

### 4.1 Dichiarazione aggiunta in `kernel/mpu.h`

È stato aggiunto il prototipo pubblico della nuova funzione per i programmi caricati:

```
void mpu_set_program_regions( uint32_t slot_base, uint32_t slot_size, uint32_t code_size );

```

### 4.2 `mpu_init()` (invariata)

Chiamata una sola volta da `main()` prima di `svc #5`: configura la Regione 0 sulla FLASH (`RBAR = 0x00000010`, `RASR = 0x0602002B`), abilita la MPU con `MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA` (`0x00000005`) ed esegue le barriere `dsb` e `isb`.

### 4.3 `mpu_disable_region(uint32_t region)` *(nuova, interna a `kernel/mpu.c`)*

```
static void mpu_disable_region( uint32_t region )
{
    MPU->RNR  = region;
    MPU->RASR = 0u;
}

```

* **Scopo:** spegnere selettivamente una regione MPU (in particolare la Regione 2) quando lo scheduler passa da un programma caricato a `shell` o `idle`.

* **Motivazione:** la MPU usa un'unica coppia di registri finestra (`RBAR` e `RASR`) indicizzata da `MPU->RNR`. Scrivendo il numero del banco in `RNR` e `0u` in `RASR` si azzera il bit `ENABLE` (bit 0) senza dover configurare un indirizzo base. Se non spegnessimo la Regione 2 al ritorno alla `shell`, quest'ultima erediterebbe l'accesso in lettura/esecuzione allo slot del programma precedente.

### 4.4 `mpu_set_task_region(uint32_t base, uint32_t size, int executable)` *(aggiornata)*

Usata per i task statici (`shell` e `idle`). Mantiene intatta la configurazione della Regione 1 sulla partizione del task, ma aggiunge `mpu_disable_region(2);` prima delle barriere `dsb` e `isb`, garantendo che la Regione 2 sia sempre disattivata quando gira un task statico.

### 4.5 `mpu_set_program_regions(uint32_t slot_base, uint32_t slot_size, uint32_t code_size)` *(nuova)*

```
void mpu_set_program_regions( uint32_t slot_base, uint32_t slot_size, uint32_t code_size )
{
    uint32_t log2_size = __builtin_ctz( slot_size );

    /* Regione 1: intero slot da 4 KiB -> Read/Write, Non Eseguibile (XN=1) */
    MPU->RBAR = slot_base | MPU_RBAR_VALID | 1;
    MPU->RASR = MPU_RASR_XN
              | MPU_RASR_AP( MPU_AP_FULL_RW )
              | MPU_RASR_S | MPU_RASR_C
              | MPU_RASR_SIZE( log2_size ) | MPU_RASR_ENABLE;

    uint32_t sub_size = slot_size / 8; /* 8 sub-regioni: 4096 / 8 = 512 byte */
    if ( code_size == 0 || code_size >= slot_size )
    {
        mpu_disable_region( 2u );
        __asm volatile ( "dsb" ::: "memory" );
        __asm volatile ( "isb" ::: "memory" );
        return;
    }

    uint32_t n = ( code_size + sub_size - 1u ) / sub_size;
    if ( n == 0 || n > 7 )
    {
        mpu_disable_region( 2 );
        __asm volatile ( "dsb" ::: "memory" );
        __asm volatile ( "isb" ::: "memory" );
        return;
    }

    uint32_t srd_mask = ~(( 1u << n ) - 1u ) & 0xFFu;

    /* Regione 2: stesso slot -> Read-Only, Eseguibile (XN=0), attiva solo sulle prime n fette */
    MPU->RBAR = slot_base | MPU_RBAR_VALID | 2;
    MPU->RASR = MPU_RASR_AP( MPU_AP_RO ) | MPU_RASR_S
              | MPU_RASR_C | ( srd_mask << 8 )
              | MPU_RASR_SIZE( log2_size ) | MPU_RASR_ENABLE;

    __asm volatile ( "dsb" ::: "memory" );
    __asm volatile ( "isb" ::: "memory" );
}

```

#### Spiegazione dei passaggi e scelte progettuali

1. **Calcolo di `log2_size`:** `__builtin_ctz(slot_size)` conta gli zeri finali in binario, che per una potenza di 2 coincide con $\log_2(\text{slot\_size})$. Con `slot_size = 4096` ($2^{12}$), ottiene `12`.

2. **Calcolo di** $n$ **(divisione intera per eccesso):** ogni programma ELF ha una lunghezza di `.text` diversa (nei 9 programmi del test kit va da 752 a 872 byte, fino a un massimo di 3072 byte). La formula:
   

   $$
   n = \left\lceil \frac{\text{code\_size}}{\text{sub\_size}} \right\rceil = \frac{\text{code\_size} + 511}{512}
   $$

   
   calcola quante fette da 512 byte servono per coprire completamente `.text`, funzionando sia quando `code_size` non è multiplo di 512 (es. $756 \to 2$ fette) sia quando lo è già (es. $1024 \to 2$ fette).

3. **Controlli difensivi (`code_size == 0 || code_size >= slot_size` e `n == 0 || n > 7`):**

   * Il primo controllo evita anche un possibile overflow aritmetico in `code_size + sub_size - 1u` se arrivasse un valore corrotto.

   * Il limite $n \le 7$ garantisce che almeno l'ultima fetta in cima allo slot (Fetta 7, dove parte lo stack) resti sempre scrivibile e non eseguibile. In caso di parametri anomali la Regione 2 viene spenta per sicurezza.

4. **Costruzione della maschera `srd_mask`:**
   Per attivare le prime $n$ sub-regioni (bit `0..n-1` a **`0`**) e disabilitare le rimanenti (bit `n..7` a **`1`**):

   * `(1u << n) - 1u` genera $n$ bit a `1` a destra (l'uso di `1u` unsigned evita undefined behavior sugli shift in C).

   * `~(...)` inverte tutti i 32 bit della parola.

   * **`& 0xFFu` è fondamentale:** azzera i 24 bit alti (che altrimenti varrebbero tutti `1` dopo `~`). Senza `& 0xFFu`, lo shift `srd_mask << 8` andrebbe a sporcare i bit `31:16` di `MPU->RASR`, accendendo per errore anche il bit 28 (`XN = 1`) e rendendo la Regione 2 non eseguibile.

   *Esempio con `code_size = 756` (*$n = 2$ *fette):*

   * `(1u << 2) - 1u` = `0x00000003` (`0000 0011`)

   * `~0x00000003 & 0xFFu` = `0x000000FC` (`1111 1100`: fette 0 e 1 attive, fette `2..7` spente)

   * `srd_mask << 8` = `0x0000FC00` (posizionato nei bit `15:8` di `RASR`).

5. **Barriere architetturali (`dsb` e `isb`):**

   * `dsb` (*Data Synchronization Barrier*): forza il completamento hardware di tutte le scritture nei registri MPU prima di proseguire.

   * `isb` (*Instruction Synchronization Barrier*): svuota la pipeline della CPU, costringendola a prelevare di nuovo le istruzioni successive sotto le nuove regole MPU.

   * `::: "memory"`: vieta al compilatore GCC di riordinare accessi in memoria oltre la barriera.

#### Mappa dei bit scritti nei registri MPU (esempio Slot 0 = `0x20002000`, $n = 2$)

| Registro | Bit | Campo / Macro | Regione 1 (Dati/Stack) | Regione 2 (Codice) | Ruolo | 
| ----- | ----- | ----- | ----- | ----- | ----- | 
| `MPU->RBAR` | `31:5` | `ADDR` | `0x20002000` | `0x20002000` | Indirizzo base dello slot (allineato a 4 KiB) | 
| `MPU->RBAR` | `4` | `MPU_RBAR_VALID` | `1` (`0x10`) | `1` (`0x10`) | Usa il campo `REGION` in `RBAR` evitando `RNR` | 
| `MPU->RBAR` | `3:0` | `REGION` | `1` | `2` | Indice del banco MPU (`2` prevale su `1`) | 
| `MPU->RASR` | `28` | `MPU_RASR_XN` | `1` (`0x10000000`) | `0` (`0x00000000`) | Vieta (`1`) o permette (`0`) il fetch di istruzioni | 
| `MPU->RASR` | `26:24` | `MPU_RASR_AP` | `0b011` (`MPU_AP_FULL_RW`) | `0b110` (`MPU_AP_RO`) | RW per tutti (dati/stack) vs RO per tutti (codice) | 
| `MPU->RASR` | `18:17` | `S`, `C` | `1`, `1` (`0x00060000`) | `1`, `1` (`0x00060000`) | Attributi standard di memoria SRAM | 
| `MPU->RASR` | `15:8` | `SRD` | `0x00` | `0xFC` (`0x0000FC00`) | Sub-regioni disabilitate (`0` attiva, `1` spenta) | 
| `MPU->RASR` | `5:1` | `MPU_RASR_SIZE(12)` | `11` (`0x16`) | `11` (`0x16`) | Taglia $2^{11+1} = 4096$ byte (4 KiB) | 
| `MPU->RASR` | `0` | `MPU_RASR_ENABLE` | `1` (`0x01`) | `1` (`0x01`) | Attiva la regione nei controlli hardware | 
| **Totale `RASR`** |  |  | **`0x13060017`** | **`0x0606FC17`** | *(Con* $n = 1$*, Regione 2 = `0x0606FE17`)* | 

## 5. Collegamento allo scheduler (`kernel/task.h` e `kernel/task.c`)

La configurazione W^X non viene chiamata una volta sola dal Loader, ma **ad ogni context switch** da `task_switch_context()`. In questo modo, quando il SysTick interrompe un programma per dare la CPU alla `shell`, a `idle` o a un secondo programma in un altro slot, i permessi MPU si spostano e si adattano automaticamente al task che sta per eseguire.

### 5.1 Modifiche in `kernel/task.h`

1. **Nuovo campo `uint32_t code_size;` in `struct tcb`:**

   * Aggiunto **in fondo** alla struttura, dopo `uint8_t executable;`.

   * **Vincolo critico:** il primo campo `uint32_t *sp;` non deve mai essere spostato dall'offset `0`, perché `PendSV_Handler` (`kernel/pendsv.s`) lo legge e scrive direttamente in assembly senza conoscere il layout C della struct.

   * **Scopo:** memorizzare la dimensione di `.text` propria di quel task. Ogni programma nei 4 slot ha il proprio `struct tcb` in `tasks[1..4]` con il proprio `part_start` e il proprio `code_size`: al context switch la MPU viene riprogrammata usando i dati del TCB del task scelto, quindi non c'è mai confusione tra programmi diversi.

2. **Nuovo prototipo:**

   ```
   void task_program_set_code_size( unsigned slot, uint32_t code_size );
   
   ```

### 5.2 Modifiche in `kernel/task.c`

1. **Azzeramento sistematico `code_size = 0` in tre punti:**

   * in `task_configure_shell()` (il codice della `shell` sta in FLASH, non in RAM);

   * in `task_program_create()` (stato iniziale pulito del task appena creato);

   * in `complete_program()` (quando un programma termina normalmente o per fault e libera lo slot).

   * **Motivazione:** gli elementi `tasks[1..4]` sono statici e vengono riutilizzati a ogni nuova esecuzione. Azzerare `code_size` impedisce che un nuovo programma caricato sullo stesso slot erediti il valore del programma precedente.

2. **Implementazione di `task_program_set_code_size(unsigned slot, uint32_t code_size)`:**

   ```
   void task_program_set_code_size( unsigned slot, uint32_t code_size )
   {
       if ( slot < PROGRAM_SLOT_COUNT )
       {
           tasks[PROGRAM_TASK_FIRST + slot].code_size = code_size;
       }
   }
   
   ```

   Espone al Loader un'API sicura (con controllo di indice `slot < PROGRAM_SLOT_COUNT`) per registrare `code_size` nel TCB corretto.

3. **Selezione dinamica in `task_switch_context()`:**
   Al posto della chiamata fissa a `mpu_set_task_region()`, lo scheduler ora sceglie la politica MPU in base al TCB del task `next`:

   ```
   if ( next->executable && next->code_size > 0 )
   {
       mpu_set_program_regions( next->part_start,
                                next->part_end - next->part_start,
                                next->code_size );
   }
   else
   {
       mpu_set_task_region( next->part_start,
                            next->part_end - next->part_start,
                            next->executable );
   }
   
   ```

   * Per un programma caricato (`executable == 1` e `code_size > 0`), attiva il W^X su Regione 1 + Regione 2.

   * Per `shell` e `idle` (`code_size == 0`), chiama `mpu_set_task_region()`, che imposta Regione 1 sulla loro partizione e spegne Regione 2.

## 6. Gestione dei fault (`kernel/faults.c`)

`kernel/faults.c` è stato verificato e **non richiede modifiche**: `fault_dispatch()` gestisce già completamente i fault provenienti da programmi caricati su PSP:

* verifica che l'eccezione provenga da un programma caricato su PSP senza errori di stacking/unstacking;

* stampa il formato richiesto dal test kit: `MemManage in loaded program: CFSR=0x...` seguito da `address=0x...` **solo** se il bit `SCB_CFSR_MMARVALID` è attivo (necessario per evitare indirizzi obsoleti in caso di `IACCVIOL`);

* azzera i bit di stato di `SCB->CFSR` tramite scrittura diretta W1C (`SCB->CFSR = cfsr;`);

* chiama `task_fault_current()`, che stampa `program <nome> faulted`, libera lo slot (`TASK_UNUSED`, azzera `code_size`), risveglia la `shell` in attesa con stato `-2` e richiede un context switch.

## 7. Verifiche eseguite

1. **Compilazione pulita:** `make` compila l'intero firmware senza errori né warning (`-Wall -Wextra`).

2. **Funzionamento della shell e dei comandi:** `make run` avvia correttamente la shell; `ls` elenca tutti e 9 i programmi di `build/fs.img` e `run <nome>` restituisce il controllo senza fault.

3. **Ispezione hardware dei registri MPU in GDB:**

   * Fermandosi in `shell_main` (Thread mode non privilegiato), la lettura di `0xE000ED9C` restituisce `Cannot access memory`, confermando che lo System Control Space è protetto in modalità utente.

   * Fermandosi in Handler mode (privilegiato) passo passo in `mpu_init()` e `mpu_set_task_region()`, i registri hardware misurati su QEMU corrispondono esattamente al bit ai valori teorici:

| Punto di verifica in GDB | Registro letto | Indirizzo | Valore osservato | Significato verificato | 
| ----- | ----- | ----- | ----- | ----- | 
| Fine `mpu_init()` | `MPU->CTRL` | `0xE000ED94` | `0x00000005` | `ENABLE = 1`, `PRIVDEFENA = 1` | 
| Fine `mpu_init()` | `MPU->RBAR` (R0) | `0xE000ED9C` | `0x00000000` | Regione 0, base FLASH `0x0` | 
| `mpu_set_task_region()` prima di `mpu_disable_region(2)` | `MPU->RBAR` / `RASR` (R1) | `0xE000ED9C..A0` | `0x20000001` / `0x13060017` | Regione 1 su `shell` (`0x20000000`), 4 KiB, `RW + XN` | 
| `mpu_set_task_region()` dopo `mpu_disable_region(2)` | `MPU->RBAR` / `RASR` (R2) | `0xE000ED9C..A0` | `0x00000002` / `0x00000000` | Regione 2 selezionata e spenta (`ENABLE = 0`) | 

## 8. Guida all'integrazione per il resto del gruppo

Perché il W^X funzioni end-to-end appena il Loader ELF sarà collegato alla SVC 13, servono **due soli punti di integrazione** lato linker dei programmi e lato Loader:

### 8.1 Allineamento in `programs/prog.ld` (obbligatorio)

Attualmente in `programs/prog.ld` la sezione `.text` termina con `. = ALIGN(4);`. Poiché una fetta `SRD` è larga **512 byte (`0x200`)**, con `ALIGN(4)` la sezione `.data` inizia subito dopo l'ultimo byte di `.text` (es. a offset `0x2f4`), finendo **nella stessa sub-regione da 512 byte del codice**. In quella situazione la MPU non può fisicamente assegnare permessi diversi a `.text` e `.data`.

**Modifica richiesta in `programs/prog.ld`:** sostituire `. = ALIGN(4);` alla fine di `.text` con `. = ALIGN(512);`:

```
ENTRY(_start)
SECTIONS
{
  . = 0;
  .text : { *(.text._start) *(.text*) *(.rodata*) . = ALIGN(512); }
  .data : { *(.data*) . = ALIGN(4); }
  .bss  : { *(.bss*) *(COMMON) . = ALIGN(4); }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }
}

```

Dopo la modifica, ricompilare con `make programs && make image` e verificare con `arm-none-eabi-readelf -S build/programs/wx_exec_data.elf` che l'indirizzo (`Addr`) della sezione `.data` sia un multiplo esatto di `0x200` (es. `0x00000400`).

### 8.2 Chiamata dal Loader dentro `sys_program_run_impl` (SVC 13)

Una volta validato, copiato e rilocato l'ELF nello slot libero `slot`, il Loader deve:

1. Creare il task con `int id = task_program_create( slot, name, entry, stack_top, 1 );` (passando `executable = 1`).

2. **Subito dopo** (e solo se `id >= 0`), registrare la dimensione del codice chiamando:

   ```
   task_program_set_code_size( slot, code_size );
   
   ```

   dove `code_size` è la dimensione in byte della sezione `.text` letta dallo section header dell'ELF.

**Attenzione all'ordine:** `task_program_set_code_size()` deve essere chiamata **dopo** `task_program_create()`, perché `task_program_create()` inizializza `t->code_size = 0`. L'operazione è priva di race condition perché avviene interamente dentro `SVCall` (priorità `0x80`), che non può essere interrotta da `SysTick` o `PendSV` (priorità `0xE0`).

*Nota per il Loader sui permessi di scrittura:* mentre il Loader copia e riloca l'ELF nello slot, il task corrente è ancora la `shell` (quindi Regione 2 è spenta) e la CPU gira in modalità privilegiata (`PRIVDEFENA = 1`): il kernel può scrivere nello slot liberamente. La Regione 2 (`RO + Exec`) viene attivata solo al context switch successivo, quando lo scheduler cede la CPU al programma caricato.

### 8.3 Checklist test di sicurezza post-integrazione

Una volta uniti `prog.ld` e Loader, lanciare i tre test di memoria da `scratch>`:

* `run kernel_store` -> `MemManage in loaded program: CFSR=0x82 address=...` + `program kernel_store faulted` (accesso fuori slot bloccato).

* `run wx_write_code` -> `MemManage in loaded program: CFSR=0x82 address=0x20002...` + `program wx_write_code faulted` (`DACCVIOL + MMARVALID`: scrittura su Regione 2 Read-Only bloccata).

* `run wx_exec_data` -> `MemManage in loaded program: CFSR=0x1` + `program wx_exec_data faulted` (`IACCVIOL`: fetch su Regione 1 con `XN = 1` bloccato).