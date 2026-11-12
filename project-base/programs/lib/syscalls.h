/*
 * Embedded Operating Systems and Architecture course
 * Licensed under the MIT License. See LICENSE in the repository root.
 *
 * Contact: Stefano Di Carlo <stefano.dicarlo@polito.it>
 *
 * System calls: the only way unprivileged code can ask the kernel to do
 * something for it. This header is shared by both sides:
 *   - the kernel (kernel/syscall.c) uses the service numbers to pick
 *     the function that does the work;
 *   - the application uses the functions declared below, written in
 *     lib/syscalls.s, which run `svc #number`.
 */
#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>

/* Service numbers: the number written after `svc`.
 * 0 is not a service: `svc #0` (or any unknown number) returns -1.
 *
 * Careful: the number after `svc` is part of the instruction itself, so
 * the assembler needs it written as a plain number. An assembly file
 * (lib/syscalls.s) cannot use these C names, so it writes 1, 2, 3,
 * 4, 6, 7, 8, 9, 10, 11, 12, 13 and 14 directly (and src/main.c writes
 * 5). If you change
 * a number here, change it there too. The
 * compiler cannot check this for you: the two files are never
 * compiled together. */
#define SYS_WRITE  1    /* send bytes to the console */
#define SYS_READ   2    /* take one received byte, if any */
#define SYS_UPTIME 3    /* milliseconds since start */
#define SYS_EXIT   4    /* the calling task is finished */
/* Kernel only: main() uses it once to start the first task. If a task
 * tries it, the kernel answers -1. */
#define SYS_START_TASKS 5
/* Week 5: the scheduler services. */
#define SYS_YIELD       6   /* give the rest of the time slice to another task */
#define SYS_DELAY       7   /* block for a number of ticks */
#define SYS_DELAY_UNTIL 8   /* block until an absolute tick (periodic tasks) */
/* Print the recorded task switches and a Gantt chart (kernel/trace.c). */
#define SYS_TRACE_DUMP  9
#define SYS_TICK_COUNT  10  /* ticks since the scheduler started */
/* Project loader contract. The base exports stubs only: kernel support
 * is intentionally part of the project implementation. */
#define SYS_IMAGE_COUNT  11 /* number of directory entries in fs.img */
#define SYS_IMAGE_NAME   12 /* copy one directory entry's name */
#define SYS_PROGRAM_RUN  13 /* load/start a named program; return task id */
#define SYS_PROGRAM_WAIT 14 /* wait for a loaded task to finish */

/* Offer len bytes of buf for sending. Returns how many bytes the kernel
 * accepted: maybe fewer than len, maybe 0 when it has no room right
 * now. It never waits. The caller sends the rest later. */
int sys_write( const char *buf, unsigned len );

/* Return one received byte (0..255), or -1 if no byte is waiting.
 * It never waits. */
int sys_read( void );

/* Return the milliseconds since the kernel started its clock. */
uint32_t sys_uptime( void );

/* End the calling task. It never returns: the kernel marks the task
 * finished and switches to another task (or to the idle task). */
void sys_exit( void ) __attribute__( ( noreturn ) );

/* Give the CPU to the next READY task now, without waiting for the end
 * of the time slice. The caller stays READY: it runs again at its next
 * turn (at once, if no other task is READY). */
void task_yield( void );

/* Block the calling task for `ticks` ticks (one tick = 10 ms), counted
 * from now; other tasks run meanwhile. task_delay(0) is task_yield(). */
void task_delay( uint32_t ticks );

/* Block the calling task until tick *last_wake + period, then set
 * *last_wake to that tick. Called in a loop, it wakes the task exactly
 * every `period` ticks, however long the work in between took (as long
 * as it took less than one period). Initialise *last_wake once with
 * task_tick_count(). last_wake must be in the task's own partition.
 * Returns 0, or -1 if the kernel refused last_wake. */
int task_delay_until( uint32_t *last_wake, uint32_t period );

/* Return the ticks since the scheduler started (one tick = 10 ms). */
uint32_t task_tick_count( void );

/* Ask the kernel to print the scheduler trace: every task switch it
 * recorded, then a Gantt chart. */
void trace_dump( void );

/* Return the number of programs in the read-only image, or -1 if the
 * image is invalid. */
int sys_image_count( void );

/* Copy image entry index's name to name (capacity bytes). Return 0, or
 * -1 for an invalid image, index, or destination. */
int sys_image_name( unsigned index, char *name, unsigned capacity );

/* Load and start name. Return a non-negative task id, or -1 on error. */
int sys_program_run( const char *name );

/* Block until task_id ends. Return its exit status, or -1 on error. */
int sys_program_wait( int task_id );

/* Not called by tasks directly. The kernel uses its address as the
 * return address (lr) of every task, so a task whose function returns
 * lands here, and task_exit calls sys_exit for it. */
void task_exit( void ) __attribute__( ( noreturn ) );

#endif /* SYSCALLS_H */
