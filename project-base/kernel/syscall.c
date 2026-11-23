/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * System calls, kernel side: find which service `svc #n` asked for,
 * run it, and hand the result back to the caller.
 *
 * Everything here runs inside the SVCall exception, so it is
 * privileged: it may touch the drivers and the hardware registers.
 *
 * One service does not return to its caller: SYS_START_TASKS (main()
 * starts the first task). It enters a task instead, so svc_dispatch()
 * returns the EXC_RETURN value that ends the exception (see
 * kernel/svc_entry.s).
 *
 * Since week 5, some services give the CPU away: yield, delay,
 * delay_until and exit. They do not switch here. They only change the
 * task's state and make PendSV pending (task_request_switch()). PendSV
 * has a lower priority than SVCall (0xE0 against 0x80), so it waits:
 * it runs right after this exception ends, and switches then. That is
 * why exit can now be an ordinary entry of the table: by the time the
 * task would resume after its `svc`, PendSV has already switched away,
 * and a FINISHED task is never chosen again.
 */

#include <stdint.h>
#include "kernel/syscall.h"
#include "lib/syscalls.h"
#include "drivers/uart.h"
#include "drivers/systick.h"
#include "kernel/task.h"
#include "kernel/trace.h"
#include "kernel/loader.h"

extern uint32_t __fs_image_start;

struct img_header {
    uint32_t magic;
    uint32_t version;
    uint32_t image_size;
    uint32_t count;
    uint32_t directory_offset;
};

struct img_dirent {
    char name[16];
    uint32_t offset;
    uint32_t size;
};

/* Every service has the same shape: up to three arguments (the
 * caller's r0, r1, r2) and one result (which becomes the caller's r0).
 * A service that needs fewer arguments simply ignores the others.
 * syscall_fn is the type "pointer to such a function". */
typedef uint32_t ( *syscall_fn )( uint32_t a0, uint32_t a1, uint32_t a2 );

/* Is [p, p + len) fully inside [start, end)?
 * Written as `len <= end - p` so that p + len can never wrap around. */
static int in_range( uint32_t p, uint32_t len, uint32_t start, uint32_t end )
{
    return p >= start && p <= end && len <= end - p;
}

/* SYS_WRITE: a0 = buf, a1 = len. Put as many bytes as fit into the
 * UART send buffer and return how many were taken.
 * It must never wait for room. We are inside the SVCall handler
 * (priority 0x80), and the UART send interrupt that empties the buffer
 * has a less urgent priority (0xC0): it cannot run until we return. If
 * we waited here for room, nothing would ever make room, and the
 * system would freeze. So we return the count, possibly 0, and the
 * caller tries again later. */
static uint32_t sys_write_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    (void) a2;                                  /* not used */
    const char *buf = (const char *) a0;      /* the number is really an address */
    unsigned len = a1;

    /* A task must not make the kernel read memory the task itself may
     * not read. Accept the buffer only inside its own partition or
     * inside FLASH (string constants). The kernel itself is trusted. */
    const struct tcb *t = tcb_current;
    if ( t != 0
         && !in_range( a0, len, t->part_start, t->part_end )
         && !in_range( a0, len, 0x00000000, 0x00400000 ) )
        return (uint32_t) -1;

    return uart_write( buf, len );              /* bytes accepted, 0..len */
}

/* SYS_READ: no arguments. Return one received byte, or -1 if none. */
static uint32_t sys_read_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    (void) a0;                                  /* not used */
    (void) a1;
    (void) a2;

    /* -1 becomes 0xFFFFFFFF here; the caller's `int` turns it back into -1. */
    return (uint32_t) uart_getc_polled();
}

/* SYS_UPTIME: no arguments. Return the milliseconds since start. */
static uint32_t sys_uptime_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    (void) a0;                                  /* not used */
    (void) a1;
    (void) a2;
    return systick_uptime_ms();
}

