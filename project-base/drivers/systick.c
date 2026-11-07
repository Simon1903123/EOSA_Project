/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * SysTick driver: see systick.h.
 */

#include "drivers/systick.h"
#include "cmsdk.h"
#include "kernel/task.h"

/* The speed of the processor clock, in cycles per second. */
#define CPU_CLOCK_HZ 25000000UL

/* Milliseconds since start. volatile: the handler changes it while
 * the main code reads it. */
static volatile uint32_t uptime_ms;

void systick_init( void )
{
    uptime_ms = 0;
    /* The counter counts from LOAD down to 0, which takes LOAD + 1
     * cycles. 1000 / TICK_MS ticks per second, so one tick lasts
     * CPU_CLOCK_HZ / (1000 / TICK_MS) cycles. Here: 250000 cycles. */
    SysTick->LOAD = CPU_CLOCK_HZ / ( 1000 / TICK_MS ) - 1;
    /* Writing any value to VAL sets the counter to 0. */
    SysTick->VAL = 0;
    /* Start counting, raise the exception at each wrap, use the core clock. */
    SysTick->CTRL = SysTick_CTRL_ENABLE | SysTick_CTRL_TICKINT | SysTick_CTRL_CLKSOURCE;
}

void SysTick_Handler( void )
{
    /* One more tick has passed. SysTick needs no acknowledge. */
    uptime_ms += TICK_MS;
    /* Since week 5 the tick also drives the scheduler: count it, wake
     * delayed tasks, end the time slice (kernel/task.c). */
    task_increment_tick();
}

uint32_t systick_uptime_ms( void )
{
    /* A single aligned 32-bit read cannot be torn by the interrupt. */
    return uptime_ms;
}
