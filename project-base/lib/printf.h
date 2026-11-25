/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * User-side output and waiting, built only on the system calls of
 * lib/syscalls.h. These functions run unprivileged: they never touch a
 * driver or a hardware register.
 */
#ifndef PRINTF_H
#define PRINTF_H

#include <stdint.h>

/* Send all len bytes of buf, however many system calls it takes.
 * While the kernel has no room, the CPU sleeps until the next interrupt. */
void write_all( const char *buf, unsigned len );

/* Print like the standard printf: supports %c %s %d %u %x %%.
 * Returns the number of characters printed. */
int printf( const char *fmt, ... );

/* Wait at least ms milliseconds (in steps of the 10 ms clock tick). */
void sleep_ms( uint32_t ms );

/* Keep the CPU busy for `ticks` ticks (one tick = 10 ms): a stand-in
 * for real work. Unlike sleep_ms, it never sleeps and never blocks, so
 * the task keeps the CPU until the scheduler takes it away. */
void spin_ticks( uint32_t ticks );

#endif /* PRINTF_H */