/* The scheduler services below only make sense for a task. Before the
 * first task starts (tcb_current == 0), they answer -1. */

/* SYS_YIELD: no arguments. The calling task gives up the rest of its
 * time slice: it stays READY, and the next READY task runs. If no other
 * task is READY, the scheduler picks the caller again. */
static uint32_t sys_yield_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    (void) a0;                                  /* not used */
    (void) a1;
    (void) a2;
    if ( tcb_current == 0 )
        return (uint32_t) -1;
    task_request_switch();                      /* PendSV runs after this exception */
    return 0;
}

/* SYS_DELAY: a0 = ticks. Block the caller for that many ticks, counted
 * from now. A delay of 0 is just a yield. */
static uint32_t sys_delay_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    (void) a1;                                  /* not used */
    (void) a2;
    if ( tcb_current == 0 )
        return (uint32_t) -1;
    if ( a0 == 0 )
        task_request_switch();                  /* delay(0) = yield */
    else
        task_block_until( tick_count + a0 );    /* READY again at tick now + a0 */
    return 0;
}

/* SYS_DELAY_UNTIL: a0 = last_wake (an address in the task's
 * partition), a1 = period. Block the caller until tick *last_wake +
 * period, and store that tick in *last_wake for the next call. Unlike
 * SYS_DELAY, the time the task spent working does not push the next
 * wake-up later: the task wakes at last_wake + period, + 2 * period,
 * ... exactly, which is what a periodic task needs. */
static uint32_t sys_delay_until_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    (void) a2;                                  /* not used */
    const struct tcb *t = tcb_current;
    if ( t == 0 )
        return (uint32_t) -1;

    /* The kernel is going to read and WRITE *last_wake. As for
     * SYS_WRITE, accept the address only inside the caller's own
     * partition, or a task could make the kernel write anywhere. It
     * must also be a multiple of 4, as every uint32_t is. */
    if ( !in_range( a0, sizeof( uint32_t ), t->part_start, t->part_end )
         || ( a0 & 3 ) != 0 )
        return (uint32_t) -1;

    uint32_t *last_wake = (uint32_t *) a0;    /* the number is really an address */
    uint32_t wake = *last_wake + a1;            /* the next wake-up tick */
    *last_wake = wake;                          /* remembered for the next period */

    /* Is wake still in the future? Same wrap-safe test as in
     * task_increment_tick(). If the task is late (wake already passed),
     * it does not block at all: it goes on at once. */
    if ( (int32_t) ( wake - tick_count ) > 0 )
        task_block_until( wake );
    return 0;
}

/* SYS_EXIT: no arguments. The calling task is finished: it is marked
 * FINISHED and never chosen again. The result is never seen. */
static uint32_t sys_exit_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    (void) a0;                                  /* not used */
    (void) a1;
    (void) a2;
    if ( tcb_current == 0 )
        return (uint32_t) -1;
    task_finish_current();                      /* PendSV switches away after this exception */
    return 0;
}

/* SYS_TICK_COUNT: no arguments. Return the ticks since the scheduler
 * started (one tick = 10 ms). */
static uint32_t sys_tick_count_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    (void) a0;                                  /* not used */
    (void) a1;
    (void) a2;
    return tick_count;
}

/* SYS_TRACE_DUMP: no arguments. Print the scheduler trace and its
 * Gantt chart on the UART (kernel/trace.c). */
static uint32_t sys_trace_dump_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    (void) a0;                                  /* not used */
    (void) a1;
    (void) a2;
    trace_print();
    return 0;
}

static int image_valid( void );

/* Wait bookkeeping is supplied even though the untouched base has no loader.
 * Student loader code supplies SYS_PROGRAM_RUN and creates program tasks. */
static uint32_t sys_program_wait_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    (void) a1;
    (void) a2;
    uint32_t *frame;
    __asm volatile( "mrs %0, psp" : "=r"( frame ) );
    return (uint32_t) task_wait_program( a0, &frame[0] );
}

