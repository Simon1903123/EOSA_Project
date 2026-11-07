/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * The idle task: it runs when no other task is READY (every task is
 * delayed or finished). The CPU must always run something, and this
 * is the cheapest something.
 *
 * It runs unprivileged, on its own stack, inside its own partition
 * (the linker script places this file's variables,
 * none, in the idle partition, by the file name idle.o). So it follows
 * the same MPU and stack rules as every other task: the kernel needs no
 * special case to run it.
 */

#include "src/tasks.h"

void idle_task( void )
{
    for ( ;; )
    {
        /* Sleep until the next interrupt. `wfi` ("wait for interrupt")
         * is allowed in unprivileged code. The next SysTick wakes the
         * CPU: if a delayed task is READY again by then, the kernel
         * switches to it; otherwise the idle task loops and sleeps
         * again. */
        __asm volatile ( "wfi" );
    }
}
