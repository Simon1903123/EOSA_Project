/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * Kernel print function. It sends characters with the polled UART
 * function, so it works anywhere, even inside a fault handler.
 */
#ifndef KPRINTF_H
#define KPRINTF_H

/* Print like printf: supports %c %s %d %u %x %%. */
void kprintf( const char *fmt, ... );

#endif /* KPRINTF_H */
