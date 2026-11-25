/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * Text formatting: turn numbers and a format string into characters.
 * It does not know where the characters go: the caller gives a function
 * that receives them one at a time.
 */
#ifndef FORMAT_H
#define FORMAT_H

#include <stdarg.h>
#include <stdint.h>

/* Write value in decimal into buf (at least 11 bytes), add a final '\0'
 * and return the number of characters written. */
unsigned utoa_dec( uint32_t value, char *buf );

/* Same, in lowercase hexadecimal, without prefix (buf: at least 9 bytes). */
unsigned utoa_hex( uint32_t value, char *buf );

/* Format fmt with the arguments in ap and give every character to
 * out(c, ctx). Supported: %c %s %d %u %x %% (no width, no precision).
 * An unknown conversion is printed as written. */
void vformat( void ( *out )( char c, void *ctx ), void *ctx,
              const char *fmt, va_list ap );

#endif /* FORMAT_H */