static uint32_t image_header_count( void )
{
    struct img_header *img = (struct img_header *) &__fs_image_start;

    /* 1. Controllo Magic Number (deve essere "IMG1") */
    if ( img->magic != 0x31474d49 )
    {
        return (uint32_t) -1;
    }

    /* 2. Controllo Versione (deve essere 1) */
    if ( img->version != 1 )
    {
        return (uint32_t) -1;
    }

    /* 3. Controllo Count (tra 1 e 32 programmi) */
    if ( img->count < 1 || img->count > 32 )
    {
        return (uint32_t) -1;
    }

    /* Controllo Dimensione Immagine: 
     * Minimo 20 byte (la grandezza dell'header stesso),
     * Massimo 0x00200000 (i 2 MiB fisici disponibili in Flash). */
    if ( img->image_size < 20 || img->image_size > 0x00200000 )
    {
        return (uint32_t) -1;
    }

    /* Controllo Offset della Directory:
     * Minimo 20 (deve partire dopo l'header),
     * Multiplo di 4 (il resto della divisione per 4 deve essere 0 per l'allineamento). */
    if ( img->directory_offset < 20 || ( img->directory_offset % 4 ) != 0 )
    {
        return (uint32_t) -1;
    }

    /* Controllo Limiti e Prevenzione Overflow:
     * Calcoliamo dove finisce la directory (inizio + numero di file * 24 byte). */
    uint32_t directory_end = img->directory_offset + ( img->count * 24 );

    /* Usiamo l'OR logico (||) invece del bitwise OR (|).
     * 1° check: Se directory_end è minore della partenza, c'è stato un integer overflow.
     * 2° check: Se la fine sfora la dimensione totale dell'immagine dichiarata, è un errore. */
    if ( directory_end < img->directory_offset || directory_end > img->image_size )
    {
        return (uint32_t) -1;
    }

    /* Se arriviamo qui, l'immagine è immacolata. Restituiamo il numero di file. */
    return img->count;
}

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

static uint32_t sys_image_name_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    //a0 indice
    //a1 puntatore al buffer
    //a2 dimensione del buffer

    /* 1. Controllo validità immagine e indice */
    uint32_t count = sys_image_count_impl( 0, 0, 0 );
    if ( count == (uint32_t) -1 || a0 >= count )
    {
        return (uint32_t) -1;
    }

    /* 2. Controllo capacità del buffer richiesta dalle specifiche */
    if ( a2 < 1 || a2 > 16 )
    {
        return (uint32_t) -1;
    }

    /* 3. Pointer Guard: Sicurezza della memoria utente */
    const struct tcb *t = tcb_current;
    if ( t != 0 && !in_range( a1, a2, t->part_start, t->part_end ) )
    {
        return (uint32_t) -1;
    }

    /* 4. Calcolo dell'indirizzo fisico in Flash */
    struct img_header *img = (struct img_header *) &__fs_image_start;

    char *flash_name = (char *) ( (uint32_t) img + img->directory_offset + ( a0 * 24 ) );
    char *dest_buffer = (char *) a1;

    /* 5. Copia dei caratteri */
    for ( uint32_t i = 0; i < a2; i++ )
    {
        dest_buffer[i] = flash_name[i];

        if ( flash_name[i] == '\0' )
        {
            return 0; /* Successo: fine della stringa raggiunta */
        }
    }

    /* 6. Troncamento se il buffer è troppo piccolo */
    dest_buffer[a2 - 1] = '\0';
    return (uint32_t) -1;
}

#define SLOT_SIZE 4096u /*ogni slot: 4KB */
#define PROGRAM_MAX 3072u /*programma: massimo 3Kb, il resto è stack */
#define FLASH_LIMIT 0x00400000u /*la flash va da 0 a questo indirizzo */
#define LOADER_EXECUTABLE 1 /*flag executable per la MPU */

