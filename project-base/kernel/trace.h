/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * The scheduler trace: the kernel writes down every task switch (when
 * it happened, from which task, to which task) and can print the list
 * later, with a small Gantt chart of who used the CPU.
 */
#ifndef TRACE_H
#define TRACE_H

#include <stdint.h>

/* Write down one task switch. from and to are indexes into the task
 * table (kernel/task.h). Called by task_switch_context(), only when
 * the task really changes. */
void trace_record_switch( uint8_t from, uint8_t to );

/* Print the trace with kprintf (the user side calls it trace_dump(),
 * see lib/syscalls.h). Format (also read by the course tool
 * `schedsim trace`):
 *     TRACE BEGIN
 *     T <tick> <from> <to>      one line per switch, oldest first
 *     TRACE END <tick now>
 * then the Gantt chart, for people. */
void trace_print( void );

#endif /* TRACE_H */
