/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * Kernel print function: see kprintf.h.
 */

#include "kernel/kprintf.h"
#include "lib/format.h"
#include "drivers/uart.h"

/* The output function for vformat: send one character to the UART.
 * ctx is not needed here. */
static void kprintf_out( char c, void *ctx )
{
    (void) ctx;
    uart_putc_polled( c );
}

void kprintf( const char *fmt, ... )
{
    va_list ap;                             /* the extra arguments */
    va_start( ap, fmt );                    /* start after fmt */
    vformat( kprintf_out, 0, fmt, ap );     /* format and send */
    va_end( ap );                           /* done with the arguments */
}
