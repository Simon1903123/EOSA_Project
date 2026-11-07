/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * The scheduler trace: see kernel/trace.h.
 *
 * trace_record_switch() runs inside PendSV and trace_dump() inside
 * SVCall. Both are exceptions that never interrupt each other in the
 * middle of their work (PendSV only runs when SVCall is over), so the
 * buffer needs no protection.
 */

#include <stdint.h>
#include "kernel/trace.h"
#include "kernel/task.h"
#include "kernel/kprintf.h"

/* One task switch. */
struct trace_record {
    uint32_t tick;      /* the tick at which the switch happened */
    uint8_t from;       /* index in the task table of the task that stopped */
    uint8_t to;         /* index in the task table of the task that starts */
};

/* How many records the buffer holds. */
#define TRACE_SIZE 64

/* The buffer is a ring: when it is full, a new record overwrites the
 * oldest one. */
static struct trace_record buffer[TRACE_SIZE];
/* How many records were written in all. The next one goes to slot
 * count % TRACE_SIZE. */
static uint32_t count = 0;

void trace_record_switch( uint8_t from, uint8_t to )
{
    struct trace_record *r = &buffer[count % TRACE_SIZE];   /* the next slot */
    r->tick = tick_count;                   /* when */
    r->from = from;                         /* who stopped */
    r->to = to;                             /* who starts */
    count++;                                /* one more record */
}

/* How many records are still in the buffer. */
static uint32_t stored( void )
{
    return count < TRACE_SIZE ? count : TRACE_SIZE;
}

/* Record number i, counting from the oldest one (i = 0). */
static const struct trace_record *oldest_plus( uint32_t i )
{
    return &buffer[( count - stored() + i ) % TRACE_SIZE];
}

/* Which task owned the CPU at this tick? It is the `to` of the last
 * record that is not newer than the tick. */
static uint8_t owner_at( uint32_t tick )
{
    uint8_t owner = oldest_plus( 0 )->to;
    for ( uint32_t i = 0; i < stored(); i++ )
    {
        if ( oldest_plus( i )->tick <= tick )
        {
            owner = oldest_plus( i )->to;
        }
    }
    return owner;
}

/* The Gantt chart is cut in lines of at most this many ticks, so it
 * fits a terminal. */
#define GANTT_COLUMNS 64

/* Draw the Gantt chart from tick `first` up to (not including) `now`:
 * one row per task, one character per tick. */
static void draw_gantt( uint32_t first, uint32_t now )
{
    for ( uint32_t start = first; start < now; start += GANTT_COLUMNS )
    {
        /* This line shows ticks start .. end - 1. */
        uint32_t end = start + GANTT_COLUMNS;
        if ( end > now )
        {
            end = now;
        }

        /* The ruler: the last digit of every tick number. */
        kprintf( "\n tick " );
        for ( uint32_t t = start; t < end; t++ )
        {
            kprintf( "%u", t % 10 );
        }
        kprintf( "\n" );

        /* One row per task: '#' while it runs, '.' otherwise. */
        for ( unsigned i = 0; i < NUM_TASKS; i++ )
        {
            /* The name, then spaces up to 6 characters, so the rows
             * line up (kprintf has no field width). */
            unsigned len = 0;
            while ( tasks[i].name[len] != '\0' )
            {
                len++;
            }
            kprintf( " %s", tasks[i].name );
            for ( ; len < 5; len++ )
            {
                kprintf( " " );
            }
            for ( uint32_t t = start; t < end; t++ )
            {
                kprintf( "%c", owner_at( t ) == i ? '#' : '.' );
            }
            kprintf( "\n" );
        }
    }
}

void trace_print( void )
{
    /* Part 1: the raw records, for the course tool. */
    kprintf( "TRACE BEGIN\n" );
    for ( uint32_t i = 0; i < stored(); i++ )
    {
        const struct trace_record *r = oldest_plus( i );
        kprintf( "T %u %s %s\n", r->tick, tasks[r->from].name,
                 tasks[r->to].name );
    }
    kprintf( "TRACE END %u\n", tick_count );

    /* Part 2: the chart, from the first recorded switch to now. */
    if ( stored() > 0 )
    {
        draw_gantt( oldest_plus( 0 )->tick, tick_count );
    }
}
