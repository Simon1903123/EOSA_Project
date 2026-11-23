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

/* FLASH: 0x00000000..0x003fffff, readable by every task (assignment 6.4). */
#define FLASH_LIMIT 0x00400000u

/* IMG1 image (assignment 6.1). Every number of the format is defined here
 * once: the checks below use these names, never the bare numbers. */
#define IMG_MAGIC 0x31474d49u     /* bytes 49 4d 47 31 = "IMG1" */
#define IMG_VERSION 1u
#define IMG_HEADER_SIZE 20u       /* struct img_header */
#define IMG_ENTRY_SIZE 24u        /* struct img_dirent */
#define IMG_NAME_SIZE 16u         /* up to 15 characters + NUL */
#define IMG_MAX_COUNT 32u
#define IMG_MAX_SIZE 0x00200000u  /* 2 MiB: 0x00200000..0x003fffff */
#define IMG_MIN_FILE_SIZE 40u

#define LOADER_EXECUTABLE 1 /* flag executable per la MPU (W^X via code_size) */

/* Linker symbols (scripts/mps2_m3.ld): where QEMU puts fs.img, and the
 * first of the four program slots (0x20002000). */
extern uint32_t __fs_image_start;
extern uint32_t __program_slots_start[];

struct img_header {
    uint32_t magic;
    uint32_t version;
    uint32_t image_size;
    uint32_t count;
    uint32_t directory_offset;
};

