/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * Mask and unmask interrupts around a short piece of code.
 * The two functions are written in assembly in kernel/irq.s.
 */
#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

/* Mask interrupts and return the previous mask state
 * (1 = they were already masked, 0 = they were allowed). */
uint32_t irq_save( void );

/* Put back the mask state returned by irq_save(). */
void irq_restore( uint32_t old );

#endif /* IRQ_H */