/*inizio dei 4 slot di ram (0x20002000), il simbolo lo esporta il linker script */
extern uint32_t __program_slots_start[];

/*controlla se il chiamante (kernel) può leggere i byte nella partizione [p, p + len ) */
static int caller_can_read( uint32_t p, uint32_t len )
{
    const struct tcb *t = tcb_current;

    return ( t != 0 && in_range( p, len, t->part_start, t->part_end ) ) || in_range( p, len, 0x00000000u, FLASH_LIMIT );
}

/*copia nel nel buffer dst del kernel il nome del programma all'indirizzo a0*/
static int copy_name( uint32_t a0, char dst[16] )
{
    const char *src = (const char *) a0;

    for ( uint32_t i = 0; i < 16; i++ )
    {
        if ( !caller_can_read( a0, i + 1 ) )
            return -1;
        dst[i] = src[i];
        if ( dst[i] == '\0' )  /*controlla se un carattere è NULL*/
            return i == 0 ? -1 : 0; /*se il primo carattere è NULL, restituisci -1, altrimenti 0*/
    }
    return -1;  /*ritorna -1 se non c'è nessun valore NULL nei primi 16 byte*/
}

static const struct img_dirent *img_entry( uint32_t i )
{
    const struct img_header *h = (const struct img_header *) &__fs_image_start;

    return (const struct img_dirent *) ( (uint32_t) h + h->directory_offset + i * 24u );
}

static int name_char_ok( char c )
{
    return ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-';
}

/*confronta i nomi inseriti un carattere alla volta*/
static int same_name( const char *a, const char *b )
{
    for ( int k = 0; k < 16; k++ )
    {
        if ( a[k] != b[k] )
            return 0;
        if ( a[k] == '\0' )
            return 1;
    }
    return 1;
}

static int image_valid( void )
{
 /*l'intestazione dell'immagine viene già controllata da svc 11*/
    uint32_t count = image_header_count();
    if ( count == (uint32_t) -1 )
        return 0;

    const struct img_header *h = (const struct img_header *) &__fs_image_start;
    uint32_t dir_end = h->directory_offset + count * 24u; /*non c'è overflow, già controllato in svc 11*/
    uint32_t contents_start = ( dir_end + 3u ) & ~3u; /*fine directory, arrotondata a multiplo di 4*/
    for ( uint32_t i = 0; i < count; i++ )
    {
        const struct img_dirent *e = img_entry( i );
  /*controllo del nome*/
        uint32_t n = 0;
        while ( n < 16 && e->name[n] != '\0' )
        {
            if ( !name_char_ok( e->name[n] ) )
                return 0;
            n++;
        }
        if ( n == 0 || n == 16 )
            return 0;

  /*offset multiplo di 4, dimensione almeno 40 */
        if ( ( e->offset & 3u ) != 0 || e->size < 40u )
            return 0;

  /*il file sta dopo la directory e dentro l'immagine (senza overflow)*/
        if ( e->offset < contents_start )
            return 0;
        if ( e->offset > h->image_size || e->size > h->image_size - e->offset )
            return 0;

  /*confronto della voce e con ogni voce precedente, se c'è sovrapposizione ritorna 0 */
        for ( uint32_t j = 0; j < i; j++ )
        {
            const struct img_dirent *o = img_entry( j );

            if ( same_name( e->name, o->name ) )
                return 0;
            if ( e->offset < o->offset + o->size && o->offset < e->offset + e->size )
                return 0;
        }
    }
    return 1;
}

/*cerca nella directory la voce il cui nome è uguale a name. se non c'è restituisci -2 altrimenti tieni il puntatore della voce (found) e da lì ricaviamo offset e size del file ELF*/
/**/
static const struct img_dirent *find_entry( const char *name )
{
    const struct img_header *h = (const struct img_header *) &__fs_image_start;

    for ( uint32_t i = 0; i < h->count; i++ )
        if ( same_name( name, img_entry( i )->name ) )
            return img_entry( i );
    return 0;
}

