/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * Text formatting: see format.h.
 */

#include "lib/format.h"

/* Write value in the given base (10 or 16) into buf. */
static unsigned utoa_base( uint32_t value, unsigned base, char *buf )
{
    char tmp[10];       /* digits come out last-first: keep them here */
    unsigned n = 0;     /* how many digits so far */
    unsigned i;

    /* Take the last digit with "% base", then drop it with "/ base".
     * A do-while makes sure that 0 prints one digit, "0". */
    do
    {
        unsigned d = value % base;                      /* last digit, 0..base-1 */
        tmp[n++] = (char) ( d < 10 ? '0' + d : 'a' + ( d - 10 ) );
        value /= base;                                  /* remove that digit */
    } while ( value != 0 );

    /* Copy the digits in reverse order, so the first digit comes first. */
    for ( i = 0; i < n; i++ )
    {
        buf[i] = tmp[n - 1 - i];
    }
    buf[n] = '\0';      /* end of string */
    return n;
}

unsigned utoa_dec( uint32_t value, char *buf )
{
    return utoa_base( value, 10, buf );
}

unsigned utoa_hex( uint32_t value, char *buf )
{
    return utoa_base( value, 16, buf );
}

/* Send every character of the string s to out. */
static void put_string( void ( *out )( char c, void *ctx ), void *ctx, const char *s )
{
    while ( *s != '\0' )
    {
        out( *s, ctx );
        s++;
    }
}

void vformat( void ( *out )( char c, void *ctx ), void *ctx,
              const char *fmt, va_list ap )
{
    char num[12];       /* room for the longest number plus '\0' */

    while ( *fmt != '\0' )
    {
        /* An ordinary character: pass it through. */
        if ( *fmt != '%' )
        {
            out( *fmt, ctx );
            fmt++;
            continue;
        }

        /* A '%': look at the conversion letter after it. */
        fmt++;
        switch ( *fmt )
        {
        case 'c':
            /* A char is promoted to int when passed as "...". */
            out( (char) va_arg( ap, int ), ctx );
            break;

        case 's':
        {
            const char *s = va_arg( ap, const char * );
            /* Do not crash on a null pointer. */
            put_string( out, ctx, s != 0 ? s : "(null)" );
            break;
        }

        case 'd':
        {
            int v = va_arg( ap, int );
            uint32_t mag;
            if ( v < 0 )
            {
                out( '-', ctx );
                /* Negate in unsigned arithmetic: this also works for
                 * INT_MIN, whose positive value does not fit in an int. */
                mag = 0u - (uint32_t) v;
            }
            else
            {
                mag = (uint32_t) v;
            }
            utoa_dec( mag, num );
            put_string( out, ctx, num );
            break;
        }

        case 'u':
            utoa_dec( va_arg( ap, uint32_t ), num );
            put_string( out, ctx, num );
            break;

        case 'x':
            utoa_hex( va_arg( ap, uint32_t ), num );
            put_string( out, ctx, num );
            break;

        case '%':
            out( '%', ctx );
            break;

        case '\0':
            /* The string ended right after '%': print the '%' and stop. */
            out( '%', ctx );
            return;

        default:
            /* Unknown letter: print "%" and the letter as written. */
            out( '%', ctx );
            out( *fmt, ctx );
            break;
        }
        fmt++;      /* move past the conversion letter */
    }
}
