/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * User-side output and waiting: see printf.h.
 *
 * printf uses the same formatting code as the kernel's kprintf
 * (lib/format.c). Only the place the characters go is different:
 *   - kprintf sends each character straight to the UART (privileged);
 *   - printf collects them in a small buffer and hands the buffer to
 *     the kernel with the sys_write system call (unprivileged).
 */

#include <stdarg.h>
#include "lib/printf.h"
#include "lib/format.h"
#include "lib/syscalls.h"

/* Size of printf's buffer: characters are sent in groups of up to this
 * many, so one system call carries many characters instead of one. */
#define PRINTF_BUF_SIZE 64

void write_all( const char *buf, unsigned len )
{
    while ( len > 0 )
    {
        /* Offer everything still to send; the kernel takes what fits. */
        int n = sys_write( buf, len );
        buf += n;               /* skip the bytes it took */
        len -= n;
        if ( n == 0 )
        {
            /* The kernel's send buffer is full. Asking again at once
             * would only waste time: sleep until the next interrupt
             * (for example the UART's "byte sent" interrupt, which
             * makes room). `wfi` = "wait for interrupt". It is allowed
             * in unprivileged code. C cannot write it, so we use one
             * line of inline assembly. */
            __asm volatile ( "wfi" );
        }
    }
}

/* What printf's output function needs to remember between characters. */
struct printf_ctx
{
    char buf[PRINTF_BUF_SIZE];  /* characters not sent yet */
    unsigned used;              /* how many of buf are in use */
    int total;                  /* characters produced so far */
};

/* The output function for vformat: store one character, and send the
 * buffer when it is full. ctx points at printf's struct printf_ctx. */
static void printf_out( char c, void *ctx )
{
    struct printf_ctx *p = ctx;
    p->buf[p->used] = c;        /* add the character */
    p->used++;
    p->total++;
    if ( p->used == PRINTF_BUF_SIZE )
    {
        write_all( p->buf, p->used );   /* full: send it all */
        p->used = 0;                    /* and start again empty */
    }
}

int printf( const char *fmt, ... )
{
    struct printf_ctx p;        /* lives on the stack: about 72 bytes */
    p.used = 0;
    p.total = 0;

    va_list ap;                             /* the extra arguments */
    va_start( ap, fmt );                    /* start after fmt */
    vformat( printf_out, &p, fmt, ap );     /* format into p.buf */
    va_end( ap );                           /* done with the arguments */

    write_all( p.buf, p.used );             /* send what is left */
    return p.total;
}

void sleep_ms( uint32_t ms )
{
    uint32_t start = sys_uptime();
    /* now - start is the time passed, even when the millisecond count
     * wraps around from 0xFFFFFFFF to 0 (after about 49.7 days).
     * Unsigned subtraction works modulo 2^32: for example,
     * start = 0xFFFFFFF6 and now = 0x0000000A give now - start = 20,
     * which is right. Comparing `now < start + ms` instead would go
     * wrong when start + ms wraps. */
    while ( sys_uptime() - start < ms )
    {
        /* Nothing to do: sleep until the next interrupt (at the
         * latest the next clock tick, 10 ms away). */
        __asm volatile ( "wfi" );
    }
}

void spin_ticks( uint32_t ticks )
{
    uint32_t start = task_tick_count();     /* the tick when the work starts */
    /* Same wrap-safe test as in sleep_ms: now - start is the number of
     * ticks passed. The loop body is empty on purpose: no `wfi`, no
     * delay, just busy asking for the time. If the task is switched
     * out meanwhile, the ticks go on counting, so the work may end
     * soon after the task gets the CPU back. */
    while ( task_tick_count() - start < ticks )
    {
    }
}