struct img_dirent {
    char name[IMG_NAME_SIZE];
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

/* May the calling task make the kernel READ [p, p + len)? Only inside its
 * own partition or inside FLASH (string constants). Used by SVC 1 and 13. */
static int caller_can_read( uint32_t p, uint32_t len )
{
    const struct tcb *t = tcb_current;

    return ( t != 0 && in_range( p, len, t->part_start, t->part_end ) )
           || in_range( p, len, 0x00000000u, FLASH_LIMIT );
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
    if ( tcb_current != 0 && !caller_can_read( a0, len ) )
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

/* ======================================================================
 * The IMG1 image in FLASH (assignment 6.1). It is read where it is and
 * never written or copied.
 * ====================================================================== */

static const struct img_header *img_header( void )
{
    return (const struct img_header *) &__fs_image_start;
}

/* Directory entry i. Call it only after image_header_count() succeeded,
 * so that the directory is inside the image, and with i < count. */
static const struct img_dirent *img_entry( uint32_t i )
{
    const struct img_header *h = img_header();

    return (const struct img_dirent *) ( (uint32_t) h + h->directory_offset
                                         + i * IMG_ENTRY_SIZE );
}

/* Check the 20-byte header only. Return count (1..32) or -1. */
static uint32_t image_header_count( void )
{
    const struct img_header *h = img_header();

    if ( h->magic != IMG_MAGIC || h->version != IMG_VERSION )
        return (uint32_t) -1;
    if ( h->count < 1 || h->count > IMG_MAX_COUNT )
        return (uint32_t) -1;
    if ( h->image_size < IMG_HEADER_SIZE || h->image_size > IMG_MAX_SIZE )
        return (uint32_t) -1;
    if ( h->directory_offset < IMG_HEADER_SIZE || ( h->directory_offset % 4 ) != 0 )
        return (uint32_t) -1;

    /* directory_offset + count * 24 must not overflow (count <= 32, so
     * count * 24 cannot) and must not pass image_size. Written as a
     * subtraction, so the sum is never computed. */
    if ( h->directory_offset > h->image_size
         || h->count * IMG_ENTRY_SIZE > h->image_size - h->directory_offset )
        return (uint32_t) -1;

    return h->count;
}

static int name_char_ok( char c )
{
    return ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' )
           || c == '_' || c == '-';
}

/* Compare two names, at most IMG_NAME_SIZE bytes, up to the first NUL. */
static int same_name( const char *a, const char *b )
{
    for ( uint32_t k = 0; k < IMG_NAME_SIZE; k++ )
    {
        if ( a[k] != b[k] )
            return 0;
        if ( a[k] == '\0' )
            return 1;
    }
    return 1;
}

/* The whole image: header and every directory entry. 1 = valid, 0 = not.
 * If any rule is broken the whole image is invalid (6.1). */
static int image_valid( void )
{
    uint32_t count = image_header_count();
    if ( count == (uint32_t) -1 )
        return 0;

    const struct img_header *h = img_header();
    uint32_t dir_end = h->directory_offset + count * IMG_ENTRY_SIZE; /* checked above */
    uint32_t contents_start = ( dir_end + 3u ) & ~3u;                /* rounded up to 4 */

    for ( uint32_t i = 0; i < count; i++ )
    {
        const struct img_dirent *e = img_entry( i );

        /* Name: 1..15 allowed characters, then NUL. */
        uint32_t n = 0;
        while ( n < IMG_NAME_SIZE && e->name[n] != '\0' )
        {
            if ( !name_char_ok( e->name[n] ) )
                return 0;
            n++;
        }
        if ( n == 0 || n == IMG_NAME_SIZE )
            return 0;

        /* File: aligned, at least 40 bytes, after the directory, inside
         * the image (without computing offset + size). */
        if ( ( e->offset & 3u ) != 0 || e->size < IMG_MIN_FILE_SIZE )
            return 0;
        if ( e->offset < contents_start || e->offset > h->image_size
             || e->size > h->image_size - e->offset )
            return 0;

        /* No duplicate name and no overlap with an earlier entry. The sums
         * cannot overflow: both files are already known to be inside the
         * image, which is at most 2 MiB. */
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

/* The entry called name, or 0. Call it only on a valid image. */
static const struct img_dirent *find_entry( const char *name )
{
    uint32_t count = img_header()->count;

    for ( uint32_t i = 0; i < count; i++ )
        if ( same_name( name, img_entry( i )->name ) )
            return img_entry( i );
    return 0;
}

/* SYS_IMAGE_COUNT: number of directory entries, or -1 if the image is
 * invalid (header OR directory). */
static uint32_t sys_image_count_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    (void) a0;
    (void) a1;
    (void) a2;

    if ( !image_valid() )
        return (uint32_t) -1;
    return image_header_count();
}

/* SYS_IMAGE_NAME: a0 = index, a1 = destination, a2 = capacity (1..16).
 * Copies the complete name with its NUL and returns 0; if it does not fit,
 * copies the first part, still ending with NUL, and returns -1. */
static uint32_t sys_image_name_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    uint32_t count = sys_image_count_impl( 0, 0, 0 );
    if ( count == (uint32_t) -1 || a0 >= count )
        return (uint32_t) -1;

    if ( a2 < 1 || a2 > IMG_NAME_SIZE )
        return (uint32_t) -1;

    /* The kernel WRITES here: only the caller's own partition, never FLASH. */
    const struct tcb *t = tcb_current;
    if ( t != 0 && !in_range( a1, a2, t->part_start, t->part_end ) )
        return (uint32_t) -1;

    const char *src = img_entry( a0 )->name;
    char *dst = (char *) a1;

    for ( uint32_t i = 0; i < a2; i++ )
    {
        dst[i] = src[i];
        if ( src[i] == '\0' )
            return 0; /* the whole name, NUL included */
    }

    /* Capacity too small: truncate, still ending with NUL. */
    dst[a2 - 1] = '\0';
    return (uint32_t) -1;
}

/* ======================================================================
 * SYS_PROGRAM_RUN (SVC 13)
 * ====================================================================== */

/* Copy the name at address a0 into the kernel buffer dst. Every byte read
 * must be readable by the caller. 0 = ok; -1 = empty, or no NUL within
 * IMG_NAME_SIZE bytes, or outside the caller's memory. */
static int copy_name( uint32_t a0, char dst[IMG_NAME_SIZE] )
{
    const char *src = (const char *) a0;

    for ( uint32_t i = 0; i < IMG_NAME_SIZE; i++ )
    {
        if ( !caller_can_read( a0, i + 1 ) )
            return -1;
        dst[i] = src[i];
        if ( dst[i] == '\0' )
            return i == 0 ? -1 : 0;
    }
    return -1;
}

/* The first free slot, in the order 0, 1, 2, 3, or -1. */
static int first_free_slot( void )
{
    for ( unsigned s = 0; s < PROGRAM_SLOT_COUNT; s++ )
        if ( task_program_slot_free( s ) )
            return (int) s;
    return -1;
}

/* a0 = name. Returns the task id, or -1 (bad name or image), -2 (not
 * found), -3 (no free slot), -4 (bad program file). Nothing is written into
 * the slot before every check has passed (R9). */
static uint32_t sys_program_run_impl( uint32_t a0, uint32_t a1, uint32_t a2 )
{
    (void) a1;
    (void) a2;

    char name[IMG_NAME_SIZE];
    if ( copy_name( a0, name ) != 0 || !image_valid() )
        return (uint32_t) -1;

    const struct img_dirent *found = find_entry( name );
    if ( found == 0 )
        return (uint32_t) -2;

    int slot = first_free_slot();
    if ( slot < 0 )
        return (uint32_t) -3;

    uint32_t slot_base = (uint32_t) __program_slots_start + (uint32_t) slot * LOADER_SLOT_SIZE;
    const uint8_t *f = (const uint8_t *) &__fs_image_start + found->offset; /* the ELF, in FLASH */
    uint32_t size = found->size;

    /* Layers 2 and 3: every check, nothing written yet. elf_check_layout
     * already guarantees that the plan fits in the file and in 3072 bytes. */
    struct load_plan plan = { 0 };
    if ( elf_check_layout( f, size, &plan ) != 0
         || elf_check_relocs( f, size, &plan, slot_base ) != 0 )
        return (uint32_t) -4;

    /* Layer 4: copy code and data, zero the BSS, relocate. */
    uint8_t *dst = (uint8_t *) slot_base;
    for ( uint32_t i = 0; i < plan.copy_size; i++ )
        dst[i] = f[plan.file_off + i];
    for ( uint32_t i = plan.bss_start; i < plan.bss_end; i++ )
        dst[i] = 0;
    elf_apply_relocs( f, slot_base );

    /* Layer 5: the task, with the stack at the end of the slot. */
    int id = task_program_create( (unsigned) slot, name,
                                  ( void ( * )( void ) )( slot_base + plan.entry ),
                                  (uint32_t *) ( slot_base + LOADER_SLOT_SIZE ),
                                  LOADER_EXECUTABLE );
    if ( id < 0 )
        return (uint32_t) -4;

    /* W^X: the MPU makes [0, code_end) read + execute, the rest read + write. */
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
