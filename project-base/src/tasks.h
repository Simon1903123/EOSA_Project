/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * The base starts only the idle task. The project later adds a static
 * shell and loaded program tasks.
 */
#ifndef TASKS_H
#define TASKS_H

/* The idle task: sleeps (`wfi`) for ever. Never returns. */
void idle_task( void );

#endif /* TASKS_H */
