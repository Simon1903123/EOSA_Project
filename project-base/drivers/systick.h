/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * SysTick driver: the core timer used as the system clock tick.
 */
#ifndef SYSTICK_H
#define SYSTICK_H

#include <stdint.h>

/* Milliseconds between two ticks (100 ticks per second). */
#define TICK_MS 10

/* Start SysTick so that it raises an exception every TICK_MS. */
void systick_init( void );

/* The SysTick exception handler. It is named in the vector table.
 * Besides the uptime, it calls the scheduler's tick hook,
 * task_increment_tick() (kernel/task.c). */
void SysTick_Handler( void );

/* Milliseconds since systick_init(), in steps of TICK_MS. */
uint32_t systick_uptime_ms( void );

#endif /* SYSTICK_H */