/*trova il primo slot libero, restituisce -1 se sono tutti occupati */
/*PROGRAM_SLOT_COUNT si trova in kernel/task.h*/
static int first_free_slot( void )
{
    for ( unsigned s = 0; s < PROGRAM_SLOT_COUNT; s++ )
        if ( task_program_slot_free( s ) )
            return (int) s;
    return -1;
}

/*ricontrolla il piano di Gianluigi e Athena. legge i byte de file e controlla che stiano in [0, size), poi controlla che i byte da 0 a copy_size da copiare e quelli azzerati da bss_start a bss_end siano nei limiti di 3072. alla fine controlla che l'intervallo nel bss abbia senso*/
static int plan_inside_limits( const struct load_plan *p, uint32_t size )
{
    return p->file_off <= size && p->copy_size <= size - p->file_off && p->copy_size <= PROGRAM_MAX && p->bss_end <= PROGRAM_MAX && p->bss_start <= p->bss_end;
}

static uint32_t sys_program_run_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    //a0 = nome del file che vogliamo runnare
    (void) a1;
    (void) a2;
    /*copia il nome del programma, se è valido restituisce 0, con -1 abbiamo error: program cannot be loaded*/
    char name[16];
    if ( copy_name( a0, name ) != 0 )
        return (uint32_t) -1;
    /*se l'immagine non è valida, restituisci -1 (la shell non distingue i due casi*/
    if ( !image_valid() )
        return (uint32_t) -1;
    /*trovo l'entry dell'immagine (found) a partire dal name */
    const struct img_dirent *found = find_entry( name );
    if ( found == 0 )
        return (uint32_t) -2;
    /*scelgo il primo slot libero e imposto i suoi parametri*/
    int slot = first_free_slot();
    if ( slot < 0 )
        return (uint32_t) -3; /*error: no free slot*/

    /*slot_base è l'indirizzo dove inizia lo slot libero = indirizzo del primo slot + slot * 4096, spostandosi di 4096 byte per ogni slot*/
    uint32_t slot_base = (uint32_t) __program_slots_start + (uint32_t) slot * SLOT_SIZE;
    /*f punta all'elf direttamente in FLASH. prende il primo byte dell'immagine e avanza di offset byte*/
    const uint8_t *f = (const uint8_t *) &__fs_image_start + found->offset;
    uint32_t size = found->size; /*dimensione dell'immagine*/

     /*crea il piano nello staco del kernel e mette a zero tutti i campi */
     /*controlla i piani di Gianluigi e Athena*/
    struct load_plan plan = { 0 };
    if ( elf_check_layout( f, size, &plan ) != 0 )
        return (uint32_t) -4;
    if ( elf_check_relocs( f, size, &plan, slot_base ) != 0 )
        return (uint32_t) -4;
    if ( !plan_inside_limits( &plan, size ) )
        return (uint32_t) -4;

     /*questi controlli sono necessari per la copia di dati e azzeramento bss*/
    uint8_t *dst = (uint8_t *) slot_base;
    for ( uint32_t i = 0; i < plan.copy_size; i++ )
        dst[i] = f[plan.file_off + i];
    for ( uint32_t i = plan.bss_start; i < plan.bss_end; i++ )
        dst[i] = 0;
    elf_apply_relocs( f, slot_base );
     /*creazione del task*/
     /*executable (1) è il flag che task_switch_context passa a mpu_set_task_region. a 1 la regione ram del task è eseguibile, a 0 no. il valore giusto di executable dipende dalla MPU che scrive Giulio*/
    int id = task_program_create( (unsigned) slot, name, ( void ( * )( void ) )( slot_base + plan.entry ), (uint32_t *) ( slot_base + SLOT_SIZE ), LOADER_EXECUTABLE );
    if ( id < 0 )
        return (uint32_t) -4;

    task_program_set_code_size( (unsigned) slot, plan.code_end );
    return (uint32_t) id;
}
/* The dispatch table: table[n] is the function for service n.
 * The [SYS_WRITE] = ... form puts each entry at its number, whatever
 * the order of the lines. Entry 0 is left empty (0): it is not a
 * service. Entry 5 (SYS_START_TASKS) is empty too: svc_dispatch
 * handles it itself, because it does not return to its caller. */
static const syscall_fn table[] = {
    [SYS_WRITE] = sys_write_impl,
    [SYS_READ] = sys_read_impl,
    [SYS_UPTIME] = sys_uptime_impl,
    [SYS_EXIT] = sys_exit_impl,
    [SYS_YIELD] = sys_yield_impl,
    [SYS_DELAY] = sys_delay_impl,
    [SYS_DELAY_UNTIL] = sys_delay_until_impl,
    [SYS_TRACE_DUMP] = sys_trace_dump_impl,
    [SYS_TICK_COUNT] = sys_tick_count_impl,
    [SYS_PROGRAM_WAIT] = sys_program_wait_impl,
    [SYS_IMAGE_COUNT] = sys_image_count_impl,
    [SYS_IMAGE_NAME] = sys_image_name_impl,
    [SYS_PROGRAM_RUN] = sys_program_run_impl,
};

/* How many entries the table has (here 11: numbers 0..10). */
#define TABLE_SIZE ( sizeof( table ) / sizeof( table[0] ) )

/* EXC_RETURN bit 2: 1 if the caller was on the process stack (a task),
 * 0 if it was on the main stack (main(), the kernel). */
#define EXC_RETURN_PSP_BIT 0x4u

/* frame points at the 8 registers the hardware pushed when `svc` ran:
 *     frame[0] = r0     frame[4] = r12
 *     frame[1] = r1     frame[5] = lr
 *     frame[2] = r2     frame[6] = pc   (the instruction after `svc`)
 *     frame[3] = r3     frame[7] = xPSR
 * exc_return is the EXC_RETURN value of this exception. The function
 * returns the EXC_RETURN value SVC_Handler must use to end it. */
uint32_t svc_dispatch( uint32_t *frame, uint32_t exc_return )
{
    /* Which service? The number is not in a register: it is written
     * inside the `svc #n` instruction itself. That instruction is 2
     * bytes long and sits just before the saved pc (frame[6]), and its
     * first byte in memory holds n. So we look at the byte 2 bytes
     * before the saved pc:
     *     pc - 2:  n       (low byte of `svc #n`)
     *     pc - 1:  0xDF    (high byte: "this is an svc")
     *     pc:      the next instruction  */
    uint8_t n = ( (uint8_t *) frame[6] )[-2];

    /* Did a task ask (process stack), or main() (main stack)? */
    int from_task = ( exc_return & EXC_RETURN_PSP_BIT ) != 0;

    /* SYS_START_TASKS: kernel only. A task (on the process stack) that
     * tries it falls through to the table below and gets -1. */
    if ( n == SYS_START_TASKS && !from_task && tcb_current == 0 )
    {
        return task_start_first();      /* enter the first task */
    }

    /* An unknown number, or 0, or an empty table entry: answer -1. */
    if ( n >= TABLE_SIZE || table[n] == 0 )
    {
        frame[0] = (uint32_t) -1;
        return exc_return;              /* back to the caller */
    }

    /* Run the service with the caller's r0, r1, r2 as arguments, and
     * store its result in the saved r0. When the exception ends, the
     * hardware pops the frame back into the registers, and the caller
     * finds the result in r0. */
    frame[0] = table[n]( frame[0], frame[1], frame[2] );
    return exc_return;                  /* back to the caller, as it came */
}
